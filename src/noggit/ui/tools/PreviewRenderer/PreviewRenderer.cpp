#include "PreviewRenderer.hpp"

#include <opengl/scoped.hpp>
#include <noggit/rendering/Primitives.hpp>
#include <noggit/Selection.h>
#include <noggit/tool_enums.hpp>
#include <noggit/AsyncLoader.h>
#include <noggit/rendering/ModelRender.hpp>
#include <noggit/Log.h>

#include <vector>
#include <cmath>
#include <stdexcept>
#include <limits>
#include <thread>
#include <chrono>
#include <cstdlib>

#include <QSettings>
#include <QColor>
#include <QMatrix4x4>
#include <QVector3D>


using namespace Noggit::Ui::Tools;

namespace
{
  // The offscreen preview MUST render with the main map-view GL context current. VAOs (and FBOs) are
  // NOT shared across GL contexts (OpenGL spec) -- not even with AA_ShareOpenGLContexts -- so drawing
  // the preview models in a separate offscreen context references VAOs created in the main context and
  // __fastfails the NVIDIA driver (0xC0000409 in nvoglv64). Rendering in the context that OWNS the
  // VAOs removes that crash class outright. Borrow the live map-view viewport's context to do it.
  Noggit::Ui::Tools::ViewportManager::Viewport* main_viewport()
  {
    for (auto* vp : Noggit::Ui::Tools::ViewportManager::ViewportManager::_viewports)
    {
      if (vp && vp->getRenderContext() == Noggit::MAP_VIEW)
        return vp;
    }
    return nullptr;
  }

  // Offscreen thumbnail rendering is DISABLED by default. The preview model/WMO draw __fastfails the
  // NVIDIA driver (0xC0000409 in nvoglv64), surfacing at the next GL flush (readback). Dump-confirmed
  // it's a DRIVER bug: it crashes even with the main map-view context current, it is NOT a C++
  // exception (uncatchable), and it cannot be fixed from app code -- only avoided by not drawing.
  // Set NOGGIT_ENABLE_PREVIEW_THUMBS=1 to re-enable the render (for debugging / a future driver fix).
  bool offscreen_previews_enabled()
  {
    static bool const on = std::getenv("NOGGIT_ENABLE_PREVIEW_THUMBS") != nullptr;
    return on;
  }
}


PreviewRenderer::PreviewRenderer(int width, int height, Noggit::NoggitRenderContext context, QWidget* parent)
  :  Noggit::Ui::Tools::ViewportManager::Viewport(parent)
  , _camera (glm::vec3(0.0f, 0.0f, 0.0f), math::degrees(0.0f), math::degrees(0.0f))
  , _settings (new QSettings())
  , _width(width)
  , _height(height)
  , _liquid_texture_manager(context)
{
  _context = context;
  _cache = {};

  // No private offscreen GL context: preview thumbnails are rendered into an FBO with the MAIN
  // map-view context current (see renderToPixmap). VAOs aren't shared across contexts, so a second
  // context's draws use invalid VAOs and __fastfail the NVIDIA driver; rendering in the VAO-owning
  // context eliminates that. Just configure the FBO format and default lighting here (no GL needed).
  _fmt.setSamples(1);
  _fmt.setInternalTextureFormat(GL_RGBA8);
  _fmt.setAttachment(QOpenGLFramebufferObject::Depth);

  _light_dir = glm::vec3(0.0f, 1.0f, 0.0f);
  _diffuse_light = {1.0f, 0.532352924f, 0.0f};
  _ambient_light = {0.407770514f, 0.508424163f, 0.602650642f};
  _background_color = {0.5f, 0.5f, 0.5f};
}

