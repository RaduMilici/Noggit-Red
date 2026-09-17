// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ChunkWater.hpp>
#include <noggit/db2/ModernDBC.hpp>
#include <noggit/TileWater.hpp>
#include <noggit/liquid_layer.hpp>
#include <noggit/MapChunk.h>
#include <noggit/Misc.h>
#include <ClientFile.hpp>

#include <cmath>

ChunkWater::ChunkWater(MapChunk* chunk, TileWater* water_tile, float x, float z, bool use_mclq_green_lava)
  : xbase(x)
  , zbase(z)
  , vmin(x, 0.f, z)
  , vmax(x + CHUNKSIZE, 0.f, z + CHUNKSIZE)
  , _use_mclq_green_lava(use_mclq_green_lava)
  , _chunk(chunk)
  , _water_tile(water_tile)
{
}

void ChunkWater::from_mclq(std::vector<mclq>& layers, int mcnk_liquid_id)
{
  glm::vec3 pos(xbase, 0.0f, zbase);

  if (!Render.has_value()) Render.emplace();
  for (mclq& liquid : layers)
  {
    // The MCNK header liquid flag is AUTHORITATIVE (e.g. Elwynn lakes/rivers carry lq_river/
    // lq_ocean, so they stay water). Only when the header gives nothing (mcnk_liquid_id == 0 --
    // custom/converted chunks like Draco'dar lava) fall back to the per-tile MCLQ nibble, where
    // bit 0x04 = magma. This stops the over-eager nibble heuristic from turning Elwynn water into
    // lava while still rescuing the flag-less Blackrock lava.
    int liquid_id = mcnk_liquid_id > 0 ? mcnk_liquid_id : 1;
    bool const derive_from_tiles = (mcnk_liquid_id <= 0);
    bool has_visible_liquid = false;

    for (int z = 0; z < 8; ++z)
    {
      for (int x = 0; x < 8; ++x)
      {
        mclq_tile const& tile = liquid.tiles[z * 8 + x];
        std::uint8_t raw_tile = *reinterpret_cast<std::uint8_t const*>(&tile);
        std::uint8_t liquid_type = raw_tile & 0x0F;
        bool const visible = liquid_type != 0x0F && liquid_type != 0x08;

        misc::bit_or(Render.value().fishable, x, z, tile.fishable);
        misc::bit_or(Render.value().fatigue, x, z, tile.fatigue);

        if (visible)
        {
          has_visible_liquid = true;
          if (derive_from_tiles)
          {
            // Fallback per-tile nibble: bit 0x04 = magma (Blackrock lava is types 4 AND 6),
            // 3 = slime, 1 = ocean, else water.
            if (liquid_type & 0x04)
            {
              liquid_id = 3;
            }
            else if (liquid_type == 3 && liquid_id != 3)
            {
              liquid_id = 4;
            }
            else if (liquid_type == 1 && liquid_id == 1)
            {
              liquid_id = 2;
            }
          }
        }
      }
    }

    if (!has_visible_liquid)
    {
      continue;
    }

    _layers.emplace_back(this, pos, liquid, liquid_id);
    _water_tile->tagUpdate();
  }
  update_layers();
}

