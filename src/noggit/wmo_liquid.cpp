// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/Sky.h>
#include <noggit/World.h>
#include <noggit/rendering/LiquidTextureManager.hpp>
#include <noggit/wmo_liquid.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <opengl/context.hpp>
#include <opengl/context.inl>
#include <opengl/shader.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <tuple>

namespace
{

  enum liquid_basic_types
  {
    liquid_basic_types_water = 0,
    liquid_basic_types_ocean = 1,
    liquid_basic_types_magma = 2,
    liquid_basic_types_slime = 3,

    liquid_basic_types_MASK = 3,
  };
  enum liquid_types
  {
    LIQUID_WMO_Water = 13,
    LIQUID_WMO_Ocean = 14,
    LIQUID_Green_Lava = 15,
    LIQUID_WMO_Magma = 19,
    LIQUID_WMO_Slime = 20,
    LIQUID_REAL_Magma = 3,
    LIQUID_REAL_Slime = 4,

    LIQUID_END_BASIC_LIQUIDS = 20,
    LIQUID_FIRST_NONBASIC_LIQUID_TYPE = 21,

    LIQUID_NAXX_SLIME = 21,
  };

  liquid_types to_wmo_liquid(int x, bool ocean)
  {
    liquid_basic_types const basic(static_cast<liquid_basic_types>(x & liquid_basic_types_MASK));
    switch (basic)
    {
      default:
      case liquid_basic_types_water:
        return ocean ? LIQUID_WMO_Ocean : LIQUID_WMO_Water;
      case liquid_basic_types_ocean:
        return LIQUID_WMO_Ocean;
      case liquid_basic_types_magma:
        return LIQUID_WMO_Magma;
      case liquid_basic_types_slime:
        return LIQUID_WMO_Slime;
    }
  }

  bool wmo_liquid_debug_enabled()
  {
    static bool const enabled = []()
    {
      char const* capture_debug = std::getenv("NOGGIT_CAPTURE_DEBUG");
      if (capture_debug && *capture_debug && std::strcmp(capture_debug, "0") != 0)
      {
        return true;
      }

      char const* liquid_debug = std::getenv("NOGGIT_LIQUID_DEBUG");
      return liquid_debug && *liquid_debug && std::strcmp(liquid_debug, "0") != 0;
    }();

    return enabled;
  }

  unsigned wmo_liquid_texture_profile_id(int liquid_id)
  {
    switch (liquid_id)
    {
      case LIQUID_WMO_Water:
        return 1;
      case LIQUID_WMO_Ocean:
        return 2;
      case LIQUID_WMO_Magma:
        return LIQUID_REAL_Magma;
      case LIQUID_WMO_Slime:
        return LIQUID_REAL_Slime;
      default:
        return static_cast<unsigned>(liquid_id);
    }
  }

  bool is_blackrock_lava_wmo_path(std::string const& wmo_path)
  {
    return wmo_path.find("az_blackrock") != std::string::npos
        || wmo_path.find("moltencore") != std::string::npos
        || wmo_path.find("molten_core") != std::string::npos;
  }

  bool is_timbermaw_wmo_path(std::string const& wmo_path)
  {
    return wmo_path.find("kl_timbermawdungeon") != std::string::npos
        || wmo_path.find("timbermaw") != std::string::npos;
  }

  bool is_timbermaw_overhang_liquid_path(std::string const& wmo_path)
  {
    return wmo_path.find("kl_timbermawdungeon/timbermaw_instance_001.wmo") != std::string::npos;
  }

  bool wmo_liquid_debug_color_enabled()
  {
    char const* debug_color = std::getenv("NOGGIT_WMO_LIQUID_DEBUG_COLOR");
    return debug_color && *debug_color && std::strcmp(debug_color, "0") != 0;
  }