void PreviewRenderer::setModel(std::string const &filename)
{
  _filename = filename;
  _model_instances.clear();
  _wmo_instances.clear();

  // add new model instance
  QString q_filename = QString(filename.c_str());

  if (q_filename.endsWith(".wmo"))
  {
    auto& instance = _wmo_instances.emplace_back(filename, _context);
    instance.wmo->wait_until_loaded();
    instance.recalcExtents();

  }
  else if (q_filename.endsWith(".m2"))
  {
    auto& instance = _model_instances.emplace_back(filename, _context);
    instance.model->wait_until_loaded();
    instance.recalcExtents();
  }
  else
  {
    throw std::logic_error("Preview renderer only supports viewing M2 and WMO for now.");
  }

  _lighting_needs_update = true;

  auto diffuse_color = _settings->value("assetBrowser/diffuse_light",
    QVariant::fromValue(QColor::fromRgbF(1.0f, 0.532352924f, 0.0f))).value<QColor>();
  _diffuse_light = {static_cast<float>(diffuse_color.redF()),
                    static_cast<float>(diffuse_color.greenF()),
                    static_cast<float>(diffuse_color.blueF())};

 auto ambient_color = _settings->value("assetBrowser/ambient_light",
     QVariant::fromValue(QColor::fromRgbF(0.407770514f, 0.508424163f, 0.602650642f))).value<QColor>();

 _ambient_light = {static_cast<float>(ambient_color.redF()),
                   static_cast<float>(ambient_color.greenF()),
                   static_cast<float>(ambient_color.blueF())};

  auto background_color = _settings->value("assetBrowser/background_color",
     QVariant::fromValue(QColor(127, 127, 127))).value<QColor>();

  _background_color = {static_cast<float>(background_color.redF()),
                       static_cast<float>(background_color.greenF()),
                       static_cast<float>(background_color.blueF())};

  resetCamera();
}

void PreviewRenderer::setModelOffscreen(std::string const& filename)
{
  // Offscreen thumbnails are disabled (see offscreen_previews_enabled / renderToPixmap): don't even
  // load the model for a preview that won't be drawn. renderToPixmap returns a blank thumbnail.
  if (!offscreen_previews_enabled())
  {
    _filename = filename;
    return;
  }
  // setModel does no GL (it creates model instances + blocks on async load + computes extents); the
  // actual GL upload/draw happens in renderToPixmap under the main map-view context.
  setModel(filename);
}


void PreviewRenderer::resetCamera(float x, float y, float z, float roll, float yaw, float pitch)
{
  _camera.reset(x, y, z, roll, yaw, pitch);

  std::vector<glm::vec3> extents = calcSceneExtents();
  auto const valid_vec = [](glm::vec3 const& value)
  {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
  };

  if (!valid_vec(extents[0]) || !valid_vec(extents[1])
      || extents[0].x > extents[1].x
      || extents[0].y > extents[1].y
      || extents[0].z > extents[1].z)
  {
    return;
  }

  _camera.position = (extents[0] + extents[1]) / 2.0f;
  float radius = std::max(glm::distance(_camera.position, extents[0]), glm::distance(_camera.position, extents[1]));
  if (!std::isfinite(radius) || radius < 1.0f)
  {
    radius = 1.0f;
  }

  float distance_factor = std::abs(radius / std::sin(_camera.fov()._ / 2.f));
  if (!std::isfinite(distance_factor))
  {
    distance_factor = radius * 2.0f;
  }
  _camera.move_forward_factor(-1.f, distance_factor);

}


