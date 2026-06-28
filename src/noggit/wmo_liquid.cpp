// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBC.h>
#include <noggit/Log.h>
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
                       std::string const& wmo_path)
  : pos(glm::vec3(header.pos.x, header.pos.z, -header.pos.y))
  , xtiles(header.A)
  , ytiles(header.B)
  , _debug_wmo_path(wmo_path)
{
  // Lava (LIQUID_Green_Lava) WMOs store authored per-vertex magma s/t flow UVs on EVERY
  // liquid vertex, but the per-tile `tile.liquid & 2` bit is unreliable on these maps:
  // many lava tiles have it clear and were wrongly routed to the water UV path (flat (i,j)
  // grid coords) — so most Molten Core lava rendered as a uniform scrolling sheet while only
  // the tiles that happened to have the bit set flowed. Force the magma UV path for lava so
  // all tiles use their authored flow UVs.
  bool const force_magma_uv = (group_liquid == LIQUID_Green_Lava);
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
  , depths(other.depths)
  , tex_coords(other.tex_coords)
  , vertices(other.vertices)
  , indices(other.indices)
  , _uploaded(false)
{

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

      bool render_tile = (tile.liquid & 0x8) == 0;
      if (!render_tile)
      {
        ++hidden_tile_count;
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
  gl.drawElements(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_SHORT, nullptr);
}
