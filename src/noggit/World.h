// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/frustum.hpp>
#include <math/trig.hpp>
#include <noggit/rendering/CursorRender.hpp>
#include <noggit/Misc.h>
#include <noggit/InteriorVolume.hpp>
#include <noggit/Model.h> // ModelManager
#include <noggit/ModelInstance.h>
#include <noggit/Selection.h>
#include <noggit/Sky.h> // Skies, OutdoorLighting, OutdoorLightStats
#include <noggit/WMO.h> // WMOManager
#include <noggit/map_horizon.h>
#include <noggit/map_index.hpp>
#include <noggit/TileIndex.hpp>
#include <noggit/tool_enums.hpp>
#include <noggit/world_tile_update_queue.hpp>
#include <noggit/world_model_instances_storage.hpp>
#include <noggit/ui/MinimapCreator.hpp>
#include <noggit/ContextObject.hpp>
#include <noggit/rendering/Primitives.hpp>
#include <opengl/shader.fwd.hpp>
#include <opengl/types.hpp>
#include <noggit/rendering/LiquidTextureManager.hpp>
#include <algorithm>
#include <optional>
#include <utility>
#include <QtCore/QSettings>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <array>
#include <noggit/project/ApplicationProject.h>
#include <noggit/rendering/WorldRender.hpp>

namespace Noggit
{
  struct object_paste_params;
  struct VertexSelectionCache;

  namespace Rendering
  {
    class WorldRender;
  }
}

class Brush;
class MapTile;
class QPixmap;

static const float detail_size = 8.0f;

using StripType = uint16_t;


class World
{
  friend class Noggit::Rendering::WorldRender;

public:
  struct CreatureSpawnOverlay
  {
    struct AttachmentModel
    {
      int attachment_id = -1;
      // Aura state-kit effect model rather than worn equipment: exempt from the unit's
      // CreatureModelAlpha/ghost tint propagation (client-verified: Anomalus body alpha 200,
      // its chest sparkles render full-opacity).
      bool is_aura_kit = false;
      std::optional<ModelInstance> model_instance;
      // Transform the particle pass draws this attachment's emitters with. Set each frame in the body
      // pass: the full animated attachment matrix when the model's emitters ride their parent (flag
      // 0x10), else the BIND-pose placement so world-space particles (aura sparkles) don't get
      // dragged around by the animated bone.
      glm::mat4x4 particle_transform = glm::mat4x4(1.0f);
      // World height above which this attachment's world-space particles die (the liquid surface
      // for breath bubbles). Effectively unset by default.
      float particle_kill_plane_y = 1.0e30f;
    };

    std::uint32_t guid = 0;
    std::uint32_t entry = 0;
    std::uint32_t display_id = 0;
    std::uint32_t faction = 0; // creature_template faction (FactionTemplate.dbc id) -> circle hostility color
    // Seasonal game-event membership (game_event_creature.event). 0 = base world; >0 = only while that
    // event is active; <0 = except while abs(event) is active. event_suppressed is the cached view-only
    // "hidden by the Seasonal Events filter" flag (recomputed by World::recomputeEventSuppression);
    // it never affects save/export -- only rendering and picking.
    std::int32_t event = 0;
    bool event_suppressed = false;
    std::string name;
    glm::vec3 pos = glm::vec3(0.0f);
    glm::vec3 original_pos = glm::vec3(0.0f);
    float orientation = 0.0f;
    float original_orientation = 0.0f;
    int animation_time_offset = 0;
    float template_scale = 1.0f;
    float model_scale = 1.0f;
    std::string model_path;
    bool is_character_model = false;
    std::uint32_t mainhand_display_id = 0;
    std::uint32_t offhand_display_id = 0;
    std::uint32_t ranged_display_id = 0;
    std::uint32_t mainhand_inventory_type = 0;
    std::uint32_t offhand_inventory_type = 0;
    std::uint32_t ranged_inventory_type = 0;
    // Space-separated permanent aura spell ids (creature_template.auras). Their state-kit visual
    // effect models are attached to the spawn (e.g. the arcane elementals' chest sparkle).
    std::string auras;
    // The client's exact ground selection-circle radius: server bounding radius (per display id,
    // creature_display_info_addon / creature_model_info) x RAW creature_template.scale (1 when
    // unset) -- already multiplied at load. 0 when the schema/row is missing.
    float bounding_radius = 0.0f;
    bool model_create_failed = false;
    bool hovered = false;
    bool selected = false;
    bool pending_create = false;
    bool pending_delete = false; // marked for deletion (Del); exported as DELETE, undoable via Ctrl+Z
    bool dirty = false;
    std::optional<ModelInstance> model_instance;
    std::vector<AttachmentModel> attachment_models;

    // Mount (creature_addon.mount_display_id): mounted NPCs ride a mount model. The mount renders at
    // the spawn's ground position and the rider (model_instance above) is re-seated onto the mount's
    // MountMain attachment (id 0) at draw time. Built lazily by ensureCreatureSpawnModel.
    std::uint32_t mount_display_id = 0;
    std::string mount_model_path;
    bool mount_create_failed = false;
    std::optional<ModelInstance> mount_instance;

    // NPC pose (creature_addon): UnitStandState (0 stand, 1 sit, 3 sleep, 4/5/6 chair-sit, 7 dead, 8 kneel)
    // + emote_state (Emotes.dbc id). Drives the forced idle animation so the spawn sits/sleeps/kneels/emotes
    // instead of standing. Mount pose (anim 91) takes priority when the NPC is also mounted.
    std::uint8_t  stand_state = 0;
    std::uint32_t emote_state = 0;

    // Extended `creature` columns (tortoise-wow schema; see mysql::CreatureSpawnRecord for the
    // server-RE'd semantics). `ext` is the editable state, `original_ext` the DB state -- any
    // difference marks the spawn dirty and the exporter writes the changed columns.
    struct ExtFields
    {
      std::uint32_t id2 = 0, id3 = 0, id4 = 0;      // alternate entries, random pick per spawn
      std::uint32_t spawntimesecs_min = 0;           // respawn delay range (seconds)
      std::uint32_t spawntimesecs_max = 0;
      float         wander_distance = 0.0f;          // yards; movement_type 1 wander radius
      std::uint32_t health_percent = 100;
      std::uint32_t mana_percent = 100;
      std::uint32_t movement_type = 0;               // 0 Idle, 1 Random, 2 Waypoint
      std::uint32_t spawn_flags = 0;                 // SPAWN_FLAG_* bitmask
      float         visibility_mod = 0.0f;           // yards; 0 = normal visibility distance
      std::uint32_t spawn_mask = 1;                  // cmangos: map-difficulty bitmask (bit k = diff k)
      std::uint32_t phase_mask = 1;                  // cmangos: phasing bitmask (default 1)

