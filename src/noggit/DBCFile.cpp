// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBCFile.h>
#include <noggit/Log.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <ClientFile.hpp>

#include <string>
#include <QSettings>
#include <QDir>
#include <fstream>
#include <cstdint>
#include <algorithm>
#include <cstring>


template<typename T> inline
auto write(std::ostream& stream, T const& val) -> void
{
  stream.write(reinterpret_cast<char const*>(&val), sizeof(T));
}

DBCFile::DBCFile(const std::string& _filename)
  : filename(_filename)
  , recordSize(0)
  , recordCount(0)
  , fieldCount(0)
  , stringSize(0)
{}

void DBCFile::open(std::shared_ptr<BlizzardArchive::ClientData> clientData)
{
  invalidate_id_index();
  BlizzardArchive::ClientFile f (filename, clientData.get());

  if (f.isEof())
  {
    LogError << "The DBC file \"" << filename << "\" could not be opened. This application may crash soon as the file is most likely needed." << std::endl;
    return;
  }
  LogDebug << "Opening DBC \"" << filename << "\"" << std::endl;

  char header[4];

  f.read(header, 4); // Number of records
  assert(header[0] == 'W' && header[1] == 'D' && header[2] == 'B' && header[3] == 'C');
  f.read(&recordCount, 4);
  f.read(&fieldCount, 4);
  f.read(&recordSize, 4);
  f.read(&stringSize, 4);

  // DIAGNOSTIC (grass saga, doc 37 §2b): identify WHICH GroundEffect* copy the chain served --
  // the archives carry 8 different GroundEffectTexture.dbc versions and the record count alone
  // distinguishes every one (patch-9=11816, patch-2=12742, dbc.MPQ=8466/11-field, ...). Always-on
  // LogError: two lines per session, answers "where does noggit's clutter come from" from log.txt.
  if (filename.find("GroundEffect") != std::string::npos)
  {
    LogError << "GRASS-DIAG DBC '" << filename << "' records=" << recordCount
             << " fields=" << fieldCount << " recordSize=" << recordSize << std::endl;
  }

  if (fieldCount * 4 != recordSize)
  {
    throw std::logic_error ("non four-byte-columns not supported");
  }

  data.resize (recordSize * recordCount);
  f.read (data.data(), data.size());

  stringTable.resize (stringSize);
  f.read (stringTable.data(), stringTable.size());

  f.close();
}

void DBCFile::save()
{
  QString str = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(str.endsWith('\\') || str.endsWith('/')))
  {
    str += "/";
  }

  std::string filename_proj = BlizzardArchive::ClientData::normalizeFilenameUnix(str.toStdString() + filename);
  QDir dir(str + "/DBFilesClient/");
  if (!dir.exists())
    dir.mkpath(".");

  std::ofstream stream(filename_proj, std::ios_base::out | std::ios_base::trunc | std::ios_base::binary);

  stream << 'W' << 'D' << 'B' << 'C';


  write(stream, recordCount);
  write(stream, fieldCount);
  write(stream, recordSize);
  write(stream, stringSize);

  stream.write(reinterpret_cast<char*>(data.data()), data.size());
  stream.write(stringTable.data(), stringSize);
  stream.close();
}

DBCFile::Record DBCFile::addRecord(size_t id, size_t id_field)
{
  invalidate_id_index();
  recordCount++;

  for (Iterator i = begin(); i != end(); ++i)
  {
    if (i->getUInt(id_field) == id)
      throw AlreadyExists();
  }

  size_t old_size = data.size();
  data.resize(old_size + recordSize);
  *reinterpret_cast<unsigned int*>(data.data() + old_size + id_field * sizeof(std::uint32_t)) = static_cast<unsigned int>(id);

  return Record(*this, data.data() + old_size);
}

DBCFile::Record DBCFile::addRecordCopy(size_t id, size_t id_from, size_t id_field)
{
  invalidate_id_index();
  recordCount++;

  bool from_found = false;
  size_t from_idx = 0;

  for (Iterator i = begin(); i != end(); ++i)
  {
    if (i->getUInt(id_field) == id)
      throw AlreadyExists();

    if (i->getUInt(id_field) == id_from)
    {
      from_found = true;
    }

    if (!from_found)
    {
      from_idx++;
    }
  }

  if (!from_found)
  {
    throw NotFound();
  }

  size_t old_size = data.size();
  data.resize(old_size + recordSize);

  Record record_from = getRecord(from_idx);
  std::copy(data.data() + from_idx * recordSize, data.data() + from_idx * recordSize + recordSize, data.data() + old_size);
  *reinterpret_cast<unsigned int*>(data.data() + old_size + id_field * sizeof(std::uint32_t)) = static_cast<unsigned int>(id);

  return Record(*this, data.data() + old_size);
}

void DBCFile::removeRecord(size_t id, size_t id_field)
{
  invalidate_id_index();
  recordCount--;
  size_t counter = 0;

  for (Iterator i = begin(); i != end(); ++i)
  {
    if (i->getUInt(id_field) == id)
    {
      size_t initial_size = data.size();

      unsigned char* record = data.data() + counter * recordSize;
      std::memmove(record, record + recordSize, recordSize * (recordCount - counter + 1));
      data.resize(initial_size - recordSize);
      return;
    }

    counter++;

  }

  throw NotFound();

}

int DBCFile::getEmptyRecordID(size_t id_field)
{

  unsigned int id = 0;

  for (Iterator i = begin(); i != end(); ++i)
  {
    id = std::max(i->getUInt(id_field), id);
  }

  return static_cast<int>(++id);
}



