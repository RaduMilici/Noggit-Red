// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_WORLDRENDER_HPP
#define NOGGIT_WORLDRENDER_HPP

#include <noggit/rendering/BaseRender.hpp>

#include <external/glm/glm.hpp>
#include <math/trig.hpp>

#include <noggit/tool_enums.hpp>
#include <noggit/rendering/CursorRender.hpp>
#include <noggit/rendering/LiquidTextureManager.hpp>
#include <noggit/TextureManager.h>
#include <noggit/map_horizon.h>
#include <noggit/Sky.h>

#include <opengl/shader.hpp>
#include <noggit/rendering/Primitives.hpp>
#include <noggit/ModelInstance.h>
#include <noggit/InteriorVolume.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class World;
class WMO;
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

    void upload() override;
    void unload() override;

    // True while the camera is inside a WMO this frame (cached in draw()). Read by WMORender to route WMO
    // exterior-lit / portal-spill faces to the WMO's interior context instead of the outdoor map light.
    bool cameraInsideWmo() const { return _camera_inside_wmo; }

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

    // Editor fog-distance multiplier ("fog_distance_scale" setting, default 1.0 = client-authored).
    // The zone fog in the UBO already has it applied; WMORender applies it to per-group MFOG.
    float fogDistanceScale() const { return _fog_distance_scale; }

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

    World* _world;
    float _cull_distance;         // how far OBJECTS/WMOs/models render (Object Render Distance slider), clamped to terrain
    float _terrain_cull_distance; // how far TERRAIN/horizon/sky render = view distance; fog never affects it
    float _view_distance;
    float _fog_distance_scale = 1.0f;

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
    std::unique_ptr<OpenGL::program> _m2_particles_program;
    std::unique_ptr<OpenGL::program> _m2_ribbons_program;
    std::unique_ptr<OpenGL::program> _blob_shadow_program; // unit (creature) ground blob shadows
    std::unique_ptr<OpenGL::program> _m2_box_program;
    std::unique_ptr<OpenGL::program> _wmo_program;
    std::unique_ptr<OpenGL::program> _liquid_program;
    std::unique_ptr<OpenGL::program> _wmo_liquid_program;
    std::unique_ptr<OpenGL::program> _occluder_program;

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

    // Per-object interior lighting: cache of quantized-world-position -> interior light (rgb = WMO room
    // ambient, a = 1 when the position is inside an indoor group; (0,0,0,0) = outdoor). Objects in the
    // same room share a cell, and the value is spatial, so a coarse ~1yd grid key is exact enough. Cleared
    // periodically (_interior_light_epoch) so WMOs that stream in late get picked up.
    std::unordered_map<std::int64_t, glm::vec4> _interior_light_cache;
    // Indoor-group AABBs, rebuilt only every _interior_light_epoch tick (the gather is EXPENSIVE); object
    // interior tests are then cheap AABB checks against this list.
    std::vector<InteriorVolume> _interior_volumes;
    unsigned _interior_light_epoch = 0;
    // The client's unit shadow decal texture (Textures\ShadowBlob.blp), lazily acquired on first
    // blob-shadow draw. 32x32 grayscale oval, drawn modulate (see blob_shadow_frag).
    std::unique_ptr<scoped_blp_texture_reference> _shadow_blob_texture;
    // Depth snapshot for screen-space projected decals (blob shadows): the scene depth (terrain +
    // WMO + doodads, pre-creatures) blitted into a sampleable texture once per frame.
    GLuint _decal_depth_fbo = 0;
    GLuint _decal_depth_tex = 0;
    int _decal_depth_w = -1;
    int _decal_depth_h = -1;
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
    bool _point_lights_scoped = false; // true while the UBO carries a WMO group's MOLR set
    bool _camera_inside_wmo = false;   // cached per frame; drives the WMO shader's camera_inside_wmo uniform
    OpenGL::TerrainParamsUniformBlock _terrain_params_ubo_data;


    // VAOs
    OpenGL::Scoped::deferred_upload_vertex_arrays<3> _vertex_arrays;
    GLuint const& _mapchunk_vao = _vertex_arrays[0];
    GLuint const& _liquid_chunk_vao = _vertex_arrays[1];
    GLuint const& _occluder_vao = _vertex_arrays[2];

    LiquidTextureManager _liquid_texture_manager;

    bool _need_terrain_params_ubo_update = false;
  };
}

#endif //NOGGIT_WORLDRENDER_HPP
