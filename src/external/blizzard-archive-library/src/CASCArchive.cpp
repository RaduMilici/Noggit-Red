#include <CASCArchive.hpp>

#include <Exception.hpp>
#include <CascLib.h>

#include <noggit/Log.h>

#include <atomic>
#include <cstring>


using namespace BlizzardArchive::Archive;

// src/external/casclib-patch/FileTree.cpp (local CascLib patch, copied over the fetched upstream tree)
extern "C" void CascSetPreferredContentFlags(DWORD dwMask, DWORD dwValue);

void CASCArchive::setPreferredContentFlags(std::uint32_t mask, std::uint32_t value)
{
  CascSetPreferredContentFlags(static_cast<DWORD>(mask), static_cast<DWORD>(value));
}

namespace
{
  DWORD casc_locale_mask(BlizzardArchive::Locale locale)
  {
    switch (locale)
    {
      case BlizzardArchive::Locale::enUS: return CASC_LOCALE_ENUS;
      case BlizzardArchive::Locale::enGB: return CASC_LOCALE_ENGB;
      case BlizzardArchive::Locale::deDE: return CASC_LOCALE_DEDE;
      case BlizzardArchive::Locale::koKR: return CASC_LOCALE_KOKR;
      case BlizzardArchive::Locale::frFR: return CASC_LOCALE_FRFR;
      case BlizzardArchive::Locale::zhCN: return CASC_LOCALE_ZHCN;
      case BlizzardArchive::Locale::zhTW: return CASC_LOCALE_ZHTW;
      case BlizzardArchive::Locale::esES: return CASC_LOCALE_ESES;
      case BlizzardArchive::Locale::esMX: return CASC_LOCALE_ESMX;
      case BlizzardArchive::Locale::ruRU: return CASC_LOCALE_RURU;
      default: return 0; // CascLib falls back to the storage's default locale
    }
  }
}

CASCArchive::CASCArchive(std::string const& path
                         , std::string const& cache_path
                         , std::string const& product
                         , Locale locale
                         , OpenMode open_mode
                         , Listfile::Listfile* listfile)
  : BaseArchive(path, locale, listfile)
  , _product(product.empty() ? std::string("wow") : product)
{
  // 2026-09-15: the product code used to be hardcoded to "wow" (retail). A shared install carries several
  // products in ONE store (.build.info rows: wow_classic_era, wow_anniversary, ...) and the code name picks
  // the build config -> encoding -> root manifest; the data archives are common. Requires CascLib >= 3.0
  // (root manifest v2), see cmake/FindCascLib.cmake and docs/client_re/41.
  CASC_OPEN_STORAGE_ARGS args;
  std::memset(&args, 0, sizeof(args));
  args.Size = sizeof(CASC_OPEN_STORAGE_ARGS);
  args.szCodeName = _product.c_str();
  args.szRegion = "us";
  args.dwLocaleMask = casc_locale_mask(locale);

  bool online = false;
  switch (open_mode)
  {
    case OpenMode::REMOTE:
      args.szLocalPath = cache_path.c_str();
      args.szCdnHostUrl = path.c_str();
      online = true;
      break;
    case OpenMode::LOCAL:
      args.szLocalPath = path.c_str();
      break;
  }

  if (!CascOpenStorageEx(nullptr, &args, online, &_handle))
  {
    throw Exceptions::Archive::ArchiveOpenError("Error opening CASC archive: " + path
                                                + " (product '" + _product + "'). Error code: " + std::to_string(GetCascError()));
  }

  CASC_STORAGE_PRODUCT storage_product;
  std::memset(&storage_product, 0, sizeof(storage_product));
  DWORD features = 0;
  DWORD file_count = 0;
  size_t length_needed = 0;
  CascGetStorageInfo(_handle, CascStorageProduct, &storage_product, sizeof(storage_product), &length_needed);
  CascGetStorageInfo(_handle, CascStorageFeatures, &features, sizeof(features), &length_needed);
  CascGetStorageInfo(_handle, CascStorageTotalFileCount, &file_count, sizeof(file_count), &length_needed);
  LogDebug << "CASCArchive: opened '" << path << "' product='" << storage_product.szCodeName
           << "' build=" << storage_product.BuildNumber << " features=0x" << std::hex << features << std::dec
           << " files=" << file_count << std::endl;

  if (!(features & CASC_FEATURE_FILE_DATA_IDS))
  {
    // The root manifest was not understood (the pre-3.0 CascLib on a v2 root behaves exactly like this):
    // every open-by-fileDataID would fail. Fail loudly instead of rendering nothing.
    CascCloseStorage(_handle);
    _handle = nullptr;
    throw Exceptions::Archive::ArchiveOpenError("CASC archive '" + path + "' (product '" + _product
                                                + "'): the linked CascLib does not expose fileDataIDs for this root manifest"
                                                  " (features=0x" + std::to_string(features) + ").");
  }
}

