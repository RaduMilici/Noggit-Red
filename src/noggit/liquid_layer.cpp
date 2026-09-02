// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBC.h>
#include <noggit/liquid_layer.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/Misc.h>
#include <ClientFile.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
  inline glm::vec2 default_uv(int px, int pz)
  {
    // ONE texture repeat per liquid CELL (4.1666yd) -- the client's scale (the WMO water builder
    // FUN_006b6630 emits per-tile (i,j) UVs on the same 4.1666yd tiles). The old /4 made the
    // texture repeat every 4 cells (~16.7yd) = a 4x zoom ("water texture looks 5-10x enlarged
    // up close", 2026-08-25).
    return {static_cast<float>(px), static_cast<float>(pz)};
  }

  // WoW LiquidType.dbc ids run in groups of four — water, ocean, magma, slime — repeated for the
  // slow / fast / wmo / ... variants (e.g. 7 = "slow magma" in Blackrock, 5 = "slow water"). The
  // vanilla DBC only defines the base ids (1-4, +21), so an MH2O layer authored with a variant id
  // wasn't found and used to fall back to 1 = water -> lava rendered as blue water. Map an unknown
  // id to its base type instead so it still renders as the correct liquid (3.3.5a "just worked"
  // because its DBC defines the variants; this gives vanilla parity).
  int resolve_liquid_id(int liquid_id)
  {
    if (liquid_id >= 1 && gLiquidTypeDB.CheckIfIdExists(liquid_id))
    {
      return liquid_id;
    }
    if (liquid_id < 1)
    {
      return 1;
    }
    int const base[4] = { 1, 2, 3, 4 }; // water, ocean, magma, slime
    int const mapped = base[(liquid_id - 1) % 4];
    return gLiquidTypeDB.CheckIfIdExists(mapped) ? mapped : 1;
  }
}

liquid_layer::liquid_layer(ChunkWater* chunk, glm::vec3 const& base, float height, int liquid_id)
  : _liquid_id(liquid_id)
  , _liquid_vertex_format(0)
  , _minimum(height)
  , _maximum(height)
  , _subchunks(0)
  , pos(base)
  , _chunk(chunk)
{
  _liquid_id = resolve_liquid_id(_liquid_id);

  for (int z = 0; z < 9; ++z)
  {
    for (int x = 0; x < 9; ++x)
    {
       unsigned v_index = z * 9 + x;
      _tex_coords[v_index] = default_uv(x, z);
      _depth[v_index] = 1.0f;
      _vertices[v_index] = glm::vec3(
                            pos.x + UNITSIZE * x
                            , height
                            , pos.z + UNITSIZE * z
                            );
    }
  }

  changeLiquidID(_liquid_id);
  
}

liquid_layer::liquid_layer(ChunkWater* chunk, glm::vec3 const& base, mclq& liquid, int liquid_id)
  : _liquid_id(liquid_id)
  , _liquid_vertex_format(0)
  , _minimum(liquid.min_height)
  , _maximum(liquid.max_height)
  , _subchunks(0)
  , pos(base)
  , _chunk(chunk)
{
  _liquid_id = resolve_liquid_id(_liquid_id);

  changeLiquidID(_liquid_id);

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      mclq_tile const& tile = liquid.tiles[z * 8 + x];
      std::uint8_t raw_tile = *reinterpret_cast<std::uint8_t const*>(&tile);
      std::uint8_t liquid_type = raw_tile & 0x0F;
      misc::set_bit(_subchunks, x, z, liquid_type != 0x0F && liquid_type != 0x08);
    }
  }

  for (int z = 0; z < 9; ++z)
  {
    for (int x = 0; x < 9; ++x)
    {
      const unsigned v_index = z * 9 + x;
      mclq_vertex const& v = liquid.vertices[v_index];

      if (_liquid_vertex_format == 1)
      {
        _depth[v_index] = 1.0f;
        _tex_coords[v_index] = glm::vec2(static_cast<float>(v.magma.x) / 255.f, static_cast<float>(v.magma.y) / 255.f);
      }
      else
      {
        _depth[v_index] = static_cast<float>(v.water.depth) / 255.f;
        _has_authored_depth = true; // MCLQ always carries the client's depth bytes
        _tex_coords[v_index] = default_uv(x, z);
      }

      _vertices[v_index] = glm::vec3(
                            pos.x + UNITSIZE * x
                            // sometimes there's garbage data on unused tiles that mess things up
                            , std::max(std::min(v.height, _maximum), _minimum)
                            , pos.z + UNITSIZE * z
                            );
    }
  }

}

