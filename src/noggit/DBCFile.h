// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <cassert>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <blizzard-archive-library/include/ClientData.hpp>

class DBCFile
{
public:
  explicit DBCFile(const std::string& filename);

  // Open database. It must be openened before it can be used.
  void open(std::shared_ptr<BlizzardArchive::ClientData> clientData);
  void save();

  class NotFound : public std::runtime_error
  {
  public:
    NotFound() : std::runtime_error("Key was not found.")
    { }
  };

  class AlreadyExists : public std::runtime_error
  {
  public:
    AlreadyExists() : std::runtime_error("Key already exists.")
    { }
  };

  class Iterator;
  class Record
  {
  public:
     const float& getFloat(size_t field) const
    {
      assert(field < file.fieldCount);
      return *reinterpret_cast<float*>(offset + field * 4);
    }
    const unsigned int& getUInt(size_t field) const
    {
      assert(field < file.fieldCount);
      return *reinterpret_cast<unsigned int*>(offset + field * 4);
    }
    const int& getInt(size_t field) const
    {
      assert(field < file.fieldCount);
      return *reinterpret_cast<int*>(offset + field * 4);
    }
    const char *getString(size_t field) const
    {
      if (field >= file.fieldCount || file.stringTable.empty())
      {
        return "";
      }

      size_t stringOffset = *reinterpret_cast<unsigned int*>(offset + field * 4);
      if (stringOffset >= file.stringTable.size())
      {
        return "";
      }

      return file.stringTable.data() + stringOffset;
    }
    const char *getLocalizedString(size_t field, int locale = -1) const
    {
      int loc = locale;
      if (locale == -1)
      {
        if (field >= file.fieldCount)
        {
          return "";
        }

        for (loc = 0; loc < 15; loc++)
        {
          if (field + loc >= file.fieldCount)
          {
            return "";
          }

          size_t stringOffset = getUInt(field + loc);
          if (stringOffset != 0)
            break;
        }
      }

      if (loc < 0 || field + loc >= file.fieldCount || file.stringTable.empty())
      {
        return "";
      }

      size_t stringOffset = *reinterpret_cast<unsigned int*>(offset + (field + loc) * 4);
      if (stringOffset >= file.stringTable.size())
      {
        return "";
      }

      return file.stringTable.data() + stringOffset;
    }

    template<typename T> inline
    void write(size_t field, T val)
    {
      static_assert(sizeof(T) == 4, "This function only writes int/uint/float values.");
      assert(field < file.fieldCount);
      *reinterpret_cast<T*>(offset + field * 4) = val;
    }

    void writeString(size_t field, const std::string& val)
    {
      assert(field < file.fieldCount);

      if (!val.size())
      {
        *reinterpret_cast<unsigned int*>(offset + field * 4) = 0;
        return;
      }

      size_t old_size = file.stringTable.size();
      *reinterpret_cast<unsigned int*>(offset + field * 4) = static_cast<unsigned int>(file.stringTable.size());
      file.stringTable.resize(old_size + val.size() + 1);
      std::copy(val.c_str(), val.c_str() + val.size() + 1, file.stringTable.data() + old_size);
      file.stringSize += static_cast<std::uint32_t>(val.size() + 1);
    }

    void writeLocalizedString(size_t field, const std::string& val, int locale)
    {
      assert(field < file.fieldCount);

      if (!val.size())
      {
        *reinterpret_cast<unsigned int*>(offset + ((field + locale) * 4)) = 0;
        return;
      }

      size_t old_size = file.stringTable.size();
      *reinterpret_cast<unsigned int*>(offset + ((field + locale) * 4)) = static_cast<unsigned int>(file.stringTable.size());
      file.stringTable.resize(old_size + val.size() + 1);
      std::copy(val.c_str(), val.c_str() + val.size() + 1, file.stringTable.data() + old_size);
      file.stringSize += static_cast<std::uint32_t>(val.size() + 1);
    }

  private:
    Record(DBCFile &pfile, unsigned char *poffset) : file(pfile), offset(poffset) {}
    DBCFile &file;
    unsigned char *offset;

    friend class DBCFile;
    friend class DBCFile::Iterator;
  };
  /** Iterator that iterates over records
  */
  class Iterator
  {
  public:
    Iterator(DBCFile &file, unsigned char *offset) :
      record(file, offset) {}
    /// Advance (prefix only)
    Iterator & operator++() {
      record.offset += record.file.recordSize;
      return *this;
    }
    /// Return address of current instance
    Record & operator*() { return record; }
    Record* operator->() {
      return &record;
    }
    /// Comparison
    bool operator==( Iterator const &b)  const
    {
      return record.offset == b.record.offset;
    }
  private:
    Record record;
  };

  inline Record getRecord(size_t id)
  {
    return Record(*this, data.data() + id*recordSize);
  }

