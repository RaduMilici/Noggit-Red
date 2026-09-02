// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_WORLDRENDER_HPP
#define NOGGIT_WORLDRENDER_HPP

#include <noggit/rendering/BaseRender.hpp>

#include <external/glm/glm.hpp>
#include <math/trig.hpp>

#include <set>
#include <map>
#include <functional>

namespace math { class frustum; } // referenced by the animated-MDI batching (drawDoodadsBatched/fillMdiBones)

#include <noggit/tool_enums.hpp>
#include <noggit/rendering/CursorRender.hpp>
#include <noggit/rendering/LiquidTextureManager.hpp>
#include <noggit/TextureManager.h>
#include <noggit/map_horizon.h>
#include <noggit/Sky.h>

#include <opengl/shader.hpp>
#include <opengl/types.hpp>                     // OpenGL::DrawElementsIndirectCommand (MDI doodad batching)
#include <noggit/rendering/Primitives.hpp>
#include <noggit/rendering/ModelRender.hpp>     // StaticBatchKey (MDI doodad batching)
#include <noggit/rendering/TileRender.hpp>      // TileRender::DoodadInstanceBuffer (MDI doodad batching)
#include <noggit/ModelInstance.h>
#include <noggit/InteriorVolume.hpp>

#include <QtCore/QElapsedTimer>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class World;
class WMO;
class WMOGroup;
struct MinimapRenderSettings;

namespace Noggit::Rendering
{
  // Per-frame acceleration structure for the legacy-creature-doodad overlap test. Maps a creature model
  // path to the spawn overlays that use it, so should_suppress_legacy_creature_instance() is an O(1)
  // lookup + tiny bucket scan instead of walking ALL ~35k creature spawns per creature instance per frame.
  struct LegacyOverlayInfo
  {
    glm::vec3 pos = glm::vec3(0.0f);
    float overlay_radius = 1.0f;
    std::uint32_t guid = 0;
    std::string overlay_model;
  };

  class WorldRender : public BaseRender
  {
  public:
    WorldRender(World* world);

    // [VULKAN phase A, 2026-08-29] optional compose hook: called by draw() right after the scene target
    // (MSAA/bloom FBO or the Qt framebuffer) has been cleared and BEFORE any scene geometry. MapView uses
    // it to write the Vulkan-rendered colour + depth as the base layer so the GL passes depth-test over
    // it (VK content sits UNDER the normal editor view). Null = no-op. Only for the main 3D viewport.
    std::function<void()> pre_scene_compose;
    // [VULKAN phase B] pass ownership gates. When a pass is owned by VK, draw() SKIPS the GL pass (the GL
    // code stays intact and runs unchanged with Graphics API = OpenGL -- gate, never delete).
    bool vk_owns_terrain = false;
    // [phase I] VK mode was rendering the WHOLE SCENE TWICE. VK drew terrain, WMO, M2, water, sky,
    // clouds and particles, but on the GL side only terrain/WMO/sky/clouds/particles/celestials were
    // ever gated off -- the M2 and water passes had NO gate at all, so both APIs drew every model and
    // every water surface every frame, with a full CPU sync in between. The parity harness could not
    // see it: parity mode deliberately keeps the full GL scene as the reference image.
    //
    // These gate GL's DRAW only. The recording side effects stay, because the VK feed is built out of
    // them -- same rule as g_vk_owns_wmo.
    bool vk_owns_m2 = false;
    // [phase J] The VK feeds (vkFeedClassicBucket / vkFeedWmoInstance / the terrain+water collect)
    // ran unconditionally inside WorldRender::draw -- including in PURE GL MODE, where nothing ever
    // consumes them. That is a per-frame tax on the GL baseline for buckets no one reads. Set true
    // only when the VK path (or parity, which needs both) will actually consume this frame's feeds.
    bool vk_feeding = false;
    bool vk_owns_water = false;
    // read-only view of the per-frame lighting/fog block so the VK backend renders with the same numbers
    OpenGL::LightingUniformBlock const& lightingBlock() const { return _lighting_ubo_data; }
    // [VULKAN phase B] sun band (LightIntBand 9) = the terrain specular colour GL feeds sun_spec_color
    glm::vec3 sunSpecColor() const;

    // [VULKAN phase C] CPU mirror of the MDI geometry arena + the per-frame batch streams, so the Vulkan
    // backend can draw the SAME batches from the SAME offsets GL's indirect commands use. Filled by
    // mdiEnsureModelInArena / drawDynamicBatched; consumed by MapView's VK block. GL is unaffected.
    std::vector<unsigned char> const& mdiMirrorVertices() const { return _mdi_mirror_verts; }
    std::vector<std::uint16_t> const& mdiMirrorIndices() const { return _mdi_mirror_idx; }
    bool mdiMirrorDirty() const { return _mdi_mirror_dirty; }
    void clearMdiMirrorDirty() { _mdi_mirror_dirty = false; }
    // [VULKAN] per-frame SNAPSHOT of every M2 producer's output. The scratch arrays below are
    // cleared by each producer in turn (drawPibBatched runs last and wiped the classic feed), so VK
    // reads this accumulated copy instead.
    void vkM2SnapshotClear();
    void vkM2SnapshotAppend();
    std::vector<glm::mat4x4> const& vkM2Transforms() const { return _vk_m2_tf; }
    std::vector<glm::vec4> const& vkM2Interiors() const { return _vk_m2_interior; }
    std::vector<glm::ivec4> const& vkM2TexInfo() const { return _vk_m2_tex; }
    std::vector<int> const& vkM2BlpIndex() const { return _vk_m2_blp_idx; }
    std::vector<glm::ivec4> const& vkM2State() const { return _vk_m2_state; }
    std::vector<glm::mat4x4> const& vkM2Bones() const { return _vk_m2_bones; }
    std::vector<OpenGL::DrawElementsIndirectCommand> const& vkM2Commands() const { return _vk_m2_cmds; }
    std::vector<glm::ivec4> const& vkM2Groups() const { return _vk_m2_groups; }
    LiquidTextureManager const& liquidTextureManager() const { return _liquid_texture_manager; }
    // [VULKAN] every visible WMO liquid this frame: (groups, instance transform). VK appends
    // these to the ADT water mesh so one pipeline draws both.
    struct VkWmoLiquidRef { std::vector<WMOGroup*> groups; glm::mat4x4 transform; };
    std::vector<VkWmoLiquidRef> const& vkWmoLiquids() const { return _vk_wmo_liquids; }

