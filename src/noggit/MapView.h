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
class QLineEdit;
class QListWidget;
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
  Noggit::BoolToggleProperty _draw_wmo_doodads = {true};
  Noggit::BoolToggleProperty _draw_wmo_exterior = { true };
  Noggit::BoolToggleProperty _draw_models = {true};
  Noggit::BoolToggleProperty _draw_model_animations = {true};
  Noggit::BoolToggleProperty _draw_hole_lines = {false};
  Noggit::BoolToggleProperty _draw_models_with_box = {false};
  Noggit::BoolToggleProperty _draw_fog = {false};
  Noggit::BoolToggleProperty _draw_hidden_models = {false};
  Noggit::BoolToggleProperty _draw_occlusion_boxes = {false};
  Noggit::BoolToggleProperty _game_mode_camera = { false };
  Noggit::BoolToggleProperty _draw_lights_zones = { false };
  Noggit::BoolToggleProperty _draw_creature_spawns = { false };
  Noggit::BoolToggleProperty _show_detail_info_window = { false };
  Noggit::BoolToggleProperty _show_minimap_window = { false };
private:

  int _selected_area_id = -1;

  [[nodiscard]]
  math::ray intersect_ray() const;
  selection_result intersect_result(bool terrain_only);
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
  void randomizeTerrainRotation();
  void randomizeTexturingRotation();
  void randomizeShaderRotation();
  void randomizeStampRotation();
  void onSettingsSave();
  void updateRotationEditor() { _rotation_editor_need_update = true; };
  void setCameraDirty() { _camera_moved_since_last_draw = true; };

  [[nodiscard]]
  Noggit::Ui::minimap_widget* getMinimapWidget() const { return _minimap;  }

  void set_editing_mode (editing_mode);
  editing_mode get_editing_mode() { return terrainMode; };

  [[nodiscard]]
  QWidget *getSecondaryToolBar();

  [[nodiscard]]
  QWidget *getLeftSecondaryToolbar();

  [[nodiscard]]
  QWidget* getActiveStampModeItem();

  [[nodiscard]]
  Noggit::Ui::flatten_blur_tool* getFlattenTool() { return flattenTool; };

  [[nodiscard]]
  Noggit::NoggitRenderContext getRenderContext() { return _context; };

  [[nodiscard]]
  World* getWorld() { return _world.get(); };

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
  Noggit::BoolToggleProperty _showStampPalette{false};

  Noggit::Ui::minimap_widget* _minimap;
  QDockWidget* _minimap_dock;
  QDockWidget* _texture_palette_dock;
  QDockWidget* _object_palette_dock;

  void move_camera_with_auto_height (glm::vec3 const&);

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
  QDockWidget* _creature_browser_dock;
  QDockWidget* _creature_model_picker_dock = nullptr;
  QDockWidget* _texture_browser_dock;
  QDockWidget* _texture_picker_dock;
  QDockWidget* _detail_infos_dock;

  QLineEdit* _creature_search_field = nullptr;
  QListWidget* _creature_list_widget = nullptr;
  QLabel* _creature_browser_status = nullptr;
  QTreeWidget* _creature_model_tree = nullptr;
  QLabel* _creature_model_picker_status = nullptr;

  QLabel* _creature_editor_info = nullptr;
  QDoubleSpinBox* _spawn_edit_x = nullptr;
  QDoubleSpinBox* _spawn_edit_y = nullptr;
  QDoubleSpinBox* _spawn_edit_z = nullptr;
  QDoubleSpinBox* _spawn_edit_orientation = nullptr;

  std::optional<std::uint32_t> _selected_creature_spawn_guid;
  std::optional<std::uint32_t> _hovered_creature_spawn_guid;
  bool _dragging_creature_spawn = false;
  std::optional<glm::vec3> _creature_drag_anchor_pos;
  std::vector<std::pair<std::uint32_t, glm::vec3>> _creature_drag_initial_positions;

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

  bool _mod_z_down = false;
  bool _mod_x_down = false;

  bool event(QEvent* e) override;

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
  void updateCreatureBrowserStatus(QString const& override_text = QString());
  std::size_t selectedCreatureSpawnCount() const;
  void setSelectedCreatureSpawn(std::optional<std::uint32_t> guid, bool update_browser = true);
  void addCreatureSpawnToSelection(std::uint32_t guid, bool update_browser = true);
  void selectCreatureSpawnsInArea(QRect const& rect, bool add_to_selection = true);
  void refreshCreatureEditorKnobs();
  void setHoveredCreatureSpawn(std::optional<std::uint32_t> guid);
  std::optional<std::uint32_t> findCreatureSpawnAtCursor() const;
  void updateCreatureSpawnHover(QPoint const& global_pos);
  bool tryStartCreatureSpawnDrag();
  void translateSelectedCreatureSpawns(glm::vec3 const& delta);
  void updateSelectedCreatureSpawnPosition(glm::vec3 const& pos);
  void showSelectedCreatureSpawnMenu(QPoint const& global_pos);
  void saveDirtyCreatureSpawns();
  void jumpToCreatureListItem(QListWidgetItem* item);

  QWidget* _overlay_widget;
};
