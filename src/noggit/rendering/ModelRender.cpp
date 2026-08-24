// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ModelRender.hpp"
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/Log.h>
#include <noggit/frame_profiler.hpp>
#include <external/tracy/Tracy.hpp>
#include <math/bounding_box.hpp>
#include <noggit/Misc.h>
#include <QtGui/QOpenGLContext>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>


using namespace Noggit::Rendering;

namespace
{
  constexpr char const* classic_character_render_trace_filename = "creature_geoset_trace_20260604.log";

  bool model_texture_debug_enabled()
  {
    static bool const enabled = []()
    {
      char const* value = std::getenv("NOGGIT_MODEL_TEXTURE_DEBUG");
      return value && *value && std::strcmp(value, "0") != 0;
    }();

    return enabled;
  }

  bool capture_debug_enabled()
  {
    static bool const enabled = []()
    {
      char const* value = std::getenv("NOGGIT_CAPTURE_DEBUG");
      return value && *value && std::strcmp(value, "0") != 0;
    }();

    return enabled;
  }

  bool is_classic_creature_or_character_model(Model const* model)
  {
    if (!model || !model->usesClassicLayout() || !model->file_key().hasFilepath())
    {
      return false;
    }

    auto const& path = model->file_key().filepath();
    return path.starts_with("creature/") || path.starts_with("character/");
  }

  bool is_classic_character_model(Model const* model)
  {
    if (!model || !model->usesClassicLayout() || !model->file_key().hasFilepath())
    {
      return false;
    }

    return model->file_key().filepath().starts_with("character/");
  }

  bool should_log_model_render_passes(Model const* model)
  {
    if (!capture_debug_enabled() || !model || !model->file_key().hasFilepath())
    {
      return false;
    }

    auto const& path = model->file_key().filepath();
    return capture_debug_enabled()
        && (path.find("darkironnode") != std::string::npos
            || path.find("elementalearth") != std::string::npos
            || path.find("firelord") != std::string::npos);
  }

  void append_classic_character_render_trace(std::string const& message)
  {
    if (!model_texture_debug_enabled())
    {
      return;
    }

    std::ofstream trace(classic_character_render_trace_filename, std::ios::app);
    if (!trace.is_open())
    {
      return;
    }

    trace << "render " << message << '\n';
  }

  void log_classic_character_controlled_geoset_decision(Model const* model,
                                                        ModelInstance const* instance,
                                                        std::uint16_t geoset_id,
                                                        char const* decision)
  {
    if (!model_texture_debug_enabled() || !is_classic_character_model(model) || !instance)
    {
      return;
    }

    auto const geoset_family = static_cast<std::uint16_t>(geoset_id / 100);
    bool const family_controlled = instance->isGeosetFamilyControlled(geoset_family);
    bool const geoset_visible = instance->isGeosetIdVisible(geoset_id);
    if (geoset_family < 4 || geoset_family > 15)
    {
      return;
    }

    bool const lower_body_family = geoset_family >= 10 && geoset_family <= 13;

    static std::set<std::string> logged_draws;

    std::ostringstream key;
    key << model->file_key().stringRepr() << '|'
        << instance->uid << '|'
        << decision << '|'
        << geoset_id << '|';
    for (auto family : instance->controlledGeosetFamilies())
    {
      key << family << ',';
    }
    key << '|';
    for (auto visible : instance->visibleGeosetIds())
    {
      key << visible << ',';
    }

    if (!logged_draws.insert(key.str()).second)
    {
      return;
    }

    std::ostringstream log_line;
    log_line << "Classic character controlled geoset " << decision
             << " model='" << model->file_key().stringRepr()
             << "' uid=" << instance->uid
             << " geosetId=" << geoset_id
             << " family=" << geoset_family
             << " controlled=" << (family_controlled ? 1 : 0)
             << " visible=" << (geoset_visible ? 1 : 0)
             << " controlledFamilies=[";
    for (std::size_t index = 0; index < instance->controlledGeosetFamilies().size(); ++index)
    {
      if (index != 0)
      {
        log_line << ", ";
      }
      log_line << instance->controlledGeosetFamilies()[index];
    }
    log_line << "] visibleGeosets=[";
    for (std::size_t index = 0; index < instance->visibleGeosetIds().size(); ++index)
    {
      if (index != 0)
      {
        log_line << ", ";
      }
      log_line << instance->visibleGeosetIds()[index];
    }
    log_line << "]";
    LogDebug << log_line.str() << std::endl;

    if (lower_body_family)
    {
      append_classic_character_render_trace(log_line.str());
    }
  }

  bool is_masked_lightray_model(Model const* model)
  {
    return model
      && model->file_key().hasFilepath()
      && model->file_key().filepath() == "world/nodxt/generic/passivedoodads/volumetriclights/lightray_dusty_01.m2";
  }

  bool uses_masked_lightray_additive(Model const* model, ModelRenderPass const& pass, uint16_t blend_mode)
  {
    return is_masked_lightray_model(model)
      && blend_mode == static_cast<uint16_t>(M2Blend::Add)
      && pass.texture_count == 2;
  }

  bool is_classic_effect_shell_model(Model const* model)
  {
    if (!model || !model->usesClassicLayout() || !model->file_key().hasFilepath())
    {
      return false;
    }

    auto const& path = model->file_key().filepath();
    if (path.starts_with("world/generic/passivedoodads/particleemitters/"))
    {
      return true;
    }

    return path == "world/khazmodan/ironforge/passivedoodads/lavasteam/lavasteam.m2"
        || path == "world/khazmodan/ironforge/passivedoodads/lavasteam/lavasteam_low.m2";
  }

  ModelPixelShader classic_default_pixel_shader_for_blend(std::uint16_t blend_mode)
  {
    switch (static_cast<M2Blend>(blend_mode))
    {
      case M2Blend::Opaque:
        return ModelPixelShader::Combiners_Opaque;
      case M2Blend::Mod2x:
        return ModelPixelShader::Combiners_Mod2x;
      case M2Blend::Alpha_Key:
      case M2Blend::Alpha:
      case M2Blend::No_Add_Alpha:
      case M2Blend::Add:
      case M2Blend::Mod:
      default:
        return ModelPixelShader::Combiners_Mod;
    }
  }
}

ModelRender::ModelRender(Model* model)
: _model(model)
{

}

ModelRender::~ModelRender()
{
  // Leak fix (2026-07-22): model eviction destroys the Model via AsyncObjectMultimap::erase, which does
  // NOT call unload() -- so the bone-matrix texture buffer, the ONLY ModelRender GL object stored as a raw
  // GLuint (every other one lives in the RAII _buffers / _vertex_arrays wrappers), leaked once per animated
  // model on churn until VRAM/texture handles were exhausted. Free it here. Same teardown-safe guard the
  // deferred_upload_* member destructors use: only touch GL with a live context, and never let the delete
  // throw out of a destructor (a throwing dtor -> std::terminate; teardown can run with no/mismatched
  // context, e.g. return-to-menu). Non-animated models never generated it (0), so this is a no-op for them.
  if (_bone_matrices_buf_tex && QOpenGLContext::currentContext())
  {
    try { gl.deleteTextures(1, &_bone_matrices_buf_tex); } catch (...) {}
    _bone_matrices_buf_tex = 0;
  }
}

void ModelRender::upload()
{
  noggit::perf::Scoped _prof_up(noggit::perf::Phase::ModelUpload); // M2 spike hunt: model VBO/index upload
  _vertex_box_points = math::box_points(
      misc::transform_model_box_coords(_model->header.bounding_box_min)
      , misc::transform_model_box_coords(_model->header.bounding_box_max));

  for (std::string const& texture : _model->_textureFilenames)
    _model->_textures.emplace_back(texture, _model->_context);

  _buffers.upload();
  _vertex_arrays.upload();
  // Context-loss guard (crash-hunt 2026-07-08): during a map switch, upload() can run while no usable
  // GL context is current -- glGenBuffers then yields 0 names and every bind after is
  // GL_INVALID_OPERATION, feeding the NVIDIA driver poisoned commands (the sporadic nvoglv64 AV
  // dumps). If generation failed, bail WITHOUT setting _uploaded so the next frame (with a proper
  // context) retries cleanly.
  if (_buffers[0] == 0 || _vertex_arrays[0] == 0)
  {
    static int s_ctx_guard_logs = 0;
    if (s_ctx_guard_logs < 5)
    {
      ++s_ctx_guard_logs;
      LogError << "ModelRender::upload aborted: GL name generation failed (no current context?) model='"
               << _model->file_key().stringRepr() << "' -- retrying next frame" << std::endl;
    }
    _buffers.unload();
    _vertex_arrays.unload();
    return;
  }
  _bone_matrices_buf_tex = 0;
  _bone_matrices_buffer_size = 0;

  if (_model->animBones)
  {
    gl.genTextures(1, &_bone_matrices_buf_tex);

    gl.bindTexture(GL_TEXTURE_BUFFER, _bone_matrices_buf_tex);
    OpenGL::Scoped::buffer_binder<GL_TEXTURE_BUFFER> const binder(_bone_matrices_buffer);
    _bone_matrices_buffer_size = _model->bone_matrices.size() * sizeof(glm::mat4x4);
    gl.bufferData(GL_TEXTURE_BUFFER, _bone_matrices_buffer_size, nullptr, GL_STREAM_DRAW);
    gl.texBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, _bone_matrices_buffer);
  }

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder(_vertices_buffer);
    gl.bufferData(GL_ARRAY_BUFFER, _model->_vertices.size() * sizeof(ModelVertex), _model->_vertices.data(), GL_STATIC_DRAW);
  }

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder (_box_vbo);
    gl.bufferData (GL_ARRAY_BUFFER, _vertex_box_points.size() * sizeof (glm::vec3), _vertex_box_points.data(), GL_STATIC_DRAW);
  }

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_indices_buffer);
  gl.bufferData (GL_ELEMENT_ARRAY_BUFFER, _model->_indices.size() * sizeof(uint16_t), _model->_indices.data(), GL_STATIC_DRAW);

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> box_indices_binder(_box_indices_buffer);
  gl.bufferData (GL_ELEMENT_ARRAY_BUFFER, _box_indices.size() * sizeof(uint16_t), _box_indices.data(), GL_STATIC_DRAW);

  _model->_textureFilenames.clear();

  _uploaded = true;
}

void ModelRender::unload()
{
  _model->_textures.clear();
  _buffers.unload();
  _vertex_arrays.unload();

  if (_bone_matrices_buf_tex)
  {
    gl.deleteTextures(1, &_bone_matrices_buf_tex);
    _bone_matrices_buf_tex = 0; // clear the stale name so a re-upload / dtor can't double-target it
  }

  for (auto& particle : _model->_particles)
  {
    particle.unload();
  }

  for (auto& ribbon : _model->_ribbons)
  {
    ribbon.unload();
  }

  _uploaded = false;
  _vao_setup = false;
}

