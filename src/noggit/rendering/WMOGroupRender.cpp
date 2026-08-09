// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "WMOGroupRender.hpp"
#include <atomic> // [TEXARRAYDBG] temporary
#include <sstream> // [TEXARRAYDBG] temporary
#include <vector> // [TEXBINDDBG] temporary
#include <string> // [TEXBINDDBG] temporary
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

    // [TEXARRAYDBG 2026-07-30] temporary: what does this batch sample? Compare (array,layer) against the
    // TEXARRAYDBG upload lines to see whether the layer really belongs to this material's texture.
    {
      static std::atomic<int> dbg{0};
      bool const of_interest = _wmo_group->wmo->file_key().hasFilepath()
        && _wmo_group->wmo->file_key().filepath().find("icebreaker") != std::string::npos;
      if (of_interest && dbg.fetch_add(1) < 200)
      {
        LogError << "[TEXARRAYDBG] batch wmo='" << _wmo_group->wmo->file_key().stringRepr()
                 << "' mat=" << batch.texture
                 << " shader=" << mat.shader
                 << " flags=0x" << std::hex << mat.flags.value << std::dec
                 << " blend=" << static_cast<int>(mat.blend_mode)
                 << " tex1_file='" << tex1->file_key().stringRepr() << "'"
                 << " array0=" << tex_array0 << " layer0=" << array_index0
                 << " array1=" << tex_array1 << " layer1=" << array_index1
                 << std::endl;
      }
    }

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

    // [TEXARRAYDBG 2026-07-30] temporary: the slot->array mapping this batch ends up sampling. With
    // N_ARRAY_TEX==1 every texture owns a single-layer array, so a wrong SLOT is the only way a batch can
    // sample a different texture. Any slot that is still -1 has nothing bound -> undefined sample.
    {
      static std::atomic<int> dbg{0};
      bool const of_interest = _wmo_group->wmo->file_key().hasFilepath()
        && _wmo_group->wmo->file_key().filepath().find("icebreaker") != std::string::npos;
      if (of_interest && dbg.fetch_add(1) < 200)
      {
        std::ostringstream slot_list; // NOT named 'slots': Qt defines that as a macro
        for (std::size_t s = 0; s < draw_call->samplers.size(); ++s)
        {
          if (s) slot_list << ",";
          slot_list << draw_call->samplers[s];
        }
        LogError << "[TEXARRAYDBG] drawcall wmo='" << _wmo_group->wmo->file_key().stringRepr()
                 << "' mat=" << batch.texture
                 << " shader=" << mat.shader
                 << " use_tex2=" << (use_tex2 ? 1 : 0)
                 << " newcall=" << (create_draw_call ? 1 : 0)
                 << " slot0=" << _render_batches[batch_counter].tex_array0
                 << " slot1=" << _render_batches[batch_counter].tex_array1
                 << " n_used=" << draw_call->n_used_samplers
                 << " samplers=[" << slot_list.str() << "]" << std::endl;
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

    // [TEXBINDDBG 2026-07-30] temporary: read back what is ACTUALLY bound to each texture unit right before
    // the draw, and the sampler-array uniform values. The shader proves slot 0 samples correctly while slot 1
    // returns garbage even at a fixed UV, so either unit 2 holds a different texture than the draw call
    // recorded, or the sampler uniform doesn't map slot 1 -> unit 2.
    {
      static std::atomic<int> dbg{0};
      bool const of_interest = _wmo_group->wmo->file_key().hasFilepath()
        && _wmo_group->wmo->file_key().filepath().find("icebreaker") != std::string::npos;
      if (of_interest && dbg.fetch_add(1) < 12)
      {
        std::ostringstream o;
        o << "[TEXBINDDBG] drawcall n_used=" << draw_call.n_used_samplers << " expected=[";
        for (std::size_t i = 0; i < draw_call.samplers.size() && draw_call.samplers[i] >= 0; ++i)
        {
          if (i) o << ",";
          o << draw_call.samplers[i];
        }
        o << "] actually_bound=[";
        for (int unit = 1; unit <= 6; ++unit)
        {
          GLint bound = 0;
          gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
          gl.getIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &bound);
          if (unit > 1) o << ",";
          o << "u" << unit << ":" << bound;
        }
        o << "]";

        // Read the ENV slot's texture straight off the GPU. Everything else checks out (right name bound,
        // right file uploaded, no GL errors), yet sampling it at a fixed (0.5,0.5) returns green while
        // wr_env's centre is (164,174,180). WIDTH==0 would mean the object has no storage at all.
        if (auto* f = gl._4_1_core_func)
        {
          gl.activeTexture(GL_TEXTURE0 + 2);
          GLint tw = 0, th = 0, td = 0, tfmt = 0;
          f->glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &tw);
          f->glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &th);
          f->glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_DEPTH, &td);
          f->glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_INTERNAL_FORMAT, &tfmt);
          o << " unit2_tex: " << tw << "x" << th << " layers=" << td << " fmt=" << tfmt;
          if (tw > 0 && th > 0 && td > 0)
          {
            std::vector<unsigned char> px(static_cast<std::size_t>(tw) * th * td * 4, 0);
            f->glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            auto at = [&](int x, int y) -> std::string
            {
              std::size_t const i = (static_cast<std::size_t>(y) * tw + x) * 4;
              std::ostringstream p;
              p << "(" << int(px[i]) << "," << int(px[i+1]) << "," << int(px[i+2]) << ")";
              return p.str();
            };
            o << " centre=" << at(tw/2, th/2) << " topleft=" << at(0, 0) << " q=" << at(tw/4, th/4);
          }
        }

        // The texture at unit 2 reads back CORRECT (128x128, centre = wr_env's real centre), yet sampling
        // slot 1 returns green while slot 0 is fine. So check the sampler-array uniform itself: element i
        // MUST equal texture unit 1+i. Query each element BY NAME -- glUniform1iv on the array's base
        // location silently does nothing if the driver doesn't lay the elements out contiguously.
        if (auto* f = gl._4_1_core_func)
        {
          GLint prog = 0;
          gl.getIntegerv(GL_CURRENT_PROGRAM, &prog);
          o << " prog=" << prog << " texture_samplers=[";
          for (int i = 0; i < 4; ++i)
          {
            std::string const nm = "texture_samplers[" + std::to_string(i) + "]";
            GLint const l = f->glGetUniformLocation(static_cast<GLuint>(prog), nm.c_str());
            GLint v = -999;
            if (l >= 0)
            {
              f->glGetUniformiv(static_cast<GLuint>(prog), l, &v);
            }
            if (i) o << ",";
            o << v << "@loc" << l;
          }
          o << "] (expect 1@..,2@..,3@..,4@..)";
        }

        LogError << o.str() << std::endl;
      }
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

  std::size_t batch_counter = 0;
  for (auto& batch : _wmo_group->_batches)
  {
    // Tag each vertex with the batch that draws it, so the shader can pick the batch's texture/flags per
    // fragment. WMO batches partition faces by INDEX range; a batch's [vertex_start..vertex_end] is only the
    // min/max vertex its indices REACH, NOT an exclusive owned span. Many custom WMOs (Turtle
    // world/custom/kttown/kttown_000.wmo -- 11 batches, ALL with vertex range [0..1614]) give every batch the
    // same whole-group vertex range, so filling [vertex_start..vertex_end] let each batch overwrite the ENTIRE
    // mapping and the LAST batch won for all 1614 verts -> the whole building sampled ONE texture (the reported
    // "all one texture" bug; only visible when a group's textures land in separate GL arrays so they can only
    // be told apart per-vertex). Walk the batch's own INDEX range and tag only the vertices its triangles use.
    // WMOs with exclusive vertex ranges (Stormwind etc.) get the identical result; verts shared across batches
    // at a seam resolve to the last writer -- negligible vs. the total collapse.
    for (std::size_t idx = batch.index_start;
         idx < static_cast<std::size_t>(batch.index_start) + batch.index_count && idx < _wmo_group->_indices.size();
         ++idx)
    {
      unsigned const vert = _wmo_group->_indices[idx];
      if (vert < _render_batch_mapping.size())
      {
        _render_batch_mapping[vert] = static_cast<unsigned>(batch_counter + 1);
      }
    }

    std::uint32_t flags = 0;

    // Real per-vertex LIGHTING is MOCV #1 (flag 0x4, has_vertex_color). The mocv2 flag (0x1000000,
    // use_mocv2_for_texture_blending) is a SECOND vertex-colour chunk whose RGB is 0 and whose ALPHA is
    // ONLY the two-layer texture-blend factor -- it is NOT lighting. Conflating them made modern WMOs that
    // carry ONLY a texture-blend mocv (e.g. Ascension's exterior city buildings: exterior 0x8 + mocv2
    // 0x1000000, NO 0x4) read HasMOCV=true with a black (0,0,0) vertex colour. Gate lighting on 0x4 only;
    // the blend alpha rides eWMOBatch_HasMOCVBlend so the two-layer blend still works without a lighting MOCV.
    bool const has_lighting_mocv = _wmo_group->header.flags.has_vertex_color;
    bool const has_blend_mocv =
      _wmo_group->header.flags.has_vertex_color || _wmo_group->header.flags.use_mocv2_for_texture_blending;

    // PER-BATCH interior/exterior lighting (CANON, client_re/17 + wow_cap_doorway_portal.trace, 2026-07-27).
    // A single MOGP group draws its INTERIOR batches MOCV-lit (LIGHTING=FALSE, real vertex colour) AND its
    // EXTERIOR batches sun-lit (LIGHTING=TRUE, white-placeholder MOCV) -- the split is by the batch's position
    // in the MOBA list, NOT a per-group flag. The MOGP batch counts order the list [transparency][interior]
    // [exterior]; a batch is EXTERIOR only if its index falls in the trailing exterior range. The OLD per-GROUP
    // test `flags & 0x48` sun-lit the WHOLE group, so a doorway REVEAL (an INTERIOR batch of a group that also
    // carries the 0x40 EXTERIOR_LIT flag / some exterior batches) got flooded with the outdoor sun -- bright,
    // and not dimming at night, exactly the reported doorway bug (the client draws that face interior MOCV,
    // measured ~0.4). Genuine exterior geometry stays sun-lit: real OUTDOOR groups (0x8) light every batch,
    // and any group's actual exterior batches light from the sun (harbor warehouse shells etc.).
    std::size_t const exterior_batch_start =
        static_cast<std::size_t>(_wmo_group->header.transparency_batches_count)
      + static_cast<std::size_t>(_wmo_group->header.interior_batch_count);
    bool const is_exterior_batch = batch_counter >= exterior_batch_start;
    if (_wmo_group->header.flags.exterior /* 0x8 OUTDOOR group -> all batches sun */ || is_exterior_batch)
    {
      flags |= WMORenderBatchFlags::eWMOBatch_ExteriorLit;
    }
    if (has_lighting_mocv)
    {
      flags |= WMORenderBatchFlags::eWMOBatch_HasMOCV;
    }
    if (has_blend_mocv)
    {
      flags |= WMORenderBatchFlags::eWMOBatch_HasMOCVBlend;
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

    // Env/EnvMetal material whose MOTX second entry is EMPTY: there is no environment map to reflect.
    if (batch.texture < _wmo_group->wmo->material_env_texture_missing.size()
        && _wmo_group->wmo->material_env_texture_missing[batch.texture])
    {
      flags |= WMORenderBatchFlags::eWMOBatch_NoEnvTexture;
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
