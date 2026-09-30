// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/ray.hpp>
#include <noggit/Misc.h>
#include <noggit/Selection.h>
#include <noggit/BoolToggleProperty.hpp>
#include <noggit/Camera.hpp>
#include <noggit/tool_enums.hpp>
#include <noggit/ui/ObjectEditor.h>
#include <noggit/ui/MinimapCreator.hpp>
#include <noggit/ui/UidFixWindow.hpp>
#include <noggit/unsigned_int_property.hpp>
#include <noggit/ui/tools/AssetBrowser/Ui/AssetBrowser.hpp>
#include <noggit/ui/tools/ViewportGizmo/ViewportGizmo.hpp>
#include <noggit/ui/tools/ViewportManager/ViewportManager.hpp>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>
#include <noggit/TabletManager.hpp>
#include <external/qtimgui/QtImGui.h>
#include <opengl/texture.hpp>
#include <opengl/scoped.hpp>
#include <optional>

#include <QtCore/QElapsedTimer>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QImage>   // [2026-09-04] _vk_ui_image: the editor overlay handed to Vulkan
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QOpenGLWidget>
#include <QWidgetAction>
#include <QOpenGLContext>

#include <forward_list>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <array>
#include <optional>

#include <ui_MapViewOverlay.h>


class World;
class QCheckBox;
class QComboBox;
class QSpinBox;
class QFormLayout;
class QLineEdit;
class QGroupBox;
class QListWidget;
class QVBoxLayout;
class QMenu;
class QWindow;
class QToolButton;
class QToolBar;
class QListWidgetItem;
class QPushButton;
class QTreeWidget;

namespace Noggit::Ui::Windows
{
    class NoggitWindow;
}

namespace Noggit
{

  namespace Ui::Tools::ViewToolbar::Ui
  {
    class ViewToolbar;
  }

  namespace Ui
  {
    class TimeGlobeWidget;
    class ZoneMusicPlayer;
    class CreatureInfoPanel;
  }

  namespace Ui::Tools
  {
    class BrushStack;
    class LightEditor;

    namespace ChunkManipulator
    {
      class ChunkManipulatorPanel;
    }
  }

  namespace Scripting
  {
    class scripting_tool;
  }

  class Camera;

	
  namespace Ui
  {
    class detail_infos;
    class flatten_blur_tool;
    class help;
    class minimap_widget;
    class ShaderTool;
    class TerrainTool;
    class texture_picker;
    class texturing_tool;
    class toolbar;
    class water;
    class zone_id_browser;
    class texture_palette_small;
    class hole_tool;
    struct tileset_chooser;
    class ObjectPalette;
  }
}

enum class save_mode
{
  current,
  changed,
  all
};

class MapView : public Noggit::Ui::Tools::ViewportManager::Viewport
{
  Q_OBJECT
public:
  bool _mod_alt_down = false;
  bool _mod_ctrl_down = false;
  bool _mod_shift_down = false;
  bool _mod_space_down = false;
  bool _mod_num_down = false;

  bool  leftMouse = false;
  bool  leftClicked = false;
  bool  rightMouse = false;

  std::unique_ptr<World> _world;
  Noggit::Camera _camera;
  // [harness] Screenshot requested for THIS frame, captured at the end of paintGL. Reading after
  // paintGL returns samples whatever framebuffer happens to be bound then, which is not reliably
  // the one the frame was drawn into -- that produced a pure-black capture of a scene that had in
  // fact rendered correctly (twice: findings 47c and 60).
  std::string _pending_screenshot;
  void captureFrameNow(std::string const& path);

private:

  float _2d_zoom = 1.f;
  float moving, strafing, updown, mousedir, turn, lookat;
  CursorType _cursorType;
  glm::vec3 _cursor_pos;
  QPoint _drag_start_pos;
  QPoint _right_click_pos;
  float _cursorRotation;
  bool look, freelook;
  bool ui_hidden = false;

  bool _camera_moved_since_last_draw = true;