      bool operator==(ExtFields const& o) const
      {
        return id2 == o.id2 && id3 == o.id3 && id4 == o.id4
            && spawntimesecs_min == o.spawntimesecs_min
            && spawntimesecs_max == o.spawntimesecs_max
            && wander_distance == o.wander_distance
            && health_percent == o.health_percent
            && mana_percent == o.mana_percent
            && movement_type == o.movement_type
            && spawn_flags == o.spawn_flags
            && visibility_mod == o.visibility_mod
            && spawn_mask == o.spawn_mask
            && phase_mask == o.phase_mask;
      }
      bool operator!=(ExtFields const& o) const { return !(*this == o); }
    };
    ExtFields ext;
    ExtFields original_ext;
    [[nodiscard]] bool extDirty() const { return !(ext == original_ext); }

    // World radius of the ground selection circle, EXACTLY like the live client (see
    // bounding_radius above; the scale is already applied). Falls back to the model footprint
    // estimate when the DB has no bounding radius for this display.
    [[nodiscard]] float selectionRingWorldRadius() const
    {
      // Exact client formula: scale * sqrt( sqrt(dx^2+dy^2) * 0.5 ) on the stand-animation box
      // (ModelInstance::selectionRingRadius / Model::selection_base_radius). No per-creature data.
      if (model_instance.has_value())
      {
        float const radius = model_instance->selectionRingRadius();
        if (radius > 0.01f)
        {
          return std::max(0.25f, radius);
        }
      }
      // Fallback for spawns whose model has not loaded yet.
      if (bounding_radius > 0.0f)
      {
        return std::max(0.25f, bounding_radius);
      }
      return 0.5f;
    }

    CreatureSpawnOverlay() = default;
    CreatureSpawnOverlay(CreatureSpawnOverlay&&) noexcept = default;
    CreatureSpawnOverlay& operator=(CreatureSpawnOverlay&&) noexcept = default;
    CreatureSpawnOverlay(CreatureSpawnOverlay const&) = delete;
    CreatureSpawnOverlay& operator=(CreatureSpawnOverlay const&) = delete;
  };

  struct GameObjectSpawnOverlay
  {
    std::uint32_t guid = 0;
    std::uint32_t entry = 0;
    std::uint32_t display_id = 0;
    // Seasonal game-event membership (game_event_gameobject.event). See CreatureSpawnOverlay::event.
    std::int32_t event = 0;
    bool event_suppressed = false;
    std::string name;
    glm::vec3 pos = glm::vec3(0.0f);
    glm::vec3 original_pos = glm::vec3(0.0f);
    float orientation = 0.0f;
    float original_orientation = 0.0f;
    int animation_time_offset = 0;
    float template_scale = 1.0f;
    std::string model_path;
    bool model_create_failed = false;
    bool hovered = false;
    bool selected = false;
    bool pending_create = false;
    bool pending_delete = false; // marked for deletion (Del); exported as DELETE, undoable via Ctrl+Z
    bool dirty = false;
    std::optional<ModelInstance> model_instance;
    // WMO-model gameobjects (e.g. Turtle player housing: GameObjectDisplayInfo points at a .wmo):
    // rendered through the WMO pipeline instead of the M2 spawn path.
    std::optional<WMOInstance> wmo_instance;

    GameObjectSpawnOverlay() = default;
    GameObjectSpawnOverlay(GameObjectSpawnOverlay&&) noexcept = default;
    GameObjectSpawnOverlay& operator=(GameObjectSpawnOverlay&&) noexcept = default;
    GameObjectSpawnOverlay(GameObjectSpawnOverlay const&) = delete;
    GameObjectSpawnOverlay& operator=(GameObjectSpawnOverlay const&) = delete;
  };

protected:
  bool _unloading = false; // set true at the very start of ~World; see is_unloading()
  std::vector<selection_type> _current_selection;
  // std::unordered_map<std::string, std::vector<ModelInstance*>> _models_by_filename;
  Noggit::world_model_instances_storage _model_instance_storage;
  Noggit::world_tile_update_queue _tile_update_queue;
public:
  std::vector<selection_group> _selection_groups;

  MapIndex mapIndex;
  Noggit::map_horizon horizon;

  // Temporary variables for loading a WMO, if we have a global WMO.
  std::string mWmoFilename;
  ENTRY_MODF mWmoEntry;

  unsigned int getMapID();

  // Time of the day.
  float animtime;

  // Editor weather preview (client mechanism: blends the zone light toward its STORM param set and
  // drives the precipitation pass). 0 = none, 1 = rain, 2 = snow; intensity 0..1. Runtime-only.
  int weather_type = 0;
  float weather_intensity = 1.0f;
  // Surface splash/wake ripple spawn requests (client Water0Ripple port, RE doc 36): game-mode
  // code pushes; WorldRender drains into its ripple pool each frame. kind 0 = wake, 1 = splash.
  struct WaterRippleSpawn
  {
    glm::vec3 pos;
    float rot;
    float size0;
    float growth;
    float lifetime_s;
    float alpha_peak;
    int kind;
  };
  std::vector<WaterRippleSpawn> pending_ripples;
  // Like animtime but only advances while model animations are enabled, so toggling animations off
  // freezes (pauses) model/particle animation in place while liquid/terrain keep churning on animtime.
  float model_animtime = 0.0f;
  float time;
  float _models_emitter_dt = 0.0f;

  //! \brief Name of this map.
  std::string basename;

  explicit World(const std::string& name, int map_id, Noggit::NoggitRenderContext context, bool create_empty = false);
  ~World();

