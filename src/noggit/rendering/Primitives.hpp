// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <opengl/scoped.hpp>
#include <opengl/shader.hpp>
#include <math/trig.hpp>
#include <noggit/ContextObject.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

struct blp_texture;

namespace math
{
  struct vector_3d;
  struct vector_4d;
}

class World;
namespace Noggit::Rendering::Primitives
{
  class WireBox
  {
  public:
    WireBox() {}
    WireBox(const WireBox&);
    WireBox& operator=(WireBox& box ) { return *this; };

  public:
    static WireBox& getInstance(Noggit::NoggitRenderContext context)
    {
      static std::unordered_map<Noggit::NoggitRenderContext, WireBox> instances;

      if (instances.find(context) == instances.end())
      {
        WireBox instance;
        instances[context] = instance;
      }

      return instances.at(context);
    }

    void draw ( glm::mat4x4 const& model_view
              , glm::mat4x4 const& projection
              , glm::mat4x4 const& transform
              , glm::vec4 const& color
              , glm::vec3 const& min_point
              , glm::vec3 const& max_point
              );

    void unload();

  private:
    bool _buffers_are_setup = false;

    void setup_buffers();

    OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vao;
    OpenGL::Scoped::deferred_upload_buffers<1> _buffers;
    GLuint const& _indices = _buffers[0];
    std::unique_ptr<OpenGL::program> _program;
  };

  class Grid
  {
  public:
      void draw(glm::mat4x4 const& mvp
          , glm::vec3 const& pos
          , glm::vec4  const& color
          , float radius
      );
      void unload();
  private:
      bool _buffers_are_setup = false;

      void setup_buffers();

      int _indice_count = 0;

      OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vao;
      OpenGL::Scoped::deferred_upload_buffers<2> _buffers;
      GLuint const& _vertices_vbo = _buffers[0];
      GLuint const& _indices_vbo = _buffers[1];
      std::unique_ptr<OpenGL::program> _program;
  };

  class Sphere
  {
  public:
      void draw(glm::mat4x4 const& mvp
          , glm::vec3 const& pos
          , glm::vec4  const& color
          , float radius
          , int longitude = 32
          , int latitude = 18
          , float alpha = 1.f
          , bool wireframe = false
          , bool both = false
             );
    void unload();

  private:
    bool _buffers_are_setup = false;

    void setup_buffers(int longitude, int latitude);

    int _indice_count = 0;

    OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vao;
    OpenGL::Scoped::deferred_upload_buffers<2> _buffers;
    GLuint const& _vertices_vbo = _buffers[0];
    GLuint const& _indices_vbo = _buffers[1];
    std::unique_ptr<OpenGL::program> _program;
  };

  class Square
  {
  public:
    void draw(glm::mat4x4 const& mvp
             , glm::vec3 const& pos
             , float radius // radius of the biggest circle fitting inside the square drawn
             , math::radians inclination
             , math::radians orientation
             , glm::vec4  const& color
             );
    void unload();
  private:
    bool _buffers_are_setup = false;

    void setup_buffers();

    OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vao;
    OpenGL::Scoped::deferred_upload_buffers<2> _buffers;
    GLuint const& _vertices_vbo = _buffers[0];
    GLuint const& _indices_vbo = _buffers[1];
    std::unique_ptr<OpenGL::program> _program;
  };

  /*class Cylinder
  {
  public:
      void draw(glm::mat4x4 const& mvp, glm::vec3 const& pos, const glm::vec4 color, float radius, int precision, World* world, int height = 10);
      void unload();

  private:
      bool _buffers_are_setup = false;
      void setup_buffers(int precision, World* world, int height);
      int _indice_count = 0;

      OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vao;
      OpenGL::Scoped::deferred_upload_buffers<2> _buffers;
      GLuint const& _vertices_vbo = _buffers[0];
      GLuint const& _indices_vbo = _buffers[1];
      std::unique_ptr<OpenGL::program> _program;
  };*/

  class Line
  {
  public:
      void initSpline();
      void draw(glm::mat4x4 const& mvp, std::vector<glm::vec3> const points, glm::vec4 const& color, bool spline);
      void unload();

  private:
      bool _buffers_are_setup = false;
      void setup_buffers(std::vector<glm::vec3> const points);

      void setup_buffers_interpolated(std::vector<glm::vec3> const points);
      glm::vec3 interpolate(float t, glm::vec3 p0, glm::vec3 p1, glm::vec3 m0, glm::vec3 m1);

      int _indice_count = 0;

      void setup_shader(std::vector<glm::vec3> vertices, std::vector<std::uint16_t> indices);
      OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vao;
      OpenGL::Scoped::deferred_upload_buffers<2> _buffers;
      GLuint const& _vertices_vbo = _buffers[0];
      GLuint const& _indices_vbo = _buffers[1];
      std::unique_ptr<OpenGL::program> _program;
  };