  std::array<Qt::Key, 6> _inputs = {Qt::Key_W, Qt::Key_S, Qt::Key_D, Qt::Key_A, Qt::Key_Q, Qt::Key_E};
  void checkInputsSettings();

public:
  Noggit::BoolToggleProperty _draw_vertex_color = {true};
  Noggit::BoolToggleProperty _draw_baked_shadows = { true };
  Noggit::BoolToggleProperty _draw_climb = {false};
  Noggit::BoolToggleProperty _draw_contour = {false};
  Noggit::BoolToggleProperty _draw_mfbo = {false};
  Noggit::BoolToggleProperty _draw_wireframe = {false};
  Noggit::BoolToggleProperty _draw_lines = {false};
  Noggit::BoolToggleProperty _draw_terrain = {true};
  Noggit::BoolToggleProperty _draw_wmo = {true};
  Noggit::BoolToggleProperty _draw_water = {true};
  Noggit::BoolToggleProperty _draw_clouds = {true};
  Noggit::BoolToggleProperty _draw_sun = {true};
  Noggit::BoolToggleProperty _draw_moon = {true};
  Noggit::BoolToggleProperty _draw_bloom = {true};
  Noggit::BoolToggleProperty _draw_ground_clutter = {true};
  // Opt-in coarse WDL-horizon occlusion (perf). Default off; WorldRender reads the QSetting each
  // frame, so the toggle persists on change.
  Noggit::BoolToggleProperty _wdl_horizon_occlusion = {false};
  Noggit::BoolToggleProperty _draw_wmo_doodads = {true};
  Noggit::BoolToggleProperty _draw_wmo_exterior = { true };
  Noggit::BoolToggleProperty _draw_models = {true};
  Noggit::BoolToggleProperty _draw_model_animations = {true};
  Noggit::BoolToggleProperty _draw_hole_lines = {false};
  Noggit::BoolToggleProperty _draw_models_with_box = {false};
  Noggit::BoolToggleProperty _draw_fog = {true}; // parity default: in-game always has atmospheric fog (DBC-driven). Toggle off (F12 / Graphics tab) for far editing.
  Noggit::Ui::ZoneMusicPlayer* _zone_music_player = nullptr;
  Noggit::BoolToggleProperty _draw_hidden_models = {false};
  Noggit::BoolToggleProperty _draw_occlusion_boxes = {false};
  Noggit::BoolToggleProperty _game_mode_camera = { false };
  // [game mode 2026-08-10] client-like physics state for the game-view camera (tick block in tick()):
  // vertical velocity + grounded flag. Ground follow includes WMO surfaces (roofs, bridges, stairs).
  float _game_vertical_speed = 0.0f;
  bool _game_grounded = false;
  // horizontal velocity locked in at the moment the feet leave the ground (jump or edge-drop).
  // The client's air physics are ballistic: keyboard input cannot steer a jump/fall, the body
  // keeps the takeoff velocity until it lands -- so while airborne this replays instead of WASD.
  glm::vec3 _game_air_velocity = glm::vec3(0.0f);
  // ONE-TIME AIR STEER (client, RE'd FUN_00988a20/FUN_00988b00 + speed getter FUN_00987570):
  // while FALLING, a movement key press with NO movement flags held applies ONCE -- at WALK
  // speed (the getter's air-move parameter returns min(walk, run)) -- and every later press or
  // release only sets a PENDING_* moveflag resolved at landing. So a stationary jump can be
  // steered once, slightly, toward the pressed direction; a running jump stays fully locked.
  bool _game_air_steer_allowed = false; // takeoff happened with no movement input held
  bool _game_air_steer_used = false;
  // the client's moveflags FREEZE during a fall (presses/releases only set PENDING_* bits) --
  // these mirror that: the movement input captured at takeoff, replaced ONCE if the air steer
  // applies. The strafe-twist posture and landing rules read THESE while airborne, so a running
  // strafe-jump keeps its lean and the air steer turns the body like the ground strafe would.
  float _game_air_input_moving = 0.0f;
  float _game_air_input_strafing = 0.0f;
  // client jump is EDGE-triggered: one queued jump per physical space PRESS (FUN_0072eb80 ->
  // AUCPlayerMoveEvent); holding space does NOT re-jump on landing, and presses while airborne
  // are eaten (no buffering). Set on a fresh keydown, consumed every game tick.
  bool _game_jump_pressed = false;
  bool _game_airborne_from_jump = false; // jumped (Jump anim) vs walked off an edge (Fall anim)
  // client swim state (RE'd 3.3.5a FUN_00730d10): enter at depth > 0.75*collisionHeight, exit
  // below (enter - 1/36); space near the surface converts into a REAL jump (the dolphin leap).
  bool _game_swimming = false;
  // Camera-vs-water STICKY SIDE (client behaviour: the water surface is a camera wall; the side
  // flips only when the character has stepped clearly into the other medium). 1 = camera held
  // above the surface, -1 = held below, 0 = no liquid context. Hysteresis kills the surface-bob
  // flapping. Enforced by waterBoomLimit() in both the tick's boom resolution and the pre-draw clamp.
  int _game_cam_water_side = 0;
  float waterBoomLimit(glm::vec3 const& head, glm::vec3 const& look, float dist) const;
  // GAME MODE (round 26): the Y push applied to the RENDERED boom eye so it never sits inside the
  // water-surface band. Computed in snapCameraOffWaterSurface, added to the eye in BOTH model_view()
  // and the render_eye that drives the underwater test -> view and effect always agree, character
  // untouched (no swim trap). 0 = no push.
  float _game_render_eye_dy = 0.0f;
  // The camera may NEVER dwell inside the water-surface band (user spec: snap to the very next
  // position inside or outside, so a grazing/edge-on surface view cannot exist and the underwater
  // effect side always matches). Applies to BOTH editor and game cameras each frame.
  void snapCameraOffWaterSurface();
  // Surface ripple cadence (client Water0Ripple port): next wake time + the swim edge for the
  // entry splash.
  float _game_next_wake_ms = 0.0f;
  // FOOTSTEPS (doc 38): $FSD anim-event crossing state -- the playing anim's event times are
  // cached on anim change; each tick fires the events crossed since the previous tick.
  int _game_fs_anim = -2;              // anim id the cache below belongs to (-2 = never)
  long long _game_fs_start_ms = 0;     // global anim ms when that anim started (cycle phase 0)
  int _game_fs_prev_t = 0;             // anim-local ms at the previous tick
  int _game_fs_len = 0;                // cycle length ms
  std::vector<int> _game_fs_events;    // sorted $FSD fire times (anim-local ms)
  float _game_splash_cd_ms = 0.0f;     // dive-splash cooldown (surface bob must not re-splash)
  // Last liquid surface height seen under the player. The swim-EXIT splash needs it because the
  // frame that ends swimming is often already PAST the liquid edge (jumping out at a shoreline),
  // where the live water query returns nothing -- see the exit branch in the game tick.
  std::optional<float> _game_last_water_y;
  // (_game_was_swimming_ripple removed 2026-08-26: the splash now fires on the swim-ENTER event)
  // Set at every jump LAUNCH: forces the character animation to restart even when the chosen anim
  // id is unchanged. Holding space over water dolphin-hops relaunches with JumpStart(37) already
  // active -- without a restart the model froze at JumpStart's end pose until key release.
  bool _game_anim_restart_pending = false;
  // surface micro-dip: rising through the exit depth hands to air physics for a few frames
  // before the depth check re-enters swim; the swim pose is held through it (no upright flash)
  bool _game_breaching = false;
  // smoothed swim body pitch (radians, nose-up +), the client's FUN_00719b80 [0x235] state:
  // chases the target at clamp01(dt*4pi), dt clamped [1/90, 1/20], snaps inside 0.01 rad
  float _game_swim_pitch = 0.0f;
  // swim body-yaw chase (client: a >90 deg facing flip in ONE tick swings the body round at
  // clamp01(dt*4pi) instead of teleporting); tracks the per-tick facing delta
  bool _game_swim_yaw_chasing = false;
  float _game_swim_prev_facing = 0.0f;
  // 3rd person: wheel-zoom boom length behind the head (0 = first person) and the actual length
  // after the client-style pull-in collision (camera never inside geometry).
  float _game_third_person_distance = 0.0f;
  float _game_camera_actual_distance = 0.0f;
  // latest collision-sweep result; the actual distance SNAPS IN to it but GLIDES back out at the
  // client's cameraDistanceSmoothSpeed (cvar default 8.33 yd/s) instead of teleporting per frame
  float _game_boom_limit = 0.0f;
  // CreatureDisplayInfo id the character renders as (49 = naked human male, the .morph classic);
  // editable in the Game Mode panel.
  int _game_character_display_id = 49;
  // character presentation timers: landing one-shot + time spent airborne (drives playing
  // JumpStart to its full authored length before the Jump loop takes over).
  // The landing anim is CHOSEN ONCE at touchdown from the input at that instant (client
  // FUN_0073d2b0) and plays out as a one-shot -- input changes during it must not re-pick it.
  int _game_land_anim = 0; // 39 JumpEnd / 187 JumpLandRun / 0 none
  float _game_land_anim_timer = 0.0f;
  float _game_air_time = 0.0f;
  // Highest feet height reached since the feet last left the ground -- the fall distance at
  // touchdown decides the ground-contact sound and the fall-damage vocal.
  float _game_fall_peak_y = 0.0f;
  // lower-body strafe twist, smoothed by the client's critically-damped spring (RE'd 3.3.5a
  // FUN_00719660: rate 20/s, k = 1/(1 + x + 0.48x^2 + 0.235x^3)); radians, + = left.
  float _game_twist = 0.0f;
  float _game_twist_vel = 0.0f;
  // the character's RENDER yaw (degrees) chasing the camera yaw with the same client spring, so
  // whipping the camera swings the body smoothly instead of snapping it.
  float _game_render_yaw = 0.0f;
  float _game_render_yaw_vel = 0.0f;
  // per-key down states for movement: releasing ONE of an opposing pair must hand control to the
  // still-held key instead of zeroing the axis (A->D quick flicks used to kill the input).
  bool _key_move_fwd = false;
  bool _key_move_back = false;
  bool _key_strafe_pos = false;
  bool _key_strafe_neg = false;
  bool _key_updown_pos = false;
  bool _key_updown_neg = false;
  // LMB orbit camera (client-style): offsets applied to the VIEW only -- the character keeps its
  // facing (which stays bound to _camera yaw/pitch, the RMB control).
  float _game_orbit_yaw = 0.0f;
  float _game_orbit_pitch = 0.0f;
  bool _game_orbiting = false;
  [[nodiscard]] glm::vec3 game_camera_look_direction() const;
  // [game mode] per-DRAW boom clamp: mouse orbit rotates the camera every FRAME but the collision
  // sweep runs in the game TICK -- a fast swing rendered 1-2 frames with an unswept direction
  // (visible clip-through, then "pushed out" by the next tick). Re-sweeps the CURRENT look
  // direction cheaply (5 rays) and clamps the boom DOWN before the frame's matrices are built.
  void clampGameBoomPreDraw();
  // last game-tick camera orientation / boom length: physics raycasts are skipped entirely while
  // nothing moved or turned (standing idle costs zero probes).
  float _game_prev_cam_yaw = 1.0e9f;
  float _game_prev_cam_pitch = 1.0e9f;
  float _game_prev_boom_dist = -1.0f;
  // movement speed in yd/s, driven by the game-mode speed panel (client presets: walk 2.5 / run 7 /
  // mounted 14); the panel shows only while Game View is active.
  float _game_mode_speed = 7.0f;
  QDockWidget* _game_mode_speed_dock = nullptr;
  Noggit::BoolToggleProperty _draw_lights_zones = { false };
  Noggit::BoolToggleProperty _draw_creature_spawns = { false };
  bool _creature_spawns_user_click = false;   // set by the View-menu action; only then is the toggle persisted
  Noggit::BoolToggleProperty _show_detail_info_window = { false };
  Noggit::BoolToggleProperty _show_minimap_window = { false };
  // [game mode] entry snap used by the Game View toolbar toggle: place the camera on the nearest
  // surface BELOW it -- terrain or WMO -- and reset the physics state. (The old toolbar hack used the
  // terrain-only query, so toggling while over a city dropped the camera through the rooftops.)
  void enterGameModeInPlace();
private:

  int _selected_area_id = -1;

  [[nodiscard]]
  math::ray intersect_ray() const;
  // force_objects overrides the "objects are only pickable in object/minimap mode" rule. Needed by the
  // creature/gameobject drag, which must land on top of a WMO roof or a doodad instead of falling
  // through to the terrain underneath it.
  selection_result intersect_result(bool terrain_only, bool force_objects = false);
  // Nearest solid surface under the mouse -- terrain, WMO or M2, whichever the ray hits first.
  // std::nullopt when the ray hits nothing (e.g. pointing at the sky).
  std::optional<glm::vec3> surface_pos_under_cursor();
  // [game mode] height of the nearest walkable surface (terrain OR WMO) under `feet`, probing from
  // probe_up above them so stairs/ledges up to that height still count as ground. Ignores the view
  // toggles -- physics always collides. std::nullopt over the void / unloaded space.
  std::optional<float> game_mode_ground_height(glm::vec3 const& feet, float probe_up, float max_dist = 0.0f);
  void doSelection(bool selectTerrainOnly, bool mouseMove = false);
  void update_cursor_pos();

  display_mode _display_mode;

  [[nodiscard]]
  glm::mat4x4 model_view() const;

  [[nodiscard]]
  glm::mat4x4 projection() const;

  void draw_map();

  void createGUI();

  QWidgetAction* createTextSeparator(const QString& text);

  float mTimespeed;

  void ResetSelectedObjectRotation();
  void snap_selected_models_to_the_ground();
  void DeleteSelectedObjects();
  void changeZoneIDValue (int set);

  QPointF _last_mouse_pos;
  float mh, mv, rh, rv;

  float keyx = 0, keyy = 0, keyz = 0, keyr = 0, keys = 0;

  bool MoveObj;
  float numpad_moveratio = 0.001f;