void ModelRender::draw(glm::mat4x4 const& model_view
    , ModelInstance& instance
    , OpenGL::Scoped::use_program& m2_shader
    , OpenGL::M2RenderState& model_render_state
    , math::frustum const& frustum
    , const float& cull_distance
    , const glm::vec3& camera
    , int animtime
    , display_mode display
    , bool no_cull
    , bool bloom_mask_only
    , glm::vec4 const& interior_light
    , float dist_fade
    , bool skip_animate
)
{
  if (!_model->finishedLoading() || _model->loading_failed())
  {
    return;
  }

  bool const skip_mesh_passes = is_classic_effect_shell_model(_model);

  if (!no_cull && !instance.isInFrustum(frustum) && !instance.isInRenderDist(cull_distance, camera, display))
  {
    return;
  }

  if (!_uploaded)
  {
    upload();
  }

  if (!skip_animate && _model->animated && (!_model->animcalc || _model->_per_instance_animation))
  {
    int anim_id = instance.forcedAnimationId() >= 0 ? instance.forcedAnimationId() : 0;
    // A forced animation may not exist on this model. Fall back to Stand(0) so animate() never indexes a
    // missing sequence.
    if (anim_id != 0 && !_model->hasAnimationId(anim_id))
    {
      anim_id = 0;
    }
    // Weapon grip: this instance holds an in-hand weapon -> after the body pass, overlay the HandsClosed
    // pose onto the finger bones only (see Model::applyHandGripOverlay). Set on the shared model just
    // before this instance's animate() -- creatures re-animate per draw, so it applies to this spawn only.
    _model->_hand_overlay_active_main = instance.closeHandMain();
    _model->_hand_overlay_active_off = instance.closeHandOff();
    _model->_lower_body_twist = instance.lower_body_twist;
    _model->_anim_time_scale = instance.anim_time_scale;
    // Identify which spawn is animating so the idle-variation scheduler keeps a per-instance timeline
    // (each spawn leans on its own schedule instead of all in unison). uid is the spawn guid for creatures.
    _model->_active_idle_key = static_cast<std::uint64_t>(instance.uid);
    // Feed the FULL model->view matrix (camera view * this instance's world transform) so billboard bones
    // face the camera in this spawn's own frame. model_view alone is world->view; the instance's world
    // orientation (e.g. Anomalus's 205deg heading) is applied separately in the shader via `transform`, so
    // billboarding with only the view matrix left the Purple_Glow card rotated by the spawn's heading.
    // calcMatrix uses model_view ONLY for the billboard basis, so this affects nothing else. This is a
    // single-instance draw (creature spawns re-animate per draw), so per-instance billboards are correct.
    {
      noggit::perf::Scoped _prof_anim(noggit::perf::Phase::AnimateCPU);
      _model->animate(model_view * instance.transformMatrix(), anim_id, animtime);
    }
    _model->animcalc = true;
  }
  else if (skip_animate && _model->animated && !_model->bone_matrices.empty())
  {
    // Bones were computed for this instance on a worker thread (creature parallel-animate pre-pass) and the
    // caller has already restored them into _model->bone_matrices. Just upload the TBO here (GL, main
    // thread) -- no recompute. animcalc stays as-is; the caller owns per-instance bone state this frame.
    updateBoneMatrices();
  }

  OpenGL::Scoped::vao_binder const _(_vao);

  if (skip_mesh_passes)
  {
    return;
  }

  // Camera-relative anchor: split the world transform into a ~17000 origin + a small relative transform
  // so the shader keeps everything small (jitter fix). Attachments supply a double-computed anchor;
  // everything else derives it from its (static) world position here.
  {
    glm::mat4x4 tr;
    glm::vec3 origin;
    if (instance._has_render_anchor)
    {
      origin = instance._render_origin;
      tr = instance._render_transform_rel;
    }
    else
    {
      tr = instance.transformMatrix();
      origin = glm::vec3(tr[3]);
      tr[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    m2_shader.uniform("transform", tr);
    m2_shader.uniform("model_origin", origin);
    // Always set so no stale interior value leaks from a previous indoor draw. (0,0,0,0) = outdoor.
    m2_shader.uniform("instance_interior", interior_light);
  }

  // Only bind the bone-matrix samplerBuffer when it actually has storage AND matrices. Sampling a
  // ZERO-SIZE texture buffer is undefined and the NVIDIA driver __fastfails on it -- which is the
  // asset-browser preview crash: upload() sizes this buffer from bone_matrices at upload time (0 if
  // bones aren't computed yet), and if animate()/updateBoneMatrices then doesn't run before the draw
  // (animcalc already set), the buffer stays 0-size while the draw binds+samples it. Fall back to the
  // static (unlit-bones) path in that case instead of feeding the driver an empty buffer.
  if (_model->animBones && _bone_matrices_buf_tex != 0 && _bone_matrices_buffer_size > 0
      && !_model->bone_matrices.empty())
  {
    gl.activeTexture(GL_TEXTURE0);
    gl.bindTexture(GL_TEXTURE_BUFFER, _bone_matrices_buf_tex);
    m2_shader.uniform("anim_bones", true);
    m2_shader.uniform("bone_matrix_count", static_cast<int>(_model->bone_matrices.size()));
  }
  else
  {
    m2_shader.uniform("anim_bones", false);
    m2_shader.uniform("bone_matrix_count", 0);
  }

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder(_vertices_buffer);
    m2_shader.attrib("pos", 3, GL_FLOAT, GL_FALSE, sizeof(ModelVertex), 0);
    m2_shader.attribi("bones_weight",  4, GL_UNSIGNED_BYTE, sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3)));
    m2_shader.attribi("bones_indices", 4, GL_UNSIGNED_BYTE, sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3) + 4));
    m2_shader.attrib("normal", 3, GL_FLOAT, GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*> (sizeof(::glm::vec3) + 8));
    m2_shader.attrib("texcoord1", 2, GL_FLOAT, GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*> (sizeof(::glm::vec3) * 2 + 8));
    m2_shader.attrib("texcoord2", 2, GL_FLOAT, GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*> (sizeof(::glm::vec3) * 2 + 8 + sizeof(glm::vec2)));
  }

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_indices_buffer);

  // BLOOM-MASK-ONLY re-stamp (called after the particle pass, which erases the emissive mask under
  // every particle -- so a smoke cloud from another creature was killing this creature's bloom). Draw
  // the depth-writing body batches into the ALPHA CHANNEL only, over the depth this creature already
  // laid, stamping the brightness-driven emissive mask (shader creature_bloom == 3) so the bloom
  // survives whatever drew in front. No colour, no blend, no depth writes.
  if (bloom_mask_only)
  {
    gl.colorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    gl.disable(GL_BLEND);
    gl.depthMask(GL_FALSE);
    model_render_state.bloom_mask_pass = true;
    model_render_state.creature_bloom = -1;
    for (ModelRenderPass& p : _render_passes)
    {
      if (p.blend_mode > 1)
      {
        continue;
      }
      if (p.prepareDraw(m2_shader, _model, &instance, model_render_state, dist_fade))
      {
        gl.disable(GL_BLEND);
        gl.drawElements(GL_TRIANGLES, p.index_count, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(p.index_start * sizeof(GLushort)));
        p.afterDraw();
      }
    }
    model_render_state.bloom_mask_pass = false;
    model_render_state.creature_bloom = -1;
    model_render_state.blend = 0xFFFF;
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    return;
  }

  // TRANSLUCENT CREATURE (CreatureModelAlpha < 255, e.g. Anomalus 200 -> 0.784): the live client
  // draws the body geometry TWICE (verified in the apitrace): first a DEPTH-ONLY prepass (color
  // writes off), then the same batches again with ALPHABLENDENABLE=TRUE -- so the creature reads
  // translucent against the BACKGROUND while the prepass depth keeps far-side/internal geometry from
  // ghosting through. prepareDraw promotes opaque/alpha-key batches to alpha blending for such
  // instances; here we lay the depth prepass first.
  // Gate on the COMBINED alpha (CreatureModelAlpha x cull fade): prepareDraw promotes batches to
  // alpha blending off the same product, and a promoted body WITHOUT the prepass ghosts its
  // internal geometry -- the "shading changes while fading" bug.
  if (instance.model_alpha * dist_fade < 0.999f)
  {
    gl.colorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    for (ModelRenderPass& p : _render_passes)
    {
      if (p.blend_mode > 1)
      {
        continue; // only depth-writing batches (opaque / alpha-key) participate in the prepass
      }
      if (p.prepareDraw(m2_shader, _model, &instance, model_render_state, dist_fade))
      {
        gl.drawElements(GL_TRIANGLES, p.index_count, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(p.index_start * sizeof(GLushort)));
        p.afterDraw();
      }
    }
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  }

  // CANON per-frame transparency sort (WoW.exe 5875 -- CM2Scene draw FUN_00707680 builds+buckets+sorts the
  // batches; transparent-bucket comparator FUN_0070ae10; RE'd 2026-07-18, [[twmoa-m2-transparency-sort]]):
  // the client re-sorts a model's TRANSPARENT batches EVERY FRAME back-to-front by the batch sort-centre's
  // SQUARED CAMERA DISTANCE (comparator primary key), with priorityPlane only a #3 tiebreaker; the opaque /
  // alpha-key depth-writers draw first in their own state-sorted bucket. noggit's static _render_passes sort
  // (ModelRenderPass::operator<) is priorityPlane-primary and view-independent -- right for opaque, but it
  // stacks a model's alpha-blended layers in a fixed order that reads wrong from angles where depth != the
  // authored priority. So per THIS instance THIS frame: draw depth-writers (blend<=1) first in static order,
  // then the transparent batches, grouped by blend (the client buckets transparent by render category --
  // exact criterion not fully traced, blend_mode is the proxy) and back-to-front within each group. Only the
  // single-instance path (creatures / individually-drawn doodads); the instanced doodad overload keeps the
  // static order for the instancing perf win, and additive blends commute, so only multi-alpha-blend models
  // actually change. Gated to >=2 transparent batches, so the overwhelmingly common 0/1-transparent model
  // keeps its exact prior order.
  {
    static thread_local std::vector<std::size_t> draw_order;
    draw_order.clear();

    std::size_t transparent_pass_count = 0;
    for (ModelRenderPass const& p : _render_passes)
    {
      if (p.blend_mode > 1)
      {
        ++transparent_pass_count;
      }
    }

    // depth-writers first (and, in the fast <2-transparent case, EVERY pass in exact static order)
    for (std::size_t i = 0; i < _render_passes.size(); ++i)
    {
      if (transparent_pass_count < 2 || _render_passes[i].blend_mode <= 1)
      {
        draw_order.push_back(i);
      }
    }

    if (transparent_pass_count >= 2)
    {
      std::size_t const transparent_begin = draw_order.size();
      for (std::size_t i = 0; i < _render_passes.size(); ++i)
      {
        if (_render_passes[i].blend_mode > 1)
        {
          draw_order.push_back(i);
        }
      }

      glm::mat4x4 const inst_mat = instance.transformMatrix();
      auto const dist2 = [&](std::size_t idx)
      {
        glm::vec3 const world = glm::vec3(inst_mat * glm::vec4(_render_passes[idx].sort_center, 1.0f));
        glm::vec3 const to_cam = camera - world;
        return glm::dot(to_cam, to_cam);
      };

      std::stable_sort(draw_order.begin() + transparent_begin, draw_order.end(),
        [&](std::size_t a, std::size_t b)
        {
          if (_render_passes[a].blend_mode != _render_passes[b].blend_mode)
          {
            return _render_passes[a].blend_mode < _render_passes[b].blend_mode; // keep the blend/category grouping
          }
          float const da = dist2(a);
          float const db = dist2(b);
          if (da != db)
          {
            return da > db; // back-to-front -- the client's primary transparent key
          }
          return _render_passes[a].priority_plane < _render_passes[b].priority_plane; // priorityPlane tiebreaker
        });
    }

    {
      // [CRE-PROFILE 2026-08-18 TEMP] split the single-instance creature/doodad pass work (prepareDraw =
      // per-pass uniform sets + bindTexture; drawElements) into the idle M2Submit phase, to confirm whether
      // the individual creature path's cost is the per-pass CPU state churn (-> batching collapses it).
      noggit::perf::Scoped _prof_cre_pass(noggit::perf::Phase::M2Submit);
      for (std::size_t idx : draw_order)
      {
        ModelRenderPass& p = _render_passes[idx];
        if (p.prepareDraw(m2_shader, _model, &instance, model_render_state, dist_fade))
        {
          gl.drawElements(GL_TRIANGLES, p.index_count, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(p.index_start * sizeof(GLushort)));
          p.afterDraw();
        }
      }
    }
  }

  // Translucent energy creature (Anomalus): the alpha-blended body above lands the FBO's bloom mask
  // at ~alpha^2 -- far below the emissive range -- so its bright texels never bloomed, while the client's
  // screen-luminance FFXGlow blooms them. Re-draw the depth-writing batches into the ALPHA CHANNEL ONLY
  // (color writes off, blending off, depth already laid by the prepass): the shader (creature_bloom == 3)
  // stamps a brightness-driven emissive mask for hot texels and discards the rest. The body's blended
  // COLOR -- and its see-through translucency -- is untouched.
  if (instance.model_alpha * dist_fade < 0.999f && is_classic_creature_or_character_model(_model))
  {
    gl.colorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    gl.disable(GL_BLEND);
    gl.depthMask(GL_FALSE);
    model_render_state.bloom_mask_pass = true;
    model_render_state.creature_bloom = -1; // force the uniform to re-evaluate as mode 3
    for (ModelRenderPass& p : _render_passes)
    {
      if (p.blend_mode > 1)
      {
        continue; // body batches only (opaque / alpha-key), same set as the depth prepass
      }
      if (p.prepareDraw(m2_shader, _model, &instance, model_render_state, dist_fade))
      {
        gl.disable(GL_BLEND); // prepareDraw may have re-enabled blending for the promoted pass
        gl.drawElements(GL_TRIANGLES, p.index_count, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(p.index_start * sizeof(GLushort)));
        p.afterDraw();
      }
    }
    model_render_state.bloom_mask_pass = false;
    model_render_state.creature_bloom = -1; // next model re-evaluates its own mode
    model_render_state.blend = 0xFFFF;      // blend state was forced off; make the next pass re-set it
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  }

  gl.disable(GL_BLEND);
  gl.enable(GL_CULL_FACE);
  gl.depthMask(GL_TRUE);
  // [A2C source fix 2026-08-20] If the LAST pass drawn in this scope was a ground-clutter detail doodad,
  // prepareDraw left GL_SAMPLE_ALPHA_TO_COVERAGE enabled -- and every draw path that does NOT run
  // prepareDraw (the MDI-batched doodads = most world models) then rendered with screen-door DITHER at
  // its alpha edges ("most models dither, some worse"). Clear it at the source, like the blend reset,
  // and desync the cache so the next clutter pass re-applies it.
  gl.disable(GL_SAMPLE_ALPHA_TO_COVERAGE);

  // These GL resets change state that prepareDraw caches in model_render_state, which is SHARED
  // across every model in the batch. Sync the cache to what we just forced, so the next model's first
  // pass doesn't skip re-applying a state we changed out from under it (e.g. an additive-first effect
  // model rendering with blend left disabled). blend = 0xFFFF is an invalid sentinel that forces the
  // next pass to re-issue both the GL blend func and the blend_mode uniform.
  model_render_state.blend = 0xFFFF;
  model_render_state.backface_cull = true;
  model_render_state.z_buffered = false;
  model_render_state.detail_doodad = -1;
}