void PreviewRenderer::draw()
{
  auto trace = [this](char const* step) {
    if (_preview_trace) { LogError << "PREVIEW draw step: " << step << " '" << _filename << "'" << std::endl; gl.finish(); }
  };
  trace("enter");

  if (!_uploaded)
  [[unlikely]]
  {
    upload();
  }
  trace("uploaded");

  gl.clearColor(_background_color.r, _background_color.g, _background_color.b, 1.0f);
  gl.depthMask(GL_TRUE);
  gl.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  float culldistance = 10000000;

  auto mv = model_view();
  auto proj = projection();

  glm::mat4x4 const mvp(proj * mv);
  math::frustum const frustum (glm::transpose(mvp));

  updateMVPUniformBlock(mv, proj, _camera.position);

  if (_lighting_needs_update)
    updateLightingUniformBlock();

  gl.enable(GL_DEPTH_TEST);
  gl.depthFunc(GL_LEQUAL);
  gl.enable(GL_BLEND);
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // draw WMOs
  std::unordered_map<std::string, std::vector<ModelInstance*>> _wmo_doodads;

  if (_draw_wmo.get() && !_wmo_instances.empty())
  {
    trace("wmo begin");
    /* set anim time only once per frame
    {
      OpenGL::Scoped::use_program water_shader {_liquid_render->shader_program()};
      water_shader.uniform("animtime", _animtime / 2880.f);

      //water_shader.uniform("model_view", model_view().transposed());
      //water_shader.uniform("projection", projection().transposed());

      //water_shader.uniform("ocean_color_light", ocean_color_light);
      //water_shader.uniform("ocean_color_dark", ocean_color_dark);
      //water_shader.uniform("river_color_light", river_color_light);
      //water_shader.uniform("river_color_dark", river_color_dark);
      //water_shader.uniform("use_transform", 1);
    }

     */

    {
      OpenGL::Scoped::use_program wmo_program{*_wmo_program.get()};

      wmo_program.uniform("camera", glm::vec3(_camera.position.x, _camera.position.y, _camera.position.z));


      for (auto& wmo_instance : _wmo_instances)
      {
        wmo_instance.wmo->wait_until_loaded();
        wmo_instance.wmo->waitForChildrenLoaded();
        wmo_instance.ensureExtents();
        wmo_instance.draw(
            wmo_program, nullptr, nullptr, model_view(), projection(), frustum, culldistance,
            _camera.position, _draw_boxes.get(), _draw_models.get() 
            , false, std::vector<selection_type>(), 0, false, display_mode::in_3D, true
        );

        auto doodads = wmo_instance.get_doodads(true);

        if (doodads)
        {
          for (auto& pair : *doodads)
          {
            for (auto& doodad : pair.second)
              _wmo_doodads[doodad.model->file_key().filepath()].push_back(&doodad);
          }
        }

     }

    }

  }

  // draw M2
  std::unordered_map<Model*, std::size_t> model_boxes_to_draw;

  if (_draw_models.get() && !(_model_instances.empty() && _wmo_doodads.empty()))
  {
    if (_draw_animated.get())
      ModelManager::resetAnim();

    auto setup_m2_render_state = [](OpenGL::Scoped::use_program& m2_shader,
                                    OpenGL::M2RenderState& model_render_state)
    {
      model_render_state.tex_arrays = { 0, 0 };
      model_render_state.tex_indices = { 0, 0 };
      model_render_state.tex_unit_lookups = { 0, 0 };
      gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      gl.disable(GL_BLEND);
      gl.depthMask(GL_TRUE);
      gl.enable(GL_CULL_FACE);
      m2_shader.uniform("blend_mode", 0);
      m2_shader.uniform("unfogged", static_cast<int>(model_render_state.unfogged));
      m2_shader.uniform("unlit", static_cast<int>(model_render_state.unlit));
      m2_shader.uniform("tex_unit_lookup_1", 0);
      m2_shader.uniform("tex_unit_lookup_2", 0);
      m2_shader.uniform("masked_additive", 0);
      m2_shader.uniform("pixel_shader", 0);
    };

    if (!_model_instances.empty())
    {
      trace("m2 instances begin");
      OpenGL::Scoped::use_program m2_shader {*_m2_program.get()};

      OpenGL::M2RenderState model_render_state;
      setup_m2_render_state(m2_shader, model_render_state);

      for (auto& model_instance : _model_instances)
      {
        model_instance.model->wait_until_loaded();
        model_instance.model->waitForChildrenLoaded();
        if (_preview_trace && model_instance.model->file_key().hasFilepath())
        { LogError << "PREVIEW draw m2: '" << model_instance.model->file_key().filepath() << "'" << std::endl; gl.finish(); }

        model_instance.model->renderer()->draw(
          mv
          , model_instance
          , m2_shader
          , model_render_state
          , frustum
          , culldistance
          , _camera.position
          , _animtime
          , display_mode::in_3D
        );
      }
    }

    if (!_wmo_doodads.empty())
    {
      trace("wmo doodads begin");
      // WMO doodads are drawn INDIVIDUALLY here (uniform transform), NOT through the instanced
      // path. In the asset-browser OFFSCREEN context the instanced doodad draw (per-instance
      // transform attribute + bone samplerBuffer) __fastfails the NVIDIA driver even though every
      // buffer/param is valid and the same draw works in the main-window context -- a driver-level
      // instanced-VAO fragility specific to the offscreen preview context (RE: barrel01 crash,
      // trace boneBufSize=64 valid). The individual path avoids the per-instance transform
      // attribute entirely and renders the doodads correctly.
      OpenGL::Scoped::use_program m2_shader {*_m2_program.get()};

      OpenGL::M2RenderState model_render_state;
      setup_m2_render_state(m2_shader, model_render_state);

      for (auto& it : _wmo_doodads)
      {
        for (auto* instance : it.second)
        {
          if (_preview_trace && instance->model->file_key().hasFilepath())
          { LogError << "PREVIEW draw wmo-doodad: '" << instance->model->file_key().filepath() << "'" << std::endl; gl.finish(); }
          instance->model->renderer()->draw(
            mv
            , *instance
            , m2_shader
            , model_render_state
            , frustum
            , culldistance
            , _camera.position
            , _animtime
            , display_mode::in_3D
          );
        }
      }
    }


    if(_draw_boxes.get() && !model_boxes_to_draw.empty())
    {
      OpenGL::Scoped::use_program m2_box_shader{ *_m2_box_program.get() };

      OpenGL::Scoped::bool_setter<GL_LINE_SMOOTH, GL_TRUE> const line_smooth;
      gl.hint (GL_LINE_SMOOTH_HINT, GL_NICEST);

      for (auto& it : model_boxes_to_draw)
      {
        glm::vec4 color = it.first->is_hidden()
                                ? glm::vec4(0.f, 0.f, 1.f, 1.f)
                                : ( it.first->use_fake_geometry()
                                    ? glm::vec4(1.f, 0.f, 0.f, 1.f)
                                    : glm::vec4(0.75f, 0.75f, 0.75f, 1.f)
                                )
        ;

        m2_box_shader.uniform("color", color);
        it.first->renderer()->drawBox(m2_box_shader, it.second);
      }
    }
  }


  gl.bindVertexArray(0);
  gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

  // model particles

  /*
  if (_draw_animated.get() && !model_with_particles.empty())
  {
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const cull;
    OpenGL::Scoped::depth_mask_setter<GL_FALSE> const depth_mask;

    OpenGL::Scoped::use_program particles_shader {*_m2_particles_program.get()};

    particles_shader.uniform("model_view_projection", mvp);
    particles_shader.uniform("tex", 0);
    OpenGL::texture::set_active_texture(0);

    for (auto& it : model_with_particles)
    {
      it.first->draw_particles(model_view().transposed(), particles_shader, it.second);
    }
  }

  if (_draw_animated.get() && !model_with_particles.empty())
  {
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const cull;
    OpenGL::Scoped::depth_mask_setter<GL_FALSE> const depth_mask;

    OpenGL::Scoped::use_program ribbon_shader {*_m2_ribbons_program.get()};

    ribbon_shader.uniform("model_view_projection", mvp);
    ribbon_shader.uniform("tex", 0);

    gl.blendFunc(GL_SRC_ALPHA, GL_ONE);

    for (auto& it : model_with_particles)
    {
      it.first->draw_ribbons(ribbon_shader, it.second);
    }
  }

  */

  gl.enable(GL_BLEND);
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  if (_draw_grid.get())
  {
    _grid.draw(mvp, glm::vec3(0.f, 0.f, 0.f),
               glm::vec4(0.7f, 0.7f, 0.7f, 1.0f), 30.f);

  }

}