    // [phase F] every celestial billboard this frame, in the order GL issues them. Positions are
    // camera-relative; MapView resolves `blp` to a bindless id and hands the list to Vulkan.
    struct VkCelestial
    {
      glm::vec3 center_rel;
      float half_size;
      glm::vec3 right;
      float opacity;
      glm::vec3 up;
      glm::vec3 color;
      bool additive;
      std::string blp;
    };
    std::vector<VkCelestial> const& vkCelestials() const { return _vk_celestials; }
    // groundEffectDist for this frame -- the client's per-vertex clutter fade ramp needs it.
    float vkClutterDetailDist() const { return _vk_clutter_detail_dist; }
    void setVkOwnsCelestials(bool v) { _vk_owns_celestials = v; }
    void vkClearWmoLiquids() { _vk_wmo_liquids.clear(); }
    // celestial disc direction -- GL feeds it to the liquid shader as `sheen_dir`
    glm::vec3 celestialDirForVk() const { return _skies ? _skies->celestial_dir() : glm::vec3(0.f, 1.f, 0.f); }
    std::vector<std::pair<std::string, std::string>> const& vkM2BlpPairs() const { return _vk_m2_blp_pairs; }
    int vkM2BlpPairIndex(std::string const& a, std::string const& b);

    std::vector<glm::mat4x4> const& batchInstanceTransforms() const { return _pib_scratch_tf; }
    std::vector<glm::vec4> const& batchInstanceInteriors() const { return _pib_scratch_interior; }
    std::vector<glm::ivec4> const& batchInstanceTexInfo() const { return _pib_scratch_tex; }
    std::vector<glm::mat4x4> const& batchBones() const { return _pib_scratch_bones; }
    std::vector<OpenGL::DrawElementsIndirectCommand> const& batchDrawCommands() const { return _pib_scratch_cmds; }
    // parallel to batchInstanceTexInfo(): index into batchBlpPairs() for that instance's textures
    std::vector<int> const& batchInstanceBlpIndex() const { return _pib_scratch_blp_idx; }
    std::vector<glm::ivec4> const& batchInstanceState() const { return _pib_scratch_state; }
    // per DRAW GROUP: (blend_mode, backface_cull, first_cmd, cmd_count) -- the same grouping the
    // GL path uses to switch blend/cull state, so VK can bind one pipeline per group.
    std::vector<glm::ivec4> const& batchDrawGroups() const { return _pib_scratch_groups; }
    // the per-pixel object-cull distance this batch set pushed into the GL `slice_dist` uniform
    float batchSliceDist() const { return _pib_scratch_slice; }
    // which batch function last filled these arrays (diagnostic): 1 pib, 2 dynamic, 3 creatures,
    // 4 creature bodies -- VK feeds from whatever ran last in the frame
    int batchSource() const { return _pib_scratch_src; }
    // how many draw commands that function actually ISSUED to GL (0 = built but not drawn)
    std::size_t batchIssued() const { return _pib_scratch_issued; }
    // the OTHER doodad path (_mdi_buffers, per-class cull): commands GL issued from it this frame.
    // VK is fed only the _pib_* batches, so anything drawn here is GL-only geometry.
    std::size_t mdiIssued() const { return _mdi_issued; }
    // the CLASSIC per-model instanced loop (m2_shader) -- also GL-only, also unseen by VK
    std::size_t classicIssued() const { return _classic_issued; }
    // how many classic-path instances were additionally handed to VK this frame
    std::size_t vkClassicFed() const { return _vk_classic_fed; }
    // instances the VK feed could NOT represent this frame, by reason. Phase C is done only
    // when these are zero -- they are the remaining GL-only geometry.
    // ---- [VULKAN phase D] WMO feed ----
    struct VkWmoDraw
    {
      std::uint32_t index_count = 0;
      std::uint32_t first_index = 0;
      std::int32_t  base_vertex = 0;
      std::uint32_t xform_index = 0;   // into vkWmoTransforms()
      std::int32_t  blend_mode = 0;
      std::int32_t  backface_cull = 0;
    };
    // one record per arena batch: (blp pair index, shader, flags, alpha test mode)
    std::vector<glm::ivec4> const& vkWmoBatches() const { return _vk_wmo_batches; }
    std::vector<std::pair<std::string, std::string>> const& vkWmoBlpPairs() const { return _vk_wmo_blp_pairs; }
    bool vkWmoBatchesDirty() const { return _vk_wmo_batches_dirty; }
    void clearVkWmoBatchesDirty() { _vk_wmo_batches_dirty = false; }
    std::vector<Noggit::Rendering::VkWmoVertex> const& vkWmoArenaVertices() const { return _vk_wmo_verts; }
    std::vector<std::uint16_t> const& vkWmoArenaIndices() const { return _vk_wmo_idx; }
    bool vkWmoArenaDirty() const { return _vk_wmo_arena_dirty; }
    void clearVkWmoArenaDirty() { _vk_wmo_arena_dirty = false; }
    std::vector<VkWmoDraw> const& vkWmoDraws() const { return _vk_wmo_draws; }
    std::vector<glm::mat4x4> const& vkWmoTransforms() const { return _vk_wmo_xforms; }
    // MOHD ambient per visible instance, parallel to vkWmoTransforms()
    std::vector<glm::vec4> const& vkWmoAmbients() const { return _vk_wmo_ambients; }
    // instances whose groups could not be represented yet (open TODO, must reach zero)
    std::size_t vkWmoFallback() const { return _vk_wmo_fallback; }
    // what GL issued for WMOs this frame -- the VK feed is only trustworthy while these track
    std::size_t glWmoDrawCalls() const;
    void vkFeedWmoInstance(WMOInstance* instance);
    void vkResetWmoFrame();
    // set by MapView when the VK backend has a working WMO pipeline: GL then records its runs
    // for the feed but stops drawing them (the VK compose already supplied those pixels)
    void setVkOwnsWmo(bool v) { _vk_owns_wmo = v; }

    std::size_t vkFallbackPass() const { return _vk_fb_pass; }
    std::size_t vkFallbackArena() const { return _vk_fb_arena; }
    std::size_t vkFallbackEmpty() const { return _vk_fb_empty; }
    std::vector<std::string> const& vkFallbackNames() const { return _vk_fb_names; }
    std::vector<std::pair<std::string, std::string>> const& batchBlpPairs() const { return _pib_batch_blps; }

    void upload() override;
    void unload() override;
    // Listener-submerged state for the audio layer (ambience underwater override, doc 38).
    bool camera_underwater() const { return _camera_underwater; }

    // True while the camera is inside a WMO this frame (cached in draw()). Read by WMORender to route WMO
    // exterior-lit / portal-spill faces to the WMO's interior context instead of the outdoor map light.
    bool cameraInsideWmo() const { return _camera_inside_wmo; }


    // Called by WMORender instead of drawing its liquid inline -- see _deferred_wmo_liquid.
    void queueWmoLiquid(std::vector<WMOGroup*> groups, glm::mat4x4 const& transform,
                        bool interior_only, bool draw_fog)
    {
      // [VULKAN] keep a copy for the VK water feed: _deferred_wmo_liquid is flushed and CLEARED
      // inside the same frame's water phase, so MapView's next-frame read always saw it empty.
      _vk_wmo_liquids.push_back({ groups, transform });
      _deferred_wmo_liquid.push_back({std::move(groups), transform, interior_only, draw_fog});
    }

    // [PIPELINE step 1] Replay the buckets Vulkan refused, on the render thread, after draw().
    // No-op when nothing was deferred (the usual case is a handful of all-hidden-geoset models).
    void drawDeferredGlFallback();