  // True once the World is being torn down (set first thing in ~World, before any member -- and thus any
  // MapTile -- is destroyed). MapTile::~MapTile checks this to SKIP its per-tile instance unload: during
  // full teardown the instance storage frees every M2/WMO instance at once, so the per-tile
  // derefTile()/remove_models_if_needed() dance is both pointless and unsafe (it frees instances shared
  // with other not-yet-destroyed tiles -> those tiles then deref a freed SceneObject = the map-exit crash).
  bool is_unloading() const { return _unloading; }

  void LoadSavedSelectionGroups();

  void saveSelectionGroups();

  void setBasename(const std::string& name);

  SceneObject* getObjectInstance(std::uint32_t uid);

  void update_models_emitters(float dt);
  // Per-frame emitter dt captured by update_models_emitters, read by the render pass to advance each
  // creature spawn's own per-instance particle state (see Model::swapInstanceEmitterState).
  float models_emitter_dt() const { return _models_emitter_dt; }

  unsigned int getAreaID (glm::vec3 const&);
  unsigned int getWMOAreaID(glm::vec3 const&);
  // True when pos sits below a liquid surface (used to switch the sky to the underwater LightParams set).
  bool camera_is_underwater(glm::vec3 const& pos);
  // Top-level zone id for a position (walks AreaTable ParentAreaID up from getAreaID to the zone, e.g.
  // a sub-area in Searing Gorge resolves to Searing Gorge). Returns -1 if the tile/area isn't loaded.
  unsigned int getZoneId(glm::vec3 const&);
  // ZoneMusic id for a position: WMOAreaTable.ZoneMusic when inside a WMO (dungeons/caves), else the
  // AreaTable parent chain. 0 = no music authored. wmo_field selects the WMOAreaTable column
  // (ZoneMusic vs IntroSound) so the same resolver serves looping music AND intro music.
  unsigned int getWMOZoneMusic(glm::vec3 const&, size_t wmo_field);
  int getZoneMusic(glm::vec3 const&);
  // One-shot INTRO music id (ZoneIntroMusicTable) for a position -- same resolution as getZoneMusic
  // but reads the IntroSound columns. 0 = no intro authored.
  int getZoneIntroMusic(glm::vec3 const&);
  // Zone ambience id (SoundAmbience.dbc) for a position -- same resolution walk over the
  // SoundAmbience columns (WMOAreaTable col 6, AreaTable col 7). 0 = none authored. (doc 38)
  int getZoneAmbience(glm::vec3 const&);
  // Shared resolver behind getZoneMusic/getZoneIntroMusic/getZoneAmbience: wmo_field /
  // area_field pick which WMOAreaTable / AreaTable column to read.
  int getZoneMusicField(glm::vec3 const&, size_t wmo_field, size_t area_field);
  // True if pos falls inside a loaded WMO's group AABB (i.e. the camera is standing inside a building/
  // dungeon interior, not merely inside its loose outer AABB). Used to drop the client's outdoor
  // FFXGlow floor when indoors (see renderBloomAndComposite).
  // out_h_depth (optional): how far HORIZONTALLY (x/z) the camera is past the nearest wall of the room it
  // is inside -- 0 at the boundary/entrance, growing with depth into the room. Feeds the fog's continuous
  // interior factor so the indoor/outdoor fog blends by position (the client's spatial model) instead of
  // snapping on the binary containment flip. 0 when the camera is not inside any interior group.
  bool camera_is_inside_wmo(glm::vec3 const& pos, float* out_h_depth = nullptr);
  // Blend the map's authored WMO fog spheres (MOFG) the camera is inside over the given ZONE fog, so the
  // one camera fog can be written to the MAIN UBO for terrain, doodads AND WMO geometry alike. color +
  // absolute end/start in/out (start = the pre-multiplied absolute distance); scale = fog-distance scale.
  void collect_camera_fog(glm::vec3 const& camera, float scale, glm::vec3& color, float& end, float& start_abs);
  // Sticky interior-fog state (see getInteriorFog): once a WMO fog is chosen it HOLDS while the camera
  // stays inside that instance's outer AABB -- room-AABB containment alone is gappy (Kara: one step and
  // the fog dropped). Cleared on leaving the WMO or entering an exterior group.
  unsigned _sticky_fog_uid = 0;
  int _sticky_fog_id = -1;
  // Gather every loaded indoor group's world AABB + room ambient (see InteriorVolume) once. Calls the
  // EXPENSIVE getGroupExtents per WMO -- callers MUST throttle this, never per-object-per-frame -- so
  // object interior tests become cheap AABB checks. Lights indoor objects by the room, not the sun.
  void collect_interior_volumes(std::vector<InteriorVolume>& out);
  // Cheap fingerprint of the LOADED WMO instance set (uids of finished instances). Changes the frame
  // a WMO finishes streaming in (or is added/removed), so the interior/fog volume gathers can refresh
  // IMMEDIATELY -- objects were visibly re-lit up to a second AFTER their room appeared (60-frame epoch).
  std::uint64_t loaded_wmo_fingerprint();
  // Gather every loaded WMO instance's MFOG entries in world space (see EnvFogVolume) for the
  // per-frame entity fog (the camera's fog context). Throttle like collect_interior_volumes.
  // Collects the group AABBs of every loaded WMO instance whose WMO has MORE than the default
  // MFOG entry -- the camera-fog resolve picks its containing group from these (note 27).
  void collect_fog_volumes(std::vector<WmoGroupFogVolume>& out);
  // If the camera is inside a WMO group that has authored interior fog (MFOG), fills color/start/end
  // and returns true. Used to apply that fog to the whole scene (terrain, doodads, light shafts).
  bool getInteriorFog(glm::vec3 const& pos, glm::vec3& out_color, float& out_start, float& out_end);
  void setAreaID(glm::vec3 const& pos, int id, bool adt,  float radius = -1.0f);

  Noggit::NoggitRenderContext getRenderContext() { return _context; };

  selection_result intersect (glm::mat4x4 const& model_view
                             , math::ray const&
                             , bool only_map
                             , bool do_objects
                             , bool draw_terrain
                             , bool draw_wmo
                             , bool draw_models
                             , bool draw_hidden_models
                             , bool draw_wmo_exterior
                             , bool draw_wmo_doodads = false // [game mode] collide with WMO-owned doodads
                             , float max_dist = 0.0f // > 0: skip objects farther than this from the ray
                                                     // origin (short physics probes; 0 = unlimited)
                             );