void ModelRender::draw(glm::mat4x4 const& model_view
    , std::vector<glm::mat4x4> const& instances
    , OpenGL::Scoped::use_program& m2_shader
    , OpenGL::M2RenderState& model_render_state
    , math::frustum const& frustum
    , const float& cull_distance
    , const glm::vec3& camera
    , int animtime
    , bool all_boxes
    , std::unordered_map<Model*, std::size_t>& model_boxes_to_draw
    , display_mode display
    , bool no_cull
    , ModelInstance const* representative
    , std::vector<glm::vec4> const& instance_interior
    , std::vector<float> const& instance_fades
    , std::vector<glm::mat4x4> const& per_instance_bones
)
{
  ZoneScopedN(NOGGIT_CURRENT_FUNCTION);
  bool const skip_mesh_passes = is_classic_effect_shell_model(_model);
  // Per-instance bone slices (perf 2026-07-20): non-empty => each instance owns bone_matrices.size()
  // matrices in per_instance_bones (slice i at [i*count,(i+1)*count)), so billboard doodads draw
  // INSTANCED (one call per interior group) instead of one draw per doodad. Skips the single shared
  // animate() (the caller pre-animated each instance into this buffer) and strides the bone reads.
  bool const pib = !per_instance_bones.empty();
  int const pib_bone_count = pib ? static_cast<int>(_model->bone_matrices.size()) : 0;

  {
    ZoneScopedN("Model::draw() : uploads")

    if (capture_debug_enabled())
    {
      LogDebug << "ModelRender::draw instanced uploads begin model='"
               << _model->file_key().stringRepr()
               << "' uploaded=" << _uploaded
               << " vaoSetup=" << _vao_setup
               << " instances=" << instances.size()
               << std::endl;
    }

    if (!_model->finishedLoading() || _model->loading_failed())
    {
      return;
    }

    if (!_uploaded)
    {
      upload();
    }

    if (!_vao_setup)
    {
      setupVAO(m2_shader);
    }
    else
    {
      // The VAO may have been first set up under the NON-instanced m2 program (the minimap render and the
      // individual per-instance-doodad / creature paths all bind _m2_program, where "transform" is a UNIFORM,
      // so setupVAO's attrib("transform") no-ops). An instanced draw on that VAO then reads a garbage
      // per-instance transform => inverted / misplaced meshes (the instanced billboard-doodad regression in
      // e.g. Timbermaw). Re-assert the divisor-1 transform attribute under THIS (instanced) program; it's a
      // harmless no-op for VAOs already set up instanced (the models_to_draw tree/prop buckets).
      OpenGL::Scoped::vao_binder const _v(_vao);
      {
        OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const _tb(_transform_buffer);
        m2_shader.attrib("transform", 0, 1);
      }
      // Re-assert the per-instance interior attribute for the same reason (the individual path may have
      // left the shared VAO pointing pos/normal at _vertices_buffer; interior stays bound to _interior_buffer).
      {
        OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const _ib(_interior_buffer);
        m2_shader.attrib("interior", 4, GL_FLOAT, GL_FALSE, sizeof(::glm::vec4), nullptr);
        m2_shader.attrib_divisor("interior", 1, 1);
      }
    }

    if (capture_debug_enabled())
    {
      LogDebug << "ModelRender::draw instanced uploads end model='"
               << _model->file_key().stringRepr()
               << "'" << std::endl;
    }
  }

  if (instances.empty())
  {
    return;
  }

  {
    ZoneScopedN("Model::draw() : drawing")

    // pib: the caller already animated each instance (per-instance billboards) into per_instance_bones,
    // so skip the single shared animate() that would otherwise stomp one pose over all instances.
    if (!pib && _model->animated && (!_model->animcalc || _model->_per_instance_animation))
    {
      if (capture_debug_enabled())
      {
        LogDebug << "ModelRender::draw instanced animate begin model='"
                 << _model->file_key().stringRepr()
                 << "' animcalc=" << _model->animcalc
                 << " perInstance=" << _model->_per_instance_animation
                 << " bones=" << _model->bones.size()
                 << " boneMatrices=" << _model->bone_matrices.size()
                 << " animtime=" << animtime
                 << std::endl;
      }

      _model->animate(model_view, 0, animtime);
      _model->animcalc = true;

      if (capture_debug_enabled())
      {
        LogDebug << "ModelRender::draw instanced animate end model='"
                 << _model->file_key().stringRepr()
                 << "'" << std::endl;
      }
    }

    // store the model count to draw the bounding boxes later
    if (all_boxes || _model->_hidden)
    {
      model_boxes_to_draw.emplace(_model, instances.size());
    }

    /*
    if (draw_particles && (!_particles.empty() || !_ribbons.empty()))
    {
      models_with_particles.emplace(this, n_visible_instances);
    }
     */

    OpenGL::Scoped::vao_binder const _ (_vao);

    if (capture_debug_enabled())
    {
      LogDebug << "ModelRender::draw instanced gpu begin model='"
               << _model->file_key().stringRepr()
               << "' passes=" << _render_passes.size()
               << std::endl;
    }

    // Partition the instances by (interior-light value, quantized distance fade) into contiguous
    // groups so each group can be drawn with single `instance_interior` + fade uniforms. Common case:
    // all-exterior, all-full-alpha -> one group == the old single draw. Fade is quantized to 1/64
    // (~31 ms per step of the 2 s fade -- imperceptible; the old 1/16 stepped visibly on fading
    // GAMEOBJECTS next to smoothly-fading individual creatures). Group count stays bounded: only
    // instances actually mid-fade split off extra sub-draws.
    // [perf 2026-08-05] Interior is now carried PER INSTANCE (interiors[], uploaded to _interior_buffer as a
    // divisor-1 vertex attribute) instead of a per-group uniform, so mixed-interior instances batch in ONE
    // draw. Grouping is therefore by FADE ONLY (fade still drives per-group GL blend promotion + the depth
    // prepass -- it can't move to an attribute without per-instance blend state). This collapsed WMO-doodad
    // buckets in a city from ~6 sub-draws (one per distinct room colour) to 1.
    struct InteriorGroup { float fade; std::vector<glm::mat4x4> transforms; std::vector<glm::vec4> interiors; std::vector<glm::mat4x4> bones; };
    std::vector<InteriorGroup> interior_groups;
    for (std::size_t i = 0; i < instances.size(); ++i)
    {
      glm::vec4 const inter = (i < instance_interior.size()) ? instance_interior[i] : glm::vec4(0.f);
      float const fade_raw = (i < instance_fades.size()) ? instance_fades[i] : 1.0f;
      // [PERF 2026-07-25] Quantize the cull-fade to 1/8 (was 1/64) for GROUPING ONLY. Instances are batched
      // into one instanced draw per distinct (interior, fade) value; ground clutter (grass/flowers, 2000+
      // instances) at the distance-fade ring was splitting into ~48 tiny groups by exact fade -> ~48
      // bufferData+draw(+depth-prepass) calls per model, the dominant SubmitInst cost. 1/8 collapses the ring
      // to ~8 alpha steps (imperceptible on distant fogged clutter) and ~5x fewer draws. The opaque bulk
      // (fade==1, most instances) is one draw either way.
      // 1/16 steps (was 1/8): finer alpha quantization so the widened grass fade band reads as a
      // smooth ease-in rather than visible 12.5% steps. Still collapses the fade ring to few draws.
      float fade = std::round(std::clamp(fade_raw, 0.0f, 1.0f) * 16.0f) / 16.0f;
      if (fade <= 0.0f)
      {
        // hold the faintest visible step until the raw alpha is truly imperceptible, so the final
        // drop happens below ~0.2% alpha instead of at the quantization floor (visible against fog)
        if (fade_raw < 0.002f)
        {
          continue; // fully faded out -- beyond the render distance band
        }
        fade = 1.0f / 128.0f;
      }
      InteriorGroup* grp = nullptr;
      for (auto& cand : interior_groups)
      {
        if (cand.fade == fade) { grp = &cand; break; }
      }
      if (!grp) { interior_groups.push_back({fade, {}, {}, {}}); grp = &interior_groups.back(); }
      grp->transforms.push_back(instances[i]);
      grp->interiors.push_back(inter); // per-instance room light (uploaded to _interior_buffer below)
      if (pib && pib_bone_count > 0)
      {
        // Gather THIS instance's bone slice into the group (parallel to transforms) so the group's bones
        // are laid out group-local -> gl_InstanceID indexes them directly (no base offset needed).
        auto const s = static_cast<std::size_t>(i) * static_cast<std::size_t>(pib_bone_count);
        if (s + pib_bone_count <= per_instance_bones.size())
        {
          grp->bones.insert(grp->bones.end(),
                            per_instance_bones.begin() + s,
                            per_instance_bones.begin() + s + pib_bone_count);
        }
      }
    }

    // NOGGIT_FADE_DEBUG=1: report per-model fade group makeup (find models whose fades never move)
    static bool const s_fade_dbg = std::getenv("NOGGIT_FADE_DEBUG") != nullptr;
    if (s_fade_dbg && !interior_groups.empty())
    {
      float minf = 2.0f, maxf = -1.0f;
      for (auto const& g : interior_groups) { minf = std::min(minf, g.fade); maxf = std::max(maxf, g.fade); }
      if (minf < 0.999f)
      {
        static std::set<std::string> logged;
        auto const& fp = _model->file_key().filepath();
        if (logged.insert(fp).second)
        {
          LogError << "FADEDBG model='" << fp << "' groups=" << interior_groups.size()
                   << " fades=" << minf << ".." << maxf
                   << " instances=" << instances.size()
                   << " fadesVec=" << instance_fades.size() << std::endl;
        }
      }
    }

    if (skip_mesh_passes)
    {
      // Nothing to rasterize, but the transform buffer still feeds particle/ribbon draws below.
      OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder (_transform_buffer);
      gl.bufferData(GL_ARRAY_BUFFER, instances.size() * sizeof(::glm::mat4x4), instances.data(), GL_DYNAMIC_DRAW);
      return;
    }

    // Guard as above: never bind an empty bone-matrix texture buffer (NVIDIA __fastfail -- the
    // asset-browser preview barrel crash).
    if (pib)
    {
      // Per-instance bone slices: the actual bones are uploaded per interior group below (group-local).
      // Here just declare the mode -- bones ON, per-instance count, and stride = count so instance k in
      // a group reads its slice at k*count. (The stride uniforms are set on EVERY path so a prior pib
      // draw never leaks its stride into the normal shared-bone instanced draws.)
      m2_shader.uniform("anim_bones", pib_bone_count > 0);
      m2_shader.uniform("bone_matrix_count", pib_bone_count);
      m2_shader.uniform("per_instance_bone_stride", pib_bone_count);
    }
    else if (_model->animBones && _bone_matrices_buf_tex != 0 && _bone_matrices_buffer_size > 0
        && !_model->bone_matrices.empty())
    {
      gl.activeTexture(GL_TEXTURE0);
      gl.bindTexture(GL_TEXTURE_BUFFER, _bone_matrices_buf_tex);
      m2_shader.uniform("anim_bones", true);
      m2_shader.uniform("bone_matrix_count", static_cast<int>(_model->bone_matrices.size()));
      m2_shader.uniform("per_instance_bone_stride", 0);
    }
    else
    {
      m2_shader.uniform("anim_bones", false);
      m2_shader.uniform("bone_matrix_count", 0);
      m2_shader.uniform("per_instance_bone_stride", 0);
    }

    OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_indices_buffer);

    static bool const s_inst_dbg = std::getenv("NOGGIT_INSTANCE_DEBUG") != nullptr;
    int passes_drawn = 0;
    int passes_total = 0;
    int inst_drawcalls_local = 0; // [SubmitInst breakdown] total drawElementsInstanced (prepass + main) this model

    for (auto const& group : interior_groups)
    {
      if (group.transforms.empty())
      {
        continue;
      }

      {
        // Upload only this group's transforms so index 0 is the group start (no glDraw*BaseInstance in
        // GL 3.3). The per-instance interior room light rides alongside in _interior_buffer (same order).
        OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder (_transform_buffer);
        gl.bufferData(GL_ARRAY_BUFFER, group.transforms.size() * sizeof(::glm::mat4x4), group.transforms.data(), GL_DYNAMIC_DRAW);
      }
      {
        OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const interior_binder (_interior_buffer);
        gl.bufferData(GL_ARRAY_BUFFER, group.interiors.size() * sizeof(::glm::vec4), group.interiors.data(), GL_DYNAMIC_DRAW);
      }
      if (pib && pib_bone_count > 0 && _bone_matrices_buf_tex != 0 && !group.bones.empty())
      {
        // Upload THIS group's bones group-local (slice 0 = the group's first instance) so gl_InstanceID
        // indexes the slice directly, matching the group-local transform upload above.
        OpenGL::Scoped::buffer_binder<GL_TEXTURE_BUFFER> const bone_binder (_bone_matrices_buffer);
        gl.bufferData(GL_TEXTURE_BUFFER, group.bones.size() * sizeof(::glm::mat4x4), group.bones.data(), GL_STREAM_DRAW);
        gl.activeTexture(GL_TEXTURE0);
        gl.bindTexture(GL_TEXTURE_BUFFER, _bone_matrices_buf_tex);
        gl.texBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, _bone_matrices_buffer);
      }
      // interior room light is now a per-instance attribute (uploaded above), no per-group uniform

      // FADING group (cull fade < 1): prepareDraw promotes its opaque batches to alpha blending,
      // which needs a depth prepass or internal/far-side geometry ghosts through during the fade --
      // the same two-pass scheme as the individual translucent path (client-verified).
      if (group.fade < 0.999f)
      {
        gl.colorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        for (ModelRenderPass& p : _render_passes)
        {
          if (p.blend_mode > 1)
          {
            continue; // depth-writing batches only (opaque / alpha-key)
          }
          if (p.prepareDraw(m2_shader, _model, representative, model_render_state, group.fade))
          {
            gl.drawElementsInstanced(GL_TRIANGLES, p.index_count, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(p.index_start * sizeof(GLushort)), static_cast<GLsizei>(group.transforms.size()));
            ++inst_drawcalls_local;
            p.afterDraw();
          }
        }
        gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      }

      for (ModelRenderPass& p : _render_passes)
      {
        ++passes_total;
        // Pass the representative instance so replaceable creature skins + geoset selection resolve (the
        // per-instance state the instanced draw otherwise loses -> invisible creatures). nullptr = doodads.
        if (p.prepareDraw(m2_shader, _model, representative, model_render_state, group.fade))
        {
          ++passes_drawn;
          gl.drawElementsInstanced(GL_TRIANGLES, p.index_count, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(p.index_start * sizeof(GLushort)), static_cast<GLsizei>(group.transforms.size()));
          ++inst_drawcalls_local;
          p.afterDraw();
        }
      }
    }

    // [SubmitInst breakdown 2026-08-05] record this model's instanced-draw shape (groups / total
    // drawElementsInstanced / instances) so the per-second profile report shows where the cost lives.
    noggit::perf::FrameProfiler::get().inst_add(
        static_cast<int>(interior_groups.size()), inst_drawcalls_local, instances.size());

    // Match the individual draw path's end-of-draw GL reset (single-instance overload, ~line 614): this
    // overload runs ONCE PER MODEL in a loop that SHARES one M2RenderState, so restore the caller's
    // baseline (blend OFF, cull ON, depth-write ON) and sync the cache to it. Without this, an additive
    // glow model leaves GL_BLEND enabled and the cache reading "additive"; the NEXT model whose first
    // pass is also additive then SKIPS re-issuing blend after an intervening state change, so additive
    // glow cards draw with blend disabled = opaque-black ("black mesh slices"). This was the parked
    // instanced-doodad bug -- the individual path fixed the identical class of desync the same way.
    gl.disable(GL_BLEND);
    gl.enable(GL_CULL_FACE);
    gl.depthMask(GL_TRUE);
    // [A2C source fix 2026-08-20] see the single-instance overload: a trailing ground-clutter pass left
    // GL_SAMPLE_ALPHA_TO_COVERAGE on for every prepareDraw-less path after it (MDI doodads) = global dither.
    gl.disable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    model_render_state.blend = 0xFFFF;
    model_render_state.backface_cull = true;
    model_render_state.z_buffered = false;
    model_render_state.detail_doodad = -1;

    // Leave the FULL instance set in the transform buffer for the particle/ribbon draws that follow
    // (they instance-count off it). Interior partitioning only affects the mesh passes above.
    // [perf 2026-08-05] ONLY re-upload when this model actually HAS particles/ribbons -- the vast majority
    // of instanced doodads (trees, rocks, grass) have neither, and this full bufferData ran unconditionally
    // once per model per frame (~1000+ redundant buffer orphans/frame). The mesh passes above already left
    // the buffer in a valid state; nothing downstream reads it when there are no emitters.
    if (!_model->_particles.empty() || !_model->_ribbons.empty())
    {
      OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder (_transform_buffer);
      gl.bufferData(GL_ARRAY_BUFFER, instances.size() * sizeof(::glm::mat4x4), instances.data(), GL_DYNAMIC_DRAW);
    }

    if (s_inst_dbg && _model->file_key().hasFilepath())
    {
      static std::set<std::string> logged;
      auto const& fp = _model->file_key().filepath();
      if (fp.find("creature") != std::string::npos && logged.insert(fp).second)
      {
        float bone0 = _model->bone_matrices.empty() ? -999.f : _model->bone_matrices[0][0][0];
        LogError << "INSTDBG model='" << fp << "' instances=" << instances.size()
                 << " passesDrawn=" << passes_drawn << "/" << passes_total
                 << " animated=" << _model->animated << " animBones=" << _model->animBones
                 << " animcalc=" << _model->animcalc
                 << " boneMatrices=" << _model->bone_matrices.size()
                 << " bone0.m00=" << bone0
                 << " firstXform.t=(" << instances[0][3][0] << "," << instances[0][3][1] << "," << instances[0][3][2] << ")"
                 << std::endl;
      }
    }

    if (capture_debug_enabled())
    {
      LogDebug << "ModelRender::draw instanced gpu end model='"
               << _model->file_key().stringRepr()
               << "'" << std::endl;
    }
  }

}

bool ModelRender::eligibleForPersistentDraw() const
{
  // MUST match drawPersistent's early-returns exactly, plus the emitter exclusion (particle/ribbon models
  // need the per-frame dynamic transform buffer). A model that fails this stays on the DYNAMIC path.
  return _model->finishedLoading()
      && !_model->loading_failed()
      && !is_classic_effect_shell_model(_model)
      && _model->_particles.empty()
      && _model->_ribbons.empty();
}

void ModelRender::drawPersistent(glm::mat4x4 const& model_view
    , GLuint transform_vbo
    , GLuint interior_vbo
    , int instance_count
    , OpenGL::Scoped::use_program& m2_shader
    , OpenGL::M2RenderState& model_render_state
    , int animtime
    , float extra_alpha
)
{
  ZoneScopedN(NOGGIT_CURRENT_FUNCTION);

  if (!_model->finishedLoading() || _model->loading_failed() || is_classic_effect_shell_model(_model))
  {
    // [diag] This should NEVER fire: the buffer build gates on eligibleForPersistentDraw() (same predicate),
    // so a buffered model is always drawable here. If it logs, the build/draw eligibility diverged and this
    // model's instances render nowhere (skipped in the gather AND here) = missing pieces.
    if (instance_count > 0 && noggit::perf::FrameProfiler::get().on && _model->file_key().hasFilepath())
    {
      static std::set<std::string> logged;
      auto const& fp = _model->file_key().filepath();
      if (logged.insert(fp).second)
      {
        LogError << "[PDRAW-SKIP] buffered-but-not-drawable model='" << fp
                 << "' finished=" << _model->finishedLoading() << " failed=" << _model->loading_failed()
                 << " classicShell=" << is_classic_effect_shell_model(_model) << " count=" << instance_count << std::endl;
      }
    }
    return;
  }
  if (instance_count <= 0 || transform_vbo == 0)
  {
    return;
  }

  if (!_uploaded)
  {
    upload();
  }
  if (!_vao_setup)
  {
    setupVAO(m2_shader);
  }

  // Shared pose (no per-instance bones on tile doodads) -- identical to the batched instanced path: one
  // animate() advances the model's bones + uploads its bone TBO (upload_bones defaults true).
  if (_model->animated && (!_model->animcalc || _model->_per_instance_animation))
  {
    _model->animate(model_view, 0, animtime);
    _model->animcalc = true;
  }

  OpenGL::Scoped::vao_binder const _(_vao);

  // Point the shared VAO's per-instance attributes at THIS tile-model's persistent buffers (divisor 1).
  // The dynamic path re-asserts transform+interior before its own draws, so leaving them pointed here is
  // safe. model_origin stays 0: the persistent transforms already carry each instance's full world matrix.
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const tb(transform_vbo);
    m2_shader.attrib("transform", 0, 1);
  }
  if (interior_vbo != 0)
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const ib(interior_vbo);
    m2_shader.attrib("interior", 4, GL_FLOAT, GL_FALSE, sizeof(::glm::vec4), nullptr);
    m2_shader.attrib_divisor("interior", 1, 1);
  }

  // Shared-bone uniforms (mirrors the batched instanced path's non-pib branch).
  if (_model->animBones && _bone_matrices_buf_tex != 0 && _bone_matrices_buffer_size > 0
      && !_model->bone_matrices.empty())
  {
    gl.activeTexture(GL_TEXTURE0);
    gl.bindTexture(GL_TEXTURE_BUFFER, _bone_matrices_buf_tex);
    m2_shader.uniform("anim_bones", true);
    m2_shader.uniform("bone_matrix_count", static_cast<int>(_model->bone_matrices.size()));
    m2_shader.uniform("per_instance_bone_stride", 0);
  }
  else
  {
    m2_shader.uniform("anim_bones", false);
    m2_shader.uniform("bone_matrix_count", 0);
    m2_shader.uniform("per_instance_bone_stride", 0);
  }

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_indices_buffer);

  // One instanced draw per pass over the whole bucket. extra_alpha < 1 (distance fade) promotes the opaque/
  // alpha-key passes to alpha blend via prepareDraw's translucent path -> the whole tile-bucket fades out
  // SMOOTHLY and keeps its authored lighting (translucency never changes lit/unlit), and additive
  // glow passes dim with it (mesh_color.w *= inst_alpha). No dither/shimmer. representative=nullptr.
  for (ModelRenderPass& p : _render_passes)
  {
    if (p.prepareDraw(m2_shader, _model, nullptr, model_render_state, extra_alpha))
    {
      gl.drawElementsInstanced(GL_TRIANGLES, p.index_count, GL_UNSIGNED_SHORT,
                               reinterpret_cast<void*>(p.index_start * sizeof(GLushort)),
                               static_cast<GLsizei>(instance_count));
      p.afterDraw();
    }
  }

  // Same end-of-draw GL/state reset as the batched instanced path (this runs once per model in a loop that
  // SHARES one M2RenderState) so an additive-glow bucket can't leak blend state into the next model.
  gl.disable(GL_BLEND);
  gl.enable(GL_CULL_FACE);
  gl.depthMask(GL_TRUE);
  // [A2C source fix 2026-08-20] clutter A2C must never outlive its own pass (see single-instance overload).
  gl.disable(GL_SAMPLE_ALPHA_TO_COVERAGE);
  model_render_state.blend = 0xFFFF;
  model_render_state.backface_cull = true;
  model_render_state.z_buffered = false;
  model_render_state.detail_doodad = -1;
}