  glm::vec4 debug_color_from_path(std::string const& wmo_path)
  {
    std::uint32_t hash = 2166136261u;
    for (unsigned char ch : wmo_path)
    {
      hash ^= ch;
      hash *= 16777619u;
    }

    float const r = 0.25f + static_cast<float>((hash >> 0) & 0xffu) / 255.0f * 0.75f;
    float const g = 0.25f + static_cast<float>((hash >> 8) & 0xffu) / 255.0f * 0.75f;
    float const b = 0.25f + static_cast<float>((hash >> 16) & 0xffu) / 255.0f * 0.75f;
    return glm::vec4(r, g, b, 0.72f);
  }

}

// todo: use material
wmo_liquid::wmo_liquid(BlizzardArchive::ClientFile* f,
                       WMOLiquidHeader const& header,
                       int group_liquid,
                       bool use_dbc_type,
                       bool is_ocean,
                       std::string const& wmo_path,
                       bool interior_material_color,
                       glm::vec3 const& material_color,
                       bool indoor_channel,
                       std::vector<glm::vec3> const* group_vertices)
  : pos(glm::vec3(header.pos.x, header.pos.z, -header.pos.y))
  , xtiles(header.A)
  , ytiles(header.B)
  , _debug_wmo_path(wmo_path)
  , _use_material_color(interior_material_color)
  , _material_color(material_color)
  , _indoor_channel(indoor_channel)
{
  // FLOATLIQ GEOMETRIC CLIP (2026-08-25) -- the exact rules of the PROVEN 3.3.5a data fix
  // (twmoa_toolkit/wmo/wmo_liquid_geometric_clip.py): Turtle authored flat liquid sheets covering a
  // group's whole footprint; the sheet is real data (the live turtle CLIENT shows the same slab at
  // Northshire's abbeygate waterfall) but the 3.3.5a port hides the phantom part. Port: for a FLAT
  // sheet (vertex height span <= 0.5) on a mostly-above-water group (<= 50% mesh verts below), keep
  // only tiles whose cell holds a group mesh vertex at/below waterLevel + 0.5 (a genuine basin).
  // All frames are the noggit swizzle (x, z, -y); wl = baseCoords.z = pos.y.
  if (group_vertices && !group_vertices->empty() && xtiles > 0 && ytiles > 0)
  {
    LiquidVertex const* map = reinterpret_cast<LiquidVertex const*>(f->getPointer());
    int const n_verts = (xtiles + 1) * (ytiles + 1);
    float hmin = std::numeric_limits<float>::max();
    float hmax = std::numeric_limits<float>::lowest();
    for (int v = 0; v < n_verts; ++v)
    {
      hmin = std::min(hmin, map[v].height);
      hmax = std::max(hmax, map[v].height);
    }
    if (hmax - hmin <= 0.5f) // flat sheet only
    {
      float const wl = pos.y;
      float constexpr TS = 4.1666666f;
      std::size_t below = 0;
      std::vector<float> cell_floor(static_cast<std::size_t>(xtiles) * ytiles,
                                    std::numeric_limits<float>::max());
      for (glm::vec3 const& v : *group_vertices)
      {
        if (v.y <= wl)
        {
          ++below;
        }
        int const ti = static_cast<int>((v.x - pos.x) / TS);
        int const tj = static_cast<int>((pos.z - v.z) / TS);
        if (ti >= 0 && ti < xtiles && tj >= 0 && tj < ytiles)
        {
          float& fl = cell_floor[static_cast<std::size_t>(tj) * xtiles + ti];
          fl = std::min(fl, v.y);
        }
      }
      // [2026-09-01 DISABLED] This clip is OFF. Measured on kl_grandmoocanyon (Windhorn Canyon) it
      // hid 30-57% of every pool's liquid tiles -- 611/1925, 375/660, 539/1155, 633/1375 -- which is
      // the "water in chunks, missing chunks" the user reported. A canyon WMO is mostly WALL, so it
      // trips the "mostly above water" test, and then every cell whose measured floor sits above the
      // waterline is discarded -- which in a canyon is most of the map. The rule only holds for a
      // small pool sitting in a basin, not for a liquid sheet spanning terrain-scale geometry.
      //
      // It was added 2026-08-25 for phantom liquid at Northshire's abbeygate waterfall and, per the
      // user, never fixed that either. Re-enabling it needs a test that cannot delete real water:
      // clip against the group's floor UNDER each cell (raycast/nearest-triangle), not "is there a
      // mesh vertex in this 4.17-unit cell", and only for sheets whose footprint is comparable to
      // the pool rather than the whole group.
      if (false)
      {
        _clip_hidden.assign(static_cast<std::size_t>(xtiles) * ytiles, 0);
        for (std::size_t k = 0; k < _clip_hidden.size(); ++k)
        {
          // [2026-09-01 HOLE FIX] cell_floor starts at FLT_MAX and is only lowered for cells that
          // CONTAIN a group mesh vertex. A cell with no vertex therefore kept FLT_MAX and satisfied
          // `> wl + 0.5`, so it was hidden -- even though nothing had been measured there. Liquid
          // tiles are 4.17 units; a low-poly group has vertices in only some of them, so most tiles
          // were clipped away and the surface broke into a regular grid of holes (user report:
          // Windhorn Canyon water "in chunks, missing chunks", same pattern in GL and VK because
          // both consume this same clip mask).
          //
          // Absence of a vertex is not evidence that the floor is above the water. Only hide a cell
          // that was actually measured and measured as dry -- which is what the phantom-sheet fix
          // was after in the first place.
          if (cell_floor[k] != std::numeric_limits<float>::max() && cell_floor[k] > wl + 0.5f)
          {
            _clip_hidden[k] = 1;
          }
        }
      }
    }
  }
  // Lava (LIQUID_Green_Lava) WMOs store authored per-vertex magma s/t flow UVs on EVERY
  // liquid vertex, but the per-tile `tile.liquid & 2` bit is unreliable on these maps:
  // many lava tiles have it clear and were wrongly routed to the water UV path (flat (i,j)
  // grid coords) — so most Molten Core lava rendered as a uniform scrolling sheet while only
  // the tiles that happened to have the bit set flowed. Force the magma UV path for lava so
  // all tiles use their authored flow UVs.
  // Turtle mis-types EVERY WMO liquid as group_liquid=15 (Green_Lava). Force the magma UV path ONLY for
  // REAL lava (Molten Core / Blackrock, by path). A mis-typed WATER WMO (Timbermaw Hold, Stormwind
  // canals, etc.) would otherwise be forced onto lava's per-vertex s/t UVs, which span 0..1 across the
  // WHOLE pool -> the water texture renders hugely zoomed/stretched instead of the fine grid tiling.
  // Non-lava WMOs fall through to the per-tile (tile.liquid & 2) check, which is reliably CLEAR for
  // water tiles (so they take the grid water UV) and SET for genuine magma tiles (correctly-typed
  // magma still resolves). The "& 2 unreliable" caveat only bites CLEAR-on-lava, which the blackrock
  // force still covers.
  bool const force_magma_uv = (group_liquid == LIQUID_Green_Lava) && is_blackrock_lava_wmo_path(wmo_path);
  int liquid = initGeometry(f, wmo_path, force_magma_uv);

  // see: https://wowdev.wiki/WMO#how_to_determine_LiquidTypeRec_to_use
  if (use_dbc_type)
  {
    if (group_liquid == LIQUID_Green_Lava)
    {
      _liquid_id = is_blackrock_lava_wmo_path(wmo_path)
        ? LIQUID_WMO_Magma
        : to_wmo_liquid(liquid, is_ocean);
    }
    else
    {
      _liquid_id = group_liquid;
    }
  }
  else
  {
    if (group_liquid == LIQUID_Green_Lava)
    {
      // todo: investigage
      // This method is most likely wrong since "liquid" is the last SMOLTile's liquid value
      // and it can vary from one liquid tile to another.
      _liquid_id = to_wmo_liquid(liquid, is_ocean);
    }
    else
    {
      if (group_liquid < LIQUID_END_BASIC_LIQUIDS)
      {
        _liquid_id = to_wmo_liquid(group_liquid, is_ocean);
      }
      else
      {
        _liquid_id = group_liquid + 1;
      }
    }
  }

  if (wmo_liquid_debug_enabled())
  {
    LogDebug << "WMO liquid parsed groupLiquid=" << group_liquid
             << " useDbcType=" << (use_dbc_type ? 1 : 0)
             << " ocean=" << (is_ocean ? 1 : 0)
             << " tileLiquid=" << liquid
             << " resolvedLiquidId=" << _liquid_id
             << " wmo='" << wmo_path << "'"
             << " tiles=" << xtiles << "x" << ytiles
             << " indices=" << _indices_count
             << std::endl;
  }
}

