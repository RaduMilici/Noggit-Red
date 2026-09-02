// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/rendering/vulkan/VkParticleFeed.hpp>
#include <noggit/rendering/Primitives.hpp>

#include <math/bounding_box.hpp>
#include <noggit/Misc.h>
#include <noggit/TextureManager.h>
#include <opengl/scoped.hpp>
#include <opengl/context.hpp>
#include <opengl/types.hpp>
#include <noggit/World.h>

#include <numbers>
#include <array>
#include <vector>
#include <random>
#include <cmath>
#include <algorithm>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace Noggit::Rendering::Primitives;

void WireBox::draw ( glm::mat4x4 const& model_view
                    , glm::mat4x4 const& projection
                    , glm::mat4x4 const& transform
                    , glm::vec4  const& color
                    , glm::vec3 const& min_point
                    , glm::vec3 const& max_point
                    )
{

  if (!_buffers_are_setup)
  {
    setup_buffers();
  }

  OpenGL::Scoped::use_program wire_box_shader {*_program.get()};

  auto points = math::box_points(min_point, max_point);

  auto glmPoints = std::vector<glm::vec3>();

  for(auto const point : points)
    {
        glmPoints.push_back(glm::vec3(point.x, point.y, point.z));
    }

  wire_box_shader.uniform("model_view", model_view);
  wire_box_shader.uniform("projection", projection);
  wire_box_shader.uniform("transform", transform);
  wire_box_shader.uniform("color", color);
  wire_box_shader.uniform("pointPositions", glmPoints);

  OpenGL::Scoped::bool_setter<GL_LINE_SMOOTH, GL_TRUE> const line_smooth;
  gl.hint(GL_LINE_SMOOTH_HINT, GL_NICEST);

  OpenGL::Scoped::vao_binder const _(_vao[0]);

  gl.drawElements (GL_LINE_STRIP, _indices, 16, GL_UNSIGNED_BYTE, nullptr);
}

void WireBox::setup_buffers()
{
  _program.reset(new OpenGL::program( {{ GL_VERTEX_SHADER, OpenGL::shader::src_from_qrc("wire_box_vs") }
                 , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("wire_box_fs")}}));

  _vao.upload();
  _buffers.upload();

  //std::vector<glm::vec3> positions (math::box_points (min_point, max_point));

  static std::array<std::uint8_t, 16> const indices
      {{5, 7, 3, 2, 0, 1, 3, 1, 5, 4, 0, 4, 6, 2, 6, 7}};

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> const index_buffer (_indices);
  gl.bufferData ( GL_ELEMENT_ARRAY_BUFFER
      , indices.size() * sizeof (*indices.data())
      , indices.data()
      , GL_STATIC_DRAW
  );

  OpenGL::Scoped::use_program shader (*_program.get());

  OpenGL::Scoped::vao_binder const _ (_vao[0]);

  _buffers_are_setup = true;

}

  void WireBox::unload()
  {
    _vao.unload();
    _buffers.unload();
    _program.reset();

    _buffers_are_setup = false;
  }

  void Grid::draw(glm::mat4x4 const& mvp
      , glm::vec3 const& pos
      , glm::vec4  const& color
      , float radius
  )
  {
    if (!_buffers_are_setup)
    {
      setup_buffers();
    }

    OpenGL::Scoped::use_program sphere_shader {*_program.get()};

    sphere_shader.uniform("model_view_projection", mvp);
    sphere_shader.uniform("origin", glm::vec3(pos.x,pos.y,pos.z));
    sphere_shader.uniform("color", color);
    sphere_shader.uniform("radius", radius);

    OpenGL::Scoped::vao_binder const _(_vao[0]);
    gl.drawElements(GL_LINES, _indices_vbo, _indice_count, GL_UNSIGNED_SHORT, nullptr);
  }


  void Grid::setup_buffers()
  {
    _vao.upload();
    _buffers.upload();

    _program.reset(new OpenGL::program({{ GL_VERTEX_SHADER
                                            , OpenGL::shader::src_from_qrc("grid_vs")
                                        }
                                           , { GL_FRAGMENT_SHADER
                                            , OpenGL::shader::src_from_qrc("grid_fs")
                                        }
                                       }));


    std::vector<glm::vec3> vertices;
    std::vector<std::uint16_t> indices;

    int slices = 20;

    for(int j = 0; j <= slices; ++j)
    {
      for(int i = 0; i <= slices; ++i)
      {
        float x = static_cast<float>(i) / static_cast<float>(slices);
        float y = 0;
        float z = static_cast<float>(j) / static_cast<float>(slices);
        vertices.push_back(glm::vec3(x, y, z));
      }
    }

    for(int j = 0; j < slices; ++j)
    {
      for(int i = 0; i < slices; ++i)
      {

        int row1 =  j * (slices + 1);
        int row2 = (j + 1) * (slices + 1);

        indices.push_back(row1 + i);
        indices.push_back(row1 + i + 1);
        indices.push_back(row1 + i + 1);
        indices.push_back(row2 + i + 1);

        indices.push_back(row2 + i + 1);
        indices.push_back(row2 + i);
        indices.push_back(row2 + i);
        indices.push_back(row1 + i);

      }
    }

    _indice_count = static_cast<int>(indices.size());

    gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>
        (_vertices_vbo, vertices, GL_STATIC_DRAW);
    gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>
        (_indices_vbo, indices, GL_STATIC_DRAW);


    OpenGL::Scoped::index_buffer_manual_binder indices_binder(_indices_vbo);

    OpenGL::Scoped::use_program shader (*_program.get());

    {
      OpenGL::Scoped::vao_binder const _ (_vao[0]);

      OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vertices_binder (_vertices_vbo);
      shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);

      indices_binder.bind();
    }

    _buffers_are_setup = true;
  }

  void Grid::unload()
  {
    _vao.unload();
    _buffers.unload();
    _program.reset();

    _buffers_are_setup = false;
  }


  void Sphere::draw(glm::mat4x4 const& mvp, glm::vec3 const& pos, glm::vec4  const& color
      , float radius, int longitude, int latitude, float alpha, bool wireframe, bool drawBoth)
{
  if (!_buffers_are_setup)
  {
    setup_buffers(longitude, latitude);
  }

  OpenGL::Scoped::use_program sphere_shader {*_program.get()};

  sphere_shader.uniform("model_view_projection", mvp);
  sphere_shader.uniform("origin", glm::vec3(pos.x,pos.y,pos.z));
  sphere_shader.uniform("radius", radius);
  sphere_shader.uniform("color", glm::vec4(color.r, color.g, color.b, alpha));

  OpenGL::Scoped::vao_binder const _(_vao[0]);
  if (drawBoth)
  {
      gl.drawElements(GL_TRIANGLES, _indices_vbo, _indice_count, GL_UNSIGNED_SHORT, nullptr);
      sphere_shader.uniform("color", glm::vec4(1.f, 1.f, 1.f, 1.f));
      gl.drawElements(GL_LINE_STRIP, _indices_vbo, _indice_count, GL_UNSIGNED_SHORT, nullptr);
      return;
  }

  if (!wireframe)
  {
        gl.drawElements(GL_TRIANGLES, _indices_vbo, _indice_count, GL_UNSIGNED_SHORT, nullptr);
  }
  else
  {
        gl.drawElements(GL_LINE_STRIP, _indices_vbo, _indice_count, GL_UNSIGNED_SHORT, nullptr);
  }
}