void ChunkWater::fromFile(BlizzardArchive::ClientFile &f, size_t basePos)
{
  MH2O_Header header;
  f.read(&header, sizeof(MH2O_Header));

  if (!header.nLayers)
  {
    return;
  }

  //render
  if (header.ofsRenderMask)
  {
    Render.emplace();
    f.seek(basePos + header.ofsRenderMask);
    f.read(&Render.value(), sizeof(MH2O_Render));
  }

  for (std::size_t k = 0; k < header.nLayers; ++k)
  {
    MH2O_Information info;
    uint64_t infoMask = 0xFFFFFFFFFFFFFFFF; // default = all water

    //info
    f.seek(basePos + header.ofsInformation + sizeof(MH2O_Information)* k);
    f.read(&info, sizeof(MH2O_Information));

    // Modern (CASC) ADTs: the second u16 is "liquid_object_or_lvf" -- values < 42 are the vertex format
    // itself, values >= 42 are LiquidObject.db2 ids and the format comes from LiquidObject.LiquidTypeID ->
    // LiquidType.MaterialID -> LiquidMaterial.LVF (azeroth_32_48: 5960 -> LiquidType 5 -> material 1 ->
    // LVF 0, height + depth, 5 bytes per vertex in the file), with ocean (LiquidType 2, referenced as the
    // absent LiquidObject 42) depth-only = LVF 2. Reading the harbour's 1-byte-per-vertex ocean layers as
    // LVF 0 produced NaN / 1e38 heights and a fog-coloured wall across the screen (2026-09-16, GL trace
    // call 2953950: the water batch of azeroth_29_48 had 43 such instances). WotLK data never exceeds 3.
    if (info.liquid_vertex_format >= 42)
    {
      info.liquid_vertex_format = static_cast<std::uint16_t>(Noggit::DB2::liquidObjectVertexFormat(info.liquid_vertex_format, info.liquid_id));
      // no vertex data at all = flat layer at min/max height, depth-only (WoWViewerCpp: `!offset_vertex_data
      // && liquid_type != 2 -> 2`; liquid_layer reads nothing when ofsHeightMap is 0 either way)
      if (!info.ofsHeightMap && info.liquid_vertex_format != 2)
      {
        info.liquid_vertex_format = 2;
      }
    }

    //mask
    if (info.ofsInfoMask > 0 && info.height > 0)
    {
      size_t bitmask_size = static_cast<size_t>(std::ceil(info.height * info.width / 8.0f));

      f.seek(info.ofsInfoMask + basePos);
      // only read the relevant data
      f.read(&infoMask, bitmask_size);
    }

    glm::vec3 pos(xbase, 0.0f, zbase);
    _water_tile->tagUpdate();
    _layers.emplace_back(this, f, basePos, pos, info, infoMask);
  }

  update_layers();
}


void ChunkWater::save(sExtendableArray& adt, int base_pos, int& header_pos, int& current_pos)
{
  MH2O_Header header;

  // remove empty layers
  cleanup();

  if (hasData(0))
  {
    header.nLayers = static_cast<std::uint32_t>(_layers.size());

    if (Render.has_value())
    {
        header.ofsRenderMask = current_pos - base_pos;
        adt.Insert(current_pos, sizeof(MH2O_Render), reinterpret_cast<char*>(&Render.value()));
        current_pos += sizeof(MH2O_Render);
    }
    else
    {
        header.ofsRenderMask = 0;
    }

    header.ofsInformation = current_pos - base_pos;
    int info_pos = current_pos;

    std::size_t info_size = sizeof(MH2O_Information) * _layers.size();
    current_pos += static_cast<std::uint32_t>(info_size);

    adt.Extend(static_cast<long>(info_size));

    for (liquid_layer& layer : _layers)
    {
      layer.save(adt, base_pos, info_pos, current_pos);
    }
  }

  memcpy(adt.GetPointer<char>(header_pos), &header, sizeof(MH2O_Header));
  header_pos += sizeof(MH2O_Header);
}


void ChunkWater::removeDuplicateLayers()
{
  bool changed = false;
  for (std::size_t i = 0; i < _layers.size(); )
  {
    bool dup = false;
    for (std::size_t j = 0; j < i; ++j)
    {
      if (_layers[i].liquidID() == _layers[j].liquidID()
          && _layers[i].getSubchunks() == _layers[j].getSubchunks()
          && std::abs(_layers[i].min() - _layers[j].min()) < 0.5f
          && std::abs(_layers[i].max() - _layers[j].max()) < 0.5f)
      {
        dup = true;
        break;
      }
    }
    if (dup)
    {
      _layers.erase(_layers.begin() + i);
      changed = true;
    }
    else
    {
      ++i;
    }
  }
  if (changed)
  {
    update_layers();
  }
}