  MapChunk* getChunkAt(glm::vec3 const& pos);

  bool isInIndoorWmoGroup(std::array<glm::vec3, 2> obj_bounds, glm::mat4x4 obj_transform);

protected:
  // Information about the currently selected model / WMO / triangle.
  int _selected_model_count = 0;
  std::optional<glm::vec3> _multi_select_pivot;
public:

  Noggit::Rendering::WorldRender* renderer() { return &_renderer; }

  void update_selection_pivot();
  std::optional<glm::vec3> const& multi_select_pivot() const { return _multi_select_pivot; }

  // Selection related methods.
  bool is_selected(selection_type selection) const;
  bool is_selected(std::uint32_t uid) const;
  std::vector<selection_type> const& current_selection() const { return _current_selection; }
  std::vector<selected_object_type> const get_selected_objects() const;
  std::optional<selection_type> get_last_selected_model() const;
  bool has_selection() const { return !_current_selection.empty(); }
  bool has_multiple_model_selected() const { return _selected_model_count > 1; }
  int get_selected_model_count() const { return _selected_model_count; }
  // Unused in Red, models are now iterated by adt because of the occlusion check
  // std::unordered_map<std::string, std::vector<ModelInstance*>> get_models_by_filename() const& { return _models_by_filename;  } 
  void set_current_selection(selection_type entry);
  void add_to_selection(selection_type entry, bool skip_group = false);
  void remove_from_selection(selection_type entry, bool skip_group = false);
  void remove_from_selection(std::uint32_t uid, bool skip_group = false);
  void reset_selection();
  void delete_selected_models();
  glm::vec3 get_ground_height(glm::vec3 pos);
  void range_add_to_selection(glm::vec3 const& pos, float radius, bool remove);
  Noggit::world_model_instances_storage& getModelInstanceStorage() { return _model_instance_storage; };

  enum class m2_scaling_type
  {
    set,
    add,
    mult
  };

  void snap_selected_models_to_the_ground();
  void scale_selected_models(float v, m2_scaling_type type);
  void move_selected_models(float dx, float dy, float dz);
  void move_model(selection_type entry, float dx, float dy, float dz);
  void move_selected_models(glm::vec3 const& delta)
  {
    move_selected_models(delta.x, delta.y, delta.z);
  }
  void set_selected_models_pos(float x, float y, float z, bool change_height = true)
  {
    return set_selected_models_pos({x,y,z}, change_height);
  }
  void set_selected_models_pos(glm::vec3 const& pos, bool change_height = true);
  void set_model_pos(selection_type entry, glm::vec3 const& pos, bool change_height = true);
  void rotate_selected_models(math::degrees rx, math::degrees ry, math::degrees rz, bool use_pivot);
  void rotate_selected_models_randomly(float minX, float maxX, float minY, float maxY, float minZ, float maxZ);
  void set_selected_models_rotation(math::degrees rx, math::degrees ry, math::degrees rz);

  void update_selected_model_groups();

  // Checks the normal of the terrain on model origin and rotates to that spot.
  void rotate_selected_models_to_ground_normal(bool smoothNormals);

  bool GetVertex(float x, float z, glm::vec3 *V) const;

  // check if the cursor is under map or in an unloaded tile
  bool isUnderMap(glm::vec3 const& pos);

  template<typename Fun>
  bool for_all_chunks_in_range ( glm::vec3 const& pos
                               , float radius
                               , Fun&& /* MapChunk* -> bool changed */
                               );
  template<typename Fun, typename Post>
  bool for_all_chunks_in_range ( glm::vec3 const& pos
                               , float radius
                               , Fun&& /* MapChunk* -> bool changed */
                               , Post&& /* MapChunk* -> void; called for all changed chunks */
                               );

  template<typename Fun>
  bool for_all_chunks_in_rect ( glm::vec3 const& pos
    , float radius
    , Fun&& /* MapChunk* -> bool changed */
  );

  template<typename Fun, typename Post>
  bool for_all_chunks_in_rect (glm::vec3 const& pos
    , float radius
    , Fun&& /* MapChunk* -> bool changed */
    , Post&& /* MapChunk* -> void; called for all changed chunks */
  );

  template<typename Fun>
    void for_all_chunks_on_tile (glm::vec3 const& pos, Fun&&);

    template<typename Fun>
    void for_all_chunks_on_tile(MapTile* tile, Fun&& fun);

  template<typename Fun>
    void for_chunk_at(glm::vec3 const& pos, Fun&& fun);
  template<typename Fun>
    auto for_maybe_chunk_at (glm::vec3 const& pos, Fun&& fun) -> std::optional<decltype (fun (nullptr))>;

  template<typename Fun>
    void for_tile_at(const TileIndex& pos, Fun&&);

  template<typename Fun>
    void for_tile_at_force(const TileIndex& pos, Fun&&);

  void changeObjectsWithTerrain(glm::vec3 const& pos, float change, float radius, int BrushType, float inner_radius, bool iter_wmos_ = true, bool iter_m2s = true);
  void changeTerrain(glm::vec3 const& pos, float change, float radius, int BrushType, float inner_radius);
  std::vector<selected_object_type> getObjectsInRange(glm::vec3 const& pos, float radius, bool ignore_height = true, bool iter_wmos_ = true, bool iter_m2s = true);
  void changeShader(glm::vec3 const& pos, glm::vec4 const& color, float change, float radius, bool editMode);
  void stampShader(glm::vec3 const& pos, glm::vec4 const& color, float change, float radius, bool editMode, QImage* img, bool paint, bool use_image_colors);
  glm::vec3 pickShaderColor(glm::vec3 const& pos);
  void flattenTerrain(glm::vec3 const& pos, float remain, float radius, int BrushType, flatten_mode const& mode, const glm::vec3& origin, math::degrees angle, math::degrees orientation);
  std::vector<std::pair<SceneObject*, float>> getObjectsGroundDistance(glm::vec3 const& pos, float radius, bool iter_wmos_, bool iter_m2s);
  void blurTerrain(glm::vec3 const& pos, float remain, float radius, int BrushType, flatten_mode const& mode);
  bool paintTexture(glm::vec3 const& pos, Brush *brush, float strength, float pressure, scoped_blp_texture_reference texture);
  bool stampTexture(glm::vec3 const& pos, Brush *brush, float strength, float pressure, scoped_blp_texture_reference texture, QImage* img, bool paint);
  bool sprayTexture(glm::vec3 const& pos, Brush *brush, float strength, float pressure, float spraySize, float sprayPressure, scoped_blp_texture_reference texture);
  bool replaceTexture(glm::vec3 const& pos, float radius, scoped_blp_texture_reference const& old_texture, scoped_blp_texture_reference new_texture, bool entire_chunk = false);

