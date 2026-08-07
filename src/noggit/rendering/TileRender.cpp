// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/rendering/TileRender.hpp>
#include <noggit/MapTile.h>
#include <noggit/MapChunk.h>
#include <noggit/Model.h>
#include <noggit/SceneObject.hpp>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/frame_profiler.hpp>
#include <external/tracy/Tracy.hpp>

#include <glm/vec4.hpp>

#include <algorithm>
#include <vector>
#include <cstdlib>

using namespace Noggit::Rendering;

namespace
{
  // [perf 2026-08-06] Lazy per-chunk terrain streaming toggle + per-frame chunk budget. Dev A/B gate; OFF =
  // today's all-at-once upload. See the update block in TileRender::draw.
  bool lazy_chunk_upload_enabled()
  {
    static bool const on = []
    {
      char const* v = std::getenv("NOGGIT_LAZY_CHUNK_UPLOAD");
      return v && *v && *v != '0';
    }();
    return on;
  }
  int lazy_chunk_budget()
  {
    static int const n = []
    {
      char const* v = std::getenv("NOGGIT_LAZY_CHUNK_BUDGET");
      int const b = v ? std::atoi(v) : 32;
      return b > 0 ? b : 32;
    }();
    return n;
  }
}


TileRender::TileRender(MapTile* map_tile)
: _map_tile(map_tile)
{

}

void TileRender::upload()
{
  uploadTextures();
  _buffers.upload();

  gl.bindBuffer(GL_UNIFORM_BUFFER, _chunk_instance_data_ubo);
  gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::CHUNK_INSTANCE_DATA,
                     _chunk_instance_data_ubo, 0, sizeof(OpenGL::ChunkInstanceDataUniformBlock) * 256);
  gl.bufferData(GL_UNIFORM_BUFFER, sizeof(OpenGL::ChunkInstanceDataUniformBlock) * 256, NULL, GL_DYNAMIC_DRAW);

  MapTileDrawCall& draw_call = _draw_calls.emplace_back();
  draw_call.start_chunk = 0;
  draw_call.n_chunks = 256;
  std::fill(draw_call.samplers.begin(), draw_call.samplers.end(), -1);

  gl.genQueries(1, &_tile_occlusion_query);

  // The GPU textures were just (re)allocated, so every chunk row is empty -- re-register the full update set on
  // ALL 256 chunks so the next draw definitely refills them. Without this the fill depends on whatever per-chunk
  // flags happen to still be pending: MapChunk::endChunkUpdates() zeroes them once processed, and while VERTEX
  // gets re-registered from many places during normal use (so a missed heightmap pass self-heals), MCCV is only
  // ever re-registered by the MCCV paint tools -- so in a view-only session a missed MCCV pass left that row
  // permanently unwritten (the EPL psychedelic weave: undefined texels multiplied into terrain colour).
  // registerChunkUpdate propagates to the tile, which is what gates the update block in draw().
  for (int z = 0; z < 16; ++z)
  {
    for (int x = 0; x < 16; ++x)
    {
      _map_tile->mChunks[z][x]->registerChunkUpdate(ChunkUpdateFlags::VERTEX | ChunkUpdateFlags::ALPHAMAP
                                                    | ChunkUpdateFlags::SHADOW | ChunkUpdateFlags::MCCV
                                                    | ChunkUpdateFlags::NORMALS | ChunkUpdateFlags::HOLES
                                                    | ChunkUpdateFlags::AREA_ID | ChunkUpdateFlags::FLAGS);
    }
  }

  // [perf 2026-08-06] Begin a lazy per-chunk stream (if enabled): the draw update block uploads the 256 chunks
  // a budget at a time over several frames and draws only the ready prefix, instead of all-at-once.
  _lazy_streaming = lazy_chunk_upload_enabled();
  _lazy_cursor = 0;

  _uploaded = true;

}