  glm::vec3 objMove;

  std::vector<selection_type> lastSelected;

  bool _rotation_editor_need_update = false;

  // Vars for the ground editing toggle mode store the status of some
  // view settings when the ground editing mode is switched on to
  // restore them if switch back again
  std::shared_ptr<Noggit::Project::NoggitProject> _project;
  bool  alloff = true;
  bool  alloff_models = false;
  bool  alloff_doodads = false;
  bool  alloff_contour = false;
  bool  alloff_wmo = false;
  bool  alloff_detailselect = false;
  bool  alloff_fog = false;
  bool  alloff_terrain = false;
  bool  alloff_climb = false;
  bool  alloff_vertex_color = false;
  bool  alloff_baked_shadows = false;

  editing_mode terrainMode = editing_mode::ground;
  editing_mode saveterrainMode = terrainMode;

  bool _uid_duplicate_warning_shown = false;
  bool _force_uid_check = false;
  bool _uid_fix_failed = false;
  void on_uid_fix_fail();

  uid_fix_mode _uid_fix;
  bool _from_bookmark;

  bool saving_minimap = false;

  Noggit::Ui::toolbar* _toolbar;
  Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar* _view_toolbar;
  Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar* _secondary_toolbar;
  Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar* _left_sec_toolbar;
  Noggit::Ui::TimeGlobeWidget* _time_globe = nullptr; // WC3 time-of-day globe, floated top-centre
  QWidget* _globe_balance_spacer = nullptr; // far-right spacer tracking leftSecondaryToolbarHolder width to keep the globe centred

  void save(save_mode mode);

  QSettings* _settings;
  Noggit::Ui::Tools::ViewportGizmo::ViewportGizmo _transform_gizmo;
  ImGuiContext* _imgui_context;

signals:
  void uid_fix_failed();
  void resized();
  void saved();
  void updateProgress(int value);
public slots:
  void on_exit_prompt();
  void ShowContextMenu(QPoint pos);

public:
  glm::vec4 cursor_color;

  MapView ( math::degrees ah0
          , math::degrees av0
          , glm::vec3 camera_pos
          , Noggit::Ui::Windows::NoggitWindow*
          , std::shared_ptr<Noggit::Project::NoggitProject> Project
          , std::unique_ptr<World>
          , uid_fix_mode uid_fix = uid_fix_mode::none
          , bool from_bookmark = false
          , bool capture_probe = false
          );
  ~MapView();

  void tick (float dt);
  void change_selected_wmo_nameset(int set);
  void change_selected_wmo_doodadset(int set);
  void saveMinimap(MinimapRenderSettings* settings);
  void initMinimapSave() { saving_minimap = true; };
  auto setBrushTexture(QImage const* img) -> void;
  Noggit::Camera* getCamera() { return &_camera; };
  void setCameraForCapture(glm::vec3 const& position, math::degrees yaw, math::degrees pitch);
  QImage grabRenderedFrameForCapture();
  // [VULKAN 2026-08-29] one headless frame for the self-run parity loop (no readback, no window update)
  void renderFrameForHarness();
  // [phase J] Grab the finished viewport to a PNG. Parity mode disables every VK ownership gate,
  // so the GATED path (the one users actually run) has no image test at all -- this gives it one.
  void saveHarnessScreenshot(std::string const& path);
  static void setVkParityForced(std::string const& cams_file); // force api=Vulkan + parity check + camera list
  static bool vkParityFinished();                               // harness logged its SUMMARY
  void muteAudioForHarness();                                   // zone music/ambience + sfx off (off-screen runs)
  void randomizeTerrainRotation();
  void randomizeTexturingRotation();
  void randomizeShaderRotation();
  void randomizeStampRotation();
  void onSettingsSave();
  void updateRotationEditor() { _rotation_editor_need_update = true; };
  void setCameraDirty() { _camera_moved_since_last_draw = true; };
  void requestRedraw() { _needs_redraw = true; };

  [[nodiscard]]
  Noggit::Ui::minimap_widget* getMinimapWidget() const { return _minimap;  }

  void set_editing_mode (editing_mode);
  editing_mode get_editing_mode() { return terrainMode; };

  [[nodiscard]]
  QWidget *getSecondaryToolBar();

  [[nodiscard]]
  QWidget *getLeftSecondaryToolbar();

  // Show/hide the "Quick Facts" creature-info window (creature secondary toolbar checkbox). The
  // window is a free-floating tool window the user can move around.
  void setCreatureInfoPanelVisible(bool visible);

  [[nodiscard]]
  QWidget* getActiveStampModeItem();

  [[nodiscard]]
  Noggit::Ui::flatten_blur_tool* getFlattenTool() { return flattenTool; };

  [[nodiscard]]
  Noggit::NoggitRenderContext getRenderContext() { return _context; };

  [[nodiscard]]
  World* getWorld() { return _world.get(); };

  // Calendar dropdown button (checkable game-events that toggle which seasonal event's spawns render).
  // Public so the time-globe popup can host it under its time slider.
  QToolButton* makeSeasonalEventsToolButton(QWidget* parent);
  QWidget* makeWeatherWidget(QWidget* parent); // weather (none/rain/snow + intensity) in the time-globe popup

  [[nodiscard]]
  QDockWidget* getAssetBrowser() {return _asset_browser_dock; };

  [[nodiscard]]
  Noggit::Ui::object_editor* getObjectEditor() { return objectEditor; };

  [[nodiscard]]
  QDockWidget* getObjectPalette() { return _object_palette_dock; };

  [[nodiscard]]
  QDockWidget* getTexturePalette() { return   _texture_palette_dock; };

private:
  enum Modifier
  {
    MOD_shift = 0x01,
    MOD_ctrl = 0x02,
    MOD_alt = 0x04,
    MOD_meta = 0x08,
    MOD_space = 0x10,
    MOD_num = 0x20,
    MOD_none = 0x00
  };
  struct HotKey
  {
    Qt::Key key;
    size_t modifiers;
    std::function<void()> function;
    std::function<bool()> condition;
    HotKey (Qt::Key k, size_t m, std::function<void()> f, std::function<bool()> c)
      : key (k), modifiers (m), function (f), condition (c) {}
  };

