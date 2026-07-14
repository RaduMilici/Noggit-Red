// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "WMOGroupRender.hpp"
#include <noggit/WMO.h>

#include <cstdlib>
#include <limits>

using namespace Noggit::Rendering;

namespace
{
  // Bisect switch for the per-batch (MOBA-style) frustum cull: NOGGIT_NO_MOBA_CULL=1 draws the
  // merged calls whole, as before.
  bool moba_cull_disabled()
  {
    static bool const disabled = []
    {
      char const* v = std::getenv("NOGGIT_NO_MOBA_CULL");
      return v && *v && *v != '0';
    }();
    return disabled;
  }
}

WMOGroupRender::WMOGroupRender(WMOGroup* wmo_group)
: _wmo_group(wmo_group)
{

}

void WMOGroupRender::upload()
{
  // render batches

  bool texture_not_uploaded = false;

  std::size_t batch_counter = 0;
  for (auto& batch : _wmo_group->_batches)
  {
    if (batch.texture >= _wmo_group->wmo->materials.size())
    {
      batch_counter++;
      continue;
    }

    WMOMaterial const& mat (_wmo_group->wmo->materials[batch.texture]);

    if (mat.texture1 >= _wmo_group->wmo->textures.size())
    {
      batch_counter++;
      continue;
    }

    auto& tex1 = _wmo_group->wmo->textures[mat.texture1];

    tex1->wait_until_loaded();
    tex1->upload();

    if (!tex1->is_uploaded())
    {
      batch_counter++;
      continue;
    }

    std::uint32_t tex_array0 = tex1->texture_array();
    std::uint32_t array_index0 = tex1->array_index();

    std::uint32_t tex_array1 = 0;
    std::uint32_t array_index1 = 0;
    bool use_tex2 = mat.shader == 6 || mat.shader == 5 || mat.shader == 3;

    if (use_tex2)
    {
      if (mat.texture2 >= _wmo_group->wmo->textures.size())
      {
        batch_counter++;
        continue;
      }

      auto& tex2 = _wmo_group->wmo->textures[mat.texture2];
      tex2->wait_until_loaded();
      tex2->upload();

      if (!tex2->is_uploaded())
      {
        batch_counter++;
        continue;
      }

      tex_array1 = tex2->texture_array();
      array_index1 = tex2->array_index();
    }

    _render_batches[batch_counter].tex_array0 = tex_array0;
    _render_batches[batch_counter].tex_array1 = tex_array1;
    _render_batches[batch_counter].tex0 = array_index0;
    _render_batches[batch_counter].tex1 = array_index1;

    batch_counter++;
  }

  if (texture_not_uploaded)
  {
    return;
  }

  _draw_calls.clear();
  WMOCombinedDrawCall* draw_call = nullptr;
  std::vector<WMORenderBatch*> _used_batches;

  batch_counter = 0;
  for (auto& batch : _wmo_group->_batches)
  {
    if (batch.texture >= _wmo_group->wmo->materials.size() || !_render_batches[batch_counter].tex_array0)
    {
      batch_counter++;
      continue;
    }

    WMOMaterial& mat = _wmo_group->wmo->materials[batch.texture];
    bool backface_cull = !mat.flags.unculled;
    bool use_tex2 = mat.shader == 6 || mat.shader == 5 || mat.shader == 3;

    if (use_tex2 && !_render_batches[batch_counter].tex_array1)
    {
      batch_counter++;
      continue;
    }

    bool create_draw_call = false;
    if (draw_call && draw_call->backface_cull == backface_cull
        && draw_call->blend_mode == static_cast<int>(mat.blend_mode)
        && batch.index_start == draw_call->index_start + draw_call->index_count)
    {
      // identify if we can fit this batch into current draw_call
      unsigned n_required_slots = use_tex2 ? 2 : 1;
      unsigned n_avaliable_slots = static_cast<unsigned>(draw_call->samplers.size()) - draw_call->n_used_samplers;
      unsigned n_slots_to_be_occupied = 0;

      std::vector<int>::iterator it2;
      auto it = std::find(draw_call->samplers.begin(), draw_call->samplers.end(), _render_batches[batch_counter].tex_array0);

      if (it == draw_call->samplers.end())
      {
        if (n_avaliable_slots)
          n_slots_to_be_occupied++;
        else
          create_draw_call = true;
      }


      if (!create_draw_call && use_tex2)
      {
        it2 = std::find(draw_call->samplers.begin(), draw_call->samplers.end(), _render_batches[batch_counter].tex_array1);

        if (it2 == draw_call->samplers.end())
        {
          if (n_slots_to_be_occupied < n_avaliable_slots)
            n_slots_to_be_occupied++;
          else
            create_draw_call = true;
        }

      }

      if (!create_draw_call)
      {
        if (it != draw_call->samplers.end())
        {
          _render_batches[batch_counter].tex_array0 = it - draw_call->samplers.begin();
        }
        else
        {
          draw_call->samplers[draw_call->n_used_samplers] = _render_batches[batch_counter].tex_array0;
          _render_batches[batch_counter].tex_array0 = draw_call->n_used_samplers;
          draw_call->n_used_samplers++;
        }

        if (use_tex2)
        {
          if (it2 != draw_call->samplers.end())
          {
            _render_batches[batch_counter].tex_array1 = it2 - draw_call->samplers.begin();
          }
          else
          {
            draw_call->samplers[draw_call->n_used_samplers] = _render_batches[batch_counter].tex_array1;
            _render_batches[batch_counter].tex_array1 = draw_call->n_used_samplers;
            draw_call->n_used_samplers++;
          }
        }
      }

    }
    else
    {
      create_draw_call = true;
    }

    if (create_draw_call)
    {
      // create new combined draw call
      draw_call = &_draw_calls.emplace_back();
      draw_call->samplers = std::vector<int>{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
      draw_call->index_start = batch.index_start;
      draw_call->index_count = 0;
      draw_call->n_used_samplers = use_tex2 ? 2 : 1;
      draw_call->backface_cull = backface_cull;
      draw_call->blend_mode = static_cast<int>(mat.blend_mode);

      draw_call->samplers[0] = _render_batches[batch_counter].tex_array0;
      _render_batches[batch_counter].tex_array0 = 0;

      if (use_tex2)
          [[unlikely]]
      {
        draw_call->samplers[1] = _render_batches[batch_counter].tex_array1;
        _render_batches[batch_counter].tex_array1 = 1;
      }

    }

    draw_call->index_count += batch.index_count;

    // Per-batch cull span (D4): remember this batch's index range + local AABB inside the merged
    // draw call. The box is computed from the batch's own vertex range -- same data as the authored
    // MOBA int16 box, but guaranteed to be in VBO space.
    {
      WMOBatchSpan& span = draw_call->spans.emplace_back();
      span.index_start = batch.index_start;
      span.index_count = batch.index_count;
      // min > max (the untouched sentinel) marks a span with no vertex data: never culled.
      span.aabb_min = glm::vec3(std::numeric_limits<float>::max());
      span.aabb_max = glm::vec3(std::numeric_limits<float>::lowest());
      if (!_wmo_group->_vertices.empty())
      {
        std::size_t const vert_end = std::min(static_cast<std::size_t>(batch.vertex_end),
                                              _wmo_group->_vertices.size() - 1);
        for (std::size_t v = batch.vertex_start; v <= vert_end; ++v)
        {
          span.aabb_min = glm::min(span.aabb_min, _wmo_group->_vertices[v]);
          span.aabb_max = glm::max(span.aabb_max, _wmo_group->_vertices[v]);
        }
      }
    }

    batch_counter++;
  }

  // opengl resources
  _vertex_array.upload();
  _buffers.upload();
  gl.genTextures(1, &_render_batch_tex);

  gl.bufferData<GL_ARRAY_BUFFER> ( _vertices_buffer
      , _wmo_group->_vertices.size() * sizeof (*_wmo_group->_vertices.data())
      , _wmo_group->_vertices.data()
      , GL_STATIC_DRAW
  );

  gl.bufferData<GL_ARRAY_BUFFER> ( _normals_buffer
      , _wmo_group->_normals.size() * sizeof (*_wmo_group->_normals.data())
      , _wmo_group->_normals.data()
      , GL_STATIC_DRAW
  );

  gl.bufferData<GL_ARRAY_BUFFER> ( _texcoords_buffer
      , _wmo_group->_texcoords.size() * sizeof (*_wmo_group->_texcoords.data())
      , _wmo_group->_texcoords.data()
      , GL_STATIC_DRAW
  );

  gl.bufferData<GL_ARRAY_BUFFER> ( _render_batch_mapping_buffer
      , _render_batch_mapping.size() * sizeof(unsigned)
      , _render_batch_mapping.data()
      , GL_STATIC_DRAW
  );

  gl.bindBuffer(GL_TEXTURE_BUFFER, _render_batch_tex_buffer);
  gl.bufferData(GL_TEXTURE_BUFFER, _render_batches.size() * sizeof(WMORenderBatch),_render_batches.data(), GL_STATIC_DRAW);
  gl.bindTexture(GL_TEXTURE_BUFFER, _render_batch_tex);
  gl.texBuffer(GL_TEXTURE_BUFFER,  GL_RGBA32UI, _render_batch_tex_buffer);

  gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>(_indices_buffer, _wmo_group->_indices, GL_STATIC_DRAW);

  if (_wmo_group->header.flags.has_two_motv)
  {
    gl.bufferData<GL_ARRAY_BUFFER, glm::vec2> ( _texcoords_buffer_2
        , _wmo_group->_texcoords_2
        , GL_STATIC_DRAW
    );
  }

  gl.bufferData<GL_ARRAY_BUFFER> ( _vertex_colors_buffer
      , _wmo_group->_vertex_colors.size() * sizeof (*_wmo_group->_vertex_colors.data())
      , _wmo_group->_vertex_colors.data()
      , GL_STATIC_DRAW
  );

  // free unused data
  _wmo_group->_normals.clear();
  _wmo_group->_texcoords.clear();
  _wmo_group->_texcoords_2.clear();
  _wmo_group->_vertex_colors.clear();
  _render_batches.clear();
  _render_batch_mapping.clear();

  _uploaded = true;
}

void WMOGroupRender::unload()
{
  _vertex_array.unload();
  _buffers.unload();

  gl.deleteTextures(1, &_render_batch_tex);

  _uploaded = false;
  _vao_is_setup = false;
}

void WMOGroupRender::setupVao(OpenGL::Scoped::use_program& wmo_shader)
{
  OpenGL::Scoped::index_buffer_manual_binder indices (_indices_buffer);
  {
    OpenGL::Scoped::vao_binder const _ (_vao);

    wmo_shader.attrib("position", _vertices_buffer, 3, GL_FLOAT, GL_FALSE, 0, 0);
    wmo_shader.attrib("normal", _normals_buffer, 3, GL_FLOAT, GL_FALSE, 0, 0);
    wmo_shader.attrib("texcoord", _texcoords_buffer, 2, GL_FLOAT, GL_FALSE, 0, 0);
    wmo_shader.attribi("batch_mapping", _render_batch_mapping_buffer, 1, GL_UNSIGNED_INT, 0, 0);

    if (_wmo_group->header.flags.has_two_motv)
    {
      wmo_shader.attrib("texcoord_2", _texcoords_buffer_2, 2, GL_FLOAT, GL_FALSE, 0, 0);
    }

    // even if the 2 flags are set there's only one vertex color vector, the 2nd chunk is used for alpha only
    if (_wmo_group->header.flags.has_vertex_color || _wmo_group->header.flags.use_mocv2_for_texture_blending)
    {
      wmo_shader.attrib("vertex_color", _vertex_colors_buffer, 4, GL_FLOAT, GL_FALSE, 0, 0);
    }

    indices.bind();
  }

  _vao_is_setup = true;
}

void WMOGroupRender::draw(OpenGL::Scoped::use_program& wmo_shader
    , math::frustum const& frustum
    , glm::mat4x4 const& transform
    , const float& //cull_distance
    , const glm::vec3& //camera
    , bool // draw_fog
    , bool // world_has_skies
)
{
  if (!_uploaded)
  [[unlikely]]
  {
    upload();

    if (!_uploaded)
    [[unlikely]]
    {
      return;
    }
  }

  if (!_vao_is_setup)
  [[unlikely]]
  {
    setupVao(wmo_shader);
  }

  OpenGL::Scoped::vao_binder const _ (_vao);

  gl.activeTexture(GL_TEXTURE0);
  gl.bindTexture(GL_TEXTURE_BUFFER, _render_batch_tex);

  bool backface_cull = true;
  gl.enable(GL_CULL_FACE);

  // Per-batch frustum cull (D4, client MOBA semantics): inside a merged draw call, test each member
  // batch's AABB (instance-transformed) and emit only the contiguous runs of visible batches. Runs
  // stay contiguous because batches only merge when their index ranges are adjacent.
  bool const cull_spans = !moba_cull_disabled();
  static std::vector<std::pair<std::uint32_t, std::uint32_t>> visible_runs;

  auto span_visible = [&](WMOBatchSpan const& span)
  {
    if (span.aabb_min.x > span.aabb_max.x) // no-vertex-data sentinel
    {
      return true;
    }

    std::array<glm::vec3, 8> const world_corners =
    {
      transform * glm::vec4(span.aabb_min.x, span.aabb_min.y, span.aabb_min.z, 1.0f),
      transform * glm::vec4(span.aabb_min.x, span.aabb_min.y, span.aabb_max.z, 1.0f),
      transform * glm::vec4(span.aabb_min.x, span.aabb_max.y, span.aabb_min.z, 1.0f),
      transform * glm::vec4(span.aabb_min.x, span.aabb_max.y, span.aabb_max.z, 1.0f),
      transform * glm::vec4(span.aabb_max.x, span.aabb_min.y, span.aabb_min.z, 1.0f),
      transform * glm::vec4(span.aabb_max.x, span.aabb_min.y, span.aabb_max.z, 1.0f),
      transform * glm::vec4(span.aabb_max.x, span.aabb_max.y, span.aabb_min.z, 1.0f),
      transform * glm::vec4(span.aabb_max.x, span.aabb_max.y, span.aabb_max.z, 1.0f)
    };

    return frustum.intersects(world_corners);
  };

  auto issue_draw_call = [&](WMOCombinedDrawCall& draw_call)
  {
    visible_runs.clear();

    if (!cull_spans || draw_call.spans.empty())
    {
      visible_runs.emplace_back(draw_call.index_start, draw_call.index_count);
    }
    else
    {
      for (auto const& span : draw_call.spans)
      {
        if (!span_visible(span))
        {
          continue;
        }

        if (!visible_runs.empty()
            && visible_runs.back().first + visible_runs.back().second == span.index_start)
        {
          visible_runs.back().second += span.index_count;
        }
        else
        {
          visible_runs.emplace_back(span.index_start, span.index_count);
        }
      }

      if (visible_runs.empty())
      {
        return; // whole draw call off-screen -- skip the state changes too
      }
    }

    if (backface_cull != draw_call.backface_cull)
    {
      if (draw_call.backface_cull)
      {
        gl.enable(GL_CULL_FACE);
      }
      else
      {
        gl.disable(GL_CULL_FACE);
      }

      backface_cull = draw_call.backface_cull;
    }

    for(std::size_t i = 0; i < draw_call.samplers.size(); ++i)
    {
      if (draw_call.samplers[i] < 0)
        break;

      gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + 1 + i));
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, draw_call.samplers[i]);
    }

    for (auto const& run : visible_runs)
    {
      gl.drawElements (GL_TRIANGLES, run.second, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(sizeof(std::uint16_t)*run.first));
    }
  };

  // Fixed-function fog colour trick (trace-verified: #000000 / #FFFFFF fog states in the client):
  // additive materials fog toward BLACK and modulate materials toward WHITE so distance fog fades
  // their contribution instead of tinting it. 0 = normal fog colour.
  int fog_color_mode = 0;
  wmo_shader.uniform("fog_color_mode", 0);

  // Pass 1: opaque + alpha-key materials (blend modes 0/1) -- depth write on, no GL blend, as before.
  for (auto& draw_call : _draw_calls)
  {
    if (draw_call.blend_mode > 1)
    {
      continue;
    }
    issue_draw_call(draw_call);
  }

  // Pass 2: blended materials (additive / alpha / modulate -- e.g. the skybox-mimic "globe" domes and
  // glow geometry). These were previously drawn opaque, so additive materials rendered as flat, dim
  // surfaces instead of brightening/bleeding over what's behind them. Apply the material's actual GL
  // blend and stop writing depth so they composite over the opaque scene. Additive is order-
  // independent; alpha/mod can have minor ordering artifacts without a full sort, but that's still a
  // big improvement over rendering them opaque.
  bool has_blended = false;
  for (auto const& draw_call : _draw_calls)
  {
    if (draw_call.blend_mode > 1) { has_blended = true; break; }
  }

  if (has_blended)
  {
    gl.enable(GL_BLEND);
    gl.depthMask(GL_FALSE);

    for (auto& draw_call : _draw_calls)
    {
      if (draw_call.blend_mode <= 1)
      {
        continue;
      }

      switch (draw_call.blend_mode)
      {
        case 2:  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break; // alpha
        case 3:  gl.blendFunc(GL_SRC_ALPHA, GL_ONE);                 break; // additive
        case 4:  gl.blendFunc(GL_DST_COLOR, GL_ZERO);                break; // modulate
        case 5:  gl.blendFunc(GL_DST_COLOR, GL_SRC_COLOR);           break; // mod2x
        default: gl.blendFunc(GL_SRC_ALPHA, GL_ONE);                 break; // unknown -> additive
      }

      int const wanted_mode = draw_call.blend_mode == 3 ? 1
                            : (draw_call.blend_mode == 4 || draw_call.blend_mode == 5) ? 2
                            : 0;
      if (wanted_mode != fog_color_mode)
      {
        fog_color_mode = wanted_mode;
        wmo_shader.uniform("fog_color_mode", fog_color_mode);
      }

      issue_draw_call(draw_call);
    }

    gl.depthMask(GL_TRUE);
    gl.disable(GL_BLEND);

    if (fog_color_mode != 0)
    {
      wmo_shader.uniform("fog_color_mode", 0);
    }
  }

}