    void draw (glm::mat4x4 const& model_view
        , glm::mat4x4 const& projection
        , glm::vec3 const& cursor_pos
        , float cursorRotation
        , glm::vec4 const& cursor_color
        , CursorType cursor_type
        , float brush_radius
        , bool show_unpaintable_chunks
        , bool draw_only_inside_light_sphere
        , bool draw_wireframe_light_sphere
        , float alpha_light_sphere
        , float inner_radius_ratio
        , glm::vec3 const& ref_pos
        , float angle
        , float orientation
        , bool use_ref_pos
        , bool angled_mode
        , bool draw_paintability_overlay
        , editing_mode terrainMode
        , glm::vec3 const& camera_pos
        , bool camera_moved
        , bool draw_mfbo
        , bool draw_terrain
        , bool draw_wmo
        , bool draw_water
        , bool draw_wmo_doodads
        , bool draw_models
        , bool draw_model_animations
        , bool draw_models_with_box
        , bool draw_hidden_models
        , MinimapRenderSettings* minimap_render_settings
        , bool draw_fog
        , eTerrainType ground_editing_brush
        , int water_layer
        , display_mode display
        , bool draw_occlusion_boxes = false
        , bool minimap_render = false
        , bool draw_wmo_exterior = true
        , bool draw_bloom = false
        , bool draw_ground_clutter = true
    );

    bool saveMinimap (TileIndex const& tile_idx
                      , MinimapRenderSettings* settings
                      , std::optional<QImage>& combined_image);

    [[nodiscard]]
    OpenGL::TerrainParamsUniformBlock* getTerrainParamsUniformBlock() { return &_terrain_params_ubo_data; };

    void updateTerrainParamsUniformBlock();
    void markTerrainParamsUniformBlockDirty() { _need_terrain_params_ubo_update = true; };

    [[nodiscard]] std::unique_ptr<Skies>& skies() { return _skies; };

    // Per-room (MOLR) point-light scoping: the client lights each interior WMO group ONLY by the
    // MOLT lights its MOLR chunk references. Called by WMORender between group draws to swap the
    // point-light region of the lighting UBO to the group's authored set; restore returns to the
    // frame's global nearest-16 pool (kept in _lighting_ubo_data) before non-WMO passes.
    void setWmoGroupPointLights(WMO const* wmo, std::vector<int16_t> const& light_refs,
                                glm::mat4x4 const& transform, glm::vec3 const& camera_pos);
    void restoreGlobalPointLights();

    // The zone fog currently in the lighting UBO (always the OUTDOOR values -- per-WMO MFOG is
    // blended on top by WMORender). start is a FRACTION of end (can be negative: mist scaler).
    void getZoneFog(glm::vec3& color, float& start_frac, float& end) const
    {
      color = glm::vec3(_lighting_ubo_data.FogColor_FogOn);
      start_frac = _lighting_ubo_data.DiffuseColor_FogStart.w;
      end = _lighting_ubo_data.AmbientColor_FogEnd.w;
    }

    // Editor fog-distance multiplier. There are now TWO (2026-07-25): "fog_distance_scale" for OUTDOOR
    // (zone) fog and "fog_distance_scale_interior" for INDOOR (WMO) fog. updateLightingUniformBlock picks
    // the one matching _camera_inside_wmo each frame and stores it in _active_fog_distance_scale; the zone
    // fog in the UBO already has it applied, and WMORender reads this getter for per-group MFOG so WMO
    // geometry matches the scene's active fog band.
    float fogDistanceScale() const { return _active_fog_distance_scale; }

  private:

    void drawMinimap ( MapTile *tile
        , glm::mat4x4 const& model_view
        , glm::mat4x4 const& projection
        , glm::vec3 const& camera_pos
        , MinimapRenderSettings* settings
    );

    void updateMVPUniformBlock(const glm::mat4x4& model_view, const glm::mat4x4& projection, glm::vec3 const& camera_pos);
    void updateLightingUniformBlock(bool draw_fog, glm::vec3 const& camera_pos);
    void updateLightingUniformBlockMinimap(MinimapRenderSettings* settings);

    void setupChunkVAO(OpenGL::Scoped::use_program& mcnk_shader);
    void setupLiquidChunkVAO(OpenGL::Scoped::use_program& water_shader);
    void setupOccluderBuffers();
    void setupChunkBuffers();
    void setupLiquidChunkBuffers();

    // Bloom post-process: (re)create the offscreen targets for the given viewport size, and run the
    // bright-pass -> blur -> composite once the scene has been rendered into the scene target.
    void ensureBloomTargets(int w, int h);
    void renderBloomAndComposite(GLuint target_fbo, int w, int h, glm::vec3 const& camera_pos);
    // Blit the current depth buffer into _decal_depth_tex for screen-space projected decals (blob
    // shadows + selection circles). Idempotent per frame (guarded by _decal_depth_ready). Returns
    // false if it couldn't (no viewport). Call after terrain+WMO+doodads, before creatures.
    bool snapshotDecalDepth();

    // Same, into _world_depth_tex. Must be called after the WMO pass and before the M2 pass.
    bool snapshotWorldDepth();

    World* _world;
    float _cull_distance;         // how far OBJECTS/WMOs/models render (Object Render Distance slider), clamped to terrain
    float _terrain_cull_distance; // how far TERRAIN/horizon/sky render = view distance; fog never affects it
    float _view_distance;
    float _fog_distance_scale = 1.0f;          // OUTDOOR (zone) fog distance multiplier
    float _fog_distance_scale_interior = 1.0f; // INDOOR (WMO) fog distance multiplier
    float _active_fog_distance_scale = 1.0f;   // whichever of the two matches _camera_inside_wmo this frame

    // Ground clutter (checklist 14.1): persistent model refs for detail doodads, keyed by path, so
    // the handful of shared grass/pebble M2s stay resident while the camera moves.
    std::unordered_map<std::string, scoped_model_reference> _detail_doodad_models;

    // Rebuilt from the world's creature spawns; keyed by spawn model_path. THROTTLED (2026-07-25 perf):
    // the index is derived from static spawn placements + lazily-refining model radii, but rebuilding the
    // whole string-keyed map every frame cost ~3ms in Stormwind (10% of the frame). rebuildLegacySuppressIndex
    // now only rebuilds on a spawn-count change or every N frames; these track the throttle state.
    std::unordered_map<std::string, std::vector<LegacyOverlayInfo>> _legacy_suppress_index;
    std::size_t _legacy_suppress_last_count = static_cast<std::size_t>(-1);
    unsigned _legacy_suppress_tick = 0;
    void rebuildLegacySuppressIndex();