void TileRender::unload()
{
  if (_uploaded)
  {
    _chunk_texture_arrays.unload();
    _buffers.unload();
    _uploaded = false;
    gl.deleteQueries(1, &_tile_occlusion_query);
  }


  freeDoodadInstanceBuffers();

  _map_tile->_chunk_update_flags = ChunkUpdateFlags::VERTEX | ChunkUpdateFlags::ALPHAMAP
                                  | ChunkUpdateFlags::SHADOW | ChunkUpdateFlags::MCCV
                                  | ChunkUpdateFlags::NORMALS| ChunkUpdateFlags::HOLES
                                  | ChunkUpdateFlags::AREA_ID| ChunkUpdateFlags::FLAGS;
}


// [perf 2026-08-05] Persistent per-model doodad instance buffers. Static tile M2 doodads are uploaded once
// per model (world transforms + a zero interior attribute -- tile doodads are outdoor, client MDDF) and the
// renderer draws the whole bucket every frame with tile-level cull + shader slice_dist clip, instead of
// re-culling + re-uploading each instance per frame. Rebuilt lazily when the tile's object set changes.
tsl::robin_map<Model*, TileRender::DoodadInstanceBuffer> const& TileRender::doodadInstanceBuffers()
{
  if (_map_tile->doodadBuffersDirty())
  {
    rebuildDoodadInstanceBuffers();
  }
  return _doodad_instance_buffers;
}

void TileRender::rebuildDoodadInstanceBuffers()
{
  freeDoodadInstanceBuffers();

  // [fix 2026-08-05] ALL-OR-NOTHING: build the persistent buffers only once EVERY eligible model on the tile
  // has finished loading. Until then leave the map EMPTY + the flag dirty, so the renderer skips nothing and
  // draws the tile via the DYNAMIC path -- which handles a partially-streamed tile gracefully (each instance
  // is drawn iff its model is ready). The old per-model build showed a multi-model structure in FRAGMENTS
  // while it streamed in (only the already-loaded pieces appeared). The switch to persistent is a clean
  // one-shot once the whole tile is up.
  for (auto const& pair : _map_tile->getObjectInstances())
  {
    if (pair.second.empty() || pair.second[0]->which() != eMODEL)
    {
      continue;
    }
    if (!reinterpret_cast<Model*>(pair.first)->finishedLoading())
    {
      return; // still streaming -- stay dirty, dynamic path draws it, retry next frame
    }
  }

  for (auto const& pair : _map_tile->getObjectInstances())
  {
    if (pair.second.empty() || pair.second[0]->which() != eMODEL)
    {
      continue;
    }

    Model* const model = reinterpret_cast<Model*>(pair.first);

    // Include ONLY models drawPersistent() will actually render (finished + not failed + not a classic-effect
    // shell + no particle/ribbon emitters). A model buffered here is SKIPPED in the dynamic gather, so if
    // drawPersistent then skipped it too the instances would render nowhere = the fragmented-building bug.
    // Non-eligible models fall through to the dynamic path (not added to the buffer -> not skipped there).
    if (!model->renderer()->eligibleForPersistentDraw())
    {
      continue;
    }

    std::vector<glm::mat4x4> transforms;
    transforms.reserve(pair.second.size());
    for (auto* instance : pair.second)
    {
      transforms.push_back(instance->transformMatrix());
    }
    if (transforms.empty())
    {
      continue;
    }
    // Interior is a per-instance attribute in the m2 instanced shader; tile doodads are outdoor so it is
    // all-zero. A dedicated (static) buffer keeps the same VAO attribute wiring the dynamic path uses and
    // is reused verbatim when this machinery is extended to WMO doodads (which carry real room colours).
    std::vector<glm::vec4> const interiors(transforms.size(), glm::vec4(0.0f));

    DoodadInstanceBuffer buf;
    buf.count = static_cast<GLsizei>(transforms.size());

    gl.genBuffers(1, &buf.transform_vbo);
    gl.bindBuffer(GL_ARRAY_BUFFER, buf.transform_vbo);
    gl.bufferData(GL_ARRAY_BUFFER, transforms.size() * sizeof(glm::mat4x4), transforms.data(), GL_STATIC_DRAW);

    gl.genBuffers(1, &buf.interior_vbo);
    gl.bindBuffer(GL_ARRAY_BUFFER, buf.interior_vbo);
    gl.bufferData(GL_ARRAY_BUFFER, interiors.size() * sizeof(glm::vec4), interiors.data(), GL_STATIC_DRAW);

    buf.cpu_transforms = std::move(transforms); // retained for the MDI batcher (see DoodadInstanceBuffer)
    _doodad_instance_buffers.emplace(model, std::move(buf));
  }

  gl.bindBuffer(GL_ARRAY_BUFFER, 0);

  // We only reach here once every eligible model was loaded (early-return above otherwise), so the buffers
  // are complete -- clear the dirty flag; rebuilds happen only on the next object-set change.
  _map_tile->clearDoodadBuffersDirty();
}

