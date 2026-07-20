// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ModelRender.hpp"
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/Log.h>
#include <noggit/frame_profiler.hpp>
#include <external/tracy/Tracy.hpp>
#include <math/bounding_box.hpp>
#include <noggit/Misc.h>
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

void ModelRender::upload()
{
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
    gl.deleteTextures(1, &_bone_matrices_buf_tex);

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

  if (_model->animated && (!_model->animcalc || _model->_per_instance_animation))
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
    _model->_hand_overlay_active = instance.closeHands();
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

  // These three GL resets change state that prepareDraw caches in model_render_state, which is SHARED
  // across every model in the batch. Sync the cache to what we just forced, so the next model's first
  // pass doesn't skip re-applying a state we changed out from under it (e.g. an additive-first effect
  // model rendering with blend left disabled). blend = 0xFFFF is an invalid sentinel that forces the
  // next pass to re-issue both the GL blend func and the blend_mode uniform.
  model_render_state.blend = 0xFFFF;
  model_render_state.backface_cull = true;
  model_render_state.z_buffered = false;
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
)
{
  ZoneScopedN(NOGGIT_CURRENT_FUNCTION);
  bool const skip_mesh_passes = is_classic_effect_shell_model(_model);

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

    if (_model->animated && (!_model->animcalc || _model->_per_instance_animation))
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
    struct InteriorGroup { glm::vec4 interior; float fade; std::vector<glm::mat4x4> transforms; };
    std::vector<InteriorGroup> interior_groups;
    for (std::size_t i = 0; i < instances.size(); ++i)
    {
      glm::vec4 const inter = (i < instance_interior.size()) ? instance_interior[i] : glm::vec4(0.f);
      float const fade_raw = (i < instance_fades.size()) ? instance_fades[i] : 1.0f;
      float fade = std::round(std::clamp(fade_raw, 0.0f, 1.0f) * 64.0f) / 64.0f;
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
        if (cand.interior == inter && cand.fade == fade) { grp = &cand; break; }
      }
      if (!grp) { interior_groups.push_back({inter, fade, {}}); grp = &interior_groups.back(); }
      grp->transforms.push_back(instances[i]);
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

    OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_indices_buffer);

    static bool const s_inst_dbg = std::getenv("NOGGIT_INSTANCE_DEBUG") != nullptr;
    int passes_drawn = 0;
    int passes_total = 0;

    for (auto const& group : interior_groups)
    {
      if (group.transforms.empty())
      {
        continue;
      }

      {
        // Upload only this group's transforms so index 0 is the group start (no glDraw*BaseInstance in
        // GL 3.3), then set the room light this whole group shares.
        OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder (_transform_buffer);
        gl.bufferData(GL_ARRAY_BUFFER, group.transforms.size() * sizeof(::glm::mat4x4), group.transforms.data(), GL_DYNAMIC_DRAW);
      }
      m2_shader.uniform("instance_interior", group.interior);

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
          p.afterDraw();
        }
      }
    }

    // Leave the FULL instance set in the transform buffer for the particle/ribbon draws that follow
    // (they instance-count off it). Interior partitioning only affects the mesh passes above.
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

    ModelRenderPass* first_pass = nullptr;
    bool need_reducing = false;
    uint16_t previous_render_flag = -1, some_flags = 0;

    for (auto& pass : _render_passes)
    {
      if (pass.renderflag_index == previous_render_flag)
      {
        need_reducing = true;
        continue;
      }

      previous_render_flag = pass.renderflag_index;

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
    }

    if (need_reducing)
    {
      previous_render_flag = -1;
      for (int i = 0; i < passes.size(); ++i)
      {
        auto& pass = _render_passes[i];
        uint16_t renderflag_index = pass.renderflag_index;

        if (renderflag_index == previous_render_flag)
        {
          pass.shader_id = _render_passes[i - 1].shader_id;
          pass.texture_count = _render_passes[i - 1].texture_count;
          pass.texture_combo_index = _render_passes[i - 1].texture_combo_index;
          pass.texture_coord_combo_index = _render_passes[i - 1].texture_coord_combo_index;
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

  {
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

  if (instance && !instance->controlledGeosetFamilies().empty())
  {
    auto const geoset_family = static_cast<std::uint16_t>(geoset_id / 100);
    if (instance->isGeosetFamilyControlled(geoset_family)
        && !instance->isGeosetIdVisible(geoset_id))
    {
      log_classic_character_controlled_geoset_decision(m, instance, geoset_id, "hidden");
      return false;
    }
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
  // Implemented via the depth prepass in ModelRender::draw + promote_to_alpha_blend below. Creatures
  // are also forced fullbright (spawns get no scene light; a lit material would render black).
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
  bool const backface_cull = !renderflag.flags.two_sided && !classic_alpha_pass;
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

  // A translucent creature is self-illuminated energy: render every pass FULLBRIGHT. Creature spawns don't
  // get scene lighting, so a LIT material would render BLACK (e.g. Anomalus submesh 2 = the MANAMISTBASE
  // aura on mat1, which is LIT unlike the UNLIT body). The body (unlit) is already fullbright; force the
  // others to match so the whole creature reads as bright energy.
  // AUTHORED translucency ONLY (CreatureModelAlpha) -- NOT the distance/cull fade: a normal lit
  // creature must keep its exact lit shading while fading, or its lighting visibly flips to
  // fullbright the moment the fade starts and back when it completes.
  bool const authored_translucent = (instance ? instance->model_alpha : 1.0f) < 0.999f;
  bool const effective_unlit = renderflag.flags.unlit || authored_translucent;
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
    m2_shader.uniform("tex_matrix_1", m->_texture_animations[tex_anim_lookup].mat);
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
      }
      else
      {
        m2_shader.uniform("tex_matrix_2", unit);
      }
    }
  }
  else
  {
    m2_shader.uniform("tex_matrix_1", unit);
    m2_shader.uniform("tex_matrix_2", unit);
  }


  GLint ps = static_cast<GLint>(pixel_shader.value());
  if (model_render_state.pixel_shader != ps)
  {
    m2_shader.uniform("pixel_shader", ps);
    model_render_state.pixel_shader = ps;
  }

  m2_shader.uniform("mesh_color", mesh_color);

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

  gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + index + 1));
  gl.bindTexture(GL_TEXTURE_2D_ARRAY, tex_array);
  m2_shader.uniform(index ? "tex2_index" : "tex1_index", tex_index);

  // M2 texture wrap flags (0x1 wrap X, 0x2 wrap Y): an unset bit means CLAMP addressing on that
  // axis (trace-verified against the 1.12 client). Handed to the shader inverted, as a clamp mask,
  // where it is emulated in-shader (textures share array textures, so GL wrap state can't change).
  uint32_t const wrap_flags = tex < m->_texture_flags.size() ? m->_texture_flags[tex] : 0x3;
  int const clamp_mask = (~wrap_flags) & 0x3;
  m2_shader.uniform(index ? "tex2_clamp" : "tex1_clamp", clamp_mask);

  model_render_state.tex_indices[index] = tex_index;
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
