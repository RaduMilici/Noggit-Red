// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// Modern (CASC) clients split a map tile into several files addressed by fileDataID out of the WDT's MAID
// chunk (root / obj0 / obj1 / tex0 / ...). The chunk code (MapChunk, TextureSet, ChunkWater) was written
// for the monolithic WotLK ADT, where every MCNK carries a 128-byte header with offsets to its sub-chunks.
// Rather than teaching every reader about three files, this loader reads the split files and rebuilds ONE
// WotLK-shaped MCNK stream in memory (header offsets recomputed, holes converted, texture/shadow/alpha
// sub-chunks spliced in from tex0), then hands it to the unchanged MapChunk constructor.
//
// Measured layout (docs/client_re/41_modern_casc_client_support_research.md, section 5):
//   root  MVER MHDR [MH2O] MCNK x256      MCNK = 128 B header + MCVT + MCNR + MCSE (+ MCCV / MCLV / MCBB / MCDD)
//         header ofs* are zero (except ofsSndEmitters/ofsLiquid = payload size), flags carry 0x8000
//         (do_not_fix_alpha_map) | 0x10000 (high_res_holes: the 64-bit hole mask sits in ofsHeight/ofsNormal)
//   tex0  MVER MAMP MDID MHID MCNK x256   headerless MCNK: MCLY [MCSH] [MCAL]; MDID = texture fileDataIDs
//   obj0  MVER MDDF MODF MCNK x256        MDDF/MODF nameId = fileDataID (flags 0x40 / 0x8); MCRD/MCRW refs
//   obj1  LOD doodads -- ignored
#include <noggit/MapTile.h>
#include <noggit/MapChunk.h>
#include <noggit/MapHeaders.h>
#include <noggit/World.h>
#include <noggit/Log.h>
#include <noggit/ModelInstance.h>
#include <noggit/WMOInstance.h>
#include <noggit/application/NoggitApplication.hpp>

#include <blizzard-archive-library/include/ClientFile.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
  struct SubChunk
  {
    std::size_t pos = 0;   // position of the fourcc
    std::uint32_t size = 0; // payload size
    bool present = false;
  };

  std::uint32_t read_u32(char const* buffer, std::size_t pos)
  {
    std::uint32_t value;
    std::memcpy(&value, buffer + pos, 4);
    return value;
  }

  // Walks a chunk stream in [begin, end): callback(fourcc, payload size, position of the fourcc).
  template<typename F>
  void walk_chunks(char const* buffer, std::size_t begin, std::size_t end, F&& callback)
  {
    std::size_t pos = begin;
    while (pos + 8 <= end)
    {
      std::uint32_t const tag = read_u32(buffer, pos);
      std::uint32_t const size = read_u32(buffer, pos + 4);
      if (pos + 8 + static_cast<std::size_t>(size) > end)
      {
        break;
      }
      callback(tag, size, pos);
      pos += 8 + static_cast<std::size_t>(size);
    }
  }

  // high_res_holes: 8x8 bits (bit = row * 8 + column) -> WotLK 4x4 low-res mask (bit = row * 4 + column);
  // a low-res cell is holed when any of its four high-res cells is.
  std::uint16_t low_res_holes(std::uint64_t mask)
  {
    std::uint16_t low = 0;
    for (int hy = 0; hy < 4; ++hy)
    {
      for (int hx = 0; hx < 4; ++hx)
      {
        bool holed = false;
        for (int dy = 0; dy < 2 && !holed; ++dy)
        {
          for (int dx = 0; dx < 2; ++dx)
          {
            int const bit = (2 * hy + dy) * 8 + (2 * hx + dx);
            if ((mask >> bit) & 1u)
            {
              holed = true;
              break;
            }
          }
        }
        if (holed)
        {
          low |= static_cast<std::uint16_t>(1u << (hy * 4 + hx));
        }
      }
    }
    return low;
  }
}