liquid_layer::liquid_layer(ChunkWater* chunk
                           , BlizzardArchive::ClientFile& f
                           , std::size_t base_pos
                           , glm::vec3 const& base
                           , MH2O_Information const& info
                           , std::uint64_t infomask)
  : _liquid_id(info.liquid_id)
  , _liquid_vertex_format(info.liquid_vertex_format)
  , _minimum(info.minHeight)
  , _maximum(info.maxHeight)
  , _subchunks(0)
  , pos(base)
  , _chunk(chunk)
{
  // check if liquid id is valid or some downported maps will crash
  _liquid_id = resolve_liquid_id(_liquid_id);

  int offset = 0;
  for (int z = 0; z < info.height; ++z)
  {
    for (int x = 0; x < info.width; ++x)
    {
      setSubchunk(x + info.xOffset, z + info.yOffset, (infomask >> offset) & 1);
      offset++;
    }
  }

  // default values
  for (int z = 0; z < 9; ++z)
  {
    for (int x = 0; x < 9; ++x)
    {
      const unsigned v_index = z * 9 + x;
      _tex_coords[v_index] = default_uv(x, z);
      _depth[v_index] = 1.0f;
      _vertices[v_index] = glm::vec3(
          pos.x + UNITSIZE * x
        , _minimum
        , pos.z + UNITSIZE * z
      );
    }
  }

  if (info.ofsHeightMap)
  {
    f.seek(base_pos + info.ofsHeightMap);

    if (_liquid_vertex_format == 0 || _liquid_vertex_format == 1)
    {

      for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
      {
        for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
        {
          f.read(&_vertices[z * 9 + x].y, sizeof(float));
        }
      }
    }

    if (_liquid_vertex_format == 1)
    {
      for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
      {
        for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
        {
          mh2o_uv uv;
          f.read(&uv, sizeof(mh2o_uv));
          _tex_coords[z * 9 + x] = 
            { static_cast<float>(uv.x) / 255.f
            , static_cast<float>(uv.y) / 255.f
            };
        }
      }
    }

    if (_liquid_vertex_format == 0 || _liquid_vertex_format == 2)
    {
      for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
      {
        for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
        {
          std::uint8_t depth;
          f.read(&depth, sizeof(std::uint8_t));
          _depth[z * 9 + x] = static_cast<float>(depth) / 255.f;
        }
      }
      _has_authored_depth = true;
    }
  }

  
}

liquid_layer::liquid_layer(liquid_layer&& other)
  : _liquid_id(other._liquid_id)
  , _liquid_vertex_format(other._liquid_vertex_format)
  , _minimum(other._minimum)
  , _maximum(other._maximum)
  , _subchunks(other._subchunks)
  , _vertices(other._vertices)
  , _depth(other._depth)
  , _has_authored_depth(other._has_authored_depth)
  , _tex_coords(other._tex_coords)
  , _indices_by_lod(other._indices_by_lod)
  , pos(other.pos)
  , _chunk(other._chunk)
{
  changeLiquidID(_liquid_id);
}