  std::forward_list<HotKey> hotkeys;

  void addHotkey(Qt::Key key, size_t modifiers, std::function<void()> function, std::function<bool()> condition = [] { return true; });

  QElapsedTimer _startup_time;
  qreal _last_update = 0.f;
  std::list<qreal> _last_frame_durations;

  float _last_fps_update = 0.f;

  QTimer _update_every_event_loop;

  QOpenGLContext* _last_opengl_context;

  virtual void tabletEvent(QTabletEvent* event) override;
  virtual void initializeGL() override;
  virtual void paintGL() override;
  virtual void resizeGL (int w, int h) override;
  virtual void mouseMoveEvent (QMouseEvent*) override;
  virtual void mousePressEvent (QMouseEvent*) override;
  virtual void mouseReleaseEvent (QMouseEvent*) override;
  virtual void wheelEvent (QWheelEvent*) override;
  virtual void keyReleaseEvent (QKeyEvent*) override;
  virtual void keyPressEvent (QKeyEvent*) override;
  virtual void focusOutEvent (QFocusEvent*) override;
  virtual void enterEvent(QEvent*) override;

  Noggit::Ui::Windows::NoggitWindow* _main_window;

  glm::vec4 normalized_device_coords (int x, int y) const;
  float aspect_ratio() const;

  Noggit::TabletManager* _tablet_manager;

  QLabel* _status_position;
  QLabel* _status_selection;
  QLabel* _status_area;
  QLabel* _status_time;
  QLabel* _status_fps;
  QLabel* _status_culling;
  QLabel* _status_database;

  Noggit::BoolToggleProperty _locked_cursor_mode = {false};
  Noggit::BoolToggleProperty _move_model_to_cursor_position = {true};
  Noggit::BoolToggleProperty _move_model_snap_to_objects = { true };
  Noggit::BoolToggleProperty _snap_multi_selection_to_ground = {false};
  Noggit::BoolToggleProperty _rotate_along_ground = {true };
  Noggit::BoolToggleProperty _rotate_doodads_along_doodads = { false };
  Noggit::BoolToggleProperty _rotate_doodads_along_wmos = { false };
  Noggit::BoolToggleProperty _rotate_along_ground_smooth = {true };
  Noggit::BoolToggleProperty _rotate_along_ground_random = {false };
  Noggit::BoolToggleProperty _use_median_pivot_point = {true};
  Noggit::BoolToggleProperty _display_all_water_layers = {true};
  Noggit::unsigned_int_property _displayed_water_layer = {0};
  Noggit::object_paste_params _object_paste_params;

  Noggit::BoolToggleProperty _show_node_editor = {false};
  Noggit::BoolToggleProperty _show_minimap_borders = {true};
  Noggit::BoolToggleProperty _show_minimap_skies = {false};
  Noggit::BoolToggleProperty _show_keybindings_window = {false};
  Noggit::BoolToggleProperty _show_texture_palette_window = {false};
  Noggit::BoolToggleProperty _show_texture_palette_small_window = {false};
  Noggit::BoolToggleProperty _show_creature_browser = {false};
  Noggit::BoolToggleProperty _show_gameobject_browser = {false};
  Noggit::BoolToggleProperty _showStampPalette{false};

  Noggit::Ui::minimap_widget* _minimap;
  QDockWidget* _minimap_dock;
  QDockWidget* _texture_palette_dock;
  QDockWidget* _object_palette_dock;

  void move_camera_with_auto_height (glm::vec3 const&);
  // Frame a spawn: place the camera back-and-up at ~45 degrees and aim it AT the target (used when
  // picking a creature/gameobject from the browser list), instead of dropping straight overhead.
  void focus_camera_on_target (glm::vec3 const&);

  void setToolPropertyWidgetVisibility(editing_mode mode);

  void unloadOpenglData() override;

  Noggit::Ui::help* _keybindings;
  Noggit::Ui::tileset_chooser* TexturePalette;
  Noggit::Ui::detail_infos* guidetailInfos;
  Noggit::Ui::zone_id_browser* ZoneIDBrowser;
  Noggit::Ui::texture_palette_small* _texture_palette_small;
  Noggit::Ui::ObjectPalette* _object_palette;
  Noggit::Ui::texture_picker* TexturePicker;
  Noggit::Ui::water* guiWater;
  Noggit::Ui::object_editor* objectEditor;
  Noggit::Ui::flatten_blur_tool* flattenTool;
  Noggit::Ui::TerrainTool* terrainTool;
  Noggit::Ui::ShaderTool* shaderTool;
  Noggit::Ui::texturing_tool* texturingTool;
  Noggit::Ui::hole_tool* holeTool;
  Noggit::Ui::MinimapCreator* minimapTool;
  Noggit::Ui::Tools::BrushStack* stampTool;
  Noggit::Ui::Tools::LightEditor* lightEditor;
  Noggit::Ui::Tools::ChunkManipulator::ChunkManipulatorPanel* _chunk_manipulator;
  Noggit::Scripting::scripting_tool* scriptingTool;

  OpenGL::texture* const _texBrush;

  Noggit::Ui::Tools::AssetBrowser::Ui::AssetBrowserWidget* _asset_browser;

  QDockWidget* _asset_browser_dock;
  QDockWidget* _node_editor_dock;
  QWidget* _creature_actions_overlay = nullptr;
  QDockWidget* _creature_editor_dock = nullptr;
  QWidget* _creature_editor_panel = nullptr;
  QWidget* _creature_pending_popup = nullptr;
  QListWidget* _creature_pending_list = nullptr;
  QDockWidget* _creature_browser_dock;
  QDockWidget* _creature_model_picker_dock = nullptr;
  QDockWidget* _texture_browser_dock;
  QDockWidget* _texture_picker_dock;
  QDockWidget* _detail_infos_dock;