void WMOGroupRender::initRenderBatches()
{
  _render_batch_mapping.resize(_wmo_group->_vertices.size());
  std::fill(_render_batch_mapping.begin(), _render_batch_mapping.end(), 0);

  _render_batches.resize(_wmo_group->_batches.size());

  int interior_batches_start = 0;
  if (_wmo_group->header.transparency_batches_count > 0)
  {
    interior_batches_start =
      _wmo_group->_batches[_wmo_group->header.transparency_batches_count - 1].vertex_end + 1;
  }

  std::size_t batch_counter = 0;
  for (auto& batch : _wmo_group->_batches)
  {
    for (std::size_t i = 0; i < (batch.vertex_end - batch.vertex_start + 1); ++i)
    {
      _render_batch_mapping[batch.vertex_start + i] = static_cast<unsigned>(batch_counter + 1);
    }

    std::uint32_t flags = 0;

    bool const group_exterior_lit =
      _wmo_group->header.flags.exterior_lit || _wmo_group->header.flags.exterior;
    bool const has_mocv =
      _wmo_group->header.flags.has_vertex_color || _wmo_group->header.flags.use_mocv2_for_texture_blending;

    bool const batch_is_exterior = static_cast<int>(batch.vertex_start) < interior_batches_start;

    // Route ExteriorLit PER BATCH within an exterior-lit group. NOTE: the genuine exterior batches (the
    // opening-facing reveal/jamb faces) are deliberately NOT sun-lit here -- they face down/sideways with
    // no direct sun (nDotL=0), so the exterior branch would render them near-black. They are interior
    // batches lit by their baked warm MOCV instead (Goldshire doorway reveal is authored 255,168,85, which
    // the un-halved fixup now renders bright). Only fully-exterior no-MOCV groups get the whole-group flag.
    if (group_exterior_lit && (!has_mocv || batch_is_exterior))
    {
      flags |= WMORenderBatchFlags::eWMOBatch_ExteriorLit;
    }
    if (has_mocv)
    {
      flags |= WMORenderBatchFlags::eWMOBatch_HasMOCV;
    }
    if (_wmo_group->_has_portal_openness)
    {
      flags |= WMORenderBatchFlags::eWMOBatch_PortalSpill;
    }

    if (batch.texture >= _wmo_group->wmo->materials.size())
    {
      _render_batches[batch_counter] = WMORenderBatch{0, 0, 0, 0, 0, 0, 0, 0};
      batch_counter++;
      continue;
    }

    WMOMaterial const& mat (_wmo_group->wmo->materials[batch.texture]);

    if (mat.flags.unlit)
    {
      flags |= WMORenderBatchFlags::eWMOBatch_Unlit;
    }

    if (mat.flags.unfogged)
    {
      flags |= WMORenderBatchFlags::eWMOBatch_Unfogged;
    }

    if (mat.flags.sidn)
    {
      // Self-Illuminated Day/Night: building windows (and similar) emit their own texture colour,
      // ramping up as the outdoor light fades. The shader adds the night-glow emissive term.
      flags |= WMORenderBatchFlags::eWMOBatch_Sidn;
    }

    if (mat.flags.window)
    {
      // F_WINDOW: the client swaps the hardware light to a dedicated window pair around these
      // batches (wow.exe @006b5190/@006d37e0, note 28) -- flatter, faintly lifted lighting.
      flags |= WMORenderBatchFlags::eWMOBatch_Window;
    }

    if (mat.flags.clamp_s)
    {
      flags |= WMORenderBatchFlags::eWMOBatch_ClampS;
    }

    if (mat.flags.clamp_t)
    {
      flags |= WMORenderBatchFlags::eWMOBatch_ClampT;
    }

    std::uint32_t alpha_test;

    switch (mat.blend_mode)
    {
      case 1:
        alpha_test = 1; // 224/255
        break;
      case 2:
      case 3:
      case 4:
      case 5:
      case 6:
        alpha_test = 2;
        break;
      case 0:
      default:
        alpha_test = 0;
        break;
    }

    _render_batches[batch_counter] = WMORenderBatch{flags, mat.shader, 0, 0, 0, 0, alpha_test, 0};

    batch_counter++;
  }
}
