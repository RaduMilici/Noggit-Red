// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Alphamap.hpp>
#include <opengl/context.hpp>
#include <opengl/context.inl>
#include <ClientFile.hpp>

#include <algorithm>
#include <optional>

Alphamap::Alphamap()
{
  createNew();
}

Alphamap::Alphamap(BlizzardArchive::ClientFile *f, unsigned int flags, bool use_big_alphamaps, bool do_not_fix_alpha_map)
{
  createNew();

  // The layer's own flag decides first. MCLY 0x200 (alpha_map_compressed) is an RLE-coded 64x64 map
  // whatever the WDT says; in WotLK data it only ever appeared together with MPHD 0x4 (big alpha), so
  // this used to be tested inside the big-alpha branch. Modern maps break that pairing: Scarlet Enclave
  // (Classic Era 1.15.9, map 2856) sets 0x200 on 1400+ of its layers with a WDT of 0x3ca (no 0x4), and
  // reading those RLE streams as 4-bit 2048-byte maps was the "broken texture blending"
  // (docs/client_re/42 sec 12). Uncompressed layers keep the WDT rule: 0x4 = 4096 bytes, else 2048 4-bit.
  if (flags & 0x200)
  {
    readCompressed(f);
  }
  else if (use_big_alphamaps)
  {
    readBigAlpha(f);
  }
  else
  {
    readNotCompressed(f, do_not_fix_alpha_map);
  }
}

namespace
{
  struct compressed_mcal_entry
  {
    enum mode_t
    {
      copy = 0,              // append value[0..count - 1]
      fill = 1,              // append value[0] count times
    };
    uint8_t count : 7;
    uint8_t mode : 1;

    uint8_t value[];
  };
}

void Alphamap::readCompressed(BlizzardArchive::ClientFile *f)
{
  // compressed
  char const* input = f->getPointer();
  // Never read past the file buffer, whatever the compressed stream claims.
  char const* const input_end = input + (f->getSize() > f->getPos() ? f->getSize() - f->getPos() : 0);

  for (std::size_t offset_output(0); offset_output < 4096;)
  {
    if (input >= input_end)
    {
      LogError << "MCAL: compressed alpha stream truncated, padding with zero." << std::endl;
      break;
    }

    compressed_mcal_entry const* e = reinterpret_cast<compressed_mcal_entry const*>(input);

    int count = e->count;

    if (offset_output + count > 4096)
    {
      LogError << "Invalid MCAL, uncompressed size is greater than 4096" << std::endl;
      count = static_cast<int>(4096 - offset_output);
    }

    ++input;

    if (count == 0)
    {
      continue;
    }

    if (e->mode == compressed_mcal_entry::fill)
    {
      if (input >= input_end)
      {
        LogError << "MCAL: compressed alpha stream truncated, padding with zero." << std::endl;
        break;
      }
      memset(&amap[offset_output], e->value[0], count);
      ++input;
    }
    else
    {
      if (input_end - input < count)
      {
        LogError << "MCAL: compressed alpha stream truncated, padding with zero." << std::endl;
        count = static_cast<int>(input_end - input);
        memcpy(&amap[offset_output], e->value, count);
        break;
      }
      memcpy(&amap[offset_output], e->value, count);
      input += count;
    }

    offset_output += count;
  }
}

void Alphamap::readBigAlpha(BlizzardArchive::ClientFile *f)
{
  // Never read past the file buffer: a malformed map (e.g. a wrong MPHD big-alpha flag on a
  // vanilla map whose alphas are 2048-byte 4-bit, or a truncated MCAL) would otherwise
  // access-violate on the last chunk of a tile and fail the whole tile load.
  std::size_t const remaining = f->getSize() > f->getPos() ? f->getSize() - f->getPos() : 0;
  if (remaining < 64 * 64)
  {
    LogError << "MCAL: big alpha layer truncated (" << remaining << "/4096 bytes left in file), padding with zero." << std::endl;
  }
  memcpy(amap, f->getPointer(), std::min<std::size_t>(64 * 64, remaining));
  f->seekRelative(0x1000);
}