void ChunkWater::to_mclq(std::vector<mclq>& out) const
{
  // Vanilla MCLQ stores ONE block per liquid type (block count == set type-flag count). MH2O can
  // hold several layers per chunk, but a strict 1.12 client expects a single block per category, so
  // collapse same-category layers here: emit one block per present category (in lq_river/ocean/
  // magma/slime bit order to match the header flags), built from the first layer of that category
  // with the other same-category layers' coverage merged into its tile mask.
  auto nibble_for_cat = [](int cat) -> std::uint8_t
  {
    switch (cat) { case 1: return 1; case 2: return 6; case 3: return 3; default: return 0; }
  };

  for (int cat = 0; cat < 4; ++cat) // 0 water, 1 ocean, 2 magma, 3 slime
  {
    int first = -1;
    for (std::size_t i = 0; i < _layers.size(); ++i)
    {
      if (_layers[i].mclq_liquid_type() == cat) { first = static_cast<int>(i); break; }
    }
    if (first < 0)
    {
      continue;
    }

    mclq block{};
    _layers[first].to_mclq(block);

    std::uint8_t const nibble = nibble_for_cat(cat);
    for (std::size_t k = first + 1; k < _layers.size(); ++k)
    {
      if (_layers[k].mclq_liquid_type() != cat)
      {
        continue;
      }
      for (int z = 0; z < 8; ++z)
      {
        for (int x = 0; x < 8; ++x)
        {
          if (_layers[k].hasSubchunk(x, z))
          {
            std::uint8_t* raw = reinterpret_cast<std::uint8_t*>(&block.tiles[z * 8 + x]);
            if ((*raw & 0x0F) == 0x0F) // was no-render -> include this tile in the merged block
            {
              *raw = nibble;
            }
          }
        }
      }
    }

    out.push_back(block);
  }
}

mcnk_flags ChunkWater::mclq_header_flags() const
{
  mcnk_flags flags;
  flags.value = 0;

  for (liquid_layer const& layer : _layers)
  {
    switch (layer.mclq_liquid_type())
    {
    case 1: flags.flags.lq_ocean = 1; break; // ocean
    case 2: flags.flags.lq_magma = 1; break; // magma
    case 3: flags.flags.lq_slime = 1; break; // slime
    default: flags.flags.lq_river = 1; break; // water
    }
  }

  return flags;
}

void ChunkWater::autoGen(MapChunk *chunk, float factor)
{
  for (liquid_layer& layer : _layers)
  {
    layer.update_opacity(chunk, factor);
  }
  update_layers();
}


void ChunkWater::CropWater(MapChunk* chunkTerrain)
{
  for (liquid_layer& layer : _layers)
  {
    layer.crop(chunkTerrain);
  }
  update_layers();
}

int ChunkWater::getType(size_t layer) const
{
  return hasData(layer) ? _layers[layer].liquidID() : 0;
}

void ChunkWater::setType(int type, size_t layer)
{
  if(hasData(layer))
  {
    _layers[layer].changeLiquidID(type);
  }
  _water_tile->tagUpdate();
}

bool ChunkWater::is_visible ( const float& cull_distance
                            , const math::frustum& frustum
                            , const glm::vec3& camera
                            , display_mode display
                            ) const
{
  static const float chunk_radius = std::sqrt (CHUNKSIZE * CHUNKSIZE / 2.0f);

  float dist = display == display_mode::in_3D
             ? (camera - vcenter).length() - chunk_radius
             : std::abs(camera.y - vmax.y);

  return frustum.intersects(vmax, vmin) && dist < cull_distance;
}