  // A polyline painted onto the terrain / WMO surface as a screen-space projected decal, using the
  // same machinery as Circle::drawProjectedDecal. A plain 3D line between two waypoints cuts
  // straight through every hill and building between them; this instead reconstructs each covered
  // pixel's surface from the scene depth and paints the route onto it, so the whole path stays
  // visible on top of the mesh. Width is specified in world units but clamped to a pixel range, so
  // it thins with distance without ever disappearing or becoming a slab underfoot.
  class PathDecal
  {
  public:
    void draw(glm::mat4x4 const& mvp_rel          // camera-relative view-projection
             , glm::mat4x4 const& inv_mvp_rel     // its inverse, for the per-pixel reconstruction
             , glm::vec2 const& inv_viewport
             , GLuint scene_depth_tex               // everything drawn so far, for occlusion
             , GLuint world_depth_tex               // terrain + WMO only, the ground to paint on
             , std::vector<glm::vec3> const& points // world space, at least 2
             , glm::vec3 const& camera
             , glm::vec3 const& view_axis         // camera forward, world space
             , glm::vec4 const& color
             , float world_width
             , float min_pixels
             , float max_pixels
             , float px_scale                     // world units per pixel, per unit of view depth
             );

    // Filled disc decal at each point (degenerate zero-length segments -> the shader's round end
    // caps make dots). Used for the waypoint nodes on patrol routes.
    void draw_dots(glm::mat4x4 const& mvp_rel
                  , glm::mat4x4 const& inv_mvp_rel
                  , glm::vec2 const& inv_viewport
                  , GLuint scene_depth_tex
                  , GLuint world_depth_tex
                  , std::vector<glm::vec3> const& points // world space, at least 1
                  , glm::vec3 const& camera
                  , glm::vec3 const& view_axis
                  , glm::vec4 const& color
                  , float world_width               // dot DIAMETER, world units
                  , float min_pixels
                  , float max_pixels
                  , float px_scale
                  );

    void unload();

  private:
    bool _buffers_are_setup = false;
    void setup_buffers();

    std::vector<float> _segment_data; // scratch: (ax, ay, az, bx, by, bz) per instance

    OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vao;
    OpenGL::Scoped::deferred_upload_buffers<1> _buffers;
    GLuint const& _segments_vbo = _buffers[0];
    std::unique_ptr<OpenGL::program> _program;
  };

  // Precipitation pass — the visible half of the client's weather (MapWeather.cpp, RE'd in
  // docs/client_re/36): a camera-centered volume of falling rain streaks or snow flakes drawn with
  // the client's own textures (textures\Weather\RainDrop01.blp / SnowFlake01.blp; volume box
  // 44x44x25 around the camera per the client's weather init FUN_00674620/FUN_00677420). The
  // light/fog half (the zone light blending to its STORM param) lives in Sky::colorFor.
  class WeatherEffect
  {
  public:
    WeatherEffect();
    ~WeatherEffect(); // out-of-line: unique_ptr<blp_texture> members with forward-declared type

    void draw(glm::mat4x4 const& mvp
             , glm::vec3 const& camera_pos
             , int type                    // 0 none, 1 rain, 2 snow, 3 underwater particulates
             , float intensity             // 0..1
             , float animtime_ms
             , glm::vec3 const& light_color
             , Noggit::NoggitRenderContext context
             , int liquid_family = 0       // motes only: 0/1 water/ocean, 2 magma, 3 slime -> the
                                           // client's per-family sheet-cell row (DAT_0086a0a0)
             );

    void unload();

  private:
    struct Drop
    {
      glm::vec3 pos;
      float speed;
      float seed;
      float size = 0.05f; // motes: per-particle half-size (client rand[0.5,1.5] x 1/36)
      int frame = 0;      // motes: sprite cell 0-7 in the 4x4 WaterPoop02 sheet
    };

    void setup(Noggit::NoggitRenderContext context);

    std::vector<Drop> _drops;
    std::vector<float> _vertex_data; // scratch: 6 verts x (pos3 + uv2) per drop
    int _active_type = 0;
    float _last_time = -1.0f;
    // Motes: ONE shared slow current per emitter (client FUN_0068e1c0/FUN_0068e930) -- no
    // per-particle velocity; visible motion is mostly camera parallax.
    glm::vec3 _mote_dir = glm::vec3(0.0f, 1.0f, 0.0f);
    float _mote_speed = 0.02f; // legacy, unused since the exact oscillation law below
    // Client mote drift epoch (FUN_0068e1c0/e4f0, constants dumped 2026-08-26): shared direction
    // oscillation sin(2pi x phase x freq) x amp, re-rolled every half period.
    float _mote_freq = 0.0125f;
    float _mote_amp = 0.005f;
    float _mote_phase = 0.0f;
    // Current liquid family of the mote pool (client obj+0xfa24 = type & 3). A family change
    // re-seeds the pool like the client's FUN_0068e720.
    int _mote_family = 0;