wmo_liquid::wmo_liquid(wmo_liquid const& other)
  : pos(other.pos)
  , mTransparency(other.mTransparency)
  , xtiles(other.xtiles)
  , ytiles(other.ytiles)
  , _liquid_id(other._liquid_id)
  , _debug_wmo_path(other._debug_wmo_path)
  , _use_material_color(other._use_material_color)
  , _material_color(other._material_color)
  , _indoor_channel(other._indoor_channel)
  , depths(other.depths)
  , tex_coords(other.tex_coords)
  , vertices(other.vertices)
  , indices(other.indices)
  , _uploaded(false)
{

}


unsigned wmo_liquid::vkTextureProfileId(int liquid_id)
{
  return wmo_liquid_texture_profile_id(liquid_id);
}

int wmo_liquid::initGeometry(BlizzardArchive::ClientFile* f, std::string const& wmo_path, bool force_magma_uv)
{
  LiquidVertex const* map = reinterpret_cast<LiquidVertex const*>(f->getPointer());
  SMOLTile const* tiles = reinterpret_cast<SMOLTile const*>(f->getPointer() + (xtiles + 1)*(ytiles + 1) * sizeof(LiquidVertex));
  std::uint8_t const* raw_tiles = reinterpret_cast<std::uint8_t const*>(tiles);
  int last_liquid_id = 0;
  std::array<int, 256> raw_counts{};
  int rendered_tile_count = 0;
  int shared_tile_count = 0;
  int rendered_shared_tile_count = 0;
  int hidden_tile_count = 0;
  float min_height = std::numeric_limits<float>::max();
  float max_height = std::numeric_limits<float>::lowest();
  float min_depth = std::numeric_limits<float>::max();
  float max_depth = std::numeric_limits<float>::lowest();
  double depth_sum = 0.0;
  int depth_count = 0;

  // generate vertices
  std::vector<glm::vec3> lVertices ((xtiles + 1)*(ytiles + 1));

  for (int j = 0; j<ytiles + 1; j++)
  {
    for (int i = 0; i<xtiles + 1; ++i)
    {
      size_t p = j*(xtiles + 1) + i;
      lVertices[p] = glm::vec3( pos.x + UNITSIZE * i
                                    , map[p].height
                                    , pos.z - UNITSIZE * j
                                    );
    }
  }

  // Probe data (heightAtLocal, FLOATLIQ 2026-08-24): the full vertex-height grid + per-tile
  // rendered flags, so submersion tests are per-tile like the client instead of pool-AABB/max.
  _grid_heights.resize((xtiles + 1) * (ytiles + 1));
  for (int v = 0; v < (xtiles + 1) * (ytiles + 1); ++v)
  {
    _grid_heights[v] = map[v].height;
  }
  _tile_rendered.assign(static_cast<std::size_t>(xtiles) * ytiles, 0);

  // CLIENT-EXACT legacy type resolution (wow112 FUN_006ba970): the FIRST tile whose low nibble is
  // not 0xF ("no liquid") gives the group's type nibble. (Was: the LAST rendered tile's value,
  // which let one stray trailing tile re-type the whole pool -- the Stormwind-canal-green bug.)
  int first_liquid_nibble = 0xF;
  bool first_liquid_found = false;

  std::uint16_t index (0);

  for (int j = 0; j<ytiles; j++)
  {
    for (int i = 0; i<xtiles; ++i)
    {
      SMOLTile const& tile = tiles[j*xtiles + i];
      std::uint8_t const raw_tile = raw_tiles[j * xtiles + i];
      ++raw_counts[raw_tile];
      if ((raw_tile & 0x80) != 0)
      {
        ++shared_tile_count;
      }

      bool render_tile = (tile.liquid & 0x8) == 0
                       && (_clip_hidden.empty() || !_clip_hidden[static_cast<std::size_t>(j) * xtiles + i]);
      if (!render_tile)
      {
        ++hidden_tile_count;
      }
      _tile_rendered[static_cast<std::size_t>(j) * xtiles + i] = render_tile ? 1 : 0;
      if (!first_liquid_found && (raw_tile & 0xF) != 0xF)
      {
        first_liquid_nibble = raw_tile & 0xF;
        first_liquid_found = true;
      }

      // it seems that if (liquid & 8) != 0 => do not render
      if (render_tile)
      {
        ++rendered_tile_count;
        if ((raw_tile & 0x80) != 0)
        {
          ++rendered_shared_tile_count;
        }

        last_liquid_id = tile.liquid;

        size_t p = j*(xtiles + 1) + i;

        if (!force_magma_uv && !(tile.liquid & 2))
        {
          auto record_water_depth = [&](std::size_t vertex_index)
          {
            float const height = map[vertex_index].height;
            float const depth = static_cast<float>(map[vertex_index].water_vertex.flow1) / 255.0f;
            min_height = std::min(min_height, height);
            max_height = std::max(max_height, height);
            min_depth = std::min(min_depth, depth);
            max_depth = std::max(max_depth, depth);
            depth_sum += depth;
            ++depth_count;
          };

          record_water_depth(p);
          record_water_depth(p + 1);
          record_water_depth(p + xtiles + 1 + 1);
          record_water_depth(p + xtiles + 1);

          depths.emplace_back(static_cast<float>(map[p].water_vertex.flow1) / 255.0f);
          tex_coords.emplace_back(i, j);

          depths.emplace_back(static_cast<float>(map[p + 1].water_vertex.flow1) / 255.0f);
          tex_coords.emplace_back(i+1, j);

          depths.emplace_back(static_cast<float>(map[p + xtiles + 1 + 1].water_vertex.flow1) / 255.0f);
          tex_coords.emplace_back(i+1, j+1);

          depths.emplace_back(static_cast<float>(map[p + xtiles + 1].water_vertex.flow1) / 255.0f);
          tex_coords.emplace_back(i, j+1);
        }
        else
        {
          min_height = std::min({min_height,
                                 map[p].height,
                                 map[p + 1].height,
                                 map[p + xtiles + 1 + 1].height,
                                 map[p + xtiles + 1].height});
          max_height = std::max({max_height,
                                 map[p].height,
                                 map[p + 1].height,
                                 map[p + xtiles + 1 + 1].height,
                                 map[p + xtiles + 1].height});

          // todo handle that properly
          // depth isn't used for lava, it's just to fill up the buffer
          depths.emplace_back(1.f);
          depths.emplace_back(1.f);
          depths.emplace_back(1.f);
          depths.emplace_back(1.f);

          tex_coords.emplace_back( static_cast<float>(map[p].magma_vertex.s) / 255.f
                                 , static_cast<float>(map[p].magma_vertex.t) / 255.f
                                 );
          tex_coords.emplace_back( static_cast<float>(map[p + 1].magma_vertex.s) / 255.f
                                 , static_cast<float>(map[p + 1].magma_vertex.t) / 255.f
                                 );
          tex_coords.emplace_back( static_cast<float>(map[p + xtiles + 1 + 1].magma_vertex.s) / 255.f
                                 , static_cast<float>(map[p + xtiles + 1 + 1].magma_vertex.t) / 255.f
                                 );
          tex_coords.emplace_back( static_cast<float>(map[p + xtiles + 1].magma_vertex.s) / 255.f
                                 , static_cast<float>(map[p + xtiles + 1].magma_vertex.t) / 255.f
                                 );
        }

        vertices.emplace_back (lVertices[p]);
        vertices.emplace_back (lVertices[p + 1]);
        vertices.emplace_back (lVertices[p + xtiles + 1 + 1]);
        vertices.emplace_back (lVertices[p + xtiles + 1]);

        indices.emplace_back(index);
        indices.emplace_back(index+1);
        indices.emplace_back(index+2);

        indices.emplace_back(index+2);
        indices.emplace_back(index+3);
        indices.emplace_back(index);

        index += 4;
      }
    }
  }

  // [2026-09-01 STAIRCASE FIX] WMO liquid depth is the authored `water_vertex.flow1 / 255`, and the
  // open-air-pool path now mixes the shallow->deep colour by it (the round-11 change that stopped
  // flat river-deep pools standing out against the ADT river beside them). That mix is only
  // meaningful if the WMO actually authored a depth GRADIENT. Turtle's WMOs frequently author flow1
  // as one or two values, so the mix snapped between two colours along the 4.17-unit liquid tile
  // grid -- a hard staircase of two shades across a pool with no real deep part (user report,
  // Windhorn Canyon). Pre-round-11 this could not happen because the pool was drawn flat.
  //
  // So: use the gradient only when there IS one. With fewer than three distinct authored values
  // there is nothing to interpolate, and every vertex takes the mean -- one flat colour, which is
  // what the pool looked like before the depth mix existed. Depth also drives ALPHA, so the mean
  // (rather than 0 or 1) keeps the opacity where the artist put it.
  if (!depths.empty())
  {
    float dmin = depths[0], dmax = depths[0], dsum = 0.0f;
    std::vector<float> distinct;
    for (float d : depths)
    {
      dmin = std::min(dmin, d);
      dmax = std::max(dmax, d);
      dsum += d;
      if (distinct.size() < 3
          && std::find_if(distinct.begin(), distinct.end(),
                          [d](float e) { return std::abs(e - d) < 1.0f / 255.0f; }) == distinct.end())
      {
        distinct.push_back(d);
      }
    }
    // Measured on this map: depthMin=0 depthMax=1 with the values sitting AT the extremes -- a
    // bimodal mask, not a gradient. The distinct-value test below missed it because a handful of
    // intermediate values exist. Treat "almost everything is at 0 or 1" as no gradient too.
    std::size_t extremes = 0;
    for (float d : depths)
    {
      if (d <= 2.0f / 255.0f || d >= 253.0f / 255.0f) ++extremes;
    }
    bool const bimodal = extremes * 10 >= depths.size() * 9;   // >=90% at an extreme
    if (bimodal || distinct.size() < 3 || (dmax - dmin) < 4.0f / 255.0f)
    {
      float const mean = dsum / static_cast<float>(depths.size());
      std::fill(depths.begin(), depths.end(), mean);
    }
  }

  {
    static int wdiag = 0;
    if (wdiag < 25)
    {
      float dmn = 1e9f, dmx = -1e9f;
      for (float d : depths) { dmn = std::min(dmn, d); dmx = std::max(dmx, d); }
      std::size_t hidden = 0;
      for (std::uint8_t h : _clip_hidden) { if (h) ++hidden; }
      LogError << "[WATERDIAG-WMO] " << wmo_path
               << " tiles=" << xtiles << "x" << ytiles
               << " verts=" << vertices.size()
               << " depthMin=" << (depths.empty() ? -1.f : dmn)
               << " depthMax=" << (depths.empty() ? -1.f : dmx)
               << " clipHidden=" << hidden << "/" << _clip_hidden.size()
               << std::endl;
      wdiag++;
    }
  }

  // [VULKAN] mirror the finished geometry: GL uploads `vertices`/`indices` from locals that go out of
  // scope here, and `depths`/`tex_coords` are parallel to the vertex list.
  {
    _vk_verts.clear();
    _vk_verts.reserve(vertices.size() * 6);
    for (std::size_t i = 0; i < vertices.size(); ++i)
    {
      _vk_verts.push_back(vertices[i].x);
      _vk_verts.push_back(vertices[i].y);
      _vk_verts.push_back(vertices[i].z);
      _vk_verts.push_back(i < depths.size() ? depths[i] : 1.f);
      _vk_verts.push_back(i < tex_coords.size() ? tex_coords[i].x : 0.f);
      _vk_verts.push_back(i < tex_coords.size() ? tex_coords[i].y : 0.f);
    }
    _vk_indices.assign(indices.begin(), indices.end());
  }

  _indices_count = static_cast<int>(indices.size());

  if (wmo_liquid_debug_enabled() && is_timbermaw_wmo_path(wmo_path))
  {
    std::ostringstream raw_summary;
    for (std::size_t raw = 0; raw < raw_counts.size(); ++raw)
    {
      if (raw_counts[raw] == 0)
      {
        continue;
      }

      if (raw_summary.tellp() > 0)
      {
        raw_summary << ", ";
      }

      raw_summary << "0x" << std::hex << raw << std::dec << ":" << raw_counts[raw];
    }

    LogDebug << "Timbermaw WMO liquid tile stats wmo='" << wmo_path << "'"
             << " tiles=" << xtiles << "x" << ytiles
             << " renderedTiles=" << rendered_tile_count
             << " hiddenTiles=" << hidden_tile_count
             << " sharedTiles=" << shared_tile_count
             << " renderedSharedTiles=" << rendered_shared_tile_count
             << " heightRange=[" << (rendered_tile_count ? min_height : 0.0f)
             << ", " << (rendered_tile_count ? max_height : 0.0f) << "]"
             << " depthRange=[" << (depth_count ? min_depth : 0.0f)
             << ", " << (depth_count ? max_depth : 0.0f) << "]"
             << " depthAvg=" << (depth_count ? depth_sum / static_cast<double>(depth_count) : 0.0)
             << " rawCounts={" << raw_summary.str() << "}"
             << std::endl;
  }

  // Client rule (FUN_006ba970): first non-0xF tile nibble; all-0xF -> 0xF (callers mask & 3).
  if (first_liquid_found)
  {
    return first_liquid_nibble;
  }
  return last_liquid_id;
}