liquid_layer::liquid_layer(liquid_layer const& other)
  : _liquid_id(other._liquid_id)
  , _liquid_vertex_format(other._liquid_vertex_format)
  , _minimum(other._minimum)
  , _maximum(other._maximum)
  , _subchunks(other._subchunks)
  , _vertices(other._vertices)
  , _depth(other._depth)
  , _has_authored_depth(other._has_authored_depth)
  , _tex_coords(other._tex_coords)
  , _indices_by_lod(other._indices_by_lod)
  , pos(other.pos)
  , _chunk(other._chunk)
{
  changeLiquidID(_liquid_id);
}

liquid_layer& liquid_layer::operator= (liquid_layer&& other)
{
  std::swap(_liquid_id, other._liquid_id);
  std::swap(_liquid_vertex_format, other._liquid_vertex_format);
  std::swap(_minimum, other._minimum);
  std::swap(_maximum, other._maximum);
  std::swap(_subchunks, other._subchunks);
  std::swap(_vertices, other._vertices);
  std::swap(_depth, other._depth);
  std::swap(_has_authored_depth, other._has_authored_depth);
  std::swap(_tex_coords, other._tex_coords);
  std::swap(pos, other.pos);
  std::swap(_indices_by_lod, other._indices_by_lod);
  std::swap(_chunk, other._chunk);

  changeLiquidID(_liquid_id);
  other.changeLiquidID(other._liquid_id);

  return *this;
}

liquid_layer& liquid_layer::operator=(liquid_layer const& other)
{
  changeLiquidID(other._liquid_id);
  _liquid_vertex_format = other._liquid_vertex_format;
  _minimum = other._minimum;
  _maximum = other._maximum;
  _subchunks = other._subchunks;
  _vertices = other._vertices;
  _depth = other._depth;
  _has_authored_depth = other._has_authored_depth;
  _tex_coords = other._tex_coords;
  pos = other.pos;
  _indices_by_lod = other._indices_by_lod;
  _chunk = other._chunk;

  return *this;
}

void liquid_layer::save(sExtendableArray& adt, int base_pos, int& info_pos, int& current_pos) const
{
  int min_x = 9, min_z = 9, max_x = 0, max_z = 0;
  bool filled = true;

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      if (hasSubchunk(x, z))
      {
        min_x = std::min(x, min_x);
        min_z = std::min(z, min_z);
        max_x = std::max(x + 1, max_x);
        max_z = std::max(z + 1, max_z);
      }
      else
      {
        filled = false;
      }
    }
  }

  MH2O_Information info;
  std::uint64_t mask = 0;

  info.liquid_id = _liquid_id;
  info.liquid_vertex_format = _liquid_vertex_format;
  info.minHeight = _minimum;
  info.maxHeight = _maximum;
  info.xOffset = min_x;
  info.yOffset = min_z;
  info.width = max_x - min_x;
  info.height = max_z - min_z;

  if (filled)
  {
    info.ofsInfoMask = 0;
  }
  else
  {
    std::uint64_t value = 1;
    for (int z = info.yOffset; z < info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x < info.xOffset + info.width; ++x)
      {
        if (hasSubchunk(x, z))
        {
          mask |= value;
        }
        value <<= 1;
      }
    }

    if (mask > 0)
    {
      info.ofsInfoMask = current_pos - base_pos;
      adt.Insert(current_pos, 8, reinterpret_cast<char*>(&mask));
      current_pos += 8;
    }
  }

  info.ofsHeightMap = current_pos - base_pos;

  int vertices_count = (info.width + 1) * (info.height + 1);

  if (_liquid_vertex_format == 0 || _liquid_vertex_format == 1)
  {
    adt.Extend(vertices_count * sizeof(float));

    for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
      {
        memcpy(adt.GetPointer<char>(current_pos), &_vertices[z * 9 + x].y, sizeof(float));
        current_pos += sizeof(float);
      }
    }
  }

  if (_liquid_vertex_format == 1)
  {
    adt.Extend(vertices_count * sizeof(mh2o_uv));

    for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
      {
        mh2o_uv uv;
        uv.x = static_cast<std::uint16_t>(std::min(_tex_coords[z * 9 + x].x * 255.f, 65535.f));
        uv.y = static_cast<std::uint16_t>(std::min(_tex_coords[z * 9 + x].y * 255.f, 65535.f));

        memcpy(adt.GetPointer<char>(current_pos), &uv, sizeof(mh2o_uv));
        current_pos += sizeof(mh2o_uv);
      }
    }
  }

  if (_liquid_vertex_format == 0 || _liquid_vertex_format == 2)
  {
    adt.Extend(vertices_count * sizeof(std::uint8_t));

    for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
      {
        std::uint8_t depth = static_cast<std::uint8_t>(std::min(_depth[z * 9 + x] * 255.0f, 255.f));
        memcpy(adt.GetPointer<char>(current_pos), &depth, sizeof(std::uint8_t));
        current_pos += sizeof(std::uint8_t);
      }
    }
  }

  memcpy(adt.GetPointer<char>(info_pos), &info, sizeof(MH2O_Information));
  info_pos += sizeof(MH2O_Information);
}

