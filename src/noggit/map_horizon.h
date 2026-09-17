// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/frustum.hpp>

#include <noggit/tool_enums.hpp>

#include <opengl/texture.hpp>
#include <opengl/scoped.hpp>
#include <opengl/shader.fwd.hpp>

#include <QtGui/QImage>

#include <cstdint>
#include <memory>
#include <vector>

class MapIndex;
class MapTile;
class MapView;
class World;

namespace Noggit
{

struct map_horizon_tile
{
    int16_t height_17[17][17];
    int16_t height_16[16][16];
    int16_t holes[16];
};

struct map_horizon_batch
{
  map_horizon_batch ()
    : vertex_start (0)
    , vertex_count (0)
    , min_height (0)
    , max_height (0)
  {}

  map_horizon_batch (uint32_t _vertex_start, uint32_t _vertex_count, int16_t _min_height, int16_t _max_height)
    : vertex_start(_vertex_start)
    , vertex_count(_vertex_count)
    , min_height(_min_height)
    , max_height(_max_height)
  {}

  uint32_t vertex_start;
  uint32_t vertex_count;
  // MARE height extent = the tile AABB the client frustum-tests (CMapLowDetail tile +0x4..+0x18)
  int16_t min_height;
  int16_t max_height;
};

class map_horizon
{
public:
  struct render
  {
    render(const map_horizon& horizon);

    // [2026-09-08 WDL HORIZON, reverse-engineered from the 3.3.5a client's MapLowDetail.cpp]
    // Per-frame tile selection + index build, shared by the GL draw and the Vulkan feed.
    // The client renders the low-res mesh with ITS OWN PROJECTION (FUN_00791170): same fov/aspect,
    // near = camera far clip - 50 yd, far = farclip * horizonFarclipScale (CVar default 4.0). The
    // near plane at the terrain's far clip is what keeps the coarse mesh off the loaded terrain;
    // the far depth slice (horizon_vert.glsl) makes it lose against anything real in the 50 yd
    // overlap. Selector (FUN_007cc0b0): every WDL tile within round(4 * farclip * 0.03) chunks of
    // the camera, widened by 2 tiles per side, whose AABB passes the 6-plane test against THAT
    // frustum. Cells whose MAHO bit is set (fully holed chunk) land in hole_indices(); the client
    // draws them as a second range with the depth-write state flipped (RenderTile FUN_007d5e70).
    // terrain_reach = how far real terrain is actually drawn (the near plane sits 50 yd inside it);
    // farclip = the view distance (far plane = 4 x farclip, selector radius = 4 x farclip). The client
    // uses its camera far clip for both (FUN_00791170: near = far clip - 50) and clears the frame with
    // the same fog colour the mesh paints, so nothing beyond its terrain ever shows as a void; noggit
    // only keeps the "ADT loading radius" grid, so its terrain can end well before the view distance.
    // farclip_scale = the client's horizonFarclipScale CVar (stock 4.0, capped 6.0).
    void build_frame(float terrain_reach, float farclip, float farclip_scale,
                     glm::mat4x4 const& projection, glm::mat4x4 const& model_view,
                     glm::vec3 const& camera);
    // GL draw of what build_frame selected (state = client CMap::RenderLowDetail FUN_00795f80).
    void draw_gl(glm::mat4x4 const& model_view, glm::vec3 const& color);
    // the low-detail projection built by build_frame, and projection * model_view for the VK push
    glm::mat4x4 const& ld_projection() const { return _ld_projection; }
    glm::mat4x4 const& ld_mvp() const { return _ld_mvp; }
    // build_frame + draw_gl (old signature; index/display are unused, the client ignores them too)
    void draw(glm::mat4x4 const& model_view
             , glm::mat4x4 const& projection
             , MapIndex *index
             , const glm::vec3& color
             , const float& cull_distance
             , const math::frustum& frustum
             , const glm::vec3& camera
             , display_mode display
             );

    // for the Vulkan feed: the whole map's WDL mesh (static after the ctor) + this frame's ranges
    std::vector<glm::vec3> const& vertices() const { return _vertices; }
    std::uint32_t vertex_generation() const { return _vertex_generation; }
    std::vector<std::uint32_t> const& solid_indices() const { return _solid_indices; }
    std::vector<std::uint32_t> const& hole_indices() const { return _hole_indices; }
    int tiles_selected() const { return _tiles_selected; }

    map_horizon_batch _batches[64][64];

    OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vaos;
    GLuint const& _vao = _vaos[0];
    OpenGL::Scoped::buffers<2> _buffers;
    GLuint const& _index_buffer = _buffers[0];
    GLuint const& _vertex_buffer = _buffers[1];
    std::unique_ptr<OpenGL::program> _map_horizon_program;

  private:
    const map_horizon* _horizon;               // MAHO lookup for the hole split
    std::vector<glm::vec3> _vertices;          // CPU copy of the vertex buffer (Vulkan upload)
    std::uint32_t _vertex_generation = 1;      // bumps whenever _vertices change
    std::vector<std::uint32_t> _solid_indices; // this frame: cells with the MAHO bit clear
    std::vector<std::uint32_t> _hole_indices;  // this frame: cells with the MAHO bit set
    std::vector<std::uint32_t> _gl_indices;    // solid + holes, one GL upload
    int _tiles_selected = 0;
    glm::mat4x4 _ld_projection = glm::mat4x4(1.f);
    glm::mat4x4 _ld_mvp = glm::mat4x4(1.f);
  };

  class minimap : public OpenGL::texture
  {
  public:
    minimap(const map_horizon& horizon);
  };

  map_horizon(const std::string& basename, const MapIndex * const index);

  void update_minimap_tile(int y, int x, bool has_data);

  void set_minimap(const MapIndex* const index);

  void remove_horizon_tile(int y, int x);

  Noggit::map_horizon_tile* get_horizon_tile(int y, int x);

  QImage _qt_minimap;

  void update_horizon_tile(MapTile* mTile);

  void save_wdl(World* world, bool regenerate = false);

private:
  int16_t getWdlheight(MapTile* tile, float x, float y);

  std::string _filename;

  std::vector<std::string> mWMOFilenames;
  // std::vector<ENTRY_MODF> lWMOInstances;

  std::unique_ptr<map_horizon_tile> _tiles[64][64];
};

}