    // shaders
    std::unique_ptr<OpenGL::program> _mcnk_program;;
    std::unique_ptr<OpenGL::program> _mfbo_program;
    std::unique_ptr<OpenGL::program> _m2_program;
    std::unique_ptr<OpenGL::program> _m2_instanced_program;
    std::unique_ptr<OpenGL::program> _m2_batched_program; // [perf 2026-08-05] MDI cross-model doodad batching (instanced+batched defines)
    std::unique_ptr<OpenGL::program> _m2_particles_program;
    std::unique_ptr<OpenGL::program> _m2_ribbons_program;
    std::unique_ptr<OpenGL::program> _blob_shadow_program; // unit (creature) ground blob shadows
    std::unique_ptr<OpenGL::program> _m2_box_program;
    std::unique_ptr<OpenGL::program> _wmo_program;
    std::unique_ptr<OpenGL::program> _liquid_program;
    std::unique_ptr<OpenGL::program> _wmo_liquid_program;
    std::unique_ptr<OpenGL::program> _occluder_program;

    // [perf 2026-08-05] MDI cross-model doodad batching (NOGGIT_DOODAD_MDI). An append-only shared geometry
    // arena holds every batchable doodad model's geometry (concatenated once at first use, never freed); each
    // frame the visible batchable instances are assembled into one instance buffer + indirect-command buffer
    // and drawn with one glMultiDrawElementsIndirect per (texture-array x state) group via _m2_batched_program.
    // See ensureMdiArena / mdiEnsureModelInArena / drawDoodadsBatched in WorldRender.cpp.
    struct MdiArenaSlot { GLint base_vertex = 0; GLuint index_base = 0; bool ok = false; };
    // Keyed by the model's STABLE file identity, NOT Model* -- the arena is append-only/never cleared, but
    // Model* addresses are reused after a model unloads (dense streaming), so a Model* key would map a new
    // model to a DIFFERENT unloaded model's arena geometry -> exploded vertices. Same file == same geometry,
    // so file-keying is both correct and deduplicating.
    std::unordered_map<std::string, MdiArenaSlot> _mdi_slots;     // arena location per model FILE (append-only)
    std::unordered_map<Model*, std::uint8_t> _mdi_batched_models; // per-frame: models the MDI pass drew (skip elsewhere)
    OpenGL::Scoped::deferred_upload_buffers<8> _mdi_buffers;      // 0 arena_vbo 1 arena_ibo 2 inst_tf 3 inst_interior 4 inst_tex 5 indirect 6 bone_ssbo 7 inst_cull_class
    OpenGL::Scoped::deferred_upload_vertex_arrays<1> _mdi_vao_arr;
    bool _mdi_ready = false;
    GLsizeiptr _mdi_arena_vbo_cap = 0, _mdi_arena_ibo_cap = 0;
    GLsizei _mdi_arena_vtx = 0, _mdi_arena_idx = 0;
    GLsizeiptr _mdi_inst_cap = 0, _mdi_indirect_cap = 0;          // current instance/indirect buffer byte capacities
    std::vector<std::pair<Model*, TileRender::DoodadInstanceBuffer const*>> _mdi_all_loaded; // GPU-driven P1: ALL loaded tiles' doodads (camera-independent) -> batch rebuilds only on tile load/unload
    std::vector<glm::mat4x4> _mdi_scratch_tf;                     // per-frame scratch (retained to avoid re-alloc)
    std::vector<glm::vec4>   _mdi_scratch_interior;
    std::vector<glm::ivec4>  _mdi_scratch_tex;
    std::vector<float>       _mdi_scratch_class;                  // per-instance size class 0-4 (size-class draw distance)
    std::vector<OpenGL::DrawElementsIndirectCommand> _mdi_scratch_cmds;
    // [animated MDI 2026-08-07] SHARED-pose animated batched models: each gets a FIXED bone-block slot in the
    // batched bone SSBO (buffer 6), so inst_tex.z (block base) / inst_tex.w (bone count) are STABLE across
    // frames (cached with the structure). Each frame fillMdiBones() re-animates the VISIBLE ones (bbox frustum
    // cull) into _mdi_scratch_bones at their base and uploads it; off-screen models keep last pose (they're
    // GPU frustum-culled anyway). This keeps the draw-call collapse without an "animate everything" regression.
    struct MdiBoneModel { Model* model; std::uint32_t bone_base; std::uint32_t bone_count; glm::vec3 bbox_min; glm::vec3 bbox_max; };
    std::vector<MdiBoneModel> _mdi_bone_models;    // cached with the structure (rebuilt on cache miss)
    std::vector<glm::mat4x4>  _mdi_scratch_bones;  // per-frame bone matrices for ALL batched animated models
    std::size_t _mdi_bone_total = 0;               // total mat4 slots in the bone SSBO
    bool _mdi_bones_uploaded_once = false;         // first upload must happen even if nothing is visible yet

    // [pib-MDI 2026-08-07] batch the per-instance-animation (billboard) WMO doodads through MDI, IN-PLACE at
    // the same pipeline position as the old serial per-group draws (ordering semantics preserved). Own
    // per-frame instance buffers + VAO (the tile batch's are cached across frames; these re-upload each
    // frame) sharing the SAME geometry arena, and a bone SSBO with PER-INSTANCE blocks: each instance's
    // inst_tex.z points at its own pose (big_bones is already the per-instance concatenation).
    struct PibGroup
    {
      Model* pmodel = nullptr;
      std::vector<ModelInstance*> const* doodads = nullptr;
      bool has_bones = false;
      bool batched = false; // consumed by drawPibBatched -> the serial fallback loop skips it
      std::vector<glm::mat4x4> transforms;
      std::vector<glm::vec4> interiors;
      std::vector<std::uint64_t> keys;
      std::vector<glm::mat4x4> big_bones;
    };
    // [GL perf 2026-08-08] PibPrep cache: the dedupe/group/transform/interior prep (~1.7ms) rebuilt only when
    // the VISIBLE pib set changes (cheap pointer-XOR signature per frame; cache-node pointers are stable).
    // The bake (bones) + draws stay per-frame; big_bones are cleared per bake since groups now persist.
    std::vector<PibGroup> _pib_groups_cache;
    unsigned long long _pib_groups_sig = 0;
    bool _pib_groups_valid = false;

    OpenGL::Scoped::deferred_upload_buffers<5> _pib_buffers; // 0 inst_tf 1 inst_interior 2 inst_tex 3 indirect 4 bone_ssbo
    OpenGL::Scoped::deferred_upload_vertex_arrays<1> _pib_vao_arr;
    bool _pib_ready = false;
    std::vector<glm::mat4x4> _pib_scratch_tf;
    std::vector<glm::vec4>   _pib_scratch_interior;
    std::vector<glm::ivec4>  _pib_scratch_tex;
    std::vector<OpenGL::DrawElementsIndirectCommand> _pib_scratch_cmds;
    std::vector<glm::mat4x4> _pib_scratch_bones;
    void ensurePibMdi();                                // VAO over the shared arena + the pib instance buffers
    void drawPibBatched(std::vector<PibGroup>& groups); // classify + build + MDI-draw; marks consumed groups

