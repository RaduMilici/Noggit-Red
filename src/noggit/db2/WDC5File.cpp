// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/db2/WDC5File.hpp>

#include <algorithm>
#include <cstring>

namespace Noggit::DB2
{
  namespace
  {
    template<typename T>
    T read_at(std::vector<char> const& data, std::size_t offset)
    {
      T value;
      std::memcpy(&value, data.data() + offset, sizeof(T));
      return value;
    }

    struct SectionHeader
    {
      std::uint64_t tact_key = 0;
      std::uint32_t file_offset = 0;
      std::uint32_t num_records = 0;
      std::uint32_t string_table_size = 0;
      std::uint32_t offset_records_end = 0;
      std::uint32_t index_data_size = 0;
      std::uint32_t parent_lookup_size = 0;
      std::uint32_t offset_map_id_count = 0;
      std::uint32_t copy_table_count = 0;
    };

    constexpr std::uint32_t FLAG_SPARSE = 0x1;
    constexpr std::uint32_t FLAG_SECONDARY_KEY = 0x2;
  }

  bool WDC5File::load(std::vector<char> data, std::string* error)
  {
    auto fail = [&](std::string const& what)
    {
      if (error) *error = what;
      _valid = false;
      return false;
    };

    _data = std::move(data);
    if (_data.size() < 4)
    {
      return fail("file too short");
    }

    std::uint32_t const magic = read_at<std::uint32_t>(_data, 0);
    std::size_t pos = 4;
    if (magic == 0x35434457u) // "WDC5"
    {
      // u32 schemaVersion + char[128] schema string ("WOWSTATIC_1_15_9_68185")
      pos += 4 + 128;
    }
    else if (magic != 0x33434457u && magic != 0x34434457u) // "WDC3", "WDC4"
    {
      return fail("not a WDC3/WDC4/WDC5 file");
    }
    bool const has_encrypted_id_lists = magic != 0x33434457u;

    if (_data.size() < pos + 68)
    {
      return fail("header truncated");
    }

    _records_count_header = read_at<std::uint32_t>(_data, pos + 0);
    std::uint32_t const fields_count = read_at<std::uint32_t>(_data, pos + 4);
    _record_size = read_at<std::uint32_t>(_data, pos + 8);
    // string table size (+12) is the sum over sections
    _table_hash = read_at<std::uint32_t>(_data, pos + 16);
    _layout_hash = read_at<std::uint32_t>(_data, pos + 20);
    _min_id = read_at<std::int32_t>(_data, pos + 24);
    // max id (+28), locale (+32)
    _flags = read_at<std::uint16_t>(_data, pos + 36);
    _id_field_index = read_at<std::uint16_t>(_data, pos + 38);
    // total fields count (+40), packed data offset (+44), lookup column count (+48), column meta size (+52),
    // common data size (+56), pallet data size (+60)
    std::uint32_t const sections_count = read_at<std::uint32_t>(_data, pos + 64);
    pos += 68;

    if (_flags & FLAG_SPARSE)
    {
      return fail("sparse (offset-map) tables are not supported");
    }

    std::vector<SectionHeader> sections(sections_count);
    for (auto& section : sections)
    {
      if (_data.size() < pos + 40) return fail("section headers truncated");
      section.tact_key = read_at<std::uint64_t>(_data, pos + 0);
      section.file_offset = read_at<std::uint32_t>(_data, pos + 8);
      section.num_records = read_at<std::uint32_t>(_data, pos + 12);
      section.string_table_size = read_at<std::uint32_t>(_data, pos + 16);
      section.offset_records_end = read_at<std::uint32_t>(_data, pos + 20);
      section.index_data_size = read_at<std::uint32_t>(_data, pos + 24);
      section.parent_lookup_size = read_at<std::uint32_t>(_data, pos + 28);
      section.offset_map_id_count = read_at<std::uint32_t>(_data, pos + 32);
      section.copy_table_count = read_at<std::uint32_t>(_data, pos + 36);
      pos += 40;
    }

    _fields.assign(fields_count, Field{});
    for (auto& field : _fields)
    {
      if (_data.size() < pos + 4) return fail("field meta truncated");
      field.meta_bits = read_at<std::int16_t>(_data, pos);
      field.meta_offset = read_at<std::uint16_t>(_data, pos + 2);
      pos += 4;
    }
    for (auto& field : _fields)
    {
      if (_data.size() < pos + 24) return fail("column meta truncated");
      field.record_offset = read_at<std::uint16_t>(_data, pos + 0);
      field.size = read_at<std::uint16_t>(_data, pos + 2);
      field.additional_data_size = read_at<std::uint32_t>(_data, pos + 4);
      field.compression = read_at<std::uint32_t>(_data, pos + 8);
      field.a1 = read_at<std::uint32_t>(_data, pos + 12);
      field.a2 = read_at<std::uint32_t>(_data, pos + 16);
      field.a3 = read_at<std::uint32_t>(_data, pos + 20);
      pos += 24;
    }

    _pallet.assign(fields_count, {});
    _common.assign(fields_count, {});
    for (std::size_t i = 0; i < fields_count; ++i)
    {
      auto const& field = _fields[i];
      if (field.compression == Pallet || field.compression == PalletArray)
      {
        if (_data.size() < pos + field.additional_data_size) return fail("pallet data truncated");
        std::size_t const count = field.additional_data_size / 4;
        _pallet[i].resize(count);
        for (std::size_t k = 0; k < count; ++k)
        {
          _pallet[i][k] = read_at<std::uint32_t>(_data, pos + k * 4);
        }
        pos += field.additional_data_size;
      }
    }
    for (std::size_t i = 0; i < fields_count; ++i)
    {
      auto const& field = _fields[i];
      if (field.compression == Common)
      {
        if (_data.size() < pos + field.additional_data_size) return fail("common data truncated");
        std::size_t const count = field.additional_data_size / 8;
        _common[i].reserve(count);
        for (std::size_t k = 0; k < count; ++k)
        {
          _common[i][read_at<std::uint32_t>(_data, pos + k * 8)] = read_at<std::uint32_t>(_data, pos + k * 8 + 4);
        }
        pos += field.additional_data_size;
      }
    }

    if (has_encrypted_id_lists)
    {
      for (auto const& section : sections)
      {
        if (section.tact_key == 0) continue;
        if (_data.size() < pos + 4) return fail("encrypted id list truncated");
        std::uint32_t const count = read_at<std::uint32_t>(_data, pos);
        pos += 4 + static_cast<std::size_t>(count) * 4;
      }
    }

    _strings.clear();
    _records.clear();
    std::size_t global_index = 0;
    for (auto const& section : sections)
    {
      pos = section.file_offset;
      std::size_t const records_start = pos;
      std::size_t const records_bytes = static_cast<std::size_t>(section.num_records) * _record_size;
      if (_data.size() < records_start + records_bytes + section.string_table_size)
      {
        return fail("section data truncated");
      }
      pos += records_bytes;

      // an encrypted section whose key is unavailable is zero-filled; DBCD skips it the same way
      if (section.tact_key != 0)
      {
        bool all_zero = true;
        for (std::size_t k = 0; k < std::min<std::size_t>(records_bytes, 64); ++k)
        {
          if (_data[records_start + k] != 0) { all_zero = false; break; }
        }
        if (all_zero && records_bytes)
        {
          global_index += section.num_records;
          continue;
        }
      }

      std::size_t const strings_base = _strings.size();
      _strings.insert(_strings.end(), _data.begin() + pos, _data.begin() + pos + section.string_table_size);
      pos += section.string_table_size;

      std::vector<std::uint32_t> ids;
      if (section.index_data_size)
      {
        std::size_t const count = section.index_data_size / 4;
        if (_data.size() < pos + section.index_data_size) return fail("id list truncated");
        ids.resize(count);
        bool all_zero = true;
        for (std::size_t k = 0; k < count; ++k)
        {
          ids[k] = read_at<std::uint32_t>(_data, pos + k * 4);
          if (ids[k]) all_zero = false;
        }
        if (all_zero && count)
        {
          for (std::size_t k = 0; k < count; ++k) ids[k] = static_cast<std::uint32_t>(_min_id + global_index + k);
        }
        pos += section.index_data_size;
      }

      std::vector<std::pair<std::uint32_t, std::uint32_t>> copies;
      if (section.copy_table_count)
      {
        if (_data.size() < pos + static_cast<std::size_t>(section.copy_table_count) * 8) return fail("copy table truncated");
        for (std::uint32_t k = 0; k < section.copy_table_count; ++k)
        {
          copies.emplace_back(read_at<std::uint32_t>(_data, pos + k * 8), read_at<std::uint32_t>(_data, pos + k * 8 + 4));
        }
        pos += static_cast<std::size_t>(section.copy_table_count) * 8;
      }

      // (sparse entries + secondary-key sparse ids would sit here for FLAG_SPARSE tables -- rejected above)

      std::unordered_map<std::uint32_t, std::uint32_t> parents; // record index -> parent id
      if (section.parent_lookup_size)
      {
        if (_data.size() < pos + 12) return fail("reference data truncated");
        std::uint32_t const count = read_at<std::uint32_t>(_data, pos);
        pos += 12; // count, min id, max id
        if (_data.size() < pos + static_cast<std::size_t>(count) * 8) return fail("reference entries truncated");
        for (std::uint32_t k = 0; k < count; ++k)
        {
          std::uint32_t const parent = read_at<std::uint32_t>(_data, pos + k * 8);
          std::uint32_t const index = read_at<std::uint32_t>(_data, pos + k * 8 + 4);
          parents[index] = parent;
        }
        pos += static_cast<std::size_t>(count) * 8;
      }

      std::size_t const first_record = _records.size();
      for (std::uint32_t r = 0; r < section.num_records; ++r)
      {
        Record record;
        record.data_offset = records_start + static_cast<std::size_t>(r) * _record_size;
        record.global_index = global_index + r;
        record.id = 0;
        auto const parent_it = parents.find(r);
        record.parent = parent_it == parents.end() ? 0u : parent_it->second;
        _records.push_back(record);
      }

      // ids: from the id list, else inline in the record (IdFieldIndex)
      for (std::uint32_t r = 0; r < section.num_records; ++r)
      {
        auto& record = _records[first_record + r];
        if (!ids.empty() && r < ids.size())
        {
          record.id = ids[r];
        }
        else if (_id_field_index < _fields.size())
        {
          record.id = static_cast<std::uint32_t>(rawValue(record, _fields[_id_field_index], 0, nullptr));
        }
      }

      for (auto const& [destination, source] : copies)
      {
        if (destination == source) continue;
        for (std::size_t r = first_record; r < first_record + section.num_records; ++r)
        {
          if (_records[r].id == source)
          {
            Record clone = _records[r];
            clone.id = destination;
            _records.push_back(clone);
            break;
          }
        }
      }

      global_index += section.num_records;
      (void)strings_base;
    }

    _id_index.clear();
    _id_index.reserve(_records.size());
    for (std::size_t r = 0; r < _records.size(); ++r)
    {
      _id_index.emplace(_records[r].id, r); // first occurrence wins
    }

    _valid = true;
    return true;
  }