void TileRender::freeDoodadInstanceBuffers()
{
  for (auto& kv : _doodad_instance_buffers)
  {
    // Local copies: the map's mapped value is const through this iterator, and deleteBuffers wants GLuint*.
    GLuint t = kv.second.transform_vbo;
    GLuint i = kv.second.interior_vbo;
    if (t)
    {
      gl.deleteBuffers(1, &t);
    }
    if (i)
    {
      gl.deleteBuffers(1, &i);
    }
  }
  _doodad_instance_buffers.clear();
}


void TileRender::draw (OpenGL::Scoped::use_program& mcnk_shader
    , const glm::vec3& camera
    , bool show_unpaintable_chunks
    , bool draw_paintability_overlay
    , bool is_selected
)
{
  ZoneScopedN(NOGGIT_CURRENT_FUNCTION);

  static constexpr unsigned NUM_SAMPLERS = 11;

  if (!_map_tile || !_map_tile->finished.load() || _map_tile->loading_failed())
  [[unlikely]]
  {
    return;
  }

  if (!_uploaded)
  [[unlikely]]
  {
    // First draw of a freshly-streamed tile: allocate its GPU buffers/textures on the MAIN thread. This
    // + the 256-chunk update block below are the "chunk load" spike; attribute both to TileStream.
    noggit::perf::Scoped _prof_upload(noggit::perf::Phase::TileStream);
    upload();
  }

  bool alphamap_bound = false;
  bool heightmap_bound = false;
  bool shadowmap_bound = false;
  bool mccv_bound = false;

  _texture_not_loaded = false;

  // figure out if we need to update based on paintability
  bool need_paintability_update = false;
  if (_requires_paintability_recalc && draw_paintability_overlay && show_unpaintable_chunks)
  [[unlikely]]
  {
    auto cur_tex = Noggit::Ui::selected_texture::get();
    for (int j = 0; j < 16; ++j)
    {
      for (int i = 0; i < 16; ++i)
      {
        auto& chunk = _map_tile->mChunks[j][i];
        bool cant_paint = cur_tex && !chunk->canPaintTexture(*cur_tex);

        if (chunk->currently_paintable != !cant_paint)
        {
          chunk->currently_paintable = !cant_paint;
          _chunk_instance_data[i * 16 + j].ChunkHoles_DrawImpass_TexLayerCount_CantPaint[3] = cant_paint;
          need_paintability_update = true;
        }
      }
    }

    _requires_paintability_recalc = false;
  }

  // run chunk updates. running this when splitdraw call detected unused sampler configuration as well.
  if (_map_tile->_chunk_update_flags || is_selected != _selected || need_paintability_update || _requires_sampler_reset || _texture_not_loaded)
  {
    // Per-chunk texture/vertex uploads (texSubImage). A freshly-uploaded tile flags ALL 256 chunks at
    // once here -> the dominant part of the "chunk load" spike. Attributed to TileStream (nested in
    // Terrain). Incremental paint edits also pass through, but those touch few chunks and stay cheap.
    noggit::perf::Scoped _prof_chunk_upd(noggit::perf::Phase::TileStream);

    gl.bindBuffer(GL_UNIFORM_BUFFER, _chunk_instance_data_ubo);

    if (_requires_sampler_reset)
    [[unlikely]]
    {
      _draw_calls.clear();
      MapTileDrawCall& draw_call = _draw_calls.emplace_back();
      std::fill(draw_call.samplers.begin(), draw_call.samplers.end(), -1);
      draw_call.start_chunk = 0;
      draw_call.n_chunks = 256;
    }

    _selected = is_selected;

    // [perf 2026-08-06] LAZY streaming: process only a budget of chunks (in raster order) per frame, draw the
    // ready prefix, and defer clearing the tile update flag until all 256 are done -- spreading the TileStream
    // spike across frames (client-like progressive fill). Falls back to all-at-once (0..256) when not streaming
    // / split-sampler / paint edits / special passes. On a split detected mid-window we abort lazy and finish
    // every remaining chunk this frame (rare, heavily-textured tiles only).
    bool lazy_active = _lazy_streaming && !_split_drawcall && !_requires_sampler_reset
                    && !_texture_not_loaded && !need_paintability_update;
    int const win_start = lazy_active ? _lazy_cursor : 0;
    int win_end = lazy_active ? std::min(_lazy_cursor + lazy_chunk_budget(), 256) : 256;

    for (int i = win_start; i < win_end; ++i)
    {
      int chunk_x = i / 16;
      int chunk_y = i % 16;

      auto& chunk = _map_tile->mChunks[chunk_y][chunk_x];

      _chunk_instance_data[i].ChunkXZ_TileXZ[0] = chunk->px;
      _chunk_instance_data[i].ChunkXZ_TileXZ[1] = chunk->py;
      _chunk_instance_data[i].ChunkXZ_TileXZ[2] = static_cast<int>(_map_tile->index.x);
      _chunk_instance_data[i].ChunkXZ_TileXZ[3] = static_cast<int>(_map_tile->index.z);

      unsigned flags = chunk->getUpdateFlags();

      if (flags & ChunkUpdateFlags::ALPHAMAP || _requires_sampler_reset || _texture_not_loaded)
      {
        gl.activeTexture(GL_TEXTURE0 + 3);
        gl.bindTexture(GL_TEXTURE_2D_ARRAY, _alphamap_tex);
        alphamap_bound = true;
        chunk->texture_set->uploadAlphamapData();

        if (!_split_drawcall && !fillSamplers(chunk.get(), i, static_cast<unsigned int>(_draw_calls.size() - 1)))
        {
          _split_drawcall = true;
          if (lazy_active) { lazy_active = false; win_end = 256; } // abort lazy -> finish every chunk this frame
        }
      }

      if (!flags)
        continue;

      if (flags & ChunkUpdateFlags::VERTEX || flags & ChunkUpdateFlags::NORMALS)
      {
        heightmap_bound = true;
        if (flags & ChunkUpdateFlags::VERTEX)
        {
          chunk->updateVerticesData();
        }

        gl.activeTexture(GL_TEXTURE0 + 0);
        gl.bindTexture(GL_TEXTURE_2D, _height_tex);
        gl.texSubImage2D(GL_TEXTURE_2D, 0, 0, i, mapbufsize, 1, GL_RGBA,
                         GL_FLOAT, _map_tile->_chunk_heightmap_buffer.data() + i * mapbufsize * 4);
      }

      if (flags & ChunkUpdateFlags::MCCV)
      {
        mccv_bound = true;
        gl.activeTexture(GL_TEXTURE0 + 1);
        gl.bindTexture(GL_TEXTURE_2D, _mccv_tex);
        chunk->update_vertex_colors();
      }

      if (flags & ChunkUpdateFlags::SHADOW)
      {
        shadowmap_bound = true;
        gl.activeTexture(GL_TEXTURE0 + 2);
        gl.bindTexture(GL_TEXTURE_2D_ARRAY, _shadowmap_tex);
        chunk->update_shadows();
      }

      if (flags & ChunkUpdateFlags::HOLES)
      {
        _chunk_instance_data[i].ChunkHoles_DrawImpass_TexLayerCount_CantPaint[0] = chunk->holes;
      }

      if (flags & ChunkUpdateFlags::FLAGS)
      {
        _chunk_instance_data[i].ChunkHoles_DrawImpass_TexLayerCount_CantPaint[1] = chunk->header_flags.flags.impass;

        for (int k = 0; k < chunk->texture_set->num(); ++k)
        {
          unsigned layer_flags = chunk->texture_set->flag(k);
          auto flag_view = reinterpret_cast<MCLYFlags*>(&layer_flags);

          // bit 0 = UV animation enabled; bit 1 = MCLY 0x80 overbright ("way brighter, used for lava
          // to make it glow" -- vanilla MCLY bit, audit RE_notes/21 A1). The frag shader applies x2.
          _chunk_instance_data[i].ChunkTexDoAnim[k] = (flag_view->animation_enabled ? 1 : 0)
                                                    | (flag_view->overbright ? 2 : 0);
          _chunk_instance_data[i].ChunkTexAnimSpeed[k] = flag_view->animation_speed;
          _chunk_instance_data[i].ChunkTexAnimDir[k] = flag_view->animation_rotation;
        }
        // (Removed a stray `ChunkTexDoAnim[1] = impass` that stomped layer 1's anim flag -- the impass
        // overlay reads ChunkHoles_DrawImpass_TexLayerCount_CantPaint[1], set above.)
      }

      if (flags & ChunkUpdateFlags::AREA_ID)
      {
        _chunk_instance_data[i].AreaIDColor_Pad2_DrawSelection[0] = chunk->areaID;
      }

      _chunk_instance_data[i].AreaIDColor_Pad2_DrawSelection[3] = _selected;

      chunk->endChunkUpdates();

      if (_texture_not_loaded)
        chunk->registerChunkUpdate(ChunkUpdateFlags::ALPHAMAP);

    }

    _requires_sampler_reset = false;


    if (_split_drawcall)
    {
      _draw_calls.clear();
      MapTileDrawCall& draw_call = _draw_calls.emplace_back();
      std::fill(draw_call.samplers.begin(), draw_call.samplers.end(), -1);
      draw_call.start_chunk = 0;
      draw_call.n_chunks = 0;

      for (int i = 0; i < 256; ++i)
      {
        auto& chunk = _map_tile->mChunks[i % 16][i / 16];

        if (!fillSamplers(chunk.get(), i, static_cast<unsigned int>(_draw_calls.size() - 1)))
        {
          MapTileDrawCall& previous_draw_call = _draw_calls[_draw_calls.size() - 1];
          unsigned new_start = previous_draw_call.start_chunk + previous_draw_call.n_chunks;

          MapTileDrawCall& new_draw_call = _draw_calls.emplace_back();
          std::fill(new_draw_call.samplers.begin(), new_draw_call.samplers.end(), -1);
          new_draw_call.start_chunk = new_start;
          new_draw_call.n_chunks = 1;

          fillSamplers(chunk.get(), i, static_cast<unsigned int>(_draw_calls.size() - 1));
        }
        else
        {
          MapTileDrawCall& last_draw_call = _draw_calls.back();
          last_draw_call.n_chunks++;
          assert(last_draw_call.n_chunks <= 256);
        }

      }

      if (_draw_calls.size() <= 1)
      {
        _split_drawcall = false;
        _requires_sampler_reset = true;
      }
    }

    // [perf 2026-08-06] LAZY: draw only the ready prefix [0, cursor) and keep the tile flag PENDING (chunks
    // beyond the window are still flagged) so the update block resumes next frame; clear it only once fully
    // streamed. Non-lazy / aborted paths processed every chunk this frame -> clear now.
    if (lazy_active && !_split_drawcall)
    {
      _lazy_cursor = win_end;
      if (!_draw_calls.empty())
      {
        _draw_calls[0].start_chunk = 0;
        _draw_calls[0].n_chunks = static_cast<unsigned>(_lazy_cursor);
      }
      if (_lazy_cursor >= 256)
      {
        _lazy_streaming = false;
        _map_tile->endChunkUpdates();
      }
    }
    else
    {
      _lazy_streaming = false;
      _map_tile->endChunkUpdates();
    }

    // [wrong-texture fix 2026-08-07] Persist the retry across passes. _texture_not_loaded resets every pass,
    // so a miss inside an early LAZY window used to be forgotten by the final window: endChunkUpdates() above
    // cleared the tile flag with no re-arm, orphaning the affected chunks' re-registered ALPHAMAP retries --
    // their sampler entries stayed the -1 sentinel, which renders as a real (WRONG) texture (sampler slot 0,
    // non-specular layer 1) until a full reload. The latch keeps the tile-level flag armed every pass until a
    // FULL (non-lazy) pass refills every pending chunk (they keep their per-chunk ALPHAMAP flags) without a
    // single miss; only then does it release.
    if (_texture_not_loaded)
      _pending_tex_retry = true;
    else if (!lazy_active)
      _pending_tex_retry = false; // full pass, all pending chunks refilled, nothing missing -> resolved

    if (_texture_not_loaded || _pending_tex_retry)
      _map_tile->registerChunkUpdate(ChunkUpdateFlags::ALPHAMAP);

    gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(OpenGL::ChunkInstanceDataUniformBlock) * 256,
                     &_chunk_instance_data);
  }

  _map_tile->recalcExtents();

  gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::CHUNK_INSTANCE_DATA,
                     _chunk_instance_data_ubo, 0, sizeof(OpenGL::ChunkInstanceDataUniformBlock) * 256);


  for (auto& draw_call : _draw_calls)
  {

    if (!alphamap_bound)
    {
      gl.activeTexture(GL_TEXTURE0 + 3);
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, _alphamap_tex);
    }

    if (!shadowmap_bound)
    {
      gl.activeTexture(GL_TEXTURE0 + 2);
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, _shadowmap_tex);
    }

    if (!mccv_bound)
    {
      gl.activeTexture(GL_TEXTURE0 + 1);
      gl.bindTexture(GL_TEXTURE_2D, _mccv_tex);
    }

    if (!heightmap_bound)
    {
      gl.activeTexture(GL_TEXTURE0 + 0);
      gl.bindTexture(GL_TEXTURE_2D, _height_tex);
    }

    float tile_center_x = _map_tile->xbase + TILESIZE / 2.0f;
    float tile_center_z = _map_tile->zbase + TILESIZE / 2.0f;

    bool is_lod = misc::dist(tile_center_x, tile_center_z, camera.x, camera.z) > TILESIZE * 3;
    mcnk_shader.uniform("lod_level", int(is_lod));

    assert(draw_call.n_chunks <= 256);
    mcnk_shader.uniform("base_instance", static_cast<int>(draw_call.start_chunk));

    for (int i = 0; i < NUM_SAMPLERS; ++i)
    {
      gl.activeTexture(GL_TEXTURE0 + 5 + i);

      if (draw_call.samplers[i] < 0)
      {
        gl.bindTexture(GL_TEXTURE_2D_ARRAY, 0);
        continue;
      }

      gl.bindTexture(GL_TEXTURE_2D_ARRAY, draw_call.samplers[i]);
    }

    if (is_lod)
    {
      gl.drawElementsInstanced(GL_TRIANGLES, 192, GL_UNSIGNED_SHORT,
                               reinterpret_cast<void*>(768 * sizeof(std::uint16_t)), draw_call.n_chunks);
    }
    else
    {
      gl.drawElementsInstanced(GL_TRIANGLES, 768, GL_UNSIGNED_SHORT, nullptr,
                               draw_call.n_chunks);
    }

  }
}