glm::mat4x4 PreviewRenderer::model_view() const
{
  return _camera.look_at_matrix();
}

glm::mat4x4 PreviewRenderer::projection() const
{
  float far_z = _settings->value("farZ", 2048).toFloat();
  return glm::perspective(_camera.fov()._, aspect_ratio(), 1.f, far_z);
}

float PreviewRenderer::aspect_ratio() const
{
  return static_cast<float>(_width) / static_cast<float>(_height);
}

std::vector<glm::vec3> PreviewRenderer::calcSceneExtents()
{
  glm::vec3 min = {std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max()};

  glm::vec3 max = {std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::lowest()};

  for (auto& instance : _model_instances)
  {
    for (int i = 0; i < 3; ++i)
    {
      min[i] = std::min(instance.extents[0][i], min[i]);
      max[i] = std::max(instance.extents[1][i], max[i]);
    }
  }

  for (auto& instance : _wmo_instances)
  {
    for (int i = 0; i < 3; ++i)
    {
      min[i] = std::min(instance.extents[0][i], min[i]);
      max[i] = std::max(instance.extents[1][i], max[i]);
    }
  }

  return std::move(std::vector<glm::vec3>{min, max});
}

QPixmap* PreviewRenderer::renderToPixmap()
{
  std::tuple<std::string, int, int> const curEntry{_filename, _width, _height};
  auto it{_cache.find(curEntry)};

  if(it != _cache.end())
    return &it->second;

  // Offscreen thumbnails are DISABLED: the preview model/WMO draw __fastfails the NVIDIA driver
  // (0xC0000409 in nvoglv64), surfacing at the next GL flush (this readback). Dump-confirmed it is a
  // DRIVER bug -- it crashes even with the main map-view context current, is uncatchable (a driver
  // __fastfail, not a C++ exception), and can only be avoided, not fixed, from app code. Return a
  // blank thumbnail rather than issuing the crashing draw. The onscreen 3D ModelViewer uses a
  // different path and is unaffected. Set NOGGIT_ENABLE_PREVIEW_THUMBS=1 to re-enable (debug only).
  if (!offscreen_previews_enabled())
    return &(_cache[curEntry] = QPixmap());

  // (Only reached when previews are explicitly re-enabled.) Render into an FBO with the MAIN
  // map-view context current -- mirrors WorldRender::saveMinimap. try/catch stays because noggit's
  // gl-error-check (verify_context) can throw; a driver __fastfail, however, it cannot catch.
  auto* main = main_viewport();
  if (!main || !main->context() || !main->context()->isValid())
  {
    LogError << "PreviewRenderer: no map-view GL context available for '" << _filename
             << "' -- skipping preview." << std::endl;
    return &(_cache[curEntry] = QPixmap());
  }

  try
  {
    main->makeCurrent();
    OpenGL::context::scoped_setter const context_set (::gl, main->context());

    QOpenGLFramebufferObject pixel_buffer(_width, _height, _fmt);
    pixel_buffer.bind();

    gl.viewport(0, 0, _width, _height);
    gl.clearColor(_background_color.r, _background_color.g, _background_color.b, 1.f);
    gl.depthMask(GL_TRUE);
    gl.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // draw() blocks per instance on wait_until_loaded()/waitForChildrenLoaded() (those are plain
    // condition-variable waits -- they do NOT pump the Qt event loop), so geometry and textures are
    // loaded by the time it returns. No async-loader wait/redraw dance and no context re-assert.
    tick(1.0f);
    draw();

    // Clear alpha from the image (opaque thumbnail).
    gl.colorMask(false, false, false, true);
    gl.clearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.clear(GL_COLOR_BUFFER_BIT);
    gl.colorMask(true, true, true, true);

    QPixmap result{};
    if (pixel_buffer.isValid())
      result = QPixmap::fromImage(pixel_buffer.toImage());
    pixel_buffer.release();
    main->doneCurrent();

    if (result.isNull())
    {
      LogError << "PreviewRenderer: null render result for '" << _filename
               << "' -- skipping preview." << std::endl;
      return &(_cache[curEntry] = QPixmap());
    }

    return &(_cache[curEntry] = std::move(result));
  }
  catch (std::exception const& ex)
  {
    LogError << "PreviewRenderer: preview render failed for '" << _filename
             << "' (" << ex.what() << ") -- skipping preview (no crash)." << std::endl;
    try { main->doneCurrent(); } catch (...) {}
    return &(_cache[curEntry] = QPixmap());
  }
  catch (...)
  {
    LogError << "PreviewRenderer: preview render failed for '" << _filename
             << "' (unknown) -- skipping preview (no crash)." << std::endl;
    try { main->doneCurrent(); } catch (...) {}
    return &(_cache[curEntry] = QPixmap());
  }
}