  std::uint64_t WDC5File::readBits(std::size_t data_offset, std::size_t bit_offset, std::size_t bit_count) const
  {
    if (bit_count == 0) return 0;
    if (bit_count > 64) bit_count = 64;
    std::size_t const byte = data_offset + bit_offset / 8;
    std::size_t const shift = bit_offset % 8;
    // gather up to 9 bytes little-endian (bit_count + shift <= 72)
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    for (std::size_t k = 0; k < 9; ++k)
    {
      std::size_t const index = byte + k;
      if (index >= _data.size()) break;
      std::uint64_t const b = static_cast<unsigned char>(_data[index]);
      if (k < 8) low |= b << (k * 8);
      else high = b;
    }
    std::uint64_t value = low >> shift;
    if (shift)
    {
      value |= high << (64 - shift);
    }
    if (bit_count < 64)
    {
      value &= (std::uint64_t(1) << bit_count) - 1;
    }
    return value;
  }

  std::size_t WDC5File::cardinality(std::size_t field) const
  {
    if (field >= _fields.size()) return 1;
    auto const& f = _fields[field];
    switch (f.compression)
    {
      case None:
      {
        int bit_size = 32 - f.meta_bits;
        if (bit_size <= 0) bit_size = static_cast<int>(f.a2);
        if (bit_size <= 0) return 1;
        return std::max<std::size_t>(1, f.size / static_cast<std::size_t>(bit_size));
      }
      case PalletArray:
        return std::max<std::size_t>(1, f.a3);
      default:
        return 1;
    }
  }