    bool _buffers_are_setup = false;
    OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vao;
    OpenGL::Scoped::deferred_upload_buffers<1> _buffers;
    GLuint const& _vbo = _buffers[0];
    std::unique_ptr<OpenGL::program> _program;
    std::unique_ptr<blp_texture> _rain_texture;
    std::unique_ptr<blp_texture> _snow_texture;
    std::unique_ptr<blp_texture> _particulate_texture; // Textures\WaterPoop02.blp (underwater motes)
    bool _texture_failed = false;
  };

  // Client 1.12 Water0Ripple port (RE doc 36, FUN_0068f8b0 chain): flat additive quads on the
  // liquid surface -- wake rings while swimming (XTextures\splash\wake.blp), a splash burst on
  // water entry (splash.blp). Client laws: linear size growth over the lifetime, alpha
  // ATTACK/DECAY peaking at 40% of life (measured, both kinds), per-entry rotation, white x
  // alpha. (The client also clips each quad to the liquid surface triangles; this draws plain
  // surface-height quads -- documented simplification.)
  class WaterRipples
  {
  public:
    WaterRipples();
    ~WaterRipples(); // out-of-line: unique_ptr<blp_texture> members with forward-declared type

    // kind: 0 = wake ring, 1 = splash burst
    void spawn(glm::vec3 const& surface_pos, float rotation_rad, float size0, float growth,
               float lifetime_s, float alpha_peak, int kind);

    void draw(glm::mat4x4 const& mvp, float animtime_ms, Noggit::NoggitRenderContext context);
    void unload();

  private:
    struct RippleEntry
    {
      glm::vec3 pos;
      float rot;
      float size0, growth;
      float birth_ms, lifetime_ms;
      float alpha_peak;
      int kind;
    };

    void setup(Noggit::NoggitRenderContext context);

    std::vector<RippleEntry> _entries;
    std::vector<float> _vertex_data;
    bool _buffers_are_setup = false;
    OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vao;
    OpenGL::Scoped::deferred_upload_buffers<1> _buffers;
    GLuint const& _vbo = _buffers[0];
    std::unique_ptr<OpenGL::program> _program;
    std::unique_ptr<blp_texture> _wake_texture;
    std::unique_ptr<blp_texture> _splash_texture;
    bool _texture_failed = false;
  };

  // Flat thick ring drawn in the XZ plane.
  // Uses the same square_vs / square_fs shaders as Square.
  class Circle
  {
  public:
    Circle();
    ~Circle(); // out-of-line: unique_ptr<blp_texture> member with forward-declared type

    void draw(glm::mat4x4 const& mvp
             , glm::vec3 const& pos
             , glm::vec4 const& color
             , float radius
             );

    // SCREEN-SPACE PROJECTED DECAL path (matches the blob shadow): paints the UnitSelectTexture ring
    // onto the actual terrain/WMO ground under the unit by reconstructing world position from a
    // pre-captured scene-depth texture -- wraps the existing mesh instead of a draped disc.
    void drawProjectedDecal(glm::mat4x4 const& mvp
                           , glm::mat4x4 const& inv_view_projection
                           , glm::vec2 const& inv_viewport
                           , GLuint scene_depth_tex
                           , GLuint world_depth_tex // terrain + WMO only; models must occlude the ring
                           , GLuint empty_vao
                           , glm::vec3 const& center
                           , glm::vec3 const& camera
                           , float radius
                           , glm::vec4 const& color
                           , float uv_rotation
                           // simple_ring = plain procedural outline instead of the client's
                           // UnitSelectTexture. Used by the spawn-tool aim cursor, which is a UI
                           // affordance and not a client selection circle.
                           , bool simple_ring = false
                           );

    void unload();

  private:
    static constexpr int N_SEGMENTS = 64;
    bool _buffers_are_setup = false;
    void setup_buffers();

    int _indice_count = 0;

    OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vao;
    OpenGL::Scoped::deferred_upload_buffers<2> _buffers;
    GLuint const& _vertices_vbo = _buffers[0];
    GLuint const& _indices_vbo = _buffers[1];
    std::unique_ptr<OpenGL::program> _program;
    std::unique_ptr<OpenGL::program> _decal_program; // projected-decal path

    // The client's selection-circle texture (Textures\UnitSelectTexture.blp), loaded lazily.
    std::unique_ptr<blp_texture> _select_texture;
    bool _select_texture_failed = false;
  };

}