void ModelRender::drawParticles(glm::mat4x4 const& model_view
    , OpenGL::Scoped::use_program& particles_shader
    , std::size_t instance_count
)
{
  for (auto& p : _model->_particles)
  {
    p.draw(model_view, particles_shader, _transform_buffer, static_cast<int>(instance_count));
  }
}

void ModelRender::drawParticlesFiltered(glm::mat4x4 const& model_view
    , OpenGL::Scoped::use_program& particles_shader
    , std::vector<glm::mat4x4> const& transforms
)
{
  if (_model->_particles.empty() || transforms.empty() || !_uploaded)
  {
    return; // !_uploaded: GL buffers don't exist until the mesh path uploads (see drawParticlesForInstance)
  }
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder(_transform_buffer);
    gl.bufferData(GL_ARRAY_BUFFER, transforms.size() * sizeof(glm::mat4x4), transforms.data(), GL_DYNAMIC_DRAW);
  }
  drawParticles(model_view, particles_shader, transforms.size());
}

void ModelRender::drawRibbonsFiltered(OpenGL::Scoped::use_program& ribbons_shader
    , std::vector<glm::mat4x4> const& transforms
)
{
  if (_model->_ribbons.empty() || transforms.empty() || !_uploaded)
  {
    return; // !_uploaded: see drawParticlesForInstance
  }
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder(_transform_buffer);
    gl.bufferData(GL_ARRAY_BUFFER, transforms.size() * sizeof(glm::mat4x4), transforms.data(), GL_DYNAMIC_DRAW);
  }
  drawRibbons(ribbons_shader, transforms.size());
}

void ModelRender::drawParticlesForInstance(glm::mat4x4 const& model_view
    , OpenGL::Scoped::use_program& particles_shader
    , glm::mat4x4 const& transform
    , float model_alpha
)
{
  // _uploaded guard: the mesh draw frustum-culls BEFORE upload(), so a freshly-streamed model whose
  // every copy was culled so far has NO GL buffers yet -- binding/bufferData on the never-generated
  // _transform_buffer name here fed the NVIDIA driver garbage and it AV'd on a later flush. Skip until
  // the mesh path has uploaded (first visible frame).
  if (_model->_particles.empty() || !_uploaded)
  {
    return;
  }
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder(_transform_buffer);
    gl.bufferData(GL_ARRAY_BUFFER, sizeof(glm::mat4x4), &transform, GL_DYNAMIC_DRAW);
  }
  // Apply CreatureModelAlpha to the particles (same value the mesh uses). The client fades the whole
  // model -- so the energy-elemental feet smoke that was rendering full-opacity now tracks the body.
  particles_shader.uniform("particle_alpha_mod", model_alpha);
  drawParticles(model_view, particles_shader, 1);
  particles_shader.uniform("particle_alpha_mod", 1.0f); // restore for the batched (non-creature) pass
}

void ModelRender::drawRibbons( OpenGL::Scoped::use_program& ribbons_shader
    , std::size_t instance_count
)
{
  for (auto& r : _model->_ribbons)
  {
    r.draw(ribbons_shader, _transform_buffer, static_cast<int>(instance_count));
  }
}

void ModelRender::drawBox(OpenGL::Scoped::use_program& m2_box_shader, std::size_t box_count)
{

  OpenGL::Scoped::vao_binder const _ (_box_vao);

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder (_transform_buffer);
    m2_box_shader.attrib("transform", 0, 1);
  }

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder (_box_vbo);
    m2_box_shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);
  }

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_box_indices_buffer);

  gl.drawElementsInstanced (GL_LINE_STRIP, static_cast<GLsizei>(_box_indices.size()), GL_UNSIGNED_SHORT, nullptr, static_cast<GLsizei>(box_count));
}

void ModelRender::setupVAO(OpenGL::Scoped::use_program& m2_shader)
{
  OpenGL::Scoped::vao_binder const _(_vao);

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder (_vertices_buffer);
    m2_shader.attrib("pos",           3, GL_FLOAT, GL_FALSE, sizeof (ModelVertex), 0);
    m2_shader.attribi("bones_weight", 4, GL_UNSIGNED_BYTE,   sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3)));
    m2_shader.attribi("bones_indices",4, GL_UNSIGNED_BYTE,  sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3) + 4));
    m2_shader.attrib("normal",        3, GL_FLOAT, GL_FALSE, sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3) + 8));
    m2_shader.attrib("texcoord1",     2, GL_FLOAT, GL_FALSE, sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3) * 2 + 8));
    m2_shader.attrib("texcoord2",     2, GL_FLOAT, GL_FALSE, sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3) * 2 + 8 + sizeof(glm::vec2)));
  }

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder (_transform_buffer);
    gl.bufferData(GL_ARRAY_BUFFER, 10 * sizeof(::glm::mat4x4), nullptr, GL_DYNAMIC_DRAW);
    m2_shader.attrib("transform", 0, 1);
  }

  {
    // [perf 2026-08-05] Per-instance interior room-light attribute (divisor 1), parallel to transform.
    // Lets mixed-interior instances draw in ONE call instead of one sub-draw per distinct interior value.
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const interior_binder (_interior_buffer);
    gl.bufferData(GL_ARRAY_BUFFER, 10 * sizeof(::glm::vec4), nullptr, GL_DYNAMIC_DRAW);
    m2_shader.attrib("interior", 4, GL_FLOAT, GL_FALSE, sizeof(::glm::vec4), nullptr);
    m2_shader.attrib_divisor("interior", 1, 1);
  }

  _vao_setup = true;
}


void ModelRender::fixShaderIdBlendOverride()
{
  for (auto& pass : _render_passes)
  {
    if (pass.shader_id & 0x8000)
    {
      continue;
    }

    int shader = 0;
    bool blend_mode_override = !_model->_uses_classic_layout && (_model->header.Flags & 8);

    if (is_masked_lightray_model(_model)
        && pass.renderflag_index < _model->_render_flags.size())
    {
      uint16_t const blend = _model->_render_flags[pass.renderflag_index].blend;
      if (blend == static_cast<uint16_t>(M2Blend::Add))
      {
        if (pass.texture_count == 2)
        {
          pass.shader_id = 0x8002;
          continue;
        }
      }
    }

    // fuckporting check
    if (pass.texture_coord_combo_index + pass.texture_count - 1 >= _model->_texture_unit_lookup.size())
    {
      LogDebug << "wrong texture coord combo index on fuckported model: " << _model->_file_key.stringRepr() << std::endl;
      // use default stuff
      pass.shader_id = 0;
      pass.texture_count = 1;

      continue;
    }

    if (!blend_mode_override)
    {
      uint16_t texture_unit_lookup = _model->_texture_unit_lookup[pass.texture_coord_combo_index];

      if (_model->_render_flags[pass.renderflag_index].blend)
      {
        shader = 1;

        if (texture_unit_lookup == 0xFFFF)
        {
          shader |= 0x8;
        }
      }

      shader <<= 4;

      if (texture_unit_lookup == 1)
      {
        shader |= 0x4000;
      }
    }
    else
    {
      uint16_t runtime_shader_val[2] = { 0, 0 };

      for (int i = 0; i < pass.texture_count; ++i)
      {
        if (pass.shader_id + i >= _model->blend_override.size())
        {
          blend_mode_override = false;
          shader = 0;
          break;
        }

        uint16_t override_blend = _model->blend_override[pass.shader_id + i];
        uint16_t texture_unit_lookup = _model->_texture_unit_lookup[pass.texture_coord_combo_index + i];

        if (i == 0 && _model->_render_flags[pass.renderflag_index].blend == 0)
        {
          override_blend = 0;
        }

        runtime_shader_val[i] = override_blend;

        if (texture_unit_lookup == 0xFFFF)
        {
          runtime_shader_val[i] |= 0x8;
        }

        if (texture_unit_lookup == 1 && i + 1 == pass.texture_count)
        {
          shader |= 0x4000;
        }
      }

      shader |= (runtime_shader_val[1] & 0xFFFF) | ((runtime_shader_val[0] << 4) & 0xFFFF);
    }

    pass.shader_id = shader;
  }
}

void ModelRender::fixShaderIDLayer()
{
  // Classic M2 models don't use the WotLK layering/merging system.
  // Applying it drops render passes that share a renderflag_index (e.g. most
  // body-part geosets on character models), leaving only holes.  Just assign
  // texture/animation indices directly and return.
  if (_model->_uses_classic_layout)
  {
    for (auto& pass : _render_passes)
    {
      pass.textures[0]      = pass.texture_combo_index;
      pass.textures[1]      = pass.texture_count > 1 ? pass.texture_combo_index + 1 : 0;
      pass.uv_animations[0] = pass.animation_combo_index;
      pass.uv_animations[1] = pass.texture_count > 1 ? pass.animation_combo_index + 1 : 0;
    }
    return;
  }

  int non_layered_count = 0;

  for (auto const& pass : _render_passes)
  {
    if (pass.material_layer <= 0)
    {
      non_layered_count++;
    }
  }

  if (non_layered_count < _render_passes.size())
  {
    std::vector<ModelRenderPass> passes;
    // [2026-08-20 Varian face-shine fix] first_pass must point into THIS surviving vector: the loop
    // pushes COPIES into `passes`, and the merge branches below then write through first_pass
    // (shader_id 0x8001/0x8002/0xE, texture_count, textures[1]). When first_pass pointed at the
    // SOURCE _render_passes element, every one of those writes landed on a copy that was thrown
    // away at `_render_passes = passes` -- the merged EnvMetal pass kept its UNMASKED
    // Combiners_Opaque_Mod2x shader while its alpha cover layer was dropped as "merged", so
    // reflective models (KingVarianWrynn etc.) shone x2 across skin the texture alpha masks out
    // in the client. reserve() guarantees the pointers stay valid (no reallocation).
    passes.reserve(_render_passes.size());

    ModelRenderPass* first_pass = nullptr;
    bool need_reducing = false;
    uint16_t previous_render_flag = -1, some_flags = 0;
    uint16_t previous_submesh = 0xFFFF;

    for (auto& pass : _render_passes)
    {
      // [2026-08-20 Jaina.m2 missing forearms, SIMULATED before shipping] This reduction block only runs
      // for models with material_layer > 0 batches -- character models have NONE (verified: humanmale/
      // humanfemale/dwarfmale/bloodelffemale all layered=0, reduction skipped), so nothing here can
      // affect character NPCs. (The 2026-08-20 NPC gear/eye chaos attributed to an earlier version of
      // this fix was actually the unsatisfiable-selection fallback + sleeve stacking, both fixed
      // elsewhere.) The dedupe may only collapse duplicate layers of the SAME skin section: a shared
      // renderflag_index across DIFFERENT submeshes is ordinary material reuse, not layering -- Jaina.m2
      // shares material 0 across the base passes of submeshes 0/1/3/4 (material 1 across their alpha
      // layers), and the unguarded dedupe deleted her gloves/forearms, earrings and necklace (12 built
      // passes -> 5 drawn). And the merge state machine must reset at a submesh boundary, so a merge
      // group can never pair passes of different body parts.
      if (pass.submesh != previous_submesh)
      {
        some_flags = 0;
        first_pass = nullptr;
      }
      if (pass.renderflag_index == previous_render_flag && pass.submesh == previous_submesh)
      {
        need_reducing = true;
        continue;
      }

      previous_render_flag = pass.renderflag_index;
      previous_submesh = pass.submesh;

      uint8_t lower_bits = pass.shader_id & 0x7;

      if (pass.material_layer == 0)
      {
        if (pass.texture_count >= 1 && _model->_render_flags[pass.renderflag_index].blend == 0)
        {
          pass.shader_id &= 0xFF8F;
        }

        first_pass = &pass;
      }

      if (!first_pass)
      {
        first_pass = &pass;
      }

      bool xor_unlit = ((_model->_render_flags[pass.renderflag_index].flags.unlit ^ _model->_render_flags[first_pass->renderflag_index].flags.unlit) & 1) == 0;

      if ((some_flags & 0xFF) == 1)
      {
        if ((_model->_render_flags[pass.renderflag_index].blend == 1 || _model->_render_flags[pass.renderflag_index].blend == 2)
            && pass.texture_count == 1
            && xor_unlit
            && pass.texture_combo_index == first_pass->texture_combo_index
            )
        {
          if (_model->_transparency_lookup[pass.transparency_combo_index] == _model->_transparency_lookup[first_pass->transparency_combo_index])
          {
            pass.shader_id = 0x8000;
            first_pass->shader_id = 0x8001;

            some_flags = (some_flags & 0xFF00) | 3;

            // current pass removed (not needed)
            continue;
          }
        }

        some_flags = (some_flags & 0xFF00);
      }

      int16_t texture_unit_lookup = _model->_texture_unit_lookup[pass.texture_coord_combo_index];

      if ((some_flags & 0xFF) < 2)
      {
        if ((_model->_render_flags[pass.renderflag_index].blend == 0) && (pass.texture_count == 2) && ((lower_bits == 4) || (lower_bits == 6)))
        {
          if (texture_unit_lookup == 0 && (_model->_texture_unit_lookup[pass.texture_coord_combo_index + 1] == -1))
          {
            some_flags = (some_flags & 0xFF00) | 1;
          }
        }
      }

      if ((some_flags >> 8) != 0)
      {
        if ((some_flags >> 8) == 1)
        {
          if (((_model->_render_flags[pass.renderflag_index].blend != 4) && (_model->_render_flags[pass.renderflag_index].blend != 6)) || (pass.texture_count != 1) || (texture_unit_lookup >= 0))
          {
            some_flags &= 0xFF00;
          }
          // tod
          else  if ((_model->_transparency_lookup.size() > pass.transparency_combo_index
          && _model->_transparency_lookup.size() > first_pass->transparency_combo_index)
          && _model->_transparency_lookup[pass.transparency_combo_index]
            == _model->_transparency_lookup[first_pass->transparency_combo_index])
          {
            pass.shader_id = 0x8000;
            first_pass->shader_id = _model->_render_flags[pass.renderflag_index].blend != 4 ? 0xE : 0x8002;

            some_flags = (some_flags & 0xFF) | (2 << 8);

            first_pass->texture_count = 2;

            first_pass->textures[1] = pass.texture_combo_index;
            first_pass->uv_animations[1] = pass.animation_combo_index;

            // current pass removed (merged with the previous one)
            continue;
          }
        }
        else
        {
          if ((some_flags >> 8) != 2)
          {
            continue;
          }

          if ( ((_model->_render_flags[pass.renderflag_index].blend != 2) && (_model->_render_flags[pass.renderflag_index].blend != 1))
               || (pass.texture_count != 1)
               || xor_unlit
               || ((pass.texture_combo_index & 0xff) != (first_pass->texture_combo_index & 0xff))
              )
          {
            some_flags &= 0xFF00;
          }
          else  if (_model->_transparency_lookup[pass.transparency_combo_index] == _model->_transparency_lookup[first_pass->transparency_combo_index])
          {
            // current pass ignored/removed
            pass.shader_id = 0x8000;
            first_pass->shader_id = ((first_pass->shader_id == 0x8002 ? 2 : 0) - 0x7FFF) & 0xFFFF;
            some_flags = (some_flags & 0xFF) | (3 << 8);
            continue;
          }
        }
        some_flags = (some_flags & 0xFF);
      }

      if ((_model->_render_flags[pass.renderflag_index].blend == 0) && (pass.texture_count == 1) && (texture_unit_lookup == 0))
      {
        some_flags = (some_flags & 0xFF) | (1 << 8);
      }

      // setup texture and anim lookup indices
      pass.textures[0] = pass.texture_combo_index;
      pass.textures[1] = pass.texture_count > 1 ? pass.texture_combo_index + 1 : 0;
      pass.uv_animations[0] = pass.animation_combo_index;
      pass.uv_animations[1] = pass.texture_count > 1 ? pass.animation_combo_index + 1 : 0;

      passes.push_back(pass);
      // Later merge iterations write through first_pass -- retarget it at the SURVIVING copy
      // (see the reserve() note above; without this the writes go to the discarded source vector).
      if (first_pass == &pass)
      {
        first_pass = &passes.back();
      }
    }

    if (need_reducing)
    {
      // [2026-08-20] Operate on the SURVIVING `passes` vector -- this block used to read/write
      // _render_passes (the pre-removal source, with mismatched indices) and every write was then
      // discarded by the assignment below.
      previous_render_flag = -1;
      for (std::size_t i = 0; i < passes.size(); ++i)
      {
        auto& pass = passes[i];
        uint16_t renderflag_index = pass.renderflag_index;

        if (renderflag_index == previous_render_flag && i > 0)
        {
          pass.shader_id = passes[i - 1].shader_id;
          pass.texture_count = passes[i - 1].texture_count;
          pass.texture_combo_index = passes[i - 1].texture_combo_index;
          pass.texture_coord_combo_index = passes[i - 1].texture_coord_combo_index;
        }
        else
        {
          previous_render_flag = renderflag_index;
        }
      }
    }

    _render_passes = passes;
  }
    // no layering, just setting some infos
  else
  {
    for (auto& pass : _render_passes)
    {
      pass.textures[0] = pass.texture_combo_index;
      pass.textures[1] = pass.texture_count > 1 ? pass.texture_combo_index + 1 : 0;
      pass.uv_animations[0] = pass.animation_combo_index;
      pass.uv_animations[1] = pass.texture_count > 1 ? pass.animation_combo_index + 1 : 0;
    }
  }
}