void Sphere::setup_buffers(int longitude, int latitude)
{
  _vao.upload();
  _buffers.upload();

  const int na = longitude;
  const int nb = latitude;
  const int na3 = na * 3;
  const int nn = nb * na3;

  std::vector<glm::vec3> vertices;
  std::vector<std::uint16_t> indices;

  _program.reset(new OpenGL::program({{ GL_VERTEX_SHADER
               , OpenGL::shader::src_from_qrc("sphere_vs")}
             , { GL_FRAGMENT_SHADER
               , OpenGL::shader::src_from_qrc("sphere_fs")
               }}));

  float x, y, z, a, b, da, db, r = 3.5f;
  int ia, ib, ix, iy;
  da = glm::two_pi<float>() / float(na);
  db = glm::pi<float>() / float(nb - 1);

  for (ix = 0, b = -glm::half_pi<float>(), ib = 0; ib < nb; ib++, b += db)
  {
      for (a = 0.f, ia = 0; ia < na; ia++, a += da, ix += 3)
      {
          x = cos(b) * cos(a);
          z = cos(b) * sin(a);
          y = sin(b);

          vertices.emplace_back(x, y, z);
      }
  }

  for (ix = 0, iy = 0, ib = 1; ib < nb; ib++)
  {
      for (ia = 1; ia < na; ia++, iy++)
      {
          indices.push_back(iy); ix++;
          indices.push_back(iy + 1); ix++;
          indices.push_back(iy + na); ix++;

          indices.push_back(iy + na); ix++;
          indices.push_back(iy + 1); ix++;
          indices.push_back(iy + na + 1); ix++;
      }

      indices.push_back(iy); ix++;
      indices.push_back(iy + 1 - na); ix++;
      indices.push_back(iy + na); ix++;

      indices.push_back(iy + na); ix++;
      indices.push_back(iy - na + 1); ix++;
      indices.push_back(iy + 1); ix++;
      iy++;
  }

  _indice_count = (int)indices.size();

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>
    (_vertices_vbo, vertices, GL_STATIC_DRAW);
  gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>
    (_indices_vbo, indices, GL_STATIC_DRAW);


  OpenGL::Scoped::index_buffer_manual_binder indices_binder(_indices_vbo);

  OpenGL::Scoped::use_program shader (*_program.get());

  {
    OpenGL::Scoped::vao_binder const _ (_vao[0]);

    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vertices_binder (_vertices_vbo);
    shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);

    indices_binder.bind();
  }

  _buffers_are_setup = true;
}

  void Sphere::unload()
  {
    _vao.unload();
    _buffers.unload();
    _program.reset();

    _buffers_are_setup = false;
  }

  void Square::draw(glm::mat4x4 const& mvp
                 , glm::vec3 const& pos
                 , float radius
                 , math::radians inclination
                 , math::radians orientation
                 , glm::vec4  const& color
                 )
{
  if (!_buffers_are_setup)
  {
    setup_buffers();
  }

  OpenGL::Scoped::use_program sphere_shader {*_program.get()};

  sphere_shader.uniform("model_view_projection", mvp);
  sphere_shader.uniform("origin", glm::vec3(pos.x,pos.y,pos.z));
  sphere_shader.uniform("radius", radius);
  sphere_shader.uniform("inclination", inclination._);
  sphere_shader.uniform("orientation", orientation._);
  sphere_shader.uniform("color", color);

  OpenGL::Scoped::vao_binder const _ (_vao[0]);
  gl.drawElements(GL_TRIANGLES, _indices_vbo, 6, GL_UNSIGNED_SHORT, nullptr);
}