    // [dyn-MDI 2026-08-07] the DYNAMIC instanced pool (per-frame gathered WMO doodads + GOs, models_to_draw)
    // through the same per-frame MDI machinery (shares the pib buffers/VAO -- bufferData orphaning makes the
    // sequential reuse safe). Consumed buckets land in _dyn_batched_models; the classic loop keeps ALL its
    // side-effects (deferral, particle collection) and just skips the draw for them. Shared-pose bone block
    // per model (one animate per model, all its instances point at it).
    std::unordered_map<Model*, std::uint8_t> _dyn_batched_models;
    void drawDynamicBatched(tsl::robin_map<Model*, std::vector<glm::mat4x4>> const& buckets,
                            tsl::robin_map<Model*, std::vector<glm::vec4>>& interiors,
                            tsl::robin_map<Model*, std::vector<float>>& fades,
                            std::set<Model*> const& go_buckets,
                            glm::mat4x4 const& model_view, int animtime, bool draw_hidden_models);

    // [creature MDI 2026-08-18] PHASE A: fold the far/simple instanced creature groups (creature_instanced,
    // keyed by (Model*, display_id)) through the same per-frame pib-MDI machinery as drawDynamicBatched --
    // one shared bone block per model + one MDI draw per state group, replacing the per-group animate() +
    // bone-TBO STREAM churn that dominates M2Creatures. Each group carries its REPRESENTATIVE instance so the
    // replaceable skin (array,layer) + geoset visibility resolve (resolveStaticBatch's rep path). Groups that
    // don't fully resolve (hidden geoset, animated-uv pass, mid-fade, unresolved skin) are left OUT of
    // out_batched so the classic loop below still draws them. Toggle NOGGIT_CREATURE_MDI (default off until
    // validated); out_batched tells the classic loop which groups to skip.
    std::set<std::pair<Model*, std::uint32_t>> _creature_batched;
    void drawCreaturesBatched(
        std::map<std::pair<Model*, std::uint32_t>, std::vector<glm::mat4x4>> const& creatures,
        std::map<std::pair<Model*, std::uint32_t>, ModelInstance const*> const& reps,
        std::map<std::pair<Model*, std::uint32_t>, std::vector<float>>& fades,
        std::function<glm::vec4(glm::vec3 const&)> const& interior_at,
        glm::mat4x4 const& model_view, int animtime, bool draw_hidden_models,
        std::set<std::pair<Model*, std::uint32_t>>& out_batched);

    // [creature body MDI 2026-08-18] PHASE A (real): fold the INDIVIDUAL creature BODY draws (the near/complex
    // pool creature_spawn_instances_to_draw) through the pib-MDI machinery with PER-INSTANCE bone blocks (each
    // near creature has its own live pose). Kills the per-creature draw-call + bone-TBO-upload storm that
    // dominates M2Creatures+IndivDraw in a crowd. BODIES ONLY -- mounts / attachments / particle creatures /
    // mid-fade / tinted / translucent stay on the individual path; a body that doesn't fully resolve
    // (hidden-geoset-only, animated-uv/blend pass, unresolved skin, arena-full) also stays individual. Toggle
    // NOGGIT_CREATURE_BODY_MDI (default off). World-free item struct so the hpp needn't include World.h (which
    // already includes this header -> circular); the call site fills it where World is visible.
    struct CreatureBodyBatchItem
    {
      ModelInstance* instance = nullptr;
      std::uint32_t display_id = 0;
      int anim_time_offset = 0;
      float fade = 1.0f;
      bool has_mount = false;
    };
    std::set<ModelInstance const*> _creature_body_batched; // bodies the batch drew; the classic loop skips them
    void drawCreatureBodiesBatched(
        std::vector<CreatureBodyBatchItem> const& items,
        std::function<glm::vec4(glm::vec3 const&)> const& interior_at,
        glm::mat4x4 const& model_view, int animtime, bool draw_hidden_models,
        std::set<ModelInstance const*>& out_batched);

    // [perf 2026-08-06] AMORTIZATION: the assembled batch is cached across frames and rebuilt ONLY when the
    // visible doodad set changes (cheap per-frame signature over persistent_doodad_draws + the texture-upload
    // epoch). On a cache hit the GPU instance/indirect buffers still hold the last upload, so per-frame work
    // collapses to re-issuing the cached groups. _mdi_batched_models is likewise kept across a hit.
    struct MdiGroup { StaticBatchKey key; std::uint32_t first_cmd = 0; std::uint32_t cmd_count = 0; };
    std::vector<MdiGroup> _mdi_cached_groups;
    unsigned long long _mdi_last_sig = 0;
    bool _mdi_cache_valid = false;
    std::size_t _mdi_cached_instances = 0; // rendered-object count added each frame (hit or miss)

    void ensureMdiArena();          // one-time arena/instance/indirect buffer + VAO setup
    bool mdiEnsureModelInArena(Model* m); // lazily append a model's geometry to the arena; false if it can't batch
    void drawDoodadsBatched(
        std::vector<std::pair<Model*, TileRender::DoodadInstanceBuffer const*>> const& persistent_doodad_draws,
        glm::mat4x4 const& model_view, bool draw_hidden_models,
        math::frustum const& frustum, int animtime); // fills _mdi_batched_models + bone SSBO + MDI-draws them
    void drawMdiGroups();           // issue the cached groups (constant uniforms + per-group check-before-set + MDI)
    void fillMdiBones(glm::mat4x4 const& model_view, math::frustum const& frustum, int animtime); // per-frame: animate VISIBLE batched models into the bone SSBO + upload

    // bloom post-process
    std::unique_ptr<OpenGL::program> _bloom_bright_program;
    std::unique_ptr<OpenGL::program> _bloom_blur_program;
    std::unique_ptr<OpenGL::program> _bloom_composite_program;
    bool _bloom_initialized = false;
    int _bloom_w = -1, _bloom_h = -1, _bloom_bw = 0, _bloom_bh = 0;
    GLuint _bloom_vao = 0;
    std::unique_ptr<OpenGL::program> _sun_program; // sky sun disc
    std::unique_ptr<OpenGL::program> _sunshaft_program; // screen-space radial sunshaft
    std::unique_ptr<OpenGL::program> _moon_program; // textured celestial billboard (sun/moon disc + glare)
    std::unique_ptr<scoped_blp_texture_reference> _moon_texture;       // textures/moon.blp  (White Lady disc)
    std::unique_ptr<scoped_blp_texture_reference> _moon2_texture;      // textures/moon02.blp (Blue Child disc)
    std::unique_ptr<scoped_blp_texture_reference> _moon_glare_texture; // textures/moonGlare.blp (white moon halo)
    std::unique_ptr<scoped_blp_texture_reference> _sun_center_texture; // textures/sunCenter.blp (sun disc)
    std::unique_ptr<scoped_blp_texture_reference> _sun_glare_texture;  // textures/sunGlare.blp (sun corona/rays)
    glm::vec2 _sun_screen_uv{0.5f, 0.5f}; // sun projected to screen [0,1], for the sunshaft pass
    float _sun_shaft_strength = 0.0f;     // view-alignment-faded strength; 0 = sun off-screen/behind
    // [VULKAN phase C] MDI arena mirror (see mdiMirrorVertices)
    static glm::ivec4 batchStateVec(StaticBatchKey const& k);
    // [VULKAN phase C] append one classic-path bucket to the VK feed arrays (GL untouched)
    // Returns TRUE when Vulkan actually took this bucket, so the caller can skip GL's draw for it.
    // A bucket VK rejected (unresolved pass, arena failure) must still be drawn by GL.
    bool vkFeedClassicBucket(Model* m, std::vector<glm::mat4x4> const& transforms,
                             std::vector<glm::vec4> const* interiors);
    int batchBlpPairIndex(std::string const& blp0, std::string const& blp1);
    // [VULKAN phase C] per-instance BATCH STATE for the VK shader: x = blend_mode, y = flag bits
    // (0 unlit, 1 unfogged, 2 classic_alpha era, 3 backface_cull), z = pixel_shader, w = tu lookups.
    std::vector<glm::ivec4> _pib_scratch_state;
    std::vector<glm::ivec4> _pib_scratch_groups;

