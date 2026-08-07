// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TILERENDER_HPP
#define NOGGIT_TILERENDER_HPP

#include <noggit/rendering/BaseRender.hpp>
#include <opengl/scoped.hpp>
#include <opengl/shader.hpp>
#include <array>

#include <external/tsl/robin_map.h>
#include <glm/mat4x4.hpp>

class MapTile;
class MapChunk;
class Model;

namespace Noggit::Rendering
{
  struct MapTileDrawCall
  {
    std::array<int, 11> samplers;
    unsigned start_chunk;
    unsigned n_chunks;
  };

  class TileRender : public BaseRender
  {
  public:
    explicit TileRender(MapTile* map_tile);

    void upload() override;
    void unload() override;

    void draw (OpenGL::Scoped::use_program& mcnk_shader
        , const glm::vec3& camera
        , bool show_unpaintable_chunks
        , bool draw_paintability_overlay
        , bool is_selected
    );

    void doTileOcclusionQuery(OpenGL::Scoped::use_program& occlusion_shader);
    bool getTileOcclusionQueryResult(glm::vec3 const& camera);
    void discardTileOcclusionQuery() { _tile_occlusion_query_in_use = false; }
    void notifyTileRendererOnSelectedTextureChange() { _requires_paintability_recalc = true; };

    void initChunkData(MapChunk* chunk);

    [[nodiscard]]
    GLuint shadowmapTexture() const { return _shadowmap_tex; }

    [[nodiscard]]
    unsigned objectsFrustumCullTest() const { return _objects_frustum_cull_test; };
    void setObjectsFrustumCullTest(unsigned state) { _objects_frustum_cull_test = state; };

    [[nodiscard]]
    bool isOccluded() const { return _tile_occluded; } ;
    void setOccluded(bool state) { _tile_occluded = state; };

    [[nodiscard]]
    bool isFrustumCulled() const{ return _tile_frustum_culled; };
    void setFrustumCulled(bool state) {_tile_frustum_culled = state; };

    [[nodiscard]]
    bool isOverridingOcclusionCulling() const { return _tile_occlusion_cull_override; };
    void setOverrideOcclusionCulling(bool state) { _tile_occlusion_cull_override = state; };

    // [perf 2026-08-05] Persistent per-model doodad instance buffers. A tile's static (non-per-instance-
    // animated) M2 doodads are uploaded ONCE per model to a GPU buffer here and reused every frame -- the
    // renderer draws the whole bucket (tile-level cull + shader slice_dist clip) instead of re-culling +
    // re-uploading every instance per frame. Rebuilt lazily from MapTile::object_instances whenever the
    // tile's object set changes (MapTile::doodadBuffersDirty()); freed in unload().
    struct DoodadInstanceBuffer
    {
      GLuint transform_vbo = 0;
      GLuint interior_vbo = 0;   // per-instance interior (all-zero for outdoor tile doodads; kept for WMO reuse)
      GLsizei count = 0;
      // [perf 2026-08-05] CPU copy of the same transforms (already computed during the rebuild) retained so the
      // MDI doodad batcher can concatenate a model's instances across tiles into one indirect-draw instance
      // buffer without a GPU readback or a per-frame transformMatrix() recompute. Unused when MDI is off.
      std::vector<glm::mat4x4> cpu_transforms;
    };
    // Rebuilds if the tile's object set changed since the last build; returns the current per-model buffers.
    [[nodiscard]] tsl::robin_map<Model*, DoodadInstanceBuffer> const& doodadInstanceBuffers();

  private:

    void uploadTextures();
    bool fillSamplers(MapChunk* chunk, unsigned chunk_index, unsigned draw_call_index);

    MapTile* _map_tile;

    bool _uploaded = false;
    bool _selected = false;
    bool _split_drawcall = false;
    bool _requires_sampler_reset = false;
    bool _requires_paintability_recalc = true;
    bool _texture_not_loaded = false;
    // [wrong-texture fix 2026-08-07] PERSISTENT retry latch. _texture_not_loaded is a per-UPDATE-PASS working
    // flag (reset each pass), so with LAZY streaming a texture miss in an early window was forgotten by the
    // final window: the tile flag got cleared with no re-arm, orphaning the chunk's re-registered ALPHAMAP
    // retry -> its sampler entry stayed the -1 sentinel, which the shader reads as a VALID "non-specular
    // layer 1" of sampler slot 0 -> a whole section rendered the WRONG texture until reload. This latch stays
    // set until a full (non-lazy) update pass resolves every pending chunk with no misses.
    bool _pending_tex_retry = false;

    // [perf 2026-08-06] Lazy per-chunk terrain streaming (NOGGIT_LAZY_CHUNK_UPLOAD). A freshly-loaded tile
    // uploads its 256 chunks a BUDGET at a time over several frames -- drawing only the ready prefix meanwhile
    // -- instead of all 256 in one frame, spreading the ~30-80ms TileStream spike into small per-frame costs
    // (client-like progressive fill). Falls back to all-at-once for split-sampler tiles / edits / special passes.
    bool _lazy_streaming = false;
    int  _lazy_cursor = 0; // chunks uploaded so far during the current lazy stream (0..256)

    // culling
    unsigned _objects_frustum_cull_test = 0;
    bool _tile_occluded = false;
    bool _tile_frustum_culled = true;
    bool _tile_occlusion_cull_override = true;

    // drawing
    std::vector<MapTileDrawCall> _draw_calls;

    OpenGL::Scoped::deferred_upload_textures<4> _chunk_texture_arrays;
    GLuint const& _height_tex = _chunk_texture_arrays[0];
    GLuint const& _mccv_tex = _chunk_texture_arrays[1];
    GLuint const& _shadowmap_tex = _chunk_texture_arrays[2];
    GLuint const& _alphamap_tex = _chunk_texture_arrays[3];

    GLuint _tile_occlusion_query;
    bool _tile_occlusion_query_in_use = false;

    OpenGL::Scoped::deferred_upload_buffers<1> _buffers;

    GLuint const& _chunk_instance_data_ubo = _buffers[0];
    OpenGL::ChunkInstanceDataUniformBlock _chunk_instance_data[256];

    // persistent per-model doodad instance buffers (raw GL names -- lifetime managed by rebuild/free below)
    tsl::robin_map<Model*, DoodadInstanceBuffer> _doodad_instance_buffers;
    void rebuildDoodadInstanceBuffers();
    void freeDoodadInstanceBuffers();

  };
}

#endif //NOGGIT_TILERENDER_HPP