void MapTile::finishLoadingModern()
{
  using BlizzardArchive::ClientFile;
  using BlizzardArchive::Listfile::FileKey;

  auto const& files = *_modern_files;
  auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();

  auto abort_loading = [&]()
  {
    _tile_is_being_reloaded = false;
    error_on_loading();
  };

  char load_stage[128] = "open";
  try
  {
    // The tile keeps its conventional path (world/maps/<map>/<map>_x_y.adt) next to each part's id, so the
    // project-folder override lookup and every log line still show a name; the archive opens by id.
    auto key_for = [&](std::uint32_t fdid, char const* suffix)
    {
      std::string path = _file_key.hasFilepath() ? _file_key.filepath() : std::string();
      if (!path.empty() && suffix && *suffix)
      {
        auto const dot = path.rfind(".adt");
        if (dot != std::string::npos)
        {
          path.insert(dot, suffix);
        }
      }
      return path.empty() ? FileKey(fdid) : FileKey(path, fdid);
    };

    if (!files.root)
    {
      LogError << "ADT \"" << _file_key.stringRepr() << "\": the WDT lists no root fileDataID for this tile." << std::endl;
      abort_loading();
      return;
    }

    ClientFile root(key_for(files.root, ""), client_data);

    Log << "Opening tile " << index.x << ", " << index.z << " (\"" << _file_key.stringRepr()
        << "\" root=" << files.root << " tex0=" << files.tex0 << " obj0=" << files.obj0
        << ") from " << (root.isExternal() ? "disk" : "CASC") << "." << std::endl;

    if (root.isEof() || root.getSize() < 8)
    {
      LogError << "ADT \"" << _file_key.stringRepr() << "\": root part " << files.root << " is empty." << std::endl;
      abort_loading();
      return;
    }

    // ---------------------------------------------------------------------------------------- root
    std::snprintf(load_stage, sizeof(load_stage), "root chunk walk");
    char const* rb = root.getBuffer();
    std::size_t const rsize = root.getSize();

    MHDR Header;
    std::memset(&Header, 0, sizeof(MHDR));
    bool have_mhdr = false;
    bool version_ok = false;
    std::vector<std::size_t> root_mcnk;
    root_mcnk.reserve(256);

    walk_chunks(rb, 0, rsize, [&](std::uint32_t tag, std::uint32_t size, std::size_t pos)
    {
      if (tag == 'MVER' && size >= 4)
      {
        std::uint32_t const version = read_u32(rb, pos + 8);
        version_ok = version == 18;
      }
      else if (tag == 'MHDR')
      {
        std::memcpy(&Header, rb + pos + 8, std::min<std::size_t>(size, sizeof(MHDR)));
        have_mhdr = true;
      }
      else if (tag == 'MCNK')
      {
        root_mcnk.push_back(pos);
      }
      // MH2O / MFBO are reached through the MHDR offsets below; MAMP and unknown chunks are skipped.
    });

    if (!version_ok || !have_mhdr || root_mcnk.size() != 256)
    {
      LogError << "ADT \"" << _file_key.stringRepr() << "\": unexpected root layout (version ok=" << version_ok
               << " mhdr=" << have_mhdr << " mcnk=" << root_mcnk.size() << "). Aborting tile load." << std::endl;
      abort_loading();
      return;
    }

    mFlags = Header.flags;

    // - MH2O (same reader as WotLK; the LiquidObject rule lives in ChunkWater::fromFile) --------------
    std::snprintf(load_stage, sizeof(load_stage), "MH2O/MFBO");
    if (Header.mh2o != 0 && static_cast<std::size_t>(Header.mh2o) + 0x14 + 8 <= rsize)
    {
      root.seek(Header.mh2o + 0x14);
      std::uint32_t fourcc = 0, size = 0;
      root.read(&fourcc, 4);
      root.read(&size, 4);
      if (fourcc == 'MH2O' && size != 0)
      {
        Water.readFromFile(root, Header.mh2o + 0x14 + 0x8);
      }
    }

    // - MFBO ---------------------------------------------------------------------------------------
    if ((mFlags & 1) && Header.mfbo != 0 && static_cast<std::size_t>(Header.mfbo) + 0x14 + 8 + 36 <= rsize)
    {
      root.seek(Header.mfbo + 0x14);
      std::uint32_t fourcc = 0, size = 0;
      root.read(&fourcc, 4);
      root.read(&size, 4);
      if (fourcc == 'MFBO')
      {
        int16_t mMaximum[9], mMinimum[9];
        root.read(mMaximum, sizeof(mMaximum));
        root.read(mMinimum, sizeof(mMinimum));

        const float xPositions[] = { this->xbase, this->xbase + 266.0f, this->xbase + 533.0f };
        const float yPositions[] = { this->zbase, this->zbase + 266.0f, this->zbase + 533.0f };

        for (int y = 0; y < 3; y++)
        {
          for (int x = 0; x < 3; x++)
          {
            int pos = x + y * 3;
            auto&& z{ std::minmax(mMinimum[pos], mMaximum[pos]) };
            mMinimumValues[pos] = { xPositions[x], static_cast<float>(z.first), yPositions[y] };
            mMaximumValues[pos] = { xPositions[x], static_cast<float>(z.second), yPositions[y] };
          }
        }
      }
    }

    // ---------------------------------------------------------------------------------------- tex0
    std::snprintf(load_stage, sizeof(load_stage), "tex0");
    std::unique_ptr<ClientFile> tex;
    std::vector<std::size_t> tex_mcnk;
    if (_load_textures && files.tex0)
    {
      try
      {
        tex = std::make_unique<ClientFile>(key_for(files.tex0, "_tex0"), client_data);
      }
      catch (std::exception const& e)
      {
        LogError << "ADT \"" << _file_key.stringRepr() << "\": tex0 part " << files.tex0 << " failed: " << e.what()
                 << "; loading terrain without textures." << std::endl;
      }
      if (tex && tex->isEof())
      {
        tex.reset();
      }
    }
    if (tex)
    {
      char const* tb = tex->getBuffer();
      std::vector<std::uint32_t> texture_ids;
      walk_chunks(tb, 0, tex->getSize(), [&](std::uint32_t tag, std::uint32_t size, std::size_t pos)
      {
        if (tag == 'MDID')
        {
          for (std::uint32_t k = 0; k < size / 4; ++k)
          {
            texture_ids.push_back(read_u32(tb, pos + 8 + k * 4));
          }
        }
        else if (tag == 'MCNK')
        {
          tex_mcnk.push_back(pos);
        }
        // MAMP, MHID (height textures) and MTXP are not used by the editor
      });

      for (std::uint32_t fdid : texture_ids)
      {
        std::string path = client_data->listfile()->getPath(fdid);
        if (path.empty())
        {
          // opened by id through the "fdid:" pseudo name (TextureManager)
          path = "fdid:" + std::to_string(fdid);
        }
        mTextureFilenames.push_back(BlizzardArchive::ClientData::normalizeFilenameInternal(path));
      }

      if (tex_mcnk.size() != 256)
      {
        LogError << "ADT \"" << _file_key.stringRepr() << "\": tex0 holds " << tex_mcnk.size()
                 << " MCNK chunks; loading terrain without textures." << std::endl;
        tex.reset();
        tex_mcnk.clear();
      }
    }
    if (!tex)
    {
      _load_textures = false;
    }

    // ---------------------------------------------------------------------------------------- obj0
    std::snprintf(load_stage, sizeof(load_stage), "obj0");
    std::vector<ENTRY_MDDF> lModelInstances;
    std::vector<ENTRY_MODF> lWMOInstances;
    if (_load_models && files.obj0)
    {
      try
      {
        ClientFile obj(key_for(files.obj0, "_obj0"), client_data);
        if (!obj.isEof())
        {
          char const* ob = obj.getBuffer();
          walk_chunks(ob, 0, obj.getSize(), [&](std::uint32_t tag, std::uint32_t size, std::size_t pos)
          {
            if (tag == 'MDDF')
            {
              for (std::uint32_t k = 0; k < size / sizeof(ENTRY_MDDF); ++k)
              {
                ENTRY_MDDF entry;
                std::memcpy(&entry, ob + pos + 8 + k * sizeof(ENTRY_MDDF), sizeof(ENTRY_MDDF));
                lModelInstances.push_back(entry);
              }
            }
            else if (tag == 'MODF')
            {
              for (std::uint32_t k = 0; k < size / sizeof(ENTRY_MODF); ++k)
              {
                ENTRY_MODF entry;
                std::memcpy(&entry, ob + pos + 8 + k * sizeof(ENTRY_MODF), sizeof(ENTRY_MODF));
                lWMOInstances.push_back(entry);
              }
            }
            // per-chunk MCRD/MCRW reference lists are rebuilt from the instance extents, as for WotLK MCRF
          });
        }
      }
      catch (std::exception const& e)
      {
        LogError << "ADT \"" << _file_key.stringRepr() << "\": obj0 part " << files.obj0 << " failed: " << e.what()
                 << "; loading terrain without object instances." << std::endl;
      }
    }

    if (_load_models)
    {
      // nameId IS the fileDataID (MDDF flag 0x40 / MODF flag 0x8 are always set on shipped data; a
      // missing flag would mean an MMDX/MWMO name table the split format does not have, so treat it as
      // an id regardless). The listfile supplies a display path when it knows the id.
      for (auto const& object : lWMOInstances)
      {
        FileKey key(object.nameID);
        key.deduceOtherComponent(client_data->listfile());
        std::snprintf(load_stage, sizeof(load_stage), "wmo-instance uid=%u fdid=%u", object.uniqueID, object.nameID);
        add_model(_world->add_wmo_instance(WMOInstance(key, &object, _context), _tile_is_being_reloaded));
      }

      for (auto const& model : lModelInstances)
      {
        FileKey key(model.nameID);
        key.deduceOtherComponent(client_data->listfile());
        std::snprintf(load_stage, sizeof(load_stage), "m2-instance uid=%u fdid=%u", model.uniqueID, model.nameID);
        add_model(_world->add_model_instance(ModelInstance(key, &model, _context), _tile_is_being_reloaded));
      }

      _world->need_model_updates = true;
    }

    // ---------------------------------------------------------------------- rebuild a WotLK MCNK stream
    std::snprintf(load_stage, sizeof(load_stage), "MCNK rebuild");
    std::vector<char> blob;
    blob.reserve(rsize + (tex ? tex->getSize() : 0) + 256 * 16);
    std::vector<std::size_t> chunk_offsets(256, 0);

    auto append_sub = [&](std::size_t chunk_start, char const* src, std::size_t pos, std::uint32_t size) -> std::uint32_t
    {
      std::uint32_t const relative = static_cast<std::uint32_t>(blob.size() - chunk_start);
      blob.insert(blob.end(), src + pos, src + pos + 8 + size);
      return relative;
    };

    for (int c = 0; c < 256; ++c)
    {
      std::size_t const rpos = root_mcnk[c];
      std::uint32_t const rsz = read_u32(rb, rpos + 4);
      if (rsz < 128)
      {
        LogError << "ADT \"" << _file_key.stringRepr() << "\": root MCNK " << c << " is shorter than its header." << std::endl;
        abort_loading();
        return;
      }

      MapChunkHeader hdr;
      std::memcpy(&hdr, rb + rpos + 8, 128);

      SubChunk mcvt, mcnr, mccv, mcly, mcsh, mcal;
      walk_chunks(rb, rpos + 8 + 128, rpos + 8 + rsz, [&](std::uint32_t tag, std::uint32_t size, std::size_t pos)
      {
        if (tag == 'MCVT') mcvt = { pos, size, true };
        else if (tag == 'MCNR') mcnr = { pos, size, true };
        else if (tag == 'MCCV') mccv = { pos, size, true };
        // MCSE / MCLV / MCBB / MCDD: not consumed by the editor
      });

      char const* tb = nullptr;
      if (tex)
      {
        tb = tex->getBuffer();
        std::size_t const tpos = tex_mcnk[c];
        std::uint32_t const tsz = read_u32(tb, tpos + 4);
        walk_chunks(tb, tpos + 8, tpos + 8 + tsz, [&](std::uint32_t tag, std::uint32_t size, std::size_t pos)
        {
          if (tag == 'MCLY') mcly = { pos, size, true };
          else if (tag == 'MCSH') mcsh = { pos, size, true };
          else if (tag == 'MCAL') mcal = { pos, size, true };
        });
      }

      if (!mcvt.present || mcvt.size < 145 * 4 || !mcnr.present || mcnr.size < 145 * 3)
      {
        LogError << "ADT \"" << _file_key.stringRepr() << "\": root MCNK " << c << " lacks MCVT/MCNR." << std::endl;
        abort_loading();
        return;
      }

      mcnk_flags flags;
      flags.value = hdr.flags;
      if (flags.flags.high_res_holes)
      {
        std::uint64_t const mask = static_cast<std::uint64_t>(hdr.ofsHeight) | (static_cast<std::uint64_t>(hdr.ofsNormal) << 32);
        hdr.holes = (hdr.holes & 0xFFFF0000u) | low_res_holes(mask);
        flags.flags.high_res_holes = 0; // ofsHeight/ofsNormal become real offsets below
      }
      flags.flags.has_mccv = mccv.present ? 1 : 0;
      flags.flags.has_mcsh = mcsh.present ? 1 : 0;
      hdr.flags = flags.value;

      hdr.nLayers = mcly.present ? std::min<std::uint32_t>(4, mcly.size / sizeof(ENTRY_MCLY)) : 0;
      hdr.nDoodadRefs = 0;
      hdr.nMapObjRefs = 0;
      hdr.ofsRefs = 0;
      hdr.ofsSndEmitters = 0;
      hdr.nSndEmitters = 0;
      hdr.ofsLiquid = 0;
      hdr.sizeLiquid = 0;
      hdr.unused1 = 0;
      hdr.unused2 = 0;

      std::size_t const start = blob.size();
      chunk_offsets[c] = start;
      blob.resize(start + 8 + 128); // MCNK fourcc + size + header, filled in below

      hdr.ofsHeight = append_sub(start, rb, mcvt.pos, mcvt.size);
      hdr.ofsNormal = append_sub(start, rb, mcnr.pos, mcnr.size);
      hdr.ofsMCCV = mccv.present ? append_sub(start, rb, mccv.pos, mccv.size) : 0;
      hdr.ofsLayer = (mcly.present && hdr.nLayers) ? append_sub(start, tb, mcly.pos, mcly.size) : 0;
      if (mcsh.present && mcsh.size >= 0x200)
      {
        hdr.ofsShadow = append_sub(start, tb, mcsh.pos, mcsh.size);
        hdr.sizeShadow = mcsh.size + 8;
      }
      else
      {
        hdr.ofsShadow = 0;
        hdr.sizeShadow = 0;
        flags.flags.has_mcsh = 0;
        hdr.flags = flags.value;
      }
      if (mcal.present)
      {
        hdr.ofsAlpha = append_sub(start, tb, mcal.pos, mcal.size);
        hdr.sizeAlpha = mcal.size + 8;
      }
      else
      {
        hdr.ofsAlpha = 0;
        hdr.sizeAlpha = 0;
      }

      std::uint32_t const tag = 'MCNK';
      std::uint32_t const chunk_size = static_cast<std::uint32_t>(blob.size() - start - 8);
      std::memcpy(blob.data() + start, &tag, 4);
      std::memcpy(blob.data() + start + 4, &chunk_size, 4);
      std::memcpy(blob.data() + start + 8, &hdr, 128);
    }

    // ---------------------------------------------------------------------------------------- chunks
    ClientFile merged(_file_key, client_data, ClientFile::NEW_FILE);
    merged.setBuffer(std::move(blob));

    for (int nextChunk = 0; nextChunk < 256; ++nextChunk)
    {
      std::snprintf(load_stage, sizeof(load_stage), "MCNK %d (ofs=0x%zX)", nextChunk, chunk_offsets[nextChunk]);
      merged.seek(chunk_offsets[nextChunk]);

      unsigned x = nextChunk / 16;
      unsigned z = nextChunk % 16;

      mChunks[x][z] = std::make_unique<MapChunk>(this, &merged, mBigAlpha, _mode, _context, false, 0, _load_textures);

      auto& chunk = mChunks[x][z];
      _renderer.initChunkData(chunk.get());
    }

    root.close();

    LogDebug << "Done loading modern tile " << index.x << "," << index.z << "." << std::endl;
    finished = true;
    _tile_is_being_reloaded = false;
    _state_changed.notify_all();
  }
  catch (std::exception const& e)
  {
    LogError << "MapTile " << index.x << "," << index.z << " (\"" << _file_key.stringRepr()
             << "\") modern load exception at stage: " << load_stage << " -- " << e.what() << std::endl;
    throw;
  }
  catch (...)
  {
    LogError << "MapTile " << index.x << "," << index.z << " (\"" << _file_key.stringRepr()
             << "\") modern load exception at stage: " << load_stage << std::endl;
    throw;
  }
}
