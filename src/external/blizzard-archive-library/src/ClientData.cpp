#include <ClientData.hpp>
#include <Exception.hpp>
#include <MPQArchive.hpp>
#include <DirectoryArchive.hpp>
#include <CASCArchive.hpp>

#include <noggit/Log.h>

#include <filesystem>
#include <atomic>
#include <cassert>
#include <regex>

using namespace BlizzardArchive;
namespace fs = std::filesystem;

namespace
{
  bool has_classic_mpq_layout(fs::path const& client_path)
  {
    fs::path const data_path = client_path / "Data";
    return fs::exists(data_path / "base.MPQ")
        && fs::exists(data_path / "dbc.MPQ")
        && fs::exists(data_path / "terrain.MPQ");
  }

  // Vanilla/Turtle clients split their base data by asset type instead of using
  // WotLK's common/expansion/lichking archive set. Archives loaded later have
  // higher priority because ClientData searches _archives in reverse order.
  constexpr std::array<std::string_view, 13> ClassicArchiveNames {
      "backup.MPQ",
      "base.MPQ",
      "dbc.MPQ",
      "fonts.MPQ",
      "interface.MPQ",
      "misc.MPQ",
      "model.MPQ",
      "sound.MPQ",
      "speech.MPQ",
      "terrain.MPQ",
      "texture.MPQ",
      "wmo.MPQ",
      "patch.MPQ"
  };
}

ClientData::ClientData(std::string const& path, ClientVersion version, Locale locale, std::string const& local_path
                       , std::string const& casc_product, std::string const& listfile_path)
  : _version(version)
  , _open_mode(OpenMode::LOCAL)
  , _storage_type((version > ClientVersion::WOTLK) ? StorageType::CASC : StorageType::MPQ)
  , _locale_mode(locale)
  , _path(path)
  , _local_path(ClientData::normalizeFilenameUnix(local_path))
  , _casc_product(casc_product.empty() ? std::string("wow") : casc_product)
  , _listfile_path(listfile_path)
{

  validateLocale();

  switch (_storage_type)
  {
  case StorageType::MPQ:
    initializeMPQStorage();
    break;
  case StorageType::CASC:
    initializeCASCStorage();
    break;
  }
}

ClientData::ClientData(std::string const& path, std::string const& cdn_cache_path, ClientVersion version, Locale locale, std::string const& local_path
                       , std::string const& casc_product, std::string const& listfile_path)
    : _version(version)
    , _open_mode(OpenMode::REMOTE)
    , _storage_type((version > ClientVersion::WOTLK) ? StorageType::CASC : StorageType::MPQ)
    , _locale_mode(locale)
    , _path(path)
    , _local_path(ClientData::normalizeFilenameUnix(local_path))
    , _cdn_cache_path(cdn_cache_path)
    , _casc_product(casc_product.empty() ? std::string("wow") : casc_product)
    , _listfile_path(listfile_path)
{

  validateLocale();

  switch (_storage_type)
  {
    case StorageType::CASC:
      initializeCASCStorage();
      break;
    case StorageType::MPQ:
      throw Exceptions::Archive::ArchiveOpenError("MPQ storage does not support online loading.");
      break;
  }
}

ClientData::~ClientData()
{
  // clean up
  for (auto archive : _archives)
  {
    delete archive;
  }
}

void ClientData::loadMPQArchive(std::string const& mpq_path)
{
  if (!fs::exists(mpq_path) || fs::equivalent(mpq_path, _local_path))
    return;

  if (fs::is_directory(mpq_path))
  {
    _archives.push_back(new Archive::DirectoryArchive(mpq_path, _locale_mode, &_listfile));
  }
  else
  {
    _archives.push_back(new Archive::MPQArchive(mpq_path, _locale_mode, &_listfile));
  }


}