  inline Iterator begin()
  {
    return Iterator(*this, data.data());
  }
  inline Iterator end()
  {
    return Iterator(*this, data.data() + data.size());
  }

  inline size_t getRecordCount()  { return recordCount; }
  inline size_t getFieldCount()  { return fieldCount; }
  // getByID/CheckIfIdExists/getRecordRowId were O(recordCount) LINEAR SCANS. That is catastrophic for a
  // hot caller against a large DBC: MapChunk::detailDoodads() does gGroundEffectTextureDB.getByID() per
  // subcell (64/chunk) + per scattered doodad, and an Ascension client ships a 38k-record
  // GroundEffectTexture.dbc (~14x a stock WotLK one) -> ~1e9 comparisons per tile -> the map view HANGS
  // on every map. Build an id->row hash index once (lazy) and look up in O(1). The index is only valid for
  // the primary key (field 0); a non-0 field still scans (rare). Invalidated on record add/remove (see
  // DBCFile.cpp) so the DBC editor stays correct. First-occurrence-wins matches the old scan semantics.
  void build_id_index()
  {
    _id_index.clear();
    _id_index.reserve(recordCount);
    for (std::uint32_t r = 0; r < recordCount; ++r)
    {
      unsigned int const rid = *reinterpret_cast<unsigned int const*>(data.data() + static_cast<size_t>(r) * recordSize);
      _id_index.emplace(rid, r);
    }
    _id_index_built = true;
  }
  void invalidate_id_index() { _id_index_built = false; _key_index_built = false; }

  // Secondary lazy index for a NON-primary key column, same O(1) contract as _id_index above.
  // 1.12's GroundEffectDoodad.dbc is keyed on field 1, not field 0: the client builds its lookup
  // table as table[rec.field1] = filename (WoW.exe FUN_006b1a90, stride 0xc, key at +4, string at
  // +8). That lookup runs once per scattered doodad in the grass path, so the linear scan below
  // would cost ~1e6 comparisons per chunk. Only one column is cached at a time -- callers alternate
  // rarely, and a field change simply rebuilds.
  void build_key_index(size_t field)
  {
    _key_index.clear();
    _key_index.reserve(recordCount);
    for (std::uint32_t r = 0; r < recordCount; ++r)
    {
      unsigned int const k = *reinterpret_cast<unsigned int const*>(
          data.data() + static_cast<size_t>(r) * recordSize + field * 4u);
      _key_index.emplace(k, r);
    }
    _key_index_field = field;
    _key_index_built = true;
  }

  inline Record getByID(unsigned int id, size_t field = 0)
  {
    if (field == 0)
    {
      if (!_id_index_built) { build_id_index(); }
      auto const it = _id_index.find(id);
      if (it != _id_index.end())
        return Record(*this, data.data() + static_cast<size_t>(it->second) * recordSize);
      throw NotFound();
    }
    if (!_key_index_built || _key_index_field != field) { build_key_index(field); }
    auto const kit = _key_index.find(id);
    if (kit != _key_index.end())
      return Record(*this, data.data() + static_cast<size_t>(kit->second) * recordSize);
    throw NotFound();
  }
  inline bool CheckIfIdExists(unsigned int id, size_t field = 0)
  {
      if (field == 0)
      {
        if (!_id_index_built) { build_id_index(); }
        return _id_index.find(id) != _id_index.end();
      }
      if (!_key_index_built || _key_index_field != field) { build_key_index(field); }
      return _key_index.find(id) != _key_index.end();
  }
  inline int getRecordRowId(unsigned int id, size_t field = 0)
  {
      if (field == 0)
      {
        if (!_id_index_built) { build_id_index(); }
        auto const it = _id_index.find(id);
        if (it != _id_index.end())
          return static_cast<int>(it->second);
        throw NotFound();
      }
      int row_id = 0;
      for (Iterator i = begin(); i != end(); ++i)
      {
          if (i->getUInt(field) == id)
              return row_id;

          row_id++;
      }
      throw NotFound();
  }

  Record addRecord(size_t id, size_t id_field = 0);
  Record addRecordCopy(size_t id, size_t id_from, size_t id_field = 0);
  void removeRecord(size_t id, size_t id_field = 0);
  int getEmptyRecordID(size_t id_field = 0);

private:
  std::string filename;
  std::uint32_t recordSize;
  std::uint32_t recordCount;
  std::uint32_t fieldCount;
  std::uint32_t stringSize;
  std::vector<unsigned char> data;
  std::vector<char> stringTable;

  // Lazy id (field 0) -> row index for O(1) getByID. Rebuilt on demand; invalidated on record add/remove.
  std::unordered_map<unsigned int, std::uint32_t> _id_index;
  bool _id_index_built = false;
  // secondary index, keyed on _key_index_field (see build_key_index)
  std::unordered_map<unsigned int, std::uint32_t> _key_index;
  size_t _key_index_field = 0;
  bool _key_index_built = false;
};
