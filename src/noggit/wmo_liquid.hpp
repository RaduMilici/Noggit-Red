// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once
#include <noggit/TextureManager.h>
#include <opengl/scoped.hpp>

#include <memory>
#include <optional>
#include <algorithm>
#include <string>

namespace BlizzardArchive
{
  class ClientFile;
}

namespace Noggit::Rendering
{
  class LiquidTextureManager;
}

struct CImVector
{
  std::uint8_t b;
  std::uint8_t g;
  std::uint8_t r;
  std::uint8_t a;
};

struct CArgb
{
  std::uint8_t r;
  std::uint8_t g;
  std::uint8_t b;
  std::uint8_t a;
};

struct SMOLTile
{
  uint8_t liquid : 6;
  uint8_t fishable : 1;
  uint8_t shared : 1;
};

struct WMOMaterial 
{
  union
  {
    uint32_t value;
    struct
    {
      uint32_t unlit :  1;
      uint32_t unfogged : 1;
      uint32_t unculled : 1;
      uint32_t ext_light: 1; // darkened used for the intern face of windows
      uint32_t sidn :  1;
      uint32_t window :  1; // lighting related(flag checked in CMapObj::UpdateSceneMaterials)
      uint32_t clamp_s :  1;
      uint32_t clamp_t : 1;
      uint32_t unused : 24;
    };
  } flags;
  uint32_t shader;
  uint32_t blend_mode; // Blending: 0 for opaque, 1 for transparent
  uint32_t texture_offset_1; // Start position for the first texture filename in the MOTX data block
  CImVector sidn_color; // emissive color
  CImVector frame_sidn_color; // runtime value
  uint32_t texture_offset_2; // Start position for the second texture filename in the MOTX data block
  CArgb diffuse_color;
  uint32_t ground_type;
  uint32_t texture_offset_3; 
  uint32_t color_2;
  uint32_t flag_2;
  uint32_t runtime_data[2];
  // also runtime data
  uint32_t texture1; // this is the first texture object.
  uint32_t texture2; // this is the second texture object.
};

struct WMOLiquidHeader {
  int32_t X, Y, A, B;
  glm::vec3 pos;
  int16_t material_id;
};

struct SMOWVert
{
  std::uint8_t flow1;
  std::uint8_t flow2;
  std::uint8_t flow1Pct;
  std::uint8_t filler;
};
struct SMOMVert
{
  std::int16_t s;
  std::int16_t t;
};

struct LiquidVertex {
  union
  {
    SMOWVert water_vertex;
    SMOMVert magma_vertex;
  };
  float height;
};

class wmo_liquid
{
public:
  wmo_liquid(BlizzardArchive::ClientFile* f,
             WMOLiquidHeader const& header,
             int group_liquid,
             bool use_dbc_type,
             bool is_ocean,
             std::string const& wmo_path,
             bool interior_material_color = false,
             glm::vec3 const& material_color = glm::vec3(0.0f),
             bool indoor_channel = false,
             bool dbc_exterior = false,
             std::vector<glm::vec3> const* group_vertices = nullptr);
  wmo_liquid(wmo_liquid const& other);

  // [game mode] local-space surface height when local_pos lies within this liquid's XZ extent
  // (WMO pools are flat-ish: the max vertex height). nullopt outside.
  // TILE-ACCURATE submersion probe (FLOATLIQ, 2026-08-24). The old version returned the MAX height
  // of every rendered vertex over the whole pool's XZ AABB -- on multi-level liquid (MC lava spans
  // -110..-20) that flooded every room below the pool's HIGHEST ledge ("starts swimming in a lot of
  // sections"), and phantom Z~0 sheets extended the AABB. Correct = find the tile under the point,
  // require it RENDERED (hidden bit 0x8 respected, exactly like the render path), and bilinear the
  // 4 corner heights of THAT tile -- the 1.12 client's per-tile behaviour.
  std::optional<float> heightAtLocal(glm::vec3 const& local_pos) const
  {
    if (_tile_rendered.empty() || _grid_heights.empty())
    {
      return std::nullopt;
    }
    float const fx = (local_pos.x - pos.x) / 4.1666666f;
    float const fz = (pos.z - local_pos.z) / 4.1666666f;
    if (fx < 0.0f || fz < 0.0f || fx >= static_cast<float>(xtiles) || fz >= static_cast<float>(ytiles))
    {
      return std::nullopt;
    }
    int const i = static_cast<int>(fx);
    int const j = static_cast<int>(fz);
    if (!_tile_rendered[j * xtiles + i])
    {
      return std::nullopt;
    }
    float const tx = fx - static_cast<float>(i);
    float const tz = fz - static_cast<float>(j);
    int const stride = xtiles + 1;
    float const h =
      (_grid_heights[j * stride + i] * (1.0f - tx) + _grid_heights[j * stride + i + 1] * tx) * (1.0f - tz)
      + (_grid_heights[(j + 1) * stride + i] * (1.0f - tx) + _grid_heights[(j + 1) * stride + i + 1] * tx) * tz;
    return h;
  }