void ClientData::initializeMPQStorage()
{
  if (_version == ClientVersion::CLASSIC && has_classic_mpq_layout(_path))
  {
    fs::path const data_path = fs::path(_path) / "Data";

    for (std::string_view const filename : ClassicArchiveNames)
    {
      loadMPQArchive((data_path / filename).string());
    }

    // Numbered and lettered patches override both the modular base archives
    // and patch.MPQ. Missing entries are intentionally ignored.
    for (char number = '2'; number <= '9'; ++number)
    {
      loadMPQArchive((data_path / (std::string("patch-") + number + ".MPQ")).string());
    }
    for (char letter = 'a'; letter <= 'z'; ++letter)
    {
      loadMPQArchive((data_path / (std::string("patch-") + letter + ".MPQ")).string());
    }

    return;
  }

  for (auto const& filename : ClientData::ArchiveNameTemplates)
  {
    std::string mpq_path = (fs::path(_path) / "Data" / filename).string();

    std::string::size_type location(std::string::npos);
    std::string_view const& locale = ClientData::Locales[static_cast<int>(_locale_mode) - 1];

    do
    {
      location = mpq_path.find("{locale}");
      if (location != std::string::npos)
      {
        mpq_path.replace(location, 8, locale);
      }
    } while (location != std::string::npos);

    if (mpq_path.find("{number}") != std::string::npos)
    {
      location = mpq_path.find("{number}");
      mpq_path.replace(location, 8, " ");
      for (char j = '2'; j <= '9'; j++)
      {
        mpq_path.replace(location, 1, std::string(&j, 1));
        loadMPQArchive(mpq_path);
      }
    }
    else if (mpq_path.find("{character}") != std::string::npos)
    {
      location = mpq_path.find("{character}");
      mpq_path.replace(location, 11, " ");
      for (char c = 'a'; c <= 'z'; c++)
      {
        mpq_path.replace(location, 1, std::string(&c, 1));
        loadMPQArchive(mpq_path);
      }
    }
    else
    {
      loadMPQArchive(mpq_path);
    }
  }

 
}

void ClientData::initializeCASCStorage()
{
  std::string listfile = (fs::path(_local_path) / "listfile.csv").string();
  if (!fs::exists(listfile) && !_listfile_path.empty())
  {
    listfile = _listfile_path;
  }
  LogDebug << "ClientData: CASC product '" << _casc_product << "', listfile '" << listfile << "'" << std::endl;
  _listfile.initFromCSV(listfile);
  LogDebug << "ClientData: listfile holds " << _listfile.fileDataIDToPathMap().size() << " entries" << std::endl;

  switch (_open_mode)
  {
    case OpenMode::LOCAL:
    {
      _archives.push_back(new Archive::CASCArchive(_path, "", _casc_product, _locale_mode, _open_mode, &_listfile));
      break;
    }
    case OpenMode::REMOTE:
    {
      assert(_cdn_cache_path.has_value());
      _archives.push_back(new Archive::CASCArchive(_path, _cdn_cache_path.value(), _casc_product, _locale_mode, _open_mode, &_listfile));
      break;
    }
  }

}

void ClientData::validateLocale()
{
  switch (_storage_type)
  {
    case StorageType::MPQ:
    {
      // Vanilla and Turtle clients keep realmlist.wtf at the client root and
      // have no Data/<locale> directory. MPQ file lookup itself is not locale
      // dependent, so use the English locale for this layout when AUTO was
      // requested by the project.
      if (_version == ClientVersion::CLASSIC && has_classic_mpq_layout(_path))
      {
        fs::path const realmlist_path = fs::path(_path) / "realmlist.wtf";
        if (!fs::exists(realmlist_path))
        {
          throw Exceptions::Locale::LocaleNotFoundError(
              "The Vanilla client directory does not contain realmlist.wtf at its root.");
        }

        if (_locale_mode == Locale::AUTO)
        {
          _locale_mode = Locale::enUS;
        }
        break;
      }

      if (static_cast<int>(_locale_mode)) // manual locale
      [[unlikely]]
      {
        fs::path realmlist_path = fs::path(_path) / "Data"
            / ClientData::Locales[static_cast<int>(_locale_mode) - 1] / "realmlist.wtf";

        if (!fs::exists(realmlist_path))
        {
          throw Exceptions::Locale::LocaleNotFoundError("Requested locale \""
            + std::string(ClientData::Locales[static_cast<int>(_locale_mode) - 1].data()) +
            "\" does not exist in the client directory."
            "Be sure, that there is one containing the file \"realmlist.wtf\".");
        }

      }
      else // auto locale
      [[likely]]
      {
        for (unsigned i = 0; i < ClientData::Locales.size(); ++i)
        {
          fs::path realmlist_path = fs::path(_path) / "Data" / ClientData::Locales[i] / "realmlist.wtf";

          if (fs::exists(realmlist_path))
          {
            _locale_mode = static_cast<Locale>(i + 1);
            return;
          }
        }

        throw Exceptions::Locale::LocaleNotFoundError("Automatic locale detection failed. "
                                                      "The client directory does not contain any locale directory. Be "
                                                      "sure, that there is one containing the file \"realmlist.wtf\".");
      }
      break;
    }
    case StorageType::CASC:
    {
      if (_locale_mode == Locale::AUTO)
      {
        throw Exceptions::Locale::IncorrectLocaleModeError("Automatic locale detection is not"
                                                           " supported for CASC-based clients.");
      }
      break;
    }
  }
  
}