  QLineEdit* _creature_search_field = nullptr;
  QCheckBox* _creature_zone_filter = nullptr;
  QCheckBox* _gameobject_zone_filter = nullptr;
  QListWidget* _creature_list_widget = nullptr;
  QLabel* _creature_browser_status = nullptr;
  // Type/rank filter for the current-map creature browser (same controls as the model picker). Looked
  // up per spawn by entry from _creature_template_filter_info (filled when the model picker loads
  // creature_template).
  QComboBox* _creature_browser_type_filter = nullptr;
  QCheckBox* _creature_browser_elite = nullptr;
  QCheckBox* _creature_browser_boss = nullptr;
  QCheckBox* _creature_browser_civilian = nullptr;
  QCheckBox* _creature_browser_trainer = nullptr;
  struct CreatureFilterInfo
  {
    std::uint32_t creature_type = 0;
    std::uint32_t rank = 0;
    std::uint32_t type_flags = 0;
    std::uint32_t flags_extra = 0;
    std::uint32_t npc_flags = 0;
  };
  std::unordered_map<std::uint32_t, CreatureFilterInfo> _creature_template_filter_info;
  // guid -> resolved zone id cache for the "Zone only" browser filter (only valid ids cached, so
  // spawns in not-yet-loaded tiles get retried). Cleared on spawn reload.
  std::unordered_map<std::uint32_t, unsigned int> _creature_zone_cache;
  std::unordered_map<std::uint32_t, unsigned int> _gameobject_zone_cache;
  QTreeWidget* _creature_model_tree = nullptr;
  QLabel* _creature_model_picker_status = nullptr;

  QLabel* _creature_editor_info = nullptr;
  // "Quick Facts" dropdown (creature_template stats + spell/aura icons) toggled from the creature
  // secondary toolbar; refreshed on spawn selection change.
  Noggit::Ui::CreatureInfoPanel* _creature_info_panel = nullptr;
  QDoubleSpinBox* _spawn_edit_x = nullptr;
  QDoubleSpinBox* _spawn_edit_y = nullptr;
  QDoubleSpinBox* _spawn_edit_z = nullptr;
  QDoubleSpinBox* _spawn_edit_orientation = nullptr;

  // "Edit/New Creature" form fields -- populated from the selected spawn so its guid/entry/display show.
  QGroupBox* _creature_spawn_box = nullptr;
  QLineEdit* _creature_spawn_guid_field = nullptr;
  QLineEdit* _creature_spawn_entry_field = nullptr;
  QLineEdit* _creature_spawn_display_field = nullptr;
  // Extended `creature` columns (tortoise-wow schema; fields disabled when the connected DB lacks
  // the column). Semantics RE'd from the live server source -- see World::CreatureSpawnOverlay::ExtFields.
  QLineEdit* _creature_spawn_id2_field = nullptr;
  QLineEdit* _creature_spawn_id3_field = nullptr;
  QLineEdit* _creature_spawn_id4_field = nullptr;
  QSpinBox* _creature_spawn_respawn_min = nullptr;
  QSpinBox* _creature_spawn_respawn_max = nullptr;
  QDoubleSpinBox* _creature_spawn_wander = nullptr;
  QSpinBox* _creature_spawn_health_pct = nullptr;
  QSpinBox* _creature_spawn_mana_pct = nullptr;
  QComboBox* _creature_spawn_movement = nullptr;
  QPushButton* _creature_spawn_flags_button = nullptr;
  QMenu* _creature_spawn_flags_menu = nullptr;
  std::uint32_t _creature_spawn_flags_value = 0;
  QDoubleSpinBox* _creature_spawn_visibility = nullptr;
  // Row show/hide plumbing: fields whose column the connected DB lacks are HIDDEN entirely (not
  // greyed). Compound rows live in container widgets so the row hides as one unit; the form
  // layouts are kept to resolve each row's label via labelForField.
  QFormLayout* _creature_spawn_form_left = nullptr;
  QFormLayout* _creature_spawn_form_ext = nullptr;
  QWidget* _creature_spawn_alt_row = nullptr;
  QWidget* _creature_spawn_respawn_row = nullptr;
  QWidget* _creature_spawn_pct_row = nullptr;
  // cmangos-only columns (hidden on Turtle-schema DBs): spawnMask difficulty bitmask + phaseMask.
  QPushButton* _creature_spawn_spawnmask_button = nullptr;
  QMenu* _creature_spawn_spawnmask_menu = nullptr;
  std::uint32_t _creature_spawn_spawnmask_value = 1;
  QSpinBox* _creature_spawn_phasemask = nullptr;
  bool _creature_spawn_form_updating = false; // guards the change handlers during programmatic fills
  // Widgets -> spawn.ext for the given guid (recomputes dirty); spawn.ext -> widgets (guid 0 =
  // defaults for a New spawn); flags-button text from _creature_spawn_flags_value; wander-radius
  // ring state (World::wander_viz) from the current selection + field focus.
  void applyCreatureExtFormToSpawn(std::uint32_t guid);
  void populateCreatureExtForm(std::uint32_t guid);
  void updateCreatureSpawnFlagsButton();
  void updateWanderVisualization();
  bool _creature_wander_field_focused = false;
  QGroupBox* _gameobject_spawn_box = nullptr;
  QLineEdit* _gameobject_spawn_guid_field = nullptr;
  QLineEdit* _gameobject_spawn_entry_field = nullptr;
  QLineEdit* _gameobject_spawn_display_field = nullptr;

  std::optional<std::uint32_t> _selected_creature_spawn_guid;
  std::optional<std::uint32_t> _hovered_creature_spawn_guid;
  bool _dragging_creature_spawn = false;
  std::optional<glm::vec3> _creature_drag_anchor_pos;
  // Per-spawn state captured at drag START: position for the group-drag layout math, position +
  // orientation together as the before-state for the drag's Ctrl+Z undo op (wheel-rotate during a
  // drag changes orientation too, so it must be part of the snapshot).
  struct SpawnDragState
  {
    std::uint32_t guid = 0;
    glm::vec3 pos = glm::vec3(0.0f);
    float orientation = 0.0f;
  };
  std::vector<SpawnDragState> _creature_drag_initial_positions;

