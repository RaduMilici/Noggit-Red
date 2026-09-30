#include <Listfile.hpp>
#include <Exception.hpp>
#include <ClientData.hpp>
#include <fstream>
#include <sstream>
#include <cstdint>

using namespace BlizzardArchive::Listfile;

FileKey::FileKey()
: _file_data_id(0)
, _file_path("")
{
}

FileKey::FileKey(std::string const& filepath, std::uint32_t file_data_id)
: _file_data_id(file_data_id)
, _file_path(ClientData::normalizeFilenameInternal(filepath))
{}

FileKey::FileKey(std::string const& filepath, Listfile* listfile)
  : _file_path(ClientData::normalizeFilenameInternal(filepath))
{
  if (listfile)
  {
    deduceOtherComponent(listfile);
  }

}

FileKey::FileKey(const char* filepath, Listfile* listfile)
: _file_path(ClientData::normalizeFilenameInternal(filepath))
{
  if (listfile)
  {
    deduceOtherComponent(listfile);
  }
}

FileKey::FileKey(const char* filepath, std::uint32_t file_data_id)
  : _file_path(ClientData::normalizeFilenameInternal(filepath))
  , _file_data_id(file_data_id)
{}


FileKey::FileKey(std::uint32_t file_data_id, Listfile* listfile)
  : _file_data_id(file_data_id)
{
  if (listfile)
  {
    deduceOtherComponent(listfile);
  }
}

void Listfile::initFromCSV(std::string const& listfile_path)
{
  std::ifstream fstream;
  fstream.open(listfile_path);

  if (!fstream.is_open())
  {
    throw Exceptions::Listfile::ListfileNotFoundError();
  }
  else
  {
    std::string line = "";
    while (std::getline(fstream, line))
    {
      std::string uid_str;
      std::string filename;
      std::uint32_t uid;

      std::stringstream ss(line);

      getline(ss, uid_str, ';');
      getline(ss, filename);

      uid = std::atoi(uid_str.c_str());

      // normalizeFilenameInternal rewrites a legacy ".mdx"/".mdl" suffix to ".m2", so the community
      // listfile's MDX rows (modern stores still carry them under their own, much higher ids) land on
      // the SAME key as the real M2. Whichever came last used to win -- the MDX id -- and every model
      // opened by path failed (2026-09-15, Classic Era: elwynntreecanopy03.m2 -> 706753 = the .mdx).
      // A real .m2 row always wins; an .mdx/.mdl row only fills a key nothing else claimed.
      std::string const key = ClientData::normalizeFilenameInternal(filename);
      bool const legacy_model_row = filename.size() > 4
        && (ClientData::normalizeFilenameUnix(filename).ends_with(".mdx") || ClientData::normalizeFilenameUnix(filename).ends_with(".mdl")
            || ClientData::normalizeFilenameUnix(filename).ends_with(".MDX") || ClientData::normalizeFilenameUnix(filename).ends_with(".MDL"));
      if (legacy_model_row)
      {
        _path_to_fdid.emplace(key, uid);
      }
      else
      {
        _path_to_fdid[key] = uid;
      }
      _fdid_to_path[uid] = ClientData::normalizeFilenameWoW(filename);

    }

  }
}

void Listfile::initFromFileList(std::vector<char> const& file_list_blob)
{
  // TODO: feels very sketchy, copied it from original Noggit.
  // check if approach from initFromCSV() works any better (less reallocs maybe).

  std::string current;
  for (char c : file_list_blob)
  {
    if (c == '\r')
    {
      continue;
    }
    if (c == '\n')
    {
      _path_to_fdid[ClientData::normalizeFilenameInternal(current)] = 0;
      current.resize(0);
    }
    else
    {
      current += c;
    }
  }

  if (!current.empty())
  {
    _path_to_fdid[ClientData::normalizeFilenameInternal(current)] = 0;
  }
}

void Listfile::registerPath(std::uint32_t file_data_id, std::string const& path) const
{
  if (!file_data_id || path.empty())
  {
    return;
  }
  _path_to_fdid[ClientData::normalizeFilenameInternal(path)] = file_data_id;
  _fdid_to_path.emplace(file_data_id, ClientData::normalizeFilenameWoW(path));
}

std::uint32_t Listfile::getFileDataID(std::string const& filename) const
{
  auto it = _path_to_fdid.find(filename);

  if (it != _path_to_fdid.end())
  {
    return it->second;
  }
  else
  {
    return 0; // Not found
  }

}

std::string Listfile::getPath(std::uint32_t file_data_id) const
{
  auto it = _fdid_to_path.find(file_data_id);

  if (it != _fdid_to_path.end())
  {
    return it->second;
  }
  else
  {
    return ""; // Not found
  }

}

bool FileKey::deduceOtherComponent(const Listfile* listfile)
{
  if (hasFileDataID() && !hasFilepath())
  {
    std::string path = listfile->getPath(fileDataID());

    if (path.empty())
    {
      return false;
    }

    _file_path = path;
    return true;

  }
  else if (hasFilepath() && !hasFileDataID())
  {
    std::uint32_t fdid = listfile->getFileDataID(filepath());

    if (!fdid)
    {
      return false;
    }

    _file_data_id = fdid;
    return true;
  }

  return false;
}

bool FileKey::operator==(const FileKey& rhs) const
{
  if (hasFileDataID() && rhs.hasFileDataID())
  {
    return _file_data_id == rhs.fileDataID();
  }
  else if (hasFilepath() && rhs.hasFilepath())
  {
    return filepath() == rhs.filepath();
  }

  return false;
}

FileKey::FileKey(FileKey&& other) noexcept
{
  std::swap(_file_data_id, other._file_data_id);
  std::swap(_file_path, other._file_path);
}

std::string FileKey::stringRepr() const
{
  return hasFilepath() ? _file_path.value() : std::to_string(_file_data_id);
}

bool FileKey::operator<(const FileKey& rhs) const
{
  if (hasFileDataID() && rhs.hasFileDataID())
  {
    return _file_data_id < rhs.fileDataID();
  }
  else if (hasFilepath() && rhs.hasFilepath())
  {
    return filepath() < rhs.filepath();
  }

  return false;
}

FileKey& FileKey::operator=(FileKey&& other) noexcept
{
  std::swap(_file_data_id, other._file_data_id);
  std::swap(_file_path, other._file_path);
  return *this;
}

FileKey::FileKey(FileKey const& other)
: _file_data_id(other._file_data_id)
, _file_path(other._file_path)
{
}

FileKey& FileKey::operator= (FileKey const& other)
{
  _file_data_id = other._file_data_id;
  _file_path = other._file_path;

  return *this;
}