namespace
{

// https://wowdev.wiki/M2/.skin/WotLK_shader_selection
  std::optional<ModelPixelShader> GetPixelShader(uint16_t texture_count, uint16_t shader_id)
  {
    uint16_t texture1_fragment_mode = (shader_id >> 4) & 7;
    uint16_t texture2_fragment_mode = shader_id & 7;
    // uint16_t texture1_env_map = (shader_id >> 4) & 8;
    // uint16_t texture2_env_map = shader_id & 8;

    std::optional<ModelPixelShader> pixel_shader;

    if (texture_count == 1)
    {
      switch (texture1_fragment_mode)
      {
        case 0:
          pixel_shader = ModelPixelShader::Combiners_Opaque;
          break;
        case 2:
          pixel_shader = ModelPixelShader::Combiners_Decal;
          break;
        case 3:
          pixel_shader = ModelPixelShader::Combiners_Add;
          break;
        case 4:
          pixel_shader = ModelPixelShader::Combiners_Mod2x;
          break;
        case 5:
          pixel_shader = ModelPixelShader::Combiners_Fade;
          break;
        default:
          pixel_shader = ModelPixelShader::Combiners_Mod;
          break;
      }
    }
    else
    {
      if (!texture1_fragment_mode)
      {
        switch (texture2_fragment_mode)
        {
          case 0:
            pixel_shader = ModelPixelShader::Combiners_Opaque_Opaque;
            break;
          case 3:
            pixel_shader = ModelPixelShader::Combiners_Opaque_Add;
            break;
          case 4:
            pixel_shader = ModelPixelShader::Combiners_Opaque_Mod2x;
            break;
          case 6:
            pixel_shader = ModelPixelShader::Combiners_Opaque_Mod2xNA;
            break;
          case 7:
            pixel_shader = ModelPixelShader::Combiners_Opaque_AddNA;
            break;
          default:
            pixel_shader = ModelPixelShader::Combiners_Opaque_Mod;
            break;
        }
      }
      else if (texture1_fragment_mode == 1)
      {
        switch (texture2_fragment_mode)
        {
          case 0:
            pixel_shader = ModelPixelShader::Combiners_Mod_Opaque;
            break;
          case 3:
            pixel_shader = ModelPixelShader::Combiners_Mod_Add;
            break;
          case 4:
            pixel_shader = ModelPixelShader::Combiners_Mod_Mod2x;
            break;
          case 6:
            pixel_shader = ModelPixelShader::Combiners_Mod_Mod2xNA;
            break;
          case 7:
            pixel_shader = ModelPixelShader::Combiners_Mod_AddNA;
            break;
          default:
            pixel_shader = ModelPixelShader::Combiners_Mod_Mod;
            break;
        }
      }
      else if (texture1_fragment_mode == 3)
      {
        if (texture2_fragment_mode == 1)
        {
          pixel_shader = ModelPixelShader::Combiners_Add_Mod;
        }
      }
      else if (texture1_fragment_mode == 4 && texture2_fragment_mode == 4)
      {
        pixel_shader = ModelPixelShader::Combiners_Mod2x_Mod2x;
      }
      else if (texture2_fragment_mode == 1)
      {
        pixel_shader = ModelPixelShader::Combiners_Mod_Mod2x;
      }
    }


    return pixel_shader;
  }

  std::optional<ModelPixelShader> M2GetPixelShaderID (uint16_t texture_count, uint16_t shader_id)
  {
    std::optional<ModelPixelShader> pixel_shader;

    if (!(shader_id & 0x8000))
    {
      pixel_shader = GetPixelShader(texture_count, shader_id);

      if (!pixel_shader)
      {
        pixel_shader = GetPixelShader(texture_count, 0x11);
      }
    }
    else
    {
      switch (shader_id & 0x7FFF)
      {
        case 1:
          pixel_shader = ModelPixelShader::Combiners_Opaque_Mod2xNA_Alpha;
          break;
        case 2:
          pixel_shader = ModelPixelShader::Combiners_Opaque_AddAlpha;
          break;
        case 3:
          pixel_shader = ModelPixelShader::Combiners_Opaque_AddAlpha_Alpha;
          break;
      }
    }

    return pixel_shader;
  }
}

void ModelRender::computePixelShaderIDs()
{
  for (auto& pass : _render_passes)
  {
    pass.pixel_shader = M2GetPixelShaderID(pass.texture_count, pass.shader_id);
  }
}

void ModelRender::initRenderPasses(ModelView const* view, ModelTexUnit const* tex_unit, ModelGeoset const* model_geosets)
{
  _render_passes.reserve(view->n_texture_unit);
  for (size_t j = 0; j<view->n_texture_unit; j++)
  {
    size_t geoset = tex_unit[j].submesh;
    bool const classic_static = _model->_uses_classic_layout;

    if (geoset >= view->n_submesh
        || tex_unit[j].renderflag_index >= _model->_render_flags.size()
        || tex_unit[j].texture_count == 0
        || tex_unit[j].texture_count > 2
        || tex_unit[j].texture_combo_index >= _model->_texture_lookup.size()
        || tex_unit[j].texture_count > _model->_texture_lookup.size() - tex_unit[j].texture_combo_index
        || (!classic_static && tex_unit[j].transparency_combo_index != 0xFFFF && tex_unit[j].transparency_combo_index >= _model->_transparency_lookup.size()))
    {
      LogError << "Skipping invalid model render pass " << j << " for " << _model->file_key().stringRepr() << std::endl;
      continue;
    }

    ModelRenderPass pass(tex_unit[j], _model);
    pass.ordering_thingy = model_geosets[geoset].BoundingBox[0].x;
    // Full submesh sort-centre for the per-frame transparency sort: BoundingBox[0] is the client's per-batch
    // sort point (classic geoset.center, wotlk SkinSection CenterPosition), so this matches WoW.exe's key.
    pass.sort_center = model_geosets[geoset].BoundingBox[0];
    pass.geoset_id = model_geosets[geoset].id;

    pass.index_start = model_geosets[geoset].istart;
    pass.index_count = model_geosets[geoset].icount;
    pass.vertex_start = model_geosets[geoset].vstart;
    pass.vertex_end = pass.vertex_start + model_geosets[geoset].vcount;

    if (should_log_model_render_passes(_model))
    {
      auto const& renderflag = _model->_render_flags[tex_unit[j].renderflag_index];
      std::ostringstream texture_summary;
      for (std::size_t texture_index = 0; texture_index < tex_unit[j].texture_count; ++texture_index)
      {
        if (texture_index != 0)
        {
          texture_summary << ", ";
        }

        auto const lookup_index = tex_unit[j].texture_combo_index + texture_index;
        texture_summary << "lookup" << lookup_index;
        if (lookup_index < _model->_texture_lookup.size())
        {
          auto const texture = _model->_texture_lookup[lookup_index];
          texture_summary << "->tex" << texture;
          if (texture < _model->_textureFilenames.size())
          {
            texture_summary << ":'" << _model->_textureFilenames[texture] << "'";
          }
          if (texture < _model->_specialTextures.size())
          {
            texture_summary << " special=" << _model->_specialTextures[texture];
          }
        }
      }

      LogDebug << "M2 render pass model='" << _model->file_key().stringRepr()
               << "' pass=" << j
               << " geoset=" << geoset
               << " geosetId=" << pass.geoset_id
               << " indices=[" << pass.index_start << "+" << pass.index_count << "]"
               << " vertices=[" << pass.vertex_start << ".." << pass.vertex_end << ")"
               << " renderflag=" << tex_unit[j].renderflag_index
               << " flags={unlit:" << renderflag.flags.unlit
               << ", unfogged:" << renderflag.flags.unfogged
               << ", twoSided:" << renderflag.flags.two_sided
               << ", billboard:" << renderflag.flags.billboard
               << ", zBuffered:" << renderflag.flags.z_buffered
               << "} blend=" << renderflag.blend
               << " textureCount=" << tex_unit[j].texture_count
               << " textures=[" << texture_summary.str() << "]"
               << " colorIndex=" << tex_unit[j].color_index
               << " transparencyCombo=" << tex_unit[j].transparency_combo_index
               << " shaderId=" << tex_unit[j].shader_id
               << std::endl;
    }

    _render_passes.push_back(std::move(pass));
  }


  fixShaderIdBlendOverride();
  fixShaderIDLayer();
  computePixelShaderIDs();


  for (auto& pass : _render_passes)
  {
    pass.initUVTypes(_model);
  }

  // transparent parts come later
  std::sort(_render_passes.begin(), _render_passes.end());
}

void ModelRender::hideEmitterPlaceholderCards()
{
  // [black-cone fix 2026-08-08] Particle-emitter doodads (LavaSmokeEmitterB/LavaSmokeEmitter/LavaSteam:
  // the MC smoke/steam columns) carry a tiny OPAQUE billboard quad the client never draws -- only their
  // particles render. Drawing it paints the smoke BLP's black background as a giant camera-facing card
  // (the MC "black cone"). Signature is data-driven: model owns emitters + has a billboard bone, and the
  // pass is opaque/alpha-key with <=2 triangles. Additive glow cards (blend>=2) and real meshes
  // (campfire logs etc.) don't match. Runs after particles load; hides via showGeosets so both the
  // individual path (prepareDraw) and the MDI batch classifier (resolveStaticBatch) skip it.
  // MC set (both layouts, stock v264 and turtle v256 alike): LavaSmokeEmitter/B + LavaSplashParticle
  // (4-vert quad), BlackrockStatueLavaSplash (9-vert fan), LavaSteam -- every one is a 1-pass model whose
  // only mesh is a trivial opaque sheet; the particles are the visual. BlackRockLavaFalls01/02 (6-8
  // passes, 183+ verts, real geometry) must stay -- hence the pass-count and vertex-span guards.
  //
  // [2026-08-10 THRESHOLD FIX -- cost two days] the original <=24-vert gate ALSO swallowed the ACTIVE
  // VolcanicVent cones (21 verts, 1 pass, own emitters). Their smoke then rose from an invisible cone
  // while the emitterless *Off01 variants (no emitters -> never hidden) stood ~10yd away -- read in the
  // field as "smoke misplaced off the chimneys" and hunted through the entire particle pipeline, which
  // was correct the whole time. Real placeholder cards are 4-9 verts (flat sheets); real small meshes
  // start ~20. The gate is now <=10 verts. If a placeholder ever exceeds 10, prefer a flatness test
  // (degenerate extent on one axis) over raising this back.
  if (_model->_particles.empty() || _render_passes.size() > 2)
    return;
  for (auto const& pass : _render_passes)
  {
    if (pass.blend_mode <= 1
        && pass.vertex_end > pass.vertex_start
        && static_cast<int>(pass.vertex_end) - static_cast<int>(pass.vertex_start) <= 10
        && pass.submesh < _model->showGeosets.size())
    {
      _model->showGeosets[pass.submesh] = false;
    }
  }
}

void ModelRender::updateBoneMatrices()
{
  if (!_model->animBones || _model->bone_matrices.empty())
  {
    return;
  }

  std::size_t const upload_size = _model->bone_matrices.size() * sizeof(glm::mat4x4);

  if (capture_debug_enabled()
      && _model->file_key().filepath().find("gnomemachine") != std::string::npos)
  {
    LogDebug << "ModelRender::updateBoneMatrices begin model='"
             << _model->file_key().stringRepr()
             << "' uploadSize=" << upload_size
             << " bufferSize=" << _bone_matrices_buffer_size
             << " buffer=" << _bone_matrices_buffer
             << " texture=" << _bone_matrices_buf_tex
             << std::endl;
  }

  // GUARD (2026-07-23): only upload when the GL bone buffer + texture actually exist. The instanced
  // billboard bake (WorldRender ~3295) calls animate() PER INSTANCE to compute CPU bones BEFORE
  // ModelRender::draw() runs upload(), so both are still 0 here -> bufferData(GL_TEXTURE_BUFFER) on the 0
  // buffer = GL_INVALID_OPERATION, corrupting GL state -> the Timbermaw billboard doodads render BLACK
  // (masked only by the per-call glGetError sync). The bake reads the CPU bone_matrices directly and
  // uploads bones per-group inside draw(), so this per-instance GL upload is both unneeded AND harmful
  // here -- skip it until the buffers exist (the real per-instance/creature uploads run after upload()).
  if (_bone_matrices_buf_tex != 0 && _bone_matrices_buffer != 0)
  {
    // [CRE-PROFILE 2026-08-18 TEMP] per-model bone-TBO re-upload (bufferData STREAM_DRAW + texBuffer
    // re-association), run once per individual creature/attachment draw. Suspected bulk of M2Creatures.
    noggit::perf::Scoped _prof_boneup(noggit::perf::Phase::TileStream);
    OpenGL::Scoped::buffer_binder<GL_TEXTURE_BUFFER> const binder (_bone_matrices_buffer);
    // Orphan-on-upload (perf 2026-07-20). The per-instance doodad/creature path re-uploads this SHARED
    // per-model bone buffer once per instance and immediately draws from it. A plain bufferSubData then
    // BLOCKS the next instance's upload on the GPU still reading the previous draw's bones -- the "both
    // CPU+GPU idle" SubmitIndiv stall (~27ms in dense billboard scenes). Respecifying the whole store
    // with bufferData(STREAM_DRAW) tells the driver to discard the old contents, so it hands back fresh
    // storage instead of waiting. Bones are byte-identical; only the stall is removed. texBuffer is
    // re-associated each upload because respecifying the store can otherwise leave the buffer texture
    // reading stale storage on some drivers.
    _bone_matrices_buffer_size = upload_size;
    gl.bufferData(GL_TEXTURE_BUFFER, upload_size, _model->bone_matrices.data(), GL_STREAM_DRAW);
    if (_bone_matrices_buf_tex)
    {
      gl.bindTexture(GL_TEXTURE_BUFFER, _bone_matrices_buf_tex);
      gl.texBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, _bone_matrices_buffer);
    }
  }

  if (capture_debug_enabled()
      && _model->file_key().filepath().find("gnomemachine") != std::string::npos)
  {
    LogDebug << "ModelRender::updateBoneMatrices end model='"
             << _model->file_key().stringRepr()
             << "'" << std::endl;
  }
}

// ModelRenderPass


ModelRenderPass::ModelRenderPass(ModelTexUnit const& tex_unit, Model* m)
    : ModelTexUnit(tex_unit)
    , blend_mode(m->_render_flags[renderflag_index].blend)
{
}

// [2026-08-20 CLIENT-EXACT] Controlled-geoset hide test: a controlled family draws its SELECTED geoset
// and nothing else -- INCLUDING when the selected variant doesn't exist in the model, where the client
// renders NOTHING for that family (eyeglow default 1701 absent -> no glow; tabard 1201 absent -> no
// tabard). Two earlier special rules were removed after causing NPC-wide artifacts: "unsatisfiable ->
// show lowest existing variant" (painted DK-glow eyes and phantom tabards/gear) and "never hide the
// family-8 arm-skin variant" (stacked sleeve bands under gauntlets). The bare-arm case both were invented
// for is solved at the SELECTION level instead: family-8 defaults to variant 1 (802 bare arm, which every
// character model has) and a real glove geoset suppresses the sleeve family (World.cpp).
// External linkage: shared by the individual (prepareDraw), MDI-batch (resolveStaticBatch) and
// creature-batch (WorldRender) hide sites so they can never diverge. Returns true = hide this geoset.
bool noggit_geoset_hidden_by_controlled_family(Model* m,
    std::vector<std::uint16_t> const& controlled_families,
    std::vector<std::uint16_t> const& visible_ids,
    std::uint16_t geoset_id)
{
  (void)m;
  std::uint16_t const family = static_cast<std::uint16_t>(geoset_id / 100);
  if (std::find(controlled_families.begin(), controlled_families.end(), family) == controlled_families.end())
  {
    return false; // family not controlled by the instance's selection -> this rule never hides it
  }
  return std::find(visible_ids.begin(), visible_ids.end(), geoset_id) == visible_ids.end();
}