void ChunkWater::update_layers()
{
  std::size_t count = 0;

  auto& extents = _water_tile->getExtents();

  vmin.y = std::numeric_limits<float>::max();
  vmax.y = std::numeric_limits<float>::lowest();

  for (liquid_layer& layer : _layers)
  {
    vmin.y = std::min (vmin.y, layer.min());
    vmax.y = std::max (vmax.y, layer.max());

    extents[0].y = std::min(extents[0].y, vmin.y);
    extents[1].y = std::max(extents[1].y, vmax.y);

    _water_tile->tagUpdate();
    count++;
  }

  _water_tile->tagExtents(true);

  vcenter = (vmin + vmax) * 0.5f;
}

bool ChunkWater::hasData(size_t layer) const
{
  return _layers.size() > layer;
}


void ChunkWater::paintLiquid( glm::vec3 const& pos
                            , float radius
                            , int liquid_id
                            , bool add
                            , math::radians const& angle
                            , math::radians const& orientation
                            , bool lock
                            , glm::vec3 const& origin
                            , bool override_height
                            , bool override_liquid_id
                            , MapChunk* chunk
                            , float opacity_factor
                            )
{
  if (override_liquid_id && !override_height)
  {
    bool layer_found = false;
    for (liquid_layer& layer : _layers)
    {
      if (layer.liquidID() == liquid_id)
      {
        copy_height_to_layer(layer, pos, radius);
        layer_found = true;
        break;
      }
    }

    if (!layer_found)
    {
      liquid_layer layer(this, glm::vec3(xbase, 0.0f, zbase), pos.y, liquid_id);
      copy_height_to_layer(layer, pos, radius);
      _water_tile->tagUpdate();
      _layers.push_back(layer);
    }
  }

  bool painted = false;
  for (liquid_layer& layer : _layers)
  {
    // remove the water on all layers or paint the layer with selected id
    if (!add || layer.liquidID() == liquid_id || !override_liquid_id)
    {
      layer.paintLiquid(pos, radius, add, angle, orientation, lock, origin, override_height, chunk, opacity_factor);
      painted = true;
    }
    else
    {
      layer.paintLiquid(pos, radius, false, angle, orientation, lock, origin, override_height, chunk, opacity_factor);
    }
  }

  cleanup();

  if (!add || painted)
  {
    update_layers();
    return;
  }

  if (hasData(0))
  {
    liquid_layer layer(_layers[0]);
    layer.clear(); // remove the liquid to not override the other layer
    layer.paintLiquid(pos, radius, true, angle, orientation, lock, origin, override_height, chunk, opacity_factor);
    layer.changeLiquidID(liquid_id);
    _water_tile->tagUpdate();
    _layers.push_back(layer);
  }
  else
  {
    liquid_layer layer(this, glm::vec3(xbase, 0.0f, zbase), pos.y, liquid_id);
    layer.paintLiquid(pos, radius, true, angle, orientation, lock, origin, override_height, chunk, opacity_factor);
    _water_tile->tagUpdate();
    _layers.push_back(layer);
  }

  update_layers();
}

void ChunkWater::cleanup()
{
  for (int i = static_cast<int>(_layers.size() - 1); i >= 0; --i)
  {
    if (_layers[i].empty())
    {
      _layers.erase(_layers.begin() + i);
      _water_tile->tagUpdate();
    }
  }
}

void ChunkWater::copy_height_to_layer(liquid_layer& target, glm::vec3 const& pos, float radius)
{
  for (liquid_layer& layer : _layers)
  {
    if (layer.liquidID() == target.liquidID())
    {
      continue;
    }

    for (int z = 0; z < 8; ++z)
    {
      for (int x = 0; x < 8; ++x)
      {
        if (misc::getShortestDist(pos.x, pos.z, xbase + x*UNITSIZE, zbase + z*UNITSIZE, UNITSIZE) <= radius)
        {
          if (layer.hasSubchunk(x, z))
          {
            target.copy_subchunk_height(x, z, layer);
          }
        }
      }
    }
  }
}

void ChunkWater::tagUpdate()
{
  _water_tile->tagUpdate();
}