void liquid_layer::changeLiquidID(int id)
{
  _liquid_id = id;
  // Baseline from the well-known vanilla liquid ids (magma=3, slime=4, Naxx slime=21 use the
  // UV-bearing vertex format 1; water=1/ocean=2 use format 0). The DBC pass below refines it for
  // custom (Turtle) liquids.
  _liquid_vertex_format = (_liquid_id == 3 || _liquid_id == 4 || _liquid_id == 21) ? 1 : 0;

  try
  {
    DBCFile::Record lLiquidTypeRow = gLiquidTypeDB.getByID(_liquid_id);

    // Vanilla (1.12) LiquidType.dbc has only 4 fields: ID, Name, Type(field 2), SpellID(field 3),
    // with Type enum 0=Magma, 2=Slime, 3=Water/Ocean. The WotLK (3.3.5) layout puts Type at field 3
    // (enum 0=water/1=ocean/2=magma/3=slime). Reading the WotLK index on vanilla data picked up the
    // SpellID (0 for magma/slime) -> default branch -> magma & slime were wrongly coerced to the water
    // vertex format 0. Choose the field + enum from the DBC's actual field count.
    if (gLiquidTypeDB.getFieldCount() <= 4)
    {
      int const type = lLiquidTypeRow.getInt(2); // vanilla Type
      _liquid_vertex_format = (type == 0 /*magma*/ || type == 2 /*slime*/) ? 1 : 0;
    }
    else
    {
      switch (lLiquidTypeRow.getInt(LiquidTypeDB::Type)) // WotLK Type (field 3)
      {
      case 2: // magma
      case 3: // slime
        _liquid_vertex_format = 1;
        break;
      default:
        _liquid_vertex_format = 0;
        break;
      }
    }
  }
  catch (...)
  {
  }
}

int liquid_layer::mclq_liquid_type() const
{
  // Returns the base MCLQ category (0 water, 1 ocean, 2 magma, 3 slime) used for the MCNK header
  // liquid flag, the per-tile nibble, and the vertex format. Derive it straight from the liquid id:
  // neither LiquidType.dbc's Type field (vanilla encodes it differently than WotLK) nor
  // _liquid_vertex_format are reliable here (the vanilla DBC even zeroes the magma vertex format).
  // This mirrors the renderer's classification (LiquidTextureManager aliases) and the reader's
  // flag->id mapping (lq_river=1, lq_ocean=2, lq_magma=3, lq_slime=4).
  int const id = resolve_liquid_id(_liquid_id);

  switch (id)
  {
  case 1:  return 0; // water / river
  case 2:  return 1; // ocean
  case 3:  return 2; // magma (lava)
  case 4:  return 3; // slime
  case 21: return 3; // naxxramas slime
  default: break;
  }

  // Non-base ids: WoW liquid ids group in 4s (water, ocean, magma, slime).
  switch ((id - 1) % 4)
  {
  case 1:  return 1; // ocean
  case 2:  return 2; // magma
  case 3:  return 3; // slime
  default: return 0; // water
  }
}