bool ModelRenderPass::prepareDraw(OpenGL::Scoped::use_program& m2_shader, Model *m, ModelInstance const* instance, OpenGL::M2RenderState& model_render_state, float extra_alpha)
{
  auto const* visible_geosets = &m->showGeosets;
  if (instance && !instance->geosetVisibility().empty())
  {
    visible_geosets = &instance->geosetVisibility();
  }

  if (submesh >= visible_geosets->size()
      || renderflag_index >= m->_render_flags.size()
      || (!m->_uses_classic_layout && !pixel_shader))
  {
    return false;
  }

  if (!(*visible_geosets)[submesh])
  {
    return false;
  }

  if (instance && !instance->controlledGeosetFamilies().empty()
      && noggit_geoset_hidden_by_controlled_family(m, instance->controlledGeosetFamilies(),
                                                   instance->visibleGeosetIds(), geoset_id))
  {
    log_classic_character_controlled_geoset_decision(m, instance, geoset_id, "hidden");
    return false;
  }

  log_classic_character_controlled_geoset_decision(m, instance, geoset_id, "draw");

  if (is_masked_lightray_model(m) && !model_render_state.allow_lightray_model)
  {
    return false;
  }

  // COLOUR
  // Get the colour and transparency and check that we should even render
  glm::vec4 mesh_color = glm::vec4(1.0f, 1.0f, 1.0f, m->trans); // ??
  glm::vec4 emissive_color = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);

  auto const& renderflag(m->_render_flags[renderflag_index]);

  if (m->_uses_classic_layout && !pixel_shader)
  {
    pixel_shader = classic_default_pixel_shader_for_blend(renderflag.blend);
  }

  // emissive colors
  // Sample BOTH tracks preferring the current animation with an anim-0 fallback (same convention
  // as the transparency block below). Previously RGB was hardcoded to anim 0 while opacity used the
  // current sequence -- asymmetric, and models whose color track is keyed on the playing animation
  // (portal pulses) never advanced.
  if (color_index != -1 && static_cast<size_t>(color_index) < m->_colors.size())
  {
    auto& color_track (m->_colors[color_index].color);
    auto& opacity_track (m->_colors[color_index].opacity);

    bool has_color = true;
    ::glm::vec3 c (1.0f, 1.0f, 1.0f);
    if (color_track.uses (m->_current_anim_seq))
    {
      c = color_track.getValue (m->_current_anim_seq, m->_anim_time, m->_global_animtime);
    }
    else if (color_track.uses (0))
    {
      c = color_track.getValue (0, m->_anim_time, m->_global_animtime);
    }
    else
    {
      has_color = false;
    }

    if (has_color)
    {
      if (opacity_track.uses (m->_current_anim_seq))
      {
        mesh_color.w = opacity_track.getValue (m->_current_anim_seq, m->_anim_time, m->_global_animtime);
      }
      else if (opacity_track.uses (0))
      {
        mesh_color.w = opacity_track.getValue (0, m->_anim_time, m->_global_animtime);
      }

      mesh_color.x = c.x; mesh_color.y = c.y; mesh_color.z = c.z;

      emissive_color = glm::vec4(c.x,c.y,c.z, mesh_color.w);
    }
  }

  // opacity
  if (transparency_combo_index != 0xFFFF && transparency_combo_index < m->_transparency_lookup.size())
  {
    auto transparency_index = m->_transparency_lookup[transparency_combo_index];
    if (transparency_index >= 0
        && static_cast<std::size_t>(transparency_index) < m->_transparency.size())
    {
      auto& transparency (m->_transparency[static_cast<std::size_t>(transparency_index)].trans);
      if (transparency.uses (m->_current_anim_seq))
      {
        mesh_color.w = mesh_color.w * transparency.getValue(m->_current_anim_seq, m->_anim_time, m->_global_animtime);
      }
      else if (transparency.uses (0))
      {
        mesh_color.w = mesh_color.w * transparency.getValue(0, m->_anim_time, m->_global_animtime);
      }
    }
  }

  // TRACE-VERIFIED (RE_notes/20): a pass whose animated opacity evaluates to ~0 is NOT drawn by the
  // client (alphatest >= 1/255 kills it). E.g. Anomalus's MANAMISTBASE shell is DEATH-ONLY
  // (ModelColor alpha = 0 in every idle anim) -- drawing it at idle painted a dim lit layer over his
  // bright body. Skip such passes outright -- but ONLY for creature/character models, which is the case
  // this was written for. World GameObject force-field / portal WALLS (RazorfenForceField, ZulGurub
  // Forcefield, InstancePortal, Summon_Ritual, ...) are ADDITIVE submeshes whose authored transparency
  // legitimately fades to 0 in their "open"/gone GameObject state; nuking the whole panel over one
  // frame's animated alpha collapsed the barrier to just its particle ring (the "tiny" regression, added
  // in 7c43c6a0). For an ADDITIVE pass alpha~=0 already contributes ~=0 under premultiplied ONE/ONE, so
  // NOT skipping world passes is visually free.
  if (mesh_color.w < (1.0f / 255.0f) && is_classic_creature_or_character_model(m))
  {
    return false;
  }

  // How the LIVE 1.12 client renders a translucent creature (CreatureModelAlpha < 255, e.g. Anomalus
  // 200) -- verified against the apitrace (the body geometry is submitted TWICE per frame):
  //   * PASS A: depth-only prepass -- same body batches, COLORWRITEENABLE=0, ZWRITEENABLE=TRUE.
  //   * PASS B: the body again with ALPHABLENDENABLE=TRUE (SRCALPHA/INVSRCALPHA) and color writes on,
  //     alpha = CreatureModelAlpha -- translucent against the BACKGROUND, while the prepass depth
  //     keeps far-side/internal geometry from ghosting through.
  //   * Runes / Purple_Glow / glows: drawn ADDITIVE (SRCBLEND=SRCALPHA, DESTBLEND=ONE) with ALPHATEST
  //     on, ZWRITEENABLE=FALSE, CULL=NONE -- glowing on top.
  // (An earlier read of a previous capture concluded "solid body" -- that was PASS A, whose color
  // writes are off; the blended PASS B right after it is what makes the creature see-through.)
  // Implemented via the depth prepass in ModelRender::draw + promote_to_alpha_blend below. Lighting
  // is NOT changed by translucency -- authored lit/unlit per material, like the client (see the
  // effective_unlit note below).
  // extra_alpha = distance fade for instanced draws; folds into the same channel as
  // CreatureModelAlpha so opaque passes promote to alpha-blend and everything dims consistently.
  float const inst_alpha = (instance ? instance->model_alpha : 1.0f) * extra_alpha;
  uint16_t effective_blend = renderflag.blend;
  bool const translucent_display = inst_alpha < 0.999f;

  // Fishing-pool water-effect geosets (foam ring / bubbles / sparkles): the M2 authors them blend=0
  // Opaque, so we'd draw the flat foam disc as a solid bright quad z-fighting the water. These effect
  // textures are DARK-BACKGROUND wisp glows with NO/faint alpha (WaterWake3 avg RGB 13,24,30 alpha 255;
  // Star1 37,36,37 alpha 255; Tornado 14,19,24). Opaque bloomed white; alpha painted a solid blue veil;
  // additive glowed too bright/blue. The wake should read like the murky water it sits in -- dark and
  // grey. So promote to Alpha blend + drop depth-write, and (via water_surface_effect below) the
  // fragment shader desaturates the wisps to grey and uses the texel BRIGHTNESS as the alpha so the
  // near-black field goes transparent (water colour shows through) and only a faint grey wake remains,
  // lit by the scene. Resolved once per pass by texture name (only on _water_surface_effect models).
  if (_water_effect_translucent < 0)
  {
    _water_effect_translucent = 0;
    if (m->_water_surface_effect)
    {
      std::string tex_name = "(unresolved)";
      if (textures[0] < m->_texture_lookup.size())
      {
        uint16_t const tex = m->_texture_lookup[textures[0]];
        if (tex < m->_textures.size())
        {
          // Resolve the texture name the way bindTexture does (m->_textures[tex], NOT _textureFilenames,
          // which is empty for these classic models) -- the ref's file_key holds the real BLP path.
          tex_name = m->_textures[tex]->file_key().stringRepr();
          std::transform(tex_name.begin(), tex_name.end(), tex_name.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
          for (char const* marker : {"waterwake", "wake", "bubble", "star", "tornado", "splash", "foam", "ripple"})
          {
            if (tex_name.find(marker) != std::string::npos)
            {
              _water_effect_translucent = 1;
              break;
            }
          }
        }
      }
    }
  }
  bool const water_effect_geoset = _water_effect_translucent == 1;
  if (water_effect_geoset && effective_blend == static_cast<uint16_t>(M2Blend::Opaque))
  {
    effective_blend = static_cast<uint16_t>(M2Blend::Alpha);
  }
  int const water_effect_uniform = water_effect_geoset ? 1 : 0;
  if (model_render_state.water_surface_effect != water_effect_uniform)
  {
    m2_shader.uniform("water_surface_effect", water_effect_uniform);
    model_render_state.water_surface_effect = water_effect_uniform;
  }

  // NOTE: the "glow/mist core" placeholder mesh (the out-of-place torso pill on the arcane elementals) is
  // hidden at load time in Model.cpp via a geometric compact+dense-core-sphere test -- see showGeosets
  // there. That replaces an earlier per-pass particle-texture skip which only caught the Anomalus.

  bool const is_additive_blend = effective_blend == static_cast<uint16_t>(M2Blend::Add)
                              || effective_blend == static_cast<uint16_t>(M2Blend::No_Add_Alpha);
  // TRACE-VERIFIED (RE_notes/20, Anomalus draw block): CreatureModelAlpha multiplies the ALPHA of
  // EVERY pass -- the body's blend alpha AND the additive layers' tex.a x 0.784 (SRCALPHA/ONE, so it
  // scales their added light). It NEVER multiplies RGB. The old !is_additive gate left additive
  // layers unscaled.
  mesh_color.w *= inst_alpha;
  (void)is_additive_blend;

  // Aura char-proc tint (Ghost Visual's light-blue shift): whole-model color multiplier, applied
  // to every pass including additive glows -- the client's ghost effect shifts the entire body.
  if (instance)
  {
    mesh_color.x *= instance->model_tint.x;
    mesh_color.y *= instance->model_tint.y;
    mesh_color.z *= instance->model_tint.z;
  }

  // exit and return false before affecting the opengl render state
  if (mesh_color.w <= 0.0f)
  {
    return false;
  }

  bool const masked_additive = uses_masked_lightray_additive(m, *this, effective_blend);

  // Translucent creatures (CreatureModelAlpha < 255): the client draws the opaque/alpha-key body
  // batches ALPHA-BLENDED (over a depth prepass laid in ModelRender::draw) so the whole creature is
  // see-through against the background. Promote only the GL blend state -- the blend_mode UNIFORM
  // stays the authored value so alpha-key batches keep their alpha test in the shader.
  bool const promote_to_alpha_blend = translucent_display
    && (effective_blend == static_cast<uint16_t>(M2Blend::Opaque)
        || effective_blend == static_cast<uint16_t>(M2Blend::Alpha_Key));
  uint16_t const blend_state_key = effective_blend | (promote_to_alpha_blend ? 0x100 : 0);

  if (model_render_state.blend != blend_state_key)
  {
    switch (promote_to_alpha_blend ? M2Blend::Alpha : static_cast<M2Blend>(effective_blend))
    {
      default:
      case M2Blend::Opaque:
      case M2Blend::Alpha_Key:
        gl.disable(GL_BLEND);
        break;
      case M2Blend::Alpha:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        break;
      case M2Blend::No_Add_Alpha:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_ONE, GL_ONE);
        break;
      case M2Blend::Add:
        gl.enable(GL_BLEND);
        // Premultiplied additive: m2_frag folds the material alpha into RGB for Add passes (visually
        // identical to SRC_ALPHA/ONE) so the alpha channel can carry the EMITTED-brightness bloom
        // mask instead of coverage alpha -- coverage alpha marked whole glow-card quads emissive and
        // bloomed the bright sky behind them into hard white boxes (see m2_frag bloom-mask block).
        gl.blendFunc(GL_ONE, GL_ONE);
        break;
      case M2Blend::Mod:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_DST_COLOR, GL_ZERO);
        break;
      case M2Blend::Mod2x:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_DST_COLOR, GL_SRC_COLOR);
        break;
    }

    m2_shader.uniform("blend_mode", static_cast<int>(effective_blend));
    model_render_state.blend = blend_state_key;
  }

  if (model_render_state.masked_additive != masked_additive)
  {
    m2_shader.uniform("masked_additive", static_cast<int>(masked_additive));
    model_render_state.masked_additive = masked_additive;
  }

  bool const classic_alpha_pass = m->_uses_classic_layout && effective_blend != static_cast<uint16_t>(M2Blend::Opaque);
  // ADDITIVE passes (No_Add_Alpha / Add) are two-sided in the client: additive blending is order-independent
  // so both faces add the same, and many effect models -- notably the mage portals (world/generic/activedoodads/
  // spellportals/*, v256 additive discs) -- ship WITHOUT the two_sided flag yet the client draws both faces.
  // Honouring only the flag rendered them one-sided (vanished from the back). Match the client for additive.
  bool const additive_two_sided = effective_blend == static_cast<uint16_t>(M2Blend::Add)
                               || effective_blend == static_cast<uint16_t>(M2Blend::No_Add_Alpha);
  bool const backface_cull = !renderflag.flags.two_sided && !classic_alpha_pass && !additive_two_sided;
  if (model_render_state.backface_cull != backface_cull)
  {
    if (!backface_cull)
    {
      gl.disable(GL_CULL_FACE);
    }
    else
    {
      gl.enable(GL_CULL_FACE);
    }

    model_render_state.backface_cull = backface_cull;
  }

  bool const no_depth_write = renderflag.flags.z_buffered || water_effect_geoset;
  if (model_render_state.z_buffered != no_depth_write)
  {
    gl.depthMask(no_depth_write ? GL_FALSE : GL_TRUE);
    model_render_state.z_buffered = no_depth_write;
  }

  if (model_render_state.unfogged != renderflag.flags.unfogged)
  {
    m2_shader.uniform("unfogged", (int)renderflag.flags.unfogged);
    model_render_state.unfogged = renderflag.flags.unfogged;
  }

  // CLIENT-EXACT (2026-08-22, Stratholme ghost-citizen hunt): CreatureModelAlpha does NOT change
  // lighting -- the trace rule above (RE_notes/20) is that it multiplies the ALPHA of every pass and
  // nothing else. Translucent creatures keep their AUTHORED lit/unlit materials: the Spectral/Ghostly
  // Citizens are ordinary LIT humans (warm-brown baked skins, display alpha 128, aura 16331 has NO
  // spell visual) that the client dims with scene light like any other NPC. The old forcing
  // (unlit ||= model_alpha < 1) painted their raw warm textures fullbright over the dark scene =
  // the reported wrong ghost hue. Its "spawns get no scene light -> lit renders black" premise
  // predates the client-exact M2 sun/interior lighting; Anomalus-style energy bodies are AUTHORED
  // unlit and keep their look without it.
  bool const effective_unlit = renderflag.flags.unlit;
  if (model_render_state.unlit != effective_unlit)
  {
    m2_shader.uniform("unlit", (int)effective_unlit);
    model_render_state.unlit = effective_unlit;
  }

  // Ground-clutter detail doodads: colored day/night lighting via the detail path + alpha-to-
  // coverage (with MSAA) so distant sub-pixel blades blend instead of aliasing into green dots.
  int const detail_doodad = (m && m->_force_unlit) ? 1 : 0;
  if (model_render_state.detail_doodad != detail_doodad)
  {
    m2_shader.uniform("detail_doodad", detail_doodad);
    if (detail_doodad)
    {
      gl.enable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    }
    else
    {
      gl.disable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    }
    model_render_state.detail_doodad = detail_doodad;
  }

  // Emissive bloom for bright energy creatures (Anomalus, arcane elementals, ghosts): the live client's
  // FFXGlow blooms bright SCREEN pixels regardless of material flags. Noggit's bloom is emissive-mask
  // (alpha) driven, so an opaque bright body never blooms. Scope a luminance->mask contribution to
  // creature/character M2s only (not WMO/terrain, which is what caused the Ironforge wash).
  // Mode 1 = opaque ENERGY creature pass: hot texels feed the mask directly (alpha channel is free).
  //          Gated on the pass being UNLIT (self-illuminated) -- that's what makes a body "energy"
  //          (elemental/ghost bodies are unlit; ordinary skin/cloth is lit). Without the gate, any
  //          brightly-LIT surface crossed the luma knee and bloomed (glowing white blouses on NPCs);
  //          the client never blooms lit bodies -- its FFXGlow has no per-pixel gate at all.
  // Mode 3 = the alpha-only mask re-draw of a TRANSLUCENT creature (see ModelRender::draw): the body
  //          pass itself (mode 0) keeps its authored blend alpha so the body stays see-through.
  // Mode 2 = the body pass of a translucent creature: alpha is the BLEND FACTOR there, so the
  //          shader must not clobber it with the fog bloom-mask write (that overwrite made
  //          translucent creatures render opaque on any fogged map).
  // Mode 2 ("alpha IS the blend factor -- never clobber it with the fog/bloom-mask writes") must
  // cover EVERY translucent/fading draw, not only creature models: gated on
  // is_classic_creature_or_character_model, GAMEOBJECTS and equipment attachments (sword, shield,
  // helmet, shoulders -- item models) had their fade alpha REPLACED by the fog mask
  // (color.a = 1 - fogFactor) on any fogged map, so they rendered opaque through the whole fade
  // and POPPED while creature bodies (mode 2) faded correctly through the same pipeline.
  int const creature_bloom = model_render_state.bloom_mask_pass
    ? 3
    : (translucent_display
        ? 2
        : ((is_classic_creature_or_character_model(m) && effective_unlit) ? 1 : 0));
  if (model_render_state.creature_bloom != creature_bloom)
  {
    m2_shader.uniform("creature_bloom", creature_bloom);
    model_render_state.creature_bloom = creature_bloom;
  }

  if (texture_count > 1)
  {
    if (!bindTexture(1, m, instance, model_render_state, m2_shader))
    {
      return false;
    }
  }

  if (!bindTexture(0, m, instance, model_render_state, m2_shader))
  {
    return false;
  }

  GLint tu1 = static_cast<GLint>(tu_lookups[0]), tu2 = static_cast<GLint>(tu_lookups[1]);

  if (model_render_state.tex_unit_lookups[0] != tu1)
  {
    m2_shader.uniform("tex_unit_lookup_1", tu1);
    model_render_state.tex_unit_lookups[0] = tu1;
  }

  if (model_render_state.tex_unit_lookups[1] != tu2)
  {
    m2_shader.uniform("tex_unit_lookup_2", tu2);
    model_render_state.tex_unit_lookups[1] = tu2;
  }

  int16_t tex_anim_lookup = -1;
  if (uv_animations[0] < m->_texture_animation_lookups.size())
  {
    tex_anim_lookup = m->_texture_animation_lookups[uv_animations[0]];
  }
  static const glm::mat4x4 unit(glm::mat4x4(1));

  // Lightray cones: the two mesh passes are distinct layers, identified by their primary texture.
  //   layer 1 = Lightray_Dusty_01: STATIC stretched depth anchor (vRange 0..1, no scroll).
  //   layer 2 = Lightray_Dusty_02 (+_Mask): the scrolling dust, low opacity, masked.
  // The model's authored tex-anim lookups resolve to -1, so we drive the scroll here from the
  // model's animated texture matrix and leave layer 1 on identity. lightray_layer is read by
  // m2_frag's masked-additive branch to pick the per-layer opacity/mask behaviour.
  // Volumetric light-ray cones have a second pass -- the dark dusty texture -- whose tex-anim lookup
  // slot resolves to -1, so it renders frozen. In-game that dusty layer scrolls downward (the
  // "falling dust" of the shaft). Fall it back to the model's first texture animation so it flows.
  // Scoped to volumetric lights so no other model's intentionally-static pass is affected.
  // Only the 2-texture dusty pass (Dusty_02 + its mask) scrolls. The 1-texture pass (Dusty_01) is the
  // static stretched depth anchor and must stand still, so gate the scroll fallback on texture_count.
  if (tex_anim_lookup == -1
      && texture_count > 1
      && !m->_texture_animations.empty()
      && m->file_key().hasFilepath()
      && m->file_key().filepath().find("volumetriclight") != std::string::npos)
  {
    tex_anim_lookup = 0;
  }

  bool const is_volumetric_light = m->file_key().hasFilepath()
                                && m->file_key().filepath().find("volumetriclight") != std::string::npos;

  if (tex_anim_lookup != -1 && static_cast<size_t>(tex_anim_lookup) < m->_texture_animations.size())
  {
    // animated UV: the matrix changes per frame, so always re-upload (these models are rare).
    m2_shader.uniform("tex_matrix_1", m->_texture_animations[tex_anim_lookup].mat);
    model_render_state.tex_matrix_state[0] = 0;
    if (texture_count > 1)
    {
      int16_t tex_anim_lookup_2 = -1;
      if (uv_animations[1] < m->_texture_animation_lookups.size())
      {
        tex_anim_lookup_2 = m->_texture_animation_lookups[uv_animations[1]];
      }
      // Volumetric light dust: the second texture layer's lookup is also -1 (frozen). Animate it too,
      // and shift it half a texture in V so the two scrolling layers overlap and fill each other's
      // wrap gap (a continuous stream rather than gappy "patchy showers").
      if (tex_anim_lookup_2 == -1 && is_volumetric_light && !m->_texture_animations.empty())
      {
        tex_anim_lookup_2 = 0;
      }
      if (tex_anim_lookup_2 != -1 && static_cast<size_t>(tex_anim_lookup_2) < m->_texture_animations.size())
      {
        glm::mat4x4 mat2 = m->_texture_animations[tex_anim_lookup_2].mat;
        if (is_volumetric_light)
        {
          mat2[3].y += 0.5f;
        }
        m2_shader.uniform("tex_matrix_2", mat2);
        model_render_state.tex_matrix_state[1] = 0;
      }
      else if (model_render_state.tex_matrix_state[1] != 1)
      {
        m2_shader.uniform("tex_matrix_2", unit);
        model_render_state.tex_matrix_state[1] = 1;
      }
    }
  }
  else
  {
    // [perf 2026-07-23] static UV (the common case): both matrices are identity. Skip re-uploading
    // identity if the last upload on this unit was already identity within this draw call. A mat4
    // uniform is 64 bytes to the driver -- eliminating the redundant ones is the biggest single win.
    if (model_render_state.tex_matrix_state[0] != 1)
    {
      m2_shader.uniform("tex_matrix_1", unit);
      model_render_state.tex_matrix_state[0] = 1;
    }
    if (model_render_state.tex_matrix_state[1] != 1)
    {
      m2_shader.uniform("tex_matrix_2", unit);
      model_render_state.tex_matrix_state[1] = 1;
    }
  }


  GLint ps = static_cast<GLint>(pixel_shader.value());
  if (model_render_state.pixel_shader != ps)
  {
    m2_shader.uniform("pixel_shader", ps);
    model_render_state.pixel_shader = ps;
  }

  // [FALLS-DIAG 2026-08-08] NOGGIT_FALLS_RAWTEX=1|2: make the shader output the falls' RAW tex1 sample
  // (1 = rgb, 2 = alpha) -- splits the hunt: red raw => array/upload fine, gold is post-sampling;
  // gold raw => the array layer content itself is wrong. Set per-pass; 0 for every other model.
  {
    static int const s_falls_rawtex = [] { char const* e = std::getenv("NOGGIT_FALLS_RAWTEX");
      return e ? std::atoi(e) : 0; }();
    if (s_falls_rawtex != 0)
    {
      bool const is_falls = m->file_key().hasFilepath()
        && m->file_key().filepath().find("lavafalls") != std::string::npos;
      m2_shader.uniform("debug_rawtex", is_falls ? s_falls_rawtex : 0);
    }
  }

  // [FALLS-DIAG 2026-08-08] NOGGIT_FALLS_DIAG=1: dump the exact per-pass state the MC lavafalls draw
  // with (~once per 360 pass-draws, in bursts so one burst covers a whole model) -- chasing the
  // view-dependent gold/red colour snap. Diagnostic only.
  {
    static bool const s_falls_diag = std::getenv("NOGGIT_FALLS_DIAG") != nullptr;
    if (s_falls_diag && m->file_key().hasFilepath()
        && m->file_key().filepath().find("lavafalls") != std::string::npos)
    {
      static int s_tick = 0;
      if ((++s_tick % 360) < 8)
      {
        glm::mat4x4 const& tm = (tex_anim_lookup != -1
                                 && static_cast<size_t>(tex_anim_lookup) < m->_texture_animations.size())
                                ? m->_texture_animations[tex_anim_lookup].mat : unit;
        LogError << "[FALLS] geoset=" << geoset_id << " blend=" << effective_blend
                 << " ps=" << ps
                 << " arr=" << model_render_state.tex_arrays[0] << "/" << model_render_state.tex_indices[0]
                 << " clamp=" << model_render_state.tex_clamp[0]
                 << " talookup=" << tex_anim_lookup
                 << " tmat_t=(" << tm[3].x << "," << tm[3].y << ")"
                 << " tmat_s=(" << tm[0].x << "," << tm[1].y << ")"
                 << " color=(" << mesh_color.x << "," << mesh_color.y << ","
                 << mesh_color.z << "," << mesh_color.w << ")"
                 << " unlit=" << (renderflag.flags.unlit ? 1 : 0)
                 << " seq=" << m->_current_anim_seq << " t=" << m->_anim_time
                 << " gt=" << m->_global_animtime << std::endl;
        // live texture resolution -- catches a stale/wrong array-layer bind (the texture the pass
        // SHOULD sample vs what the cached uniforms bound)
        if (textures[0] < m->_texture_lookup.size())
        {
          uint16_t const tex = m->_texture_lookup[textures[0]];
          if (tex < m->_textures.size())
          {
            auto& t = m->_textures[tex];
            LogError << "[FALLS-TEX] name=" << t->file_key().stringRepr()
                     << " live_arr=" << t->texture_array()
                     << " live_layer=" << t->array_index()
                     << " uploaded=" << (t->is_uploaded() ? 1 : 0) << std::endl;
          }
        }
      }
    }
  }

  // [perf 2026-07-23] check-before-set: most world doodads have a constant (1,1,1,trans) mesh_color, so
  // it repeats across a model's passes (and across instances whose alpha/tint match). A per-instance
  // alpha/tint difference correctly misses and re-uploads.
  if (model_render_state.mesh_color != mesh_color)
  {
    m2_shader.uniform("mesh_color", mesh_color);
    model_render_state.mesh_color = mesh_color;
  }

  return true;
}