  int liquid_id() const { return _liquid_id; }
  // [VULKAN] geometry mirror captured in initGeometry (the GL path discards its locals).
  // 6 floats per vertex: pos xyz | depth | uv xy.
  std::vector<float> const& vkVertices() const { return _vk_verts; }
  std::vector<std::uint16_t> const& vkIndices() const { return _vk_indices; }
  glm::vec3 const& materialColor() const { return _material_color; }
  bool vkUseMaterialColor() const { return _use_material_color; }
  bool vkIndoorChannel() const { return _indoor_channel; }
  bool vkDbcExterior() const { return _dbc_exterior; }
  // exterior water colour law: 0 = legacy open-air pool (user-tuned river blend), 1 = city channel
  // (indoor+exterior-lit, user-tuned navy), 2 = DBC-path exterior water = the client's flat river-deep
  int waterMode() const { return _indoor_channel ? 1 : (_dbc_exterior ? 2 : 0); }
  // LiquidTextureManager profile key -- WMO liquid ids are remapped before the lookup, so VK must
  // use the SAME mapping GL's draw() does or it picks the wrong texture/anim/type.
  static unsigned vkTextureProfileId(int liquid_id);

  void upload(OpenGL::Scoped::use_program& water_shader);
  void draw(glm::mat4x4 const& transform,
            OpenGL::Scoped::use_program& water_shader,
            Noggit::Rendering::LiquidTextureManager& texture_manager,
            int animtime);

private:
  int initGeometry(BlizzardArchive::ClientFile* f, std::string const& wmo_path, bool force_magma_uv);

  glm::vec3 pos;
  bool mTransparency;
  int xtiles, ytiles;
  int _liquid_id;
  std::string _debug_wmo_path;

  // INTERIOR WMO water: paint with the WMO material's baked MOMT.diffColor (client FUN_006b6420)
  // instead of the zone water light. Resolved in WMO.cpp from the group EXTERIOR/exterior-lit flags.
  bool _use_material_color = false;
  glm::vec3 _material_color = glm::vec3(0.0f);
  // City water CHANNELS (Stormwind canals/harbor, Booty Bay): MOGP indoor flag 0x2000 + exterior_lit
  // -- the ocean-dark opaque look. Open-air WMO pools (Northshire abbeygate stream) lack indoor and
  // blend with the river instead.
  bool _indoor_channel = false;
  // [2026-09-09] EXTERIOR water on the LiquidType.dbc path (MOHD 0x4 set, e.g. stock 3.3.5a
  // Stormwind: canal groups flagged EXTERIOR, groupLiquid 5 "Slow Water"). Those groups have neither
  // the indoor flag (the Turtle/vanilla canal encoding) nor a legacy id, so they fell into the
  // open-air-pool river blend and came out olive green. The client draws every exterior WMO water
  // with ONE flat zone colour, Light band 17 = river deep (FUN_006b6630); this mode uses exactly that.
  bool _dbc_exterior = false;

  std::vector<float> _vk_verts;
  std::vector<std::uint16_t> _vk_indices;
  std::vector<float> depths;
  std::vector<glm::vec2> tex_coords;
  std::vector<glm::vec3> vertices;
  std::vector<std::uint16_t> indices;

  // Per-tile probe data (heightAtLocal): rendered flag per tile (hidden bit 0x8 clear) + the FULL
  // (xtiles+1)*(ytiles+1) vertex height grid in group-local space. Filled in initGeometry.
  std::vector<std::uint8_t> _tile_rendered;
  std::vector<float> _grid_heights;
  // FLOATLIQ geometric clip (port of twmoa_toolkit wmo_liquid_geometric_clip.py, the fix that
  // hides Turtle's flat phantom sheets in the 3.3.5a data): 1 = tile hidden because the flat
  // sheet hovers over open air there (no group mesh vertex at/below water level in its cell).
  // Empty when the clip doesn't apply (non-flat pool / mostly-submerged pool / no mesh).
  std::vector<std::uint8_t> _clip_hidden;

  int _indices_count;

  bool _uploaded = false;

  OpenGL::Scoped::deferred_upload_buffers<4> _buffer;
  GLuint const& _indices_buffer = _buffer[0];
  GLuint const& _vertices_buffer = _buffer[1];
  GLuint const& _depth_buffer = _buffer[2];
  GLuint const& _tex_coord_buffer = _buffer[3];
  OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vertex_array;
  GLuint const& _vao = _vertex_array[0];
};