void TileRender::uploadTextures()
{
  _chunk_texture_arrays.upload();
  gl.activeTexture(GL_TEXTURE0 + 0);
  gl.bindTexture(GL_TEXTURE_2D, _height_tex);
  gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, mapbufsize,
                256, 0, GL_RGBA, GL_FLOAT,nullptr);

  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  //gl.texParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  //gl.texParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  //gl.texParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  //const GLint swizzleMask[] = {GL_RED, GL_RED, GL_RED, GL_ONE};
  //gl.texParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzleMask);

  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  gl.bindTexture(GL_TEXTURE_2D, 0);

  gl.activeTexture(GL_TEXTURE0 + 2);
  gl.bindTexture(GL_TEXTURE_2D, _mccv_tex);
  // Initialize to WHITE (1,1,1 = the neutral vertex colour) instead of nullptr. With nullptr the contents are
  // UNDEFINED, so any chunk row that never runs update_vertex_colors samples garbage floats, and the frag
  // shader multiplies terrain by them (`out_color.rgb *= vary_mccv`) -> a psychedelic per-vertex weave. That is
  // exactly the EPL bug: every EPL ADT's MCCV is a uniform 127,127,127 (= 1.0 neutral, verified against
  // patch-3.mpq), so the file data can never tint anything -- the colour could only come from unwritten rows.
  // Rows that DO upload overwrite this, so a legitimate MCCV is unaffected; unwritten rows now read neutral.
  {
    std::vector<float> white(static_cast<std::size_t>(mapbufsize) * 256 * 3, 1.0f);
    gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, mapbufsize,
                  256, 0, GL_RGB, GL_FLOAT, white.data());
  }

  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  //gl.texParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  //gl.texParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  //gl.texParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  gl.bindTexture(GL_TEXTURE_2D, 0);

  gl.activeTexture(GL_TEXTURE0 + 4);
  gl.bindTexture(GL_TEXTURE_2D_ARRAY, _alphamap_tex);
  gl.texImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB, 64, 64,
                256, 0, GL_RGB, GL_FLOAT,nullptr);

  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl.bindTexture(GL_TEXTURE_2D_ARRAY, 0);


  gl.activeTexture(GL_TEXTURE0 + 3);
  gl.bindTexture(GL_TEXTURE_2D_ARRAY, _shadowmap_tex);
  gl.texImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RED, 64, 64, 256,
                0, GL_RED, GL_UNSIGNED_BYTE,nullptr);

  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl.bindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