  void eraseTextures(glm::vec3 const& pos);
  void overwriteTextureAtCurrentChunk(glm::vec3 const& pos, scoped_blp_texture_reference const& oldTexture, scoped_blp_texture_reference newTexture);
  void setBaseTexture(glm::vec3 const& pos);
  void clear_shadows(glm::vec3 const& pos);
  void clearTextures(glm::vec3 const& pos);
  void swapTexture(glm::vec3 const& pos, scoped_blp_texture_reference tex);
  void swapTextureGlobal(scoped_blp_texture_reference tex);
  void removeTexture(glm::vec3 const& pos, scoped_blp_texture_reference tex);
  void removeTexDuplicateOnADT(glm::vec3 const& pos);
  void change_texture_flag(glm::vec3 const& pos, scoped_blp_texture_reference const& tex, std::size_t flag, bool add);

  void setHole(glm::vec3 const& pos, float radius, bool big, bool hole);
  void setHoleADT(glm::vec3 const& pos, bool hole);

  void exportADTAlphamap(glm::vec3 const& pos);
  void exportADTNormalmap(glm::vec3 const& pos);
  void exportADTAlphamap(glm::vec3 const& pos, std::string const& filename);
  void exportADTHeightmap(glm::vec3 const& pos, float min_height, float max_height);
  void exportADTVertexColorMap(glm::vec3 const& pos);
  void exportAllADTsAlphamap();
  void exportAllADTsAlphamap(std::string const& filename);
  void exportAllADTsHeightmap();
  void exportAllADTsVertexColorMap();

  void importADTAlphamap(glm::vec3 const& pos, QImage const& image, unsigned layer);
  void importADTAlphamap(glm::vec3 const& pos);
  void importADTHeightmap(glm::vec3 const& pos, QImage const& image, float multiplier, unsigned mode, bool tiledEdges);
  void importADTHeightmap(glm::vec3 const& pos, float multiplier, unsigned mode, bool tiledEdges);
  void importADTVertexColorMap(glm::vec3 const& pos, int mode, bool tiledEdges);
  void importADTVertexColorMap(glm::vec3 const& pos, QImage const& image, int mode, bool tiledEdges);

  void importAllADTsAlphamaps();
  void importAllADTsHeightmaps(float multiplier, unsigned mode, bool tiledEdges);
  void importAllADTVertexColorMaps(unsigned mode, bool tiledEdges);

  void ensureAllTilesetsADT(glm::vec3 const& pos);
  void ensureAllTilesetsAllADTs();

  void notifyTileRendererOnSelectedTextureChange();

  void addM2 ( BlizzardArchive::Listfile::FileKey const& file_key
             , glm::vec3 newPos
             , float scale, math::degrees::vec3 rotation
             , Noggit::object_paste_params*
             );
  void addWMO ( BlizzardArchive::Listfile::FileKey const& file_key
              , glm::vec3 newPos
              , math::degrees::vec3 rotation
              );

  ModelInstance* addM2AndGetInstance ( BlizzardArchive::Listfile::FileKey const& file_key
      , glm::vec3 newPos
      , float scale, math::degrees::vec3 rotation
      , Noggit::object_paste_params*
      , bool ignore_params = false
  );

  WMOInstance* addWMOAndGetInstance ( BlizzardArchive::Listfile::FileKey const& file_key
      , glm::vec3 newPos
      , math::degrees::vec3 rotation
  );

  auto stamp(glm::vec3 const& pos, float dt, QImage const* img, float radiusOuter
  , float radiusInner, int BrushType, bool sculpt) -> void;

  // add a m2 instance to the world (needs to be positioned already), return the uid
  std::uint32_t add_model_instance(ModelInstance model_instance, bool from_reloading);
  // add a wmo instance to the world (needs to be positioned already), return the uid
  std::uint32_t add_wmo_instance(WMOInstance wmo_instance, bool from_reloading);

  std::optional<selection_type> get_model(std::uint32_t uid);
  void remove_models_if_needed(std::vector<uint32_t> const& uids);

  void reload_tile(TileIndex const& tile);

  void updateTilesEntry(selection_type const& entry, model_update type);
  void updateTilesEntry(SceneObject* entry, model_update type);
  // mark_changed=false: register into tiles without flagging them dirty (load-time UID-reassign / reload;
  // see world_tile_update_queue::queue_update). Real edits use the default so their changes still persist.
  void updateTilesWMO(WMOInstance* wmo, model_update type, bool mark_changed = true);
  void updateTilesModel(ModelInstance* m2, model_update type, bool mark_changed = true);
  void wait_for_all_tile_updates();

  void deleteModelInstance(int uid);
  void deleteWMOInstance(int uid);
  void deleteInstance(int uid);

  bool uid_duplicates_found() const;
  void delete_duplicate_model_and_wmo_instances();
  // used after the uid fix all
  void unload_every_model_and_wmo_instance();

	static bool IsEditableWorld(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow& record);
  static bool IsEditableWorld(DBCFile::Record const& record);

    static bool IsWMOWorld(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow& record);

  static bool IsWMOWorld(DBCFile::Record const& record);

  void clearHeight(glm::vec3 const& pos);
  void clearAllModelsOnADT(TileIndex const& tile);

  // liquids
  void paintLiquid( glm::vec3 const& pos
                  , float radius
                  , int liquid_id
                  , bool add
                  , math::radians const& angle
                  , math::radians const& orientation
                  , bool lock
                  , glm::vec3 const& origin
                  , bool override_height
                  , bool override_liquid_id
                  , float opacity_factor
                  );
  void CropWaterADT(const TileIndex& pos);
  void setWaterType(const TileIndex& pos, int type, int layer);
  int getWaterType(const TileIndex& tile, int layer);
  void autoGenWaterTrans(const TileIndex&, float factor);