void Alphamap::readNotCompressed(BlizzardArchive::ClientFile *f, bool do_not_fix_alpha_map)
{
  std::size_t const remaining = f->getSize() > f->getPos() ? f->getSize() - f->getPos() : 0;
  std::size_t const avail = std::min<std::size_t>(0x800, remaining);
  if (avail < 0x800)
  {
    LogError << "MCAL: 4-bit alpha layer truncated (" << remaining << "/2048 bytes left in file), padding with zero." << std::endl;
  }
  char const* abuf = f->getPointer();

  for (std::size_t k(0); k < avail; ++k)
  {
    std::size_t const x = k / 32;
    std::size_t const y = (k % 32) * 2;
    amap[x * 64 + y + 0] = ((*abuf & 0x0f) << 4) | (*abuf & 0x0f);
    amap[x * 64 + y + 1] = ((*abuf & 0xf0) >> 4) | (*abuf & 0xf0);
    ++abuf;
  }

  if (!do_not_fix_alpha_map)
  {
    for (std::size_t i(0); i < 64; ++i)
    {
      amap[i * 64 + 63] = amap[i * 64 + 62];
      amap[63 * 64 + i] = amap[62 * 64 + i];
    }
    amap[63 * 64 + 63] = amap[62 * 64 + 62];
  }
  f->seekRelative(0x800);
}

void Alphamap::createNew()
{
  memset(amap, 0, 64 * 64);
}

void Alphamap::setAlpha(size_t offset, unsigned char value)
{
  amap[offset] = value;
}

void Alphamap::setAlpha(unsigned char *pAmap)
{
  memcpy(amap, pAmap, 64*64);
}

unsigned char Alphamap::getAlpha(size_t offset) const
{
  return amap[offset];
}

const unsigned char *Alphamap::getAlpha()
{
  return amap;
}

std::vector<uint8_t> Alphamap::compress() const
{
  std::vector<uint8_t> data(amap, amap+4096);
  auto current (data.begin());
  auto const end (data.end());
  int column_pos = 0;

  auto const consume_fill
  ( 
    [&]
    {
      int8_t count (0);
      column_pos %= 64;

      while ((current + 1 < end) && *current == *(current + 1) && column_pos < 63)
      {
        ++current;
        ++count;
        ++column_pos;
      }

      // include current (current is incremented in the for loop)
      if (count)
      {
        ++count;
        ++column_pos;
      }

      return count;
    }
  );

  std::vector<uint8_t> result;
  std::optional<std::size_t> current_copy_entry_offset{std::nullopt};
  auto const current_copy_entry
  ( 
    [&]
    {
      return reinterpret_cast<compressed_mcal_entry*> (&*(result.begin() + *current_copy_entry_offset));
    }
  );

  for (; current != end; ++current)
  {
    auto const fill (consume_fill());
    if (fill)
    {
      current_copy_entry_offset = std::nullopt;

      result.emplace_back();
      result.emplace_back(*current);

      compressed_mcal_entry* e (reinterpret_cast<compressed_mcal_entry*> (&*(result.rbegin() + 1)));
      e->mode = compressed_mcal_entry::fill;
      e->count = fill;

      column_pos %= 64;
    }
    else
    {
      if ( current_copy_entry_offset == std::nullopt
          || column_pos == 64
          )
      {
        current_copy_entry_offset = result.size();
        result.emplace_back();
        result.emplace_back(*current);
        current_copy_entry()->mode = compressed_mcal_entry::copy;
        current_copy_entry()->count = 1;

        column_pos %= 64;
      }
      else
      {
        result.emplace_back(*current);
        current_copy_entry()->count++;
      }

      column_pos++;
    }
  }

  return result;
}