    std::vector<glm::mat4x4> _vk_m2_tf;
    std::vector<glm::vec4> _vk_m2_interior;
    std::vector<glm::ivec4> _vk_m2_tex;
    std::vector<int> _vk_m2_blp_idx;
    std::vector<glm::ivec4> _vk_m2_state;
    std::vector<glm::mat4x4> _vk_m2_bones;
    std::vector<OpenGL::DrawElementsIndirectCommand> _vk_m2_cmds;
    std::vector<glm::ivec4> _vk_m2_groups;
    std::vector<std::pair<std::string, std::string>> _vk_m2_blp_pairs;
    // index OF the pair table -- the linear scan it replaces was O(batches x pairs) string
    // compares every frame, and the table grows as more of the world is visited.
    std::unordered_map<std::string, int> _vk_m2_blp_pair_index;
    float _pib_scratch_slice = 0.f;
    int _pib_scratch_src = 0;
    std::size_t _pib_scratch_issued = 0;
    std::size_t _mdi_issued = 0;
    std::size_t _classic_issued = 0;
    std::size_t _vk_classic_fed = 0;
    std::size_t _vk_fb_pass = 0, _vk_fb_arena = 0, _vk_fb_empty = 0;
    std::size_t _vk_gl_still_drew = 0;   // instances GL drew anyway while VK owned M2
    std::size_t _vk_fb_hidden = 0;       // declined because every pass is a hidden geoset
    bool _vk_last_feed_nothing = false;  // last feed declined AND had nothing visible to draw

    // [PIPELINE step 1] Buckets Vulkan refuses are drawn by GL from INSIDE the traversal today, which
    // is the one thing stopping the traversal from moving off the render thread (GL calls are not
    // legal there). Collect them instead and replay them on the render thread after the walk. They
    // cannot simply be dropped: see finding 54, that loses ~230 pixels.
    struct VkGlDeferredBucket
    {
      Model* model = nullptr;
      std::vector<glm::mat4x4> transforms;
      std::vector<glm::vec4> interiors;
      std::vector<float> fades;
      // the bucket's own slice_dist -- hardcoding 0 here disables the distance slice and draws
      // models that should have been sliced out (worth ~75 pixels, caught by the static check)
      float slice_dist = 0.0f;
    };
    std::vector<VkGlDeferredBucket> _vk_gl_deferred;
    // Per-INSTANCE doodads Vulkan refused. Separate list because these replay through the
    // non-instanced `draw(model_view, ModelInstance&, ...)` overload under _m2_program, not the
    // instanced one the bucket list uses.
    struct VkGlDeferredInstance
    {
      Model* model = nullptr;
      ModelInstance* instance = nullptr;
      glm::vec4 interior{0.0f};
    };
    std::vector<VkGlDeferredInstance> _vk_gl_deferred_pi;
    // traversal context the replay needs, stashed once per frame
    glm::mat4x4 _vk_def_model_view{1.0f};
    // the MVP the frustum is built from -- math::frustum is only forward-declared here, so store the
    // matrix and rebuild it at replay time (that is all its constructor takes anyway)
    glm::mat4x4 _vk_def_mvp{1.0f};
    glm::vec3 _vk_def_camera_pos{0.0f};
    display_mode _vk_def_display = display_mode::in_3D;
    bool _vk_def_boxes = false;
    std::vector<std::string> _vk_fb_names;   // this frame's GL-only models (why + path)

    // [VULKAN phase D] WMO arena (append-only, keyed by wmo file + group index) and per-frame feed
    struct VkWmoSlot { std::int32_t base_vertex = 0; std::uint32_t index_base = 0;
                       std::uint32_t batch_base = 0; bool ok = false; };
    std::vector<glm::ivec4> _vk_wmo_batches;
    std::vector<std::pair<std::string, std::string>> _vk_wmo_blp_pairs;
    bool _vk_wmo_batches_dirty = false;
    bool _vk_owns_wmo = false;
    int vkWmoBlpPairIndex(std::string const& a, std::string const& b);
    std::unordered_map<std::string, VkWmoSlot> _vk_wmo_slots;
    std::vector<Noggit::Rendering::VkWmoVertex> _vk_wmo_verts;
    std::vector<std::uint16_t> _vk_wmo_idx;
    bool _vk_wmo_arena_dirty = false;
    std::vector<VkWmoDraw> _vk_wmo_draws;
    std::vector<glm::mat4x4> _vk_wmo_xforms;
    std::vector<glm::vec4> _vk_wmo_ambients;
    std::size_t _vk_wmo_fallback = 0;
    std::vector<int> _pib_scratch_blp_idx;
    std::vector<std::pair<std::string, std::string>> _pib_batch_blps;
        std::vector<unsigned char> _mdi_mirror_verts;
    std::vector<std::uint16_t> _mdi_mirror_idx;
    bool _mdi_mirror_dirty = false;
        GLuint _bloom_scene_fbo = 0, _bloom_scene_color = 0, _bloom_scene_depth = 0;
    // MSAA: the scene renders into multisampled renderbuffers (when render/msaa > 0) and is resolved
    // into _bloom_scene_color before the bloom chain. 0 = off.
    GLuint _msaa_fbo = 0, _msaa_color_rb = 0, _msaa_depth_rb = 0;
    int _msaa_samples = 0;
    // Live-apply guard for render/anisotropic_filtering: draw() re-applies AF to every loaded texture
    // array (via TextureManager + liquid manager) only when this changes. -1 forces apply on frame 1.
    float _last_anisotropy = -1.0f;
    GLuint _bloom_fbo[2] = {0, 0};
    GLuint _bloom_tex[2] = {0, 0};