  void fixAllGaps();

  void convert_alphamap(bool to_big_alpha);

  bool deselectVertices(glm::vec3 const& pos, float radius);
  void selectVertices(glm::vec3 const& pos, float radius);
  void moveVertices(float h);
  void orientVertices ( glm::vec3 const& ref_pos
                      , math::degrees vertex_angle
                      , math::degrees vertex_orientation
                      );
  void flattenVertices (float height);

  void updateSelectedVertices();
  void updateVertexCenter();
  void clearVertexSelection();

  void deleteObjects(std::vector<selection_type> const& types);

  float getMaxTileHeight(const TileIndex& tile);

  glm::vec3 const& vertexCenter();

  void recalc_norms (MapChunk*) const;

  Noggit::VertexSelectionCache getVertexSelectionCache();
  void setVertexSelectionCache(Noggit::VertexSelectionCache& cache);

  bool need_model_updates = false;

  void loadAllTiles();
  unsigned getNumLoadedTiles() const { return _n_loaded_tiles; };
  unsigned getNumRenderedTiles() const { return _n_rendered_tiles; };
  unsigned getNumRenderedObjects() const { return _n_rendered_objects; };

  void select_objects_in_area(
      const std::array<glm::vec2, 2> selection_box, 
      bool reset_selection,
      glm::mat4x4 view,
      glm::mat4x4 projection,
      int viewport_width,
      int viewport_height,
      float user_depth,
      glm::vec3 camera_position
  );

  void add_object_group_from_selection();
  void remove_selection_group(selection_group* group);

  void clear_selection_groups();
  bool reloadCreatureSpawns();
  void ensureCreatureSpawnsLoaded();
  void clearCreatureSpawns();
  bool ensureCreatureSpawnModel(CreatureSpawnOverlay& spawn);
  // [mem 2026-08-26] inverse of ensureCreatureSpawnModel: frees the spawn's model/mount/attachment
  // instances AND the per-instance state they parked on the shared models. Called by the render
  // loop for spawns far past the draw distance -- without it every spawn ever approached kept its
  // models (and their texture refs) for the whole session.
  void releaseCreatureSpawnModel(CreatureSpawnOverlay& spawn);
  bool ensureGameObjectSpawnModel(GameObjectSpawnOverlay& spawn);
  std::vector<std::pair<std::size_t, std::string>> applyCreatureSpawnModelAppearance(CreatureSpawnOverlay const& spawn,
                                                                                     ModelInstance& model_instance,
                                                                                     Noggit::NoggitRenderContext context) const;
  void setDrawCreatureSpawns(bool state) { _draw_creature_spawns = state; }
  bool drawCreatureSpawns() const { return _draw_creature_spawns; }
  // Selection markers (the disc under a spawn) are only drawn while that spawn's editing tool is
  // active, even if the spawn models themselves stay visible via the view toggle.
  void setDrawCreatureMarkers(bool state) { _draw_creature_markers = state; }
  bool drawCreatureMarkers() const { return _draw_creature_markers; }
  void setDrawGameObjectMarkers(bool state) { _draw_gameobject_markers = state; }
  bool drawGameObjectMarkers() const { return _draw_gameobject_markers; }
  // Patrol-path overlay (creature waypoint lines). Loaded lazily from the DB the first time it's
  // shown; toggled from the creature tool's top action bar.
  void setDrawCreaturePatrolPaths(bool state) { _draw_creature_patrol_paths = state; }
  bool drawCreaturePatrolPaths() const { return _draw_creature_patrol_paths; }
  void ensureCreaturePatrolPathsLoaded();
  // guid -> ordered waypoint positions in client space (does NOT include the spawn position itself).
  std::unordered_map<std::uint32_t, std::vector<glm::vec3>> const& creaturePatrolPaths() const { return _creature_patrol_paths; }
  bool hasCreatureSpawnsLoaded() const { return _creature_spawns_loaded; }
  std::size_t creatureSpawnCount() const { return _creature_spawns.size(); }
  std::size_t creatureSpawnModelCount() const;
  std::size_t dirtyCreatureSpawnCount() const;
  std::string const& creatureSpawnStatus() const { return _creature_spawn_status; }
  std::vector<CreatureSpawnOverlay>& creatureSpawns() { return _creature_spawns; }
  std::vector<CreatureSpawnOverlay> const& creatureSpawns() const { return _creature_spawns; }
  std::vector<GameObjectSpawnOverlay>& gameObjectSpawns() { return _gameobject_spawns; }
  std::vector<GameObjectSpawnOverlay> const& gameObjectSpawns() const { return _gameobject_spawns; }
  CreatureSpawnOverlay* findCreatureSpawn(std::uint32_t guid);
  CreatureSpawnOverlay const* findCreatureSpawn(std::uint32_t guid) const;

  // Which extended `creature` columns the connected DB has (resolved REAL column names; empty =
  // the schema has no equivalent -> editor field disabled, exporter omits it). Mirrors
  // mysql::CreatureSpawnTableColumns (kept separate so World.h stays free of the mysql headers).
  struct CreatureSpawnColumns
  {
    std::string entry_col = "id";
    std::string id2_col, id3_col, id4_col;
    std::string respawn_min_col, respawn_max_col;
    std::string wander_col;
    std::string health_percent_col, mana_percent_col;
    bool health_mana_absolute = false; // AzerothCore curhealth/curmana (absolute, 0 = full) vs Turtle percents
    std::string movement_col;
    std::string spawn_flags_col;
    std::string visibility_col;
    std::string spawn_mask_col, phase_mask_col; // cmangos + AzerothCore
    // cmangos alternative to id2..id4: creature_spawn_entry (guid, entry) rows + creature.id = 0
    // = random entry per spawn. The alt-id editor slots map onto that table on such schemas.
    bool spawn_entry_table = false;
  };
  CreatureSpawnColumns const& creatureSpawnColumns() const { return _creature_spawn_columns; }

  // Attachment models (helm / shoulders / weapons) for one spawn, resolved for the picker's 3D
  // PREVIEW: it draws these as extra instances at the body's bind-pose attachment points. (The
  // world path renders them via CreatureSpawnOverlay::attachment_models instead.) Without them a
  // helmeted NPC previews BALD -- the helmet rule hides the hair geoset and nothing draws the helm.
  struct CreaturePreviewAttachment
  {
    int attachment_id = -1;
    std::string model_path;
    std::vector<std::pair<std::size_t, std::string>> texture_overrides;
  };
  std::vector<CreaturePreviewAttachment> resolveCreaturePreviewAttachments(CreatureSpawnOverlay const& spawn);