void PreviewRenderer::setLightDirection(float y, float z)
{
  _light_dir = {1.f, 0.5f, 0.f};
  QMatrix4x4 matrix = QMatrix4x4();
  matrix.rotate(z, 0.f, 1.f, 0.f);
  matrix.rotate(y, 1.f, 0.f, 0.f);

  QVector3D light_dir = {_light_dir.x, _light_dir.y, _light_dir.z};
  light_dir = matrix * light_dir;

  _light_dir.x = light_dir.x();
  _light_dir.y = light_dir.y();
  _light_dir.z = light_dir.z();

  _lighting_needs_update = true;
}


void PreviewRenderer::update_emitters(float dt)
{
  // Advance ONLY this preview's own models -- NOT the global ModelManager (which ticks EVERY loaded model's
  // particles/animation). The main MapView already ticks the global set once per frame; when a tool's
  // preview was also open it called the global update a SECOND time, so every creature in the main scene
  // animated at ~2x speed (and got out of sync with its bones). Updating just _model_instances keeps the
  // preview animated without touching the main scene.
  for (auto& instance : _model_instances)
  {
    if (!instance.model->finishedLoading())
    {
      continue;
    }

    float remaining = dt;
    while (remaining > 0.1f)
    {
      instance.model->updateEmitters(0.1f);
      remaining -= 0.1f;
    }
    instance.model->updateEmitters(remaining);
  }
}