  std::uint64_t WDC5File::rawValue(Record const& record, Field const& field, std::size_t index, bool* is_signed) const
  {
    if (is_signed) *is_signed = false;
    switch (field.compression)
    {
      case None:
      {
        int bit_size = 32 - field.meta_bits;
        if (bit_size <= 0) bit_size = static_cast<int>(field.a2);
        if (bit_size <= 0) bit_size = 32;
        return readBits(record.data_offset, field.record_offset + index * static_cast<std::size_t>(bit_size), static_cast<std::size_t>(bit_size));
      }
      case Immediate:
      {
        std::size_t const width = field.a2 ? field.a2 : field.size;
        return readBits(record.data_offset, field.record_offset, width);
      }
      case SignedImmediate:
      {
        std::size_t const width = field.a2 ? field.a2 : field.size;
        std::uint64_t value = readBits(record.data_offset, field.record_offset, width);
        if (width > 0 && width < 64 && (value & (std::uint64_t(1) << (width - 1))))
        {
          value |= ~((std::uint64_t(1) << width) - 1); // sign-extend
        }
        if (is_signed) *is_signed = true;
        return value;
      }
      case Common:
      {
        std::size_t const field_index = static_cast<std::size_t>(&field - _fields.data());
        auto const& table = _common[field_index];
        auto const it = table.find(record.id);
        return it == table.end() ? field.a1 : it->second;
      }
      case Pallet:
      {
        std::size_t const field_index = static_cast<std::size_t>(&field - _fields.data());
        std::size_t const pallet_index = static_cast<std::size_t>(readBits(record.data_offset, field.record_offset, field.a2 ? field.a2 : field.size));
        auto const& pallet = _pallet[field_index];
        return pallet_index < pallet.size() ? pallet[pallet_index] : 0u;
      }
      case PalletArray:
      {
        std::size_t const field_index = static_cast<std::size_t>(&field - _fields.data());
        std::size_t const card = std::max<std::size_t>(1, field.a3);
        std::size_t const pallet_index = static_cast<std::size_t>(readBits(record.data_offset, field.record_offset, field.a2 ? field.a2 : field.size));
        auto const& pallet = _pallet[field_index];
        std::size_t const slot = pallet_index * card + index;
        return slot < pallet.size() ? pallet[slot] : 0u;
      }
      default:
        return 0;
    }
  }