bool ClientData::readFile(Listfile::FileKey const& file_key, std::vector<char>& buffer)
{
  const std::lock_guard _lock(_mutex);

  HANDLE handle = nullptr;

  for (auto it = _archives.rbegin(); it != _archives.rend(); ++it)
  {
    if (!(*it)->openFile(file_key, _locale_mode, &handle))
      continue;

    std::uint64_t buf_size = (*it)->getFileSize(handle);
    buffer.resize(buf_size);

    bool const read_ok = buf_size > 0 && (*it)->readFile(handle, buffer.data(), buf_size);
    if (!read_ok)
    {
      static std::atomic<int> logged_unreadable{0};
      if (logged_unreadable++ < 32)
      {
        LogError << "ClientData::readFile: '" << file_key.stringRepr() << "' opened in " << (*it)->path()
                 << " but " << (buf_size ? "the read failed" : "it is empty") << "; treating it as missing" << std::endl;
      }
      (*it)->closeFile(handle);
      buffer.clear();
      continue;
    }

    if (!(*it)->closeFile(handle))
    {
      assert(false);
    }

    return true;
  }

  return false;
}

bool ClientData::existsOnDisk(Listfile::FileKey const& file_key)
{
  if (!file_key.hasFilepath())
    return false;

  return fs::exists(getDiskPath(file_key));
}

bool ClientData::exists(Listfile::FileKey const& file_key)
{
  if (ClientData::existsOnDisk(file_key))
  {
    return true;
  }

  const std::lock_guard _lock(_mutex);

  for (auto it = _archives.rbegin(); it != _archives.rend(); ++it)
  {
    if ((*it)->exists(file_key, _locale_mode))
      return true;
  }

  return false;
}

std::string ClientData::getDiskPath(Listfile::FileKey const& file_key)
{
  const std::lock_guard _lock(_mutex);

  if (file_key.hasFilepath())
  {
    return (fs::path(_local_path) / ClientData::normalizeFilenameUnix(file_key.filepath())).string();
  }
  else
  {
    // try deducing filepath from listfile
    assert(file_key.hasFileDataID());
    std::string filepath = _listfile.getPath(file_key.fileDataID());

    if (!filepath.empty())
    {
      return (fs::path(_local_path) / ClientData::normalizeFilenameUnix(filepath)).string();
    }
    else
    {
      return (fs::path(_local_path) / "unknown_files/" / std::to_string(file_key.fileDataID())).string();
    }
  }
   
}

std::string ClientData::normalizeFilenameUnix(std::string filename)
{
  std::transform(filename.begin(), filename.end(), filename.begin()
    , [](char c)
    {
      return c == '\\' ? '/' : c;
    }
  );
  return filename;
}

std::string ClientData::normalizeFilenameInternal(std::string filename)
{
  std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
  std::transform(filename.begin(), filename.end(), filename.begin()
      , [](char c)
                 {
                   return c == '\\' ? '/' : c;
                 }
  );

  if (filename.ends_with(".mdx"))
  {
    filename = std::regex_replace(filename, std::regex(".mdx"), ".m2");
  }
  else if(filename.ends_with(".mdl"))
  {
    filename = std::regex_replace(filename, std::regex(".mdl"), ".m2");
  }

  return filename;
}

std::string ClientData::normalizeFilenameWoW(std::string filename)
{
  std::transform(filename.begin(), filename.end(), filename.begin(), ::toupper);
  std::transform(filename.begin(), filename.end(), filename.begin()
    , [](char c)
    {
      return c == '/' ? '\\' : c;
    }
  );
  return filename;
}