void liquid_layer::to_mclq(mclq& out) const
{
  // Exact inverse of liquid_layer(ChunkWater*, base, mclq&, liquid_id).
  out.min_height = _minimum;
  out.max_height = _maximum;

  int const cat = mclq_liquid_type();           // 0 water, 1 ocean, 2 magma, 3 slime
  bool const magma_format = (cat == 2 || cat == 3); // magma & slime use the s/t vertex layout

  // 9x9 vertices
  for (int z = 0; z < 9; ++z)
  {
    for (int x = 0; x < 9; ++x)
    {
      const unsigned i = z * 9 + x;
      out.vertices[i].height = _vertices[i].y;

      if (magma_format)
      {
        // magma/slime: tex coords stored back as 0..255
        float const mx = std::round(_tex_coords[i].x * 255.f);
        float const my = std::round(_tex_coords[i].y * 255.f);
        out.vertices[i].magma.x = static_cast<std::uint16_t>(std::clamp(mx, 0.f, 65535.f));
        out.vertices[i].magma.y = static_cast<std::uint16_t>(std::clamp(my, 0.f, 65535.f));
      }
      else
      {
        // water: depth as 0..255, the rest are zeroed
        float const d = std::round(_depth[i] * 255.f);
        out.vertices[i].water.depth = static_cast<std::uint8_t>(std::clamp(d, 0.f, 255.f));
        out.vertices[i].water.flow_0_pct = 0;
        out.vertices[i].water.flow_1_pct = 0;
        out.vertices[i].water.filler = 0;
      }
    }
  }

  // Map resolved type -> vanilla MCLQ tile low-nibble liquid_type:
  // water -> 0, ocean -> 1, slime -> 3, magma -> 6.
  std::uint8_t type_nibble;
  switch (mclq_liquid_type())
  {
  case 1:  type_nibble = 1; break; // ocean
  case 2:  type_nibble = 6; break; // magma
  case 3:  type_nibble = 3; break; // slime
  default: type_nibble = 0; break; // water
  }

  // 8x8 tiles
  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      const unsigned t = z * 8 + x;
      std::uint8_t* raw = reinterpret_cast<std::uint8_t*>(&out.tiles[t]);

      if (hasSubchunk(x, z))
      {
        // visible: liquid_type nibble set, dont_render = 0, other flags cleared
        *raw = type_nibble;
      }
      else
      {
        // no subchunk -> 0x0F no-render sentinel (the reader skips low-nibble 0x0F)
        *raw = 0x0F;
      }
    }
  }
}

void liquid_layer::crop(MapChunk* chunk)
{
  if (_maximum < chunk->getMinHeight())
  {
    _subchunks = 0;
  }
  else
  {
    for (int z = 0; z < 8; ++z)
    {
      for (int x = 0; x < 8; ++x)
      {
        if (hasSubchunk(x, z))
        {
          int water_index = 9 * z + x, terrain_index = 17 * z + x;

          if (_vertices[water_index].y < chunk->mVertices[terrain_index].y
            && _vertices[water_index + 1].y < chunk->mVertices[terrain_index + 1].y
            && _vertices[water_index + 9].y < chunk->mVertices[terrain_index + 17].y
            && _vertices[water_index + 10].y < chunk->mVertices[terrain_index + 18].y
            )
          {
            setSubchunk(x, z, false);
          }
        }
      }
    }
  }

  update_min_max();
}

void liquid_layer::update_opacity(MapChunk* chunk, float factor)
{
  for (int z = 0; z < 9; ++z)
  {
    for (int x = 0; x < 9; ++x)
    {
      update_vertex_opacity(x, z, chunk, factor);
    }
  }
}

