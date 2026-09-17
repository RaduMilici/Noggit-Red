// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Noggit::DB2
{
  // Reader for the WDC3 / WDC4 / WDC5 client database format (every CASC client since 8.x; 1.15.9 Classic
  // Era and 2.5.6 Anniversary ship WDC5). Whole-table, typed, in-memory. Field ORDER only -- names come
  // from the WoWDBDefs definitions (ModernDBC.cpp). Layout measured on the shipped files and checked
  // against wowdev/DBCD's WDC5Reader; see docs/client_re/41 section 3.
  //
  //   header   = magic + [WDC5: u32 schemaVersion + char[128] schemaString] + 18 x u32/u16 body fields
  //   sections = 40 B each (u64 tactKey, fileOffset, numRecords, stringTableSize, offsetRecordsEnd,
  //              indexDataSize, parentLookupSize, offsetMapIdCount, copyTableCount)
  //   fields   = {i16 bits, u16 offset} x fieldCount, then column meta 24 B x fieldCount
  //   pallet / common blobs, [WDC4/5: encrypted id lists], then per section: records, string table,
  //   id list, copy table, (sparse entries), reference data, (sparse ids)
  class WDC5File
  {
  public:
    enum Compression : std::uint32_t
    {
      None = 0,
      Immediate = 1,
      Common = 2,
      Pallet = 3,
      PalletArray = 4,
      SignedImmediate = 5,
    };

    struct Field
    {
      std::int16_t meta_bits = 0;      // FieldMeta.bits (32 - bit size for None-compressed fields)
      std::uint16_t meta_offset = 0;   // FieldMeta.offset (bytes)
      std::uint16_t record_offset = 0; // ColumnMeta.recordOffset (bits)
      std::uint16_t size = 0;          // ColumnMeta.size (bits)
      std::uint32_t additional_data_size = 0;
      std::uint32_t compression = None;
      std::uint32_t a1 = 0;            // bitOffset / defaultValue
      std::uint32_t a2 = 0;            // bitWidth
      std::uint32_t a3 = 0;            // cardinality (PalletArray) / flags
    };

    // Takes ownership of the file bytes. Returns false (and fills *error) on a format it cannot read.
    bool load(std::vector<char> data, std::string* error = nullptr);

    [[nodiscard]] bool valid() const { return _valid; }
    [[nodiscard]] std::uint32_t layoutHash() const { return _layout_hash; }
    [[nodiscard]] std::uint32_t tableHash() const { return _table_hash; }
    [[nodiscard]] std::size_t fieldCount() const { return _fields.size(); }
    // Records including copy-table clones (each clone is its own record with the destination id).
    [[nodiscard]] std::size_t recordCount() const { return _records.size(); }
    [[nodiscard]] std::uint32_t idFieldIndex() const { return _id_field_index; }

    [[nodiscard]] std::uint32_t id(std::size_t record) const { return _records[record].id; }
    // Parent id from the section's reference (relation) data; 0 when the table has none.
    [[nodiscard]] std::uint32_t parentId(std::size_t record) const { return _records[record].parent; }

    // Array length of a field (1 for scalars).
    [[nodiscard]] std::size_t cardinality(std::size_t field) const;

    [[nodiscard]] std::uint32_t getUInt(std::size_t record, std::size_t field, std::size_t index = 0) const;
    [[nodiscard]] std::int32_t getInt(std::size_t record, std::size_t field, std::size_t index = 0) const;
    [[nodiscard]] float getFloat(std::size_t record, std::size_t field, std::size_t index = 0) const;
    [[nodiscard]] std::string getString(std::size_t record, std::size_t field, std::size_t index = 0) const;

    // First record with the given id (recordCount() when absent).
    [[nodiscard]] std::size_t findById(std::uint32_t id) const;

  private:
    struct Record
    {
      std::size_t data_offset = 0;   // absolute offset of the record bytes in _data
      std::size_t global_index = 0;  // index across sections (string offsets are relative to it)
      std::uint32_t id = 0;
      std::uint32_t parent = 0;
    };

    std::uint64_t readBits(std::size_t data_offset, std::size_t bit_offset, std::size_t bit_count) const;
    std::uint64_t rawValue(Record const& record, Field const& field, std::size_t index, bool* is_signed) const;

    std::vector<char> _data;
    std::vector<Field> _fields;
    std::vector<Record> _records;
    std::vector<std::vector<std::uint32_t>> _pallet;                       // per field
    std::vector<std::unordered_map<std::uint32_t, std::uint32_t>> _common; // per field, by record id
    std::vector<char> _strings;                                            // string tables of all sections, concatenated
    std::unordered_map<std::uint32_t, std::size_t> _id_index;

    bool _valid = false;
    std::uint32_t _record_size = 0;
    std::uint32_t _records_count_header = 0;
    std::uint32_t _flags = 0;
    std::uint32_t _id_field_index = 0;
    std::uint32_t _layout_hash = 0;
    std::uint32_t _table_hash = 0;
    std::int32_t _min_id = 0;
  };
}