void TileRender::doTileOcclusionQuery(OpenGL::Scoped::use_program& occlusion_shader)
{
  if (_tile_occlusion_query_in_use || !_uploaded)
    return;

  _tile_occlusion_query_in_use = true;
  gl.beginQuery(GL_ANY_SAMPLES_PASSED, _tile_occlusion_query);
  occlusion_shader.uniform("aabb", _map_tile->_combined_extents.data(), _map_tile->_combined_extents.size());
  gl.drawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
  gl.endQuery(GL_ANY_SAMPLES_PASSED);
}

bool TileRender::getTileOcclusionQueryResult(glm::vec3 const& camera)
{
  // returns true if tile is not occluded by other tiles

  if (!_tile_occlusion_query_in_use)
  [[unlikely]]
  {
    return !_tile_occluded;
  }

  if (!_uploaded)
    return !_tile_occluded;

  if (misc::pointInside(camera, _map_tile->_combined_extents))
  {
    _tile_occlusion_query_in_use = false;
    return true;
  }

  GLint result;
  gl.getQueryObjectiv(_tile_occlusion_query, GL_QUERY_RESULT_AVAILABLE, &result);

  if (result != GL_TRUE)
  {
    return _tile_occlusion_cull_override || !_tile_occluded;
  }

  if (!_tile_occlusion_cull_override)
  {
    gl.getQueryObjectiv(_tile_occlusion_query, GL_QUERY_RESULT, &result);
  }
  else
  {
    result = true;
  }

  _tile_occlusion_query_in_use = false;
  _tile_occlusion_cull_override = false;

  return static_cast<bool>(result);
}