bool CASCArchive::openFile(Listfile::FileKey const& file_key, Locale locale, HANDLE* file_handle) const
{
  std::uint32_t file_data_id;

  assert(file_key.hasFileDataID() || file_key.hasFilepath());

  if (file_key.hasFileDataID())
  {
    assert(file_key.fileDataID());

    file_data_id = file_key.fileDataID();
  }
  else
  {
    file_data_id = _listfile->getFileDataID(file_key.filepath());
  }

  // Name the first failures: a 0 here means the listfile does not know the path, a CascOpenFile
  // failure means the id is not in this product's root (or the storage rejected it).
  static std::atomic<int> logged_failures{0};
  if (!file_data_id)
  {
    // The community listfile names ~160k of the 213k files; every Classic root block carries the Jenkins
    // name hash of its entries (421 of 421 blocks, 2026-09-16), so a path the listfile lacks (1210 map
    // folders, e.g. world/maps/mauradon/mauradon.wdt) still opens by NAME -- CascLib hashes the
    // normalised path and looks it up in the root.
    if (file_key.hasFilepath() && CascOpenFile(_handle, file_key.filepath().c_str(), 0, CASC_OPEN_BY_NAME, file_handle))
    {
      return true;
    }
    if (logged_failures++ < 64)
      LogError << "CASCArchive::openFile: listfile has no fileDataID for '" << file_key.stringRepr() << "' and the root has no name hash for it" << std::endl;
    return false;
  }

  bool const ok = CascOpenFile(_handle, CASC_FILE_DATA_ID(file_data_id), 0, CASC_OPEN_BY_FILEID, file_handle);
  if (!ok && logged_failures++ < 64)
  {
    LogError << "CASCArchive::openFile: CascOpenFile(" << file_data_id << ") failed, error " << GetCascError()
             << " for '" << file_key.stringRepr() << "'" << std::endl;
  }
  return ok;
}

bool CASCArchive::readFile(HANDLE file_handle, char* buffer, std::size_t buf_size) const
{
  assert(file_handle);
  return CascReadFile(file_handle, buffer, buf_size, nullptr);
}

bool CASCArchive::closeFile(HANDLE file_handle) const
{
  assert(file_handle);
  return CascCloseFile(file_handle);
}

std::uint64_t CASCArchive::getFileSize(HANDLE file_handle) const
{
  assert(file_handle);
  unsigned long long size = 0;

  // A file whose data is not in the local storage (partial Battle.net install of the Forever Beta) or
  // that needs a missing TACT key opens but has no size: report 0 so readFile treats it as missing
  // (docs/client_re/42 sec 17.1) instead of resizing the buffer to an uninitialised value.
  if (!CascGetFileSize64(file_handle, static_cast<PULONGLONG>(&size)))
  {
    return 0;
  }

  return size;
}

bool CASCArchive::exists(Listfile::FileKey const& file_key, Locale locale) const
{
  HANDLE file_handle = nullptr;
  std::uint32_t file_data_id;

  assert(file_key.hasFileDataID() || file_key.hasFilepath());

  if (file_key.hasFileDataID())
  {
    assert(file_key.fileDataID());

    file_data_id = file_key.fileDataID();
  }
  else
  {
    file_data_id = _listfile->getFileDataID(file_key.filepath());
  }


  bool status = false;
  if (file_data_id)
  {
    status = CascOpenFile(_handle, CASC_FILE_DATA_ID(file_data_id), 0, CASC_OPEN_BY_FILEID, &file_handle);
  }
  else if (file_key.hasFilepath())
  {
    status = CascOpenFile(_handle, file_key.filepath().c_str(), 0, CASC_OPEN_BY_NAME, &file_handle); // root name hash, see openFile
  }
  if (status)
  {
    // A file the root lists but the local storage cannot decode -- a Forever Beta (1.60.1) entry behind a
    // TACT key CascLib does not have (ERROR_FILE_ENCRYPTED 1005) -- opens fine and fails on the first read.
    // Report it as missing so callers take their fallbacks instead of reading an empty buffer
    // (docs/client_re/42 sec 17.1).
    char probe = 0;
    DWORD read = 0;
    if (!CascReadFile(file_handle, &probe, 1, &read) || read != 1)
    {
      static std::atomic<int> logged_unreadable{0};
      if (logged_unreadable++ < 32)
      {
        DWORD const err = GetCascError();
        LogError << "CASCArchive::exists: '" << file_key.stringRepr() << "' opens but cannot be read (CascLib error "
                 << err << (err == 1005 ? ", encrypted: no TACT key" : "") << "); treating it as missing" << std::endl;
      }
      status = false;
    }
    CascCloseFile(file_handle);
  }
  return status;

}

CASCArchive::~CASCArchive()
{
  if (_handle)
    CascCloseStorage(_handle);
}