void PreviewRenderer::tick(float dt)
{
  dt = std::min(dt, 1.0f);

  _animtime += dt * 1000.0f;

  if (_draw_animated.get())
  {
    update_emitters(dt);
  }
}


void PreviewRenderer::upload()
{
  _buffers.upload();

  // m2

  _m2_program.reset
  (new OpenGL::program
    { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("m2_vs") }
      , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("m2_fs") }
    }
  );

  {
    OpenGL::Scoped::use_program m2_shader{ *_m2_program.get() };
    m2_shader.uniform("bone_matrices", 0);
    m2_shader.uniform("tex1", 1);
    m2_shader.uniform("tex2", 2);

    m2_shader.bind_uniform_block("matrices", 0);
    gl.bindBuffer(GL_UNIFORM_BUFFER, _mvp_ubo);
    gl.bufferData(GL_UNIFORM_BUFFER, sizeof(OpenGL::MVPUniformBlock), NULL, GL_DYNAMIC_DRAW);
    gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::MVP, _mvp_ubo, 0, sizeof(OpenGL::MVPUniformBlock));
    gl.bindBuffer(GL_UNIFORM_BUFFER, 0);

    m2_shader.bind_uniform_block("lighting", 1);
    gl.bindBuffer(GL_UNIFORM_BUFFER, _lighting_ubo);
    gl.bufferData(GL_UNIFORM_BUFFER, sizeof(OpenGL::LightingUniformBlock), NULL, GL_DYNAMIC_DRAW);
    gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::LIGHTING, _lighting_ubo, 0, sizeof(OpenGL::LightingUniformBlock));
    gl.bindBuffer(GL_UNIFORM_BUFFER, 0);
  }

  // m2 instaced
  
  _m2_instanced_program.reset
  (new OpenGL::program
    { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("m2_vs", {"instanced"}) }
        , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("m2_fs") }
    }
  );

  {
    OpenGL::Scoped::use_program m2_shader_instanced{ *_m2_instanced_program.get() };
    m2_shader_instanced.bind_uniform_block("matrices", 0);
    m2_shader_instanced.bind_uniform_block("lighting", 1);
    m2_shader_instanced.uniform("bone_matrices", 0);
    m2_shader_instanced.uniform("tex1", 1);
    m2_shader_instanced.uniform("tex2", 2);
  }
 
  // m2 box

  _m2_box_program.reset
  (new OpenGL::program
    { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("m2_box_vs") }
        , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("m2_box_fs") }
    }
  );

  {
    OpenGL::Scoped::use_program m2_box_shader{ *_m2_box_program.get() };
    m2_box_shader.bind_uniform_block("matrices", 0);
  }


  /*
  

  _m2_ribbons_program.reset
  (new OpenGL::program
    { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("ribbon_vs") }
        , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("ribbon_fs") }
    }
  );
  

  _m2_particles_program.reset
  (new OpenGL::program
    { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("particle_vs") }
        , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("particle_fs") }
    }
  );

  */

  // wmo
  
  _wmo_program.reset
  (new OpenGL::program
    { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("wmo_vs") }
        , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("wmo_fs") }
    }
  );

  {
    std::vector<int> samplers{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

    OpenGL::Scoped::use_program wmo_program{ *_wmo_program.get() };
    wmo_program.uniform("render_batches_tex", 0);
    wmo_program.uniform("texture_samplers", samplers);
    wmo_program.bind_uniform_block("matrices", 0);
    wmo_program.bind_uniform_block("lighting", 1);
  }

  // liquid
  _liquid_texture_manager.upload();
  _liquid_program.reset
    (new OpenGL::program
      { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("liquid_vs") }
          , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("liquid_fs") }
      }
    );

  {
    OpenGL::Scoped::use_program liquid_render{ *_liquid_program.get() };

    //setupLiquidChunkBuffers();
    //setupLiquidChunkVAO(liquid_render);

    static std::vector<int> samplers{ 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

    liquid_render.bind_uniform_block("matrices", 0);
    liquid_render.bind_uniform_block("lighting", 1);
    liquid_render.bind_uniform_block("liquid_layers_params", 4);
    liquid_render.uniform("vertex_data", 0);
    liquid_render.uniform("texture_samplers", samplers);

  }

  auto background_color = _settings->value("assetBrowser/background_color",
    QVariant::fromValue(QColor(127, 127, 127))).value<QColor>();

  _background_color = { static_cast<float>(background_color.redF()),
                       static_cast<float>(background_color.greenF()),
                       static_cast<float>(background_color.blueF()) };
 
  _uploaded = true;

}