    // 3.3.5a-style dynamic shadow map (client extShadowQuality 0-5, RE doc 35): a single directional
    // depth map rendered from the scene light at END of draw() over the unit caster list (creatures +
    // game character; environmental casters = next stage), PCF-sampled by terrain/wmo/m2 next frame.
    // Level 0 = off (blob shadows, the client's own level-0 behaviour). Map size by level:
    // 1/3 -> 1024, 2/4 -> 2048, 5 -> 4096 (single-map stand-in for the client's cascades).
    void ensureShadowTarget(int size);
    int shadowQuality() const { return _shadow_quality; }
    bool _shadow_initialized = false;
    GLuint _shadow_fbo = 0, _shadow_tex = 0;         // UNIT map (client: +-20yd player bubble)
    GLuint _shadow_env_fbo = 0, _shadow_env_tex = 0; // ENVIRONMENTAL map (client: ring cascade)
    int _shadow_size = 0;
    int _shadow_quality = 0;              // cached from settings each frame
    bool _shadow_map_valid = false;       // a map was rendered last frame -> receivers may sample
    bool _shadow_env_valid = false;       // env map rendered (levels >= 3)
    glm::mat4 _shadow_matrix{1.0f};       // world -> [0,1]^3 of the LAST rendered unit map
    glm::vec4 _shadow_center_range{0.f};  // xyz = map world center, w = half-range (distance fade)
    glm::mat4 _shadow_env_matrix{1.0f};
    glm::vec4 _shadow_env_center_range{0.f};

    // horizon && skies && lighting
    std::unique_ptr<Noggit::map_horizon::render> _horizon_render;
    std::unique_ptr<OutdoorLighting> _outdoor_lighting;
    OutdoorLightStats _outdoor_light_stats;
    std::unique_ptr<Skies> _skies;

    // cursor
    Noggit::CursorRender _cursor_render;
    Noggit::Rendering::Primitives::Sphere _sphere_render;
    Noggit::Rendering::Primitives::Square _square_render;
    Noggit::Rendering::Primitives::Line _line_render;
    Noggit::Rendering::Primitives::Circle _circle_render;
    Noggit::Rendering::Primitives::PathDecal _path_decal_render; // creature patrol routes
    Noggit::Rendering::Primitives::WeatherEffect _weather_effect; // rain / snow precipitation
    Noggit::Rendering::Primitives::WeatherEffect _underwater_motes; // waterParticulates when submerged
    Noggit::Rendering::Primitives::WaterRipples _water_ripples;   // surface wake/splash rings
    bool _camera_underwater = false; // cached each frame in updateLightingUniformBlock
    int _camera_liquid_family = -1;  // while submerged: 0 water, 1 ocean, 2 magma, 3 slime (else -1)
    float _camera_liquid_surface_y = 0.0f; // surface height of the covering liquid while submerged
    // GEOMETRY-MODEL particle models (checklist 12.2), lazily loaded + cached by normalized path.
    std::unordered_map<std::string, scoped_model_reference> _geometry_particle_models;

    // Patrol routes DRAPED onto the walkable surface. The authored waypoints are only corner points,
    // so the straight chord between two of them cuts under a bridge deck and through a hill crest.
    // Trying to absorb that with a vertical tolerance in the shader can't work: the tolerance needed
    // to clear an arched bridge is also wide enough to swallow a roof, a tree canopy and the body of
    // an NPC standing on the route. So the route is resampled and probed down onto the real ground
    // once, cached, and the shader then only has to tolerate micro-relief.
    // NOTE: routes are NOT probed onto the ground. That was tried and removed: each probe is a
    // World::intersect against every loaded tile and WMO instance, which cost so much that routes
    // took many seconds to appear even spread across frames. It is also no longer needed -- draping
    // existed to keep the shader's height tolerance tight enough to exclude NPC bodies, and the
    // world-depth comparison now excludes models outright, so the tolerance can be loose enough to
    // span a bridge arch on its own.

    // Per-object interior lighting: cache of quantized-world-position -> interior light (rgb = WMO room
    // ambient, a = 1 when the position is inside an indoor group; (0,0,0,0) = outdoor). Objects in the
    // same room share a cell, and the value is spatial, so a coarse ~1yd grid key is exact enough. Cleared
    // periodically (_interior_light_epoch) so WMOs that stream in late get picked up.
    std::unordered_map<std::int64_t, glm::vec4> _interior_light_cache;
    // Indoor-group AABBs, rebuilt only every _interior_light_epoch tick (the gather is EXPENSIVE); object
    // interior tests are then cheap AABB checks against this list.
    std::vector<InteriorVolume> _interior_volumes;
    unsigned _interior_light_epoch = 0;
    // Per-frame budget for cold-cache interior-light computes, so a WMO streaming in spreads its doodads'
    // interior sampling over frames instead of one hitch. Reset each frame in updateLightingUniformBlock.
    int _interior_miss_budget = 0;
    // The client's unit shadow decal texture (Textures\ShadowBlob.blp), lazily acquired on first
    // blob-shadow draw. 32x32 grayscale oval, drawn modulate (see blob_shadow_frag).
    std::unique_ptr<scoped_blp_texture_reference> _shadow_blob_texture;
    // Depth snapshot for screen-space projected decals (blob shadows): the scene depth (terrain +
    // WMO + doodads, pre-creatures) blitted into a sampleable texture once per frame.
    GLuint _decal_depth_fbo = 0;
    GLuint _decal_depth_tex = 0;
    int _decal_depth_w = -1;
    int _decal_depth_h = -1;