  // Wander-distance visualization (creature editor): while the wander_distance field is focused,
  // WorldRender draws a ground ring of this radius (yards) at this center. nullopt = off.
  struct WanderViz { glm::vec3 center; float radius; };
  std::optional<WanderViz> wander_viz;

  // [game mode] the Game View player character: a creature-display-driven model walked around in
  // 3rd person. Rides the whole creature-spawn model pipeline (display id -> model + skin +
  // attachments + anims) but lives outside _creature_spawns so it never touches save/export and
  // draws independently of the creature-spawns overlay toggle.
  bool setGameCharacterDisplayId(std::uint32_t display_id);
  void updateGameCharacter(glm::vec3 const& pos, float orientation_deg, int anim_id,
                           float lower_body_twist_rad = 0.0f, float anim_time_scale = 1.0f,
                           float body_pitch_deg = 0.0f, bool force_anim_restart = false);
  // Authored moveSpeed (yd/s) of one of the character's animations; 0 when unavailable.
  float gameCharacterAnimMoveSpeed(int anim_id);
  // Underwater breath bubbles (client: HARDCODED "Breath Underwater" -> Particles\Bubbles.m2 at
  // the Breath attachment 17), toggled while the character's head is below the water surface.
  void setGameCharacterBubbles(bool on, float surface_y = 1.0e30f);
  // [game mode] client swim law (335a FUN_00730d10): thresholds scale with the UNIT's collision
  // height -- CreatureModelData.CollisionHeight x display scale for the CURRENT character
  // display, cached by setGameCharacterDisplayId (Turtle HumanMale = 2.031).
  float gameCharacterCollisionHeight() const { return _game_char_collision_height; }

  // [game mode] sorted list of REAL CreatureDisplayInfo row ids (lazy, cached) -- the Game Mode
  // panel's display-id spinner steps through these instead of every integer.
  std::vector<std::uint32_t> const& creatureDisplayIds();

  // [game mode] ADT liquid surface height covering pos (top-most layer); nullopt when dry.
  // Bilinear over the layer's 9x9 vertex grid, subchunk-coverage aware.
  std::optional<float> getLiquidHeightAt(glm::vec3 const& pos);

  // Like getLiquidHeightAt but also reports WHICH liquid covers pos ({surface height, liquid id}) --
  // ADT layer liquidID / WMO group liquid id -- for the type-aware submersion effects (motes only
  // in water, etc). WMO part needs the probe cache (game mode); ADT part always works.
  std::optional<std::pair<float, int>> getLiquidAt(glm::vec3 const& pos);

  // [doc 38 water loops] Base liquid CLASS of a liquid id: 0 water/river, 1 ocean, 2 magma,
  // 3 slime -- the SoundWaterType.LiquidClass axis. Mirrors liquid_layer::mclq_liquid_type.
  static int liquidClassForId(int liquid_id);
  // Nearest liquid of each class within `radius` around `center` (surface-plane distance).
  // Samples a ring of getLiquidAt probes (footstep-rate cost); feeds the WaterSoundPlayer
  // (doc 38 FUN_00462b50). liquid_id is carried through so the 3.3.5a path can read the
  // per-variant loop sound straight off LiquidType.dbc.
  struct WaterLoopSample
  {
    bool found = false;
    float distance = 0.0f;
    int liquid_id = 0;
  };
  void sampleWaterLoopSources(glm::vec3 const& center, float radius,
                              std::array<WaterLoopSample, 4>& out);

  // [game mode] PROBE CACHE: physics/camera rays fire ~15x per frame, and walking the whole
  // instance storage per ray was the game-mode frame lag. The cache holds the collidables near
  // the player (refreshed on movement / periodically); intersectProbe tests only those.
  void ensureProbeCache(glm::vec3 const& center);
  selection_result intersectProbe(glm::mat4x4 const& model_view, math::ray const&, float max_dist);
  void setGameCharacterVisible(bool visible) { _game_character_visible = visible; }
  // Authored length (ms) of one of the character's animations; 0 when the model isn't ready or
  // lacks the id. Used to play JumpStart to its real end before switching to the Jump loop.
  int gameCharacterAnimLengthMs(int anim_id);
  // Fire times (anim-local ms, sorted) of an M2 anim event on the game character's model --
  // e.g. '$FSD' footfalls of the Run cycle (doc 38). Empty while the model loads / none authored.
  std::vector<int> gameCharacterAnimEventTimes(std::uint32_t fourcc, int anim_id);
  // The character's CreatureFootstepID: display.Sound override else CreatureModelData.SoundID
  // -> CreatureSoundData column 9 (client resolution, doc 38). 0 = none (no footstep sounds).
  std::uint32_t gameCharacterFootstepId();
  // The CHARACTER water-entry/exit splash: SoundEntries SoundType 21
  // ("CharacterSplashSoundSmall/Medium/large", dir Sound\Character\Footsteps\EnterWaterSplash).
  // This is the BODY hitting/leaving the water -- a different lane from the type-20 footstep
  // wading splash the FootstepTerrainLookup splash column provides (doc 38 round 18). 0 = the
  // client data has no such row.
  int characterSplashSoundEntry();

  // Any other SoundEntries column of the game character's CreatureSoundData row (same display ->
  // CreatureSoundData resolution as the footstep id): e.g. CreatureSoundDataDB::Wound for the
  // damage-taken vocal. 0 = the row does not author that lane.
  std::uint32_t gameCharacterSoundEntry(std::size_t column);
  // TerrainType ROW under a world position = the 3.3.5a client's CWorld ground-type cast
  // (FUN_007c2a70, RE'd 2026-09-09): a ray from 0.1 above the feet 1000 down through every WMO
  // (wmo_ground_query: the physics "support" face and the visible "typed" face are tracked
  // independently); the terrain wins only when it is closer than the support face. WMO -> the
  // typed face's MOMT.ground_type, or -1 when no typed face lies below (collision-only ghost
  // floors); terrain -> MapChunk::groundTerrainTypeRowAt. -1 = unknown, which the footstep
  // resolve turns into TerrainType 0. model_view = the camera MV the terrain probe expects.
  int groundTerrainTypeAt(glm::vec3 const& pos, glm::mat4x4 const& model_view);
  // FootstepTerrainLookup resolve: (footstep id x TerrainType row's SoundClass x wet) ->
  // SoundEntries id (0 = none). Rows scanned once into a cache. Client FUN_004cf170: a row that
  // yields nothing (unknown -1, or no lookup entry for its class) is retried as row 0 (Dirt).
  int footstepSoundEntry(std::uint32_t footstep_id, int terrain_row, bool splash);