void PreviewRenderer::unload()
{
  _buffers.unload();

  _m2_program.reset();
  _m2_instanced_program.reset();
  _m2_particles_program.reset();
  _m2_ribbons_program.reset();
  _m2_box_program.reset();
  _wmo_program.reset();
  _liquid_program.reset();
  _liquid_texture_manager.unload();

  _model_instances.clear();
  _wmo_instances.clear();

  _uploaded = false;

}

void PreviewRenderer::unloadOpenglData()
{
  if (_offscreen_mode)
  {
    // Offscreen previews now render (and upload) in the MAIN map-view context, so unload our GL data
    // there. If that context is gone (teardown / map-view destroyed) the GL resources die with it --
    // skip; the deferred_upload_* dtors are already teardown-guarded. ViewportManager also calls this
    // when the map-view loses its context, which resets _uploaded so we re-upload cleanly next time.
    auto* main = main_viewport();
    if (!main || !main->context() || !main->context()->isValid())
      return;  // context gone: GL resources die with it; unload() w/o a context could throw
    main->makeCurrent();
    OpenGL::context::scoped_setter const context_set (::gl, main->context());

    unload();
    main->doneCurrent();
    return;
  }

  // On return-to-menu the QOpenGLWidget viewport is being destroyed and its context() is already
  // null/invalid; makeCurrent() + scoped_setter(context()) would then null-deref in
  // versionFunctions (context.cpp:21) -> 0xc0000005. Skip the GL unload; the context is taking the
  // resources with it anyway.
  QOpenGLContext* const ctx = context();
  if (!ctx || !ctx->isValid())
    return;

  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, ctx);

  ModelManager::unload_all(_context);
  WMOManager::unload_all(_context);
  TextureManager::unload_all(_context);

  unload();
}