    // WORLD-ONLY depth: the same blit, but taken after terrain + WMOs and BEFORE the M2 pass, so it
    // holds the walkable world without any doodad, creature or gameobject model in it.
    //
    // A ground decal needs both. The world depth says where the GROUND is at a pixel -- the surface
    // the decal belongs on -- while the full scene depth says what is actually VISIBLE there. When
    // something is visible in front of the ground, the model owns that pixel and the decal must not
    // paint it. Reconstructing from the full scene depth alone cannot express that: on an NPC's
    // boots the visible surface IS the NPC, which is why the ribbon and the selection rings were
    // painting over the models no matter how the height tolerances were tuned.
    GLuint _world_depth_fbo = 0;
    GLuint _world_depth_tex = 0;
    int _world_depth_w = -1;
    int _world_depth_h = -1;
    bool _world_depth_ready = false; // snapshot taken THIS frame (reset at draw start)
    // Cache of faction-template id -> selection-circle hostility color (red/green/yellow), so the
    // FactionTemplate.dbc isn't walked per spawn per frame.
    std::unordered_map<std::uint32_t, glm::vec4> _faction_reaction_cache;
    // Per-frame spawn guid -> cull-fade alpha, so the selection circle fades in lockstep with its
    // creature (populated in the creature gather, read in the marker pass).
    std::unordered_map<std::uint32_t, float> _creature_fade_by_guid;
    bool _decal_depth_ready = false;      // snapshot taken THIS frame (reset at draw start)
    glm::mat4x4 _decal_inv_vp{1.0f};      // inv(proj*view) captured with the snapshot
    glm::vec2 _decal_inv_viewport{0.0f};  // 1/viewport
    // Loaded-WMO-set fingerprint from last volume gather: a change (WMO streamed in/out) triggers an
    // immediate re-gather + cache flush so freshly loaded rooms light their objects the SAME frame.
    std::uint64_t _last_wmo_fingerprint = 0;
    // Placement-keyed cache of the OWNING ModelInstance copies for per-instance-animated WMO doodads
    // (billboarded glow cards / global-seq flicker). The by-value copy bumps the ModelManager +
    // TextureManager refcounts through a shared mutex, so it is EXPENSIVE (~25ms/frame of GatherMerge in
    // dense interiors like Ironforge). Copying once per placement and reusing across frames removes that
    // per-frame cost. std::unordered_map is NODE-BASED, so element addresses are STABLE across insert /
    // rehash -- per_instance_wmo_doodads holds bare pointers INTO this map and they stay valid for the
    // whole frame (we only ever emplace, never erase, mid-frame). The owning copy keeps its Model alive
    // (async-unload-safe -- the same safety the old by-value list bought). Cleared ONLY when the loaded-
    // WMO fingerprint changes (load/unload/move/rotate/doodadset edit) -- never on the interior-light
    // 60-frame epoch -- so it persists across pure camera panning. Render-thread only (no races).
    std::unordered_map<std::uint64_t, ModelInstance> _pi_doodad_cache;
    // World-space MFOG entries (rebuilt on the same epoch tick) for the per-frame ENTITY fog: the
    // camera's fog context written into the lighting UBO Env slots (see types.hpp).
    std::vector<WmoGroupFogVolume> _env_fog_volumes;
    // [MC red fog 2026-08-08] sticky interior-group MFOG (held across group-AABB gaps while the camera
    // stays inside a WMO -- prevents red<->zone fog snapping on bridges/doorways).
    glm::vec3 _int_fog_color = glm::vec3(0.f);
    float _int_fog_end = 0.f;
    float _int_fog_start = 0.f;
    bool _int_fog_valid = false;
    // [fog transition 2026-08-18] Temporally EASED scene fog. The TARGET (color/end/start) is recomputed each
    // frame from the character's placement; the DISPLAYED fog fades toward it over ~NOGGIT_FOG_FADE_SECONDS so
    // crossing the indoor/outdoor boundary animates instead of snapping. Snapped on the first frame / after a
    // pause or teleport (frame dt out of range) so it never fades in from a stale state.
    glm::vec3 _fog_eased_color = glm::vec3(0.f);
    float _fog_eased_end = 0.f;
    float _fog_eased_start = 0.f;
    bool _fog_eased_valid = false;
    // [fog transition 2026-08-18] Temporally eased fog-distance SCALE only (the fog colour is unchanged across
    // a WMO boundary; only the band scales, e.g. Stormwind exterior 4x -> interior 1x). Easing just the scale
    // fades that band without adding any lag to maps whose scale never flips (Timbermaw's MFOG-sphere fog).
    float _fog_scale_eased = 1.f;
    bool _fog_scale_eased_valid = false;

    // buffers
    OpenGL::Scoped::deferred_upload_buffers<8> _buffers;
    GLuint const& _mvp_ubo = _buffers[0];
    GLuint const& _lighting_ubo = _buffers[1];
    GLuint const& _terrain_params_ubo = _buffers[2];
    GLuint const& _mapchunk_vertex = _buffers[3];
    GLuint const& _mapchunk_index = _buffers[4];
    GLuint const& _mapchunk_texcoord = _buffers[5];
    GLuint const& _liquid_chunk_vertex = _buffers[6];
    GLuint const& _occluder_index = _buffers[7];

    // uniform blocks
    OpenGL::MVPUniformBlock _mvp_ubo_data;
    OpenGL::LightingUniformBlock _lighting_ubo_data;

    // [20.4 light-collection cache] uids of instances that carry authored point lights, so the
    // per-rebuild collection walks only them instead of every instance. pending_* = instances whose
    // model (or a WMO's doodad models) had not finished async-loading at classification time --
    // re-checked each rebuild until final. Valid while storage.light_epoch() == epoch; any instance
    // add/move/remove bumps the epoch and forces a full re-derive (which costs exactly the old walk).
    struct PointLightRegistry
    {
      std::vector<std::uint32_t> m2_uids;
      std::vector<std::uint32_t> pending_m2;
      std::vector<std::uint32_t> wmo_uids;
      std::vector<std::uint32_t> pending_wmo;
      std::uint64_t epoch = ~0ull;
    };
    PointLightRegistry _point_light_registry;
    bool _point_lights_scoped = false; // true while the UBO carries a WMO group's MOLR set
    bool _camera_inside_wmo = false;   // cached per frame; drives the WMO shader's camera_inside_wmo uniform
    // Camera's smallest containing WMO group is a TRUE interior (MOGP indoor, not exterior-lit):
    // suppresses precipitation (client behaviour: no rain/snow inside buildings; exterior city
    // groups and open ext-lit channels keep raining). Valid only while _camera_inside_wmo.
    bool _camera_in_indoor_group = false;
    // Wobble warp+veil weight: BINARY (1 underwater, 0 above). The old ~5 s ease was a derived
    // invention and is REMOVED (user 2026-08-27 "nothing guessed"; the decompiled "5 s fade"
    // FUN_00460b00 is the AUDIO ambience volume -- doc 37 rounds 52/56/56b).
    float _uw_ffx_weight = 0.0f;
    float _camera_wmo_interior_factor = 0.0f; // continuous 0..1 = depth into the WMO room over the fog falloff
                                              // band; blends the interior fog in by distance (client-spatial)
                                              // instead of the binary _camera_inside_wmo snap
    glm::vec3 _fog_probe_pos = glm::vec3(0.0f); // position the fog is SELECTED from: the game character in
                                               // 3rd person (so orbiting the camera never changes the fog),
                                               // else the camera (editor / 1st person). Set each frame.
    OpenGL::TerrainParamsUniformBlock _terrain_params_ubo_data;


    // VAOs
    OpenGL::Scoped::deferred_upload_vertex_arrays<3> _vertex_arrays;
    GLuint const& _mapchunk_vao = _vertex_arrays[0];
    GLuint const& _liquid_chunk_vao = _vertex_arrays[1];
    GLuint const& _occluder_vao = _vertex_arrays[2];

    LiquidTextureManager _liquid_texture_manager;

    // Deferred WMO liquid. WMO groups are drawn in the WMO pass, which runs BEFORE the M2/creature
    // passes -- so WMO water drawn inline there is behind everything that comes after it: creatures
    // standing in it painted straight over the surface with no water tint at all. ADT water does not
    // have this problem because its pass runs after the models. Queue the WMO liquid here during the
    // WMO pass and flush it in the water phase instead, so it blends over the creatures like ADT
    // water does. Cleared every frame.
    struct DeferredWmoLiquid
    {
      std::vector<WMOGroup*> groups;
      glm::mat4x4 transform;
      bool interior_only;
      bool draw_fog;
    };
    std::vector<DeferredWmoLiquid> _deferred_wmo_liquid;
    std::vector<VkWmoLiquidRef> _vk_wmo_liquids;   // persists past the deferred flush, for VK
    std::vector<VkCelestial> _vk_celestials;
    float _vk_clutter_detail_dist = 0.f;
    bool _vk_owns_celestials = false;

    bool _need_terrain_params_ubo_update = false;
  };
}

#endif //NOGGIT_WORLDRENDER_HPP