void ModelRenderPass::afterDraw()
{
  // Intentionally empty. This used to reset the blend func to alpha (SRC_ALPHA, ONE_MINUS_SRC_ALPHA)
  // after every pass -- but prepareDraw only re-issues glBlendFunc when model_render_state.blend
  // CHANGES between passes. Resetting the func here without touching that cache desynced the two:
  // a run of consecutive same-blend passes (e.g. Anomalus' additive glow submeshes 3/4/5) had the
  // func silently reverted to alpha after the first pass, so every following pass -- believing it was
  // still additive -- actually alpha-blended. For an additive texture with an opaque black background
  // (Purple_Glow, ArcaneElementalRune) alpha blend darkens the destination toward black => the "black
  // boxes". prepareDraw is the sole owner of the blend state; leave the GL func exactly as it set it.
}

bool ModelRenderPass::bindTexture(size_t index, Model* m, ModelInstance const* instance, OpenGL::M2RenderState& model_render_state, OpenGL::Scoped::use_program& m2_shader)
{
  if (index >= texture_count || textures[index] >= m->_texture_lookup.size())
  {
    return false;
  }

  uint16_t tex = m->_texture_lookup[textures[index]];

  if (tex >= m->_specialTextures.size() || tex >= m->_textures.size())
  {
    return false;
  }

  scoped_blp_texture_reference const* selected_texture = nullptr;
  bool unresolved_special_texture = false;

  if (m->_specialTextures[tex] != -1)
  {
    if (instance)
    {
      auto const& instance_replacements = instance->replaceTextures();
      auto instance_replacement = instance_replacements.find(m->_specialTextures[tex]);
      if (instance_replacement != instance_replacements.end())
      {
        selected_texture = &instance_replacement->second;
      }
    }

    if (!selected_texture)
    {
      auto replacement = m->_replaceTextures.find(m->_specialTextures[tex]);
      if (replacement != m->_replaceTextures.end())
      {
        selected_texture = &replacement->second;
      }
    }

    unresolved_special_texture = !selected_texture;
  }

  if (!selected_texture)
  {
    selected_texture = &m->_textures[tex];
  }

  if (model_texture_debug_enabled() && m->_specialTextures[tex] > 0 && m->_specialTextures[tex] < 32)
  {
    std::uint32_t const special_type = static_cast<std::uint32_t>(m->_specialTextures[tex]);
    std::uint32_t const special_bit = (1u << special_type);
    bool const using_placeholder = selected_texture == &m->_textures[tex];

    if (using_placeholder && (m->_logged_missing_special_texture_mask & special_bit) == 0)
    {
      m->_logged_missing_special_texture_mask |= special_bit;
      LogDebug << "M2 unresolved special texture model='" << m->file_key().stringRepr()
               << "' textureIndex=" << tex
               << " specialType=" << special_type
               << " hasInstance=" << (instance ? 1 : 0)
               << " classicLayout=" << m->usesClassicLayout()
               << std::endl;
    }
  }

  if (unresolved_special_texture)
  {
    return false;
  }

  bool const using_black_placeholder = selected_texture == &m->_textures[tex]
                                    && tex < m->_textureFilenames.size()
                                    && m->_textureFilenames[tex] == "tileset/generic/black.blp";
  bool const non_opaque_classic_pass = m->_uses_classic_layout
                                    && blend_mode != static_cast<uint16_t>(M2Blend::Opaque)
                                    && blend_mode != static_cast<uint16_t>(M2Blend::Alpha_Key);
  if (using_black_placeholder && non_opaque_classic_pass)
  {
    return false;
  }

  auto& texture = *selected_texture;
  if (!texture->finishedLoading() && is_classic_creature_or_character_model(m))
  {
    texture->wait_until_loaded();
  }

  // For classic creature/character overrides, give the selected replacement a
  // chance to finish loading before falling back to black.
  if (selected_texture != &m->_textures[tex])
  {
    auto& override_tex = *selected_texture;
    if (override_tex->loading_failed() || !override_tex->finishedLoading())
    {
      selected_texture = &m->_textures[tex];
    }
  }

  auto& resolved_texture = *selected_texture;
  if (!resolved_texture->finishedLoading() && is_classic_creature_or_character_model(m))
  {
    resolved_texture->wait_until_loaded();
  }

  if (!resolved_texture->finishedLoading() || resolved_texture->loading_failed())
  {
    return false;
  }

  resolved_texture->upload();
  if (!resolved_texture->is_uploaded())
  {
    return false;
  }

  GLuint tex_array = resolved_texture->texture_array();
  int tex_index = resolved_texture->array_index();

  // [wrong-texture fix 2026-08-08] bind UNCONDITIONALLY, every pass -- the pre-texture-array behaviour.
  // The check-before-set cache (perf 2026-07-23) assumed nothing else touches the unit's binding between
  // passes; in practice lazy texture uploads (and anything else running GL mid-scope) rebind the active
  // unit behind the cache's back, and every "skipped redundant bind" after that samples ANOTHER array --
  // the MC lavafalls drawing catwalk-metal/molten-steel instead of lava, varying with camera because
  // stream-in order varied. A redundant glBindTexture is nanoseconds; wrong-texture frames are not.
  gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + index + 1));
  gl.bindTexture(GL_TEXTURE_2D_ARRAY, tex_array);
  model_render_state.tex_arrays[index] = tex_array;
  m2_shader.uniform(index ? "tex2_index" : "tex1_index", tex_index);
  model_render_state.tex_indices[index] = static_cast<GLuint>(tex_index);

  // M2 texture wrap flags (0x1 wrap X, 0x2 wrap Y): an unset bit means CLAMP addressing on that
  // axis (trace-verified against the 1.12 client). Handed to the shader inverted, as a clamp mask,
  // where it is emulated in-shader (textures share array textures, so GL wrap state can't change).
  uint32_t const wrap_flags = tex < m->_texture_flags.size() ? m->_texture_flags[tex] : 0x3;
  int const clamp_mask = (~wrap_flags) & 0x3;
  if (model_render_state.tex_clamp[index] != clamp_mask)
  {
    m2_shader.uniform(index ? "tex2_clamp" : "tex1_clamp", clamp_mask);
    model_render_state.tex_clamp[index] = clamp_mask;
  }
  return true;
}