  // Spell details for the spawned creatures' permanent auras (fetched once per spawn reload from the
  // server's spell_template). Keyed by spell id; used for aura state-kit visuals and the creature-info
  // UI (name/description/icon).
  struct SpellInfo
  {
    std::uint32_t entry = 0;
    std::uint32_t spell_visual = 0;
    std::uint32_t icon_id = 0;
    std::uint32_t school = 0;
    std::string name;
    std::string description;
  };
  std::map<std::uint32_t, SpellInfo> const& spellInfos() const { return _spell_infos; }

  // GameObject editing (mirrors the creature equivalents). GameObjects are loaded alongside creatures
  // by reloadCreatureSpawns(), so loading just ensures that ran.
  void setDrawGameObjectSpawns(bool state) { _draw_gameobject_spawns = state; }
  bool drawGameObjectSpawns() const { return _draw_gameobject_spawns; }
  void ensureGameObjectSpawnsLoaded();
  std::size_t gameObjectSpawnCount() const { return _gameobject_spawns.size(); }
  std::size_t gameObjectSpawnModelCount() const;
  std::size_t dirtyGameObjectSpawnCount() const;
  GameObjectSpawnOverlay* findGameObjectSpawn(std::uint32_t guid);
  GameObjectSpawnOverlay const* findGameObjectSpawn(std::uint32_t guid) const;

  // --- Seasonal game-event visibility filter (view-only; never affects save/export) ---
  // Populated from game_event + game_event_creature/_gameobject when spawns load. A spawn's `event` is
  // 0 for base-world spawns (always shown). Marking an event "active" reveals its event>0 spawns and
  // hides its event<0 ("spawn except during") spawns; recomputeEventSuppression() caches the result on
  // each overlay's event_suppressed. Default: no events active => base world only.
  void setEventActive(std::int32_t entry, bool active);
  bool isEventActive(std::int32_t entry) const { return _active_events.count(entry) != 0; }
  void clearActiveEvents();
  void recomputeEventSuppression();
  // game_event entry -> description for every event in the DB (labels for the Seasonal Events panel).
  std::map<std::int32_t, std::string> const& gameEventNames() const { return _game_event_names; }
  // Event entries (abs value, excluding 0) that actually have spawns on the currently loaded map.
  std::set<std::int32_t> spawnedEventEntries() const;

protected:
  // void update_models_by_filename();

  std::unordered_set<MapChunk*>& vertexBorderChunks();

  std::unordered_set<MapTile*> _vertex_tiles;
  std::unordered_set<MapChunk*> _vertex_chunks;
  std::unordered_set<MapChunk*> _vertex_border_chunks;
  std::unordered_set<glm::vec3*> _vertices_selected;
  glm::vec3 _vertex_center;
  bool _vertex_center_updated = false;
  bool _vertex_border_updated = false;

  QSettings* _settings;

  Noggit::NoggitRenderContext _context;
  bool _draw_creature_spawns = false;
  bool _draw_creature_markers = false;
  bool _draw_gameobject_spawns = false;
  bool _draw_gameobject_markers = false;
  bool _draw_creature_patrol_paths = false;
  bool _patrol_paths_load_attempted = false;
  std::unordered_map<std::uint32_t, std::vector<glm::vec3>> _creature_patrol_paths;
  bool _creature_spawns_loaded = false;
  bool _creature_spawns_load_attempted = false;
  std::string _creature_spawn_status;
  std::vector<CreatureSpawnOverlay> _creature_spawns;
  CreatureSpawnColumns _creature_spawn_columns;
  // [game mode] see setGameCharacterDisplayId; drawn by WorldRender when visible.
  CreatureSpawnOverlay _game_character;
  bool _game_character_visible = false;
  bool _game_character_bubbles = false;
  float _game_char_collision_height = 2.031f; // see gameCharacterCollisionHeight()
  std::vector<std::uint32_t> _creature_display_ids; // see creatureDisplayIds()
  // [game mode] probe cache state (see ensureProbeCache / intersectProbe)
  static constexpr float k_probe_cache_radius = 45.0f; // covers the 25yd camera boom + corners + margin
  glm::vec3 _probe_cache_center = glm::vec3(0.0f);
  bool _probe_cache_valid = false;
  int _probe_cache_age = 0;
  std::vector<WMOInstance*> _probe_cache_wmos;
  std::vector<ModelInstance*> _probe_cache_m2s;
  // [perf 2026-08-19] Flattened near-and-COLLIDING WMO doodads (a city WMO holds thousands; only those with a
  // collision mesh within the cache radius are kept). intersectProbe fires ~220 short rays/frame (camera boom
  // clearance) and each one used to walk EVERY doodad of the containing WMO to distance-reject it. Testing this
  // pre-filtered near-set instead is behaviour-identical for physics probes (max_dist << cache radius).
  std::vector<wmo_doodad_instance*> _probe_cache_wmo_doodads;
  std::vector<GameObjectSpawnOverlay> _gameobject_spawns;
  std::map<std::uint32_t, SpellInfo> _spell_infos;
  // Seasonal event filter state (see the accessors above).
  std::unordered_set<std::int32_t> _active_events;
  std::map<std::int32_t, std::string> _game_event_names;

  std::array<std::pair<std::pair<int, int>, MapTile*>, 64 * 64 > _loaded_tiles_buffer;

  Noggit::Rendering::WorldRender _renderer;

  // Debug metrics
  unsigned _n_loaded_tiles;
  unsigned _n_rendered_tiles;

  // unsigned _n_loaded_objects; // done from instance storage size currently
  unsigned _n_rendered_objects;

};