  // GameObject tool docks/widgets (mirror the creature ones above; gameobjects have no model picker,
  // so the coordinate editor panel lives inside the browser dock).
  QWidget* _gameobject_actions_overlay = nullptr;
  QWidget* _gameobject_editor_panel = nullptr;
  QWidget* _gameobject_pending_popup = nullptr;
  QListWidget* _gameobject_pending_list = nullptr;
  QDockWidget* _gameobject_browser_dock = nullptr;
  QDockWidget* _gameobject_model_picker_dock = nullptr;
  QTreeWidget* _gameobject_model_tree = nullptr;
  QLabel* _gameobject_model_picker_status = nullptr;

  // Seasonal Events calendar dropdown lives in the toolbar strip right of the time globe (see makeSeasonalEventsToolButton).

  QLineEdit* _gameobject_search_field = nullptr;
  QListWidget* _gameobject_list_widget = nullptr;
  QLabel* _gameobject_browser_status = nullptr;
  // Type filter for the current-map gameobject browser (gameobjects only have a type, no rank/elite).
  // Looked up per spawn entry from _gameobject_template_filter_type (filled by the model picker).
  QComboBox* _gameobject_browser_type_filter = nullptr;
  std::unordered_map<std::uint32_t, std::uint32_t> _gameobject_template_filter_type;

  QLabel* _gameobject_editor_info = nullptr;
  QDoubleSpinBox* _go_spawn_edit_x = nullptr;
  QDoubleSpinBox* _go_spawn_edit_y = nullptr;
  QDoubleSpinBox* _go_spawn_edit_z = nullptr;
  QDoubleSpinBox* _go_spawn_edit_orientation = nullptr;

  std::optional<std::uint32_t> _selected_gameobject_spawn_guid;
  std::optional<std::uint32_t> _hovered_gameobject_spawn_guid;
  bool _dragging_gameobject_spawn = false;
  std::optional<glm::vec3> _gameobject_drag_anchor_pos;
  std::vector<SpawnDragState> _gameobject_drag_initial_positions;

  Noggit::Ui::Tools::ToolPanel* _tool_panel_dock;

  ::Ui::MapViewOverlay* _viewport_overlay_ui;
  ImGuizmo::MODE _gizmo_mode = ImGuizmo::MODE::WORLD;
  ImGuizmo::OPERATION _gizmo_operation = ImGuizmo::OPERATION::TRANSLATE;
  Noggit::BoolToggleProperty _gizmo_on = {true};

  bool _change_operation_mode = false;
  void updateGizmoOverlay(ImGuizmo::OPERATION operation);

  bool _gl_initialized = false;
  bool _destroying = false;
  bool _needs_redraw = false;
  bool _capture_probe = false;

  // [VULKAN NATIVE PRESENT, 2026-09-03] native child window the VK swapchain presents into.
  // Created on demand via a QUEUED call (never inside paintGL); input-transparent (WM_NCHITTEST ->
  // HTTRANSPARENT + WA_TransparentForMouseEvents on the container) so every event still lands on
  // this widget. GL mode never creates it -- gate, never delete.
  QWindow* _vk_present_window = nullptr;
  QWidget* _vk_present_container = nullptr;
  bool _vk_present_requested = false;
  void ensureVkPresentSurface();
  // [2026-09-04 NATIVE UI COMPOSITE] The overlay image handed to Vulkan each time it changes.
  // Grabbed on a TIMER, never inside paintGL: QWidget::render() re-enters Qt's painting machinery,
  // and one of the overlay widgets calls QMainWindow::statusBar(), which lazily CONSTRUCTS a
  // QStatusBar -> QWidget::setParent -> reparentFocusWidgets -> throw, inside the paint. That
  // crashed the app within 60 frames. Grab outside the paint, upload inside it.
  QImage _vk_ui_image;
  bool _vk_ui_image_dirty = false;
  QTimer _vk_ui_timer;
  void grabVkUiOverlay();
  // Promote viewport overlay widgets above the native present surface (see the .cpp note).
  void raiseViewportOverlayWidgets();

  bool _mod_z_down = false;
  bool _mod_x_down = false;

  bool event(QEvent* e) override;
  bool eventFilter(QObject* obj, QEvent* e) override;

  unsigned _mmap_async_index = 0;
  unsigned _mmap_render_index = 0;
  std::optional<QImage> _mmap_combined_image;

  OpenGL::Scoped::deferred_upload_buffers<2> _buffers;

  QRubberBand* _area_selection;

public:

private:

  void setupViewportOverlay();
  void setupRaiseLowerUi();
  void setupFlattenBlurUi();
  void setupTexturePainterUi();
  void setupHoleCutterUi();
  void setupAreaDesignatorUi();
  void setupFlagUi();
  void setupWaterEditorUi();
  void setupVertexPainterUi();
  void setupObjectEditorUi();
  void setupCreatureEditorUi();
  void setupCreatureBrowserUi();
  void setupCreatureActionsUi();
  void setupCreatureModelPickerUi();
  void setupGameObjectEditorUi();
  void setupGameObjectBrowserUi();
  void setupGameObjectModelPickerUi();
  void setupGameObjectActionsUi();
  void populateSeasonalEventsMenu(QMenu* menu);
  void setupMinimapEditorUi();
  void setupStampUi();
  void setupLightEditorUi();
  void setupScriptingUi();
  void setupChunkManipulatorUi();
  void setupNodeEditor();
  void setupAssetBrowser();
  void setupDetailInfos();
  void updateDetailInfos(bool no_sel_change_check = false);
  void setupToolbars();
  void setupKeybindingsGui();
  void setupMinimap();
  void setupFileMenu();
  void setupEditMenu();
  void setupAssistMenu();
  void setupViewMenu();
  void setupHelpMenu();
  void setupHotkeys();
  void refreshCreatureSpawnOverlay(bool force_reload = false);
  void updateDatabaseStatus();
  void rebuildCreatureBrowserList(bool preserve_selection = true);
  // Move the highlighted row without rebuilding the list -- a selection change touches no content.
  void highlightCreatureBrowserSelection();
  // Label for one creature row, shared by the full rebuild and the targeted refresh.
  static QString creature_spawn_item_text(World::CreatureSpawnOverlay const& spawn);
  // Retext only the listed spawns' rows (cheap) instead of repopulating the whole list.
  void refreshCreatureBrowserItems(std::vector<std::uint32_t> const& guids);
  void updateCreatureBrowserStatus(QString const& override_text = QString());
  std::size_t selectedCreatureSpawnCount() const;
  void setSelectedCreatureSpawn(std::optional<std::uint32_t> guid, bool update_browser = true);
  void addCreatureSpawnToSelection(std::uint32_t guid, bool update_browser = true);
  void selectCreatureSpawnsInArea(QRect const& rect, bool add_to_selection = true);
  void refreshCreatureEditorKnobs();
  void setHoveredCreatureSpawn(std::optional<std::uint32_t> guid);
  std::optional<std::uint32_t> findCreatureSpawnAtCursor(); // mesh-first pick (non-const: intersect)
  void updateCreatureSpawnHover(QPoint const& global_pos);
  bool tryStartCreatureSpawnDrag();
  void translateSelectedCreatureSpawns(glm::vec3 const& delta);
  // Wheel-rotate every spawn currently being dragged, about its own centre (degrees).
  void rotateDraggedSpawns(float degrees);
  void updateSelectedCreatureSpawnPosition(glm::vec3 const& pos);
  void showSelectedCreatureSpawnMenu(QPoint const& global_pos);
  void discardPendingCreatureSpawns();
  // Generate the pending-changes SQL. rebase_state=true also commits the editor state (originals =
  // current, pending_create cleared) -- exactly what a successful export/apply implies.
  QString buildDirtyCreatureSpawnSql(bool rebase_state);
  QString buildDirtyGameObjectSpawnSql(bool rebase_state);
  void saveDirtyCreatureSpawns();
  void jumpToCreatureListItem(QListWidgetItem* item);
  // Delete (Del) the selected creature spawn(s): marks them pending_delete (DELETE on SQL export,
  // hidden from view/browser) and records them so Ctrl+Z restores the most recent batch.
  void deleteSelectedCreatureSpawns();

  // Dedicated creature/gameobject edit undo (Ctrl+Z in those modes) -- fully independent from the
  // terrain/object ActionManager: while a spawn tool is active Ctrl+Z ONLY walks this stack and
  // never touches terrain/water/object history. Moves store the absolute before-state; Delete
  // stores the marked guids (undo un-marks); Create stores the added guids (undo removes a still-
  // pending spawn, or re-marks an already-exported one as pending_delete).
  struct SpawnUndoOp
  {
    enum class Kind { Move, Delete, Create };
    struct MoveState
    {
      std::uint32_t guid = 0;
      glm::vec3 pos = glm::vec3(0.0f);
      float orientation = 0.0f;
    };
    Kind kind = Kind::Move;
    std::vector<MoveState> moves;     // Kind::Move: state to RESTORE
    std::vector<std::uint32_t> guids; // Kind::Delete / Kind::Create
    qint64 timestamp_ms = 0;          // for spinbox coalescing
    bool from_spinbox = false;
  };
  void pushCreatureUndoOp(SpawnUndoOp op);
  void pushGameObjectUndoOp(SpawnUndoOp op);

  // Coalesced browser-list rebuilds: the full rebuild allocates a QListWidgetItem per world spawn
  // (0.1-1.4s on big maps), so spawn-mutation paths (delete/undo/create) must never run it inline.
  // Schedule instead: one deferred rebuild per burst, a beat after the action.
  void scheduleCreatureBrowserRebuild();
  void scheduleGameObjectBrowserRebuild();
  bool _creature_browser_rebuild_pending = false;
  bool _gameobject_browser_rebuild_pending = false;
  bool undoCreatureEdit();
  bool undoGameObjectEdit();
  std::vector<SpawnUndoOp> _creature_undo_ops;
  std::vector<SpawnUndoOp> _gameobject_undo_ops;

  // GameObject tool methods (mirror the creature ones; no model picker / new-spawn creation).
  void rebuildGameObjectBrowserList(bool preserve_selection = true);
  void highlightGameObjectBrowserSelection();
  // Retext only the listed gameobject spawns' rows instead of repopulating the whole list.
  void refreshGameObjectBrowserItems(std::vector<std::uint32_t> const& guids);
  void updateGameObjectBrowserStatus(QString const& override_text = QString());
  std::size_t selectedGameObjectSpawnCount() const;
  void setSelectedGameObjectSpawn(std::optional<std::uint32_t> guid, bool update_browser = true);
  void addGameObjectSpawnToSelection(std::uint32_t guid, bool update_browser = true);
  void selectGameObjectSpawnsInArea(QRect const& rect, bool add_to_selection = true);
  void refreshGameObjectEditorKnobs();
  void setHoveredGameObjectSpawn(std::optional<std::uint32_t> guid);
  std::optional<std::uint32_t> findGameObjectSpawnAtCursor(); // mesh-first pick (non-const: intersect)
  void updateGameObjectSpawnHover(QPoint const& global_pos);
  bool tryStartGameObjectSpawnDrag();
  void translateSelectedGameObjectSpawns(glm::vec3 const& delta);
  void updateSelectedGameObjectSpawnPosition(glm::vec3 const& pos);
  void showSelectedGameObjectSpawnMenu(QPoint const& global_pos);
  void discardPendingGameObjectSpawns();
  void saveDirtyGameObjectSpawns();
  void jumpToGameObjectListItem(QListWidgetItem* item);
  void deleteSelectedGameObjectSpawns();

  // SQL apply / reset tooling (see setupAssistMenu): apply pending spawn changes or a .sql file to
  // the project's connected database, or rebuild the database from folders of base .sql dumps.
#ifdef USE_MYSQL_UID_STORAGE
  void applyDirtyCreatureSpawnsToDb();
  void applyDirtyGameObjectSpawnsToDb();
  void applySqlFileToDb();
  void resetDatabaseFromSqlFolders();
#endif

  QWidget* _overlay_widget;
};