bool liquid_layer::hasSubchunk(int x, int z, int size) const
{
  for (int pz = z; pz < z + size; ++pz)
  {
    for (int px = x; px < x + size; ++px)
    {
      if ((_subchunks >> (pz * 8 + px)) & 1)
      {
        return true;
      }
    }
  }
  return false;
}

void liquid_layer::setSubchunk(int x, int z, bool water)
{
  misc::set_bit(_subchunks, x, z, water);
}

void liquid_layer::paintLiquid( glm::vec3 const& cursor_pos
                              , float radius
                              , bool add
                              , math::radians const& angle
                              , math::radians const& orientation
                              , bool lock
                              , glm::vec3 const& origin
                              , bool override_height
                              , MapChunk* chunk
                              , float opacity_factor
                              )
{
  glm::vec3 ref ( lock
                      ? origin
                      : glm::vec3 (cursor_pos.x, cursor_pos.y + 1.0f, cursor_pos.z)
                      );

  int id = 0;

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      if (misc::getShortestDist(cursor_pos, _vertices[id], UNITSIZE) <= radius)
      {
        if (add)
        {
          for (int index : {id, id + 1, id + 9, id + 10})
          {
            bool no_subchunk = !hasSubchunk(x, z);
            bool in_range = misc::dist(cursor_pos, _vertices[index]) <= radius;

            if (no_subchunk || (in_range && override_height))
            {
              _vertices[index].y = misc::angledHeight(ref, _vertices[index], angle, orientation);
            }
            if (no_subchunk || in_range)
            {
              update_vertex_opacity(index % 9, index / 9, chunk, opacity_factor);
            }
          }
        }
        setSubchunk(x, z, add);
      }

      id++;
    }
    // to go to the next row of subchunks
    id++;
  }

  update_min_max();
}

void liquid_layer::update_min_max()
{
  _minimum = std::numeric_limits<float>::max();
  _maximum = std::numeric_limits<float>::lowest();
  int x = 0, z = 0;

  for (glm::vec3& v : _vertices)
  {
    if (hasSubchunk(std::min(x, 7), std::min(z, 7)))
    {
      _maximum = std::max(_maximum, v.y);
      _minimum = std::min(_minimum, v.y);
    }

    if (++x == 9)
    {
      z++;
      x = 0;
    }
  }

  // lvf = 2 means the liquid height is 0, switch to lvf 0 when that's not the case
  if (_liquid_vertex_format == 2 && (!misc::float_equals(0.f, _minimum) || !misc::float_equals(0.f, _maximum)))
  {
    _liquid_vertex_format = 0;
  }
  // use lvf 2 when possible to save space
  else if (_liquid_vertex_format == 0 && misc::float_equals(0.f, _minimum) && misc::float_equals(0.f, _maximum))
  {
    _liquid_vertex_format = 2;
  }
}

void liquid_layer::copy_subchunk_height(int x, int z, liquid_layer const& from)
{
  int id = 9 * z + x;

  for (int index : {id, id + 1, id + 9, id + 10})
  {
    _vertices[index].y = from._vertices[index].y;
  }

  setSubchunk(x, z, true);
}

void liquid_layer::update_vertex_opacity(int x, int z, MapChunk* chunk, float factor)
{
  float diff = _vertices[z * 9 + x].y - chunk->mVertices[z * 17 + x].y;
  _depth[z * 9 + x] = diff < 0.0f ? 0.0f : (std::min(1.0f, std::max(0.0f, (diff + 1.0f) * factor)));
}

int liquid_layer::get_lod_level(glm::vec3 const& camera_pos) const
{
  auto const& center_vertex (_vertices[5 * 9 + 4]);
  auto const dist ((center_vertex - camera_pos).length());

  return dist < 1000.f ? 0
       : dist < 2000.f ? 1
       : dist < 4000.f ? 2
       : 3;
}