  std::uint32_t WDC5File::getUInt(std::size_t record, std::size_t field, std::size_t index) const
  {
    if (record >= _records.size() || field >= _fields.size()) return 0;
    return static_cast<std::uint32_t>(rawValue(_records[record], _fields[field], index, nullptr));
  }

  std::int32_t WDC5File::getInt(std::size_t record, std::size_t field, std::size_t index) const
  {
    if (record >= _records.size() || field >= _fields.size()) return 0;
    bool is_signed = false;
    std::uint64_t const value = rawValue(_records[record], _fields[field], index, &is_signed);
    if (is_signed)
    {
      return static_cast<std::int32_t>(static_cast<std::int64_t>(value));
    }
    // sign-extend narrow None/Immediate fields (i8/i16 columns) by their bit width
    auto const& f = _fields[field];
    std::size_t width = 32;
    if (f.compression == None)
    {
      int bit_size = 32 - f.meta_bits;
      if (bit_size <= 0) bit_size = static_cast<int>(f.a2);
      if (bit_size > 0) width = static_cast<std::size_t>(bit_size);
    }
    else if (f.compression == Immediate)
    {
      width = f.a2 ? f.a2 : f.size;
    }
    if (width < 32 && width > 0 && (value & (std::uint64_t(1) << (width - 1))))
    {
      return static_cast<std::int32_t>(static_cast<std::int64_t>(value | ~((std::uint64_t(1) << width) - 1)));
    }
    return static_cast<std::int32_t>(value);
  }

  float WDC5File::getFloat(std::size_t record, std::size_t field, std::size_t index) const
  {
    std::uint32_t const bits = getUInt(record, field, index);
    float value;
    std::memcpy(&value, &bits, sizeof(float));
    return value;
  }

  std::string WDC5File::getString(std::size_t record, std::size_t field, std::size_t index) const
  {
    if (record >= _records.size() || field >= _fields.size()) return {};
    auto const& rec = _records[record];
    auto const& f = _fields[field];
    // A string field holds an offset relative to ITS OWN position inside the records block; the string
    // tables follow the records block (DBCD: index = recordOffset + fieldBytePos + value, recordOffset =
    // recordIndex * recordSize - recordsCount * recordSize, keyed into the concatenated string tables).
    std::int64_t const value = static_cast<std::int64_t>(static_cast<std::int32_t>(getUInt(record, field, index)));
    std::int64_t const field_byte = (f.record_offset / 8) + static_cast<std::int64_t>(index) * 4;
    std::int64_t const position = static_cast<std::int64_t>(rec.global_index) * _record_size + field_byte + value
                                - static_cast<std::int64_t>(_records_count_header) * _record_size;
    if (position < 0 || static_cast<std::size_t>(position) >= _strings.size())
    {
      return {};
    }
    std::size_t end = static_cast<std::size_t>(position);
    while (end < _strings.size() && _strings[end] != '\0') ++end;
    return std::string(_strings.data() + position, end - static_cast<std::size_t>(position));
  }

  std::size_t WDC5File::findById(std::uint32_t id) const
  {
    auto const it = _id_index.find(id);
    return it == _id_index.end() ? _records.size() : it->second;
  }
}