void Square::setup_buffers()
{
  _vao.upload();
  _buffers.upload();

  std::vector<glm::vec3> vertices =
  {
     {-1.f, 0.f, -1.f}
    ,{-1.f, 0.f,  1.f}
    ,{ 1.f, 0.f,  1.f}
    ,{ 1.f, 0.f, -1.f}
  };
  std::vector<std::uint16_t> indices = {0,1,2, 2,3,0};

  _program.reset(new OpenGL::program({{ GL_VERTEX_SHADER
               , OpenGL::shader::src_from_qrc("square_vs")
               }, { GL_FRAGMENT_SHADER
               , OpenGL::shader::src_from_qrc("square_fs")
               }}));


  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>
    (_vertices_vbo, vertices, GL_STATIC_DRAW);
  gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>
    (_indices_vbo, indices, GL_STATIC_DRAW);


  OpenGL::Scoped::index_buffer_manual_binder indices_binder (_indices_vbo);

  OpenGL::Scoped::use_program shader(*_program.get());

  {
    OpenGL::Scoped::vao_binder const _ (_vao[0]);

    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vertices_binder (_vertices_vbo);
    shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);

    indices_binder.bind();
  }

  _buffers_are_setup = true;
}

  void Square::unload()
  {
    _vao.unload();
    _buffers.unload();
    _program.reset();

    _buffers_are_setup = false;

  }

  WeatherEffect::WeatherEffect() = default;
  WeatherEffect::~WeatherEffect() = default;

  void WeatherEffect::setup(Noggit::NoggitRenderContext context)
  {
    _vao.upload();
    _buffers.upload();

    // Inline shader: textured, colour-tinted, alpha-blended quads. Precipitation is small and
    // per-frame dynamic; no qrc round-trip needed.
    static char const* vs =
      "#version 410 core\n"
      "in vec3 pos;\n"
      "in vec2 uv;\n"
      "uniform mat4 mvp;\n"
      "out vec2 f_uv;\n"
      "void main() { f_uv = uv; gl_Position = mvp * vec4(pos, 1.0); }\n";
    static char const* fs =
      "#version 410 core\n"
      "uniform sampler2DArray tex;\n"
      "uniform float tex_index;\n"
      "uniform vec4 color;\n"
      "in vec2 f_uv;\n"
      "out vec4 out_color;\n"
      "void main()\n"
      "{\n"
      "  vec4 t = texture(tex, vec3(f_uv, tex_index));\n"
      "  out_color = vec4(t.rgb * color.rgb, t.a * color.a);\n"
      "  if (out_color.a < 0.01) discard;\n"
      "}\n";
    _program.reset(new OpenGL::program(
      {{ GL_VERTEX_SHADER, std::string(vs) }
      ,{ GL_FRAGMENT_SHADER, std::string(fs) }}));

    OpenGL::Scoped::use_program sp (*_program.get());
    {
      OpenGL::Scoped::vao_binder const _ (_vao[0]);
      OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vb (_vbo);
      GLsizei const stride = static_cast<GLsizei>(sizeof(float) * 5);
      sp.attrib("pos", 3, GL_FLOAT, GL_FALSE, stride, nullptr);
      sp.attrib("uv", 2, GL_FLOAT, GL_FALSE, stride,
                reinterpret_cast<GLvoid const*>(sizeof(float) * 3));
    }

    if (!_texture_failed)
    {
      try
      {
        _rain_texture = std::make_unique<blp_texture>(
          BlizzardArchive::Listfile::FileKey("textures\\Weather\\RainDrop01.blp"), context);
        _rain_texture->finishLoading();
        _rain_texture->upload();
        _snow_texture = std::make_unique<blp_texture>(
          BlizzardArchive::Listfile::FileKey("textures\\Weather\\SnowFlake01.blp"), context);
        _snow_texture->finishLoading();
        _snow_texture->upload();
        // Underwater particulate motes -- the client's waterParticulates system (MapWeather RE,
        // docs/client_re/36: FUN_0066f6c0 loads exactly this texture).
        _particulate_texture = std::make_unique<blp_texture>(
          BlizzardArchive::Listfile::FileKey("Textures\\WaterPoop02.blp"), context);
        _particulate_texture->finishLoading();
        _particulate_texture->upload();
      }
      catch (std::exception const&)
      {
        _rain_texture.reset();
        _snow_texture.reset();
        _texture_failed = true;
      }
    }

    _buffers_are_setup = true;
  }

  void WeatherEffect::draw(glm::mat4x4 const& mvp
                          , glm::vec3 const& camera_pos
                          , int type
                          , float intensity
                          , float animtime_ms
                          , glm::vec3 const& light_color
                          , Noggit::NoggitRenderContext context
                          , int liquid_family
                          )
  {
    if (type == 0 || intensity <= 0.0f)
    {
      _active_type = 0;
      _drops.clear();
      _last_time = -1.0f;
      return;
    }

    if (!_buffers_are_setup)
    {
      setup(context);
    }

    bool const motes = (type == 3);
    blp_texture* tex = motes ? _particulate_texture.get()
                     : (type == 2) ? _snow_texture.get() : _rain_texture.get();
    if (!tex || !tex->is_uploaded())
    {
      return;
    }

    // Client weather volume (MapWeather init): a 44 x 44 horizontal, +-25 vertical box around the
    // camera. Density LAW extracted from the client (FUN_006749e0, docs/client_re/36): spawn rate =
    // weatherDensity(0.66 default) x K x intensity with K = 6500 rain / 35000 snow, pool cap 6144.
    // Steady-state visible count = rate x fall time through the volume: rain ~3400 x intensity;
    // snow saturates toward the cap. (Fall speeds are still editor approximations.)
    // Underwater particulates: the client emitter EXACTLY (FUN_0068e5a0 chain, RE doc 36):
    // ctor(density=1.0, size=1/36, range=30, WaterPoop02): count = ftol(density x 4000), placement
    // +-range/2 = +-15yd cube around the camera, per-particle half-size rand[0.5,1.5] x 1/36,
    // sprite frame = index & 7 (cells 0-7 of the 4x4 sheet = the soft blobs), ONE shared slow
    // current (up-biased), white untinted color. Rain/snow keep the 44x44x25 weather box.
    float const kBoxH = motes ? 15.0f : 44.0f;
    float const kBoxV = motes ? 15.0f : 25.0f;
    bool const snow = (type == 2);
    int const target = motes ? static_cast<int>(4000 * std::min(1.0f, intensity))
                     : std::min(6144, static_cast<int>((snow ? 5000 : 3400) * intensity));

    float dt = (_last_time >= 0.0f) ? (animtime_ms - _last_time) / 1000.0f : 0.016f;
    _last_time = animtime_ms;
    dt = std::clamp(dt, 0.0f, 0.1f);

    static std::mt19937 rng(20260823u);
    auto frand = [&](float a, float b) {
      return a + (b - a) * (static_cast<float>(rng()) / static_cast<float>(rng.max()));
    };

    // CLIENT mote-drift epoch roll (FUN_0068e1c0, constants dumped 2026-08-26): a fully RANDOM
    // 3D direction (random azimuth; vertical component x0.25 then ABS = always some upward bias),
    // normalized; freq = rand[1,2] x 0.0125 Hz; amp = rand[1,2] x 0.005 (per client frame);
    // phase reset. Called on mote activation AND at every half-period boundary (see the drift
    // computation below) -- the client re-picks the direction every ~20-40 s, with sin() at zero
    // exactly at the boundary so each epoch is one smooth glide, never a snap.
    auto roll_mote_epoch = [&]()
    {
      float const ra = frand(-3.1415927f, 3.1415927f);
      float const rb = frand(-3.1415927f, 3.1415927f);
      glm::vec3 dir(std::cos(rb) * std::sin(ra),
                    std::abs(std::cos(ra)) * 0.25f,
                    std::sin(rb) * std::sin(ra));
      float const len = glm::length(dir);
      _mote_dir = (len > 1e-6f) ? dir / len : glm::vec3(0.0f, 1.0f, 0.0f);
      _mote_freq = frand(1.0f, 2.0f) * 0.0125f;
      _mote_amp = frand(1.0f, 2.0f) * 0.005f;
      _mote_phase = 0.0f;
    };

    // CLIENT (FUN_006809c0 -> FUN_0068e720): a liquid-TYPE change re-seeds every particle and
    // stores the new family -- mirrored by clearing the pool so the refill below re-frames with
    // the new family's sheet cells (doc 37: water/ocean row {0..7}, magma {9..12}x2, slime {0}x8).
    if (motes && liquid_family != _mote_family)
    {
      _mote_family = liquid_family;
      _drops.clear();
    }
    if (_active_type != type)
    {
      _active_type = type;
      _drops.clear();
      if (motes)
      {
        roll_mote_epoch();
      }
    }
    while (static_cast<int>(_drops.size()) < target)
    {
      Drop d;
      d.pos = glm::vec3(camera_pos.x + frand(-kBoxH, kBoxH),
                        camera_pos.y + frand(-kBoxV, kBoxV),
                        camera_pos.z + frand(-kBoxH, kBoxH));
      // rain/snow fall; motes carry no per-particle velocity (shared current instead)
      d.speed = motes ? 0.0f
              : snow ? frand(4.0f, 7.0f) : frand(50.0f, 70.0f);
      d.seed = frand(0.0f, 6.2831853f);
      // Client size = FULL quad width (billboard corner table FUN_0068eb30 is +-0.5): half = /2.
      d.size = motes ? frand(0.5f, 1.5f) * (1.0f / 72.0f) : 0.0f;
      // Client frame law (FUN_0068efe0): cell = DAT_0086a0a0[(index & 7) + family * 8].
      // Rows: water {0..7}, ocean {0..7}, magma {9,10,11,12,9,10,11,12}, slime {0,0,0,0,0,0,0,0}.
      static constexpr int kFamilyFrames[4][8] = {
        {0, 1, 2, 3, 4, 5, 6, 7},
        {0, 1, 2, 3, 4, 5, 6, 7},
        {9, 10, 11, 12, 9, 10, 11, 12},
        {0, 0, 0, 0, 0, 0, 0, 0},
      };
      int const fam = motes ? (_mote_family & 3) : 0;
      d.frame = kFamilyFrames[fam][static_cast<int>(_drops.size()) & 7];
      _drops.push_back(d);
    }
    if (static_cast<int>(_drops.size()) > target)
    {
      _drops.resize(target);
    }

    // CLIENT drift (FUN_0068e4f0, exact -- 2026-08-26, user: "in game they move in a sliding
    // animation... ours don't"): the OLD port added amp as yd/SECOND, but the client adds the
    // sine value PER FRAME -> ~30x faster than we ran (peak ~0.3-0.6 yd/s at 60 fps): the visible
    // slide. dt*60 normalises the per-frame add to the 60 fps feel (the client itself is
    // frame-rate dependent here -- documented derivation). MAGMA family: constant SINK at
    // -0.02 yd/s (dumped, dt-scaled in the client -- heavy ash, not rising embers). SLIME: still.
    glm::vec3 mote_drift(0.0f);
    if (motes)
    {
      if (_mote_family <= 1)
      {
        _mote_phase += dt;
        if (_mote_phase * _mote_freq > 0.5f)
        {
          roll_mote_epoch(); // new random direction/freq/amp at the sine zero -- seamless
        }
        mote_drift = _mote_dir
                   * (std::sin(6.2831853f * _mote_phase * _mote_freq) * _mote_amp * dt * 60.0f);
      }
      else if (_mote_family == 2)
      {
        mote_drift = glm::vec3(0.0f, -0.02f * dt, 0.0f);
      }
    }
    float const t_s = animtime_ms / 1000.0f;
    for (Drop& d : _drops)
    {
      if (motes)
      {
        d.pos += mote_drift; // ONE shared current for the whole pool (client FUN_0068e930)
      }
      else
      {
        d.pos.y -= d.speed * dt;
        if (snow)
        {
          float const sway = 1.2f;
          d.pos.x += std::sin(t_s * 0.9f + d.seed) * sway * dt;
          d.pos.z += std::cos(t_s * 0.7f + d.seed) * sway * 0.85f * dt;
        }
      }
      // Keep the volume centered on the (moving) camera: wrap on every axis. Motes wrap plainly on
      // Y too (client cube wrap); falling rain/snow respawns X/Z on a Y wrap so streaks vary.
      if (motes)
      {
        if (d.pos.y < camera_pos.y - kBoxV) d.pos.y += 2.0f * kBoxV;
        if (d.pos.y > camera_pos.y + kBoxV) d.pos.y -= 2.0f * kBoxV;
      }
      else
      {
        if (d.pos.y < camera_pos.y - kBoxV)
        {
          d.pos.y += 2.0f * kBoxV;
          d.pos.x = camera_pos.x + frand(-kBoxH, kBoxH);
          d.pos.z = camera_pos.z + frand(-kBoxH, kBoxH);
        }
        if (d.pos.y > camera_pos.y + kBoxV)
        {
          d.pos.y -= 2.0f * kBoxV;
          d.pos.x = camera_pos.x + frand(-kBoxH, kBoxH);
          d.pos.z = camera_pos.z + frand(-kBoxH, kBoxH);
        }
      }
      if (d.pos.x < camera_pos.x - kBoxH) d.pos.x += 2.0f * kBoxH;
      if (d.pos.x > camera_pos.x + kBoxH) d.pos.x -= 2.0f * kBoxH;
      if (d.pos.z < camera_pos.z - kBoxH) d.pos.z += 2.0f * kBoxH;
      if (d.pos.z > camera_pos.z + kBoxH) d.pos.z -= 2.0f * kBoxH;
    }

    // Build camera-facing quads: rain = tall thin streak, snow = small flake, motes = tiny specks
    // (per-particle size + one 64px cell of the 4x4 WaterPoop02 sheet each).
    _vertex_data.clear();
    _vertex_data.reserve(_drops.size() * 6 * 5);
    for (Drop const& d : _drops)
    {
      float const half_w = motes ? d.size : snow ? 0.09f : 0.03f;
      float const half_h = motes ? d.size : snow ? 0.09f : 0.9f;
      glm::vec3 const to_cam = camera_pos - d.pos;
      glm::vec3 right = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), to_cam);
      float const len2 = glm::dot(right, right);
      right = (len2 > 1e-6f) ? right * (half_w / std::sqrt(len2)) : glm::vec3(half_w, 0.0f, 0.0f);
      glm::vec3 const up(0.0f, half_h, 0.0f);

      float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
      if (motes)
      {
        // CLIENT-EXACT frame rects (UV table builder FUN_0068ebf0): the sheet is a grid of
        // 51/256 = 0.19921875 cells, frames 0-7 = columns 0-3 of rows 0-1. The smiley-face
        // easter-egg sprite lives in COLUMN 4 (frame 8, not in the water set) -- a 0.25 cell
        // size wrongly reached into it and sliced the real sprites.
        float const cell = 0.19921875f; // _DAT_00810334 = 51/256
        u0 = static_cast<float>(d.frame % 4) * cell;
        v0 = static_cast<float>(d.frame / 4) * cell;
        u1 = u0 + cell;
        v1 = v0 + cell;
      }

      glm::vec3 const a = d.pos - right - up;
      glm::vec3 const b = d.pos + right - up;
      glm::vec3 const c = d.pos + right + up;
      glm::vec3 const e = d.pos - right + up;
      auto push = [&](glm::vec3 const& p, float u, float v) {
        _vertex_data.push_back(p.x); _vertex_data.push_back(p.y); _vertex_data.push_back(p.z);
        _vertex_data.push_back(u);   _vertex_data.push_back(v);
      };
      push(a, u0, v1); push(b, u1, v1); push(c, u1, v0);
      push(a, u0, v1); push(c, u1, v0); push(e, u0, v0);
    }
    if (_vertex_data.empty())
    {
      return;
    }

    OpenGL::Scoped::use_program sp (*_program.get());
    sp.uniform("mvp", mvp);
    // Motes: the client writes 0xFFFFFFFF vertex color -- untinted white, the DXT3 alpha shapes
    // the dot. Rain/snow stay light-tinted (editor approximation).
    sp.uniform("color", motes ? glm::vec4(1.0f)
                              : glm::vec4(light_color, snow ? 0.85f : 0.55f));
    gl.activeTexture(GL_TEXTURE0);
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, tex->texture_array());
    sp.uniform("tex", 0);
    sp.uniform("tex_index", static_cast<float>(tex->array_index()));

    OpenGL::Scoped::vao_binder const _ (_vao[0]);
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vb (_vbo);
    gl.bufferData(GL_ARRAY_BUFFER,
                  static_cast<GLsizeiptr>(_vertex_data.size() * sizeof(float)),
                  _vertex_data.data(), GL_STREAM_DRAW);

    // [VULKAN phase G] mirror the precipitation into the shared quad feed: textured, alpha-blended,
    // one flat colour -- structurally a particle emitter, so it reuses those pipelines.
    {
      auto& feed = Noggit::Rendering::VK::particleFeed();
      glm::vec4 const wcolor = motes ? glm::vec4(1.0f)
                                     : glm::vec4(light_color, std::clamp(intensity, 0.f, 1.f));
      Noggit::Rendering::VK::ParticleFeed::Draw d;
      d.first_index = static_cast<std::uint32_t>(feed.indices.size());
      d.index_count = static_cast<std::uint32_t>(_vertex_data.size() / 5u);
      d.base_vertex = static_cast<std::int32_t>(feed.vertices.size() / 9u);
      d.blend = 2;            // SRC_ALPHA / ONE_MINUS_SRC_ALPHA
      d.alpha_test = 0.f;
      d.alpha_mod = 1.f;
      d.ribbon = true;        // no black-fringe divide: this is not a particle sprite sheet
      d.blp = tex && tex->file_key().hasFilepath() ? tex->file_key().filepath() : std::string();
      for (std::size_t i = 0; i + 4 < _vertex_data.size(); i += 5)
      {
        feed.vertices.push_back(_vertex_data[i]);
        feed.vertices.push_back(_vertex_data[i + 1]);
        feed.vertices.push_back(_vertex_data[i + 2]);
        feed.vertices.push_back(_vertex_data[i + 3]);
        feed.vertices.push_back(_vertex_data[i + 4]);
        feed.vertices.push_back(wcolor.x); feed.vertices.push_back(wcolor.y);
        feed.vertices.push_back(wcolor.z); feed.vertices.push_back(wcolor.w);
      }
      // The GL draw is a plain drawArrays; the shared feed is indexed, so number the triangles.
      for (std::uint32_t i = 0; i < d.index_count; ++i)
        feed.indices.push_back(i);
      if (d.index_count)
        feed.draws.push_back(std::move(d));
    }
    if (Noggit::Rendering::VK::vkOwnsParticles())
    {
      return;   // Vulkan draws the precipitation
    }

    OpenGL::Scoped::bool_setter<GL_BLEND, GL_TRUE> const blend;
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    OpenGL::Scoped::depth_mask_setter<GL_FALSE> const no_depth_write;
    // the GL wrapper exposes only the instanced form; instancecount=1 == plain drawArrays
    gl.drawArraysInstanced(GL_TRIANGLES, 0, static_cast<GLsizei>(_vertex_data.size() / 5), 1);
  }

  void WeatherEffect::unload()
  {
    _vao.unload();
    _buffers.unload();
    _program.reset();
    _rain_texture.reset();
    _snow_texture.reset();
    _buffers_are_setup = false;
    _texture_failed = false;
    _drops.clear();
  }

  Circle::Circle() = default;
  Circle::~Circle() = default;

  void Circle::draw(glm::mat4x4 const& mvp
                  , glm::vec3 const& pos
                  , glm::vec4 const& color
                  , float radius)
  {
    if (!_buffers_are_setup)
      setup_buffers();

    OpenGL::Scoped::use_program shader {*_program.get()};
    shader.uniform("model_view_projection", mvp);
    shader.uniform("origin", glm::vec3(pos.x, pos.y, pos.z));
    shader.uniform("radius", radius);
    shader.uniform("inclination", 0.f);
    shader.uniform("orientation", 0.f);
    shader.uniform("world_space", 0);
    shader.uniform("use_texture", 0);
    shader.uniform("color", color);

    OpenGL::Scoped::vao_binder const _ (_vao[0]);
    gl.drawElements(GL_TRIANGLE_STRIP, _indices_vbo, _indice_count, GL_UNSIGNED_SHORT, nullptr);
  }

  void Circle::drawProjectedDecal(glm::mat4x4 const& mvp
                                 , glm::mat4x4 const& inv_view_projection
                                 , glm::vec2 const& inv_viewport
                                 , GLuint scene_depth_tex
                                 , GLuint world_depth_tex
                                 , GLuint empty_vao
                                 , glm::vec3 const& center
                                 , glm::vec3 const& camera
                                 , float radius
                                 , glm::vec4 const& color
                                 , float uv_rotation
                                 , bool simple_ring)
  {
    if (!_decal_program)
    {
      _decal_program.reset(new OpenGL::program(
        {{ GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("circle_decal_vs") }
        ,{ GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("circle_decal_fs") }}));
    }

    if (!_select_texture && !_select_texture_failed)
    {
      try
      {
        _select_texture = std::make_unique<blp_texture>(
          BlizzardArchive::Listfile::FileKey("Textures\\UnitSelectTexture.blp"),
          Noggit::NoggitRenderContext::MAP_VIEW);
        _select_texture->finishLoading();
        _select_texture->upload();
      }
      catch (std::exception const&)
      {
        _select_texture.reset();
        _select_texture_failed = true;
      }
    }
    if (!scene_depth_tex || !world_depth_tex)
    {
      return; // no depth snapshot -> can't project; but a missing texture is fine (procedural fallback)
    }
    bool const use_texture = _select_texture && _select_texture->is_uploaded();

    OpenGL::Scoped::use_program shader {*_decal_program.get()};
    // Same GL state as the WORKING blob-shadow decal (WorldRender): the coverage quad must NOT be
    // depth-tested (it sits at foot height; depth-testing it clips it into the floor -- the "clips
    // thru the WMO mesh" bug). The fragment shader reconstructs each pixel's world pos from the
    // depth TEXTURE and paints onto whatever ground is there, so occlusion is handled per-pixel.
    OpenGL::Scoped::bool_setter<GL_DEPTH_TEST, GL_FALSE> const no_depth_test;
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const no_cull;
    shader.uniform("model_view_projection", mvp);
    shader.uniform("inv_view_projection", inv_view_projection);
    shader.uniform("inv_viewport", inv_viewport);
    shader.uniform("center", center);
    shader.uniform("camera", camera);
    shader.uniform("radius", radius);
    // vertical range generous enough that a bumpy WMO floor within the disc still gets painted
    shader.uniform("v_range", std::max(radius, 3.0f));
    shader.uniform("color", color);
    shader.uniform("uv_rotation", uv_rotation);
    shader.uniform("expand", 2.0f);
    // 2 = plain outline ring (spawn-tool cursor), 1 = client UnitSelectTexture, 0 = blob fallback.
    shader.uniform("use_texture", simple_ring ? 2 : (use_texture ? 1 : 0));

    gl.activeTexture(GL_TEXTURE0);
    if (use_texture && !simple_ring)
    {
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, _select_texture->texture_array());
      shader.uniform("tex", 0);
      shader.uniform("tex_index", static_cast<float>(_select_texture->array_index()));
    }

    gl.activeTexture(GL_TEXTURE1);
    gl.bindTexture(GL_TEXTURE_2D, scene_depth_tex);
    shader.uniform("scene_depth", 1);
    gl.activeTexture(GL_TEXTURE2);
    gl.bindTexture(GL_TEXTURE_2D, world_depth_tex);
    shader.uniform("world_depth", 2);
    gl.activeTexture(GL_TEXTURE0);

    gl.bindVertexArray(empty_vao); // corners from gl_VertexID (TRIANGLE_STRIP, 4 verts)
    gl.drawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);
  }

  void Circle::setup_buffers()
  {
    _vao.upload();
    _buffers.upload();

    // Filled disc (fan strip: rim + centre pairs) -- the fragment shader now renders the client's
    // filled ground blob, so no inner hole.
    std::vector<glm::vec3> vertices;
    vertices.reserve((N_SEGMENTS + 1) * 2);
    std::vector<std::uint16_t> indices;
    indices.reserve((N_SEGMENTS + 1) * 2);

    for (int i = 0; i <= N_SEGMENTS; ++i)
    {
      float angle = glm::two_pi<float>() * i / float(N_SEGMENTS);
      float x = std::cos(angle);
      float z = std::sin(angle);
      vertices.push_back({x, 0.f, z});
      vertices.push_back({0.f, 0.f, 0.f});
      indices.push_back(static_cast<std::uint16_t>(i * 2));
      indices.push_back(static_cast<std::uint16_t>(i * 2 + 1));
    }
    _indice_count = static_cast<int>(indices.size());

    _program.reset(new OpenGL::program({{ GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("circle_vs") }
                       ,{ GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("circle_fs") }}));

    gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>(_vertices_vbo, vertices, GL_STATIC_DRAW);
    gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>(_indices_vbo, indices, GL_STATIC_DRAW);

    OpenGL::Scoped::index_buffer_manual_binder indices_binder (_indices_vbo);
    OpenGL::Scoped::use_program sp (*_program.get());
    {
      OpenGL::Scoped::vao_binder const _ (_vao[0]);
      OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vb (_vertices_vbo);
      sp.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);
      indices_binder.bind();
    }

    _buffers_are_setup = true;
  }

  void Circle::unload()
  {
    _vao.unload();
    _buffers.unload();
    _program.reset();
    _decal_program.reset();
    _buffers_are_setup = false;
  }

  void PathDecal::draw(glm::mat4x4 const& mvp_rel
                      , glm::mat4x4 const& inv_mvp_rel
                      , glm::vec2 const& inv_viewport
                      , GLuint scene_depth_tex
                      , GLuint world_depth_tex
                      , std::vector<glm::vec3> const& points
                      , glm::vec3 const& camera
                      , glm::vec3 const& view_axis
                      , glm::vec4 const& color
                      , float world_width
                      , float min_pixels
                      , float max_pixels
                      , float px_scale)
  {
    if (points.size() < 2 || !scene_depth_tex || !world_depth_tex)
    {
      return; // no route, or no depth snapshot to project onto
    }

    if (!_buffers_are_setup)
    {
      setup_buffers();
    }

    // One instance per segment, CAMERA-RELATIVE. Absolute coords at Karazhan's ~19000 range lose
    // float precision and make the ribbon shimmer as the camera moves -- the same fix the selection
    // circle needed.
    _segment_data.clear();
    _segment_data.reserve((points.size() - 1) * 6);
    for (std::size_t i = 0; i + 1 < points.size(); ++i)
    {
      glm::vec3 const a(points[i] - camera);
      glm::vec3 const b(points[i + 1] - camera);
      _segment_data.insert(_segment_data.end(), {a.x, a.y, a.z, b.x, b.y, b.z});
    }

    gl.bufferData<GL_ARRAY_BUFFER, float>(_segments_vbo, _segment_data, GL_STREAM_DRAW);

    OpenGL::Scoped::use_program shader {*_program.get()};
    // Same GL state as the circle decal: the coverage boxes are not the thing being drawn, and
    // depth-testing them would clip the decal into the very geometry it is meant to drape over.
    // Occlusion is resolved per pixel from the depth texture instead.
    OpenGL::Scoped::bool_setter<GL_DEPTH_TEST, GL_FALSE> const no_depth_test;
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const no_cull;

    shader.uniform("model_view_projection", mvp_rel);
    shader.uniform("inv_view_projection", inv_mvp_rel);
    shader.uniform("inv_viewport", inv_viewport);
    shader.uniform("view_axis", view_axis);
    shader.uniform("color", color);
    shader.uniform("world_width", world_width);
    shader.uniform("min_pixels", min_pixels);
    shader.uniform("max_pixels", max_pixels);
    shader.uniform("px_scale", px_scale);

    gl.activeTexture(GL_TEXTURE1);
    gl.bindTexture(GL_TEXTURE_2D, scene_depth_tex);
    shader.uniform("scene_depth", 1);
    gl.activeTexture(GL_TEXTURE2);
    gl.bindTexture(GL_TEXTURE_2D, world_depth_tex);
    shader.uniform("world_depth", 2);
    gl.activeTexture(GL_TEXTURE0);

    OpenGL::Scoped::vao_binder const _ (_vao[0]);
    gl.drawArraysInstanced(GL_TRIANGLES, 0, 36, static_cast<GLsizei>(points.size() - 1));
  }

  void PathDecal::draw_dots(glm::mat4x4 const& mvp_rel
                           , glm::mat4x4 const& inv_mvp_rel
                           , glm::vec2 const& inv_viewport
                           , GLuint scene_depth_tex
                           , GLuint world_depth_tex
                           , std::vector<glm::vec3> const& points
                           , glm::vec3 const& camera
                           , glm::vec3 const& view_axis
                           , glm::vec4 const& color
                           , float world_width
                           , float min_pixels
                           , float max_pixels
                           , float px_scale)
  {
    if (points.empty() || !scene_depth_tex || !world_depth_tex)
    {
      return;
    }

    if (!_buffers_are_setup)
    {
      setup_buffers();
    }

    // One DEGENERATE segment per point (a == b): the fragment shader's clamped closest-point math
    // collapses to plain distance-to-point, and its round end caps render a filled disc.
    _segment_data.clear();
    _segment_data.reserve(points.size() * 6);
    for (auto const& point : points)
    {
      glm::vec3 const a(point - camera);
      _segment_data.insert(_segment_data.end(), {a.x, a.y, a.z, a.x, a.y, a.z});
    }

    gl.bufferData<GL_ARRAY_BUFFER, float>(_segments_vbo, _segment_data, GL_STREAM_DRAW);

    OpenGL::Scoped::use_program shader {*_program.get()};
    OpenGL::Scoped::bool_setter<GL_DEPTH_TEST, GL_FALSE> const no_depth_test;
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const no_cull;

    shader.uniform("model_view_projection", mvp_rel);
    shader.uniform("inv_view_projection", inv_mvp_rel);
    shader.uniform("inv_viewport", inv_viewport);
    shader.uniform("view_axis", view_axis);
    shader.uniform("color", color);
    shader.uniform("world_width", world_width);
    shader.uniform("min_pixels", min_pixels);
    shader.uniform("max_pixels", max_pixels);
    shader.uniform("px_scale", px_scale);

    gl.activeTexture(GL_TEXTURE1);
    gl.bindTexture(GL_TEXTURE_2D, scene_depth_tex);
    shader.uniform("scene_depth", 1);
    gl.activeTexture(GL_TEXTURE2);
    gl.bindTexture(GL_TEXTURE_2D, world_depth_tex);
    shader.uniform("world_depth", 2);
    gl.activeTexture(GL_TEXTURE0);

    OpenGL::Scoped::vao_binder const _ (_vao[0]);
    gl.drawArraysInstanced(GL_TRIANGLES, 0, 36, static_cast<GLsizei>(points.size()));
  }

  void PathDecal::setup_buffers()
  {
    _vao.upload();
    _buffers.upload();

    _program.reset(new OpenGL::program(
      {{ GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("path_decal_vs") }
      ,{ GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("path_decal_fs") }}));

    OpenGL::Scoped::use_program sp (*_program.get());
    {
      OpenGL::Scoped::vao_binder const _ (_vao[0]);
      OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vb (_segments_vbo);
      // Interleaved (start, end) per instance; the box corners come from gl_VertexID.
      GLsizei const stride = static_cast<GLsizei>(sizeof(float) * 6);
      sp.attrib("seg_a", 3, GL_FLOAT, GL_FALSE, stride, nullptr);
      sp.attrib("seg_b", 3, GL_FLOAT, GL_FALSE, stride,
                reinterpret_cast<GLvoid const*>(sizeof(float) * 3));
      sp.attrib_divisor("seg_a", 1);
      sp.attrib_divisor("seg_b", 1);
    }

    _buffers_are_setup = true;
  }

  void PathDecal::unload()
  {
    _vao.unload();
    _buffers.unload();
    _program.reset();
    _buffers_are_setup = false;
  }

  /*void Cylinder::draw(glm::mat4x4 const& mvp, glm::vec3 const& pos, const glm::vec4 color, float radius, int precision, World* world, int height)
  {
      if (!_buffers_are_setup)
      {
          setup_buffers(precision, world, height);
      }

      OpenGL::Scoped::use_program cylinder_shader {*_program.get()};

      cylinder_shader.uniform("model_view_projection", mvp);
      cylinder_shader.uniform("origin", glm::vec3(pos.x,pos.y,pos.z));
      cylinder_shader.uniform("radius", radius);
      cylinder_shader.uniform("color", color);
      cylinder_shader.uniform("height", height);

      OpenGL::Scoped::vao_binder const _(_vao[0]);

      gl.drawElements(GL_TRIANGLES, _indices_vbo, _indice_count, GL_UNSIGNED_SHORT, nullptr);
  }

  void Cylinder::unload()
  {
    _vao.unload();
    _buffers.unload();
    _program.reset();

    _buffers_are_setup = false;
  }

  void Cylinder::setup_buffers(int precision, World* world, int height)
  {
      if (height <= 10.f)
          height = 10.f;

      _vao.upload();
      _buffers.upload();

      std::vector<glm::vec3> vertices;
      std::vector<std::uint16_t> indices;

      _program.reset(new OpenGL::program({
          { GL_VERTEX_SHADER, OpenGL::shader::src_from_qrc("cylinder_vs")},
          { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("cylinder_fs")}
      }));

      int num = (precision + 1) * 2;
      int numi = precision * 6;

      vertices.resize(num);
      indices.resize(numi);

      for (int i = 0; i <= 1; i++)
      {
          for (int j = 0; j <= precision; j++)
          {
              float y = (i == 0) ? 0.f : float(height);
              float x = -(float)cos(j * glm::two_pi<float>() / precision);
              float z = (float)sin(j * glm::two_pi<float>() / precision);

              vertices[i * (precision + 1) + j] = glm::vec3(x, y, z);
          }
      }

      for (int i = 0; i < 1; i++)
      {
          for (int j = 0; j < precision; j++)
          {
              indices[6 * (i * precision + j) + 0] = i * (precision + 1) + j;
              indices[6 * (i * precision + j) + 1] = i * (precision + 1) + j + 1;
              indices[6 * (i * precision + j) + 2] = (i + 1) * (precision + 1) + j;
              indices[6 * (i * precision + j) + 3] = i * (precision + 1) + j + 1;
              indices[6 * (i * precision + j) + 4] = (i + 1) * (precision + 1) + j + 1;
              indices[6 * (i * precision + j) + 5] = (i + 1) * (precision + 1) + j;
          }
      }

      _indice_count = (int)indices.size();

      gl.bufferData<GL_ARRAY_BUFFER, glm::vec3> (_vertices_vbo, vertices, GL_STATIC_DRAW);
      gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t> (_indices_vbo, indices, GL_STATIC_DRAW);


      OpenGL::Scoped::index_buffer_manual_binder indices_binder(_indices_vbo);
      OpenGL::Scoped::use_program shader (*_program.get());

      {
          OpenGL::Scoped::vao_binder const _ (_vao[0]);
          OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vertices_binder (_vertices_vbo);
          shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);
          indices_binder.bind();
      }

      _buffers_are_setup = true;
  }*/

  void Line::initSpline()
  {
      draw(glm::mat4x4{},
          std::vector<glm::vec3>{ {}, {} },
          glm::vec4{},
          false);
  }

  void Line::draw(glm::mat4x4 const& mvp
      , std::vector<glm::vec3> const points
      , glm::vec4 const& color
      , bool spline
  )
  {
      if (points.size() < 2)
          return;

      if (!spline || points.size() == 2)
      {
          setup_buffers(points);
      }
      else
      {
          initSpline();
          setup_buffers_interpolated(points);
      }

      OpenGL::Scoped::use_program line_shader{ *_program.get() };

      line_shader.uniform("model_view_projection", mvp);
      line_shader.uniform("color", color);

      OpenGL::Scoped::vao_binder const _(_vao[0]);
      gl.drawElements(GL_LINE_STRIP, _indices_vbo, _indice_count, GL_UNSIGNED_SHORT, nullptr);
  }


  void Line::setup_buffers(std::vector<glm::vec3> const points)
  {
      _vao.upload();
      _buffers.upload();

      std::vector<glm::vec3> vertices = points;
      std::vector<std::uint16_t> indices;

      for (int i = 0; i < points.size(); ++i)
      {
          indices.push_back(i);
      }

      setup_shader(vertices, indices);
  }

  void Line::setup_buffers_interpolated(std::vector<glm::vec3> const points)
  {
      const float tension = 0.5f;

      std::vector<glm::vec3> tempPoints;
      tempPoints.push_back(points[0]);

      for (auto const& p : points)
          tempPoints.push_back(p);

      tempPoints.push_back(points[points.size() - 1]);

      std::vector<glm::vec3> vertices;
      std::vector<std::uint16_t> indices;

      for (int i = 1; i < tempPoints.size() - 2; i++)
      {
          auto s = tension * 2.f;
          auto p0 = tempPoints[i - 1];
          auto p1 = tempPoints[i + 0];
          auto p2 = tempPoints[i + 1];
          auto p3 = tempPoints[i + 2];

          glm::vec3 m1(
              (p2.x - p0.x) / s,
              (p2.y - p0.y) / s,
              (p2.z - p0.z) / s
          );

          glm::vec3 m2(
              (p3.x - p1.x) / s,
              (p3.y - p1.y) / s,
              (p3.z - p1.z) / s
          );

          vertices.push_back(interpolate(0, p1, p2, m1, m2));

          for (float t = 0.01f; t < 1.f; t += 0.01f)
              vertices.push_back(interpolate(t, p1, p2, m1, m2));

          vertices.push_back(interpolate(1, p1, p2, m1, m2));
      }

      for (int i = 0; i < vertices.size(); ++i)
      {
          indices.push_back(i);
      }

      setup_shader(vertices, indices);
  }

  glm::vec3 Line::interpolate(float t, glm::vec3 p0, glm::vec3 p1, glm::vec3 m0, glm::vec3 m1)
  {
      auto c = 2 * t * t * t - 3 * t * t;
      auto c0 = c + 1;
      auto c1 = t * t * t - 2 * t * t + t;
      auto c2 = -c;
      auto c3 = t * t * t - t * t;

      return (c0 * p0 + c1 * m0 + c2 * p1 + c3 * m1);
  }

  void Line::setup_shader(std::vector<glm::vec3> vertices, std::vector<std::uint16_t> indices)
  {
      _indice_count = (int)indices.size();
      // Compile the program only once; setup_shader runs on every draw() (the vertex data changes per
      // line), and recreating the GL program each call would stall when drawing many lines per frame.
      if (!_program)
      {
        _program.reset(new OpenGL::program(
            {
                { GL_VERTEX_SHADER, OpenGL::shader::src_from_qrc("line_vs") },
                { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("line_fs") }
            }
        ));
      }

      gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>(_vertices_vbo, vertices, GL_STATIC_DRAW);
      gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>(_indices_vbo, indices, GL_STATIC_DRAW);

      OpenGL::Scoped::index_buffer_manual_binder indices_binder(_indices_vbo);
      OpenGL::Scoped::use_program shader(*_program.get());

      {
          OpenGL::Scoped::vao_binder const _(_vao[0]);

          OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vertices_binder(_vertices_vbo);
          shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);
          indices_binder.bind();
      }

      _buffers_are_setup = true;
  }

  void Line::unload()
  {
      _vao.unload();
      _buffers.unload();
      _program.reset();

      _buffers_are_setup = false;

  }

  // ==================== WaterRipples (client Water0Ripple port, RE doc 36) ====================

  WaterRipples::WaterRipples() = default;
  WaterRipples::~WaterRipples() = default;

  void WaterRipples::setup(Noggit::NoggitRenderContext context)
  {
    _program.reset(new OpenGL::program(
      { { GL_VERTEX_SHADER
        , R"code(
#version 330 core
uniform mat4 mvp;
in vec3 position;
in vec2 tex_coord;
out vec2 uv_;
void main()
{
  uv_ = tex_coord;
  gl_Position = mvp * vec4(position, 1.0);
}
)code" }
      , { GL_FRAGMENT_SHADER
        , R"code(
#version 330 core
uniform sampler2DArray tex;
uniform float tex_index;
uniform float ripple_alpha;
in vec2 uv_;
out vec4 out_color;
void main()
{
  vec4 t = texture(tex, vec3(uv_, tex_index));
  // white x alpha additive (the client draws vertex colour alpha<<24|0xFFFFFF)
  out_color = vec4(t.rgb, t.a) * ripple_alpha;
}
)code" } }));

    auto load_tex = [&](std::unique_ptr<blp_texture>& slot, char const* path)
    {
      try
      {
        slot = std::make_unique<blp_texture>(BlizzardArchive::Listfile::FileKey(path), context);
        slot->finishLoading();
        slot->upload();
      }
      catch (std::exception const&)
      {
        _texture_failed = true;
      }
    };
    load_tex(_wake_texture, "XTextures\\splash\\wake.blp");
    load_tex(_splash_texture, "XTextures\\splash\\splash.blp");

    _vao.upload();
    _buffers.upload();
    _buffers_are_setup = true;
  }

  void WaterRipples::spawn(glm::vec3 const& surface_pos, float rotation_rad, float size0,
                           float growth, float lifetime_s, float alpha_peak, int kind)
  {
    if (_entries.size() >= 128) // client pool size
    {
      _entries.erase(_entries.begin());
    }
    RippleEntry e;
    e.pos = surface_pos;
    e.rot = rotation_rad;
    e.size0 = size0;
    e.growth = growth;
    e.birth_ms = -1.0f; // stamped on first draw (animtime supplied there)
    e.lifetime_ms = lifetime_s * 1000.0f;
    e.alpha_peak = alpha_peak;
    e.kind = kind;
    _entries.push_back(e);
  }

  void WaterRipples::draw(glm::mat4x4 const& mvp, float animtime_ms,
                          Noggit::NoggitRenderContext context)
  {
    if (_entries.empty())
    {
      return;
    }
    if (!_buffers_are_setup)
    {
      setup(context);
    }
    // RIPPLE-DIAG (one-shot): proves the DRAW stage runs and names the texture state.
    {
      static bool s_ripple_draw_diag = false;
      if (!s_ripple_draw_diag)
      {
        s_ripple_draw_diag = true;
        LogError << "RIPPLE-DIAG draw: entries=" << _entries.size()
                 << " tex_failed=" << (_texture_failed ? 1 : 0)
                 << " wake=" << (_wake_texture ? (_wake_texture->is_uploaded() ? "up" : "not-up") : "null")
                 << " splash=" << (_splash_texture ? (_splash_texture->is_uploaded() ? "up" : "not-up") : "null")
                 << std::endl;
      }
    }
    if (_texture_failed || !_wake_texture || !_splash_texture
        || !_wake_texture->is_uploaded() || !_splash_texture->is_uploaded())
    {
      return;
    }

    for (auto& e : _entries)
    {
      if (e.birth_ms < 0.0f)
      {
        e.birth_ms = animtime_ms;
      }
    }
    _entries.erase(std::remove_if(_entries.begin(), _entries.end(),
                                  [&](RippleEntry const& e)
                                  { return animtime_ms - e.birth_ms > e.lifetime_ms; }),
                   _entries.end());
    if (_entries.empty())
    {
      return;
    }

    OpenGL::Scoped::use_program sp(*_program.get());
    sp.uniform("mvp", mvp);
    OpenGL::Scoped::vao_binder const _(_vao[0]);
    OpenGL::Scoped::bool_setter<GL_BLEND, GL_TRUE> const blend;
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE); // additive over the water surface
    OpenGL::Scoped::depth_mask_setter<GL_FALSE> const no_depth_write;
    // USER-DIAGNOSED 2026-08-26 ("they all draw under water only, on the underwater side of the
    // surface"): the quad winding faced DOWN (ax x az = (0,-1,0)), so back-face culling showed the
    // rings only from below. Double-side the pass -- a flat surface ring must read from above AND
    // (harmlessly) from underwater.
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const no_cull;

    for (RippleEntry const& e : _entries)
    {
      float const age = std::clamp((animtime_ms - e.birth_ms) / e.lifetime_ms, 0.0f, 1.0f);
      // client laws: linear growth over life; alpha attack to 40% of life then decay (K = 0.4)
      float const size = e.size0 + e.growth * age * (e.lifetime_ms / 1000.0f);
      float const alpha = e.alpha_peak * (age < 0.4f ? age / 0.4f : (1.0f - age) / 0.6f);
      float const c = std::cos(e.rot);
      float const s = std::sin(e.rot);
      glm::vec3 const ax(c * size, 0.0f, s * size);
      glm::vec3 const az(-s * size, 0.0f, c * size);
      glm::vec3 const p(e.pos.x, e.pos.y + 0.05f, e.pos.z);

      glm::vec3 const va = p - ax - az;
      glm::vec3 const vb = p + ax - az;
      glm::vec3 const vc = p + ax + az;
      glm::vec3 const vd = p - ax + az;
      _vertex_data.clear();
      auto push = [&](glm::vec3 const& v, float u, float w)
      {
        _vertex_data.push_back(v.x); _vertex_data.push_back(v.y); _vertex_data.push_back(v.z);
        _vertex_data.push_back(u);   _vertex_data.push_back(w);
      };
      // Winding flipped up-facing (see the cull note above) -- kept correct even if a future
      // change re-enables culling in this pass.
      push(va, 0.f, 0.f); push(vc, 1.f, 1.f); push(vb, 1.f, 0.f);
      push(va, 0.f, 0.f); push(vd, 0.f, 1.f); push(vc, 1.f, 1.f);

      blp_texture* tex = (e.kind == 0) ? _wake_texture.get() : _splash_texture.get();
      gl.activeTexture(GL_TEXTURE0);
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, tex->texture_array());
      sp.uniform("tex", 0);
      sp.uniform("tex_index", static_cast<float>(tex->array_index()));
      sp.uniform("ripple_alpha", alpha);

      OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vb_bind(_vbo);
      gl.bufferData(GL_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(_vertex_data.size() * sizeof(float)),
                    _vertex_data.data(), GL_STREAM_DRAW);
      sp.attrib("position", 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), 0);
      sp.attrib("tex_coord", 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                reinterpret_cast<void*>(3 * sizeof(float)));
      gl.drawArraysInstanced(GL_TRIANGLES, 0, 6, 1);
    }
  }

  void WaterRipples::unload()
  {
    _vao.unload();
    _buffers.unload();
    _program.reset();
    _wake_texture.reset();
    _splash_texture.reset();
    _buffers_are_setup = false;
    _texture_failed = false;
    _entries.clear();
  }