// [CRE-BODY-DIAG 2026-08-19] the last rej() code, read by WorldRender::drawCreatureBodiesBatched to attribute
// creature-group batch failures to a specific gate (the shared s_rej histogram mixes doodads + creatures).
thread_local int g_last_static_batch_reject = 0;

bool ModelRenderPass::resolveStaticBatch(Model* m, StaticBatchKey& out, bool for_pib, ModelInstance const* rep) const
{
  // [perf 2026-08-05] Conservative batchability gate. Everything rejected here falls back to the classic
  // per-model persistent draw, so it is always safe to reject; coverage is widened later (3b). The batched
  // program draws with CONSTANT unfogged=0/unlit=0/masked_additive=0/mesh_color=(1,1,1,1)/identity tex-matrix/
  // anim_bones=false and blend OFF + depth-write ON, so any pass needing otherwise must be rejected here.
  //
  // [MDI-DIAG 2026-08-06] rejection histogram -- coverage came out zero, so tally WHICH gate rejects. Called
  // only on the draw thread (single-threaded) so plain statics are fine; dumped cumulatively every 200k calls.
  static unsigned long s_calls = 0, s_ok = 0, s_rej[16] = {0};
  ++s_calls;
  auto dump = [&]()
  {
    if (s_calls % 200000ul != 0ul) return;
    LogError << "[MDI-REJECT] calls=" << s_calls << " ok=" << s_ok
             << " renderflag=" << s_rej[1] << " no_ps=" << s_rej[2] << " blend=" << s_rej[3]
             << " flags=" << s_rej[4] << " color=" << s_rej[5] << " transp=" << s_rej[6]
             << " trans=" << s_rej[7] << " creature=" << s_rej[8] << " lightray=" << s_rej[9]
             << " water=" << s_rej[10] << " bones=" << s_rej[11] << " tex0=" << s_rej[12]
             << " tex1=" << s_rej[13] << " animuv=" << s_rej[14] << std::endl;
  };
  // [FALLS-DIAG 2026-08-08] name the falls' bake decisions -- the visible gold falls behave like a
  // BATCHED draw (no scroll, no prepareDraw debug), so log whether/why the classifier admits them.
  static bool const s_falls_bake_dbg = std::getenv("NOGGIT_FALLS_DIAG") != nullptr;
  bool const falls_dbg = s_falls_bake_dbg && m->file_key().hasFilepath()
    && m->file_key().filepath().find("lavafalls") != std::string::npos;
  auto rej = [&](int code) -> bool
  {
    g_last_static_batch_reject = code;
    ++s_rej[code];
    if (falls_dbg)
    {
      static int s_fb_tick = 0;
      if ((++s_fb_tick % 120) < 4)
      {
        LogError << "[FALLS-BAKE] REJECT code=" << code << " for_pib=" << (for_pib ? 1 : 0)
                 << " submesh=" << submesh << " model=" << m->file_key().stringRepr() << std::endl;
      }
    }
    dump();
    return false;
  };

  if (renderflag_index >= m->_render_flags.size())
    return rej(1);
  // [black-cone fix 2026-08-08] mirror prepareDraw's visible_geosets gate: submeshes hidden at load
  // (emitter placeholder cards, elementalearth shells, centroid cores) must never enter an MDI batch.
  // Rejecting drops the model to the individual path, which skips the hidden submesh.
  // [creature MDI 2026-08-18] filter passes by geoset VISIBILITY exactly like prepareDraw: a creature batch
  // uses the representative instance's per-display visibility (rep->geosetVisibility() + controlled families),
  // a doodad batch uses the model default m->showGeosets. Using the model default for a creature would show
  // equipment/hidden geosets or hide body geosets.
  {
    std::vector<bool> const& visible_geosets =
      (rep && !rep->geosetVisibility().empty()) ? rep->geosetVisibility() : m->showGeosets;
    if (submesh < visible_geosets.size() && !visible_geosets[submesh])
      return rej(4);
    if (rep && !rep->controlledGeosetFamilies().empty()
        && noggit_geoset_hidden_by_controlled_family(m, rep->controlledGeosetFamilies(),
                                                     rep->visibleGeosetIds(), geoset_id))
      return rej(4);
  }
  auto const& renderflag = m->_render_flags[renderflag_index];

  // effective pixel shader (mirror prepareDraw: classic layout derives a default from the blend)
  std::optional<ModelPixelShader> ps = pixel_shader;
  if (m->_uses_classic_layout && !ps)
    ps = classic_default_pixel_shader_for_blend(renderflag.blend);
  if (!ps)
    return rej(2);

  // Tile batch: Opaque / Alpha_Key only -- they share GL blend state (blend OFF); Alpha_Key differs by the
  // in-shader alpha test keyed on the per-group blend_mode uniform. inst_alpha is 1 (doodads) so nothing
  // promotes. [pib-MDI 2026-08-07] for_pib additionally admits Alpha(2) / No_Add_Alpha(3) / Add(4) -- the
  // dominant glow-card blends; the pib MDI draw sets GL blend + depthMask per group from key.blend_mode
  // (opaque groups draw before blended ones). Mod/Mod2x (5/6) stay rejected (rare, dest-coupled).
  uint16_t const blend = renderflag.blend;
  uint16_t const max_blend = for_pib ? static_cast<uint16_t>(M2Blend::Add)
                                     : static_cast<uint16_t>(M2Blend::Alpha_Key);
  if (blend > max_blend)
    return rej(3);

  // Batch-constant flags: z_buffered always rejects. unfogged/unlit reject in the tile batch (its draw uses
  // constant 0/0 uniforms) but are ADMITTED per-group for the pib batch (carried in the key -- glow cards are
  // commonly unlit and/or unfogged).
  if (renderflag.flags.z_buffered)
    return rej(4);
  if (!for_pib && (renderflag.flags.unfogged || renderflag.flags.unlit))
    return rej(4);

  // require mesh_color == (1,1,1,1). RGB: no animated colour track (rare). Alpha: EVALUATE the transparency
  // exactly as prepareDraw does and batch only when it is ~1 -- the common case is a CONSTANT-1 track (opaque
  // prop), which IS batchable; only genuine fades (alpha < 1) fall back to the uniform path. Blanket-rejecting
  // any transparency track killed ~96% of coverage (nearly every doodad has one). [MDI-DIAG 2026-08-06]
  // A CONSTANT-white M2Color track is extremely common on CREATURES (most bodies carry one with no visual
  // effect) -- it evaluates to opaque white and IS batchable; only a genuinely coloured/animated track must
  // fall back. [2026-08-19] For CREATURE batches (rep != null, which rebuild the batch EVERY frame) evaluate
  // the track exactly like prepareDraw (~2020-2053) and accept an opaque-white frame -- the pib shader's
  // constant mesh_color=(1,1,1,1) is then byte-identical (emissive_color is dead, so white = no-op). DOODAD
  // batches (rep == null) are CACHED across frames, where a per-frame eval could lock an animated colour, so
  // keep the blanket reject for them. This was the ENTIRE creature-body-MDI failure: 100% of rejects = rej5.
  if (color_index != -1)
  {
    bool white_frame = false;
    if (rep && static_cast<std::size_t>(color_index) < m->_colors.size())
    {
      auto& ctrack = m->_colors[color_index].color;
      auto& otrack = m->_colors[color_index].opacity;
      ::glm::vec3 c(1.0f, 1.0f, 1.0f);
      if (ctrack.uses(m->_current_anim_seq))
        c = ctrack.getValue(m->_current_anim_seq, m->_anim_time, m->_global_animtime);
      else if (ctrack.uses(0))
        c = ctrack.getValue(0, m->_anim_time, m->_global_animtime);
      float o = 1.0f;
      if (otrack.uses(m->_current_anim_seq))
        o = otrack.getValue(m->_current_anim_seq, m->_anim_time, m->_global_animtime);
      else if (otrack.uses(0))
        o = otrack.getValue(0, m->_anim_time, m->_global_animtime);
      white_frame = c.x >= 0.999f && c.x <= 1.001f && c.y >= 0.999f && c.y <= 1.001f
                 && c.z >= 0.999f && c.z <= 1.001f && o >= 0.999f;
    }
    if (!white_frame)
      return rej(5);
  }
  float alpha = m->trans;
  if (transparency_combo_index != 0xFFFF && transparency_combo_index < m->_transparency_lookup.size())
  {
    int const ti = m->_transparency_lookup[transparency_combo_index];
    if (ti >= 0 && static_cast<std::size_t>(ti) < m->_transparency.size())
    {
      auto& tr = m->_transparency[static_cast<std::size_t>(ti)].trans;
      if (tr.uses(m->_current_anim_seq))
        alpha *= tr.getValue(m->_current_anim_seq, m->_anim_time, m->_global_animtime);
      else if (tr.uses(0))
        alpha *= tr.getValue(0, m->_anim_time, m->_global_animtime);
    }
  }
  if (alpha < 0.999f)
    return rej(7); // genuine fade -> real alpha needed, keep uniform path

  // classes handled by other paths / not representable in the static batch
  // [creature MDI 2026-08-18] a creature BATCH (rep != null) admits creature/character models -- they fold
  // into the pib-style per-instance-bone MDI. The doodad batch (rep == null) still routes them to the
  // individual path.
  if (!rep && is_classic_creature_or_character_model(m))
    return rej(8);
  if (is_masked_lightray_model(m))
    return rej(9);
  if (m->_water_surface_effect)
    return rej(10);
  // [animated MDI 2026-08-07] SHARED-pose animated models (animBones) are now BATCHABLE: their per-model bone
  // matrices ride the batched bone SSBO, indexed per-instance by inst_tex.z (block base) / inst_tex.w (count)
  // -- drawDoodadsBatched fills them each frame. Only PER-INSTANCE animation (billboards) is still rejected:
  // each instance would need its own bone block, which one per-model slot can't represent.
  // _per_instance_animation (billboard-boned trees -- the vast majority of animated doodads, 97% of the
  // reject histogram) is deliberately NOT rejected: the persistent tile-doodad path this batch replaces
  // ALREADY draws them with the SHARED pose (one animate per model per frame; the per-instance billboard
  // refinement only exists in the individual WMO-doodad path, which still runs unbatched). One shared bone
  // block per model in the SSBO reproduces today's user-accepted look exactly.
  if (m->animBones && m->bone_matrices.empty())
    return rej(11); // bones not computed yet (fresh load) -> batching now would lock it to bind pose; the
                    // unbatched fallback animates it and a later rebuild picks it up with a real bone block

  // BASE-texture resolution per unit (instance-independent). ret: 1 ok, 0 unit unused, -1 reject/defer.
  auto resolve_unit = [&](std::size_t index, GLuint& arr, int& layer, int& clamp) -> int
  {
    if (index >= texture_count) { arr = 0; layer = 0; clamp = 0; return 0; }
    if (textures[index] >= m->_texture_lookup.size()) return -1;
    uint16_t const tex = m->_texture_lookup[textures[index]];
    if (tex >= m->_specialTextures.size() || tex >= m->_textures.size()) return -1;
    scoped_blp_texture_reference const* sel = nullptr;
    if (m->_specialTextures[tex] != -1)
    {
      // [creature MDI 2026-08-18] special/replaceable texture. A doodad batch can't represent it -> reject.
      // A creature batch (rep) resolves the display's skin from the representative instance's replaceTextures
      // (then the model default), mirroring ModelRenderPass::bindTexture. Since the group is keyed by
      // display_id, all instances share this (array,layer) -> it rides inst_tex.x/y like a doodad's layer.
      if (!rep) return -1;
      auto const special = static_cast<std::size_t>(m->_specialTextures[tex]);
      auto const& repl = rep->replaceTextures();
      auto it = repl.find(special);
      if (it != repl.end()) { sel = &it->second; }
      if (!sel)
      {
        auto mr = m->_replaceTextures.find(special);
        if (mr != m->_replaceTextures.end()) { sel = &mr->second; }
      }
      if (!sel) return -1; // unresolved special skin this frame -> fall back (retried next frame)
    }
    scoped_blp_texture_reference const& t = sel ? *sel : m->_textures[tex];
    if (t->loading_failed() || !t->finishedLoading()) return -1; // defer until loaded (retried next frame)
    t->upload();
    if (!t->is_uploaded()) return -1;
    arr = t->texture_array();
    layer = t->array_index();
    uint32_t const wrap = tex < m->_texture_flags.size() ? m->_texture_flags[tex] : 0x3;
    clamp = static_cast<int>((~wrap) & 0x3);
    return 1;
  };

  int const r0 = resolve_unit(0, out.tex_array0, out.layer0, out.tex_clamp0);
  if (r0 != 1)
    return rej(12); // unit 0 must resolve
  int const r1 = resolve_unit(1, out.tex_array1, out.layer1, out.tex_clamp1);
  if (r1 == -1)
    return rej(13); // unit 1 present but not batchable/deferred

  // static UV only (no animated texture matrix) -- mirror the prepareDraw tex_anim_lookup resolution
  auto static_uv = [&](std::size_t idx) -> bool
  {
    if (uv_animations[idx] < m->_texture_animation_lookups.size())
      return m->_texture_animation_lookups[uv_animations[idx]] == -1;
    return true; // no lookup -> not animated
  };
  if (!static_uv(0))
    return rej(14);
  if (r1 == 1 && !static_uv(1))
    return rej(14);

  out.tu_lookup0 = static_cast<int>(tu_lookups[0]);
  out.tu_lookup1 = static_cast<int>(tu_lookups[1]);
  out.pixel_shader = static_cast<int>(ps.value());
  out.blend_mode = blend;
  out.unfogged = for_pib && renderflag.flags.unfogged; // tile batch always resolves these false (gated above)
  out.unlit = for_pib && renderflag.flags.unlit;
  bool const classic_alpha_pass = m->_uses_classic_layout && blend != static_cast<uint16_t>(M2Blend::Opaque);
  // Additive passes are two-sided (see prepareDraw) -- keep the batched draw consistent for additive effects.
  bool const additive_two_sided = blend == static_cast<uint16_t>(M2Blend::Add)
                               || blend == static_cast<uint16_t>(M2Blend::No_Add_Alpha);
  out.backface_cull = !renderflag.flags.two_sided && !classic_alpha_pass && !additive_two_sided;
  ++s_ok;
  if (falls_dbg)
  {
    static int s_fa_tick = 0;
    if ((++s_fa_tick % 120) < 4)
    {
      LogError << "[FALLS-BAKE] ADMIT for_pib=" << (for_pib ? 1 : 0)
               << " submesh=" << submesh << " blend=" << out.blend_mode
               << " model=" << m->file_key().stringRepr() << std::endl;
    }
  }
  dump();
  return true;
}

void ModelRenderPass::initUVTypes(Model* m)
{
  tu_lookups[0] = texture_unit_lookup::none;
  tu_lookups[1] = texture_unit_lookup::none;

  if (m->_texture_unit_lookup.size() < texture_coord_combo_index + texture_count)
  {
    LogError << "model: texture_coord_combo_index out of range " << m->file_key().stringRepr() << std::endl;

    for (int i = 0; i < texture_count; ++i)
    {
      switch (i)
      {
        case 0: tu_lookups[i] = texture_unit_lookup::t1; break;
        case 1: tu_lookups[i] = texture_unit_lookup::t2; break;
      }
    }

    return;

    //throw std::out_of_range("model: texture_coord_combo_index out of range " + m->filename);
  }

  for (int i = 0; i < texture_count; ++i)
  {
    switch (m->_texture_unit_lookup[texture_coord_combo_index + i])
    {
      case (int16_t)(-1): tu_lookups[i] = texture_unit_lookup::environment; break;
      case 0: tu_lookups[i] = texture_unit_lookup::t1; break;
      case 1: tu_lookups[i] = texture_unit_lookup::t2; break;
    }
  }
}