void wmo_liquid::upload(OpenGL::Scoped::use_program& water_shader)
{
  _buffer.upload();
  _vertex_array.upload();

  gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>(_indices_buffer, indices, GL_STATIC_DRAW);

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>(_vertices_buffer, vertices, GL_STATIC_DRAW);
  gl.bufferData<GL_ARRAY_BUFFER, glm::vec2>(_tex_coord_buffer, tex_coords, GL_STATIC_DRAW);
  gl.bufferData<GL_ARRAY_BUFFER, float>(_depth_buffer, depths, GL_STATIC_DRAW);

  OpenGL::Scoped::index_buffer_manual_binder indices_binder (_indices_buffer);

  {
    OpenGL::Scoped::vao_binder const _ (_vao);
    
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vertices_binder (_vertices_buffer);
    water_shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);

    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const tex_coord_binder(_tex_coord_buffer);
    water_shader.attrib("tex_coord", 2, GL_FLOAT, GL_FALSE, 0, 0);

    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const depth_binder(_depth_buffer);
    water_shader.attrib("depth", 1, GL_FLOAT, GL_FALSE, 0, 0);

    indices_binder.bind();
  }

  _uploaded = true;
}

void wmo_liquid::draw ( glm::mat4x4 const& transform
                      , OpenGL::Scoped::use_program& water_shader
                      , Noggit::Rendering::LiquidTextureManager& texture_manager
                      , int animtime
                      )
{
  if (indices.empty())
  {
    return;
  }

  if (is_timbermaw_overhang_liquid_path(_debug_wmo_path) && xtiles == 40 && ytiles == 40)
  {
    if (wmo_liquid_debug_enabled())
    {
      LogDebug << "WMO liquid culled Timbermaw overhang plane wmo='" << _debug_wmo_path << "'"
               << " tiles=" << xtiles << "x" << ytiles
               << std::endl;
    }
    return;
  }

  if (char const* filter = std::getenv("NOGGIT_WMO_LIQUID_PATH_FILTER"))
  {
    if (*filter && std::strcmp(filter, "0") != 0 && _debug_wmo_path.find(filter) == std::string::npos)
    {
      return;
    }
  }

  if (!_uploaded)
  {
    upload(water_shader);
  }

  auto const& texture_frames = texture_manager.getTextureFrames();
  unsigned const texture_profile_id = wmo_liquid_texture_profile_id(_liquid_id);
  auto texture_profile = texture_frames.find(texture_profile_id);
  if (texture_profile == texture_frames.end())
  {
    if (wmo_liquid_debug_enabled())
    {
      LogDebug << "WMO liquid skipped missing profile liquidId=" << _liquid_id
               << " profileId=" << texture_profile_id
               << " indices=" << _indices_count
               << std::endl;
    }
    return;
  }

  auto const& [texture_array, anim_uv, liquid_type, frame_count] = texture_profile->second;
  int const frame = frame_count == 0
    ? 0
    : static_cast<int>((static_cast<unsigned>(std::max(animtime, 0)) / 60u) % frame_count); // ~16.7fps, matches ADT/reference

  // Lava flow tuning. magma_flow_dir is the scroll direction in UV space; magma_flow_speed is
  // UV units per ms. The LiquidType anim direction (anim_uv ~ (1,0)) scrolls along the authored
  // U axis, which runs ACROSS the lava channels -> reads as side-to-side. The authored V axis
  // runs ALONG the channels, so scroll there for downstream "oozing down the river" flow.
  // Speed: old hard-coded rate was anim/2880 per ms; 0.25/2880 is a quarter of that.
  glm::vec2 const magma_flow_dir = glm::vec2(0.0f, 1.0f);
  float const magma_flow_speed = 0.25f / 2880.0f;

  water_shader.uniform ("transform", transform);
  water_shader.uniform ("animtime", static_cast<float>(animtime));
  water_shader.uniform ("tex_frame", frame);
  water_shader.uniform ("liquid_type", liquid_type);
  water_shader.uniform ("anim_uv", anim_uv);
  water_shader.uniform ("magma_flow_dir", magma_flow_dir);
  water_shader.uniform ("magma_flow_speed", magma_flow_speed);
  water_shader.uniform ("use_material_color", _use_material_color ? 1 : 0);
  water_shader.uniform ("material_color", _material_color);
  // Exterior WMO water flat colour = the WATER param's river-deep band (Skies computes it per
  // frame) -- the CLEAR param's band is the muddy teal, the WATER param's is the canal dark blue.
  water_shader.uniform ("wmo_water_river_dark", Skies::water_river_dark());
  // City channel (indoor+exterior_lit: canals/harbor/Booty Bay) vs open-air WMO pool (abbeygate
  // stream) -- picks the ocean-dark opaque look vs the river-blend look in the shader.
  water_shader.uniform ("wmo_indoor_channel", _indoor_channel ? 1 : 0);
  water_shader.uniform ("debug_liquid_color", wmo_liquid_debug_color_enabled()
                                                  ? debug_color_from_path(_debug_wmo_path)
                                                  : glm::vec4(0.0f));

  if (wmo_liquid_debug_enabled())
  {
    LogDebug << "WMO liquid draw liquidId=" << _liquid_id
             << " profileId=" << texture_profile_id
             << " liquidType=" << liquid_type
             << " frame=" << frame
             << " frameCount=" << frame_count
             << " anim=(" << anim_uv.x << ", " << anim_uv.y << ")"
             << " indices=" << _indices_count
             << " wmo='" << _debug_wmo_path << "'"
             << std::endl;
  }

  gl.activeTexture(GL_TEXTURE2);
  gl.bindTexture(GL_TEXTURE_2D_ARRAY, texture_array);

  OpenGL::Scoped::vao_binder const _ (_vao);

  OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const cull;
  // Water/slime: do NOT write framebuffer alpha (the scene alpha channel is the bloom's emissive
  // mask). This lets the blend alpha reach the client's authored 1.0 (deep canal water is OPAQUE --
  // the old 0.85 cap kept the bottom visible) without water registering as emissive. Lava keeps
  // its alpha write (alpha 1.0 IS its emissive/bloom flag).
  bool const is_lava = (liquid_type == 2);
  if (!is_lava)
  {
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
  }
  gl.drawElements(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_SHORT, nullptr);
  if (!is_lava)
  {
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  }
}
