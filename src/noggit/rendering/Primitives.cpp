// This file is part of Noggit3, licensed under GNU General Public License (version 3).

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

    blp_texture* tex = (type == 2) ? _snow_texture.get() : _rain_texture.get();
    if (!tex || !tex->is_uploaded())
    {
      return;
    }

    // Client weather volume (MapWeather init): a 44 x 44 horizontal, +-25 vertical box around the
    // camera. Density: the client's flake pool caps at 0x1800 (6144); scaled down for the editor's
    // CPU refill and by the intensity slider (the client's weatherDensity works the same way).
    float const kBoxH = 44.0f;
    float const kBoxV = 25.0f;
    bool const snow = (type == 2);
    int const target = static_cast<int>((snow ? 900 : 1500) * intensity);

    float dt = (_last_time >= 0.0f) ? (animtime_ms - _last_time) / 1000.0f : 0.016f;
    _last_time = animtime_ms;
    dt = std::clamp(dt, 0.0f, 0.1f);

    static std::mt19937 rng(20260823u);
    auto frand = [&](float a, float b) {
      return a + (b - a) * (static_cast<float>(rng()) / static_cast<float>(rng.max()));
    };

    if (_active_type != type)
    {
      _active_type = type;
      _drops.clear();
    }
    while (static_cast<int>(_drops.size()) < target)
    {
      Drop d;
      d.pos = glm::vec3(camera_pos.x + frand(-kBoxH, kBoxH),
                        camera_pos.y + frand(-kBoxV, kBoxV),
                        camera_pos.z + frand(-kBoxH, kBoxH));
      d.speed = snow ? frand(4.0f, 7.0f) : frand(50.0f, 70.0f);
      d.seed = frand(0.0f, 6.2831853f);
      _drops.push_back(d);
    }
    if (static_cast<int>(_drops.size()) > target)
    {
      _drops.resize(target);
    }

    float const t_s = animtime_ms / 1000.0f;
    for (Drop& d : _drops)
    {
      d.pos.y -= d.speed * dt;
      if (snow)
      {
        d.pos.x += std::sin(t_s * 0.9f + d.seed) * 1.2f * dt;
        d.pos.z += std::cos(t_s * 0.7f + d.seed) * 1.0f * dt;
      }
      // Keep the volume centered on the (moving) camera: wrap on every axis.
      if (d.pos.y < camera_pos.y - kBoxV)
      {
        d.pos.y += 2.0f * kBoxV;
        d.pos.x = camera_pos.x + frand(-kBoxH, kBoxH);
        d.pos.z = camera_pos.z + frand(-kBoxH, kBoxH);
      }
      if (d.pos.x < camera_pos.x - kBoxH) d.pos.x += 2.0f * kBoxH;
      if (d.pos.x > camera_pos.x + kBoxH) d.pos.x -= 2.0f * kBoxH;
      if (d.pos.z < camera_pos.z - kBoxH) d.pos.z += 2.0f * kBoxH;
      if (d.pos.z > camera_pos.z + kBoxH) d.pos.z -= 2.0f * kBoxH;
    }

    // Build camera-facing quads: rain = tall thin streak, snow = small flake.
    float const half_w = snow ? 0.09f : 0.03f;
    float const half_h = snow ? 0.09f : 0.9f;
    _vertex_data.clear();
    _vertex_data.reserve(_drops.size() * 6 * 5);
    for (Drop const& d : _drops)
    {
      glm::vec3 const to_cam = camera_pos - d.pos;
      glm::vec3 right = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), to_cam);
      float const len2 = glm::dot(right, right);
      right = (len2 > 1e-6f) ? right * (half_w / std::sqrt(len2)) : glm::vec3(half_w, 0.0f, 0.0f);
      glm::vec3 const up(0.0f, half_h, 0.0f);

      glm::vec3 const a = d.pos - right - up;
      glm::vec3 const b = d.pos + right - up;
      glm::vec3 const c = d.pos + right + up;
      glm::vec3 const e = d.pos - right + up;
      auto push = [&](glm::vec3 const& p, float u, float v) {
        _vertex_data.push_back(p.x); _vertex_data.push_back(p.y); _vertex_data.push_back(p.z);
        _vertex_data.push_back(u);   _vertex_data.push_back(v);
      };
      push(a, 0.f, 1.f); push(b, 1.f, 1.f); push(c, 1.f, 0.f);
      push(a, 0.f, 1.f); push(c, 1.f, 0.f); push(e, 0.f, 0.f);
    }
    if (_vertex_data.empty())
    {
      return;
    }

    OpenGL::Scoped::use_program sp (*_program.get());
    sp.uniform("mvp", mvp);
    sp.uniform("color", glm::vec4(light_color, snow ? 0.85f : 0.55f));
    gl.activeTexture(GL_TEXTURE0);
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, tex->texture_array());
    sp.uniform("tex", 0);
    sp.uniform("tex_index", static_cast<float>(tex->array_index()));

    OpenGL::Scoped::vao_binder const _ (_vao[0]);
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vb (_vbo);
    gl.bufferData(GL_ARRAY_BUFFER,
                  static_cast<GLsizeiptr>(_vertex_data.size() * sizeof(float)),
                  _vertex_data.data(), GL_STREAM_DRAW);

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