void Noggit::Ui::Tools::PreviewRenderer::updateLightingUniformBlock()
{

  glm::vec4 ocean_color_light(glm::vec3(1.0f, 1.0f, 1.0f), 1.f);
  glm::vec4 ocean_color_dark(glm::vec3(1.0f, 1.0f, 1.0f), 1.f);
  glm::vec4 river_color_light(glm::vec3(1.0f, 1.0f, 1.0f), 1.f);
  glm::vec4 river_color_dark(glm::vec3(1.0f, 1.0f, 1.0f), 1.f);

  _lighting_ubo_data.DiffuseColor_FogStart = { _diffuse_light.x,_diffuse_light.y,_diffuse_light.z, 0};
  _lighting_ubo_data.AmbientColor_FogEnd = { _ambient_light.x, _ambient_light.y, _ambient_light.z, 0};
  _lighting_ubo_data.FogColor_FogOn = { 0, 0, 0, 0};
  _lighting_ubo_data.LightDir_FogRate = { _light_dir.x, _light_dir.y, _light_dir.z, 1.0f};
  _lighting_ubo_data.OceanColorLight = { 1.0f, 1.0f, 1.0f, 1.0f };
  _lighting_ubo_data.OceanColorDark = { 1.0f, 1.0f, 1.0f, 1.0f };
  _lighting_ubo_data.RiverColorLight = { 1.0f, 1.0f, 1.0f, 1.0f };
  _lighting_ubo_data.RiverColorDark = { 1.0f, 1.0f, 1.0f, 1.0f };

  gl.bindBuffer(GL_UNIFORM_BUFFER, _lighting_ubo);
  gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(OpenGL::LightingUniformBlock), &_lighting_ubo_data);
  
  _lighting_needs_update = false;
}

void Noggit::Ui::Tools::PreviewRenderer::updateMVPUniformBlock(const glm::mat4x4& model_view, const glm::mat4x4& projection, const glm::vec3& camera_pos)
{
  _mvp_ubo_data.model_view = model_view;
  _mvp_ubo_data.projection = projection;
  // The M2/WMO/terrain vertex shaders render CAMERA-RELATIVE (they zero the view translation and subtract
  // camera_pos): without a real camera_pos the camera's translation is ignored, so moving the preview
  // camera did nothing and it appeared glued to the model centre. Feed the actual camera position.
  _mvp_ubo_data.camera_pos = glm::vec4(camera_pos, 1.0f);

  gl.bindBuffer(GL_UNIFORM_BUFFER, _mvp_ubo);
  gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(OpenGL::MVPUniformBlock), &_mvp_ubo_data);

}