bool TileRender::fillSamplers(MapChunk* chunk, unsigned chunk_index,  unsigned int draw_call_index)
{
  MapTileDrawCall& draw_call = _draw_calls[draw_call_index];

  static constexpr unsigned NUM_LAYERS = 4;
  static constexpr unsigned NUM_SAMPLERS = 11;

  auto const n_render_layers = std::min<std::size_t>(chunk->texture_set->num(), NUM_LAYERS);
  _chunk_instance_data[chunk_index].ChunkHoles_DrawImpass_TexLayerCount_CantPaint[2] = static_cast<int>(n_render_layers);

  for (unsigned k = 0; k < NUM_LAYERS; ++k)
  {
    _chunk_instance_data[chunk_index].ChunkTextureSamplers[k] = 0;
    _chunk_instance_data[chunk_index].ChunkTextureArrayIDs[k] = -1;
  }


  auto& chunk_textures = (*chunk->texture_set->getTextures());
  for (std::size_t k = 0; k < n_render_layers; ++k)
  {
    chunk_textures[k]->upload();

    if (!chunk_textures[k]->is_uploaded())
    {
      _texture_not_loaded = true;
      continue;
    }

    GLuint tex_array = (*chunk->texture_set->getTextures())[k]->texture_array();
    int tex_index = (*chunk->texture_set->getTextures())[k]->array_index();

    int sampler_id = -1;
    for (int n = 0; n < draw_call.samplers.size(); ++n)
    {
      if (draw_call.samplers[n] == tex_array)
      {
        sampler_id = n;
        break;
      }
      else if (draw_call.samplers[n] < 0)
      {
        draw_call.samplers[n] = tex_array;
        sampler_id = n;
        break;
      }
    }

    // If there are not enough sampler slots (11) we have to split the drawcall :(.
    // Extremely infrequent for terrain. Never for Blizzard terrain as their tilesets
    // use uniform BLP format per map.
    if (sampler_id < 0)
    [[unlikely]]
    {
      return false;
    }

    _chunk_instance_data[chunk_index].ChunkTextureSamplers[k] = sampler_id;
    _chunk_instance_data[chunk_index].ChunkTextureArrayIDs[k] = (*chunk->texture_set->getTextures())[k]->is_specular() ? tex_index : -tex_index;

  }

  return true;
}

void TileRender::initChunkData(MapChunk* chunk)
{
  auto& chunk_render_instance = _chunk_instance_data[chunk->px * 16 + chunk->py];

  chunk_render_instance.ChunkHoles_DrawImpass_TexLayerCount_CantPaint[0] = chunk->holes;
  chunk_render_instance.ChunkHoles_DrawImpass_TexLayerCount_CantPaint[1] = chunk->header_flags.flags.impass;
  chunk_render_instance.ChunkHoles_DrawImpass_TexLayerCount_CantPaint[2] = static_cast<int>(chunk->texture_set->num());
  chunk_render_instance.ChunkHoles_DrawImpass_TexLayerCount_CantPaint[3] = 0;
  chunk_render_instance.AreaIDColor_Pad2_DrawSelection[0] = chunk->areaID;
  chunk_render_instance.AreaIDColor_Pad2_DrawSelection[3] = 0;
}
