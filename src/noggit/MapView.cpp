// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/DBC.h>
#include <noggit/MapChunk.h>
#include <noggit/MapView.h>
#include <noggit/Misc.h>
#include <noggit/ModelManager.h> // ModelManager
#include <noggit/TextureManager.h> // TextureManager, Texture
#include <noggit/WMO.h> // WMOManager (mem-diag)
#include <noggit/WMOInstance.h> // WMOInstance
#ifdef _WIN32
#include <windows.h>
#include <psapi.h> // GetProcessMemoryInfo (mem-diag)
#endif
#include <noggit/World.h>
#include <noggit/map_index.hpp>
#include <noggit/uid_storage.hpp>
#include <noggit/ui/CurrentTexture.h>
#include <noggit/ui/DetailInfos.h> // detailInfos
#include <noggit/ui/FlattenTool.hpp>
#include <noggit/ui/Help.h>
#include <noggit/ui/HelperModels.h>
#include <noggit/ui/ModelImport.h>
#include <noggit/ui/ObjectEditor.h>
#include <noggit/ui/RotationEditor.h>
#include <noggit/ui/TexturePicker.h>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/ui/ZoneMusicPlayer.hpp>
#include <noggit/ui/CreatureInfoPanel.hpp>
#include <noggit/ui/Toolbar.h> // Noggit::Ui::toolbar
#include <noggit/ui/Water.h>
#include <noggit/ui/ZoneIDBrowser.h>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/ui/minimap_widget.hpp>
#include <noggit/ui/ShaderTool.hpp>
#include <noggit/ui/TerrainTool.hpp>
#include <noggit/ui/texture_swapper.hpp>
#include <noggit/ui/texturing_tool.hpp>
#include <noggit/ui/hole_tool.hpp>
#include <noggit/ui/texture_palette_small.hpp>
#include <noggit/ui/MinimapCreator.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/frame_profiler.hpp>
#include <opengl/scoped.hpp>
#include <noggit/ui/tools/ViewToolbar/Ui/ViewToolbar.hpp>
#include <noggit/ui/tools/TimeGlobe/TimeGlobeWidget.hpp>
#include <QtWidgets/QAction>
#include <QtWidgets/QToolBar>
#include <noggit/ui/tools/AssetBrowser/Ui/AssetBrowser.hpp>
#include <noggit/ui/tools/AssetBrowser/ModelView.hpp>
#include <noggit/ui/tools/PresetEditor/Ui/PresetEditor.hpp>
#include <noggit/ui/tools/NodeEditor/Ui/NodeEditor.hpp>
#include <noggit/ui/tools/UiCommon/ImageBrowser.hpp>
#include <noggit/ui/tools/BrushStack/BrushStack.hpp>
#include <noggit/ui/tools/LightEditor/LightEditor.hpp>
#include <noggit/ui/tools/ChunkManipulator/ChunkManipulatorPanel.hpp>
#include <external/imguipiemenu/PieMenu.hpp>
#include <external/tracy/Tracy.hpp>
#include <noggit/ui/object_palette.hpp>
#include <external/glm/gtc/type_ptr.hpp>
#include <opengl/types.hpp>
#include <limits>
#include <variant>
#include <noggit/Selection.h>
#include <math/ray.hpp>

#ifdef USE_MYSQL_UID_STORAGE
#include <mysql/mysql.h>

#include <QtCore/QSettings>
#include <noggit/MySqlSettings.hpp>
#endif

#include <noggit/scripting/scripting_tool.hpp>
#include <noggit/scripting/script_settings.hpp>

#include <noggit/ActionManager.hpp>
#include <noggit/Action.hpp>

#include <noggit/ui/FontNoggit.hpp>

#include "revision.h"

#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QColor>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QToolButton>
#include <QtGui/QIconEngine>
#include <QtGui/QPainter>
#include <QtGui/QPalette>
#include <QtGui/QPixmap>
#include <QtGui/QIcon>
#include <QWidgetAction>
#include <QSurfaceFormat>
#include <QMessageBox>
#include <QAbstractScrollArea>
#include <QScrollBar>
#include <QDateTime>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QProgressDialog>
#include <QClipboard>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <memory>

#include <vector>
#include <random>

namespace
{
  bool capture_debug_enabled()
  {
    if (char const* value = std::getenv("NOGGIT_CAPTURE_DEBUG"))
    {
      return std::string(value) != "0";
    }

    return false;
  }

  bool creature_capture_overlay_enabled()
  {
    if (char const* value = std::getenv("NOGGIT_CAPTURE_CREATURES"))
    {
      return std::string(value) != "0";
    }

    return true;
  }

  glm::vec3 server_to_client_creature_position(float server_x, float server_y, float server_z, bool global_wmo_map)
  {
    if (global_wmo_map)
    {
      return {-server_y, server_z, -server_x};
    }

    return {ZEROPOINT - server_y, server_z, ZEROPOINT - server_x};
  }

  glm::vec3 client_to_server_creature_position(glm::vec3 const& client_pos, bool global_wmo_map)
  {
    if (global_wmo_map)
    {
      return {-client_pos.z, -client_pos.x, client_pos.y};
    }

    return {ZEROPOINT - client_pos.z, ZEROPOINT - client_pos.x, client_pos.y};
  }

  float client_to_server_creature_orientation(float client_orientation)
  {
    auto orientation = glm::radians(client_orientation + 180.0f);
    auto full_rotation = glm::two_pi<float>();
    orientation = std::fmod(orientation, full_rotation);
    if (orientation < 0.0f)
    {
      orientation += full_rotation;
    }

    return orientation;
  }

  QString creature_type_label(std::uint32_t creature_type)
  {
    switch (creature_type)
    {
      case 1:  return "Beast";
      case 2:  return "Dragonkin";
      case 3:  return "Demon";
      case 4:  return "Elemental";
      case 5:  return "Giant";
      case 6:  return "Undead";
      case 7:  return "Humanoid";
      case 8:  return "Critter";
      case 9:  return "Mechanical";
      case 10: return "Not specified";
      case 11: return "Totem";
      case 12: return "Non-combat Pet";
      case 13: return "Gas Cloud";
      default: return QString("Type %1").arg(creature_type);
    }
  }

  class CreaturePreviewModelViewer final : public Noggit::Ui::Tools::AssetBrowser::ModelViewer
  {
  public:
    explicit CreaturePreviewModelViewer(QWidget* parent = nullptr)
      : Noggit::Ui::Tools::AssetBrowser::ModelViewer(parent, Noggit::NoggitRenderContext::ASSET_BROWSER)
    {
    }

    std::function<void()> on_double_click;

    void setCreatureSpawnPreview(World& world, World::CreatureSpawnOverlay const& spawn)
    {
      setModel(spawn.model_path);
      if (_model_instances.empty())
      {
        return;
      }

      auto& instance = _model_instances.back();
      instance.scale = std::clamp(spawn.template_scale * spawn.model_scale,
                                  ModelInstance::min_scale(),
                                  ModelInstance::max_scale());
      instance.dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
      instance.updateTransformMatrix();
      world.applyCreatureSpawnModelAppearance(spawn, instance, _context);
      instance.recalcExtents();
      resetCamera();
    }

  protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
      if (on_double_click)
      {
        on_double_click();
      }
      event->accept();
    }
  };

  // A QMenu that stays open when a CHECKABLE item is clicked, so the Seasonal Events dropdown can toggle
  // several events without reopening each time. Non-checkable items (All / None) and outside clicks close
  // it as usual.
  class MultiToggleMenu final : public QMenu
  {
  public:
    using QMenu::QMenu;

  protected:
    void mouseReleaseEvent(QMouseEvent* event) override
    {
      QAction* const action = activeAction();
      if (action && action->isEnabled() && action->isCheckable())
      {
        action->trigger(); // toggle in place; keep the menu open
        return;
      }
      QMenu::mouseReleaseEvent(event);
    }
  };

  // A clean calendar QIcon rendered ONCE at high resolution (so Qt smooth-scales it down without the
  // aliasing/artifacts a per-size QIconEngine produced at 16px), tinted to the SAME palette color as the
  // noggit font-glyph toolbar icons (see FontNoggitIconEngine).
  QIcon make_calendar_icon()
  {
    Noggit::Ui::FontNoggitButtonStyle style;
    style.ensurePolished();
    QColor const color = style.palette().color(QPalette::WindowText);

    int const S = 128;
    QPixmap pm(S, S);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    QPen pen(color);
    pen.setWidthF(S * 0.055);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    // Calendar body (rounded), leaving headroom at the top for the two binding tabs.
    QRectF const body(S * 0.15, S * 0.24, S * 0.70, S * 0.60);
    p.drawRoundedRect(body, S * 0.07, S * 0.07);

    // Two binding tabs crossing the top edge.
    qreal const tabX1 = body.left() + body.width() * 0.28;
    qreal const tabX2 = body.left() + body.width() * 0.72;
    qreal const tabTop = S * 0.15;
    qreal const tabBot = body.top() + body.height() * 0.10;
    p.drawLine(QPointF(tabX1, tabTop), QPointF(tabX1, tabBot));
    p.drawLine(QPointF(tabX2, tabTop), QPointF(tabX2, tabBot));

    // Header separator, then a filled header strip so it reads clearly as a calendar at small sizes.
    qreal const headerY = body.top() + body.height() * 0.30;
    p.drawLine(QPointF(body.left(), headerY), QPointF(body.right(), headerY));

    // A single centered "day" block in the lower area (one clean mark scales far better than a dot grid).
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    QRectF const day(body.center().x() - body.width() * 0.16,
                     headerY + body.height() * 0.22,
                     body.width() * 0.32,
                     body.height() * 0.30);
    p.drawRoundedRect(day, S * 0.03, S * 0.03);

    p.end();
    return QIcon(pm);
  }
}


/* Some ugly macros we use */
// TODO: make those methods instead???

#define DESTRUCTIVE_ACTION(ACTION_CODE)                                                                                \
QMessageBox::StandardButton reply;                                                                                     \
reply = QMessageBox::question(this, "Destructive action", "This action cannot be undone. Current change history will be lost. Continue?", \
QMessageBox::Yes|QMessageBox::No);                                                                                     \
if (reply == QMessageBox::Yes)                                                                                         \
{                                                                                                                      \
NOGGIT_ACTION_MGR->purge();                                                                            \
ACTION_CODE                                                                                                            \
}                                                                                                                      \


#define ADD_ACTION_NS(menu, name, on_action)                      \
  {                                                               \
    auto action (menu->addAction (name));                         \
    connect (action, &QAction::triggered, on_action);             \
  }


#define ADD_TOGGLE(menu_, name_, shortcut_, property_)            \
  do                                                              \
  {                                                               \
    QAction* action (new QAction (name_, this));                  \
    action->setShortcut (QKeySequence (shortcut_));               \
    action->setCheckable (true);                                  \
    action->setChecked (property_.get());                         \
    menu_->addAction (action);                                    \
    connect ( action, &QAction::toggled                           \
            , &property_, &Noggit::BoolToggleProperty::set      \
            );                                                    \
    connect ( &property_, &Noggit::BoolToggleProperty::changed  \
            , action, &QAction::setChecked                        \
            );                                                    \
  }                                                               \
  while (false)


#define ADD_TOGGLE_NS(menu_, name_, property_)                    \
  do                                                              \
  {                                                               \
    QAction* action (new QAction (name_, this));                  \
    action->setCheckable (true);                                  \
    action->setChecked (property_.get());                         \
    menu_->addAction (action);                                    \
    connect ( action, &QAction::toggled                           \
            , &property_, &Noggit::BoolToggleProperty::set      \
            );                                                    \
    connect ( &property_, &Noggit::BoolToggleProperty::changed  \
            , action, &QAction::setChecked                        \
            );                                                    \
  }                                                               \
  while (false)


#define ADD_TOGGLE_POST(menu_, name_, shortcut_, property_, post_)\
  do                                                              \
  {                                                               \
    QAction* action (new QAction (name_, this));                  \
    action->setShortcut (QKeySequence (shortcut_));               \
    action->setCheckable (true);                                  \
    action->setChecked (property_.get());                         \
    menu_->addAction (action);                                    \
    connect ( action, &QAction::toggled                           \
            , &property_, &Noggit::BoolToggleProperty::set      \
            );                                                    \
    connect ( &property_, &Noggit::BoolToggleProperty::changed  \
            , action, &QAction::setChecked                        \
            );                                                    \
    connect ( action, &QAction::toggled, post_);                  \
    connect ( &property_, &Noggit::BoolToggleProperty::changed, \
    post_);                                                       \
  }                                                               \
  while (false)



#define ADD_TOGGLE_NS_POST(menu_, name_, property_, code_)        \
  do                                                              \
  {                                                               \
    QAction* action (new QAction (name_, this));                  \
    action->setCheckable (true);                                  \
    action->setChecked (property_.get());                         \
    menu_->addAction (action);                                    \
    connect ( action, &QAction::toggled                           \
            , &property_, &Noggit::bool_toggle_property::set      \
            );                                                    \
    connect ( &property_, &Noggit::bool_toggle_property::changed  \
            , action, &QAction::setChecked                        \
            );                                                    \
      connect ( action, &QAction::toggled                         \
            ,  code_                                              \
            );                                                    \
    connect ( &property_, &Noggit::bool_toggle_property::changed  \
            , code_                                               \
            );                                                    \
  }                                                               \
  while (false)



#define ADD_ACTION(menu, name, shortcut, on_action)               \
  {                                                               \
    auto action (menu->addAction (name));                         \
    action->setShortcut (QKeySequence (shortcut));                \
    auto callback = on_action;                                    \
    connect (action, &QAction::triggered, [this, callback]()      \
    {                                                             \
       if (NOGGIT_CUR_ACTION) \
        return;                                                   \
       callback();                                                \
                                                                  \
    });                                                           \
  }


static const float XSENS = 15.0f;
static const float YSENS = 15.0f;

void MapView::set_editing_mode(editing_mode mode)
{
  // Selection markers (the disc under a spawn) only show while that spawn's tool is the active mode.
  _world->setDrawCreatureMarkers(mode == editing_mode::creature);
  _world->setDrawGameObjectMarkers(mode == editing_mode::gameobject);
  _world->setDrawGameObjectSpawns(mode == editing_mode::gameobject);

  {
    QSignalBlocker const asset_browser_blocker(_asset_browser_dock);
    QSignalBlocker const tex_browser_blocker(_texture_browser_dock);
    QSignalBlocker const texture_palette_blocker(_texture_palette_dock);
    QSignalBlocker const object_palette_blocker(_object_palette_dock);

    objectEditor->modelImport->hide();
    objectEditor->rotationEditor->hide();
    _texture_browser_dock->hide();
    _texture_picker_dock->hide();
    _texture_palette_dock->hide();
    _object_palette_dock->hide();
    _asset_browser_dock->hide();
    _viewport_overlay_ui->gizmoBar->hide();
  }

  auto previous_mode = _left_sec_toolbar->getCurrentMode();

  _left_sec_toolbar->setCurrentMode(this, mode);

  if (context() && context()->isValid())
  {
    if (mode == editing_mode::holes && previous_mode != editing_mode::holes)
    {
        _world->renderer()->getTerrainParamsUniformBlock()->draw_lines = true;
        _world->renderer()->getTerrainParamsUniformBlock()->draw_hole_lines = true;
    }
    else if (previous_mode == editing_mode::holes && mode != editing_mode::holes)
    {
        _world->renderer()->getTerrainParamsUniformBlock()->draw_lines = _draw_lines.get();
        _world->renderer()->getTerrainParamsUniformBlock()->draw_hole_lines = _draw_hole_lines.get();
    }

    _world->renderer()->getTerrainParamsUniformBlock()->draw_areaid_overlay = false;
    _world->renderer()->getTerrainParamsUniformBlock()->draw_impass_overlay = false;
    _world->renderer()->getTerrainParamsUniformBlock()->draw_paintability_overlay = false;
    _world->renderer()->getTerrainParamsUniformBlock()->draw_selection_overlay = false;
    _minimap->use_selection(nullptr);

    bool use_classic_ui = _settings->value("classicUI", true).toBool();

    switch (mode)
    {
      case editing_mode::ground:
        if (terrainTool->_edit_type != eTerrainType_Vertex || (terrainTool->_edit_type != eTerrainType_Script && terrainTool->getImageMaskSelector()->isEnabled()))
        {
          terrainTool->updateMaskImage();
        }
        break;
      case editing_mode::paint:
        if (texturingTool->getTexturingMode() == Noggit::Ui::texturing_mode::paint && texturingTool->getImageMaskSelector()->isEnabled())
        {
          texturingTool->updateMaskImage();
        }

        if (use_classic_ui)
        {
            if (texturingTool->show_unpaintable_chunks())
            {
                _world->renderer()->getTerrainParamsUniformBlock()->draw_paintability_overlay = true;
            }
        }
        else
        {
            if (_left_sec_toolbar->showUnpaintableChunk())
            {
                _world->renderer()->getTerrainParamsUniformBlock()->draw_paintability_overlay = true;
            }
        }
        break;
      case editing_mode::mccv:
        if (shaderTool->getImageMaskSelector()->isEnabled())
        {
          shaderTool->updateMaskImage();
        }
        break;
      case editing_mode::stamp:
        if (stampTool->getActiveBrushItem() && stampTool->getActiveBrushItem()->isEnabled())
        {
          stampTool->getActiveBrushItem()->updateMask();
        }
        break;
      case editing_mode::areaid:
        _world->renderer()->getTerrainParamsUniformBlock()->draw_areaid_overlay = true;
        break;
      case editing_mode::flags:
        _world->renderer()->getTerrainParamsUniformBlock()->draw_impass_overlay = true;
        break;
      case editing_mode::minimap:
        _world->renderer()->getTerrainParamsUniformBlock()->draw_selection_overlay = true;
        _minimap->use_selection(minimapTool->getSelectedTiles());
        break;
      case editing_mode::creature:
        _show_creature_browser.set(true);
        if (!_world->hasCreatureSpawnsLoaded())
        {
          _world->reloadCreatureSpawns();
        }
        _world->setDrawCreatureSpawns(true);
        rebuildCreatureBrowserList(true);
        updateDatabaseStatus();
        break;
      case editing_mode::gameobject:
        _show_gameobject_browser.set(true);
        _world->ensureGameObjectSpawnsLoaded();
        rebuildGameObjectBrowserList(true);
        updateGameObjectBrowserStatus();
        break;
      default:
        break;
    }
  }

  MoveObj = false;
  _world->reset_selection();
  _rotation_editor_need_update = true;

  if (!ui_hidden)
  {
    setToolPropertyWidgetVisibility(mode);
  }

  terrainMode = mode;
  _toolbar->check_tool (mode);
  this->activateWindow();

  _world->renderer()->markTerrainParamsUniformBlockDirty();
}

void MapView::setToolPropertyWidgetVisibility(editing_mode mode)
{
  bool const creature_mode = mode == editing_mode::creature;
  bool const gameobject_mode = mode == editing_mode::gameobject;
  bool const spawn_mode = creature_mode || gameobject_mode; // dock-based tools without a tool panel
  _main_window->setCorner(Qt::BottomRightCorner,
                          spawn_mode ? Qt::BottomDockWidgetArea : Qt::RightDockWidgetArea);

  if (_tool_panel_dock)
  {
    _tool_panel_dock->setVisible(!ui_hidden && !spawn_mode);
    if (!spawn_mode)
    {
      // gameobject (=15) has no tool-panel page; setCurrentIndex must not be called with it.
      _tool_panel_dock->setCurrentIndex(static_cast<int>(mode));
    }
  }

  auto set_creature_docks_visible = [](bool visible, std::initializer_list<QDockWidget*> docks)
  {
    for (auto* dock : docks)
    {
      if (dock)
      {
        dock->setVisible(visible);
      }
    }
  };

  if (mode != editing_mode::creature)
  {
    if (_creature_actions_overlay)
    {
      _creature_actions_overlay->setVisible(false);
    }
    set_creature_docks_visible(false, {_creature_browser_dock,
                                       _creature_editor_dock,
                                       _creature_model_picker_dock});
  }

  if (mode != editing_mode::gameobject)
  {
    if (_gameobject_actions_overlay)
    {
      _gameobject_actions_overlay->setVisible(false);
    }
    set_creature_docks_visible(false, {_gameobject_browser_dock,
                                       _gameobject_model_picker_dock});
  }

  switch (mode)
  {

  case editing_mode::object:
    _asset_browser_dock->setVisible(!ui_hidden && _settings->value("map_view/asset_browser", false).toBool());
    _object_palette_dock->setVisible(!ui_hidden && _settings->value("map_view/object_palette", false).toBool());
    _viewport_overlay_ui->gizmoBar->setVisible(!ui_hidden);
    break;
  case editing_mode::creature:
    if (_creature_actions_overlay)
    {
      _creature_actions_overlay->setVisible(!ui_hidden && _show_creature_browser.get());
    }
    set_creature_docks_visible(!ui_hidden && _show_creature_browser.get(),
                               {_creature_browser_dock,
                                _creature_editor_dock,
                                _creature_model_picker_dock});
    if (!ui_hidden && _show_creature_browser.get())
    {
      if (_creature_browser_dock && _creature_editor_dock)
      {
        _main_window->resizeDocks({_creature_browser_dock, _creature_editor_dock},
                                  {3, 1},
                                  Qt::Vertical);
      }
      if (_creature_browser_dock)
      {
        _main_window->resizeDocks({_creature_browser_dock}, {330}, Qt::Horizontal);
      }
      if (_creature_model_picker_dock)
      {
        _main_window->resizeDocks({_creature_model_picker_dock}, {260}, Qt::Vertical);
      }
    }
    break;
  case editing_mode::gameobject:
    if (_gameobject_actions_overlay)
    {
      _gameobject_actions_overlay->setVisible(!ui_hidden && _show_gameobject_browser.get());
    }
    set_creature_docks_visible(!ui_hidden && _show_gameobject_browser.get(),
                               {_gameobject_browser_dock,
                                _gameobject_model_picker_dock});
    if (!ui_hidden && _show_gameobject_browser.get())
    {
      if (_gameobject_browser_dock)
      {
        _main_window->resizeDocks({_gameobject_browser_dock}, {330}, Qt::Horizontal);
      }
      if (_gameobject_model_picker_dock)
      {
        _main_window->resizeDocks({_gameobject_model_picker_dock}, {260}, Qt::Vertical);
      }
    }
    break;
  case editing_mode::paint:
    _texture_browser_dock->setVisible(!ui_hidden && _settings->value("map_view/texture_browser", false).toBool());
    _texture_palette_dock->setVisible(!ui_hidden && _settings->value("map_view/texture_palette", false).toBool());
    break;
  default:
    break;
  }

  
}

void MapView::ResetSelectedObjectRotation()
{
  if (terrainMode != editing_mode::object)
  {
    return;
  }

  for (auto& selection : _world->current_selection())
  {
    if (selection.index() != eEntry_Object)
      continue;

    auto obj = std::get<selected_object_type>(selection);

    if (obj->which() == eWMO)
    {
      WMOInstance* wmo = static_cast<WMOInstance*>(obj);
      _world->updateTilesWMO(wmo, model_update::remove);
      wmo->resetDirection();
      _world->updateTilesWMO(wmo, model_update::add);
    }
    else if (obj->which() == eMODEL)
    {
      ModelInstance* m2 = static_cast<ModelInstance*>(obj);
      _world->updateTilesModel(m2, model_update::remove);
      m2->resetDirection();
      m2->recalcExtents();
      _world->updateTilesModel(m2, model_update::add);
    }
  }

  _rotation_editor_need_update = true;
}

void MapView::snap_selected_models_to_the_ground()
{
  if (terrainMode != editing_mode::object)
  {
    return;
  }

  _world->snap_selected_models_to_the_ground();
  _rotation_editor_need_update = true;
}


void MapView::DeleteSelectedObjects()
{
  if (terrainMode != editing_mode::object)
  {
    return;
  }

  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());

  _world->delete_selected_models();
  _rotation_editor_need_update = true;
}


void MapView::changeZoneIDValue (int set)
{
  _selected_area_id = set;
}


QWidgetAction* MapView::createTextSeparator(const QString& text)
{
  auto* pLabel = new QLabel(text);
  //pLabel->setMinimumWidth(this->minimumWidth() - 4);
  pLabel->setAlignment(Qt::AlignCenter);
  auto* separator = new QWidgetAction(this);
  separator->setDefaultWidget(pLabel);
  return separator;
}

void MapView::enterEvent(QEvent* event)
{
  // check if noggit is the currently active windows
  if (static_cast<QApplication*>(QApplication::instance())->applicationState() & Qt::ApplicationActive)
  {
    activateWindow();
  }
}

void MapView::setupViewportOverlay()
{
  _overlay_widget = new QWidget(this);
  _viewport_overlay_ui = new ::Ui::MapViewOverlay();
  _viewport_overlay_ui->setupUi(_overlay_widget);
  _overlay_widget->setAttribute(Qt::WA_TranslucentBackground);
  _overlay_widget->setMouseTracking(true);
  _overlay_widget->setGeometry(0,0, width(), height());

  _viewport_overlay_ui->gizmoVisibleButton->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::GIZMO_VISIBILITY));
  _viewport_overlay_ui->gizmoModeButton->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::GIZMO_LOCAL));
  _viewport_overlay_ui->gizmoRotateButton->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::GIZMO_ROTATE));
  _viewport_overlay_ui->gizmoScaleButton->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::GIZMO_SCALE));
  _viewport_overlay_ui->gizmoTranslateButton->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::GIZMO_TRANSLATE));

  connect(this, &MapView::resized
    ,[this]()
          {
            _overlay_widget->setGeometry(0, 0, width(), height());
          }
  );

  connect(_viewport_overlay_ui->gizmoVisibleButton, &QPushButton::clicked
    ,[this]()
          {
            _gizmo_on.set(_viewport_overlay_ui->gizmoVisibleButton->isChecked());
          }
  );

  connect(&_gizmo_on, &Noggit::BoolToggleProperty::changed
    ,[this](bool state)
          {
            _viewport_overlay_ui->gizmoVisibleButton->setChecked(state);
          }
  );

  connect(_viewport_overlay_ui->gizmoModeButton, &QPushButton::clicked, [this]()
  {
      if (_viewport_overlay_ui->gizmoModeButton->isChecked())
      {
          _gizmo_mode = ImGuizmo::MODE::WORLD;
      }
      else
      {
          _gizmo_mode = ImGuizmo::MODE::LOCAL;
      }
  });

  connect(_viewport_overlay_ui->gizmoTranslateButton, &QPushButton::clicked, [this]() {
      updateGizmoOverlay(ImGuizmo::OPERATION::TRANSLATE);
    });

  connect(_viewport_overlay_ui->gizmoRotateButton, &QPushButton::clicked, [this]() {
      updateGizmoOverlay(ImGuizmo::OPERATION::ROTATE);
    });

  connect(_viewport_overlay_ui->gizmoScaleButton, &QPushButton::clicked, [this]() {
      updateGizmoOverlay(ImGuizmo::OPERATION::SCALE);
    });
}

void MapView::updateGizmoOverlay(ImGuizmo::OPERATION operation)
{
  if (operation == ImGuizmo::OPERATION::TRANSLATE)
  {
    _viewport_overlay_ui->gizmoRotateButton->setChecked(false);
    _viewport_overlay_ui->gizmoScaleButton->setChecked(false);

    if (!_viewport_overlay_ui->gizmoTranslateButton->isChecked())
      _viewport_overlay_ui->gizmoTranslateButton->setChecked(true);
  }

  if (operation == ImGuizmo::OPERATION::ROTATE)
  {
    _viewport_overlay_ui->gizmoTranslateButton->setChecked(false);
    _viewport_overlay_ui->gizmoScaleButton->setChecked(false);

    if (!_viewport_overlay_ui->gizmoRotateButton->isChecked())
      _viewport_overlay_ui->gizmoRotateButton->setChecked(true);
  }

  if (operation == ImGuizmo::OPERATION::SCALE)
  {
    _viewport_overlay_ui->gizmoTranslateButton->setChecked(false);
    _viewport_overlay_ui->gizmoRotateButton->setChecked(false);

    if (!_viewport_overlay_ui->gizmoScaleButton->isChecked())
      _viewport_overlay_ui->gizmoScaleButton->setChecked(true);
  }

  _gizmo_operation = operation;
}

void MapView::setupRaiseLowerUi()
{
  terrainTool = new Noggit::Ui::TerrainTool(this, this);
  _tool_panel_dock->registerTool("Raise | Lower", terrainTool);

  connect(terrainTool
    , &Noggit::Ui::TerrainTool::updateVertices
    , [this](int vertex_mode, math::degrees const& angle, math::degrees const& orientation)
          {
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());

            _world->orientVertices(vertex_mode == eVertexMode_Mouse
                                   ? _cursor_pos
                                   : _world->vertexCenter()
              , angle
              , orientation
            );
          }
  );

  terrainTool->storeCursorPos(&_cursor_pos);

}

void MapView::setupFlattenBlurUi()
{
  flattenTool = new Noggit::Ui::flatten_blur_tool(this);
  _tool_panel_dock->registerTool("Flatten | Blur", flattenTool);
}

void MapView::setupTexturePainterUi()
{
  /* Tool */
  texturingTool = new Noggit::Ui::texturing_tool(&_camera.position, this, &_show_texture_palette_small_window, this);
  _tool_panel_dock->registerTool("Texture Painter", texturingTool);

  // Connects
  connect( texturingTool->texture_swap_tool()->texture_display()
    , &Noggit::Ui::current_texture::texture_dropped
    , [=] (std::string const& filename)
           {
             makeCurrent();
             OpenGL::context::scoped_setter const _(::gl, context());

             texturingTool->texture_swap_tool()->set_texture(filename);
           }
  );

  connect( texturingTool->_current_texture
    , &Noggit::Ui::current_texture::texture_dropped
    , [=] (std::string const& filename)
           {
             makeCurrent();
             OpenGL::context::scoped_setter const _(::gl, context());

             Noggit::Ui::selected_texture::set({filename, _context});
           }
  );

  connect(texturingTool->_current_texture, &Noggit::Ui::current_texture::clicked
    , [=]
          {
            _texture_browser_dock->setVisible(!_texture_browser_dock->isVisible());
          }
  );

  /* Additional tools */

  /* Texture Browser */

  // Dock
  _texture_browser_dock = new QDockWidget("Texture Browser", this);
  _texture_browser_dock->setFeatures(QDockWidget::DockWidgetMovable
                                     | QDockWidget::DockWidgetFloatable
                                     | QDockWidget::DockWidgetClosable);
  _texture_browser_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea | Qt::LeftDockWidgetArea);
  _main_window->addDockWidget(Qt::BottomDockWidgetArea, _texture_browser_dock);
  _texture_browser_dock->hide();

  connect(_texture_browser_dock, &QDockWidget::visibilityChanged,
          [=](bool visible)
          {
            if (ui_hidden)
              return;

            _settings->setValue ("map_view/texture_browser", visible);
            _settings->sync();
          });

  connect(this, &QObject::destroyed, _texture_browser_dock, &QObject::deleteLater);
  // End Dock

  TexturePalette = new Noggit::Ui::tileset_chooser(this);
  _texture_browser_dock->setWidget(TexturePalette);
  connect(this, &QObject::destroyed, TexturePalette, &QObject::deleteLater);

  connect(TexturePalette, &Noggit::Ui::tileset_chooser::selected
    , [=](std::string const& filename)
          {
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());

            Noggit::Ui::selected_texture::set({filename, _context});
            texturingTool->_current_texture->set_texture(filename);
            TexturePicker->setMainTexture(texturingTool->_current_texture);
            TexturePicker->updateSelection();
          }
  );

  connect ( TexturePalette, &Noggit::Ui::widget::visibilityChanged
    , &_show_texture_palette_window, &Noggit::BoolToggleProperty::set
  );

  connect ( &_show_texture_palette_window, &Noggit::BoolToggleProperty::changed
    ,  [this]
            {
              if ((terrainMode == editing_mode::paint || terrainMode == editing_mode::stamp)  && !ui_hidden)
              {
                _texture_browser_dock->setVisible(_show_texture_palette_window.get());
              }
              else
              {
                QSignalBlocker const _ (_show_texture_palette_window);
                _show_texture_palette_window.set(false);
              }
            }
  );


  /* Texture Palette Small */
  _texture_palette_small = new Noggit::Ui::texture_palette_small(_project, _world->getMapID(), this);

  // Dock
  _texture_palette_dock = new QDockWidget("Texture Palette", this);
  _texture_palette_dock->setFeatures(QDockWidget::DockWidgetMovable
                                     | QDockWidget::DockWidgetFloatable
                                     | QDockWidget::DockWidgetClosable
  );

  _texture_palette_dock->setWidget(_texture_palette_small);
  _texture_palette_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);;

  _main_window->addDockWidget(Qt::BottomDockWidgetArea, _texture_palette_dock);
  // End Dock

  connect(_texture_palette_dock, &QDockWidget::visibilityChanged,
          [=](bool visible)
          {
            if (ui_hidden)
              return;

            _settings->setValue ("map_view/texture_palette", visible);
            _settings->sync();
          });

  connect(_texture_palette_small, &Noggit::Ui::texture_palette_small::selected
    , [=](std::string const& filename)
          {
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());

            Noggit::Ui::selected_texture::set({filename, _context});
            texturingTool->_current_texture->set_texture(filename);
          }
  );
  connect(this, &QObject::destroyed, _texture_palette_small, &QObject::deleteLater);

  connect(&_show_texture_palette_small_window, &Noggit::BoolToggleProperty::changed
    , _texture_palette_dock, [this]
          {
            QSignalBlocker const blocker(_show_texture_palette_small_window);
            if (terrainMode == editing_mode::paint && !ui_hidden)
            {
              _texture_palette_dock->setVisible(_show_texture_palette_small_window.get());
            }
            else
            {
              _show_texture_palette_small_window.set(false);
            }
          }
  );
  connect(_texture_palette_dock, &QDockWidget::visibilityChanged
    , &_show_texture_palette_small_window, &Noggit::BoolToggleProperty::set
  );

  connect(texturingTool->_current_texture, &Noggit::Ui::current_texture::texture_updated
          , [=]()
      {
       _world->notifyTileRendererOnSelectedTextureChange();
      }
  );

  /* Texture Picker */

  // Dock
  _texture_picker_dock = new QDockWidget("Texture picker", this);
  _texture_picker_dock->setFeatures(QDockWidget::DockWidgetMovable
                                  | QDockWidget::DockWidgetFloatable
                                  | QDockWidget::DockWidgetClosable);
  _main_window->addDockWidget(Qt::BottomDockWidgetArea, _texture_picker_dock);
  _texture_picker_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  _texture_picker_dock->setFloating(true);
  _texture_picker_dock->hide();
  connect(this, &QObject::destroyed, _texture_picker_dock, &QObject::deleteLater);
  // End Dock

  TexturePicker = new Noggit::Ui::texture_picker(texturingTool->_current_texture, this);
  _texture_picker_dock->setWidget(TexturePicker);
  connect(this, &QObject::destroyed, TexturePicker, &QObject::deleteLater);

  connect( TexturePicker
    , &Noggit::Ui::texture_picker::set_texture
    , [=] (scoped_blp_texture_reference texture)
           {
             makeCurrent();
             OpenGL::context::scoped_setter const _(::gl, context());
             Noggit::Ui::selected_texture::set(std::move(texture));
           }
  );
  connect(TexturePicker, &Noggit::Ui::texture_picker::shift_left
    , [=]
          {
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());
            TexturePicker->shiftSelectedTextureLeft();
          }
  );
  connect(TexturePicker, &Noggit::Ui::texture_picker::shift_right
    , [=]
          {
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());
            TexturePicker->shiftSelectedTextureRight();
          }
  );

}

void MapView::setupHoleCutterUi()
{
  holeTool = new Noggit::Ui::hole_tool(this);
  _tool_panel_dock->registerTool("Hole Cutter", holeTool);
}

void MapView::setupAreaDesignatorUi()
{
  ZoneIDBrowser = new Noggit::Ui::zone_id_browser(this);
  _tool_panel_dock->registerTool("Area Designator", ZoneIDBrowser);

  ZoneIDBrowser->setMapID(_world->getMapID());
  connect(ZoneIDBrowser, &Noggit::Ui::zone_id_browser::selected
    , [this](int area_id) { changeZoneIDValue(area_id); }
  );
}

void MapView::setupFlagUi()
{
  auto placeholder = new QWidget(this);
  _tool_panel_dock->registerTool("Flag", placeholder);
}

void MapView::setupWaterEditorUi()
{
  guiWater = new Noggit::Ui::water(&_displayed_water_layer, &_display_all_water_layers, this);
  _tool_panel_dock->registerTool("Water Editor", guiWater);

  connect(guiWater, &Noggit::Ui::water::regenerate_water_opacity
    , [this](float factor)
          {
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());
            NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_WATER);
            _world->autoGenWaterTrans(_camera.position, factor);
            NOGGIT_ACTION_MGR->endAction();
          }
  );

  connect(guiWater, &Noggit::Ui::water::crop_water
    , [this]
          {
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());
            NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_WATER);
            _world->CropWaterADT(_camera.position);
            NOGGIT_ACTION_MGR->endAction();
          }
  );
}
void MapView::setupVertexPainterUi()
{
  shaderTool = new Noggit::Ui::ShaderTool(this, this);
  _tool_panel_dock->registerTool("Vertex Painter", shaderTool);
}

void MapView::setupScriptingUi()
{
  scriptingTool = new Noggit::Scripting::scripting_tool(this, this, _settings);
  _tool_panel_dock->registerTool("Scripting", scriptingTool);
}

void MapView::setupObjectEditorUi()
{
  /* Tool */
  objectEditor = new Noggit::Ui::object_editor(this
    , _world.get()
    , &_move_model_to_cursor_position
    , &_snap_multi_selection_to_ground
    , &_use_median_pivot_point
    , &_object_paste_params
    , &_rotate_along_ground
    , &_rotate_along_ground_smooth
    , &_rotate_along_ground_random
    , &_move_model_snap_to_objects
    , this
  );
  _tool_panel_dock->registerTool("Object Editor", objectEditor);

  /* Additional tools */

  /* Area selection */
  _area_selection = new QRubberBand(QRubberBand::Rectangle, this);

  /* Object Palette */
  _object_palette = new Noggit::Ui::ObjectPalette(this, _project, this);
  _object_palette->hide();

  // Dock
  _object_palette_dock = new QDockWidget("Object Palette", this);
  _object_palette_dock->setFeatures(QDockWidget::DockWidgetMovable
                                    | QDockWidget::DockWidgetFloatable
                                    | QDockWidget::DockWidgetClosable
  );

  _object_palette_dock->setWidget(_object_palette);
  _object_palette_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  _main_window->addDockWidget(Qt::BottomDockWidgetArea, _object_palette_dock);
  connect(this, &QObject::destroyed, _texture_palette_dock, &QObject::deleteLater);
  // End Dock

  connect(_object_palette_dock, &QDockWidget::visibilityChanged,
          [=](bool visible)
          {
            if (ui_hidden)
              return;

            _settings->setValue ("map_view/object_palette", visible);
            _settings->sync();
          });

}

void MapView::setupCreatureEditorUi()
{
  // The coordinate editor lives as a column inside the NPC Model Picker splitter (built later in
  // setupCreatureModelPickerUi); we just build the panel widget here and hand it off via the member.
  auto container = new QWidget(this);
  container->setMinimumWidth(240);
  auto layout = new QVBoxLayout(container);
  layout->setContentsMargins(6, 6, 6, 6);

  auto coord_title = new QLabel("Creature Coordinates", container);
  coord_title->setStyleSheet("font-weight: bold;");
  layout->addWidget(coord_title);

  auto hint = new QLabel("Left click: select or drag\nShift: add or box-select", container);
  hint->setWordWrap(true);
  layout->addWidget(hint);

  _creature_editor_info = new QLabel("No spawn selected", container);
  _creature_editor_info->setWordWrap(true);
  _creature_editor_info->setStyleSheet("font-style: italic; color: #888; padding: 2px 0;");
  layout->addWidget(_creature_editor_info);

  auto make_spin = [&](double lo, double hi, double step) {
    auto* sb = new QDoubleSpinBox(container);
    sb->setRange(lo, hi);
    sb->setDecimals(3);
    sb->setSingleStep(step);
    sb->setEnabled(false);
    return sb;
  };

  // Noggit client coordinates span 0..2*ZEROPOINT (~34133) on normal maps and +/-ZEROPOINT on global
  // WMO maps, so the X/Z range must be wide enough not to clamp distant spawns.
  _spawn_edit_x           = make_spin(-64000.0, 64000.0, 0.1);
  _spawn_edit_y           = make_spin(-20000.0, 20000.0, 0.1);
  _spawn_edit_z           = make_spin(-64000.0, 64000.0, 0.1);
  _spawn_edit_orientation = make_spin(     0.0,   360.0, 1.0);
  _spawn_edit_orientation->setWrapping(true);
  _spawn_edit_orientation->setSuffix(QString::fromUtf8("\xc2\xb0"));  // ┬░

  auto form = new QFormLayout();
  form->setContentsMargins(0, 4, 0, 0);
  form->setSpacing(3);
  form->addRow("X:", _spawn_edit_x);
  form->addRow("Y (height):", _spawn_edit_y);
  form->addRow("Z:", _spawn_edit_z);
  form->addRow("Orientation:", _spawn_edit_orientation);
  layout->addLayout(form);

  auto on_change = [this](double) {
    if (!_selected_creature_spawn_guid)
      return;
    auto* spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid);
    if (!spawn)
      return;

    spawn->pos = glm::vec3(
      static_cast<float>(_spawn_edit_x->value()),
      static_cast<float>(_spawn_edit_y->value()),
      static_cast<float>(_spawn_edit_z->value()));
    spawn->orientation = static_cast<float>(_spawn_edit_orientation->value());
    spawn->dirty = spawn->pending_create
                || glm::distance(spawn->pos, spawn->original_pos) > 0.01f
                || std::abs(spawn->orientation - spawn->original_orientation) > 0.01f;

    if (spawn->model_instance)
    {
      spawn->model_instance->pos = spawn->pos;
      spawn->model_instance->dir = glm::vec3(0.0f, spawn->orientation, 0.0f);
      spawn->model_instance->recalcExtents();
    }

    updateDatabaseStatus();
    rebuildCreatureBrowserList(true);
    _needs_redraw = true;
  };

  connect(_spawn_edit_x,           qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);
  connect(_spawn_edit_y,           qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);
  connect(_spawn_edit_z,           qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);
  connect(_spawn_edit_orientation, qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);

  _creature_editor_panel = container;
}

void MapView::setupCreatureBrowserUi()
{
  _creature_browser_dock = new QDockWidget("Creature Browser", _main_window);
  _creature_browser_dock->setFeatures(QDockWidget::DockWidgetMovable
                                      | QDockWidget::DockWidgetFloatable
                                      | QDockWidget::DockWidgetClosable);
  _creature_browser_dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
  _creature_browser_dock->setMinimumWidth(330);
  _creature_browser_dock->resize(380, 520);
  _main_window->addDockWidget(Qt::RightDockWidgetArea, _creature_browser_dock);
  connect(this, &QObject::destroyed, _creature_browser_dock, &QObject::deleteLater);

  auto container = new QWidget(this);
  auto layout = new QVBoxLayout(container);
  layout->setContentsMargins(6, 6, 6, 6);

  _creature_search_field = new QLineEdit(container);
  _creature_search_field->setPlaceholderText("Search by creature name, guid, or entry");
  layout->addWidget(_creature_search_field);

  _creature_zone_filter = new QCheckBox("Zone only (current zone)", container);
  _creature_zone_filter->setToolTip("Only list creatures whose position is in the same zone as the camera"
                                    " (e.g. Searing Gorge). Spawns in unloaded tiles are excluded.");
  layout->addWidget(_creature_zone_filter);
  connect(_creature_zone_filter, &QCheckBox::toggled, [this]() { rebuildCreatureBrowserList(true); });

  // Type/rank filter, mirroring the NPC model picker. Filters the current-map spawn list by the spawned
  // creature's template type/rank (looked up per spawn entry from _creature_template_filter_info, which
  // the model picker fills when it loads creature_template). The Type combo is populated there too.
  {
    auto type_row = new QHBoxLayout();
    type_row->addWidget(new QLabel("Type", container));
    _creature_browser_type_filter = new QComboBox(container);
    _creature_browser_type_filter->addItem("All types"); // index 0 -> no type filter
    _creature_browser_type_filter->setToolTip("Filter the spawn list by creature type.");
    type_row->addWidget(_creature_browser_type_filter, 1);
    layout->addLayout(type_row);

    auto flag_row = new QHBoxLayout();
    _creature_browser_elite = new QCheckBox("Elite", container);
    _creature_browser_boss = new QCheckBox("Boss", container);
    _creature_browser_civilian = new QCheckBox("Civilian", container);
    _creature_browser_trainer = new QCheckBox("Trainer", container);
    _creature_browser_elite->setToolTip("Show rank 1 and 2 spawns.");
    _creature_browser_boss->setToolTip("Show rank 3 and higher spawns.");
    flag_row->addWidget(_creature_browser_elite);
    flag_row->addWidget(_creature_browser_boss);
    flag_row->addWidget(_creature_browser_civilian);
    flag_row->addWidget(_creature_browser_trainer);
    flag_row->addStretch(1);
    layout->addLayout(flag_row);

    connect(_creature_browser_type_filter, qOverload<int>(&QComboBox::currentIndexChanged),
            [this]() { rebuildCreatureBrowserList(true); });
    for (auto* cb : {_creature_browser_elite, _creature_browser_boss,
                     _creature_browser_civilian, _creature_browser_trainer})
    {
      connect(cb, &QCheckBox::toggled, [this]() { rebuildCreatureBrowserList(true); });
    }
  }

  _creature_list_widget = new QListWidget(container);
  _creature_list_widget->setSelectionMode(QAbstractItemView::SingleSelection);
  _creature_list_widget->setMinimumHeight(360);
  layout->addWidget(_creature_list_widget, 1);

  _creature_browser_status = new QLabel(container);
  _creature_browser_status->setWordWrap(true);
  layout->addWidget(_creature_browser_status);

  _creature_browser_dock->setWidget(container);
  _creature_browser_dock->setVisible(false);

  connect(_creature_browser_dock, &QDockWidget::visibilityChanged,
          [this](bool visible)
          {
            if (ui_hidden)
              return;

            _settings->setValue("map_view/creature_browser", visible);
            _settings->sync();
          });

  connect(&_show_creature_browser, &Noggit::BoolToggleProperty::changed,
          [this](bool visible)
          {
            bool const show = visible && !ui_hidden && terrainMode == editing_mode::creature;
            if (ui_hidden && visible)
            {
              return;
            }

            if (_creature_actions_overlay)
            {
              _creature_actions_overlay->setVisible(show);
            }

            for (auto* dock : {_creature_browser_dock,
                               _creature_editor_dock,
                               _creature_model_picker_dock})
            {
              if (dock)
              {
                dock->setVisible(show);
              }
            }
          });
  connect(_creature_browser_dock, &QDockWidget::visibilityChanged,
          &_show_creature_browser, &Noggit::BoolToggleProperty::set);

  connect(_creature_search_field, &QLineEdit::textChanged,
          [this]()
          {
            rebuildCreatureBrowserList(true);
          });
  connect(_creature_list_widget, &QListWidget::itemClicked,
          this, &MapView::jumpToCreatureListItem);

  updateCreatureBrowserStatus();
}

void MapView::setupCreatureActionsUi()
{
  if (!_overlay_widget)
  {
    return;
  }

  _creature_actions_overlay = new QWidget(_overlay_widget);
  _creature_actions_overlay->setObjectName("creatureActionsOverlay");
  _creature_actions_overlay->setAttribute(Qt::WA_StyledBackground, true);
  _creature_actions_overlay->setStyleSheet(
    "#creatureActionsOverlay { background: rgba(28, 31, 37, 210); border: 1px solid rgba(85, 91, 103, 180); }"
    "#creatureActionsOverlay QPushButton { padding: 5px 9px; }");

  auto layout = new QHBoxLayout(_creature_actions_overlay);
  layout->setContentsMargins(5, 5, 5, 5);
  layout->setSpacing(5);

  auto reload_button = new QPushButton("Reload Spawns", _creature_actions_overlay);
  auto save_button = new QPushButton("Export SQL", _creature_actions_overlay);
  auto revert_button = new QPushButton("Discard Pending", _creature_actions_overlay);
  auto pending_button = new QPushButton("Pending \xE2\x96\xBE", _creature_actions_overlay);
  pending_button->setToolTip("Show the list of pending creature updates waiting for SQL export.");
  layout->addWidget(reload_button);
  layout->addWidget(save_button);
  layout->addWidget(revert_button);
  layout->addWidget(pending_button);

  // Toggleable dropdown listing every pending change (new / moved / deleted) awaiting SQL export.
  _creature_pending_popup = new QWidget(this, Qt::Popup);
  _creature_pending_popup->setObjectName("creaturePendingPopup");
  _creature_pending_popup->setAttribute(Qt::WA_StyledBackground, true);
  _creature_pending_popup->setStyleSheet(
    "#creaturePendingPopup { background: rgba(28, 31, 37, 235); border: 1px solid rgba(85, 91, 103, 200); }");
  auto pending_layout = new QVBoxLayout(_creature_pending_popup);
  pending_layout->setContentsMargins(6, 6, 6, 6);
  pending_layout->setSpacing(4);
  auto pending_title = new QLabel("Pending creature updates", _creature_pending_popup);
  pending_title->setStyleSheet("font-weight: bold; color: #ddd;");
  pending_layout->addWidget(pending_title);
  _creature_pending_list = new QListWidget(_creature_pending_popup);
  _creature_pending_list->setMinimumSize(360, 220);
  _creature_pending_list->setSelectionMode(QListWidget::NoSelection);
  pending_layout->addWidget(_creature_pending_list);

  auto refresh_pending = [this]()
  {
    if (!_creature_pending_list)
    {
      return;
    }
    _creature_pending_list->clear();
    int count = 0;
    for (auto const& spawn : _world->creatureSpawns())
    {
      if (!spawn.dirty)
      {
        continue;
      }
      // A spawn created and deleted this session never hit the DB -> nothing to export.
      if (spawn.pending_delete && spawn.pending_create)
      {
        continue;
      }
      QString action;
      if (spawn.pending_delete)
      {
        action = "DELETE";
      }
      else if (spawn.pending_create)
      {
        action = "NEW";
      }
      else
      {
        action = "MOVE";
      }
      QString const name = QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name);
      _creature_pending_list->addItem(
        QString("[%1] guid %2  entry %3  %4").arg(action).arg(spawn.guid).arg(spawn.entry).arg(name));
      ++count;
    }
    if (count == 0)
    {
      _creature_pending_list->addItem("No pending updates.");
    }
  };

  connect(pending_button, &QPushButton::clicked,
          [this, pending_button, refresh_pending]()
          {
            if (!_creature_pending_popup)
            {
              return;
            }
            if (_creature_pending_popup->isVisible())
            {
              _creature_pending_popup->hide();
              return;
            }
            refresh_pending();
            _creature_pending_popup->adjustSize();
            QPoint const below = pending_button->mapToGlobal(QPoint(0, pending_button->height() + 2));
            _creature_pending_popup->move(below);
            _creature_pending_popup->show();
          });

  auto place_actions = [this]()
  {
    if (!_creature_actions_overlay)
    {
      return;
    }

    _creature_actions_overlay->adjustSize();
    _creature_actions_overlay->move(std::max(8, width() - _creature_actions_overlay->width() - 14), 8);
    _creature_actions_overlay->raise();
  };

  _creature_actions_overlay->setVisible(false);
  place_actions();
  connect(this, &MapView::resized, this, place_actions);
  connect(this, &QObject::destroyed, _creature_actions_overlay, &QObject::deleteLater);

  connect(reload_button, &QPushButton::clicked,
          [this]()
          {
            refreshCreatureSpawnOverlay(true);
          });
  connect(save_button, &QPushButton::clicked,
          [this]()
          {
            saveDirtyCreatureSpawns();
          });
  connect(revert_button, &QPushButton::clicked,
          [this]()
          {
            discardPendingCreatureSpawns();
          });
}

void MapView::setupCreatureModelPickerUi()
{
  _creature_model_picker_dock = new QDockWidget("NPC Model Picker", _main_window);
  _creature_model_picker_dock->setFeatures(QDockWidget::DockWidgetMovable
                                           | QDockWidget::DockWidgetFloatable
                                           | QDockWidget::DockWidgetClosable);
  _creature_model_picker_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  _creature_model_picker_dock->setMinimumHeight(220);

  auto container = new QWidget(this);
  auto root_layout = new QVBoxLayout(container);
  root_layout->setContentsMargins(6, 6, 6, 6);
  root_layout->setSpacing(6);

  // Filters now live in a small vertical panel beside the list (added to the splitter below).
  auto filter_panel = new QWidget(container);
  filter_panel->setMaximumWidth(150);
  auto filter_layout = new QVBoxLayout(filter_panel);
  filter_layout->setContentsMargins(4, 4, 4, 4);
  filter_layout->setSpacing(4);
  auto filter_title = new QLabel("Filters", filter_panel);
  filter_title->setStyleSheet("font-weight: bold;");
  filter_layout->addWidget(filter_title);
  auto type_label = new QLabel("Type", filter_panel);
  auto type_filter = new QComboBox(filter_panel);
  type_filter->setToolTip("Filter entries by creature type. Populated from the loaded creature_template list.");
  type_filter->addItem("All types");  // index 0: no data -> no type filter
  auto elite_only = new QCheckBox("Elite", filter_panel);
  auto boss_only = new QCheckBox("Boss", filter_panel);
  auto civilian_only = new QCheckBox("Civilian", filter_panel);
  auto trainer_only = new QCheckBox("Trainer", filter_panel);
  elite_only->setToolTip("Show rank 1 and 2 entries.");
  boss_only->setToolTip("Show rank 3 and higher entries.");
  civilian_only->setToolTip("Show entries with the civilian type flag.");
  trainer_only->setToolTip("Show entries with trainer NPC flags.");
  filter_layout->addWidget(type_label);
  filter_layout->addWidget(type_filter);
  filter_layout->addWidget(elite_only);
  filter_layout->addWidget(boss_only);
  filter_layout->addWidget(civilian_only);
  filter_layout->addWidget(trainer_only);
  filter_layout->addStretch();

  auto splitter = new QSplitter(Qt::Horizontal, container);

  // Search box now sits directly above the model tree (list) column at the list width.
  auto list_column = new QWidget(splitter);
  auto list_column_layout = new QVBoxLayout(list_column);
  list_column_layout->setContentsMargins(0, 0, 0, 0);
  list_column_layout->setSpacing(4);

  auto search_box = new QLineEdit(list_column);
  search_box->setPlaceholderText("Search by entry id or name...");
  search_box->setClearButtonEnabled(true);
  search_box->setToolTip("Filter the list by creature_template entry id or name (case-insensitive).");
  list_column_layout->addWidget(search_box);

  _creature_model_tree = new QTreeWidget(list_column);
  _creature_model_tree->setHeaderHidden(true);
  _creature_model_tree->setMinimumWidth(360);
  list_column_layout->addWidget(_creature_model_tree, 1);

  auto preview = new CreaturePreviewModelViewer(splitter);
  preview->setMinimumSize(360, 220);

  auto spawn_box = new QGroupBox("Edit/New Creature", splitter);
  _creature_spawn_box = spawn_box;
  auto spawn_layout = new QFormLayout(spawn_box);
  auto guid_field = new QLineEdit(spawn_box);
  auto entry_field = new QLineEdit(spawn_box);
  auto display_field = new QLineEdit(spawn_box);
  _creature_spawn_guid_field = guid_field;
  _creature_spawn_entry_field = entry_field;
  _creature_spawn_display_field = display_field;
  auto add_button = new QPushButton("Add Pending Spawn", spawn_box);
  add_button->setEnabled(false);
  guid_field->setPlaceholderText("GUID");
  entry_field->setPlaceholderText("Entry");
  display_field->setPlaceholderText("Display ID");
  spawn_layout->addRow("GUID:", guid_field);
  spawn_layout->addRow("Entry:", entry_field);
  spawn_layout->addRow("Display:", display_field);
  spawn_layout->addRow(add_button);
  splitter->addWidget(list_column);
  splitter->addWidget(filter_panel);
  splitter->addWidget(preview);
  splitter->addWidget(spawn_box);
  if (_creature_editor_panel)
  {
    splitter->addWidget(_creature_editor_panel);
  }
  splitter->setStretchFactor(0, 2);  // model tree
  splitter->setStretchFactor(1, 0);  // filter panel
  splitter->setStretchFactor(2, 3);  // preview
  splitter->setStretchFactor(3, 1);  // new spawn
  splitter->setStretchFactor(4, 1);  // coordinate editor
  splitter->setSizes({380, 130, 520, 240, 280});
  root_layout->addWidget(splitter, 1);

  _creature_model_picker_status = new QLabel("Loading creature_template entries...", container);
  _creature_model_picker_status->setWordWrap(true);
  root_layout->addWidget(_creature_model_picker_status);

  struct TemplatePickerEntry
  {
    std::uint32_t entry = 0;
    std::uint32_t faction = 0;
    std::uint32_t creature_type = 0;
    std::uint32_t rank = 0;
    std::uint32_t npc_flags = 0;
    std::uint32_t type_flags = 0;
    std::uint32_t flags_extra = 0;
    std::uint32_t display_id = 0;
    std::uint32_t model_id = 0;
    std::string name;
    std::string path;
    float template_scale = 1.0f;
    float model_scale = 1.0f;
  };

  auto normalize_picker_path = [](std::string path)
  {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c)
    {
      return static_cast<char>(std::tolower(c));
    });

    if (auto extension_pos = path.rfind(".mdx"); extension_pos != std::string::npos)
    {
      path.replace(extension_pos, 4, ".m2");
    }
    else if (auto extension_pos = path.rfind(".mdl"); extension_pos != std::string::npos)
    {
      path.replace(extension_pos, 4, ".m2");
    }
    else if (path.rfind('.') == std::string::npos)
    {
      path += ".m2";
    }
    return path;
  };

  // model_scale = CreatureModelData.ModelScale (M, intrinsic). display_scale_out =
  // CreatureDisplayInfo.CreatureModelScale (D), the object-scale fallback used when
  // creature_template.scale is 0 (server ObjectMgr.cpp:1436). Final render = (template.scale or D) * M.
  // D is a fallback, NOT an extra multiplier -- do not fold it into model_scale.
  auto resolve_display_model = [&](std::uint32_t display_id,
                                   std::uint32_t& model_id,
                                   std::string& model_path,
                                   float& model_scale,
                                   float& display_scale_out)
  {
    try
    {
      auto display = gCreatureDisplayInfoDB.getByID(display_id);
      model_id = display.getUInt(CreatureDisplayInfoDB::ModelID);
      auto model = gCreatureModelDataDB.getByID(model_id);
      model_path = normalize_picker_path(model.getString(CreatureModelDataDB::ModelName));
      float display_scale = display.getFloat(CreatureDisplayInfoDB::CreatureModelScale);
      float model_data_scale = model.getFloat(CreatureModelDataDB::ModelScale);
      display_scale_out = display_scale > 0.0f ? display_scale : 1.0f;
      model_scale = model_data_scale > 0.0f ? model_data_scale : 1.0f;
      return !model_path.empty();
    }
    catch (DBCFile::NotFound const&)
    {
      model_id = 0;
      model_path.clear();
      model_scale = 1.0f;
      display_scale_out = 1.0f;
      return false;
    }
  };

  auto template_entries = std::make_shared<std::vector<TemplatePickerEntry>>();

#ifdef USE_MYSQL_UID_STORAGE
  std::string template_error;
  auto records = mysql::getCreatureTemplates(25000, &template_error);
  template_entries->reserve(records.size());
  for (auto const& record : records)
  {
    TemplatePickerEntry entry;
    entry.entry = record.entry;
    entry.faction = record.faction;
    entry.creature_type = record.creature_type;
    entry.rank = record.rank;
    entry.npc_flags = record.npc_flags;
    entry.type_flags = record.type_flags;
    entry.flags_extra = record.flags_extra;
    entry.display_id = record.display_id;
    entry.name = record.name;
    entry.template_scale = record.template_scale;
    if (entry.display_id)
    {
      float display_scale = 1.0f;
      resolve_display_model(entry.display_id, entry.model_id, entry.path, entry.model_scale, display_scale);
      // creature_template.scale of 0 -> fall back to CreatureDisplayInfo scale D (server behavior).
      if (entry.template_scale <= 0.0f)
      {
        entry.template_scale = display_scale;
      }
    }
    if (entry.template_scale <= 0.0f)
    {
      entry.template_scale = 1.0f;
    }
    template_entries->push_back(std::move(entry));
  }
#else
  std::string template_error = "Build does not include MySQL support.";
#endif

  // Populate the type dropdown with every distinct creature type present in the loaded list.
  {
    std::set<std::uint32_t> distinct_types;
    for (auto const& entry : *template_entries)
    {
      distinct_types.insert(entry.creature_type);
    }
    for (auto const creature_type : distinct_types)  // std::set keeps them sorted
    {
      QString const label = creature_type
        ? QString("%1 (%2)").arg(creature_type_label(creature_type)).arg(creature_type)
        : QString("None (0)");
      type_filter->addItem(label, static_cast<qulonglong>(creature_type));
    }
  }

  // Share the loaded creature_template type/rank info with the current-map creature browser so it can
  // offer the same Type/Elite/Boss/Civilian/Trainer filter, and populate its Type combo identically.
  {
    _creature_template_filter_info.clear();
    for (auto const& entry : *template_entries)
    {
      _creature_template_filter_info[entry.entry] =
        CreatureFilterInfo{entry.creature_type, entry.rank, entry.type_flags, entry.flags_extra, entry.npc_flags};
    }
    if (_creature_browser_type_filter)
    {
      std::set<std::uint32_t> distinct_types;
      for (auto const& entry : *template_entries)
      {
        distinct_types.insert(entry.creature_type);
      }
      QSignalBlocker blocker(_creature_browser_type_filter);
      for (auto const creature_type : distinct_types)
      {
        QString const label = creature_type
          ? QString("%1 (%2)").arg(creature_type_label(creature_type)).arg(creature_type)
          : QString("None (0)");
        _creature_browser_type_filter->addItem(label, static_cast<qulonglong>(creature_type));
      }
    }
    rebuildCreatureBrowserList(true); // refresh now that the filter data is available
  }

  auto selected_template = std::make_shared<std::optional<TemplatePickerEntry>>();

  auto suggested_guid = [this]()
  {
    std::uint32_t highest = 0;
    for (auto const& spawn : _world->creatureSpawns())
    {
      highest = std::max(highest, spawn.guid);
    }
    return highest + 1;
  };

  auto passes_filters = [=](TemplatePickerEntry const& entry)
  {
    auto const needle = search_box->text().trimmed();
    if (!needle.isEmpty())
    {
      QString const name = QString::fromStdString(entry.name);
      QString const id = QString::number(entry.entry);
      if (!name.contains(needle, Qt::CaseInsensitive) && !id.contains(needle))
      {
        return false;
      }
    }
    auto const type_data = type_filter->currentData();
    if (type_data.isValid() && entry.creature_type != static_cast<std::uint32_t>(type_data.toULongLong()))
    {
      return false;
    }
    if (elite_only->isChecked() && !(entry.rank == 1u || entry.rank == 2u))
    {
      return false;
    }
    if (boss_only->isChecked() && entry.rank < 3u && (entry.type_flags & 0x4u) == 0u)
    {
      return false;
    }
    if (civilian_only->isChecked() && (entry.flags_extra & 0x2u) == 0u && (entry.type_flags & 0x80u) == 0u)
    {
      return false;
    }
    if (trainer_only->isChecked() && (entry.npc_flags & 0x10u) == 0u)
    {
      return false;
    }
    return true;
  };

  auto rebuild_template_tree = [=]()
  {
    _creature_model_tree->clear();
    std::size_t visible_count = 0;
    std::size_t previewable_count = 0;

    for (auto const& entry : *template_entries)
    {
      if (!passes_filters(entry))
      {
        continue;
      }

      ++visible_count;
      if (!entry.path.empty())
      {
        ++previewable_count;
      }

      auto* row = new QTreeWidgetItem(_creature_model_tree);
      row->setText(0, QString("%1 - %2").arg(entry.entry).arg(QString::fromStdString(entry.name)));
      row->setData(0, Qt::UserRole, static_cast<qulonglong>(entry.entry));
      row->setData(0, Qt::UserRole + 1, true);
      if (entry.path.empty())
      {
        row->setForeground(0, QColor(135, 135, 135));
      }
    }

    _creature_model_tree->sortItems(0, Qt::AscendingOrder);

    if (_creature_model_picker_status)
    {
      QString message = QString("%1 creature_template entr%2 shown, %3 previewable")
                          .arg(visible_count)
                          .arg(visible_count == 1 ? "y" : "ies")
                          .arg(previewable_count);
      if (!template_error.empty())
      {
        message = QString("Template load failed: %1").arg(QString::fromStdString(template_error));
      }
      _creature_model_picker_status->setText(message);
    }
  };

  auto update_add_button = [=]()
  {
    bool guid_ok = false;
    bool entry_ok = false;
    bool display_ok = false;
    guid_field->text().toUInt(&guid_ok);
    entry_field->text().toUInt(&entry_ok);
    display_field->text().toUInt(&display_ok);
    add_button->setEnabled(guid_ok && entry_ok && display_ok && selected_template && selected_template->has_value()
                           && !selected_template->value().path.empty());
  };

  auto select_template_entry = [=](std::uint32_t entry_id)
  {
    auto found = std::find_if(template_entries->begin(), template_entries->end(),
      [entry_id](TemplatePickerEntry const& entry)
      {
        return entry.entry == entry_id;
      });

    if (found == template_entries->end())
    {
      selected_template->reset();
      update_add_button();
      return;
    }

    *selected_template = *found;
    guid_field->setText(QString::number(suggested_guid()));
    entry_field->setText(QString::number(found->entry));
    display_field->setText(QString::number(found->display_id));

    if (!found->path.empty())
    {
      World::CreatureSpawnOverlay preview_spawn;
      preview_spawn.guid = suggested_guid();
      preview_spawn.entry = found->entry;
      preview_spawn.display_id = found->display_id;
      preview_spawn.name = found->name;
      preview_spawn.template_scale = found->template_scale;
      preview_spawn.model_scale = found->model_scale;
      preview_spawn.model_path = found->path;
      preview_spawn.is_character_model = preview_spawn.model_path.rfind("character/", 0) == 0;

      try
      {
        preview->setCreatureSpawnPreview(*_world, preview_spawn);
        _creature_model_picker_status->setText(QString("%1 | entry %2 | display %3 | %4")
                                                 .arg(QString::fromStdString(found->name))
                                                 .arg(found->entry)
                                                 .arg(found->display_id)
                                                 .arg(found->creature_type
                                                   ? creature_type_label(found->creature_type)
                                                   : QString("faction %1").arg(found->faction)));
      }
      catch (std::exception const& error)
      {
        _creature_model_picker_status->setText(QString("Preview failed for %1: %2")
                                                 .arg(QString::fromStdString(found->path))
                                                 .arg(error.what()));
      }
      catch (...)
      {
        _creature_model_picker_status->setText(QString("Preview failed for %1")
                                                 .arg(QString::fromStdString(found->path)));
      }
    }
    else
    {
      _creature_model_picker_status->setText(QString("%1 has no previewable CreatureDisplayInfo model")
                                               .arg(QString::fromStdString(found->name)));
    }

    update_add_button();
  };

  auto add_pending_spawn = [=]()
  {
    if (!selected_template || !selected_template->has_value())
    {
      _main_window->statusBar()->showMessage("Select a creature_template entry first", 5000);
      return;
    }

    bool guid_ok = false;
    bool entry_ok = false;
    bool display_ok = false;
    auto const guid = guid_field->text().toUInt(&guid_ok);
    auto const entry_id = entry_field->text().toUInt(&entry_ok);
    auto const display_id = display_field->text().toUInt(&display_ok);
    if (!guid_ok || !entry_ok || !display_ok || !guid || !entry_id || !display_id)
    {
      _main_window->statusBar()->showMessage("Enter a valid GUID, entry, and display ID", 5000);
      return;
    }

    if (_world->findCreatureSpawn(guid))
    {
      _main_window->statusBar()->showMessage(QString("Creature GUID %1 already exists in this overlay").arg(guid), 5000);
      return;
    }

    TemplatePickerEntry entry = selected_template->value();
    if (entry.display_id != display_id)
    {
      entry.display_id = display_id;
      float display_scale = 1.0f;
      resolve_display_model(entry.display_id, entry.model_id, entry.path, entry.model_scale, display_scale);
      if (entry.template_scale <= 0.0f)
      {
        entry.template_scale = display_scale;
      }
    }

    if (entry.path.empty())
    {
      _main_window->statusBar()->showMessage(QString("Creature template %1 has no previewable model").arg(entry_id), 5000);
      return;
    }

    glm::vec3 spawn_pos = _camera.position + _camera.direction() * 8.0f;
    if (spawn_pos.y < -5000.0f)
    {
      spawn_pos = _cursor_pos;
    }

    World::CreatureSpawnOverlay spawn;
    spawn.guid = guid;
    spawn.entry = entry_id;
    spawn.display_id = entry.display_id;
    spawn.name = entry.name.empty() ? "New pending NPC" : entry.name;
    spawn.pos = spawn_pos;
    spawn.original_pos = spawn.pos;
    spawn.orientation = _camera.yaw()._;
    spawn.original_orientation = spawn.orientation;
    spawn.animation_time_offset = static_cast<int>(((guid * 1103515245u) + (entry_id * 12345u)) % 3500u);
    spawn.template_scale = entry.template_scale;
    spawn.model_scale = entry.model_scale;
    spawn.model_path = entry.path;
    spawn.is_character_model = spawn.model_path.rfind("character/", 0) == 0;
    spawn.pending_create = true;
    spawn.dirty = true;
    spawn.selected = true;

    for (auto& existing_spawn : _world->creatureSpawns())
    {
      existing_spawn.selected = false;
    }

    _world->creatureSpawns().push_back(std::move(spawn));
    auto& added_spawn = _world->creatureSpawns().back();
    _world->ensureCreatureSpawnModel(added_spawn);

    _selected_creature_spawn_guid = guid;
    rebuildCreatureBrowserList(true);
    refreshCreatureEditorKnobs();
    updateDatabaseStatus();
    guid_field->setText(QString::number(suggested_guid()));
    _needs_redraw = true;
    _main_window->statusBar()->showMessage(QString("Pending creature spawn %1 added in front of camera").arg(guid), 5000);
  };

  connect(_creature_model_tree, &QTreeWidget::itemClicked,
          [=](QTreeWidgetItem* item, int)
          {
            if (!item || !item->data(0, Qt::UserRole + 1).toBool())
            {
              return;
            }
            select_template_entry(static_cast<std::uint32_t>(item->data(0, Qt::UserRole).toULongLong()));
          });

  connect(guid_field, &QLineEdit::textChanged, update_add_button);
  connect(entry_field, &QLineEdit::textChanged, update_add_button);
  connect(display_field, &QLineEdit::textChanged, update_add_button);
  connect(type_filter, qOverload<int>(&QComboBox::currentIndexChanged), rebuild_template_tree);
  connect(elite_only, &QCheckBox::stateChanged, rebuild_template_tree);
  connect(boss_only, &QCheckBox::stateChanged, rebuild_template_tree);
  connect(civilian_only, &QCheckBox::stateChanged, rebuild_template_tree);
  connect(trainer_only, &QCheckBox::stateChanged, rebuild_template_tree);
  connect(search_box, &QLineEdit::textChanged, rebuild_template_tree);
  connect(add_button, &QPushButton::clicked, add_pending_spawn);
  preview->on_double_click = add_pending_spawn;

  rebuild_template_tree();

  _creature_model_picker_dock->setWidget(container);
  _main_window->addDockWidget(Qt::BottomDockWidgetArea, _creature_model_picker_dock);
  _creature_model_picker_dock->setVisible(false);
  connect(this, &QObject::destroyed, _creature_model_picker_dock, &QObject::deleteLater);
}

void MapView::setupGameObjectEditorUi()
{
  // GameObjects have no model picker, so the coordinate editor panel is hosted inside the browser dock
  // (added below the list in setupGameObjectBrowserUi); we just build the panel widget here.
  auto container = new QWidget(this);
  container->setMinimumWidth(240);
  auto layout = new QVBoxLayout(container);
  layout->setContentsMargins(6, 6, 6, 6);

  auto coord_title = new QLabel("GameObject Coordinates", container);
  coord_title->setStyleSheet("font-weight: bold;");
  layout->addWidget(coord_title);

  auto hint = new QLabel("Left click: select or drag\nShift: add or box-select", container);
  hint->setWordWrap(true);
  layout->addWidget(hint);

  _gameobject_editor_info = new QLabel("No spawn selected", container);
  _gameobject_editor_info->setWordWrap(true);
  _gameobject_editor_info->setStyleSheet("font-style: italic; color: #888; padding: 2px 0;");
  layout->addWidget(_gameobject_editor_info);

  auto make_spin = [&](double lo, double hi, double step) {
    auto* sb = new QDoubleSpinBox(container);
    sb->setRange(lo, hi);
    sb->setDecimals(3);
    sb->setSingleStep(step);
    sb->setEnabled(false);
    return sb;
  };

  // Match the creature editor: client coordinates can exceed 20000, so use a wide range to avoid clamping.
  _go_spawn_edit_x           = make_spin(-64000.0, 64000.0, 0.1);
  _go_spawn_edit_y           = make_spin(-20000.0, 20000.0, 0.1);
  _go_spawn_edit_z           = make_spin(-64000.0, 64000.0, 0.1);
  _go_spawn_edit_orientation = make_spin(     0.0,   360.0, 1.0);
  _go_spawn_edit_orientation->setWrapping(true);
  _go_spawn_edit_orientation->setSuffix(QString::fromUtf8("\xc2\xb0"));  // ┬░

  auto form = new QFormLayout();
  form->setContentsMargins(0, 4, 0, 0);
  form->setSpacing(3);
  form->addRow("X:", _go_spawn_edit_x);
  form->addRow("Y (height):", _go_spawn_edit_y);
  form->addRow("Z:", _go_spawn_edit_z);
  form->addRow("Orientation:", _go_spawn_edit_orientation);
  layout->addLayout(form);

  auto on_change = [this](double) {
    if (!_selected_gameobject_spawn_guid)
      return;
    auto* spawn = _world->findGameObjectSpawn(*_selected_gameobject_spawn_guid);
    if (!spawn)
      return;

    spawn->pos = glm::vec3(
      static_cast<float>(_go_spawn_edit_x->value()),
      static_cast<float>(_go_spawn_edit_y->value()),
      static_cast<float>(_go_spawn_edit_z->value()));
    spawn->orientation = static_cast<float>(_go_spawn_edit_orientation->value());
    spawn->dirty = spawn->pending_create
                || glm::distance(spawn->pos, spawn->original_pos) > 0.01f
                || std::abs(spawn->orientation - spawn->original_orientation) > 0.01f;

    if (spawn->model_instance)
    {
      spawn->model_instance->pos = spawn->pos;
      spawn->model_instance->dir = glm::vec3(0.0f, spawn->orientation, 0.0f);
      spawn->model_instance->recalcExtents();
    }

    updateGameObjectBrowserStatus();
    rebuildGameObjectBrowserList(true);
    _needs_redraw = true;
  };

  connect(_go_spawn_edit_x,           qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);
  connect(_go_spawn_edit_y,           qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);
  connect(_go_spawn_edit_z,           qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);
  connect(_go_spawn_edit_orientation, qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);

  _gameobject_editor_panel = container;
}

void MapView::setupGameObjectBrowserUi()
{
  _gameobject_browser_dock = new QDockWidget("GameObject Browser", _main_window);
  _gameobject_browser_dock->setFeatures(QDockWidget::DockWidgetMovable
                                        | QDockWidget::DockWidgetFloatable
                                        | QDockWidget::DockWidgetClosable);
  _gameobject_browser_dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
  _gameobject_browser_dock->setMinimumWidth(330);
  _gameobject_browser_dock->resize(380, 520);
  _main_window->addDockWidget(Qt::RightDockWidgetArea, _gameobject_browser_dock);
  connect(this, &QObject::destroyed, _gameobject_browser_dock, &QObject::deleteLater);

  auto container = new QWidget(this);
  auto layout = new QVBoxLayout(container);
  layout->setContentsMargins(6, 6, 6, 6);

  _gameobject_search_field = new QLineEdit(container);
  _gameobject_search_field->setPlaceholderText("Search by gameobject name, guid, or entry");
  layout->addWidget(_gameobject_search_field);

  _gameobject_zone_filter = new QCheckBox("Zone only (current zone)", container);
  _gameobject_zone_filter->setToolTip("Only list gameobjects whose position is in the same zone as the"
                                      " camera. Spawns in unloaded tiles are excluded.");
  layout->addWidget(_gameobject_zone_filter);
  connect(_gameobject_zone_filter, &QCheckBox::toggled, [this]() { rebuildGameObjectBrowserList(true); });

  // Type filter mirroring the gameobject model picker. Filters the current-map spawn list by the
  // spawned object's template type (looked up per spawn entry from _gameobject_template_filter_type;
  // the combo is populated by the model picker when it loads gameobject_template).
  {
    auto type_row = new QHBoxLayout();
    type_row->addWidget(new QLabel("Type", container));
    _gameobject_browser_type_filter = new QComboBox(container);
    _gameobject_browser_type_filter->addItem("All types"); // index 0 -> no type filter
    _gameobject_browser_type_filter->setToolTip("Filter the spawn list by gameobject type.");
    type_row->addWidget(_gameobject_browser_type_filter, 1);
    layout->addLayout(type_row);
    connect(_gameobject_browser_type_filter, qOverload<int>(&QComboBox::currentIndexChanged),
            [this]() { rebuildGameObjectBrowserList(true); });
  }

  _gameobject_list_widget = new QListWidget(container);
  _gameobject_list_widget->setSelectionMode(QAbstractItemView::SingleSelection);
  _gameobject_list_widget->setMinimumHeight(280);
  layout->addWidget(_gameobject_list_widget, 1);

  _gameobject_browser_status = new QLabel(container);
  _gameobject_browser_status->setWordWrap(true);
  layout->addWidget(_gameobject_browser_status);

  // The coordinate editor panel now lives in the model picker (last splitter column),
  // mirroring the creature tool.

  _gameobject_browser_dock->setWidget(container);
  _gameobject_browser_dock->setVisible(false);

  connect(_gameobject_browser_dock, &QDockWidget::visibilityChanged,
          [this](bool visible)
          {
            if (ui_hidden)
              return;

            _settings->setValue("map_view/gameobject_browser", visible);
            _settings->sync();
          });

  connect(&_show_gameobject_browser, &Noggit::BoolToggleProperty::changed,
          [this](bool visible)
          {
            bool const show = visible && !ui_hidden && terrainMode == editing_mode::gameobject;
            if (ui_hidden && visible)
            {
              return;
            }

            if (_gameobject_actions_overlay)
            {
              _gameobject_actions_overlay->setVisible(show);
            }

            if (_gameobject_browser_dock)
            {
              _gameobject_browser_dock->setVisible(show);
            }

            if (_gameobject_model_picker_dock)
            {
              _gameobject_model_picker_dock->setVisible(show);
            }
          });
  connect(_gameobject_browser_dock, &QDockWidget::visibilityChanged,
          &_show_gameobject_browser, &Noggit::BoolToggleProperty::set);

  connect(_gameobject_search_field, &QLineEdit::textChanged,
          [this]()
          {
            rebuildGameObjectBrowserList(true);
          });
  connect(_gameobject_list_widget, &QListWidget::itemClicked,
          this, &MapView::jumpToGameObjectListItem);

  updateGameObjectBrowserStatus();
}

void MapView::setupGameObjectModelPickerUi()
{
  _gameobject_model_picker_dock = new QDockWidget("GameObject Model Picker", _main_window);
  _gameobject_model_picker_dock->setFeatures(QDockWidget::DockWidgetMovable
                                             | QDockWidget::DockWidgetFloatable
                                             | QDockWidget::DockWidgetClosable);
  _gameobject_model_picker_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  _gameobject_model_picker_dock->setMinimumHeight(220);

  auto container = new QWidget(this);
  auto root_layout = new QVBoxLayout(container);
  root_layout->setContentsMargins(6, 6, 6, 6);
  root_layout->setSpacing(6);

  // Gameobject "Type" label helper (mirrors creature_type_label, but local to the picker).
  auto gameobject_type_label = [](std::uint32_t type) -> QString
  {
    switch (type)
    {
      case 0:  return "Door";
      case 1:  return "Button";
      case 2:  return "Quest Giver";
      case 3:  return "Chest";
      case 4:  return "Binding";
      case 5:  return "Generic";
      case 6:  return "Trap";
      case 7:  return "Chair";
      case 8:  return "Spell Focus";
      case 9:  return "Text";
      case 10: return "Goober";
      case 11: return "Transport";
      case 12: return "Area Damage";
      case 13: return "Camera";
      case 14: return "Map Object";
      case 15: return "Mo Transport";
      case 17: return "Fishing Node";
      case 21: return "Door (summon)";
      case 22: return "Summoning Ritual";
      case 25: return "Auction House";
      case 26: return "Guard";
      default: return QString("Type %1").arg(type);
    }
  };

  // Filters live in a thin vertical panel beside the list (added to the splitter below).
  auto filter_panel = new QWidget(container);
  filter_panel->setMaximumWidth(150);
  auto filter_layout = new QVBoxLayout(filter_panel);
  filter_layout->setContentsMargins(4, 4, 4, 4);
  filter_layout->setSpacing(4);
  auto filter_title = new QLabel("Filters", filter_panel);
  filter_title->setStyleSheet("font-weight: bold;");
  filter_layout->addWidget(filter_title);
  auto type_label = new QLabel("Type", filter_panel);
  auto type_filter = new QComboBox(filter_panel);
  type_filter->setToolTip("Filter entries by gameobject type. Populated from the loaded gameobject_template list.");
  type_filter->addItem("All types");  // index 0: no data -> no type filter
  filter_layout->addWidget(type_label);
  filter_layout->addWidget(type_filter);
  filter_layout->addStretch();

  auto splitter = new QSplitter(Qt::Horizontal, container);

  // Search box sits directly above the model tree (list) column at the list width.
  auto list_column = new QWidget(splitter);
  auto list_column_layout = new QVBoxLayout(list_column);
  list_column_layout->setContentsMargins(0, 0, 0, 0);
  list_column_layout->setSpacing(4);

  auto search_box = new QLineEdit(list_column);
  search_box->setPlaceholderText("Search by entry id or name...");
  search_box->setClearButtonEnabled(true);
  search_box->setToolTip("Filter the list by gameobject_template entry id or name (case-insensitive).");
  list_column_layout->addWidget(search_box);

  _gameobject_model_tree = new QTreeWidget(list_column);
  _gameobject_model_tree->setHeaderHidden(true);
  _gameobject_model_tree->setMinimumWidth(360);
  list_column_layout->addWidget(_gameobject_model_tree, 1);

  auto preview = new CreaturePreviewModelViewer(splitter);
  preview->setMinimumSize(360, 220);

  auto spawn_box = new QGroupBox("Edit/New GameObject", splitter);
  _gameobject_spawn_box = spawn_box;
  auto spawn_layout = new QFormLayout(spawn_box);
  auto guid_field = new QLineEdit(spawn_box);
  auto entry_field = new QLineEdit(spawn_box);
  auto display_field = new QLineEdit(spawn_box);
  _gameobject_spawn_guid_field = guid_field;
  _gameobject_spawn_entry_field = entry_field;
  _gameobject_spawn_display_field = display_field;
  auto add_button = new QPushButton("Add Pending Spawn", spawn_box);
  add_button->setEnabled(false);
  guid_field->setPlaceholderText("GUID");
  entry_field->setPlaceholderText("Entry");
  display_field->setPlaceholderText("Display ID");
  spawn_layout->addRow("GUID:", guid_field);
  spawn_layout->addRow("Entry:", entry_field);
  spawn_layout->addRow("Display:", display_field);
  spawn_layout->addRow(add_button);

  splitter->addWidget(list_column);
  splitter->addWidget(filter_panel);
  splitter->addWidget(preview);
  splitter->addWidget(spawn_box);
  if (_gameobject_editor_panel)
  {
    splitter->addWidget(_gameobject_editor_panel);
  }
  splitter->setStretchFactor(0, 2);  // model tree
  splitter->setStretchFactor(1, 0);  // filter panel
  splitter->setStretchFactor(2, 3);  // preview
  splitter->setStretchFactor(3, 1);  // new spawn
  splitter->setStretchFactor(4, 1);  // coordinate editor
  splitter->setSizes({380, 130, 520, 240, 280});
  root_layout->addWidget(splitter, 1);

  _gameobject_model_picker_status = new QLabel("Loading gameobject_template entries...", container);
  _gameobject_model_picker_status->setWordWrap(true);
  root_layout->addWidget(_gameobject_model_picker_status);

  struct TemplatePickerEntry
  {
    std::uint32_t entry = 0;
    std::uint32_t type = 0;
    std::uint32_t display_id = 0;
    std::string name;
    std::string path;
    float template_scale = 1.0f;
  };

  auto normalize_picker_path = [](std::string path)
  {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c)
    {
      return static_cast<char>(std::tolower(c));
    });

    if (auto extension_pos = path.rfind(".mdx"); extension_pos != std::string::npos)
    {
      path.replace(extension_pos, 4, ".m2");
    }
    else if (auto extension_pos = path.rfind(".mdl"); extension_pos != std::string::npos)
    {
      path.replace(extension_pos, 4, ".m2");
    }
    else if (path.rfind('.') == std::string::npos)
    {
      path += ".m2";
    }
    return path;
  };

  auto resolve_display_model = [&](std::uint32_t display_id, std::string& model_path)
  {
    try
    {
      auto display = gGameObjectDisplayInfoDB.getByID(display_id);
      model_path = normalize_picker_path(display.getString(GameObjectDisplayInfoDB::ModelName));
      return !model_path.empty();
    }
    catch (DBCFile::NotFound const&)
    {
      model_path.clear();
      return false;
    }
  };

  auto template_entries = std::make_shared<std::vector<TemplatePickerEntry>>();

#ifdef USE_MYSQL_UID_STORAGE
  std::string template_error;
  auto records = mysql::getGameObjectTemplates(25000, &template_error);
  template_entries->reserve(records.size());
  for (auto const& record : records)
  {
    TemplatePickerEntry entry;
    entry.entry = record.entry;
    entry.type = record.type;
    entry.display_id = record.display_id;
    entry.name = record.name;
    entry.template_scale = record.template_scale;
    if (entry.display_id)
    {
      resolve_display_model(entry.display_id, entry.path);
    }
    template_entries->push_back(std::move(entry));
  }
#else
  std::string template_error = "Build does not include MySQL support.";
#endif

  // Populate the type dropdown with every distinct gameobject type present in the loaded list.
  {
    std::set<std::uint32_t> distinct_types;
    for (auto const& entry : *template_entries)
    {
      distinct_types.insert(entry.type);
    }
    for (auto const type : distinct_types)  // std::set keeps them sorted
    {
      type_filter->addItem(QString("%1 (%2)").arg(gameobject_type_label(type)).arg(type),
                           static_cast<qulonglong>(type));
    }
  }

  // Share the gameobject template type info with the current-map browser so it can offer the same Type
  // filter, and populate its Type combo identically.
  {
    _gameobject_template_filter_type.clear();
    for (auto const& entry : *template_entries)
    {
      _gameobject_template_filter_type[entry.entry] = entry.type;
    }
    if (_gameobject_browser_type_filter)
    {
      std::set<std::uint32_t> distinct_types;
      for (auto const& entry : *template_entries)
      {
        distinct_types.insert(entry.type);
      }
      QSignalBlocker blocker(_gameobject_browser_type_filter);
      for (auto const type : distinct_types)
      {
        _gameobject_browser_type_filter->addItem(
          QString("%1 (%2)").arg(gameobject_type_label(type)).arg(type), static_cast<qulonglong>(type));
      }
    }
    rebuildGameObjectBrowserList(true);
  }

  auto selected_template = std::make_shared<std::optional<TemplatePickerEntry>>();

  auto suggested_guid = [this]()
  {
    std::uint32_t highest = 0;
    for (auto const& spawn : _world->gameObjectSpawns())
    {
      highest = std::max(highest, spawn.guid);
    }
    return highest + 1;
  };

  auto passes_filters = [=](TemplatePickerEntry const& entry)
  {
    auto const needle = search_box->text().trimmed();
    if (!needle.isEmpty())
    {
      QString const name = QString::fromStdString(entry.name);
      QString const id = QString::number(entry.entry);
      if (!name.contains(needle, Qt::CaseInsensitive) && !id.contains(needle))
      {
        return false;
      }
    }
    auto const type_data = type_filter->currentData();
    if (type_data.isValid() && entry.type != static_cast<std::uint32_t>(type_data.toULongLong()))
    {
      return false;
    }
    return true;
  };

  auto rebuild_template_tree = [=]()
  {
    _gameobject_model_tree->clear();
    std::size_t visible_count = 0;
    std::size_t previewable_count = 0;

    for (auto const& entry : *template_entries)
    {
      if (!passes_filters(entry))
      {
        continue;
      }

      ++visible_count;
      if (!entry.path.empty())
      {
        ++previewable_count;
      }

      auto* row = new QTreeWidgetItem(_gameobject_model_tree);
      row->setText(0, QString("%1 - %2").arg(entry.entry).arg(QString::fromStdString(entry.name)));
      row->setData(0, Qt::UserRole, static_cast<qulonglong>(entry.entry));
      row->setData(0, Qt::UserRole + 1, true);
      if (entry.path.empty())
      {
        row->setForeground(0, QColor(135, 135, 135));
      }
    }

    _gameobject_model_tree->sortItems(0, Qt::AscendingOrder);

    if (_gameobject_model_picker_status)
    {
      QString message = QString("%1 gameobject_template entr%2 shown, %3 previewable")
                          .arg(visible_count)
                          .arg(visible_count == 1 ? "y" : "ies")
                          .arg(previewable_count);
      if (!template_error.empty())
      {
        message = QString("Template load failed: %1").arg(QString::fromStdString(template_error));
      }
      _gameobject_model_picker_status->setText(message);
    }
  };

  auto update_add_button = [=]()
  {
    bool guid_ok = false;
    bool entry_ok = false;
    bool display_ok = false;
    guid_field->text().toUInt(&guid_ok);
    entry_field->text().toUInt(&entry_ok);
    display_field->text().toUInt(&display_ok);
    add_button->setEnabled(guid_ok && entry_ok && display_ok && selected_template && selected_template->has_value()
                           && !selected_template->value().path.empty());
  };

  auto select_template_entry = [=](std::uint32_t entry_id)
  {
    auto found = std::find_if(template_entries->begin(), template_entries->end(),
      [entry_id](TemplatePickerEntry const& entry)
      {
        return entry.entry == entry_id;
      });

    if (found == template_entries->end())
    {
      selected_template->reset();
      update_add_button();
      return;
    }

    *selected_template = *found;
    guid_field->setText(QString::number(suggested_guid()));
    entry_field->setText(QString::number(found->entry));
    display_field->setText(QString::number(found->display_id));

    if (!found->path.empty())
    {
      World::CreatureSpawnOverlay preview_spawn;
      preview_spawn.guid = suggested_guid();
      preview_spawn.entry = found->entry;
      preview_spawn.display_id = found->display_id;
      preview_spawn.name = found->name;
      preview_spawn.template_scale = found->template_scale;
      preview_spawn.model_scale = 1.0f;
      preview_spawn.model_path = found->path;
      preview_spawn.is_character_model = false;

      try
      {
        preview->setCreatureSpawnPreview(*_world, preview_spawn);
        _gameobject_model_picker_status->setText(QString("%1 | entry %2 | display %3 | %4")
                                                 .arg(QString::fromStdString(found->name))
                                                 .arg(found->entry)
                                                 .arg(found->display_id)
                                                 .arg(gameobject_type_label(found->type)));
      }
      catch (std::exception const& error)
      {
        _gameobject_model_picker_status->setText(QString("Preview failed for %1: %2")
                                                 .arg(QString::fromStdString(found->path))
                                                 .arg(error.what()));
      }
      catch (...)
      {
        _gameobject_model_picker_status->setText(QString("Preview failed for %1")
                                                 .arg(QString::fromStdString(found->path)));
      }
    }
    else
    {
      _gameobject_model_picker_status->setText(QString("%1 has no previewable GameObjectDisplayInfo model")
                                               .arg(QString::fromStdString(found->name)));
    }

    update_add_button();
  };

  auto add_pending_spawn = [=]()
  {
    if (!selected_template || !selected_template->has_value())
    {
      _main_window->statusBar()->showMessage("Select a gameobject_template entry first", 5000);
      return;
    }

    bool guid_ok = false;
    bool entry_ok = false;
    bool display_ok = false;
    auto const guid = guid_field->text().toUInt(&guid_ok);
    auto const entry_id = entry_field->text().toUInt(&entry_ok);
    auto const display_id = display_field->text().toUInt(&display_ok);
    if (!guid_ok || !entry_ok || !display_ok || !guid || !entry_id || !display_id)
    {
      _main_window->statusBar()->showMessage("Enter a valid GUID, entry, and display ID", 5000);
      return;
    }

    if (_world->findGameObjectSpawn(guid))
    {
      _main_window->statusBar()->showMessage(QString("GameObject GUID %1 already exists in this overlay").arg(guid), 5000);
      return;
    }

    TemplatePickerEntry entry = selected_template->value();
    if (entry.display_id != display_id)
    {
      entry.display_id = display_id;
      resolve_display_model(entry.display_id, entry.path);
    }

    if (entry.path.empty())
    {
      _main_window->statusBar()->showMessage(QString("GameObject template %1 has no previewable model").arg(entry_id), 5000);
      return;
    }

    glm::vec3 spawn_pos = _camera.position + _camera.direction() * 8.0f;
    if (spawn_pos.y < -5000.0f)
    {
      spawn_pos = _cursor_pos;
    }

    World::GameObjectSpawnOverlay spawn;
    spawn.guid = guid;
    spawn.entry = entry_id;
    spawn.display_id = entry.display_id;
    spawn.name = entry.name.empty() ? "New pending gameobject" : entry.name;
    spawn.pos = spawn_pos;
    spawn.original_pos = spawn.pos;
    spawn.orientation = _camera.yaw()._;
    spawn.original_orientation = spawn.orientation;
    spawn.template_scale = entry.template_scale;
    spawn.model_path = entry.path;
    spawn.pending_create = true;
    spawn.dirty = true;
    spawn.selected = true;

    for (auto& existing_spawn : _world->gameObjectSpawns())
    {
      existing_spawn.selected = false;
    }

    _world->gameObjectSpawns().push_back(std::move(spawn));
    auto& added_spawn = _world->gameObjectSpawns().back();
    _world->ensureGameObjectSpawnModel(added_spawn);

    _selected_gameobject_spawn_guid = guid;
    rebuildGameObjectBrowserList(true);
    refreshGameObjectEditorKnobs();
    updateGameObjectBrowserStatus();
    guid_field->setText(QString::number(suggested_guid()));
    _needs_redraw = true;
    _main_window->statusBar()->showMessage(QString("Pending gameobject spawn %1 added in front of camera").arg(guid), 5000);
  };

  connect(_gameobject_model_tree, &QTreeWidget::itemClicked,
          [=](QTreeWidgetItem* item, int)
          {
            if (!item || !item->data(0, Qt::UserRole + 1).toBool())
            {
              return;
            }
            select_template_entry(static_cast<std::uint32_t>(item->data(0, Qt::UserRole).toULongLong()));
          });

  connect(guid_field, &QLineEdit::textChanged, update_add_button);
  connect(entry_field, &QLineEdit::textChanged, update_add_button);
  connect(display_field, &QLineEdit::textChanged, update_add_button);
  connect(type_filter, qOverload<int>(&QComboBox::currentIndexChanged), rebuild_template_tree);
  connect(search_box, &QLineEdit::textChanged, rebuild_template_tree);
  connect(add_button, &QPushButton::clicked, add_pending_spawn);
  preview->on_double_click = add_pending_spawn;

  rebuild_template_tree();

  _gameobject_model_picker_dock->setWidget(container);
  _main_window->addDockWidget(Qt::BottomDockWidgetArea, _gameobject_model_picker_dock);
  _gameobject_model_picker_dock->setVisible(false);
  connect(this, &QObject::destroyed, _gameobject_model_picker_dock, &QObject::deleteLater);
}

void MapView::setupGameObjectActionsUi()
{
  if (!_overlay_widget)
  {
    return;
  }

  _gameobject_actions_overlay = new QWidget(_overlay_widget);
  _gameobject_actions_overlay->setObjectName("gameobjectActionsOverlay");
  _gameobject_actions_overlay->setAttribute(Qt::WA_StyledBackground, true);
  _gameobject_actions_overlay->setStyleSheet(
    "#gameobjectActionsOverlay { background: rgba(28, 31, 37, 210); border: 1px solid rgba(85, 91, 103, 180); }"
    "#gameobjectActionsOverlay QPushButton { padding: 5px 9px; }");

  auto layout = new QHBoxLayout(_gameobject_actions_overlay);
  layout->setContentsMargins(5, 5, 5, 5);
  layout->setSpacing(5);

  auto reload_button = new QPushButton("Reload Spawns", _gameobject_actions_overlay);
  auto save_button = new QPushButton("Export SQL", _gameobject_actions_overlay);
  auto revert_button = new QPushButton("Discard Pending", _gameobject_actions_overlay);
  auto pending_button = new QPushButton("Pending \xE2\x96\xBE", _gameobject_actions_overlay);
  pending_button->setToolTip("Show the list of pending gameobject updates waiting for SQL export.");
  layout->addWidget(reload_button);
  layout->addWidget(save_button);
  layout->addWidget(revert_button);
  layout->addWidget(pending_button);

  // Toggleable dropdown listing every pending change (moved / deleted) awaiting SQL export.
  _gameobject_pending_popup = new QWidget(this, Qt::Popup);
  _gameobject_pending_popup->setObjectName("gameobjectPendingPopup");
  _gameobject_pending_popup->setAttribute(Qt::WA_StyledBackground, true);
  _gameobject_pending_popup->setStyleSheet(
    "#gameobjectPendingPopup { background: rgba(28, 31, 37, 235); border: 1px solid rgba(85, 91, 103, 200); }");
  auto pending_layout = new QVBoxLayout(_gameobject_pending_popup);
  pending_layout->setContentsMargins(6, 6, 6, 6);
  pending_layout->setSpacing(4);
  auto pending_title = new QLabel("Pending gameobject updates", _gameobject_pending_popup);
  pending_title->setStyleSheet("font-weight: bold; color: #ddd;");
  pending_layout->addWidget(pending_title);
  _gameobject_pending_list = new QListWidget(_gameobject_pending_popup);
  _gameobject_pending_list->setMinimumSize(360, 220);
  _gameobject_pending_list->setSelectionMode(QListWidget::NoSelection);
  pending_layout->addWidget(_gameobject_pending_list);

  auto refresh_pending = [this]()
  {
    if (!_gameobject_pending_list)
    {
      return;
    }
    _gameobject_pending_list->clear();
    int count = 0;
    for (auto const& spawn : _world->gameObjectSpawns())
    {
      if (!spawn.dirty)
      {
        continue;
      }
      QString action = spawn.pending_delete ? "DELETE" : "MOVE";
      QString const name = QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name);
      _gameobject_pending_list->addItem(
        QString("[%1] guid %2  entry %3  %4").arg(action).arg(spawn.guid).arg(spawn.entry).arg(name));
      ++count;
    }
    if (count == 0)
    {
      _gameobject_pending_list->addItem("No pending updates.");
    }
  };

  connect(pending_button, &QPushButton::clicked,
          [this, pending_button, refresh_pending]()
          {
            if (!_gameobject_pending_popup)
            {
              return;
            }
            if (_gameobject_pending_popup->isVisible())
            {
              _gameobject_pending_popup->hide();
              return;
            }
            refresh_pending();
            _gameobject_pending_popup->adjustSize();
            QPoint const below = pending_button->mapToGlobal(QPoint(0, pending_button->height() + 2));
            _gameobject_pending_popup->move(below);
            _gameobject_pending_popup->show();
          });

  auto place_actions = [this]()
  {
    if (!_gameobject_actions_overlay)
    {
      return;
    }

    _gameobject_actions_overlay->adjustSize();
    _gameobject_actions_overlay->move(std::max(8, width() - _gameobject_actions_overlay->width() - 14), 8);
    _gameobject_actions_overlay->raise();
  };

  _gameobject_actions_overlay->setVisible(false);
  place_actions();
  connect(this, &MapView::resized, this, place_actions);
  connect(this, &QObject::destroyed, _gameobject_actions_overlay, &QObject::deleteLater);

  connect(reload_button, &QPushButton::clicked,
          [this]()
          {
            _world->ensureGameObjectSpawnsLoaded();
            rebuildGameObjectBrowserList(true);
          });
  connect(save_button, &QPushButton::clicked,
          [this]()
          {
            saveDirtyGameObjectSpawns();
          });
  connect(revert_button, &QPushButton::clicked,
          [this]()
          {
            discardPendingGameObjectSpawns();
          });
}

QToolButton* MapView::makeSeasonalEventsToolButton(QWidget* parent)
{
  // Calendar section button hosted under the time-globe popup's slider. Its menu of checkable game-events
  // (checked = that event's creatures/objects render; nothing = base world only, the default) flies out to
  // the RIGHT of the popup instead of dropping down, so it expands the panel sideways. Rebuilt on open so
  // it always reflects the events in the loaded spawns. Affects both creature AND gameobject spawns.
  auto button = new QToolButton(parent);
  button->setIcon(make_calendar_icon());
  button->setIconSize(QSize(18, 18));
  button->setText("Seasonal Events");
  button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  button->setToolTip("Toggle which event's creatures and objects are shown.\n"
                     "Nothing checked = base world only.");
  button->setAutoRaise(true);

  // Not setMenu() (that drops DOWN): pop the menu manually anchored to the host popup's top-right corner so
  // it opens to the right, like a submenu flyout.
  auto menu = new MultiToggleMenu(button);
  connect(menu, &QMenu::aboutToShow, [this, menu]() { populateSeasonalEventsMenu(menu); });
  connect(button, &QToolButton::clicked, [button, menu]()
  {
    if (menu->isVisible())
    {
      menu->hide();
      return;
    }
    QWidget* const host = button->parentWidget() ? button->parentWidget() : button;
    QPoint const anchor = host->mapToGlobal(QPoint(host->width() + 2, 0));
    menu->popup(anchor);
  });
  return button;
}

void MapView::populateSeasonalEventsMenu(QMenu* menu)
{
  if (!menu)
  {
    return;
  }

  menu->clear();

  auto const entries = _world->spawnedEventEntries();
  if (entries.empty())
  {
    auto* empty = menu->addAction(_world->hasCreatureSpawnsLoaded()
                                    ? "No seasonal-event spawns on this map"
                                    : "Enable creature/object spawns first");
    empty->setEnabled(false);
    return;
  }

  auto* all_action = menu->addAction("Show all events");
  auto* none_action = menu->addAction("Hide all (base world only)");
  connect(all_action, &QAction::triggered, [this]()
  {
    for (auto entry : _world->spawnedEventEntries())
    {
      _world->setEventActive(entry, true);
    }
    update();
  });
  connect(none_action, &QAction::triggered, [this]()
  {
    _world->clearActiveEvents();
    update();
  });
  menu->addSeparator();

  auto const& names = _world->gameEventNames();
  for (std::int32_t entry : entries)
  {
    auto const name_it = names.find(entry);
    QString const label = name_it != names.end() && !name_it->second.empty()
      ? QString("%1 - %2").arg(entry).arg(QString::fromStdString(name_it->second))
      : QString("Event %1").arg(entry);

    auto* action = menu->addAction(label);
    action->setCheckable(true);
    action->setChecked(_world->isEventActive(entry));
    connect(action, &QAction::toggled, [this, entry](bool checked)
    {
      _world->setEventActive(entry, checked);
      update();
    });
  }
}

void MapView::setupMinimapEditorUi()
{
  minimapTool = new Noggit::Ui::MinimapCreator(this, _world.get(), this);
  _tool_panel_dock->registerTool("Minimap Editor", minimapTool);
}
void MapView::setupStampUi()
{
  stampTool = new Noggit::Ui::Tools::BrushStack(this, this);
  _tool_panel_dock->registerTool("Stamp", stampTool);
}

void MapView::setupLightEditorUi()
{
  lightEditor = new Noggit::Ui::Tools::LightEditor(this, this);
  _tool_panel_dock->registerTool("Light Editor", lightEditor);
}

void MapView::setupChunkManipulatorUi()
{
  _chunk_manipulator = new Noggit::Ui::Tools::ChunkManipulator::ChunkManipulatorPanel(this, this);
  _tool_panel_dock->registerTool("Chunk Manipulator", _chunk_manipulator);
}

void MapView::setupNodeEditor()
{
  _node_editor_dock = new QDockWidget("Node editor", this);
  _node_editor_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea | Qt::LeftDockWidgetArea);

  _main_window->addDockWidget(Qt::LeftDockWidgetArea, _node_editor_dock);
  _node_editor_dock->setFeatures(QDockWidget::DockWidgetMovable
                                 | QDockWidget::DockWidgetFloatable
                                 | QDockWidget::DockWidgetClosable);

  // QtNodes startup can still abort the process during style initialization.
  // Keep the dock shell, but do not restore/create the widget automatically.
  auto ensure_node_editor = [this]()
  {
    if (_node_editor_dock->widget())
    {
      return;
    }

    try
    {
      auto node_editor = new Noggit::Ui::Tools::NodeEditor::Ui::NodeEditorWidget(this);
      _node_editor_dock->setWidget(node_editor);
    }
    catch (...)
    {
      LogError << "Failed to initialize node editor dock during map view setup; hiding dock to keep map load alive."
               << std::endl;
      _settings->setValue("map_view/node_editor", false);
      _settings->sync();
      _node_editor_dock->hide();
    }
  };

  _settings->setValue("map_view/node_editor", false);
  _settings->sync();
  _node_editor_dock->hide();

  connect(_node_editor_dock, &QDockWidget::visibilityChanged,
          [=](bool visible)
          {
            if (visible)
            {
              _node_editor_dock->hide();
              return;
            }

            if (ui_hidden)
              return;

            _settings->setValue ("map_view/node_editor", visible);
            _settings->sync();
          });

  connect(this, &QObject::destroyed, _node_editor_dock, &QObject::deleteLater);

  connect ( &_show_node_editor, &Noggit::BoolToggleProperty::changed
    , _node_editor_dock, [this]
            {
              if (!ui_hidden)
                _node_editor_dock->setVisible(_show_node_editor.get());
            }
  );

  connect ( _node_editor_dock, &QDockWidget::visibilityChanged
    , &_show_node_editor, &Noggit::BoolToggleProperty::set
  );

}

void MapView::setupAssetBrowser()
{
  _asset_browser_dock = new QDockWidget("Asset browser", this);
  _asset_browser = new Noggit::Ui::Tools::AssetBrowser::Ui::AssetBrowserWidget(this, this);

  //_main_window->addDockWidget(Qt::BottomDockWidgetArea, _asset_browser_dock);
  _asset_browser_dock->setFeatures(QDockWidget::DockWidgetMovable
                                   | QDockWidget::DockWidgetFloatable
                                   | QDockWidget::DockWidgetClosable);
  _asset_browser_dock->setAllowedAreas(Qt::NoDockWidgetArea);

  _asset_browser_dock->setFloating(true);
  _asset_browser_dock->hide();

  _asset_browser_dock->setWidget(_asset_browser);
  _asset_browser_dock->setWindowFlags(
    Qt::CustomizeWindowHint |
    Qt::Window | 
    Qt::WindowMinimizeButtonHint |
    Qt::WindowMaximizeButtonHint |
    Qt::WindowCloseButtonHint | 
    Qt::WindowStaysOnTopHint);

  connect(_asset_browser_dock, &QDockWidget::visibilityChanged,
          [=](bool visible)
          {
            if (ui_hidden)
              return;

            _settings->setValue ("map_view/asset_browser", visible);
            _settings->sync();
          });;

  connect(this, &QObject::destroyed, _asset_browser_dock, &QObject::deleteLater);

}

void MapView::setupDetailInfos()
{

  // Dock
  _detail_infos_dock = new QDockWidget("Detail info", this);
  _detail_infos_dock->setFeatures(QDockWidget::DockWidgetMovable
                                  | QDockWidget::DockWidgetFloatable
                                  | QDockWidget::DockWidgetClosable);

  _detail_infos_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea | Qt::LeftDockWidgetArea);


  _main_window->addDockWidget(Qt::BottomDockWidgetArea, _detail_infos_dock);
  _detail_infos_dock->setFloating(true);
  _detail_infos_dock->hide();
  // End Dock

  guidetailInfos = new Noggit::Ui::detail_infos(this);
  _detail_infos_dock->setWidget(guidetailInfos);;


  connect ( &_show_detail_info_window, &Noggit::BoolToggleProperty::changed
    , guidetailInfos, [this]
            {
              if (!ui_hidden)
                _detail_infos_dock->setVisible(_show_detail_info_window.get());
            }
  );

  connect ( guidetailInfos, &Noggit::Ui::widget::visibilityChanged
    , &_show_detail_info_window, &Noggit::BoolToggleProperty::set
  );

  connect(NOGGIT_ACTION_MGR, &Noggit::ActionManager::onActionBegin,
    [this](Noggit::Action*)
    {
      updateDetailInfos(true);
    });

  connect(NOGGIT_ACTION_MGR, &Noggit::ActionManager::onActionEnd,
    [this](Noggit::Action*)
    {
      updateDetailInfos(true);
    });

  connect(NOGGIT_ACTION_MGR, &Noggit::ActionManager::currentActionChanged,
    [this](unsigned)
    {
      updateDetailInfos(true);
    });
}

void MapView::updateDetailInfos(bool no_sel_change_check)
{
  auto& current_selection = _world->current_selection();

  // update detail infos TODO: selection update signal.
  static std::uintptr_t last_sel = 0;

  if (guidetailInfos->isVisible())
  {
    if (current_selection.size() > 0)
    {
      selection_type& last_selection = const_cast<selection_type&>(current_selection.at(current_selection.size() - 1));

      switch (last_selection.index())
      {
        case eEntry_Object:
        {
          auto obj = std::get<selected_object_type>(last_selection);

          if (no_sel_change_check || reinterpret_cast<std::uintptr_t>(obj) != last_sel || NOGGIT_CUR_ACTION)
          {
            last_sel = reinterpret_cast<std::uintptr_t>(obj);
            obj->updateDetails(guidetailInfos);
          }
          break;
        }
        case eEntry_MapChunk:
        {
          selected_chunk_type& chunk_sel(std::get<selected_chunk_type>(last_selection));

          if (no_sel_change_check || reinterpret_cast<std::uintptr_t>(chunk_sel.chunk) != last_sel || NOGGIT_CUR_ACTION)
          {
            last_sel = reinterpret_cast<std::uintptr_t>(chunk_sel.chunk);
            chunk_sel.updateDetails(guidetailInfos);
          }
          break;
        }
      }
    }
    else
    {
      guidetailInfos->setText("");
    }
  }
}

void MapView::setupToolbars()
{
  _toolbar = new Noggit::Ui::toolbar([this] (editing_mode mode) { set_editing_mode (mode); });
  _toolbar->setOrientation(Qt::Vertical);
  auto right_toolbar_layout = new QVBoxLayout(_viewport_overlay_ui->leftToolbarHolder);
  right_toolbar_layout->addWidget( _toolbar);
  right_toolbar_layout->setDirection(QBoxLayout::LeftToRight);
  right_toolbar_layout->setContentsMargins(0, 5, 0, 5);
  connect (this, &QObject::destroyed, _toolbar, &QObject::deleteLater);

  auto left_sec_toolbar_layout = new QVBoxLayout(_viewport_overlay_ui->leftSecondaryToolbarHolder);
  left_sec_toolbar_layout->setContentsMargins(5, 0, 5, 0);

  _left_sec_toolbar = new Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar(this, terrainMode);
  connect(this, &QObject::destroyed, _left_sec_toolbar, &QObject::deleteLater);
  left_sec_toolbar_layout->addWidget( _left_sec_toolbar);

  auto top_toolbar_layout = new QVBoxLayout(_viewport_overlay_ui->upperToolbarHolder);
  top_toolbar_layout->setContentsMargins(5, 0, 5, 0);
  auto sec_toolbar_layout = new QVBoxLayout(_viewport_overlay_ui->secondaryToolbarHolder);
  sec_toolbar_layout->setContentsMargins(5, 0, 5, 0);

  _viewport_overlay_ui->secondaryToolbarHolder->hide();
  _secondary_toolbar = new Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar(this);
  connect (this, &QObject::destroyed, _secondary_toolbar, &QObject::deleteLater);

  _view_toolbar = new Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar(this, _secondary_toolbar);
  connect (this, &QObject::destroyed, _view_toolbar, &QObject::deleteLater);

  top_toolbar_layout->addWidget( _view_toolbar);
  sec_toolbar_layout->addWidget( _secondary_toolbar);

  // WC3-style time-of-day globe: its own widget, NEVER inside a button layout (so it can't stretch the
  // toolbars to its height). The icon strip is split into a left half (_view_toolbar) and a right half
  // (right_toolbar); the globe sits between them and all three are laid out as siblings, vertically
  // centred, so the button strips keep their normal small height and only the globe is tall.
  _time_globe = new Noggit::Ui::TimeGlobeWidget(this);
  _time_globe->setFixedSize(_time_globe->sizeHint());

  auto* right_toolbar = new QToolBar(_overlay_widget);
  right_toolbar->setMovable(false);
  right_toolbar->setContextMenuPolicy(Qt::PreventContextMenu);
  right_toolbar->setIconSize(_view_toolbar->iconSize());
  right_toolbar->setStyleSheet(_view_toolbar->styleSheet());
  right_toolbar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
  right_toolbar->setContentsMargins(0, 0, 0, 0);
  // no padding between the button strips and the globe
  _view_toolbar->setContentsMargins(0, 0, 0, 0);
  top_toolbar_layout->setContentsMargins(0, 0, 0, 0);
  {
    auto const acts = _view_toolbar->actions();
    int const half = (static_cast<int>(acts.size()) + 1) / 2;
    for (int i = half; i < acts.size(); ++i)
    {
      QAction* const a = acts.at(i);
      _view_toolbar->removeAction(a);
      right_toolbar->addAction(a);
    }
  }
  // Hover tool-options (secondaryToolbarHolder, shown via getSecondaryToolBar()) stay in the row BELOW the
  // globe so they never shove it when they pop up on hover.
  _viewport_overlay_ui->horizontalLayout_4->removeWidget(_viewport_overlay_ui->secondaryToolbarHolder);
  if (auto* below = _viewport_overlay_ui->horizontalLayout_8)
    below->insertWidget(0, _viewport_overlay_ui->secondaryToolbarHolder, 0, Qt::AlignTop);

  // The MODE secondary tools (Patrol paths / Creature info in creature mode; flatten & texture options in
  // the terrain modes -> getLeftSecondaryToolbar()) live in leftSecondaryToolbarHolder, which the .ui put
  // in the SECOND row. The tall time globe -- added to the row ABOVE it -- pushed that whole second row
  // down, so the panel rendered well below the icon strip. Lift the holder INTO the globe's row at the FAR
  // LEFT, TOP-aligned, so it sits at the same Y as the icons again (it is hidden except in the modes that
  // populate it). A zero-width spacer on the far right, whose width tracks the holder (see eventFilter),
  // keeps the centred globe block at the true viewport centre when the holder appears -- nothing shoves
  // the globe.
  if (auto* below = _viewport_overlay_ui->horizontalLayout_8)
    below->removeWidget(_viewport_overlay_ui->leftSecondaryToolbarHolder);

  _globe_balance_spacer = new QWidget(_overlay_widget);
  _globe_balance_spacer->setFixedWidth(0);
  _globe_balance_spacer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
  _viewport_overlay_ui->leftSecondaryToolbarHolder->installEventFilter(this);

  if (auto* row = _viewport_overlay_ui->horizontalLayout_4)
  {
    // drop the trailing .ui spacer
    for (int i = row->count() - 1; i >= 0; --i)
    {
      if (row->itemAt(i)->spacerItem()) { delete row->takeAt(i); break; }
    }
    // Row order: [leftSecondary][stretch] main | globe | right-toolbar [stretch][balance-spacer]. Spacing 0
    // so the button strips butt up against the globe; top-aligned so only the tall globe hangs down.
    row->setSpacing(0);
    row->insertWidget(1, right_toolbar, 0, Qt::AlignTop);
    row->insertWidget(1, _time_globe, 0, Qt::AlignTop);
    row->insertStretch(0, 1);
    row->addStretch(1);
    row->addWidget(_globe_balance_spacer, 0, Qt::AlignTop);
    row->insertWidget(0, _viewport_overlay_ui->leftSecondaryToolbarHolder, 0, Qt::AlignTop);
    row->setAlignment(_viewport_overlay_ui->upperToolbarHolder, Qt::AlignTop);
    row->setAlignment(_viewport_overlay_ui->leftSecondaryToolbarHolder, Qt::AlignTop);
  }
}

void MapView::setupKeybindingsGui()
{
  _keybindings = new Noggit::Ui::help(this);
  _keybindings->hide();
  connect(this, &QObject::destroyed, _keybindings, &QObject::deleteLater);

  connect ( &_show_keybindings_window, &Noggit::BoolToggleProperty::changed
    , _keybindings, &QWidget::setVisible
  );

  connect ( _keybindings, &Noggit::Ui::widget::visibilityChanged
    , &_show_keybindings_window, &Noggit::BoolToggleProperty::set
  );
}

void MapView::setupFileMenu()
{
  auto file_menu (_main_window->_menuBar->addMenu ("Editor"));
  connect (this, &QObject::destroyed, file_menu, &QObject::deleteLater);

  ADD_ACTION (file_menu, "Save current tile", "Ctrl+Shift+S", [this] { save(save_mode::current); emit saved();});
  ADD_ACTION (file_menu, "Save changed tiles", QKeySequence::Save, [this] { save(save_mode::changed); emit saved(); });
  ADD_ACTION (file_menu, "Save all tiles", "Ctrl+Shift+A", [this] { save(save_mode::all); emit saved(); });
  ADD_ACTION(file_menu, "Generate new WDL", "", [this] 
      { 
     QMessageBox prompt;
    prompt.setIcon(QMessageBox::Warning);
    prompt.setWindowFlags(Qt::WindowStaysOnTopHint);
     prompt.setText(std::string("Warning!\nThis will attempt to load all tiles in the map to generate a new WDL."
         "\nThis is likely to crash if there is any issue with any tile, it is recommended that you save your work first. Only use this if you really need a fresh WDL.").c_str());
     prompt.setInformativeText(std::string("Are you sure ?").c_str());
     prompt.setStandardButtons(QMessageBox::StandardButton::Yes | QMessageBox::StandardButton::No);
     prompt.setDefaultButton(QMessageBox::No);
     bool answer = prompt.exec() == QMessageBox::StandardButton::Yes;
     if (answer)
         _world->horizon.save_wdl(_world.get(), true);
      }
  );

  ADD_ACTION ( file_menu
  , "Reload tile"
  , "Shift+J"
  , [this]
               {
                 makeCurrent();
                 OpenGL::context::scoped_setter const _ (::gl, context());
                 _world->reload_tile (_camera.position);
                 _rotation_editor_need_update = true;
                 emit saved();
               }
  );

  file_menu->addSeparator();
  ADD_ACTION_NS (file_menu, "Force uid check on next opening", [this] { _force_uid_check = true; });
  file_menu->addSeparator();

  ADD_ACTION ( file_menu
  , "Add bookmark"
  , Qt::CTRL | Qt::Key_F5
      , [this]
      {

          auto bookmark = Noggit::Project::NoggitProjectBookmarkMap();
          bookmark.position = _camera.position;
          bookmark.camera_pitch = _camera.pitch()._;
          bookmark.camera_yaw = _camera.yaw()._;
          bookmark.map_id = _world->getMapID();
          bookmark.name = gAreaDB.getAreaName(_world->getAreaID(_camera.position));

        _project->createBookmark(bookmark);

      }
  );

  ADD_ACTION(file_menu
      , "Write coordinates to port.txt and copy to clipboard"
      , Qt::Key_G
      , [this]
      {
                 std::stringstream port_command;
                 port_command << ".go XYZ " << (ZEROPOINT - _camera.position.z) << " " << (ZEROPOINT - _camera.position.x) << " " << _camera.position.y << " " << _world->getMapID();
                 std::ofstream f("ports.txt", std::ios_base::app);
                 f << "Map: " << gAreaDB.getAreaName(_world->getAreaID (_camera.position)) << " on ADT " << std::floor(_camera.position.x / TILESIZE) << " " << std::floor(_camera.position.z / TILESIZE) << std::endl;
                 f << "Trinity/AC:" << std::endl << port_command.str() << std::endl;
                 // f << "ArcEmu:" << std::endl << ".worldport " << _world->getMapID() << " " << (ZEROPOINT - _camera.position.z) << " " << (ZEROPOINT - _camera.position.x) << " " << _camera.position.y << " " << std::endl << std::endl;
                 f.close();
                 QClipboard* clipboard = QGuiApplication::clipboard();
                 clipboard->setText(port_command.str().c_str(), QClipboard::Clipboard);
               }
  );

}

void MapView::setupEditMenu()
{
  auto edit_menu (_main_window->_menuBar->addMenu ("Edit"));
  connect (this, &QObject::destroyed, edit_menu, &QObject::deleteLater);

  edit_menu->addSeparator();
  edit_menu->addAction(createTextSeparator("Selected object"));
  edit_menu->addSeparator();
  ADD_ACTION (edit_menu, "Delete", Qt::Key_Delete, [this]
  {
    if (terrainMode == editing_mode::creature)
    {
      deleteSelectedCreatureSpawns();
      return;
    }
    if (terrainMode == editing_mode::gameobject)
    {
      deleteSelectedGameObjectSpawns();
      return;
    }
    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_REMOVED);
    DeleteSelectedObjects();
    NOGGIT_ACTION_MGR->endAction();
  });

  ADD_ACTION (edit_menu, "Reset rotation", "Ctrl+R",
              [this]
              {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
                ResetSelectedObjectRotation();
                NOGGIT_ACTION_MGR->endAction();
              });
  ADD_ACTION (edit_menu, "Set to ground", Qt::Key_PageDown,
              [this] {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
                snap_selected_models_to_the_ground();
                NOGGIT_ACTION_MGR->endAction();

              });

  edit_menu->addSeparator();
  edit_menu->addAction(createTextSeparator("Options"));
  edit_menu->addSeparator();
  ADD_TOGGLE_NS (edit_menu, "Locked cursor mode", _locked_cursor_mode);

  edit_menu->addSeparator();
  edit_menu->addAction(createTextSeparator("State"));
  edit_menu->addSeparator();
  ADD_ACTION (edit_menu, "Undo", "Ctrl+Z", [this]
  {
    if (terrainMode == editing_mode::creature && undoLastCreatureDelete()) { return; }
    if (terrainMode == editing_mode::gameobject && undoLastGameObjectDelete()) { return; }
    NOGGIT_ACTION_MGR->undo();
  });
  ADD_ACTION (edit_menu, "Redo", "Ctrl+Shift+Z", [this] { NOGGIT_ACTION_MGR->redo(); });
}

void MapView::setupAssistMenu()
{
  auto assist_menu (_main_window->_menuBar->addMenu ("Assist"));
  connect (this, &QObject::destroyed, assist_menu, &QObject::deleteLater);

  ADD_ACTION_NS (assist_menu, "Reload creature spawns", [this] { refreshCreatureSpawnOverlay(true); });
  ADD_ACTION_NS (assist_menu, "Export creature spawn SQL", [this] { saveDirtyCreatureSpawns(); });

  assist_menu->addSeparator();
  assist_menu->addAction(createTextSeparator("Model"));
  assist_menu->addSeparator();
  ADD_ACTION (assist_menu, "Last M2 from WMV", "Shift+V", [this] { objectEditor->import_last_model_from_wmv(eMODEL); });
  ADD_ACTION (assist_menu, "Last WMO from WMV", "Alt+V", [this] { objectEditor->import_last_model_from_wmv(eWMO); });
  ADD_ACTION_NS (assist_menu, "Helper models", [this] { objectEditor->helper_models_widget->show(); });

  assist_menu->addSeparator();
  assist_menu->addAction(createTextSeparator("Current ADT"));
  assist_menu->addSeparator();
  ADD_ACTION_NS ( assist_menu
  , "Set Area ID"
  , [this]
                  {
                    if (_selected_area_id != -1)
                    {
                      NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_AREAID);
                      _world->setAreaID(_camera.position, _selected_area_id, true);
                      NOGGIT_ACTION_MGR->endAction();
                    }
                  }
  );

  ADD_ACTION_NS ( assist_menu
  , "Ensure 4 texture layers"
  , [=]
    {
      makeCurrent();
      OpenGL::context::scoped_setter const _(::gl, context());

      NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
      _world->ensureAllTilesetsADT(_camera.position);
      NOGGIT_ACTION_MGR->endAction();

    }
  );

  auto cleanup_menu (assist_menu->addMenu ("Clean up"));

  ADD_ACTION_NS ( cleanup_menu
  , "Clear height map"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
                    _world->clearHeight(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Remove texture duplicates"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
                    _world->removeTexDuplicateOnADT(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Clear textures"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
                    _world->clearTextures(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Clear textures + set base"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
                    _world->setBaseTexture(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Clear shadows"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNK_SHADOWS);
                    _world->clear_shadows(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Clear models"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_REMOVED);
                    _world->clearAllModelsOnADT(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                    _rotation_editor_need_update = true;
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Clear duplicate models"
  , [this]
                  {
                    DESTRUCTIVE_ACTION
                      (
                        makeCurrent();
                    OpenGL::context::scoped_setter const _(::gl, context());
                    _world->delete_duplicate_model_and_wmo_instances();
                    )
                  }
  );

  auto cur_adt_export_menu(assist_menu->addMenu("Export"));
  ADD_ACTION_NS ( cur_adt_export_menu
  , "Export alphamaps"
  , [this]
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _(::gl, context());
    _world->exportADTAlphamap(_camera.position);
  }
  );

  ADD_ACTION_NS ( cur_adt_export_menu
  , "Export alphamaps (current texture)"
  , [this]
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _(::gl, context());

    if (!!Noggit::Ui::selected_texture::get())
    {
      _world->exportADTAlphamap(_camera.position, Noggit::Ui::selected_texture::get()->get()->file_key().filepath());
    }

  }
  );

  ADD_ACTION_NS ( cur_adt_export_menu
  , "Export vertex color map"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _(::gl, context());

                    _world->exportADTVertexColorMap(_camera.position);
                  }
  );

  QDialog* heightmap_export_params = new QDialog(this);
  heightmap_export_params->setWindowFlags(Qt::Popup);
  heightmap_export_params->setWindowTitle("Heightmap Exporter");
  QVBoxLayout* heightmap_export_params_layout = new QVBoxLayout(heightmap_export_params);

  heightmap_export_params_layout->addWidget(new QLabel("Min:", heightmap_export_params));
  QDoubleSpinBox* heightmap_export_min = new QDoubleSpinBox(heightmap_export_params);
  heightmap_export_min->setRange(-10000000, 10000000);
  heightmap_export_params_layout->addWidget(heightmap_export_min);

  heightmap_export_params_layout->addWidget(new QLabel("Max:", heightmap_export_params));
  QDoubleSpinBox* heightmap_export_max = new QDoubleSpinBox(heightmap_export_params);
  heightmap_export_max->setRange(-10000000, 10000000);
  heightmap_export_max->setValue(100.0);
  heightmap_export_params_layout->addWidget(heightmap_export_max);

  QPushButton* heightmap_export_okay = new QPushButton("Okay", heightmap_export_params);
  heightmap_export_params_layout->addWidget(heightmap_export_okay);

  connect(heightmap_export_min, qOverload<double>(&QDoubleSpinBox::valueChanged),
          [=](double value)
          {
            if (!(heightmap_export_max->value() > value))
              heightmap_export_max->setValue(value + 1.0);

          });

  connect(heightmap_export_max, qOverload<double>(&QDoubleSpinBox::valueChanged),
          [=](double value)
          {
            if (!(heightmap_export_min->value() < value))
              heightmap_export_min->setValue(value - 1.0);

          });

  connect(heightmap_export_okay, &QPushButton::clicked
    ,[=]()
    {
      heightmap_export_params->accept();

    });



  ADD_ACTION_NS ( cur_adt_export_menu
  , "Export heightmap"
  , [=]
              {
                QPoint new_pos = QCursor::pos();

                heightmap_export_params->setGeometry(new_pos.x(),
                new_pos.y(),
                heightmap_export_params->width(),
                heightmap_export_params->height());

                if (heightmap_export_params->exec() == QDialog::Accepted)
                {
                  makeCurrent();
                  OpenGL::context::scoped_setter const _(::gl, context());

                  _world->exportADTHeightmap(_camera.position, heightmap_export_min->value(), heightmap_export_max->value());
                }

              }
  );

  ADD_ACTION_NS ( cur_adt_export_menu
  , "Export normalmap"
  , [this]
      {
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());
        _world->exportADTNormalmap(_camera.position);
      }
  );

  auto cur_adt_import_menu(assist_menu->addMenu("Import"));


  QDialog* adt_import_params = new QDialog(this);
  adt_import_params->setWindowFlags(Qt::Popup);
  adt_import_params->setWindowTitle("Alphamap Importer");
  QVBoxLayout* adt_import_params_layout = new QVBoxLayout(adt_import_params);

  adt_import_params_layout->addWidget(new QLabel("Layer:", adt_import_params));
  QSpinBox* adt_import_params_layer = new QSpinBox(adt_import_params);
  adt_import_params_layer->setRange(1, 3);
  adt_import_params_layout->addWidget(adt_import_params_layer);

  QPushButton* adt_import_params_okay = new QPushButton("Okay", adt_import_params);
  adt_import_params_layout->addWidget(adt_import_params_okay);

  connect(adt_import_params_okay, &QPushButton::clicked
    ,[=]()
    {
      adt_import_params->accept();

    });

  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import alphamap (file)"
  , [=]
                  {
                    QPoint new_pos = QCursor::pos();

                    adt_import_params->setGeometry(new_pos.x(),
                                                   new_pos.y(),
                                                   heightmap_export_params->width(),
                                                   heightmap_export_params->height());

                    if (adt_import_params->exec() == QDialog::Accepted)
                    {
                      makeCurrent();
                      OpenGL::context::scoped_setter const _(::gl, context());

                      QString filepath = QFileDialog::getOpenFileName(
                        this,
                        tr("Open alphamap"),
                        "",
                        "PNG file (*.png);;"
                      );

                      if(!QFileInfo::exists(filepath))
                        return;

                      QImage img;
                      img.load(filepath, "PNG");

                      NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
                      _world->importADTAlphamap(_camera.position, img, adt_import_params_layer->value());
                      NOGGIT_ACTION_MGR->endAction();
                    }

                  }
  );

  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import alphamap"
  , [=]
    {

        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());

        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
        _world->importADTAlphamap(_camera.position);
        NOGGIT_ACTION_MGR->endAction();
    }
  );

  QDialog* adt_import_height_params = new QDialog(this);
  adt_import_height_params->setWindowFlags(Qt::Popup);
  adt_import_height_params->setWindowTitle("Alphamap Importer");
  QVBoxLayout* adt_import_height_params_layout = new QVBoxLayout(adt_import_height_params);

  adt_import_height_params_layout->addWidget(new QLabel("Multiplier:", adt_import_height_params));
  QDoubleSpinBox* adt_import_height_params_multiplier = new QDoubleSpinBox(adt_import_height_params);
  adt_import_height_params_multiplier->setRange(0, 100000000);
  adt_import_height_params_layout->addWidget(adt_import_height_params_multiplier);

  adt_import_height_params_layout->addWidget(new QLabel("Mode:", adt_import_height_params));
  QComboBox* adt_import_height_params_mode = new QComboBox(adt_import_height_params);
  adt_import_height_params_layout->addWidget(adt_import_height_params_mode);
  adt_import_height_params_mode->addItems({"Set", "Add", "Subtract", "Multiply" });

  QCheckBox* adt_import_height_tiled_edges = new QCheckBox("Tiled Edges", adt_import_height_params);
  adt_import_height_params_layout->addWidget(adt_import_height_tiled_edges);

  QPushButton* adt_import_height_params_okay = new QPushButton("Okay", adt_import_height_params);
  adt_import_height_params_layout->addWidget(adt_import_height_params_okay);

  connect(adt_import_height_params_okay, &QPushButton::clicked
    ,[=]()
          {
            adt_import_height_params->accept();

          });

  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import heightmap (file)"
  , [=]
      {
        if (adt_import_height_params->exec() == QDialog::Accepted)
        {
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());

          QString filepath = QFileDialog::getOpenFileName(
            this,
            tr("Open heightmap (257x257)"),
            "",
            "PNG file (*.png);;"
          );

          if(!QFileInfo::exists(filepath))
            return;

          QImage img;
          img.load(filepath, "PNG");

          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
          _world->importADTHeightmap(_camera.position, img, adt_import_height_params_multiplier->value(),
                                     adt_import_height_params_mode->currentIndex(), adt_import_height_tiled_edges->isChecked());
          NOGGIT_ACTION_MGR->endAction();
        }
      }
  );

  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import heightmap"
  , [=]
      {
        if (adt_import_height_params->exec() == QDialog::Accepted)
        {
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());

          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
          _world->importADTHeightmap(_camera.position, adt_import_height_params_multiplier->value(),
                                     adt_import_height_params_mode->currentIndex(), adt_import_height_tiled_edges->isChecked());
          NOGGIT_ACTION_MGR->endAction();
        }
      }
  );

  QDialog* adt_import_vcol_params = new QDialog(this);
  adt_import_vcol_params->setWindowFlags(Qt::Popup);
  adt_import_vcol_params->setWindowTitle("Alphamap Importer");
  QVBoxLayout* adt_import_vcol_params_layout = new QVBoxLayout(adt_import_vcol_params);

  adt_import_vcol_params_layout->addWidget(new QLabel("Mode:", adt_import_vcol_params));
  QComboBox* adt_import_vcol_params_mode = new QComboBox(adt_import_vcol_params);
  adt_import_vcol_params_layout->addWidget(adt_import_vcol_params_mode);
  adt_import_vcol_params_mode->addItems({"Set", "Add", "Subtract", "Multiply"});

  QCheckBox* adt_import_vcol_params_mode_tiled_edges = new QCheckBox("Tiled Edges", adt_import_vcol_params);
  adt_import_vcol_params_layout->addWidget(adt_import_vcol_params_mode_tiled_edges);

  QPushButton* adt_import_vcol_params_okay = new QPushButton("Okay", adt_import_vcol_params);
  adt_import_vcol_params_layout->addWidget(adt_import_vcol_params_okay);

  connect(adt_import_vcol_params_okay, &QPushButton::clicked
    ,[=]()
          {
            adt_import_vcol_params->accept();

          });


  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import vertex color map (file)"
  , [=]
    {
      if (adt_import_vcol_params->exec() == QDialog::Accepted)
      {
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());

        QString filepath = QFileDialog::getOpenFileName(
          this,
          tr("Open vertex color map (257x257)"),
          "",
          "PNG file (*.png);;"
        );

        if(!QFileInfo::exists(filepath))
          return;

        QImage img;
        img.load(filepath, "PNG");

        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_VERTEX_COLOR);
        _world->importADTVertexColorMap(_camera.position, img, adt_import_vcol_params_mode->currentIndex(), adt_import_vcol_params_mode_tiled_edges->isChecked());
        NOGGIT_ACTION_MGR->endAction();
      }
    }
  );

  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import vertex color map"
  , [=]
      {
        if (adt_import_vcol_params->exec() == QDialog::Accepted)
        {
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());

          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_VERTEX_COLOR);
          _world->importADTVertexColorMap(_camera.position, adt_import_vcol_params_mode->currentIndex(), adt_import_vcol_params_mode_tiled_edges->isChecked());
          NOGGIT_ACTION_MGR->endAction();
        }
      }
  );


  assist_menu->addSeparator();
  assist_menu->addAction(createTextSeparator("Loaded ADTs"));
  assist_menu->addSeparator();
  ADD_ACTION_NS ( assist_menu
  , "Fix gaps"
  , [this]
      {
        makeCurrent();
        OpenGL::context::scoped_setter const _ (::gl, context());
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
        _world->fixAllGaps();
        NOGGIT_ACTION_MGR->endAction();
      }
  );

  assist_menu->addSeparator();
  assist_menu->addAction(createTextSeparator("Global"));
  assist_menu->addSeparator();
  ADD_ACTION_NS ( assist_menu
  , "Map to big alpha"
  , [this]
    {
      DESTRUCTIVE_ACTION
      (
        makeCurrent();
        OpenGL::context::scoped_setter const _ (::gl, context());
        _world->convert_alphamap(true);
      )

    }
  );
  ADD_ACTION_NS ( assist_menu
  , "Map to old alpha"
  , [this]
    {
      DESTRUCTIVE_ACTION
      (
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());
        _world->convert_alphamap(false);
      )
    }
  );


  ADD_ACTION_NS ( assist_menu
  , "Ensure 4 texture layers"
  , [=]
      {
        DESTRUCTIVE_ACTION
        (
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());
          _world->ensureAllTilesetsAllADTs();
        )

      }
  );

  auto all_adts_export_menu(assist_menu->addMenu("Export"));

  ADD_ACTION_NS ( all_adts_export_menu
  , "Export alphamaps"
  , [this]
    {
      DESTRUCTIVE_ACTION
      (
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());
        _world->exportAllADTsAlphamap();
      )
    }
  );

  ADD_ACTION_NS ( all_adts_export_menu
  , "Export alphamaps (current texture)"
  , [this]
  {
    DESTRUCTIVE_ACTION
    (
      makeCurrent();
      OpenGL::context::scoped_setter const _(::gl, context());

      if (!!Noggit::Ui::selected_texture::get())
      {
        _world->exportAllADTsAlphamap(Noggit::Ui::selected_texture::get()->get()->file_key().filepath());
      }
    )
  }
  );

  ADD_ACTION_NS ( all_adts_export_menu
  , "Export heightmap"
  , [this]
    {
      DESTRUCTIVE_ACTION
      (
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());

        _world->exportAllADTsHeightmap();
      )
    }
  );

  ADD_ACTION_NS ( all_adts_export_menu
  , "Export vertex color map"
  , [this]
    {
      DESTRUCTIVE_ACTION
      (
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());

        _world->exportAllADTsVertexColorMap();
      )
    }
  );

  auto all_adts_import_menu(assist_menu->addMenu("Import"));

  ADD_ACTION_NS ( all_adts_import_menu
  , "Import alphamaps"
  , [this]
  {
    DESTRUCTIVE_ACTION
    (
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
        _world->importAllADTsAlphamaps();
        NOGGIT_ACTION_MGR->endAction();

    )
  }
  );

  ADD_ACTION_NS ( all_adts_import_menu
  , "Import heightmaps"
  , [=]
    {
      if (adt_import_height_params->exec() == QDialog::Accepted)
      {
        DESTRUCTIVE_ACTION
        (
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());
            NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
            _world->importAllADTsHeightmaps(adt_import_height_params_multiplier->value(), adt_import_height_params_mode->currentIndex(), adt_import_height_tiled_edges->isChecked());
            NOGGIT_ACTION_MGR->endAction();
        )

      }
    }
  );

  ADD_ACTION_NS ( all_adts_import_menu
  , "Import vertex color maps"
  , [=]
  {
    if (adt_import_vcol_params->exec() == QDialog::Accepted)
    {
      DESTRUCTIVE_ACTION
      (
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());
          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_VERTEX_COLOR);
          _world->importAllADTVertexColorMaps(adt_import_vcol_params_mode->currentIndex(), adt_import_vcol_params_mode_tiled_edges->isChecked());
          NOGGIT_ACTION_MGR->endAction();
      )

    }
  }
  );

  auto debug_menu(assist_menu->addMenu("Debug"));

  ADD_ACTION_NS ( debug_menu
  , "Load all tiles"
  , [=]
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _(::gl, context());
    _world->loadAllTiles();
  }
  );

}

void MapView::setupViewMenu()
{
  // Apply persisted graphics toggles (Settings > Graphics > Render features) over the header
  // defaults, so the user's render preferences survive restarts. Done BEFORE the ADD_TOGGLE macros
  // below so the View-menu checkmarks reflect the loaded state. The pure render-path toggles are
  // read live via property.get() each frame; the two terrain-param toggles also need their uniform
  // block synced once here (the ADD_TOGGLE_POST lambdas only run that on user toggle, not at setup).
  _draw_models.set           (_settings->value("render/doodads",          true ).toBool());
  _draw_wmo_doodads.set      (_settings->value("render/wmo_doodads",      true ).toBool());
  _draw_wmo.set              (_settings->value("render/wmo",              true ).toBool());
  _draw_terrain.set          (_settings->value("render/terrain",          true ).toBool());
  _draw_water.set            (_settings->value("render/water",            true ).toBool());
  _draw_clouds.set           (_settings->value("render/draw_clouds",      true ).toBool());
  _draw_sun.set              (_settings->value("render/draw_sun",         true ).toBool());
  _draw_moon.set             (_settings->value("render/draw_moon",        true ).toBool());
  _draw_model_animations.set (_settings->value("render/model_animations", true ).toBool());
  _draw_bloom.set            (_settings->value("render/bloom",            true ).toBool());
  _draw_ground_clutter.set   (_settings->value("render/ground_clutter",   true ).toBool());
  _wdl_horizon_occlusion.set (_settings->value("render/wdl_horizon_occlusion", false).toBool());
  _draw_fog.set              (_settings->value("render/fog",              true ).toBool());
  _draw_vertex_color.set     (_settings->value("render/vertex_color",     true ).toBool());
  _draw_baked_shadows.set    (_settings->value("render/baked_shadows",    true ).toBool());
  if (_world && _world->renderer())
  {
    auto* tp = _world->renderer()->getTerrainParamsUniformBlock();
    tp->draw_vertex_color = _draw_vertex_color.get();
    tp->draw_shadows = _draw_baked_shadows.get();
    _world->renderer()->markTerrainParamsUniformBlockDirty();
  }

  auto view_menu (_main_window->_menuBar->addMenu ("View"));
  connect (this, &QObject::destroyed, view_menu, &QObject::deleteLater);

  view_menu->addSeparator();
  view_menu->addAction(createTextSeparator("Drawing"));
  view_menu->addSeparator();
  ADD_TOGGLE (view_menu, "Doodads",     Qt::Key_F1, _draw_models);
  ADD_TOGGLE (view_menu, "WMO doodads", Qt::Key_F2, _draw_wmo_doodads);
  ADD_TOGGLE (view_menu, "Terrain",     Qt::Key_F3, _draw_terrain);
  ADD_TOGGLE (view_menu, "Water",       Qt::Key_F4, _draw_water);
  // Clouds/sun/moon render-paths read the SETTINGS each frame (Skies::draw_clouds and the
  // WorldRender celestial block), so persist on toggle.
  ADD_TOGGLE_POST (view_menu, "Clouds", Qt::SHIFT | Qt::Key_F5, _draw_clouds,
                   [=]
                   {
                     _settings->setValue("render/draw_clouds", _draw_clouds.get());
                     _settings->sync();
                   });
  ADD_TOGGLE_POST (view_menu, "Sun", Qt::SHIFT | Qt::Key_F6, _draw_sun,
                   [=]
                   {
                     _settings->setValue("render/draw_sun", _draw_sun.get());
                     _settings->sync();
                   });
  ADD_TOGGLE_POST (view_menu, "Moon", Qt::SHIFT | Qt::Key_F7, _draw_moon,
                   [=]
                   {
                     _settings->setValue("render/draw_moon", _draw_moon.get());
                     _settings->sync();
                   });
  ADD_TOGGLE (view_menu, "Bloom",       Qt::Key_F5, _draw_bloom);
  ADD_TOGGLE (view_menu, "WMOs",        Qt::Key_F6, _draw_wmo);
  ADD_TOGGLE (view_menu, "Ground clutter", Qt::SHIFT | Qt::Key_G, _draw_ground_clutter);
  // WorldRender reads render/wdl_horizon_occlusion each frame, so persist on toggle.
  ADD_TOGGLE_POST (view_menu, "WDL horizon occlusion (perf)", 0, _wdl_horizon_occlusion,
                   [=]
                   {
                     _settings->setValue("render/wdl_horizon_occlusion", _wdl_horizon_occlusion.get());
                     _settings->sync();
                   });

  ADD_TOGGLE_POST (view_menu, "Lines", Qt::Key_F7, _draw_lines,
                   [=]
                   {
                     _world->renderer()->getTerrainParamsUniformBlock()->draw_lines = _draw_lines.get();
                     _world->renderer()->markTerrainParamsUniformBlockDirty();
                   });

  ADD_TOGGLE_POST (view_menu, "Contours", Qt::Key_F9, _draw_contour,
                   [=]
                   {
                     _world->renderer()->getTerrainParamsUniformBlock()->draw_terrain_height_contour = _draw_contour.get();
                     _world->renderer()->markTerrainParamsUniformBlockDirty();
                   });

  ADD_TOGGLE_POST (view_menu, "Wireframe", Qt::Key_F10, _draw_wireframe,
                   [=]
                   {
                     _world->renderer()->getTerrainParamsUniformBlock()->draw_wireframe = _draw_wireframe.get();
                     _world->renderer()->markTerrainParamsUniformBlockDirty();
                   });

  ADD_TOGGLE (view_menu, "Toggle Animation", Qt::Key_F11, _draw_model_animations);
  ADD_TOGGLE (view_menu, "Draw fog", Qt::Key_F12, _draw_fog);

  ADD_TOGGLE_POST (view_menu, "Hole lines", Qt::SHIFT | Qt::Key_F1, _draw_hole_lines,
                   [=]
                   {
                     _world->renderer()->getTerrainParamsUniformBlock()->draw_hole_lines = _draw_hole_lines.get();
                     _world->renderer()->markTerrainParamsUniformBlockDirty();
                   });

  ADD_TOGGLE_POST(view_menu, "Climb", Qt::SHIFT | Qt::Key_F2, _draw_climb,
                  [=]
                  {
                      _world->renderer()->getTerrainParamsUniformBlock()->draw_impassible_climb = _draw_climb.get();
                      _world->renderer()->markTerrainParamsUniformBlockDirty();
                  });

  ADD_TOGGLE_POST(view_menu, "Vertex Color", Qt::SHIFT | Qt::Key_F3, _draw_vertex_color,
      [=]
      {
          _world->renderer()->getTerrainParamsUniformBlock()->draw_vertex_color = _draw_vertex_color.get();
          _world->renderer()->markTerrainParamsUniformBlockDirty();
      });

  ADD_TOGGLE_POST(view_menu, "Baked Shadows", Qt::SHIFT | Qt::Key_F4, _draw_baked_shadows,
      [=]
      {
          _world->renderer()->getTerrainParamsUniformBlock()->draw_shadows = _draw_baked_shadows.get();
          _world->renderer()->markTerrainParamsUniformBlockDirty();
      });

  ADD_TOGGLE_NS (view_menu, "Flight Bounds", _draw_mfbo);

  ADD_TOGGLE_NS (view_menu, "Models with box", _draw_models_with_box);
  //! \todo space+h in object mode
  ADD_TOGGLE_NS (view_menu, "Hidden models", _draw_hidden_models);
  ADD_TOGGLE_NS (view_menu, "Creature spawns", _draw_creature_spawns);

  ADD_TOGGLE_NS(view_menu, "Game Mode", _game_mode_camera);

  auto debug_menu (view_menu->addMenu ("Debug"));
  ADD_TOGGLE_NS (debug_menu, "Occlusion boxes", _draw_occlusion_boxes);

  view_menu->addSeparator();
  view_menu->addAction(createTextSeparator("Tools"));
  view_menu->addSeparator();

  ADD_TOGGLE (view_menu, "Show Node Editor", "Shift+N", _show_node_editor);
  ADD_TOGGLE_NS (view_menu, "Creature browser", _show_creature_browser);

  view_menu->addSeparator();
  view_menu->addAction(createTextSeparator("Minimap"));
  view_menu->addSeparator();

  ADD_TOGGLE (view_menu, "Show", Qt::Key_M, _show_minimap_window);


  ADD_TOGGLE_NS(view_menu, "Show ADT borders", _show_minimap_borders);

  ADD_TOGGLE_NS(view_menu, "Show light zones", _show_minimap_skies);

  connect(&_draw_creature_spawns, &Noggit::BoolToggleProperty::changed, [this](bool enabled)
  {
    _world->setDrawCreatureSpawns(enabled);
    _settings->setValue("view/creature_spawns", enabled);

    if (enabled)
    {
      refreshCreatureSpawnOverlay(false);
    }
    else
    {
      updateDatabaseStatus();
    }
  });

  view_menu->addSeparator();
  view_menu->addAction(createTextSeparator("Windows"));
  view_menu->addSeparator();

  auto hide_widgets = [=]
  {

    QWidget *widget_list[] =
      {
        _texture_browser_dock,
        _texture_picker_dock,
        _detail_infos_dock,
        _creature_actions_overlay,
        _creature_browser_dock,
        _creature_editor_dock,
        _creature_model_picker_dock,
        _keybindings,
        _minimap_dock,
        objectEditor->modelImport,
        objectEditor->rotationEditor,
        objectEditor->helper_models_widget,
        _texture_palette_small,
        _object_palette_dock,
        _asset_browser_dock,
        _overlay_widget,
        _tool_panel_dock

      };

    if (_main_window->displayed_widgets.empty())
    {
      for (auto widget : widget_list)
        if (widget && widget->isVisible())
        {
          _main_window->displayed_widgets.emplace(widget);
          widget->hide();
        }

    }
    else
    {
      for (auto widget : _main_window->displayed_widgets)
        if (widget)
          widget->show();

      _main_window->displayed_widgets.clear();
    }


    _main_window->statusBar()->setVisible(ui_hidden);
    _toolbar->setVisible(ui_hidden);
    _view_toolbar->setVisible(ui_hidden);

    ui_hidden = !ui_hidden;

    setToolPropertyWidgetVisibility(terrainMode);

  };

  ADD_ACTION(view_menu, "Toggle UI", Qt::Key_Tab, hide_widgets);

  ADD_TOGGLE (view_menu, "Detail infos", Qt::Key_F8, _show_detail_info_window);

  ADD_TOGGLE (view_menu, "Texture Browser", Qt::Key_X, _show_texture_palette_window);

  ADD_TOGGLE_NS(view_menu, "Texture palette", _show_texture_palette_small_window);

  addHotkey( Qt::Key_H
    , MOD_none
    , [this] { _show_texture_palette_small_window.toggle(); }
    , [this] { return terrainMode == editing_mode::paint; }
  );

  ADD_ACTION (view_menu, "Increase time speed", Qt::Key_N, [this] { mTimespeed += 90.0f; });
  ADD_ACTION (view_menu, "Decrease time speed", Qt::Key_B, [this] { mTimespeed = std::max (0.0f, mTimespeed - 90.0f); });
  ADD_ACTION (view_menu, "Pause time", Qt::Key_J, [this] { mTimespeed = 0.0f; });
  ADD_ACTION (view_menu, "Invert mouse", "I", [this] { mousedir *= -1.f; });
  ADD_ACTION (view_menu, "Decrease camera speed", Qt::Key_O, [this] { _camera.move_speed *= 0.5f; });
  ADD_ACTION (view_menu, "Increase camera speed", Qt::Key_P, [this] { _camera.move_speed *= 2.0f; });
  ADD_ACTION ( view_menu
  , "Turn camera around 180°"
  , "Shift+R"
  , [this]
               {
                 _camera.add_to_yaw(math::degrees(180.f));
                 _camera_moved_since_last_draw = true;
               }
  );

  ADD_ACTION ( view_menu
  , "Toggle tile mode"
  , Qt::Key_U
  , [this]
               {
                 if (NOGGIT_CUR_ACTION)
                   return;

                 if (_display_mode == display_mode::in_2D)
                 {
                   _display_mode = display_mode::in_3D;
                   set_editing_mode (saveterrainMode);
                 }
                 else
                 {
                   _display_mode = display_mode::in_2D;
                   saveterrainMode = terrainMode;
                   set_editing_mode (editing_mode::paint);
                 }
               }
  );

}

void MapView::setupHelpMenu()
{
  auto help_menu (_main_window->_menuBar->addMenu ("Help"));
  connect (this, &QObject::destroyed, help_menu, &QObject::deleteLater);

  ADD_TOGGLE (help_menu, "Key Bindings", "Ctrl+F1", _show_keybindings_window);

#if defined(_WIN32) || defined(WIN32)
  ADD_ACTION_NS ( help_menu
                , "WoW Modding Discord"
                , []
                  {
                    ShellExecute ( nullptr
                                 , "open"
                                 , "https://discord.gg/Dnrztg7dCZ"
                                 , nullptr
                                 , nullptr
                                 , SW_SHOWNORMAL
                                 );
                  }
                );
  ADD_ACTION_NS ( help_menu
                , "Noggit Red Repository"
                , []
                  {
                    ShellExecute ( nullptr
                                 , "open"
                                 , "https://gitlab.com/prophecy-rp/noggit-red/-/tree/noggit-shadowlands?ref_type=heads"
                                 , nullptr
                                 , nullptr
                                 , SW_SHOWNORMAL
                                 );
                  }
                );

  ADD_ACTION_NS ( help_menu
                , "Noggit Red Discord"
                , []
                  {
                    ShellExecute ( nullptr
                                 , "open"
                                 , "https://discord.gg/Tk2TpN8CaF"
                                 , nullptr
                                 , nullptr
                                 , SW_SHOWNORMAL
                                 );
                  }
                );
#endif

}

void MapView::refreshCreatureSpawnOverlay(bool force_reload)
{
  _world->setDrawCreatureSpawns(_draw_creature_spawns.get());

  if (!_draw_creature_spawns.get())
  {
    setSelectedCreatureSpawn(std::nullopt, false);
    rebuildCreatureBrowserList(false);
    updateDatabaseStatus();
    return;
  }

  if (force_reload || !_world->hasCreatureSpawnsLoaded())
  {
    _world->reloadCreatureSpawns();
    // Spawns (and gameobjects, loaded alongside) were re-read -> drop the zone-filter caches.
    _creature_zone_cache.clear();
    _gameobject_zone_cache.clear();
    _main_window->statusBar()->showMessage(QString::fromStdString(_world->creatureSpawnStatus()), 5000);
  }

  rebuildCreatureBrowserList(true);
  updateDatabaseStatus();
}

void MapView::updateDatabaseStatus()
{
#ifdef USE_MYSQL_UID_STORAGE
  QStringList parts;

  if (Noggit::mysqlSetting("enabled", false).toBool())
  {
    parts << QString("MySQL %1:%2")
                 .arg(Noggit::mysqlSetting("server", "127.0.0.1").toString())
                 .arg(Noggit::mysqlSetting("port", 3306).toString());
  }

  if (_draw_creature_spawns.get())
  {
    parts << QString::fromStdString(_world->creatureSpawnStatus());

    std::size_t nearby_spawn_count = 0;
    float nearest_spawn_distance = std::numeric_limits<float>::max();
    constexpr float nearby_spawn_radius = 200.0f;

    for (auto const& spawn : _world->creatureSpawns())
    {
      float horizontal_distance = glm::distance(glm::vec2(_camera.position.x, _camera.position.z),
                                                glm::vec2(spawn.pos.x, spawn.pos.z));
      nearest_spawn_distance = std::min(nearest_spawn_distance, horizontal_distance);

      if (horizontal_distance <= nearby_spawn_radius)
      {
        ++nearby_spawn_count;
      }
    }

    if (!_world->creatureSpawns().empty())
    {
      parts << QString("nearby(%1): %2")
                   .arg(nearby_spawn_radius, 0, 'f', 0)
                   .arg(nearby_spawn_count);
      parts << QString("nearest: %1")
                   .arg(nearest_spawn_distance, 0, 'f', 1);
    }

    auto dirty_count = _world->dirtyCreatureSpawnCount();
    if (dirty_count > 0)
    {
      parts << QString("pending edits: %1").arg(dirty_count);
    }
  }

  _status_database->setText(parts.join(" | "));
#else
  _status_database->clear();
#endif

  updateCreatureBrowserStatus();
}

// One row's label. Shared by the full rebuild and by refreshCreatureBrowserItems(), so the two can
// never drift apart in what a row says.
QString MapView::creature_spawn_item_text(World::CreatureSpawnOverlay const& spawn)
{
  QString prefix;
  if (spawn.selected)
  {
    prefix += "[selected] ";
  }
  if (spawn.pending_create)
  {
    prefix += "[new] ";
  }
  if (spawn.dirty)
  {
    prefix += "[pending] ";
  }

  auto const name = QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name);
  return QString("%1%2 [entry %3] guid %4").arg(prefix).arg(name).arg(spawn.entry).arg(spawn.guid);
}

// Retext ONLY the rows whose spawns changed, instead of clearing and repopulating the whole list.
// Dragging flips a spawn's `dirty` flag, which changes its "[pending]" prefix -- but a full
// rebuildCreatureBrowserList() allocates a QListWidgetItem for every spawn in the world, which is a
// ~1s freeze on release. This walks the existing rows and touches the handful that moved.
void MapView::refreshCreatureBrowserItems(std::vector<std::uint32_t> const& guids)
{
  if (!_creature_list_widget || guids.empty())
  {
    return;
  }

  QSignalBlocker blocker(_creature_list_widget);
  for (int i = 0; i < _creature_list_widget->count(); ++i)
  {
    auto* item = _creature_list_widget->item(i);
    if (!item)
    {
      continue;
    }

    auto const guid = static_cast<std::uint32_t>(item->data(Qt::UserRole).toULongLong());
    if (std::find(guids.begin(), guids.end(), guid) == guids.end())
    {
      continue;
    }

    if (auto const* spawn = _world->findCreatureSpawn(guid))
    {
      item->setText(creature_spawn_item_text(*spawn));
    }
  }
}

// A SELECTION change does not change the list's CONTENT -- only which row is highlighted. Doing that
// through rebuildCreatureBrowserList() cleared the QListWidget and allocated a fresh item for every
// spawn in the world (plus a getZoneId() per spawn when the zone filter is on), which is the ~1s
// freeze on every click in the creature tool -- including a click on empty ground, which only
// deselects. Move the highlight instead; the rebuild is for content changes (create/delete/filter).
void MapView::highlightCreatureBrowserSelection()
{
  if (!_creature_list_widget)
  {
    return;
  }

  QSignalBlocker blocker(_creature_list_widget);

  if (_selected_creature_spawn_guid)
  {
    for (int i = 0; i < _creature_list_widget->count(); ++i)
    {
      auto* item = _creature_list_widget->item(i);
      if (item && static_cast<std::uint32_t>(item->data(Qt::UserRole).toULongLong())
                    == *_selected_creature_spawn_guid)
      {
        _creature_list_widget->setCurrentItem(item);
        updateCreatureBrowserStatus();
        return;
      }
    }
  }

  // Nothing selected, or the selected spawn is filtered out of the list by the search / zone / type
  // filters -- a rebuild would not have highlighted anything either.
  _creature_list_widget->setCurrentRow(-1);
  updateCreatureBrowserStatus();
}

void MapView::highlightGameObjectBrowserSelection()
{
  if (!_gameobject_list_widget)
  {
    return;
  }

  QSignalBlocker blocker(_gameobject_list_widget);

  if (_selected_gameobject_spawn_guid)
  {
    for (int i = 0; i < _gameobject_list_widget->count(); ++i)
    {
      auto* item = _gameobject_list_widget->item(i);
      if (item && static_cast<std::uint32_t>(item->data(Qt::UserRole).toULongLong())
                    == *_selected_gameobject_spawn_guid)
      {
        _gameobject_list_widget->setCurrentItem(item);
        updateGameObjectBrowserStatus();
        return;
      }
    }
  }

  _gameobject_list_widget->setCurrentRow(-1);
  updateGameObjectBrowserStatus();
}

void MapView::rebuildCreatureBrowserList(bool preserve_selection)
{
  if (!_creature_list_widget)
  {
    return;
  }

  auto selected_guid = preserve_selection ? _selected_creature_spawn_guid : std::optional<std::uint32_t>();
  auto search_text = _creature_search_field ? _creature_search_field->text().trimmed() : QString();
  auto search_lower = search_text.toLower();

  // "Zone only" filter: only keep spawns whose zone matches the camera's current zone. Disabled if
  // the camera's own zone can't be resolved (e.g. standing outside any loaded area).
  unsigned int camera_zone = (_creature_zone_filter && _creature_zone_filter->isChecked())
    ? _world->getZoneId(_camera.position) : 0u;
  bool const zone_only = camera_zone != 0u && camera_zone != static_cast<unsigned int>(-1);

  QSignalBlocker blocker(_creature_list_widget);
  _creature_list_widget->clear();

  for (auto const& spawn : _world->creatureSpawns())
  {
    if (spawn.pending_delete) // marked for deletion -> hidden from the list
    {
      continue;
    }
    if (zone_only)
    {
      unsigned int zone;
      auto cit = _creature_zone_cache.find(spawn.guid);
      if (cit != _creature_zone_cache.end())
      {
        zone = cit->second;
      }
      else
      {
        zone = _world->getZoneId(spawn.pos);
        if (zone != 0u && zone != static_cast<unsigned int>(-1))
          _creature_zone_cache[spawn.guid] = zone; // cache only resolved zones (retry unloaded tiles)
      }
      if (zone != camera_zone)
        continue;
    }
    auto name = QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name);
    auto entry_text = QString::number(spawn.entry);
    auto guid_text = QString::number(spawn.guid);

    if (!search_lower.isEmpty()
        && !name.toLower().contains(search_lower)
        && !entry_text.contains(search_lower)
        && !guid_text.contains(search_lower))
    {
      continue;
    }

    // Type/rank filter (same semantics as the NPC model picker), looked up by spawn entry. Spawns whose
    // creature_template info hasn't loaded yet are left visible (don't hide what we can't classify).
    {
      auto fit = _creature_template_filter_info.find(spawn.entry);
      if (fit != _creature_template_filter_info.end())
      {
        auto const& info = fit->second;
        auto const type_data = _creature_browser_type_filter ? _creature_browser_type_filter->currentData() : QVariant();
        if (type_data.isValid() && info.creature_type != static_cast<std::uint32_t>(type_data.toULongLong()))
          continue;
        if (_creature_browser_elite && _creature_browser_elite->isChecked() && !(info.rank == 1u || info.rank == 2u))
          continue;
        if (_creature_browser_boss && _creature_browser_boss->isChecked() && info.rank < 3u && (info.type_flags & 0x4u) == 0u)
          continue;
        if (_creature_browser_civilian && _creature_browser_civilian->isChecked() && (info.flags_extra & 0x2u) == 0u && (info.type_flags & 0x80u) == 0u)
          continue;
        if (_creature_browser_trainer && _creature_browser_trainer->isChecked() && (info.npc_flags & 0x10u) == 0u)
          continue;
      }
    }

    auto* item = new QListWidgetItem(creature_spawn_item_text(spawn), _creature_list_widget);
    item->setData(Qt::UserRole, static_cast<qulonglong>(spawn.guid));
    item->setData(Qt::UserRole + 1, static_cast<int>(_world->getMapID()));
    item->setData(Qt::UserRole + 6, true);

    if (selected_guid && *selected_guid == spawn.guid)
    {
      _creature_list_widget->setCurrentItem(item);
    }
  }

  updateCreatureBrowserStatus();
}

void MapView::updateCreatureBrowserStatus(QString const& override_text)
{
  if (!_creature_browser_status)
  {
    return;
  }

  if (!override_text.isEmpty())
  {
    _creature_browser_status->setText(override_text);
    return;
  }

  QStringList parts;
  parts << QString("Current map spawns: %1").arg(_world->creatureSpawnCount());
  parts << QString("models: %1").arg(_world->creatureSpawnModelCount());

  auto dirty_count = _world->dirtyCreatureSpawnCount();
  if (dirty_count > 0)
  {
    parts << QString("pending edits: %1").arg(dirty_count);
  }

  auto selected_count = selectedCreatureSpawnCount();
  if (selected_count > 1)
  {
    parts << QString("selected: %1").arg(selected_count);
  }
  else if (_selected_creature_spawn_guid)
  {
    parts << QString("selected guid: %1").arg(*_selected_creature_spawn_guid);
  }

  _creature_browser_status->setText(parts.join(" | "));
}

std::size_t MapView::selectedCreatureSpawnCount() const
{
  return static_cast<std::size_t>(std::count_if(_world->creatureSpawns().begin(),
                                                _world->creatureSpawns().end(),
    [](World::CreatureSpawnOverlay const& spawn)
    {
      return spawn.selected;
    }));
}

void MapView::setSelectedCreatureSpawn(std::optional<std::uint32_t> guid, bool update_browser)
{
  _selected_creature_spawn_guid = guid;

  // Collect the spawns whose selected flag actually flips: their row text carries a "[selected]"
  // prefix, so those are the only rows that need new text.
  std::vector<std::uint32_t> retext;
  for (auto& spawn : _world->creatureSpawns())
  {
    bool const now_selected = guid && spawn.guid == *guid;
    if (spawn.selected != now_selected)
    {
      retext.push_back(spawn.guid);
    }
    spawn.selected = now_selected;
  }

  // Populate the "Edit/New Creature" form from the selected spawn (empty = New). So clicking an existing
  // creature fills its guid/entry/display; deselecting clears back to the new-spawn state.
  if (_creature_spawn_guid_field)
  {
    World::CreatureSpawnOverlay const* sp = guid ? _world->findCreatureSpawn(*guid) : nullptr;
    if (sp)
    {
      _creature_spawn_guid_field->setText(QString::number(sp->guid));
      _creature_spawn_entry_field->setText(QString::number(sp->entry));
      _creature_spawn_display_field->setText(QString::number(sp->display_id));
    }
    else
    {
      _creature_spawn_guid_field->clear();
      _creature_spawn_entry_field->clear();
      _creature_spawn_display_field->clear();
    }
  }

  if (update_browser)
  {
    // Selection only -- no content change. Retext the handful of rows whose prefix flipped and move
    // the highlight, instead of clearing and repopulating the whole list (the ~1s click freeze).
    refreshCreatureBrowserItems(retext);
    highlightCreatureBrowserSelection();
  }
  else
  {
    updateCreatureBrowserStatus();
  }

  refreshCreatureEditorKnobs();
}

void MapView::addCreatureSpawnToSelection(std::uint32_t guid, bool update_browser)
{
  bool found = false;
  for (auto& spawn : _world->creatureSpawns())
  {
    if (spawn.guid == guid)
    {
      spawn.selected = true;
      found = true;
    }
  }

  if (!found)
  {
    return;
  }

  _selected_creature_spawn_guid = guid;

  if (update_browser)
  {
    // Only this row's "[selected]" prefix changed -- see setSelectedCreatureSpawn.
    refreshCreatureBrowserItems({guid});
    highlightCreatureBrowserSelection();
  }
  else
  {
    updateCreatureBrowserStatus();
  }

  refreshCreatureEditorKnobs();
}

void MapView::selectCreatureSpawnsInArea(QRect const& rect, bool add_to_selection)
{
  if (!_world->hasCreatureSpawnsLoaded())
  {
    return;
  }

  QRect const normalized_rect = rect.normalized();
  glm::mat4x4 const mv = model_view();
  glm::mat4x4 const proj = projection();
  glm::vec4 const vp(0.0f, 0.0f, float(width()), float(height()));

  if (!add_to_selection)
  {
    for (auto& spawn : _world->creatureSpawns())
    {
      spawn.selected = false;
    }
  }

  std::optional<std::uint32_t> primary_guid = add_to_selection ? _selected_creature_spawn_guid : std::optional<std::uint32_t>();

  for (auto& spawn : _world->creatureSpawns())
  {
    if (spawn.event_suppressed) // hidden by the Seasonal Events filter -> not box-selectable
    {
      continue;
    }
    glm::vec3 const screen = glm::project(spawn.pos, mv, proj, vp);
    if (screen.z < 0.0f || screen.z > 1.0f)
    {
      continue;
    }

    QPoint const point(static_cast<int>(std::lround(screen.x)),
                       static_cast<int>(std::lround(float(height()) - screen.y)));
    if (!normalized_rect.contains(point))
    {
      continue;
    }

    spawn.selected = true;
    if (!primary_guid)
    {
      primary_guid = spawn.guid;
    }
  }

  if (primary_guid)
  {
    _selected_creature_spawn_guid = primary_guid;
  }
  else if (!add_to_selection)
  {
    _selected_creature_spawn_guid = std::optional<std::uint32_t>();
  }

  rebuildCreatureBrowserList(true);
  refreshCreatureEditorKnobs();
  updateDatabaseStatus();
}

void MapView::setCreatureInfoPanelVisible(bool visible)
{
  if (!_creature_info_panel)
  {
    // Free-floating tool window (movable by its own title bar), initially aligned with the left
    // secondary toolbar so it doesn't cover the side icon strip.
    _creature_info_panel = new Noggit::Ui::CreatureInfoPanel(this);
    _creature_info_panel->setWindowFlags(Qt::Tool);
    _creature_info_panel->setWindowTitle("Quick Facts");

    QWidget* bar = getLeftSecondaryToolbar();
    QPoint const initial = (bar && bar->isVisible())
      ? bar->mapToGlobal(QPoint(0, bar->height() + 6))
      : mapToGlobal(QPoint(60, 110));
    _creature_info_panel->move(initial);
  }

  if (!visible)
  {
    _creature_info_panel->hide();
    return;
  }

  if (_selected_creature_spawn_guid)
  {
    if (auto const* spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid))
    {
      _creature_info_panel->setCreature(spawn->entry);
    }
  }
  else
  {
    _creature_info_panel->clearCreature();
  }
  _creature_info_panel->show();
  _creature_info_panel->raise();
}

void MapView::refreshCreatureEditorKnobs()
{
  if (!_spawn_edit_x)
    return;

  // Keep the Quick Facts dropdown following the selection.
  if (_creature_info_panel && _creature_info_panel->isVisible())
  {
    if (_selected_creature_spawn_guid)
    {
      if (auto const* info_spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid))
      {
        _creature_info_panel->setCreature(info_spawn->entry);
      }
    }
    else
    {
      _creature_info_panel->clearCreature();
    }
  }

  auto disable_all = [this]() {
    _creature_editor_info->setText("No spawn selected");
    _creature_editor_info->setStyleSheet("font-style: italic; color: #888; padding: 2px 0;");
    for (auto* w : {_spawn_edit_x, _spawn_edit_y, _spawn_edit_z, _spawn_edit_orientation})
      w->setEnabled(false);
  };

  if (!_selected_creature_spawn_guid)
  {
    disable_all();
    return;
  }

  auto selected_count = selectedCreatureSpawnCount();
  if (selected_count > 1)
  {
    _creature_editor_info->setText(QString("%1 creature spawns selected\nPrimary GUID: %2")
                                     .arg(selected_count)
                                     .arg(*_selected_creature_spawn_guid));
    _creature_editor_info->setStyleSheet("font-weight: bold; padding: 2px 0;");
    for (auto* w : {_spawn_edit_x, _spawn_edit_y, _spawn_edit_z, _spawn_edit_orientation})
      w->setEnabled(false);
    return;
  }

  auto const* spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid);
  if (!spawn)
  {
    disable_all();
    return;
  }

  QString name = QString::fromStdString(spawn->name.empty() ? std::string("<unnamed>") : spawn->name);
  _creature_editor_info->setText(
    QString("%1\nGUID: %2  Entry: %3").arg(name).arg(spawn->guid).arg(spawn->entry));
  _creature_editor_info->setStyleSheet("font-weight: bold; padding: 2px 0;");

  for (auto* w : {_spawn_edit_x, _spawn_edit_y, _spawn_edit_z, _spawn_edit_orientation})
    w->blockSignals(true);

  _spawn_edit_x->setValue(static_cast<double>(spawn->pos.x));
  _spawn_edit_y->setValue(static_cast<double>(spawn->pos.y));
  _spawn_edit_z->setValue(static_cast<double>(spawn->pos.z));
  _spawn_edit_orientation->setValue(static_cast<double>(spawn->orientation));

  for (auto* w : {_spawn_edit_x, _spawn_edit_y, _spawn_edit_z, _spawn_edit_orientation})
  {
    w->setEnabled(true);
    w->blockSignals(false);
  }
}

void MapView::setHoveredCreatureSpawn(std::optional<std::uint32_t> guid)
{
  if (_hovered_creature_spawn_guid == guid)
  {
    return;
  }

  if (_hovered_creature_spawn_guid)
  {
    if (auto* previous = _world->findCreatureSpawn(*_hovered_creature_spawn_guid))
    {
      previous->hovered = false;
    }
  }

  _hovered_creature_spawn_guid = guid;

  if (_hovered_creature_spawn_guid)
  {
    if (auto* current = _world->findCreatureSpawn(*_hovered_creature_spawn_guid))
    {
      current->hovered = true;
    }
  }

  _needs_redraw = true;
}

std::optional<std::uint32_t> MapView::findCreatureSpawnAtCursor()
{
  if (!_world->hasCreatureSpawnsLoaded())
    return std::optional<std::uint32_t>();

  glm::mat4x4 const mv = model_view();
  glm::mat4x4 const proj = projection();
  glm::vec4 const vp(0.0f, 0.0f, float(width()), float(height()));

  // Build the cursor ray in WORLD space (window Y is bottom-up for unProject; Qt mouse Y is top-down).
  float const wx = float(_last_mouse_pos.x());
  float const wy = float(height()) - float(_last_mouse_pos.y());
  glm::vec3 const ray_near = glm::unProject(glm::vec3(wx, wy, 0.0f), mv, proj, vp);
  glm::vec3 const ray_far  = glm::unProject(glm::vec3(wx, wy, 1.0f), mv, proj, vp);
  glm::vec3 const ray_dir  = ray_far - ray_near;

  // 1) Prefer a hit on the actual 3D MODEL MESH: click the creature's body, not just its ground disc.
  //    ModelInstance::intersect ray-casts the animated triangles in the instance's own transform; we
  //    take the spawn whose mesh the cursor ray strikes nearest the camera.
  {
    math::ray const world_ray(ray_near, ray_dir);
    int const base_animtime = static_cast<int>(_world->model_animtime);
    float best_dist = std::numeric_limits<float>::max();
    std::optional<std::uint32_t> best_guid;
    for (auto& spawn : _world->creatureSpawns())
    {
      if (spawn.pending_delete || spawn.event_suppressed || !spawn.model_instance.has_value())
        continue;
      auto& inst = *spawn.model_instance;
      if (!inst.model.get() || !inst.model->finishedLoading() || inst.model->loading_failed())
        continue;
      selection_result hits;
      inst.intersect(mv, world_ray, &hits, base_animtime + spawn.animation_time_offset);
      for (auto const& h : hits)
      {
        if (h.first < best_dist)
        {
          best_dist = h.first;
          best_guid = spawn.guid;
        }
      }
    }
    if (best_guid)
      return best_guid;
  }

  // 2) Fallback: the ground selection-disc, so clicking the drawn circle still selects (handy when the
  //    body is off-screen or behind terrain -- e.g. a flyer whose disc sits on the ground below it).
  //    Same world centre (spawn.pos) and radius as the rendered marker; rank by relative distance from
  //    the centre (0 = centre, <1 = inside) so overlapping discs resolve to the most-centred one.
  float best_rel = 1.0f;
  std::optional<std::uint32_t> best_guid;
  for (auto const& spawn : _world->creatureSpawns())
  {
    if (spawn.pending_delete || spawn.event_suppressed)
      continue;
    float const ring_radius = spawn.selectionRingWorldRadius();
    if (std::abs(ray_dir.y) < 1e-6f)
      continue;
    float const t = (spawn.pos.y - ray_near.y) / ray_dir.y;
    if (t < 0.0f)
      continue;
    glm::vec3 const hit = ray_near + ray_dir * t;
    float const d = glm::length(glm::vec2(hit.x - spawn.pos.x, hit.z - spawn.pos.z));
    float const rel = d / ring_radius;
    if (rel < 1.0f && rel < best_rel)
    {
      best_rel = rel;
      best_guid = spawn.guid;
    }
  }
  return best_guid;
}

void MapView::updateCreatureSpawnHover(QPoint const& global_pos)
{
  if (terrainMode != editing_mode::creature || _dragging_creature_spawn || rightMouse)
  {
    setHoveredCreatureSpawn(std::optional<std::uint32_t>());
    QToolTip::hideText();
    return;
  }

  auto hovered_guid = findCreatureSpawnAtCursor();
  setHoveredCreatureSpawn(hovered_guid);

  if (!hovered_guid)
  {
    QToolTip::hideText();
    return;
  }

  auto const* spawn = _world->findCreatureSpawn(*hovered_guid);
  if (!spawn)
  {
    QToolTip::hideText();
    return;
  }

  QString name = QString::fromStdString(spawn->name.empty() ? std::string("<unnamed>") : spawn->name);
  QToolTip::showText(global_pos, QString("%1\nGUID: %2\nEntry: %3")
                               .arg(name)
                               .arg(spawn->guid)
                               .arg(spawn->entry), this);
}

bool MapView::tryStartCreatureSpawnDrag()
{
  bool const creature_editor_mode = terrainMode == editing_mode::creature;
  bool const legacy_object_mode = terrainMode == editing_mode::object && _draw_creature_spawns.get();
  if ((!creature_editor_mode && !legacy_object_mode) || !_world->hasCreatureSpawnsLoaded())
  {
    return false;
  }

  std::optional<std::uint32_t> best_guid = findCreatureSpawnAtCursor();

  if (!best_guid)
  {
    return false;
  }

  auto const* clicked_spawn = _world->findCreatureSpawn(*best_guid);
  if (!clicked_spawn)
  {
    return false;
  }

  if (!clicked_spawn->selected || selectedCreatureSpawnCount() <= 1)
  {
    // update_browser=false: rebuildCreatureBrowserList() repopulates a QListWidget over every spawn in
    // the world, which is a ~1s hitch at the moment you grab something. The drag end already rebuilds
    // it (mouseReleaseEvent), so the list syncs there instead of stalling the grab.
    setSelectedCreatureSpawn(best_guid, /*update_browser*/ false);
  }

  _creature_drag_anchor_pos = _cursor_pos;
  _creature_drag_initial_positions.clear();
  for (auto const& spawn : _world->creatureSpawns())
  {
    if (spawn.selected)
    {
      _creature_drag_initial_positions.emplace_back(spawn.guid, spawn.pos);
    }
  }

  _dragging_creature_spawn = true;
  _main_window->statusBar()->showMessage(QString("Dragging %1 creature spawn(s). Release mouse, then use Export SQL.")
                                           .arg(_creature_drag_initial_positions.size()), 4000);
  return true;
}

void MapView::translateSelectedCreatureSpawns(glm::vec3 const& delta)
{
  if (!_selected_creature_spawn_guid)
  {
    return;
  }

  auto apply_position = [](World::CreatureSpawnOverlay& spawn, glm::vec3 const& pos)
  {
    spawn.pos = pos;
    spawn.dirty = spawn.pending_create
               || glm::distance(spawn.pos, spawn.original_pos) > 0.01f
               || std::abs(spawn.orientation - spawn.original_orientation) > 0.01f;

    if (spawn.model_instance)
    {
      spawn.model_instance->pos = spawn.pos;
      spawn.model_instance->dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
      spawn.model_instance->recalcExtents();
    }
  };

  bool moved_any = false;
  for (auto& spawn : _world->creatureSpawns())
  {
    if (!spawn.selected)
    {
      continue;
    }

    apply_position(spawn, spawn.pos + delta);
    moved_any = true;
  }

  if (!moved_any)
  {
    if (auto* spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid))
    {
      apply_position(*spawn, spawn->pos + delta);
      moved_any = true;
    }
  }

  if (!moved_any)
  {
    return;
  }

  updateDatabaseStatus();
  rebuildCreatureBrowserList(true);
  refreshCreatureEditorKnobs();
}

void MapView::translateSelectedGameObjectSpawns(glm::vec3 const& delta)
{
  if (!_selected_gameobject_spawn_guid)
  {
    return;
  }

  auto apply_position = [](World::GameObjectSpawnOverlay& spawn, glm::vec3 const& pos)
  {
    spawn.pos = pos;
    spawn.dirty = spawn.pending_create
               || glm::distance(spawn.pos, spawn.original_pos) > 0.01f
               || std::abs(spawn.orientation - spawn.original_orientation) > 0.01f;

    if (spawn.model_instance)
    {
      spawn.model_instance->pos = spawn.pos;
      spawn.model_instance->dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
      spawn.model_instance->recalcExtents();
    }
  };

  bool moved_any = false;
  for (auto& spawn : _world->gameObjectSpawns())
  {
    if (!spawn.selected)
    {
      continue;
    }
    apply_position(spawn, spawn.pos + delta);
    moved_any = true;
  }

  if (!moved_any)
  {
    if (auto* spawn = _world->findGameObjectSpawn(*_selected_gameobject_spawn_guid))
    {
      apply_position(*spawn, spawn->pos + delta);
      moved_any = true;
    }
  }

  if (!moved_any)
  {
    return;
  }

  updateGameObjectBrowserStatus();
  rebuildGameObjectBrowserList(true);
  refreshGameObjectEditorKnobs();
}

void MapView::deleteSelectedCreatureSpawns()
{
  std::vector<std::uint32_t> deleted;

  auto mark = [&](World::CreatureSpawnOverlay& spawn)
  {
    if (spawn.pending_delete)
    {
      return;
    }
    spawn.pending_delete = true; // hidden from view/browser/picking; exported as DELETE
    spawn.dirty = true;          // count it as a pending change
    spawn.selected = false;
    spawn.hovered = false;
    deleted.push_back(spawn.guid);
  };

  for (auto& spawn : _world->creatureSpawns())
  {
    if (spawn.selected)
    {
      mark(spawn);
    }
  }

  if (deleted.empty() && _selected_creature_spawn_guid)
  {
    if (auto* spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid))
    {
      mark(*spawn);
    }
  }

  if (deleted.empty())
  {
    return;
  }

  _creature_delete_undo.push_back(deleted);
  setSelectedCreatureSpawn(std::nullopt, false);
  updateDatabaseStatus();
  rebuildCreatureBrowserList(true);
  refreshCreatureEditorKnobs();
  _main_window->statusBar()->showMessage(
    QString("Marked %1 creature spawn(s) for deletion (Ctrl+Z to undo)").arg(deleted.size()), 5000);
}

bool MapView::undoLastCreatureDelete()
{
  if (_creature_delete_undo.empty())
  {
    return false;
  }

  auto const guids = _creature_delete_undo.back();
  _creature_delete_undo.pop_back();

  for (auto guid : guids)
  {
    if (auto* spawn = _world->findCreatureSpawn(guid))
    {
      spawn->pending_delete = false;
      // Keep it dirty only if it still has real edits (or is a never-saved spawn).
      spawn->dirty = spawn->pending_create
                  || glm::distance(spawn->pos, spawn->original_pos) > 0.01f
                  || std::abs(spawn->orientation - spawn->original_orientation) > 0.01f;
    }
  }

  updateDatabaseStatus();
  rebuildCreatureBrowserList(true);
  refreshCreatureEditorKnobs();
  _main_window->statusBar()->showMessage(
    QString("Restored %1 creature spawn(s)").arg(guids.size()), 5000);
  return true;
}

// Turn every spawn currently being dragged by `degrees` about its own centre. Used by the wheel while
// a drag is in progress. Deliberately does NOT rebuild the browser list or the editor knobs -- those
// are the expensive calls that used to stall the drag; the orientation spinbox is nudged directly and
// the full resync happens on mouse-up like the position does.
void MapView::rotateDraggedSpawns(float degrees)
{
  auto const wrap360 = [](float v)
  {
    v = std::fmod(v, 360.0f);
    return v < 0.0f ? v + 360.0f : v;
  };

  if (_dragging_creature_spawn)
  {
    for (auto const& drag_state : _creature_drag_initial_positions)
    {
      auto* spawn = _world->findCreatureSpawn(drag_state.first);
      if (!spawn)
      {
        continue;
      }

      spawn->orientation = wrap360(spawn->orientation + degrees);
      spawn->dirty = spawn->pending_create
                  || glm::distance(spawn->pos, spawn->original_pos) > 0.01f
                  || std::abs(spawn->orientation - spawn->original_orientation) > 0.01f;

      if (spawn->model_instance)
      {
        spawn->model_instance->dir = glm::vec3(0.0f, spawn->orientation, 0.0f);
        spawn->model_instance->recalcExtents();
      }
      if (spawn->mount_instance)
      {
        spawn->mount_instance->dir = glm::vec3(0.0f, spawn->orientation, 0.0f);
        spawn->mount_instance->recalcExtents();
      }
    }

    if (_spawn_edit_orientation && _selected_creature_spawn_guid)
    {
      if (auto const* sel = _world->findCreatureSpawn(*_selected_creature_spawn_guid))
      {
        QSignalBlocker blocker(_spawn_edit_orientation);
        _spawn_edit_orientation->setValue(static_cast<double>(sel->orientation));
      }
    }
    return;
  }

  if (_dragging_gameobject_spawn)
  {
    for (auto const& drag_state : _gameobject_drag_initial_positions)
    {
      auto* spawn = _world->findGameObjectSpawn(drag_state.first);
      if (!spawn)
      {
        continue;
      }

      spawn->orientation = wrap360(spawn->orientation + degrees);
      spawn->dirty = spawn->pending_create
                  || glm::distance(spawn->pos, spawn->original_pos) > 0.01f
                  || std::abs(spawn->orientation - spawn->original_orientation) > 0.01f;

      if (spawn->model_instance)
      {
        spawn->model_instance->dir = glm::vec3(0.0f, spawn->orientation, 0.0f);
        spawn->model_instance->recalcExtents();
      }
      if (spawn->wmo_instance)
      {
        spawn->wmo_instance->dir = glm::vec3(0.0f, spawn->orientation, 0.0f);
        spawn->wmo_instance->recalcExtents();
      }
    }

    if (_go_spawn_edit_orientation && _selected_gameobject_spawn_guid)
    {
      if (auto const* sel = _world->findGameObjectSpawn(*_selected_gameobject_spawn_guid))
      {
        QSignalBlocker blocker(_go_spawn_edit_orientation);
        _go_spawn_edit_orientation->setValue(static_cast<double>(sel->orientation));
      }
    }
  }
}

void MapView::updateSelectedCreatureSpawnPosition(glm::vec3 const& pos)
{
  if (!_selected_creature_spawn_guid)
  {
    return;
  }

  auto apply_position = [](World::CreatureSpawnOverlay& spawn, glm::vec3 const& new_pos)
  {
    spawn.pos = new_pos;
    spawn.dirty = spawn.pending_create
               || glm::distance(spawn.pos, spawn.original_pos) > 0.01f
               || std::abs(spawn.orientation - spawn.original_orientation) > 0.01f;

    if (spawn.model_instance)
    {
      spawn.model_instance->pos = spawn.pos;
      spawn.model_instance->dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
      spawn.model_instance->recalcExtents();
    }
  };

  if (_dragging_creature_spawn && _creature_drag_anchor_pos && !_creature_drag_initial_positions.empty())
  {
    if (_creature_drag_initial_positions.size() == 1)
    {
      // Single spawn: snap it directly to the ground point under the cursor so it follows the mouse
      // exactly (instead of keeping the click offset).
      if (auto* spawn = _world->findCreatureSpawn(_creature_drag_initial_positions[0].first))
      {
        apply_position(*spawn, pos);
      }
    }
    else
    {
      // Multi-select: move the whole group together by the cursor delta to preserve their layout.
      glm::vec3 const delta = pos - *_creature_drag_anchor_pos;
      for (auto const& drag_state : _creature_drag_initial_positions)
      {
        if (auto* spawn = _world->findCreatureSpawn(drag_state.first))
        {
          apply_position(*spawn, drag_state.second + delta);
        }
      }
    }
  }
  else
  {
    auto* spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid);
    if (!spawn)
    {
      return;
    }

    apply_position(*spawn, pos);
  }

  // PERF: this runs on EVERY mouse-move while dragging, and rebuildCreatureBrowserList() clears and
  // repopulates a QListWidget over every creature spawn in the world -- that is what dropped the drag
  // to ~1 fps. All three calls are pure UI sync with no bearing on the spawn's position, so defer them
  // to the end of the drag (mouseReleaseEvent), where they run exactly once.
  if (_dragging_creature_spawn)
  {
    return;
  }

  updateDatabaseStatus();
  rebuildCreatureBrowserList(true);
  refreshCreatureEditorKnobs();
}

void MapView::showSelectedCreatureSpawnMenu(QPoint const& global_pos)
{
  if (!_selected_creature_spawn_guid)
  {
    return;
  }

  auto const* spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid);
  if (!spawn)
  {
    return;
  }

  QMenu menu(this);
  menu.addAction(QString("NPC: %1").arg(QString::fromStdString(spawn->name.empty() ? std::string("<unnamed>") : spawn->name)))->setEnabled(false);
  menu.addAction(QString("Unique ID: %1").arg(spawn->guid))->setEnabled(false);
  menu.addAction(QString("Entry: %1").arg(spawn->entry))->setEnabled(false);
  menu.addAction(QString("Display ID: %1").arg(spawn->display_id))->setEnabled(false);
  menu.addAction(QString("Position: %1, %2, %3")
                   .arg(spawn->pos.x, 0, 'f', 2)
                   .arg(spawn->pos.y, 0, 'f', 2)
                   .arg(spawn->pos.z, 0, 'f', 2))->setEnabled(false);
  menu.addSeparator();
  auto* jump_action = menu.addAction("Center camera here");
  auto* save_action = menu.addAction("Save pending creature changes");
  auto* chosen = menu.exec(global_pos);

  if (chosen == jump_action)
  {
    focus_camera_on_target(spawn->pos);
  }
  else if (chosen == save_action)
  {
    saveDirtyCreatureSpawns();
  }
}

void MapView::discardPendingCreatureSpawns()
{
  auto dirty_count = _world->dirtyCreatureSpawnCount();
  if (dirty_count == 0)
  {
    _main_window->statusBar()->showMessage("No creature spawn changes to discard", 4000);
    updateCreatureBrowserStatus();
    return;
  }

  _selected_creature_spawn_guid = std::nullopt;
  _hovered_creature_spawn_guid = std::nullopt;
  _dragging_creature_spawn = false;
  _creature_drag_anchor_pos = std::nullopt;
  _creature_drag_initial_positions.clear();
  _creature_delete_undo.clear();

  std::size_t removed_new = 0;
  std::size_t reverted_existing = 0;

  try
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _ (::gl, context());

    auto& spawns = _world->creatureSpawns();
    for (auto& spawn : spawns)
    {
      spawn.selected = false;
      spawn.hovered = false;

      if (!spawn.dirty || spawn.pending_create)
      {
        continue;
      }

      spawn.pos = spawn.original_pos;
      spawn.orientation = spawn.original_orientation;
      spawn.dirty = false;
      spawn.pending_delete = false; // restore any spawn that was marked for deletion
      if (spawn.model_instance)
      {
        spawn.model_instance->pos = spawn.pos;
        spawn.model_instance->dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
        spawn.model_instance->recalcExtents();
      }
      ++reverted_existing;
    }

    auto pending_begin = std::remove_if(spawns.begin(), spawns.end(),
      [&removed_new](World::CreatureSpawnOverlay const& spawn)
      {
        if (!spawn.pending_create)
        {
          return false;
        }

        ++removed_new;
        return true;
      });
    spawns.erase(pending_begin, spawns.end());
  }
  catch (std::exception const& ex)
  {
    _main_window->statusBar()->showMessage(QString("Failed to discard creature spawn changes: %1").arg(ex.what()), 7000);
    updateCreatureBrowserStatus();
    return;
  }
  catch (...)
  {
    _main_window->statusBar()->showMessage("Failed to discard creature spawn changes: unknown error", 7000);
    updateCreatureBrowserStatus();
    return;
  }

  rebuildCreatureBrowserList(false);
  refreshCreatureEditorKnobs();
  updateDatabaseStatus();
  _needs_redraw = true;

  _main_window->statusBar()->showMessage(
    QString("Discarded %1 pending creature spawn(s), reverted %2 edited spawn(s)")
      .arg(removed_new)
      .arg(reverted_existing),
    5000);
}

void MapView::saveDirtyCreatureSpawns()
{
  auto dirty_count = _world->dirtyCreatureSpawnCount();
  if (dirty_count == 0)
  {
    _main_window->statusBar()->showMessage("No creature spawn changes to export", 4000);
    updateCreatureBrowserStatus();
    return;
  }

  QDir project_dir(QString::fromStdString(Noggit::Project::CurrentProject::get()->ProjectPath));
  QString export_dir_path = project_dir.filePath("sql_exports/creature_spawns");
  QDir export_dir(export_dir_path);
  if (!export_dir.exists() && !project_dir.mkpath("sql_exports/creature_spawns"))
  {
    auto message = QString("Failed to create creature SQL export folder: %1").arg(export_dir_path);
    _main_window->statusBar()->showMessage(message, 6000);
    updateCreatureBrowserStatus(message);
    return;
  }

  QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
  QString file_name = QString("creature_updates_map%1_%2.sql").arg(_world->getMapID()).arg(timestamp);
  QString file_path = export_dir.filePath(file_name);

  QFile output(file_path);
  if (!output.open(QIODevice::WriteOnly | QIODevice::Text | QFile::Truncate))
  {
    auto message = QString("Failed to write creature SQL export: %1").arg(file_path);
    _main_window->statusBar()->showMessage(message, 6000);
    updateCreatureBrowserStatus(message);
    return;
  }

  QTextStream stream(&output);
  stream.setCodec("UTF-8");
  stream << "-- Noggit creature spawn update export\n";
  stream << "-- Map ID: " << _world->getMapID() << "\n";
  stream << "-- Generated: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
  stream << "\n";

  for (auto& spawn : _world->creatureSpawns())
  {
    if (!spawn.dirty)
    {
      continue;
    }

    if (spawn.pending_delete)
    {
      // A spawn created this session and then deleted never reached the DB -> nothing to export.
      if (spawn.pending_create)
      {
        continue;
      }
      stream << "-- GUID " << spawn.guid << " entry " << spawn.entry << " "
             << QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name) << "\n";
      stream << "DELETE FROM creature WHERE guid=" << spawn.guid << ";\n\n";
      continue;
    }

    auto server_pos = client_to_server_creature_position(spawn.pos, _world->mapIndex.hasAGlobalWMO());
    auto server_orientation = client_to_server_creature_orientation(spawn.orientation);
    stream << "-- GUID " << spawn.guid << " entry " << spawn.entry << " "
           << QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name)
           << "\n";

    if (spawn.pending_create)
    {
      stream << "-- Preview display ID: " << spawn.display_id << "\n";
      stream << "INSERT INTO creature (guid, id, map, position_x, position_y, position_z, orientation)\n"
             << "VALUES (" << spawn.guid << ", "
             << spawn.entry << ", "
             << _world->getMapID() << ", "
             << QString::number(server_pos.x, 'f', 6) << ", "
             << QString::number(server_pos.y, 'f', 6) << ", "
             << QString::number(server_pos.z, 'f', 6) << ", "
             << QString::number(server_orientation, 'f', 6) << ");\n\n";
    }
    else
    {
      stream << "UPDATE creature\n"
             << "SET position_x = " << QString::number(server_pos.x, 'f', 6) << ",\n"
             << "    position_y = " << QString::number(server_pos.y, 'f', 6) << ",\n"
             << "    position_z = " << QString::number(server_pos.z, 'f', 6) << ",\n"
             << "    orientation = " << QString::number(server_orientation, 'f', 6) << "\n"
             << "WHERE guid = " << spawn.guid << ";\n\n";
    }

    spawn.original_pos = spawn.pos;
    spawn.original_orientation = spawn.orientation;
    spawn.pending_create = false;
    spawn.dirty = false;
  }
  output.close();

  updateDatabaseStatus();
  rebuildCreatureBrowserList(true);
  _main_window->statusBar()->showMessage(QString("Creature spawn SQL exported: %1").arg(file_path), 7000);
}

void MapView::jumpToCreatureListItem(QListWidgetItem* item)
{
  if (!item)
  {
    return;
  }

  auto guid = static_cast<std::uint32_t>(item->data(Qt::UserRole).toULongLong());
  auto map_id = item->data(Qt::UserRole + 1).toInt();
  auto is_current_map_item = item->data(Qt::UserRole + 6).toBool();

  if (is_current_map_item)
  {
    setSelectedCreatureSpawn(guid, true);
    auto const* spawn = _world->findCreatureSpawn(guid);
    if (spawn)
    {
      focus_camera_on_target(spawn->pos);
    }
    return;
  }

  glm::vec3 target_pos = server_to_client_creature_position(item->data(Qt::UserRole + 2).toFloat(),
                                                            item->data(Qt::UserRole + 3).toFloat(),
                                                            item->data(Qt::UserRole + 4).toFloat(),
                                                            _world->mapIndex.hasAGlobalWMO());
  _main_window->jumpToMapPosition(map_id, target_pos, math::degrees(30.f), math::degrees(90.f), false);
}

// ---------------------------------------------------------------------------
// GameObject tool (mirrors the creature tool above; no model picker / new-spawn creation).
// ---------------------------------------------------------------------------

// Gameobject counterpart of refreshCreatureBrowserItems -- see that function for why.
void MapView::refreshGameObjectBrowserItems(std::vector<std::uint32_t> const& guids)
{
  if (!_gameobject_list_widget || guids.empty())
  {
    return;
  }

  QSignalBlocker blocker(_gameobject_list_widget);
  for (int i = 0; i < _gameobject_list_widget->count(); ++i)
  {
    auto* item = _gameobject_list_widget->item(i);
    if (!item)
    {
      continue;
    }

    auto const guid = static_cast<std::uint32_t>(item->data(Qt::UserRole).toULongLong());
    if (std::find(guids.begin(), guids.end(), guid) == guids.end())
    {
      continue;
    }

    if (auto const* spawn = _world->findGameObjectSpawn(guid))
    {
      QString prefix;
      if (spawn->selected)
      {
        prefix += "[selected] ";
      }
      if (spawn->dirty)
      {
        prefix += "[pending] ";
      }

      auto const name = QString::fromStdString(spawn->name.empty() ? std::string("<unnamed>") : spawn->name);
      item->setText(QString("%1%2 [entry %3] guid %4")
                      .arg(prefix).arg(name).arg(spawn->entry).arg(spawn->guid));
    }
  }
}

void MapView::rebuildGameObjectBrowserList(bool preserve_selection)
{
  if (!_gameobject_list_widget)
  {
    return;
  }

  auto selected_guid = preserve_selection ? _selected_gameobject_spawn_guid : std::optional<std::uint32_t>();
  auto search_text = _gameobject_search_field ? _gameobject_search_field->text().trimmed() : QString();
  auto search_lower = search_text.toLower();

  unsigned int camera_zone = (_gameobject_zone_filter && _gameobject_zone_filter->isChecked())
    ? _world->getZoneId(_camera.position) : 0u;
  bool const zone_only = camera_zone != 0u && camera_zone != static_cast<unsigned int>(-1);

  QSignalBlocker blocker(_gameobject_list_widget);
  _gameobject_list_widget->clear();

  for (auto const& spawn : _world->gameObjectSpawns())
  {
    if (spawn.pending_delete) // marked for deletion -> hidden from the list
    {
      continue;
    }
    if (zone_only)
    {
      unsigned int zone;
      auto cit = _gameobject_zone_cache.find(spawn.guid);
      if (cit != _gameobject_zone_cache.end())
      {
        zone = cit->second;
      }
      else
      {
        zone = _world->getZoneId(spawn.pos);
        if (zone != 0u && zone != static_cast<unsigned int>(-1))
          _gameobject_zone_cache[spawn.guid] = zone;
      }
      if (zone != camera_zone)
        continue;
    }
    auto name = QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name);
    auto entry_text = QString::number(spawn.entry);
    auto guid_text = QString::number(spawn.guid);

    if (!search_lower.isEmpty()
        && !name.toLower().contains(search_lower)
        && !entry_text.contains(search_lower)
        && !guid_text.contains(search_lower))
    {
      continue;
    }

    // Type filter (mirrors the gameobject model picker), looked up by spawn entry. Spawns whose template
    // type hasn't loaded yet are left visible.
    if (_gameobject_browser_type_filter)
    {
      auto const type_data = _gameobject_browser_type_filter->currentData();
      if (type_data.isValid())
      {
        auto fit = _gameobject_template_filter_type.find(spawn.entry);
        if (fit != _gameobject_template_filter_type.end()
            && fit->second != static_cast<std::uint32_t>(type_data.toULongLong()))
        {
          continue;
        }
      }
    }

    QString prefix;
    if (spawn.selected)
    {
      prefix += "[selected] ";
    }
    if (spawn.dirty)
    {
      prefix += "[pending] ";
    }

    auto* item = new QListWidgetItem(QString("%1%2 [entry %3] guid %4")
                                       .arg(prefix)
                                       .arg(name)
                                       .arg(spawn.entry)
                                       .arg(spawn.guid),
                                     _gameobject_list_widget);
    item->setData(Qt::UserRole, static_cast<qulonglong>(spawn.guid));
    item->setData(Qt::UserRole + 1, static_cast<int>(_world->getMapID()));
    item->setData(Qt::UserRole + 6, true);

    if (selected_guid && *selected_guid == spawn.guid)
    {
      _gameobject_list_widget->setCurrentItem(item);
    }
  }

  updateGameObjectBrowserStatus();
}

void MapView::updateGameObjectBrowserStatus(QString const& override_text)
{
  if (!_gameobject_browser_status)
  {
    return;
  }

  if (!override_text.isEmpty())
  {
    _gameobject_browser_status->setText(override_text);
    return;
  }

  QStringList parts;
  parts << QString("Current map spawns: %1").arg(_world->gameObjectSpawnCount());
  parts << QString("models: %1").arg(_world->gameObjectSpawnModelCount());

  auto dirty_count = _world->dirtyGameObjectSpawnCount();
  if (dirty_count > 0)
  {
    parts << QString("pending edits: %1").arg(dirty_count);
  }

  auto selected_count = selectedGameObjectSpawnCount();
  if (selected_count > 1)
  {
    parts << QString("selected: %1").arg(selected_count);
  }
  else if (_selected_gameobject_spawn_guid)
  {
    parts << QString("selected guid: %1").arg(*_selected_gameobject_spawn_guid);
  }

  _gameobject_browser_status->setText(parts.join(" | "));
}

std::size_t MapView::selectedGameObjectSpawnCount() const
{
  return static_cast<std::size_t>(std::count_if(_world->gameObjectSpawns().begin(),
                                                _world->gameObjectSpawns().end(),
    [](World::GameObjectSpawnOverlay const& spawn)
    {
      return spawn.selected;
    }));
}

void MapView::setSelectedGameObjectSpawn(std::optional<std::uint32_t> guid, bool update_browser)
{
  _selected_gameobject_spawn_guid = guid;

  // Same as the creature path: only the rows whose selected flag flips need new text.
  std::vector<std::uint32_t> retext;
  for (auto& spawn : _world->gameObjectSpawns())
  {
    bool const now_selected = guid && spawn.guid == *guid;
    if (spawn.selected != now_selected)
    {
      retext.push_back(spawn.guid);
    }
    spawn.selected = now_selected;
  }

  // Populate the "Edit/New GameObject" form from the selected spawn (empty = New).
  if (_gameobject_spawn_guid_field)
  {
    World::GameObjectSpawnOverlay const* sp = guid ? _world->findGameObjectSpawn(*guid) : nullptr;
    if (sp)
    {
      _gameobject_spawn_guid_field->setText(QString::number(sp->guid));
      _gameobject_spawn_entry_field->setText(QString::number(sp->entry));
      _gameobject_spawn_display_field->setText(QString::number(sp->display_id));
    }
    else
    {
      _gameobject_spawn_guid_field->clear();
      _gameobject_spawn_entry_field->clear();
      _gameobject_spawn_display_field->clear();
    }
  }

  if (update_browser)
  {
    refreshGameObjectBrowserItems(retext);
    highlightGameObjectBrowserSelection();
  }
  else
  {
    updateGameObjectBrowserStatus();
  }

  refreshGameObjectEditorKnobs();
}

void MapView::addGameObjectSpawnToSelection(std::uint32_t guid, bool update_browser)
{
  bool found = false;
  for (auto& spawn : _world->gameObjectSpawns())
  {
    if (spawn.guid == guid)
    {
      spawn.selected = true;
      found = true;
    }
  }

  if (!found)
  {
    return;
  }

  _selected_gameobject_spawn_guid = guid;

  if (update_browser)
  {
    refreshGameObjectBrowserItems({guid});
    highlightGameObjectBrowserSelection();
  }
  else
  {
    updateGameObjectBrowserStatus();
  }

  refreshGameObjectEditorKnobs();
}

void MapView::selectGameObjectSpawnsInArea(QRect const& rect, bool add_to_selection)
{
  _world->ensureGameObjectSpawnsLoaded();

  QRect const normalized_rect = rect.normalized();
  glm::mat4x4 const mv = model_view();
  glm::mat4x4 const proj = projection();
  glm::vec4 const vp(0.0f, 0.0f, float(width()), float(height()));

  if (!add_to_selection)
  {
    for (auto& spawn : _world->gameObjectSpawns())
    {
      spawn.selected = false;
    }
  }

  std::optional<std::uint32_t> primary_guid = add_to_selection ? _selected_gameobject_spawn_guid : std::optional<std::uint32_t>();

  for (auto& spawn : _world->gameObjectSpawns())
  {
    if (spawn.event_suppressed) // hidden by the Seasonal Events filter -> not box-selectable
    {
      continue;
    }
    glm::vec3 const screen = glm::project(spawn.pos, mv, proj, vp);
    if (screen.z < 0.0f || screen.z > 1.0f)
    {
      continue;
    }

    QPoint const point(static_cast<int>(std::lround(screen.x)),
                       static_cast<int>(std::lround(float(height()) - screen.y)));
    if (!normalized_rect.contains(point))
    {
      continue;
    }

    spawn.selected = true;
    if (!primary_guid)
    {
      primary_guid = spawn.guid;
    }
  }

  if (primary_guid)
  {
    _selected_gameobject_spawn_guid = primary_guid;
  }
  else if (!add_to_selection)
  {
    _selected_gameobject_spawn_guid = std::optional<std::uint32_t>();
  }

  rebuildGameObjectBrowserList(true);
  refreshGameObjectEditorKnobs();
  updateGameObjectBrowserStatus();
}

void MapView::refreshGameObjectEditorKnobs()
{
  if (!_go_spawn_edit_x)
    return;

  auto disable_all = [this]() {
    _gameobject_editor_info->setText("No spawn selected");
    _gameobject_editor_info->setStyleSheet("font-style: italic; color: #888; padding: 2px 0;");
    for (auto* w : {_go_spawn_edit_x, _go_spawn_edit_y, _go_spawn_edit_z, _go_spawn_edit_orientation})
      w->setEnabled(false);
  };

  if (!_selected_gameobject_spawn_guid)
  {
    disable_all();
    return;
  }

  auto selected_count = selectedGameObjectSpawnCount();
  if (selected_count > 1)
  {
    _gameobject_editor_info->setText(QString("%1 gameobject spawns selected\nPrimary GUID: %2")
                                       .arg(selected_count)
                                       .arg(*_selected_gameobject_spawn_guid));
    _gameobject_editor_info->setStyleSheet("font-weight: bold; padding: 2px 0;");
    for (auto* w : {_go_spawn_edit_x, _go_spawn_edit_y, _go_spawn_edit_z, _go_spawn_edit_orientation})
      w->setEnabled(false);
    return;
  }

  auto const* spawn = _world->findGameObjectSpawn(*_selected_gameobject_spawn_guid);
  if (!spawn)
  {
    disable_all();
    return;
  }

  QString name = QString::fromStdString(spawn->name.empty() ? std::string("<unnamed>") : spawn->name);
  _gameobject_editor_info->setText(
    QString("%1\nGUID: %2  Entry: %3").arg(name).arg(spawn->guid).arg(spawn->entry));
  _gameobject_editor_info->setStyleSheet("font-weight: bold; padding: 2px 0;");

  for (auto* w : {_go_spawn_edit_x, _go_spawn_edit_y, _go_spawn_edit_z, _go_spawn_edit_orientation})
    w->blockSignals(true);

  _go_spawn_edit_x->setValue(static_cast<double>(spawn->pos.x));
  _go_spawn_edit_y->setValue(static_cast<double>(spawn->pos.y));
  _go_spawn_edit_z->setValue(static_cast<double>(spawn->pos.z));
  _go_spawn_edit_orientation->setValue(static_cast<double>(spawn->orientation));

  for (auto* w : {_go_spawn_edit_x, _go_spawn_edit_y, _go_spawn_edit_z, _go_spawn_edit_orientation})
  {
    w->setEnabled(true);
    w->blockSignals(false);
  }
}

void MapView::setHoveredGameObjectSpawn(std::optional<std::uint32_t> guid)
{
  if (_hovered_gameobject_spawn_guid == guid)
  {
    return;
  }

  if (_hovered_gameobject_spawn_guid)
  {
    if (auto* previous = _world->findGameObjectSpawn(*_hovered_gameobject_spawn_guid))
    {
      previous->hovered = false;
    }
  }

  _hovered_gameobject_spawn_guid = guid;

  if (_hovered_gameobject_spawn_guid)
  {
    if (auto* current = _world->findGameObjectSpawn(*_hovered_gameobject_spawn_guid))
    {
      current->hovered = true;
    }
  }

  _needs_redraw = true;
}

std::optional<std::uint32_t> MapView::findGameObjectSpawnAtCursor()
{
  glm::mat4x4 const mv = model_view();
  glm::mat4x4 const proj = projection();
  glm::vec4 const vp(0.0f, 0.0f, float(width()), float(height()));

  float const wx = float(_last_mouse_pos.x());
  float const wy = float(height()) - float(_last_mouse_pos.y());
  glm::vec3 const ray_near = glm::unProject(glm::vec3(wx, wy, 0.0f), mv, proj, vp);
  glm::vec3 const ray_far  = glm::unProject(glm::vec3(wx, wy, 1.0f), mv, proj, vp);
  glm::vec3 const ray_dir  = ray_far - ray_near;

  // 1) Prefer a hit on the actual 3D MODEL MESH: click the object's body, not just its ground disc.
  {
    math::ray const world_ray(ray_near, ray_dir);
    int const base_animtime = static_cast<int>(_world->model_animtime);
    float best_dist = std::numeric_limits<float>::max();
    std::optional<std::uint32_t> best_guid;
    for (auto& spawn : _world->gameObjectSpawns())
    {
      if (spawn.pending_delete || spawn.event_suppressed || !spawn.model_instance.has_value())
        continue;
      auto& inst = *spawn.model_instance;
      if (!inst.model.get() || !inst.model->finishedLoading() || inst.model->loading_failed())
        continue;
      selection_result hits;
      inst.intersect(mv, world_ray, &hits, base_animtime + spawn.animation_time_offset);
      for (auto const& h : hits)
      {
        if (h.first < best_dist)
        {
          best_dist = h.first;
          best_guid = spawn.guid;
        }
      }
    }
    if (best_guid)
      return best_guid;
  }

  // 2) Fallback: the ground selection-disc, so clicking the drawn circle still selects.
  float best_rel = 1.0f;
  std::optional<std::uint32_t> best_guid;
  for (auto const& spawn : _world->gameObjectSpawns())
  {
    if (spawn.pending_delete || spawn.event_suppressed)
      continue;
    float ring_radius = 0.5f;
    if (spawn.model_instance.has_value())
    {
      ring_radius = spawn.model_instance.value().selectionRingRadius();
    }
    ring_radius = std::max(0.25f, ring_radius);
    if (std::abs(ray_dir.y) < 1e-6f)
      continue;
    float const t = (spawn.pos.y - ray_near.y) / ray_dir.y;
    if (t < 0.0f)
      continue;
    glm::vec3 const hit = ray_near + ray_dir * t;
    float const d = glm::length(glm::vec2(hit.x - spawn.pos.x, hit.z - spawn.pos.z));
    float const rel = d / ring_radius;
    if (rel < 1.0f && rel < best_rel)
    {
      best_rel = rel;
      best_guid = spawn.guid;
    }
  }
  return best_guid;
}

void MapView::updateGameObjectSpawnHover(QPoint const& global_pos)
{
  if (terrainMode != editing_mode::gameobject || _dragging_gameobject_spawn || rightMouse)
  {
    setHoveredGameObjectSpawn(std::optional<std::uint32_t>());
    QToolTip::hideText();
    return;
  }

  auto hovered_guid = findGameObjectSpawnAtCursor();
  setHoveredGameObjectSpawn(hovered_guid);

  if (!hovered_guid)
  {
    QToolTip::hideText();
    return;
  }

  auto const* spawn = _world->findGameObjectSpawn(*hovered_guid);
  if (!spawn)
  {
    QToolTip::hideText();
    return;
  }

  QString name = QString::fromStdString(spawn->name.empty() ? std::string("<unnamed>") : spawn->name);
  QToolTip::showText(global_pos, QString("%1\nGUID: %2\nEntry: %3")
                               .arg(name)
                               .arg(spawn->guid)
                               .arg(spawn->entry), this);
}

bool MapView::tryStartGameObjectSpawnDrag()
{
  if (terrainMode != editing_mode::gameobject)
  {
    return false;
  }

  std::optional<std::uint32_t> best_guid = findGameObjectSpawnAtCursor();

  if (!best_guid)
  {
    return false;
  }

  auto const* clicked_spawn = _world->findGameObjectSpawn(*best_guid);
  if (!clicked_spawn)
  {
    return false;
  }

  if (!clicked_spawn->selected || selectedGameObjectSpawnCount() <= 1)
  {
    // Same as the creature path: skip the full browser-list rebuild on grab, it happens on release.
    setSelectedGameObjectSpawn(best_guid, /*update_browser*/ false);
  }

  _gameobject_drag_anchor_pos = _cursor_pos;
  _gameobject_drag_initial_positions.clear();
  for (auto const& spawn : _world->gameObjectSpawns())
  {
    if (spawn.selected)
    {
      _gameobject_drag_initial_positions.emplace_back(spawn.guid, spawn.pos);
    }
  }

  _dragging_gameobject_spawn = true;
  _main_window->statusBar()->showMessage(QString("Dragging %1 gameobject spawn(s). Release mouse, then use Export SQL.")
                                           .arg(_gameobject_drag_initial_positions.size()), 4000);
  return true;
}

void MapView::deleteSelectedGameObjectSpawns()
{
  std::vector<std::uint32_t> deleted;

  auto mark = [&](World::GameObjectSpawnOverlay& spawn)
  {
    if (spawn.pending_delete)
    {
      return;
    }
    spawn.pending_delete = true; // hidden from view/browser/picking; exported as DELETE
    spawn.dirty = true;          // count it as a pending change
    spawn.selected = false;
    spawn.hovered = false;
    deleted.push_back(spawn.guid);
  };

  for (auto& spawn : _world->gameObjectSpawns())
  {
    if (spawn.selected)
    {
      mark(spawn);
    }
  }

  if (deleted.empty() && _selected_gameobject_spawn_guid)
  {
    if (auto* spawn = _world->findGameObjectSpawn(*_selected_gameobject_spawn_guid))
    {
      mark(*spawn);
    }
  }

  if (deleted.empty())
  {
    return;
  }

  _gameobject_delete_undo.push_back(deleted);
  setSelectedGameObjectSpawn(std::nullopt, false);
  updateGameObjectBrowserStatus();
  rebuildGameObjectBrowserList(true);
  refreshGameObjectEditorKnobs();
  _main_window->statusBar()->showMessage(
    QString("Marked %1 gameobject spawn(s) for deletion (Ctrl+Z to undo)").arg(deleted.size()), 5000);
}

bool MapView::undoLastGameObjectDelete()
{
  if (_gameobject_delete_undo.empty())
  {
    return false;
  }

  auto const guids = _gameobject_delete_undo.back();
  _gameobject_delete_undo.pop_back();

  for (auto guid : guids)
  {
    if (auto* spawn = _world->findGameObjectSpawn(guid))
    {
      spawn->pending_delete = false;
      // Keep it dirty only if it still has real edits (or is a never-saved spawn).
      spawn->dirty = spawn->pending_create
                  || glm::distance(spawn->pos, spawn->original_pos) > 0.01f
                  || std::abs(spawn->orientation - spawn->original_orientation) > 0.01f;
    }
  }

  updateGameObjectBrowserStatus();
  rebuildGameObjectBrowserList(true);
  refreshGameObjectEditorKnobs();
  _main_window->statusBar()->showMessage(
    QString("Restored %1 gameobject spawn(s)").arg(guids.size()), 5000);
  return true;
}

void MapView::updateSelectedGameObjectSpawnPosition(glm::vec3 const& pos)
{
  if (!_selected_gameobject_spawn_guid)
  {
    return;
  }

  auto apply_position = [](World::GameObjectSpawnOverlay& spawn, glm::vec3 const& new_pos)
  {
    spawn.pos = new_pos;
    spawn.dirty = spawn.pending_create
               || glm::distance(spawn.pos, spawn.original_pos) > 0.01f
               || std::abs(spawn.orientation - spawn.original_orientation) > 0.01f;

    if (spawn.model_instance)
    {
      spawn.model_instance->pos = spawn.pos;
      spawn.model_instance->dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
      spawn.model_instance->recalcExtents();
    }
  };

  if (_dragging_gameobject_spawn && _gameobject_drag_anchor_pos && !_gameobject_drag_initial_positions.empty())
  {
    if (_gameobject_drag_initial_positions.size() == 1)
    {
      // Single spawn: snap directly under the cursor's ground point.
      if (auto* spawn = _world->findGameObjectSpawn(_gameobject_drag_initial_positions[0].first))
      {
        apply_position(*spawn, pos);
      }
    }
    else
    {
      glm::vec3 const delta = pos - *_gameobject_drag_anchor_pos;
      for (auto const& drag_state : _gameobject_drag_initial_positions)
      {
        if (auto* spawn = _world->findGameObjectSpawn(drag_state.first))
        {
          apply_position(*spawn, drag_state.second + delta);
        }
      }
    }
  }
  else
  {
    auto* spawn = _world->findGameObjectSpawn(*_selected_gameobject_spawn_guid);
    if (!spawn)
    {
      return;
    }

    apply_position(*spawn, pos);
  }

  // PERF: same as the creature path -- rebuildGameObjectBrowserList() repopulates a list widget over
  // every gameobject spawn on every mouse-move. Deferred to the drag end.
  if (_dragging_gameobject_spawn)
  {
    return;
  }

  updateGameObjectBrowserStatus();
  rebuildGameObjectBrowserList(true);
  refreshGameObjectEditorKnobs();
}

void MapView::showSelectedGameObjectSpawnMenu(QPoint const& global_pos)
{
  if (!_selected_gameobject_spawn_guid)
  {
    return;
  }

  auto const* spawn = _world->findGameObjectSpawn(*_selected_gameobject_spawn_guid);
  if (!spawn)
  {
    return;
  }

  QMenu menu(this);
  menu.addAction(QString("GameObject: %1").arg(QString::fromStdString(spawn->name.empty() ? std::string("<unnamed>") : spawn->name)))->setEnabled(false);
  menu.addAction(QString("Unique ID: %1").arg(spawn->guid))->setEnabled(false);
  menu.addAction(QString("Entry: %1").arg(spawn->entry))->setEnabled(false);
  menu.addAction(QString("Display ID: %1").arg(spawn->display_id))->setEnabled(false);
  menu.addAction(QString("Position: %1, %2, %3")
                   .arg(spawn->pos.x, 0, 'f', 2)
                   .arg(spawn->pos.y, 0, 'f', 2)
                   .arg(spawn->pos.z, 0, 'f', 2))->setEnabled(false);
  menu.addSeparator();
  auto* jump_action = menu.addAction("Center camera here");
  auto* save_action = menu.addAction("Save pending gameobject changes");
  auto* chosen = menu.exec(global_pos);

  if (chosen == jump_action)
  {
    focus_camera_on_target(spawn->pos);
  }
  else if (chosen == save_action)
  {
    saveDirtyGameObjectSpawns();
  }
}

void MapView::discardPendingGameObjectSpawns()
{
  auto dirty_count = _world->dirtyGameObjectSpawnCount();
  if (dirty_count == 0)
  {
    _main_window->statusBar()->showMessage("No gameobject spawn changes to discard", 4000);
    updateGameObjectBrowserStatus();
    return;
  }

  _selected_gameobject_spawn_guid = std::nullopt;
  _hovered_gameobject_spawn_guid = std::nullopt;
  _dragging_gameobject_spawn = false;
  _gameobject_drag_anchor_pos = std::nullopt;
  _gameobject_drag_initial_positions.clear();
  _gameobject_delete_undo.clear();

  std::size_t removed_new = 0;
  std::size_t reverted_existing = 0;

  try
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _ (::gl, context());

    auto& spawns = _world->gameObjectSpawns();
    for (auto& spawn : spawns)
    {
      spawn.selected = false;
      spawn.hovered = false;

      if (!spawn.dirty || spawn.pending_create)
      {
        continue;
      }

      spawn.pos = spawn.original_pos;
      spawn.orientation = spawn.original_orientation;
      spawn.dirty = false;
      spawn.pending_delete = false; // restore any spawn that was marked for deletion
      if (spawn.model_instance)
      {
        spawn.model_instance->pos = spawn.pos;
        spawn.model_instance->dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
        spawn.model_instance->recalcExtents();
      }
      ++reverted_existing;
    }

    auto pending_begin = std::remove_if(spawns.begin(), spawns.end(),
      [&removed_new](World::GameObjectSpawnOverlay const& spawn)
      {
        if (!spawn.pending_create)
        {
          return false;
        }

        ++removed_new;
        return true;
      });
    spawns.erase(pending_begin, spawns.end());
  }
  catch (std::exception const& ex)
  {
    _main_window->statusBar()->showMessage(QString("Failed to discard gameobject spawn changes: %1").arg(ex.what()), 7000);
    updateGameObjectBrowserStatus();
    return;
  }
  catch (...)
  {
    _main_window->statusBar()->showMessage("Failed to discard gameobject spawn changes: unknown error", 7000);
    updateGameObjectBrowserStatus();
    return;
  }

  rebuildGameObjectBrowserList(false);
  refreshGameObjectEditorKnobs();
  updateGameObjectBrowserStatus();
  _needs_redraw = true;

  _main_window->statusBar()->showMessage(
    QString("Discarded %1 pending gameobject spawn(s), reverted %2 edited spawn(s)")
      .arg(removed_new)
      .arg(reverted_existing),
    5000);
}

void MapView::saveDirtyGameObjectSpawns()
{
  auto dirty_count = _world->dirtyGameObjectSpawnCount();
  if (dirty_count == 0)
  {
    _main_window->statusBar()->showMessage("No gameobject spawn changes to export", 4000);
    updateGameObjectBrowserStatus();
    return;
  }

  QDir project_dir(QString::fromStdString(Noggit::Project::CurrentProject::get()->ProjectPath));
  QString export_dir_path = project_dir.filePath("sql_exports/gameobject_spawns");
  QDir export_dir(export_dir_path);
  if (!export_dir.exists() && !project_dir.mkpath("sql_exports/gameobject_spawns"))
  {
    auto message = QString("Failed to create gameobject SQL export folder: %1").arg(export_dir_path);
    _main_window->statusBar()->showMessage(message, 6000);
    updateGameObjectBrowserStatus(message);
    return;
  }

  QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
  QString file_name = QString("gameobject_updates_map%1_%2.sql").arg(_world->getMapID()).arg(timestamp);
  QString file_path = export_dir.filePath(file_name);

  QFile output(file_path);
  if (!output.open(QIODevice::WriteOnly | QIODevice::Text | QFile::Truncate))
  {
    auto message = QString("Failed to write gameobject SQL export: %1").arg(file_path);
    _main_window->statusBar()->showMessage(message, 6000);
    updateGameObjectBrowserStatus(message);
    return;
  }

  QTextStream stream(&output);
  stream.setCodec("UTF-8");
  stream << "-- Noggit gameobject spawn update export\n";
  stream << "-- Map ID: " << _world->getMapID() << "\n";
  stream << "-- Generated: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
  stream << "\n";

  for (auto& spawn : _world->gameObjectSpawns())
  {
    if (!spawn.dirty)
    {
      continue;
    }

    if (spawn.pending_delete)
    {
      // A spawn created this session and then deleted never reached the DB -> nothing to export.
      if (spawn.pending_create)
      {
        continue;
      }
      stream << "-- GUID " << spawn.guid << " entry " << spawn.entry << " "
             << QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name) << "\n";
      stream << "DELETE FROM gameobject WHERE guid=" << spawn.guid << ";\n\n";
      continue;
    }

    auto server_pos = client_to_server_creature_position(spawn.pos, _world->mapIndex.hasAGlobalWMO());
    auto server_orientation = client_to_server_creature_orientation(spawn.orientation);
    stream << "-- GUID " << spawn.guid << " entry " << spawn.entry << " "
           << QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name)
           << "\n";

    if (spawn.pending_create)
    {
      stream << "-- Preview display ID: " << spawn.display_id << "\n";
      stream << "INSERT INTO gameobject (guid, id, map, position_x, position_y, position_z, orientation)\n"
             << "VALUES (" << spawn.guid << ", "
             << spawn.entry << ", "
             << _world->getMapID() << ", "
             << QString::number(server_pos.x, 'f', 6) << ", "
             << QString::number(server_pos.y, 'f', 6) << ", "
             << QString::number(server_pos.z, 'f', 6) << ", "
             << QString::number(server_orientation, 'f', 6) << ");\n\n";
    }
    else
    {
      stream << "UPDATE gameobject\n"
             << "SET position_x = " << QString::number(server_pos.x, 'f', 6) << ",\n"
             << "    position_y = " << QString::number(server_pos.y, 'f', 6) << ",\n"
             << "    position_z = " << QString::number(server_pos.z, 'f', 6) << ",\n"
             << "    orientation = " << QString::number(server_orientation, 'f', 6) << "\n"
             << "WHERE guid = " << spawn.guid << ";\n\n";
    }

    spawn.original_pos = spawn.pos;
    spawn.original_orientation = spawn.orientation;
    spawn.pending_create = false;
    spawn.dirty = false;
  }
  output.close();

  updateGameObjectBrowserStatus();
  rebuildGameObjectBrowserList(true);
  _main_window->statusBar()->showMessage(QString("GameObject spawn SQL exported: %1").arg(file_path), 7000);
}

void MapView::jumpToGameObjectListItem(QListWidgetItem* item)
{
  if (!item)
  {
    return;
  }

  auto guid = static_cast<std::uint32_t>(item->data(Qt::UserRole).toULongLong());
  auto is_current_map_item = item->data(Qt::UserRole + 6).toBool();

  if (is_current_map_item)
  {
    setSelectedGameObjectSpawn(guid, true);
    auto const* spawn = _world->findGameObjectSpawn(guid);
    if (spawn)
    {
      focus_camera_on_target(spawn->pos);
    }
    return;
  }
}

void MapView::setupHotkeys()
{

  addHotkey ( Qt::Key_F1
    , MOD_shift
    , [this]
              {
                if (alloff)
                {
                  alloff_models = _draw_models.get();
                  alloff_doodads = _draw_wmo_doodads.get();
                  alloff_contour = _draw_contour.get();
                  alloff_climb = _draw_climb.get();
                  alloff_vertex_color = _draw_vertex_color.get();
                  alloff_baked_shadows = _draw_baked_shadows.get();
                  alloff_wmo = _draw_wmo.get();
                  alloff_fog = _draw_fog.get();
                  alloff_terrain = _draw_terrain.get();

                  _draw_models.set (false);
                  _draw_wmo_doodads.set (false);
                  _draw_contour.set (true);
                  _draw_climb.set (false);
                  _draw_vertex_color.set(true);
                  _draw_baked_shadows.set(true);
                  _draw_wmo.set (false);
                  _draw_terrain.set (true);
                  _draw_fog.set (false);
                }
                else
                {
                  _draw_models.set (alloff_models);
                  _draw_wmo_doodads.set (alloff_doodads);
                  _draw_contour.set (alloff_contour);
                  _draw_climb.set(alloff_climb);
                  _draw_vertex_color.set(alloff_vertex_color);
                  _draw_baked_shadows.set(alloff_baked_shadows);
                  _draw_wmo.set (alloff_wmo);
                  _draw_terrain.set (alloff_terrain);
                  _draw_fog.set (alloff_fog);
                }
                alloff = !alloff;
              }
  );

  addHotkey ( Qt::Key_C
    , MOD_ctrl
    , [this]
              {
                objectEditor->copy_current_selection(_world.get());
              }
    , [this] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );
  /*
  addHotkey ( Qt::Key_C
    , MOD_none
    , [this]
              {
                objectEditor->copy_current_selection(_world.get());
              }
    , [this] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );*/

  addHotkey ( Qt::Key_V
    , MOD_ctrl
    ,
              [this]
              {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_ADDED);
                objectEditor->pasteObject (_cursor_pos, _camera.position, _world.get(), &_object_paste_params);
                NOGGIT_ACTION_MGR->endAction();
              }
    , [this] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );
  /*
  addHotkey ( Qt::Key_V
    , MOD_none
    , [this]
              {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_ADDED);
                objectEditor->pasteObject (_cursor_pos, _camera.position, _world.get(), &_object_paste_params);
                NOGGIT_ACTION_MGR->endAction();
              }
    , [this] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );*/
  addHotkey ( Qt::Key_V
    , MOD_shift
    , [this] { objectEditor->import_last_model_from_wmv(eMODEL); }
    , [this] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );
  addHotkey ( Qt::Key_V
    , MOD_alt
    , [this] { objectEditor->import_last_model_from_wmv(eWMO); }
    , [this] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_C
    , MOD_none
    , [this]
    {
      NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eVERTEX_SELECTION);
      _world->clearVertexSelection();
      NOGGIT_ACTION_MGR->endAction();
    }
    , [this] { return terrainMode == editing_mode::ground && !NOGGIT_CUR_ACTION; }
  );

  addHotkey( Qt::Key_B
    , MOD_ctrl
    , [this]
             {
               NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_ADDED);
               objectEditor->copy_current_selection(_world.get());
               objectEditor->pasteObject(_cursor_pos, _camera.position, _world.get(), &_object_paste_params);
               NOGGIT_ACTION_MGR->endAction();
             }
    , [this] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_Y
    , MOD_none
    , [this] { terrainTool->nextType(); }
    , [this] { return terrainMode == editing_mode::ground && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_Y
    , MOD_none
    , [this] { flattenTool->nextFlattenType(); }
    , [this] { return terrainMode == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_T
    , MOD_none
    , [&]
              {
                flattenTool->toggleFlattenAngle();
              }
    , [&] { return terrainMode == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_T
    , MOD_space
    , [&]
              {
                _left_sec_toolbar->nextFlattenMode(this);
                flattenTool->nextFlattenMode();
              }
    , [&] { return terrainMode == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_T
    , MOD_none
    , [&]
              {
                texturingTool->toggle_tool();
              }
    , [&] { return terrainMode == editing_mode::paint && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_T
    , MOD_none
    , [&]
              {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_HOLES);
                _world->setHoleADT (_camera.position, false);
                NOGGIT_ACTION_MGR->endAction();
              }
    , [&]
              {
                return terrainMode == editing_mode::holes && !NOGGIT_CUR_ACTION;
              }
  );

  addHotkey ( Qt::Key_T
    , MOD_alt
    , [&]
              {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_HOLES);
                _world->setHoleADT (_camera.position, true);
                NOGGIT_ACTION_MGR->endAction();
              }
    , [&] { return terrainMode == editing_mode::holes && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_T
    , MOD_none
    , [&]
              {
                guiWater->toggle_angled_mode();
              }
    , [&] { return terrainMode == editing_mode::water && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_T
    , MOD_none
    , [&]
              {
                objectEditor->togglePasteMode();
              }
    , [&] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );


  addHotkey ( Qt::Key_H
    , MOD_none
    , [&]
              {
                if (_world->has_selection())
                {
                  for (auto& selection : _world->current_selection())
                  {
                    if (selection.index() != eEntry_Object)
                      continue;

                    auto obj = std::get<selected_object_type>(selection);

                    if (obj->which() == eMODEL)
                    {
                      static_cast<ModelInstance*>(obj)->model->toggle_visibility();
                    }
                    else if (obj->which() == eWMO)
                    {
                      static_cast<WMOInstance*>(obj)->wmo->toggle_visibility();
                    }
                  }
                }
              }
    , [&] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_H
    , MOD_space
    , [&]
              {
                _draw_hidden_models.toggle();
              }
    , [&] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey(Qt::Key_R
    , MOD_space
    , [&]
            {
              texturingTool->toggle_brush_level_min_max();
            }
    , [&] { return terrainMode == editing_mode::paint && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_H
    , MOD_shift
    , [&]
              {
                ModelManager::clear_hidden_models();
                WMOManager::clear_hidden_wmos();
              }
    , [&] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_F
    , MOD_space
    , [&]
              {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
                terrainTool->flattenVertices (_world.get());
                NOGGIT_ACTION_MGR->endAction();

              }
    , [&] { return terrainMode == editing_mode::ground && !NOGGIT_CUR_ACTION; }
  );
  addHotkey ( Qt::Key_F
    , MOD_space
    , [&]
              {
                flattenTool->toggleFlattenLock();
              }
    , [&] { return terrainMode == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; }
  );
  addHotkey ( Qt::Key_F
    , MOD_none
    , [&]
              {
                flattenTool->lockPos (_cursor_pos);
              }
    , [&] { return terrainMode == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; }
  );
  addHotkey ( Qt::Key_F
    , MOD_space
    , [&]
              {
                guiWater->toggle_lock();
              }
    , [&] { return terrainMode == editing_mode::water && !NOGGIT_CUR_ACTION; }
  );
  addHotkey( Qt::Key_F
    , MOD_none
    , [&]
             {
               guiWater->lockPos(_cursor_pos);
             }
    , [&] { return terrainMode == editing_mode::water && !NOGGIT_CUR_ACTION; }
  );
  addHotkey ( Qt::Key_F
    , MOD_none
    , [&]
              {

                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
                _world->set_selected_models_pos(_cursor_pos);
                _rotation_editor_need_update = true;
                NOGGIT_ACTION_MGR->endAction();
              }
    , [&] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey (Qt::Key_Plus, MOD_alt, [this] { terrainTool->changeRadius(0.01f); }
    , [this] { return terrainMode == editing_mode::ground && !NOGGIT_CUR_ACTION; });

  addHotkey (Qt::Key_Plus, MOD_alt, [this] { flattenTool->changeRadius(0.01f); }
    , [this] { return terrainMode == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; });

  addHotkey ( Qt::Key_Plus
    , MOD_alt
    , [&]
              {
                texturingTool->change_radius(0.1f);
              }
    , [this] { return terrainMode == editing_mode::paint && !NOGGIT_CUR_ACTION; }
  );

  addHotkey (Qt::Key_Minus, MOD_alt, [this] { terrainTool->changeRadius(-0.01f); }
    , [this] { return terrainMode == editing_mode::ground && !NOGGIT_CUR_ACTION; });

  addHotkey (Qt::Key_Minus, MOD_alt, [this] { flattenTool->changeRadius(-0.01f); }
    , [this] { return terrainMode == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; });

  addHotkey ( Qt::Key_Minus
    , MOD_alt
    , [&]
              {
                texturingTool->change_radius(-0.1f);
              }
    , [this] { return terrainMode == editing_mode::paint && !NOGGIT_CUR_ACTION; }
  );

  addHotkey (Qt::Key_1, MOD_shift, [this] { _camera.move_speed = 15.0f; });
  addHotkey (Qt::Key_2, MOD_shift, [this] { _camera.move_speed = 50.0f; });
  addHotkey (Qt::Key_3, MOD_shift, [this] { _camera.move_speed = 200.0f; });
  addHotkey (Qt::Key_4, MOD_shift, [this] { _camera.move_speed = 800.0f; });
  addHotkey (Qt::Key_1, MOD_alt, [this] { texturingTool->set_brush_level(0.0f); });
  addHotkey (Qt::Key_2, MOD_alt, [this] { texturingTool->set_brush_level(255.0f* 0.25f); });
  addHotkey (Qt::Key_3, MOD_alt, [this] { texturingTool->set_brush_level(255.0f* 0.5f); });
  addHotkey (Qt::Key_4, MOD_alt, [this] { texturingTool->set_brush_level(255.0f* 0.75f); });
  addHotkey (Qt::Key_5, MOD_alt, [this] { texturingTool->set_brush_level(255.0f); });

  addHotkey(Qt::Key_1, MOD_none, [this] { set_editing_mode(editing_mode::ground); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_2, MOD_none, [this] { set_editing_mode (editing_mode::flatten_blur); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_3, MOD_none, [this] { set_editing_mode (editing_mode::paint); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_4, MOD_none, [this] { set_editing_mode (editing_mode::holes); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_5, MOD_none, [this] { set_editing_mode (editing_mode::areaid); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_6, MOD_none, [this] { set_editing_mode (editing_mode::flags); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_7, MOD_none, [this] { set_editing_mode (editing_mode::water); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_8, MOD_none, [this] { set_editing_mode (editing_mode::mccv); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_9, MOD_none, [this] { set_editing_mode (editing_mode::object); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });

  addHotkey(Qt::Key_0, MOD_ctrl, [this] { change_selected_wmo_doodadset(0); });
  addHotkey(Qt::Key_1, MOD_ctrl, [this] { change_selected_wmo_doodadset(1); });
  addHotkey(Qt::Key_2, MOD_ctrl, [this] { change_selected_wmo_doodadset(2); });
  addHotkey(Qt::Key_3, MOD_ctrl, [this] { change_selected_wmo_doodadset(3); });
  addHotkey(Qt::Key_4, MOD_ctrl, [this] { change_selected_wmo_doodadset(4); });
  addHotkey(Qt::Key_5, MOD_ctrl, [this] { change_selected_wmo_doodadset(5); });
  addHotkey(Qt::Key_6, MOD_ctrl, [this] { change_selected_wmo_doodadset(6); });
  addHotkey(Qt::Key_7, MOD_ctrl, [this] { change_selected_wmo_doodadset(7); });
  addHotkey(Qt::Key_8, MOD_ctrl, [this] { change_selected_wmo_doodadset(8); });
  addHotkey(Qt::Key_9, MOD_ctrl, [this] { change_selected_wmo_doodadset(9); });

  addHotkey(Qt::Key_Escape, MOD_none, [this] { _main_window->close(); });
}

void MapView::setupMinimap()
{
  _minimap = new Noggit::Ui::minimap_widget(this);
  _minimap_dock = new QDockWidget("Minimap", this);
  _minimap_dock->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
  _minimap_dock->setFixedSize(_minimap->sizeHint());
  _minimap_dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);

  _minimap->world (_world.get());
  _minimap->camera (&_camera);
  _minimap->draw_boundaries (_show_minimap_borders.get());
  _minimap->draw_skies (_show_minimap_skies.get());
  _minimap->set_resizeable(true);

  connect ( _minimap, &Noggit::Ui::minimap_widget::map_clicked
    , [this] (glm::vec3 const& pos)
            {
              move_camera_with_auto_height (pos);
            }
  );

  _minimap_dock->setFeatures ( QDockWidget::DockWidgetMovable
                               | QDockWidget::DockWidgetFloatable
                               | QDockWidget::DockWidgetClosable
  );
  auto minimap_scroll_area = new QScrollArea(_minimap_dock);
  minimap_scroll_area->setWidget(_minimap);
  minimap_scroll_area->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);

  _minimap_dock->setWidget(minimap_scroll_area);
  _main_window->addDockWidget (Qt::LeftDockWidgetArea, _minimap_dock);
  _minimap_dock->setVisible (false);
  _minimap_dock->setFloating(true);
  _minimap_dock->move(_main_window->rect().center() - _minimap->rect().center());


  connect(this, &QObject::destroyed, _minimap_dock, &QObject::deleteLater);
  connect(this, &QObject::destroyed, _minimap, &QObject::deleteLater);

  connect ( &_show_minimap_window, &Noggit::BoolToggleProperty::changed
    , _minimap_dock, [this]
            {
              if (!ui_hidden)
                _minimap_dock->setVisible(_show_minimap_window.get());
            }
  );


  connect ( _minimap_dock, &QDockWidget::visibilityChanged
    , &_show_minimap_window, &Noggit::BoolToggleProperty::set
  );

  connect ( &_show_minimap_borders, &Noggit::BoolToggleProperty::changed
    , [this]
            {
              _minimap->draw_boundaries(_show_minimap_borders.get());
            }
  );

  connect ( &_show_minimap_skies, &Noggit::BoolToggleProperty::changed
    , [this]
            {
              _minimap->draw_skies(_show_minimap_skies.get());
            }
  );

}

void MapView::createGUI()
{
  LogDebug << "MapView::createGUI begin" << std::endl;
  // Combined dock
  _tool_panel_dock = new Noggit::Ui::Tools::ToolPanel(this);
  _tool_panel_dock->setFeatures(QDockWidget::DockWidgetMovable
                                | QDockWidget::DockWidgetFloatable);
  _tool_panel_dock->setAllowedAreas(Qt::RightDockWidgetArea);

  connect(this, &QObject::destroyed, _tool_panel_dock, &QObject::deleteLater);
  _main_window->addDockWidget(Qt::RightDockWidgetArea, _tool_panel_dock);

  // These calls need to be correctly ordered in order to work with the toolbar.
  // TODO: fix

  setupRaiseLowerUi();
  LogDebug << "MapView::createGUI setupRaiseLowerUi done" << std::endl;
  setupFlattenBlurUi();
  LogDebug << "MapView::createGUI setupFlattenBlurUi done" << std::endl;
  setupTexturePainterUi();
  LogDebug << "MapView::createGUI setupTexturePainterUi done" << std::endl;
  setupHoleCutterUi();
  LogDebug << "MapView::createGUI setupHoleCutterUi done" << std::endl;
  setupAreaDesignatorUi();
  LogDebug << "MapView::createGUI setupAreaDesignatorUi done" << std::endl;
  setupFlagUi();
  LogDebug << "MapView::createGUI setupFlagUi done" << std::endl;
  setupWaterEditorUi();
  LogDebug << "MapView::createGUI setupWaterEditorUi done" << std::endl;
  setupVertexPainterUi();
  LogDebug << "MapView::createGUI setupVertexPainterUi done" << std::endl;
  setupObjectEditorUi();
  LogDebug << "MapView::createGUI setupObjectEditorUi done" << std::endl;
  setupCreatureBrowserUi();
  LogDebug << "MapView::createGUI setupCreatureBrowserUi done" << std::endl;
  setupCreatureEditorUi();
  LogDebug << "MapView::createGUI setupCreatureEditorUi done" << std::endl;
  setupCreatureModelPickerUi();
  LogDebug << "MapView::createGUI setupCreatureModelPickerUi done" << std::endl;
  setupGameObjectEditorUi();
  LogDebug << "MapView::createGUI setupGameObjectEditorUi done" << std::endl;
  setupGameObjectBrowserUi();
  LogDebug << "MapView::createGUI setupGameObjectBrowserUi done" << std::endl;
  setupGameObjectModelPickerUi();
  LogDebug << "MapView::createGUI setupGameObjectModelPickerUi done" << std::endl;
  setupMinimapEditorUi();
  LogDebug << "MapView::createGUI setupMinimapEditorUi done" << std::endl;
  setupStampUi();
  LogDebug << "MapView::createGUI setupStampUi done" << std::endl;
  setupLightEditorUi();
  LogDebug << "MapView::createGUI setupLightEditorUi done" << std::endl;
  setupChunkManipulatorUi();
  LogDebug << "MapView::createGUI setupChunkManipulatorUi done" << std::endl;
  setupScriptingUi();
  LogDebug << "MapView::createGUI setupScriptingUi done" << std::endl;
  // End combined dock

  setupViewportOverlay();
  LogDebug << "MapView::createGUI setupViewportOverlay done" << std::endl;
  setupCreatureActionsUi();
  LogDebug << "MapView::createGUI setupCreatureActionsUi done" << std::endl;
  setupGameObjectActionsUi();
  LogDebug << "MapView::createGUI setupGameObjectActionsUi done" << std::endl;
  setupAssetBrowser();
  LogDebug << "MapView::createGUI setupAssetBrowser done" << std::endl;
  setupDetailInfos();
  LogDebug << "MapView::createGUI setupDetailInfos done" << std::endl;
  setupToolbars();
  LogDebug << "MapView::createGUI setupToolbars done" << std::endl;
  setupKeybindingsGui();
  LogDebug << "MapView::createGUI setupKeybindingsGui done" << std::endl;

  setupMinimap();
  LogDebug << "MapView::createGUI setupMinimap done" << std::endl;
  setupFileMenu();
  LogDebug << "MapView::createGUI setupFileMenu done" << std::endl;
  setupEditMenu();
  LogDebug << "MapView::createGUI setupEditMenu done" << std::endl;
  setupViewMenu();
  LogDebug << "MapView::createGUI setupViewMenu done" << std::endl;
  setupAssistMenu();
  LogDebug << "MapView::createGUI setupAssistMenu done" << std::endl;
  setupHelpMenu();
  LogDebug << "MapView::createGUI setupHelpMenu done" << std::endl;
  setupHotkeys();
  LogDebug << "MapView::createGUI setupHotkeys done" << std::endl;

  connect(_main_window, &Noggit::Ui::Windows::NoggitWindow::exitPromptOpened, this, &MapView::on_exit_prompt);

  set_editing_mode (editing_mode::ground);

  _draw_creature_spawns.set(creature_capture_overlay_enabled()
                              ? _settings->value("view/creature_spawns", false).toBool()
                              : false);
  _settings->setValue("map_view/creature_browser", false);
  _settings->sync();
  _show_creature_browser.set(false);
  refreshCreatureSpawnOverlay(false);
  updateDatabaseStatus();
  LogDebug << "MapView::createGUI end" << std::endl;
}

void MapView::on_exit_prompt()
{
  // hide all popups
  _keybindings->hide();
  _minimap_dock->hide();
  _texture_palette_small->hide();
  _object_palette_dock->hide();
  objectEditor->helper_models_widget->hide();
  objectEditor->modelImport->hide();
  objectEditor->rotationEditor->hide();
  _detail_infos_dock->hide();
  if (_creature_actions_overlay) _creature_actions_overlay->hide();
  if (_creature_browser_dock) _creature_browser_dock->hide();
  if (_creature_editor_dock) _creature_editor_dock->hide();
  if (_creature_model_picker_dock) _creature_model_picker_dock->hide();
  if (_gameobject_browser_dock) _gameobject_browser_dock->hide();
  if (_gameobject_model_picker_dock) _gameobject_model_picker_dock->hide();
  _texture_picker_dock->hide();
  _texture_browser_dock->hide();
}

MapView::MapView( math::degrees camera_yaw0
                , math::degrees camera_pitch0
                , glm::vec3 camera_pos
                , Noggit::Ui::Windows::NoggitWindow* NoggitWindow
                , std::shared_ptr<Noggit::Project::NoggitProject> Project
                , std::unique_ptr<World> world
                , uid_fix_mode uid_fix
                , bool from_bookmark
                , bool capture_probe
                )
  : _camera (camera_pos, camera_yaw0, camera_pitch0)
  , mTimespeed(0.0f)
  , _uid_fix (uid_fix)
  , _from_bookmark (from_bookmark)
  , _settings (new QSettings (this))
  , cursor_color (1.f, 1.f, 1.f, 1.f)
  , _cursorType{CursorType::CIRCLE}
  , _main_window (NoggitWindow)
  , _world (std::move (world))
  , _status_position (new QLabel (this))
  , _status_selection (new QLabel (this))
  , _status_area (new QLabel (this))
  , _status_time (new QLabel (this))
  , _status_fps (new QLabel (this))
  , _status_culling (new QLabel (this))
  , _status_database(new QLabel(this))
  , _texBrush{new OpenGL::texture{}}
  , _transform_gizmo(Noggit::Ui::Tools::ViewportGizmo::GizmoContext::MAP_VIEW)
  , _tablet_manager(Noggit::TabletManager::instance()),
    _project(Project)
{
  LogDebug << "MapView::MapView begin" << std::endl;
  _capture_probe = capture_probe;
  setWindowTitle ("Noggit Studio Red - " STRPRODUCTVER);
  setFocusPolicy (Qt::StrongFocus);
  setMouseTracking (true);
  setMinimumHeight(200);
  setMaximumHeight(10000);
  setAttribute(Qt::WA_OpaquePaintEvent, true);
  setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);

  _world->LoadSavedSelectionGroups(); // not doing this in world constructor because noggit loads world twice

  _context = Noggit::NoggitRenderContext::MAP_VIEW;
  _transform_gizmo.setWorld(_world.get());

  _main_window->setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
  _main_window->setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
  _main_window->setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
  _main_window->setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

  _main_window->statusBar()->addWidget (_status_position);
  connect ( this
          , &QObject::destroyed
          , _main_window
          , [=] { _main_window->statusBar()->removeWidget (_status_position); }
          );
  _main_window->statusBar()->addWidget (_status_selection);
  connect ( this
          , &QObject::destroyed
          , _main_window
          , [=] { _main_window->statusBar()->removeWidget (_status_selection); }
          );
  _main_window->statusBar()->addWidget (_status_area);
  connect ( this
          , &QObject::destroyed
          , _main_window
          , [=] { _main_window->statusBar()->removeWidget (_status_area); }
          );
  _main_window->statusBar()->addWidget (_status_time);
  connect ( this
          , &QObject::destroyed
          , _main_window
          , [=] { _main_window->statusBar()->removeWidget (_status_time); }
          );
  _main_window->statusBar()->addWidget (_status_fps);
  connect ( this
          , &QObject::destroyed
          , _main_window
          , [=] { _main_window->statusBar()->removeWidget (_status_fps); }
          );
  _main_window->statusBar()->addWidget (_status_culling);
  connect ( this
      , &QObject::destroyed
      , _main_window
      , [=] { _main_window->statusBar()->removeWidget (_status_culling); }
  );
  _main_window->statusBar()->addWidget(_status_database);
  connect(this
      , &QObject::destroyed
      , _main_window
      , [=] { _main_window->statusBar()->removeWidget(_status_database); }
  );

  setContextMenuPolicy(Qt::CustomContextMenu);

  connect(this, SIGNAL(customContextMenuRequested(const QPoint&)),
      this, SLOT(ShowContextMenu(const QPoint&)));

  moving = strafing = updown = lookat = turn = 0.0f;

  freelook = false;

  mousedir = -1.0f;

  look = false;
  _display_mode = display_mode::in_3D;

  _startup_time.start();

  int _fps_limit = _settings->value("fps_limit", 60).toInt();
  int _fps_calcul = (int)((1.f / (float)_fps_limit) * 1000.f);
  std::cout << "FPS limit is set to : " << _fps_limit << " (" << _fps_calcul << ")" << std::endl;

  if (!capture_probe)
  {
    _update_every_event_loop.start (_fps_calcul);
    connect(&_update_every_event_loop, &QTimer::timeout,[=]{ _needs_redraw = true; update(); });
  }
  else
  {
    LogDebug << "MapView::MapView capture probe mode: redraw timer disabled" << std::endl;
  }
  LogDebug << "MapView::MapView before createGUI" << std::endl;
  createGUI();
  LogDebug << "MapView::MapView after createGUI" << std::endl;

  // Zone music: a hidden dropdown widget (shown from the toolbar music button) that plays the current
  // zone's background music, switching playlists as the camera crosses zone boundaries (see tick()).
  // Enable/disable lives in the dropdown's own checkbox.
  _zone_music_player = new Noggit::Ui::ZoneMusicPlayer(this);
  _zone_music_player->setVisible(false);
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::MapView end capture_probe=" << _capture_probe
             << " size=" << width() << "x" << height()
             << " needs_redraw=" << _needs_redraw
             << " gl_initialized=" << _gl_initialized
             << std::endl;
  }
}

void MapView::tabletEvent(QTabletEvent* event)
{
  _tablet_manager->setPressure(event->pressure());
  _tablet_manager->setIsActive(true);
  event->ignore();
}

auto MapView::setBrushTexture(QImage const* img) -> void
{

  int const height{img->height()};
  int const width{img->width()};

  std::vector<std::uint32_t> tex(height * width);

  for(int i{}; i < height; ++i)
    for(int j{}; j < width; ++j)
      tex[i * width + j] = img->pixel(j, i);

  makeCurrent();
  OpenGL::context::scoped_setter const _{gl, context()};
  OpenGL::texture::set_active_texture(4);
  _texBrush->bind();
  gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex.data());
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void MapView::move_camera_with_auto_height (glm::vec3 const& pos)
{
  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());

  TileIndex tile_index = TileIndex(pos);
  if (_world->mapIndex.hasTile(tile_index))
  {
    _world->mapIndex.loadTile(pos)->wait_until_loaded();
  }

  _camera.position = pos;
  _camera.position.y = 0.0f;

  _world->GetVertex (pos.x, pos.z, &_camera.position);

  // min elevation according to https://wowdev.wiki/AreaTable.dbc
  //! \ todo use the current area's MinElevation
  if (_camera.position.y < -5000.0f)
  {
    //! \todo use the height of a model/wmo of the tile (or the map) ?
    _camera.position.y = 0.0f;
  }

  _camera.position.y += 50.0f;

  _camera_moved_since_last_draw = true;
}

void MapView::focus_camera_on_target (glm::vec3 const& target)
{
  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());

  TileIndex tile_index = TileIndex(target);
  if (_world->mapIndex.hasTile(tile_index))
  {
    _world->mapIndex.loadTile(target)->wait_until_loaded();
  }

  float const dist = 28.0f; // viewing distance from the target (tune)
  float const k = 0.70710678f; // cos/sin of 45 degrees

  // Approach from the current horizontal facing so the jump isn't jarring; back + up at 45 degrees.
  glm::vec3 dir = _camera.direction();
  glm::vec3 horiz (dir.x, 0.0f, dir.z);
  if (glm::length(horiz) < 0.001f)
  {
    horiz = glm::vec3(0.0f, 0.0f, 1.0f);
  }
  horiz = glm::normalize(horiz);

  glm::vec3 const eye = target - horiz * (dist * k) + glm::vec3(0.0f, dist * k, 0.0f);
  _camera.position = eye;

  // Aim at the target. direction() = (cos(pitch)*sin(yaw), -sin(pitch), cos(pitch)*cos(yaw)).
  glm::vec3 const d = glm::normalize(target - eye);
  _camera.yaw(math::degrees(glm::degrees(std::atan2(d.x, d.z))));
  _camera.pitch(math::degrees(glm::degrees(-std::asin(glm::clamp(d.y, -1.0f, 1.0f)))));

  _camera_moved_since_last_draw = true;
}

void MapView::on_uid_fix_fail()
{
  emit uid_fix_failed();

  _uid_fix_failed = true;
  deleteLater();
}

void MapView::initializeGL()
{
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::initializeGL begin capture_probe=" << _capture_probe
             << " size=" << width() << "x" << height()
             << " pos=(" << _camera.position.x << ", " << _camera.position.y << ", " << _camera.position.z << ")"
             << std::endl;
  }

  bool uid_warning = false;

  OpenGL::context::scoped_setter const _ (::gl, context());

  gl.viewport(0.0f, 0.0f, width(), height());

  gl.clearColor (0.0f, 0.0f, 0.0f, 1.0f);

  if (_uid_fix == uid_fix_mode::max_uid)
  {
    _world->mapIndex.searchMaxUID();
  }
  else if (_uid_fix == uid_fix_mode::fix_all_fail_on_model_loading_error)
  {
    auto result = _world->mapIndex.fixUIDs (_world.get(), true);

    if (result == uid_fix_status::failed)
    {
      on_uid_fix_fail();
      return;
    }
  }
  else if (_uid_fix == uid_fix_mode::fix_all_fuckporting_edition)
  {
    auto result = _world->mapIndex.fixUIDs (_world.get(), false);

    uid_warning = result == uid_fix_status::done_with_errors;
  }

  _uid_fix = uid_fix_mode::none;

  if (!_from_bookmark)
  {
    move_camera_with_auto_height (_camera.position);
  }

  if (uid_warning)
  {
    QMessageBox::warning
      ( nullptr
      , "UID Warning"
      , "Some models were missing or couldn't be loaded. "
        "This will lead to culling (visibility) errors in game\n"
        "It is recommended to fix those models (listed in the log file) and run the uid fix all again."
      , QMessageBox::Ok
      );
  }

  _imgui_context = QtImGui::initialize(this);

  emit resized();

  _last_opengl_context = context();

  _world->renderer()->upload();
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::initializeGL after renderer upload" << std::endl;
  }
  onSettingsSave();

  _buffers.upload();

  gl.bufferData<GL_PIXEL_PACK_BUFFER>(_buffers[0], 4, nullptr, GL_DYNAMIC_READ);
  gl.bufferData<GL_PIXEL_PACK_BUFFER>(_buffers[1], 4, nullptr, GL_DYNAMIC_READ);

  connect(context(), &QOpenGLContext::aboutToBeDestroyed, [this](){ emit aboutToLooseContext(); });

  _gl_initialized = true;
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::initializeGL end" << std::endl;
  }
}


void MapView::saveMinimap(MinimapRenderSettings* settings)
{

  OpenGL::context::scoped_setter const _ (::gl, context());

  bool mmap_render_success = false;

  static QProgressBar* progress;
  static QPushButton* cancel_btn;

  switch (settings->export_mode)
  {
    case MinimapGenMode::CURRENT_ADT:
    {
      TileIndex tile = TileIndex(_camera.position);

      if (_world->mapIndex.hasTile(tile))
      {
        mmap_render_success = _world->renderer()->saveMinimap(tile, settings, _mmap_combined_image);
      }

      if (mmap_render_success)
      {
        _world->mapIndex.saveMinimapMD5translate();
      }

      saving_minimap = false;

      break;
    }
    case MinimapGenMode::MAP:
    {

      // init progress
      if (!_mmap_async_index)
      {
        progress = new QProgressBar(nullptr);
        progress->setMinimum(0);
        progress->setMaximum(_world->mapIndex.getNumExistingTiles());
        _main_window->statusBar()->addPermanentWidget(progress);

        cancel_btn = new QPushButton(nullptr);
        cancel_btn->setText("Cancel");

        connect(cancel_btn, &QPushButton::clicked, 
          [=, this] 
          { 
            _mmap_async_index = 0; 
            _mmap_render_index = 0; 
            saving_minimap = false;
            progress->deleteLater(); 
            cancel_btn->deleteLater();
            _mmap_combined_image.reset();
          });

        _main_window->statusBar()->addPermanentWidget(cancel_btn);

        connect(this, &MapView::updateProgress,
                [=](int value)
                {

                  progress->setValue(value);
                });
      
        // setup combined image if necessary
        if (settings->combined_minimap)
        {
          _mmap_combined_image.emplace(8192, 8192, QImage::Format_RGBA8888);
          _mmap_combined_image->fill(Qt::black);
        }
      
      }

      if (!saving_minimap)
        return;

      if (_mmap_async_index < 4096 && static_cast<int>(_mmap_render_index) < progress->maximum())
      {
        TileIndex tile = TileIndex(_mmap_async_index / 64, _mmap_async_index % 64);

        if (_world->mapIndex.hasTile(tile))
        {
          OpenGL::context::scoped_setter const _(::gl, context());
          makeCurrent();
          mmap_render_success = _world->renderer()->saveMinimap(tile, settings, _mmap_combined_image);

          _mmap_render_index++;
          emit updateProgress(_mmap_render_index);

          if (!mmap_render_success)
          {
            LogError << "Minimap rendered incorrectly for tile: " << tile.x << "_" << tile.z << std::endl;
          }
        }

        _mmap_async_index++;
      }
      else
      {
        _mmap_async_index = 0;
        _mmap_render_index = 0;
        saving_minimap = false;
        progress->deleteLater();
        cancel_btn->deleteLater();
        _world->mapIndex.saveMinimapMD5translate();

        // save combined minimap
        if (settings->combined_minimap)
        {
          QString image_path = QString(std::string(_world->basename + "_combined_minimap.png").c_str());
          QSettings app_settings;
          QString str = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());;
          if (!(str.endsWith('\\') || str.endsWith('/')))
          {
            str += "/";
          }

          QDir dir(str + "/textures/minimap/");
          if (!dir.exists())
            dir.mkpath(".");

          _mmap_combined_image->save(dir.filePath(image_path));
          _mmap_combined_image.reset();
        }
      
      }

      //_main_window->statusBar()->showMessage("Minimap rendering done.", 2000);
      break;
    } 
    case MinimapGenMode::SELECTED_ADTS:
    {
      auto selected_tiles = minimapTool->getSelectedTiles();

      // init progress
      if (!_mmap_async_index)
      {
        progress = new QProgressBar(nullptr);
        progress->setMinimum(0);

        unsigned n_selected_tiles = 0;

        for (int i = 0; i < 4096; ++i)
        {
          if (selected_tiles->at(i))
            n_selected_tiles++;
        }

        progress->setMaximum(n_selected_tiles);
        _main_window->statusBar()->addPermanentWidget(progress);

        cancel_btn = new QPushButton(nullptr);
        cancel_btn->setText("Cancel");

        connect(cancel_btn, &QPushButton::clicked,
          [=, this]
          {
            _mmap_async_index = 0;
            _mmap_render_index = 0;
            saving_minimap = false;
            progress->deleteLater();
            cancel_btn->deleteLater();
            _mmap_combined_image.reset();
          });

        _main_window->statusBar()->addPermanentWidget(cancel_btn);

        connect(this, &MapView::updateProgress,
                [=](int value)
                {
                  // This weirdness is required due to a bug on Linux when QT repaint crashes due to too many events
                  // being passed through. TODO: this potentially only masks the issue, which may reappear on faster
                  // hardware.
                  if (progress->value() != value)
                    progress->setValue(value);
                });

        // setup combined image if necessary
        if (settings->combined_minimap)
        {
          _mmap_combined_image.emplace(8192, 8192, QImage::Format_RGBA8888);
          _mmap_combined_image->fill(Qt::black);
        }
      
      }

      if (!saving_minimap)
        return;


      if (_mmap_async_index < 4096 && static_cast<int>(_mmap_render_index) < progress->maximum())
      {
        if (selected_tiles->at(_mmap_async_index))
        {
          TileIndex tile = TileIndex(_mmap_async_index / 64, _mmap_async_index % 64);

          if (_world->mapIndex.hasTile(tile))
          {
            mmap_render_success = _world->renderer()->saveMinimap(tile, settings, _mmap_combined_image);
            _mmap_render_index++;

            emit updateProgress(_mmap_render_index);


            if (!mmap_render_success)
            {
              LogError << "Minimap rendered incorrectly for tile: " << tile.x << "_" << tile.z << std::endl;
            }
          }
        }
        _mmap_async_index++;

      }
      else
      {
        _mmap_async_index = 0;
        _mmap_render_index = 0;
        saving_minimap = false;
        progress->deleteLater();
        cancel_btn->deleteLater();
        _world->mapIndex.saveMinimapMD5translate();

        // save combined minimap
        if (settings->combined_minimap)
        {
          QString image_path = QString(std::string(_world->basename + "_combined_minimap.png").c_str());
          QString str = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
          if (!(str.endsWith('\\') || str.endsWith('/')))
          {
            str += "/";
          }

          QDir dir(str + "/textures/minimap/");
          if (!dir.exists())
            dir.mkpath(".");

          _mmap_combined_image->save(dir.filePath(image_path));
          _mmap_combined_image.reset();
        }
     
      }

      break;
     
    }
  }

  //minimapTool->progressUpdate(0);
}

void MapView::paintGL()
{
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::paintGL entered capture_probe=" << _capture_probe
             << " needs_redraw=" << _needs_redraw
             << " gl_initialized=" << _gl_initialized
             << std::endl;
  }
  static bool lock = false;

  if (lock)
    return;

  if (!_needs_redraw)
    return;
  else
    _needs_redraw = false;

  if (capture_debug_enabled())
  {
    LogDebug << "MapView::paintGL after redraw gate" << std::endl;
  }

  if (!_gl_initialized)
  {
    if (capture_debug_enabled())
    {
      LogDebug << "MapView::paintGL before initializeGL" << std::endl;
    }
    initializeGL();
    if (capture_debug_enabled())
    {
      LogDebug << "MapView::paintGL after initializeGL gl_initialized=" << _gl_initialized << std::endl;
    }
  }

  if (_last_opengl_context != context())
  {
    _gl_initialized = false;
    if (capture_debug_enabled())
    {
      LogDebug << "MapView::paintGL context changed, skipping frame" << std::endl;
    }
    return;
  }

  const qreal now(_startup_time.elapsed() / 1000.0);

  _last_frame_durations.emplace_back (now - _last_update);

  // minimap rendering
  if (saving_minimap)
  {
    OpenGL::context::scoped_setter const _(::gl, context());
    makeCurrent();
    _camera_moved_since_last_draw = true;
    lock = true;
    saveMinimap(minimapTool->getMinimapRenderSettings());
    lock = false;
    return;
  }

  if (capture_debug_enabled())
  {
    LogDebug << "MapView::paintGL before context setter" << std::endl;
  }
  OpenGL::context::scoped_setter const _(::gl, context());
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::paintGL before makeCurrent" << std::endl;
  }
  makeCurrent();
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::paintGL before clear" << std::endl;
  }

  gl.clear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  {
    // [perf] frame-to-frame total + throttled per-phase report (NOGGIT_FRAME_PROFILE=1). No-op when off.
    static std::chrono::steady_clock::time_point s_prof_last;
    static bool s_prof_have = false;
    auto& _prof = noggit::perf::FrameProfiler::get();
    auto const _prof_now = std::chrono::steady_clock::now();
    if (s_prof_have)
      _prof.add(noggit::perf::Phase::Frame,
                std::chrono::duration<double, std::milli>(_prof_now - s_prof_last).count());
    s_prof_last = _prof_now;
    s_prof_have = true;
    _prof.end_frame();

    // [mem-diag 2026-08-04] Once/sec, log process working set + loaded-asset counts so a memory climb can
    // be attributed: models/textures/wmos growing = assets not freeing; working set climbing while those
    // stay flat = a GPU/loader-side leak. NOGGIT_FRAME_PROFILE gate (already on in the launcher).
    if (_prof.on)
    {
      static std::chrono::steady_clock::time_point s_mem_last;
      static bool s_mem_have = false;
      if (!s_mem_have || std::chrono::duration<double, std::milli>(_prof_now - s_mem_last).count() > 1000.0)
      {
        s_mem_last = _prof_now;
        s_mem_have = true;
        std::size_t ws_mb = 0;
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS pmc{};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) // kernel32-exported, no psapi.lib
          ws_mb = pmc.WorkingSetSize / (1024u * 1024u);
#endif
        LogError << "[MEM] workingSet=" << ws_mb << "MB"
                 << " models=" << ModelManager::loaded_count()
                 << " textures=" << TextureManager::loaded_count()
                 << " wmos=" << WMOManager::loaded_count()
                 << " tiles=" << (_world ? _world->mapIndex.getNLoadedTiles() : 0u)
                 << std::endl;
      }
    }
  }

  if (!saving_minimap)
  {
    lock = true;
    if (capture_debug_enabled())
    {
      LogDebug << "MapView::paintGL before draw_map" << std::endl;
    }
    draw_map();
    {
      // [perf] GPU-boundedness probe (NOGGIT_FRAME_PROFILE only): time a glFinish right after the render.
      // GpuWait ~= how long the CPU must WAIT for the GPU to finish the frame's draws beyond what already
      // overlapped the CPU submit. Large GpuWait => GPU-BOUND (GPU render time ~= WorldDraw + GpuWait);
      // ~0 => CPU-bound. Intrusive (it serializes CPU<->GPU), so it only runs while profiling.
      auto& _prof_gpu = noggit::perf::FrameProfiler::get();
      if (_prof_gpu.on)
      {
        auto const _gt0 = std::chrono::steady_clock::now();
        gl.finish();
        _prof_gpu.add(noggit::perf::Phase::GpuWait,
                      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _gt0).count());
      }
    }
    if (capture_debug_enabled())
    {
      LogDebug << "MapView::paintGL after draw_map" << std::endl;
    }
    lock = false;
    if (capture_debug_enabled())
    {
      LogDebug << "MapView::paintGL before tick" << std::endl;
    }
    {
      // [perf] time tick() into the (otherwise-unused) Overlays bucket so the frame-to-frame gap can be
      // split: Frame - WorldDraw - Overlays = pure Qt event-loop/compositor cost outside our draw code.
      noggit::perf::Scoped _prof_tick(noggit::perf::Phase::Overlays);
      tick (now - _last_update);
    }
    if (capture_debug_enabled())
    {
      LogDebug << "MapView::paintGL after tick" << std::endl;
    }
  }

  _last_update = now;


  if (_gizmo_on.get() && _world->has_selection())
  {
    ImGui::SetCurrentContext(_imgui_context);
    QtImGui::newFrame();

    static bool is_open = false;
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::SetNextWindowPos(ImVec2(-100.f, -100.f));
    ImGui::Begin("Gizmo", &is_open, ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar
                                                | ImGuiWindowFlags_::ImGuiWindowFlags_NoBackground);

    auto mv = model_view();
    auto proj = projection();

    _transform_gizmo.setCurrentGizmoOperation(_gizmo_operation);
    _transform_gizmo.setCurrentGizmoMode(_gizmo_mode);
    _transform_gizmo.setUseMultiselectionPivot(_use_median_pivot_point.get());

    auto pivot = _world->multi_select_pivot().has_value() ?
        _world->multi_select_pivot().value() : glm::vec3(0.f, 0.f, 0.f);

    _transform_gizmo.setMultiselectionPivot(pivot);

    _transform_gizmo.handleTransformGizmo(this, _world->current_selection(), mv, proj);

    _world->update_selection_pivot();

    ImGui::End();

    /* Example
    std::string sText;

    if(ImGui::IsMouseClicked( 1 ) )
    {
      ImGui::OpenPopup( "PieMenu" );
    }

    if( BeginPiePopup( "PieMenu", 1 ) )
    {
      if( PieMenuItem( "Test1" ) ) sText = "Test1";
      if( PieMenuItem( "Test2" ) )
      {
        sText = "Test2";
      }
      if( PieMenuItem( "Test3", false ) ) sText = "Test3";
      if( BeginPieMenu( "Sub" ) )
      {
        if( BeginPieMenu( "Sub sub\nmenu" ) )
        {
          if( PieMenuItem( "SubSub" ) ) sText = "SubSub";
          if( PieMenuItem( "SubSub2" ) ) sText = "SubSub2";
          EndPieMenu();
        }
        if( PieMenuItem( "TestSub" ) ) sText = "TestSub";
        if( PieMenuItem( "TestSub2" ) ) sText = "TestSub2";
        EndPieMenu();
      }
      if( BeginPieMenu( "Sub2" ) )
      {
        if( PieMenuItem( "TestSub" ) ) sText = "TestSub";
        if( BeginPieMenu( "Sub sub\nmenu" ) )
        {
          if( PieMenuItem( "SubSub" ) ) sText = "SubSub";
          if( PieMenuItem( "SubSub2" ) ) sText = "SubSub2";
          EndPieMenu();
        }
        if( PieMenuItem( "TestSub2" ) ) sText = "TestSub2";
        EndPieMenu();
      }

      EndPiePopup();
    }

   */

    //ImGui::ShowDemoWindow();
    //ImGui::ShowStyleEditor();

    ImGui::Render();

  }

  if (!saving_minimap && _world->uid_duplicates_found() && !_uid_duplicate_warning_shown)
  {
    _uid_duplicate_warning_shown = true;
    LogError << "Duplicate object UIDs were found while loading this map. "
             << "Noggit reassigned duplicates for this editor session; run a max UID check before saving shared edits."
             << std::endl;
  }

  FrameMark
}

void MapView::resizeGL (int width, int height)
{
  OpenGL::context::scoped_setter const _ (::gl, context());
  gl.viewport(0.0f, 0.0f, width, height);
  emit resized();
  _camera_moved_since_last_draw = true;
  _needs_redraw = true;
}


MapView::~MapView()
{
  makeCurrent();

  _destroying = true;

  // Make teardown single-threaded BEFORE destroying the world. The async loader threads finish
  // model/WMO loads in the background, and completing a load can still feed SceneObject instances into
  // MapTile::object_instances (under the tile mutex). World destruction (~World -> ~MapTile) iterates
  // object_instances WITHOUT that lock and calls instance->derefTile(); if a loader thread mutates the
  // tile mid-iteration the instance pointer is garbage -> access violation in SceneObject::derefTile.
  // This is exactly the crash when leaving a still-streaming zone like Ironforge. Waiting until the
  // loader is idle quiesces the background so the destruction below runs with no concurrent mutation.
  // (finishLoading is CPU-side file parsing; GL upload is deferred to first draw, so this can't deadlock
  // on the now-stopped render loop.)
  AsyncLoader::instance().wait_until_idle();

  OpenGL::context::scoped_setter const _ (::gl, context());
  // Force the GPU fully idle BEFORE deleting any GL resource below (and in ~World via _world.reset()).
  // Returning to menu / switching maps crashed inside the NVIDIA GL driver (nvoglv64.dll, __fastfail):
  // the last rendered frame's draws were still in flight referencing textures/buffers that teardown then
  // deleted, so the driver touched freed objects. glFinish blocks until all queued GL commands complete,
  // so nothing the deletes free is still in use. (The per-call glGetError sync used to hide this; this is
  // the explicit, sync-setting-independent fix.)
  gl.finish();
  delete _texBrush;
  delete _viewport_overlay_ui;

  // when the uid fix fail the UI isn't created
  if (!_uid_fix_failed)
  {
    delete TexturePicker; // explicitly delete this here to avoid opengl context related crash
    delete objectEditor;
    delete texturingTool;
  }
  
  if (_force_uid_check)
  {
    uid_storage::remove_uid_for_map(_world->getMapID());
  }

  _world.reset();

  AsyncLoader::instance().reset_object_fail();

  Noggit::Ui::selected_texture::texture.reset();

  ModelManager::report();
  TextureManager::report();
  WMOManager::report();

  NOGGIT_ACTION_MGR->disconnect();

  _buffers.unload();

}

void MapView::tick (float dt)
{
	_mod_shift_down = QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
	_mod_ctrl_down = QApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
	_mod_alt_down = QApplication::keyboardModifiers().testFlag(Qt::AltModifier);
	_mod_num_down = QApplication::keyboardModifiers().testFlag(Qt::KeypadModifier);

	unsigned action_modality = 0;
	if (_mod_shift_down)
    action_modality |= Noggit::ActionModalityControllers::eSHIFT;
	if (_mod_ctrl_down)
    action_modality |= Noggit::ActionModalityControllers::eCTRL;
  if (_mod_alt_down)
    action_modality |= Noggit::ActionModalityControllers::eALT;
  if (_mod_num_down)
    action_modality |= Noggit::ActionModalityControllers::eNUM;
  if (_mod_space_down)
    action_modality |= Noggit::ActionModalityControllers::eSPACE;
  if (leftMouse)
    action_modality |= Noggit::ActionModalityControllers::eLMB;
  if (rightMouse)
    action_modality |= Noggit::ActionModalityControllers::eRMB;
  if (MoveObj)
    action_modality |= Noggit::ActionModalityControllers::eMMB;

  NOGGIT_ACTION_MGR->endActionOnModalityMismatch(action_modality);

  // start unloading tiles
  _world->mapIndex.enterTile (TileIndex (_camera.position));
  _world->mapIndex.unloadTiles (TileIndex (_camera.position));

  dt = std::min(dt, 1.0f);

  auto cur_action = NOGGIT_CUR_ACTION;

  if ((cur_action && !cur_action->getBlockCursor()) || !cur_action)
  {
    if (_locked_cursor_mode.get())
    {
      switch (terrainMode)
      {
        case editing_mode::areaid:
        case editing_mode::flags:
        case editing_mode::holes:
        case editing_mode::object:
          update_cursor_pos();
          break;
        default:
          break;
      }
    }
    else
    {
      update_cursor_pos();
    }
  }

  math::degrees yaw (-_camera.yaw()._);

  glm::vec3 dir(1.0f, 0.0f, 0.0f);
  glm::vec3 dirUp(1.0f, 0.0f, 0.0f);
  glm::vec3 dirRight(0.0f, 0.0f, 1.0f);
  math::rotate(0.0f, 0.0f, &dir.x, &dir.y, _camera.pitch());
  math::rotate(0.0f, 0.0f, &dir.x, &dir.z, yaw);

  if (_mod_ctrl_down)
  {
    dirUp.x = 0.0f;
    dirUp.y = 1.0f;
    math::rotate(0.0f, 0.0f, &dirUp.x, &dirUp.y, _camera.pitch());
    math::rotate(0.0f, 0.0f, &dirRight.x, &dirRight.y, _camera.pitch());
    math::rotate(0.0f, 0.0f, &dirUp.x, &dirUp.z, yaw);
    math::rotate(0.0f, 0.0f, &dirRight.x, &dirRight.z,yaw);
  }
  else if(!_mod_shift_down)
  {
    math::rotate(0.0f, 0.0f, &dirUp.x, &dirUp.z, yaw);
    math::rotate(0.0f, 0.0f, &dirRight.x, &dirRight.z, yaw);
  }

  auto currentSelection = _world->current_selection();
  if (_world->has_selection())
  {
    // update rotation editor if the selection has changed
    if (lastSelected != currentSelection)
    {
      _rotation_editor_need_update = true;
    }

    if (terrainMode == editing_mode::object)
    {
      // reset numpad_moveratio when no numpad key is pressed
      if (!(keyx != 0 || keyy != 0 || keyz != 0 || keyr != 0 || keys != 0))
      {
        numpad_moveratio = 0.5f;
      }
      else // Set move scale and rotate for numpad keys
      {
        if (_mod_ctrl_down && _mod_shift_down)
        {
          numpad_moveratio += 0.5f;
        }
        else if (_mod_shift_down)
        {
          numpad_moveratio += 0.05f;
        }
        else if (_mod_ctrl_down)
        {
          numpad_moveratio += 0.005f;
        }
      }

      if (keys != 0.f)
      {
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
        _world->scale_selected_models(keys*numpad_moveratio / 50.f, World::m2_scaling_type::add);
        // NOGGIT_ACTION_MGR->endAction();
        _rotation_editor_need_update = true;
      }
      if (keyr != 0.f)
      {
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
        _world->rotate_selected_models( math::degrees(0.f)
                                      , math::degrees(keyr * numpad_moveratio * 5.f)
                                      , math::degrees(0.f)
                                      , _use_median_pivot_point.get()
                                      );
        _rotation_editor_need_update = true;
      }

      if (MoveObj)
      {
        if (_mod_alt_down)
        {
          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                         Noggit::ActionModalityControllers::eALT
                                                         | Noggit::ActionModalityControllers::eMMB );
          _world->scale_selected_models(std::pow(2.f, mv*4.f), World::m2_scaling_type::mult);
        }
        else if (_mod_shift_down)
        {
          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                         Noggit::ActionModalityControllers::eSHIFT
                                                         | Noggit::ActionModalityControllers::eMMB );
          _world->move_selected_models(0.f, mv*80.f, 0.f);
        }
        else if (_mod_ctrl_down)
        {
            // do nothing
        }
        else
        {
          bool snapped = false;
          bool snapped_to_object = false;
          if (_world->has_multiple_model_selected())
          {
            NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                                 Noggit::ActionModalityControllers::eMMB );
            _world->set_selected_models_pos(_cursor_pos, false);

            if (_snap_multi_selection_to_ground.get())
            {
              snap_selected_models_to_the_ground();
              snapped = true;
            }
          }
          else
          {
            if (!_move_model_to_cursor_position.get())
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                                   Noggit::ActionModalityControllers::eMMB );

              if ((mh <= 0.01f && mh >= -0.01f) && (mv <= 0.01f && mv >= -0.01f))
              {
                  glm::vec3 _vec = (mh * dirUp + mv * dirRight);
                  _world->move_selected_models(_vec * 500.f);
              }
            }
            else
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                             Noggit::ActionModalityControllers::eMMB );

              if (_move_model_to_cursor_position.get() || _move_model_snap_to_objects.get())
              {
                selection_result results(intersect_result(false));

                if (!results.empty())
                {
                    for (auto result = results.begin(); result != results.end(); result++)
                    {
                        auto const& hit(result->second);
                        bool is_selected_model = false;

                        // if a terrain is found first use that (terrain cursor pos position updated on move already)
                        if (hit.index() == eEntry_MapChunk && _move_model_to_cursor_position.get())
                        {
                            break;
                        }

                        if (hit.index() == eEntry_Object && _move_model_snap_to_objects.get())
                        {
                            auto obj_hit = std::get<selected_object_type>(hit);
                            auto obj_hit_type = obj_hit->which();

                            // don't snap to animated models
                            if (obj_hit_type == eMODEL)
                            {
                                auto m2_model_hit = static_cast<ModelInstance*>(obj_hit);
                                if (m2_model_hit->model->animated_mesh())
                                    continue;
                            }

                            // find and ignore current object/selected models or it will keep snaping to itself
                            for (auto& entry : _world->current_selection())
                            {
                                auto type = entry.index();
                                if (type == eEntry_Object)
                                {
                                    auto& selection_obj = std::get<selected_object_type>(entry);
                                    if (selection_obj->uid == obj_hit->uid)
                                    {
                                        is_selected_model = true;
                                        break;
                                    }
                                }
                            }
                            if (is_selected_model)
                                continue;
                            auto hit_pos = intersect_ray().position(result->first);
                            _cursor_pos = hit_pos;
                            snapped_to_object = true;
                            // TODO : rotate objects to objects normal
                            // if (_rotate_doodads_along_doodads.get())
                            //    _world->rotate_selected_models_to_object_normal(_rotate_along_ground_smooth.get(), obj_hit, hit_pos, glm::transpose(model_view()), _rotate_doodads_along_wmos.get());
                            break;
                        }
                    }
                }
                _world->set_selected_models_pos(_cursor_pos, false);
                snapped = true;
              }
            }
          }

          if (snapped && _rotate_along_ground.get())
          {
            if (!snapped_to_object)
              _world->rotate_selected_models_to_ground_normal(_rotate_along_ground_smooth.get());

            if (_rotate_along_ground_random.get())
            {
              float minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;

              if (_settings->value("model/random_rotation", false).toBool())
              {
                minY = _object_paste_params.minRotation;
                maxY = _object_paste_params.maxRotation;
              }

              if (_settings->value("model/random_tilt", false).toBool())
              {
                minX = _object_paste_params.minTilt;
                maxX = _object_paste_params.maxTilt;
                minZ = minX;
                maxZ = maxX;
              }

              _world->rotate_selected_models_randomly(
                  minX,
                  maxX,
                  minY,
                  maxY,
                  minZ,
                  maxZ);

              if (_settings->value("model/random_size", false).toBool())
              {
                float min = _object_paste_params.minScale;
                float max = _object_paste_params.maxScale;

                _world->scale_selected_models(misc::randfloat(min, max), World::m2_scaling_type::set);
              }
            }
          }


        }

        _rotation_editor_need_update = true;
      }

      /* TODO: Numpad for action system
      if (keyx != 0.f || keyy != 0.f || keyz != 0.f)
      {
        _world->move_selected_models(keyx * numpad_moveratio, keyy * numpad_moveratio, keyz * numpad_moveratio);
        _rotation_editor_need_update = true;
      }
       */

      if (look)
      {
        if (_mod_ctrl_down) // X
        {
          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                         Noggit::ActionModalityControllers::eCTRL
                                                         | Noggit::ActionModalityControllers::eRMB );
          _world->rotate_selected_models( math::degrees(rh + rv)
                                        , math::degrees(0.f)
                                        , math::degrees(0.f)
                                        , _use_median_pivot_point.get()
                                        );
        }
        if (_mod_shift_down) // Y
        {
          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                         Noggit::ActionModalityControllers::eSHIFT
                                                         | Noggit::ActionModalityControllers::eRMB );
          _world->rotate_selected_models( math::degrees(0.f)
                                        , math::degrees(rh + rv)
                                        , math::degrees(0.f)
                                        , _use_median_pivot_point.get()
                                        );
        }
        if (_mod_alt_down) // Z
        {
          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                         Noggit::ActionModalityControllers::eALT
                                                         | Noggit::ActionModalityControllers::eRMB );
          _world->rotate_selected_models( math::degrees(0.f)
                                        , math::degrees(0.f)
                                        , math::degrees(rh + rv)
                                        , _use_median_pivot_point.get()
                                        );
        }

        _rotation_editor_need_update = true;
      }
    }

    for (auto& selection : currentSelection)
    {
      if (selection.index() == eEntry_MapChunk && terrainMode == editing_mode::scripting)
      {
        scriptingTool->sendBrushEvent(_cursor_pos, 7.5f * dt);
      }

      if (leftMouse && selection.index() == eEntry_MapChunk)
      {
        bool underMap = _world->isUnderMap(_cursor_pos);
        auto cur_action = NOGGIT_CUR_ACTION;

        switch (terrainMode)
        {
        case editing_mode::ground:
          if (_display_mode == display_mode::in_3D && !underMap)
          {
            auto mask_selector = terrainTool->getImageMaskSelector();

            if (_mod_shift_down && (!mask_selector->isEnabled() || mask_selector->getBrushMode()))
            {
              auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eLMB);

              action->setPostCallback(&MapView::randomizeTerrainRotation);

              terrainTool->changeTerrain(_world.get(), _cursor_pos, 7.5f * dt);
            }
            else if (_mod_ctrl_down && (!mask_selector->isEnabled() || mask_selector->getBrushMode()))
            {
              auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                             Noggit::ActionModalityControllers::eCTRL
                                                             | Noggit::ActionModalityControllers::eLMB);

              action->setPostCallback(&MapView::randomizeTerrainRotation);

              terrainTool->changeTerrain(_world.get(), _cursor_pos, -7.5f * dt);
            }
          }
          break;
        case editing_mode::flatten_blur:
          if (_display_mode == display_mode::in_3D && !underMap)
          {
            if (_mod_shift_down)
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eLMB);
              flattenTool->flatten(_world.get(), _cursor_pos, dt);
            }
            else if (_mod_ctrl_down)
            {

              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                             Noggit::ActionModalityControllers::eCTRL
                                                             | Noggit::ActionModalityControllers::eLMB);
              flattenTool->blur(_world.get(), _cursor_pos, dt);
            }
          }
          break;
        case editing_mode::paint:
          if (_mod_shift_down && _mod_ctrl_down && _mod_alt_down)
          {
            // clear chunk texture
            if (!underMap)
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE,
                                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eCTRL
                                                             | Noggit::ActionModalityControllers::eALT
                                                             | Noggit::ActionModalityControllers::eLMB);

              _world->eraseTextures(_cursor_pos);
            }
          }
          else if (_mod_ctrl_down && !ui_hidden)
          {
            // Pick texture
            _texture_picker_dock->setVisible(true);
            TexturePicker->setMainTexture(texturingTool->_current_texture);
            TexturePicker->getTextures(selection);
          }
          else  if (_mod_shift_down && !!Noggit::Ui::selected_texture::get())
          {
            if ((_display_mode == display_mode::in_3D && !underMap) || _display_mode == display_mode::in_2D)
            {
              auto image_mask_selector = texturingTool->getImageMaskSelector();

              if (NOGGIT_CUR_ACTION
              && texturingTool->getTexturingMode() == Noggit::Ui::texturing_mode::paint
              && image_mask_selector->isEnabled()
              && !image_mask_selector->getBrushMode())
                break;

              auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE,
                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eLMB);

              action->setPostCallback(&MapView::randomizeTexturingRotation);

              if (texturingTool->getTexturingMode() == Noggit::Ui::texturing_mode::paint
                  && image_mask_selector->isEnabled()
                  && !image_mask_selector->getBrushMode())
                action->setBlockCursor(true);

              texturingTool->paint(_world.get(), _cursor_pos, dt, *Noggit::Ui::selected_texture::get());
            }
          }
          break;

        case editing_mode::holes:
          // no undermap check here, else it's impossible to remove holes
          if (_mod_shift_down)
          {
            NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_HOLES,
                                                           Noggit::ActionModalityControllers::eSHIFT
                                                           | Noggit::ActionModalityControllers::eLMB);
            _world->setHole(_cursor_pos, holeTool->brushRadius(),_mod_alt_down, false);
          }
          else if (_mod_ctrl_down && !underMap)
          {
            NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_HOLES,
                                                           Noggit::ActionModalityControllers::eCTRL
                                                           | Noggit::ActionModalityControllers::eLMB);
            _world->setHole(_cursor_pos, holeTool->brushRadius(), _mod_alt_down, true);
          }
          break;
        case editing_mode::areaid:
          if (!underMap)
          {
            if (_mod_shift_down)
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_AREAID,
                                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eLMB);
              // draw the selected AreaId on current selected chunk
              _world->setAreaID(_cursor_pos, _selected_area_id, false, ZoneIDBrowser->brushRadius());
            }
            else if (_mod_ctrl_down)
            {
              // pick areaID from chunk
              MapChunk* chnk(std::get<selected_chunk_type>(selection).chunk);
              int newID = chnk->getAreaID();
              _selected_area_id = newID;
              ZoneIDBrowser->setZoneID(newID);
            }
          }
          break;
        case editing_mode::flags:
          if (!underMap)
          {
            // todo: replace this
            if (_mod_shift_down)
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_FLAGS,
                                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eLMB);
              _world->mapIndex.setFlag(true, _cursor_pos, 0x2);
            }
            else if (_mod_ctrl_down)
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_FLAGS,
                                                             Noggit::ActionModalityControllers::eCTRL
                                                             | Noggit::ActionModalityControllers::eLMB);
              _world->mapIndex.setFlag(false, _cursor_pos, 0x2);
            }
          }
          break;
        case editing_mode::water:
          if (_display_mode == display_mode::in_3D && !underMap)
          {
            if (_mod_shift_down)
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_WATER,
                                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eLMB);
              guiWater->paintLiquid(_world.get(), _cursor_pos, true);
            }
            else if (_mod_ctrl_down)
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_WATER,
                                                             Noggit::ActionModalityControllers::eCTRL
                                                             | Noggit::ActionModalityControllers::eLMB);
              guiWater->paintLiquid(_world.get(), _cursor_pos, false);
            }
          }
          break;
        case editing_mode::stamp:
          if (_display_mode == display_mode::in_3D && (_mod_shift_down || _mod_ctrl_down || _mod_alt_down) && stampTool->getBrushMode())
          {
            auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eNO_FLAG,
                                                           Noggit::ActionModalityControllers::eSHIFT
                                                           | Noggit::ActionModalityControllers::eLMB);

            if (!stampTool->getBrushMode())
              action->setBlockCursor(true);

            stampTool->execute(_cursor_pos, _world.get(), dt, _mod_shift_down, _mod_alt_down, _mod_ctrl_down, underMap);
          }
          break;
        case editing_mode::mccv:
          if (!underMap)
          {
            if (_mod_shift_down)
            {

              auto image_mask_selector = shaderTool->getImageMaskSelector();

              if (NOGGIT_CUR_ACTION
                  && image_mask_selector->isEnabled()
                  && !image_mask_selector->getBrushMode())
                break;

              auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_VERTEX_COLOR,
                                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eLMB);

              action->setPostCallback(&MapView::randomizeShaderRotation);

              if (image_mask_selector->isEnabled() && !image_mask_selector->getBrushMode())
                action->setBlockCursor(true);

              shaderTool->changeShader(_world.get(), _cursor_pos, dt, true);
            }
            if (_mod_ctrl_down)
            {

              auto image_mask_selector = shaderTool->getImageMaskSelector();

              if (NOGGIT_CUR_ACTION
                  && image_mask_selector->isEnabled()
                  && !image_mask_selector->getBrushMode())
                break;


              auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_VERTEX_COLOR,
                                                             Noggit::ActionModalityControllers::eCTRL
                                                             | Noggit::ActionModalityControllers::eLMB);

              action->setPostCallback(&MapView::randomizeShaderRotation);

              if (image_mask_selector->isEnabled() && !image_mask_selector->getBrushMode())
                action->setBlockCursor(true);

              shaderTool->changeShader(_world.get(), _cursor_pos, dt, false);
            }
          }
          break;
          default:
            break;
        }
      }
    }
  }

  mh = 0;
  mv = 0;
  rh = 0;
  rv = 0;

  if (_display_mode != display_mode::in_2D)
  {
    if (turn)
    {
      _camera.add_to_yaw(math::degrees(turn));
      _camera_moved_since_last_draw = true;
    }
    if (lookat)
    {
      _camera.add_to_pitch(math::degrees(lookat));
      _camera_moved_since_last_draw = true;
    }

    if (_game_mode_camera.get())
    {
        if (moving)
        {
            _camera.move_forward(moving, dt);
            _camera_moved_since_last_draw = true;
            // TODO use normalized speed (doesn't slow down when looking up)
            // _camera.move_forward_normalized(moving, dt);
        }
        if (strafing)
        {
            _camera.move_horizontal(strafing, dt);
            _camera_moved_since_last_draw = true;
        }
        // get ground z position
        // hack to update camera when entering mode in void ViewToolbar::add_tool_icon()
        if (_camera_moved_since_last_draw)
        {
            auto ground_pos = _world.get()->get_ground_height(_camera.position);
            _camera.position.y = ground_pos.y + 2;
        }
    }
    else
    {

      if (moving)
      {
        _camera.move_forward(moving, dt);
        _camera_moved_since_last_draw = true;
      }
      if (strafing)
      {
        _camera.move_horizontal(strafing, dt);
        _camera_moved_since_last_draw = true;
      }
      if (updown)
      {
        _camera.move_vertical(updown, dt);
        _camera_moved_since_last_draw = true;
      }
      // camera collision to ground
      /*
      auto ground_height = _world.get()->get_ground_height(_camera.position).y;
      if (_camera.position.y < ground_height)
      {
          _camera.position.y = ground_height + 3;
      }
      */

    }
  }
  else
  {
    //! \todo this is total bullshit. there should be a seperate view and camera class for tilemode
    if (moving)
    {
      _camera.position.z -= dt * _camera.move_speed * moving;
      _camera_moved_since_last_draw = true;
    }
    if (strafing)
    {
      _camera.position.x += dt * _camera.move_speed * strafing;
      _camera_moved_since_last_draw = true;
    }
    if (updown)
    {
      _2d_zoom *= pow(2.0f, dt * updown * 4.0f);
      _2d_zoom = std::max(0.01f, _2d_zoom);
      _camera_moved_since_last_draw = true;
    }
  }

  // _minimap->update(); // causes massive performance issues

  _world->time += this->mTimespeed * dt;
  // animtime must advance every frame (matches reference noggit3). It drives liquid
  // texture-frame cycling (lava/water churn) which must animate regardless of the model
  // animation toggle. Gating it here is what froze all lava.
  _world->animtime += dt * 1000.0f;

  lightEditor->UpdateWorldTime();

  if (_draw_model_animations.get())
  {
    // Advance the model animation clock (bones/particles) only while enabled, so toggling it off
    // pauses models in place rather than letting per-instance models keep animating.
    _world->model_animtime += dt * 1000.0f;
    _world->update_models_emitters(dt);
  }

  if (_world->has_selection())
  {
    lastSelected = currentSelection;
  }

  if (_rotation_editor_need_update)
  {
    objectEditor->rotationEditor->updateValues(_world.get());
    _rotation_editor_need_update = false;
  }

  // [PERF 2026-07-25] Throttle the status-bar + area / zone-music / detail-widget / db-status display refresh
  // to ~10Hz. All of it is human-readable status (coords, area name, FPS, loaded/rendered counts, zone-music
  // polling, the selection detail widget, water UI) that nobody perceives faster than that -- yet it ran EVERY
  // frame and cost ~4ms of the tick in Stormwind (QLabel setText churn + getAreaID / getZoneMusic DBC lookups
  // + updateDetailInfos). Camera movement and edit actions are handled ABOVE this gate, so they're unaffected.
  static double s_status_accum = 0.0;
  s_status_accum += dt;
  if (s_status_accum >= 0.1)
  {
    s_status_accum = 0.0;

  QString status;
  status += ( QString ("tile: %1 %2")
            . arg (std::floor (_camera.position.x / TILESIZE))
            . arg (std::floor (_camera.position.z / TILESIZE))
            );
  status += ( QString ("; coordinates client: (%1, %2, %3), server: (%4, %5, %6)")
            . arg (_camera.position.x)
            . arg (_camera.position.z)
            . arg (_camera.position.y)
            . arg (ZEROPOINT - _camera.position.z)
            . arg (ZEROPOINT - _camera.position.x)
            . arg (_camera.position.y)
            );

  _status_position->setText (status);

  if (currentSelection.size() > 0)
  {
    _status_selection->setText ("");
  }
  else if (currentSelection.size() == 1)
  {
    switch (currentSelection.begin()->index())
    {
    case eEntry_Object:
      {
        auto obj = std::get<selected_object_type>(*currentSelection.begin());

        if (obj->which() == eMODEL)
        {
          auto instance(static_cast<ModelInstance*>(obj));
          _status_selection->setText
              ( QString ("%1: %2")
                    . arg (instance->uid)
                    . arg (QString::fromStdString (instance->model->file_key().stringRepr()))
              );
        }
        else if (obj->which() == eWMO)
        {
          auto instance(static_cast<WMOInstance*>(obj));
          _status_selection->setText
              ( QString ("%1: %2")
                    . arg (instance->uid)
                    . arg (QString::fromStdString (instance->wmo->file_key().stringRepr()))
              );
        }

        break;
      }
    case eEntry_MapChunk:
      {
      auto chunk(std::get<selected_chunk_type>(*currentSelection.begin()).chunk);
        _status_selection->setText
          (QString ("%1, %2").arg (chunk->px).arg (chunk->py));
        break;
      }
    }
  }

  updateDetailInfos();

  unsigned int const current_area_id = _world->getAreaID (_camera.position);
  _status_area->setText
    (QString::fromStdString (gAreaDB.getAreaName (current_area_id)));

  // Drive zone music from the current area only. Only while enabled (the dropdown checkbox), so nothing
  // music-related (incl. the QtMultimedia backend) runs when off.
  if (_zone_music_player && _zone_music_player->enabled())
  {
    // Music deliberately does NOT follow the day/night cycle -- it used to swap tracks at every dawn/dusk
    // as time advanced. Always request the zone's DAY music so it stays put regardless of time of day.
    bool const is_day = true;
    // World resolves WMOAreaTable first (cities/dungeons/caves -- Ironforge, Caverns of Time), then the
    // AreaTable parent chain, for BOTH the looping ZoneMusic and the one-shot ZoneIntroMusic.
    _zone_music_player->update_zone(_world->getZoneMusic(_camera.position),
                                    _world->getZoneIntroMusic(_camera.position), is_day);
  }

  {
    int time ((static_cast<int>(_world->time) % 2880) / 2);
    std::stringstream timestrs;
    timestrs << "Time: " << (time / 60) << ":" << std::setfill ('0')
             << std::setw (2) << (time % 60);


    timestrs << ", Pres: " << _tablet_manager->pressure();

    _status_time->setText (QString::fromStdString (timestrs.str()));
  }

  _last_fps_update += dt;

  // update fps every sec
  if (_last_fps_update > 1.f && !_last_frame_durations.empty())
  {
    auto avg_frame_duration
      ( std::accumulate ( _last_frame_durations.begin()
                        , _last_frame_durations.end()
                        , 0.
                        )
      / qreal (_last_frame_durations.size())
      );
    _status_fps->setText ( "FPS: " + QString::number (int (1. / avg_frame_duration)) 
                         + " - Average frame time: " + QString::number(avg_frame_duration*1000.0) + "ms"
                         );

    _last_frame_durations.clear();
    _last_fps_update = 0.f;
  }

  _status_culling->setText ( "Loaded tiles: " + QString::number(_world->getNumLoadedTiles())
                         + " Rendered tiles: " + QString::number(_world->getNumRenderedTiles())
                         + " Loaded objects: " + QString::number(_world->getModelInstanceStorage().getTotalModelsCount())
                         + " Rendered objects: " + QString::number(_world->getNumRenderedObjects())
  );

  updateDatabaseStatus();

  guiWater->updatePos (_camera.position);
  } // end ~10Hz status-display throttle
}

glm::vec4 MapView::normalized_device_coords (int x, int y) const
{
  return {2.0f * x / width() - 1.0f, 1.0f - 2.0f * y / height(), 0.0f, 1.0f};
}

float MapView::aspect_ratio() const
{
  return float (width()) / float (height());
}

math::ray MapView::intersect_ray() const
{
  float mx = _last_mouse_pos.x(), mz = _last_mouse_pos.y();

  if (_display_mode == display_mode::in_3D)
  {
    // during rendering we multiply perspective * view
    // so we need the same order here and then invert.
      glm::mat4x4 const invertedViewMatrix = glm::inverse(projection() * model_view());
      auto normalisedView = invertedViewMatrix * normalized_device_coords(mx, mz);

      auto pos = glm::vec3(normalisedView.x / normalisedView.w, normalisedView.y / normalisedView.w, normalisedView.z / normalisedView.w);

    return { _camera.position, pos - _camera.position };
  }
  else
  {
    glm::vec3 const pos
    ( _camera.position.x - (width() * 0.5f - mx) * _2d_zoom
    , _camera.position.y
    , _camera.position.z - (height() * 0.5f - mz) * _2d_zoom
    );
    
    return { pos, glm::vec3(0.f, -1.f, 0.f) };
  }
}

selection_result MapView::intersect_result(bool terrain_only, bool force_objects)
{
  selection_result results
  ( _world->intersect
    ( glm::transpose(model_view())
    , intersect_ray()
    , terrain_only
    , force_objects || terrainMode == editing_mode::object || terrainMode == editing_mode::minimap
    , _draw_terrain.get()
    , _draw_wmo.get()
    , _draw_models.get()
    , _draw_hidden_models.get()
    , _draw_wmo_exterior.get()
    )
  );

  std::sort ( results.begin()
            , results.end()
            , [](selection_entry const& lhs, selection_entry const& rhs)
              {
                return lhs.first < rhs.first;
              }
            );

  return std::move(results);
}

std::optional<glm::vec3> MapView::surface_pos_under_cursor()
{
  // _cursor_pos comes from a TERRAIN-ONLY raycast, which is right for the sculpt/paint brushes but
  // wrong for placing a spawn: dragging an NPC over a building dropped it through the roof onto the
  // ground underneath. Pick against terrain + WMOs + M2s instead and take the FIRST hit -- results are
  // already sorted by ray distance, so front() is the nearest surface the cursor is actually over.
  // Any hit type works because the entry's .first is the distance along the ray; a WMO/M2 entry
  // carries no hit position of its own, so we evaluate the ray at that distance.
  math::ray const ray(intersect_ray());
  selection_result const results(intersect_result(false, true));

  if (results.empty())
  {
    return std::nullopt;
  }

  return ray.position(results.front().first);
}

void MapView::doSelection (bool selectTerrainOnly, bool mouseMove)
{
  if (_world->get_selected_model_count() && _gizmo_on.get() && (_transform_gizmo.isUsing() || _transform_gizmo.isOver()))
    return;

  selection_result results(intersect_result(selectTerrainOnly));

  if (results.empty())
  {
    _world->reset_selection();
  }
  else
  {
    auto const& hit (results.front().second);

    if (terrainMode == editing_mode::object || terrainMode == editing_mode::minimap)
    {
      float radius = 0.0f;
      switch (terrainMode)
      {
        case editing_mode::object:
         radius = objectEditor->brushRadius();
         break;

        case editing_mode::minimap:
          radius = minimapTool->brushRadius();
          break;

        default:
          break;
      }

      if (_mod_shift_down)
      {
        if (hit.index() == eEntry_Object)
        {
          if (!_world->is_selected(hit))
          {
            _world->add_to_selection(hit);
          }
          else if (!mouseMove)
          {
            _world->remove_from_selection(hit);
          }
        }
        else if (hit.index() == eEntry_MapChunk)
        {
          _world->range_add_to_selection(_cursor_pos, radius, false);
        }
      }
      else if (_mod_ctrl_down)
      {
        if (hit.index() == eEntry_MapChunk)
        {
          _world->range_add_to_selection(_cursor_pos, radius, true);
        }
      }
      else if (!_mod_space_down && !_mod_alt_down && !_mod_ctrl_down)
      {
        // objectEditor->update_selection(_world.get());
        _world->reset_selection();
        _world->add_to_selection(hit);
      }
    }
    else if (hit.index() == eEntry_MapChunk && !mouseMove)
    {
      _world->reset_selection();
      _world->add_to_selection(hit);
    }

    auto action = NOGGIT_CUR_ACTION;

    if (!action || (!action->getBlockCursor()) || !_locked_cursor_mode.get())
    {
      _cursor_pos = hit.index() == eEntry_Object ? std::get<selected_object_type>(hit)->pos
                                                 : hit.index() == eEntry_MapChunk ? std::get<selected_chunk_type>(hit).position
                                                                                  : throw std::logic_error("bad variant");
    }

  }

  _rotation_editor_need_update = true;
  objectEditor->update_selection_ui(_world.get()); 
}

void MapView::update_cursor_pos()
{
  // PERF (2026-07-21): this does a full ray-vs-terrain pick (World::intersect) EVERY frame to place the 3D
  // brush cursor -- ~7ms, measured in the tick() phase (invisible to WorldDraw but real frame cost). The
  // cursor only matters when the camera is SETTLED for editing; while you fly / look around it's wasted.
  // Skip the pick on any frame the view is still changing -- it re-picks the instant you stop, and painting
  // (camera still, mouse dragging) still picks every frame. Exact == is correct here: an unchanged camera
  // keeps identical float bits, a moving one does not.
  {
    static glm::vec3 s_last_cam_pos(std::numeric_limits<float>::max());
    static float s_last_yaw = std::numeric_limits<float>::max();
    static float s_last_pitch = std::numeric_limits<float>::max();
    static float s_last_mx = std::numeric_limits<float>::max();
    static float s_last_mz = std::numeric_limits<float>::max();
    bool const cam_still = s_last_cam_pos == _camera.position
                        && s_last_yaw == _camera.yaw()._ && s_last_pitch == _camera.pitch()._;
    s_last_cam_pos = _camera.position;
    s_last_yaw = _camera.yaw()._;
    s_last_pitch = _camera.pitch()._;
    if (!cam_still)
    {
      s_last_mx = std::numeric_limits<float>::max(); // force a re-pick the instant the camera settles
      return;
    }
    // PERF (2026-07-24): camera settled. While idle-hovering with no mouse button held, a still mouse cannot
    // move the cursor, so the World::intersect pick is redundant -- skip it. This reclaims the same ~4-7ms
    // (tick phase) when paused over a dense scene, on top of the fly-time skip above. A held button
    // (painting/sculpting) always picks, and ANY mouse move re-picks on the next frame.
    float const cur_mx = static_cast<float>(_last_mouse_pos.x());
    float const cur_mz = static_cast<float>(_last_mouse_pos.y());
    bool const mouse_still = cur_mx == s_last_mx && cur_mz == s_last_mz;
    s_last_mx = cur_mx;
    s_last_mz = cur_mz;
    if (mouse_still && !leftMouse && !rightMouse)
      return;
  }

  static bool buffer_switch = false;

  if (false && terrainMode != editing_mode::holes) // figure out why this does not work on every hardware.
  {
    float mx = _last_mouse_pos.x(), mz = _last_mouse_pos.y();

    //gl.readBuffer(GL_FRONT);
    gl.bindBuffer(GL_PIXEL_PACK_BUFFER, _buffers[static_cast<unsigned>(buffer_switch)]);

    gl.readPixels(mx, height() - mz - 1, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 0);

    gl.bindBuffer(GL_PIXEL_PACK_BUFFER, _buffers[static_cast<unsigned>(!buffer_switch)]);
    GLushort* ptr = static_cast<GLushort*>(gl.mapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY));

    buffer_switch = !buffer_switch;

    if(ptr)
    {
      glm::vec4 viewport = glm::vec4(0, 0, width(), height());
      glm::vec3 wincoord = glm::vec3(mx, height() - mz - 1, static_cast<float>(*ptr) / std::numeric_limits<unsigned short>::max());

      glm::mat4x4 model_view_ = model_view();
      glm::mat4x4 projection_ = projection();

      glm::vec3 objcoord = glm::unProject(wincoord, model_view_,projection_, viewport);


      TileIndex tile({objcoord.x, objcoord.y, objcoord.z});

      if (!_world->mapIndex.tileLoaded(tile))
      {
        gl.unmapBuffer(GL_PIXEL_PACK_BUFFER);
        gl.bindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return;
      }

      _cursor_pos = {objcoord.x, objcoord.y, objcoord.z};

      gl.unmapBuffer(GL_PIXEL_PACK_BUFFER);
    }

    gl.bindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    return;
  }

  // Spawn tools aim at a SURFACE, not at the ground: the cursor must sit on a WMO floor/roof or a
  // doodad when you hover one, so the aim circle is visible there and a click places the spawn there.
  // The brush tools below stay terrain-only -- sculpting/painting acts on chunks, so a WMO hit would
  // be meaningless for them.
  if (terrainMode == editing_mode::creature || terrainMode == editing_mode::gameobject)
  {
    if (auto const surface = surface_pos_under_cursor())
    {
      _cursor_pos = *surface;
    }
    return;
  }

  // use raycasting for holes

  selection_result results (intersect_result (true));

  if (!results.empty())
  {
    auto const& hit(results.front().second);
    // hit cannot be something else than a chunk
    auto const& chunkHit = std::get<selected_chunk_type>(hit);
    _cursor_pos = chunkHit.position;

  }
}

glm::mat4x4 MapView::model_view() const
{
  if (_display_mode == display_mode::in_2D)
  {
    glm::vec3 eye = _camera.position;
    glm::vec3 target = eye;
    target.y -= 1.f;
    target.z -= 0.001f;
    auto center = target;
    auto up = glm::vec3(0.f, 1.f, 0.f);

    return glm::lookAt(eye, target, up);
  }
  else
  {
    return _camera.look_at_matrix();
  }
}

glm::mat4x4 MapView::projection() const
{
  float far_z = _settings->value("farZ", 900).toFloat();

  if (_display_mode == display_mode::in_2D)
  {
    float half_width = width() * 0.5f * _2d_zoom;
    float half_height = height() * 0.5f * _2d_zoom;

    return glm::ortho(-half_width, half_width, -half_height, half_height, -1.f, far_z);
  }
  else
  {
    return glm::perspective(_camera.fov()._, aspect_ratio(), 1.f, far_z);
  }
}

void MapView::draw_map()
{
  ZoneScoped;
  //! \ todo: make the current tool return the radius
  float radius = 0.0f, inner_radius = 0.0f, angle = 0.0f, orientation = 0.0f;
  glm::vec3 ref_pos;
  bool angled_mode = false, use_ref_pos = false;

  _cursorType = CursorType::CIRCLE;

  switch (terrainMode)
  {
  case editing_mode::ground:
    radius = terrainTool->brushRadius();
    inner_radius = terrainTool->innerRadius();
    if ((terrainTool->_edit_type != eTerrainType_Vertex || terrainTool->_edit_type != eTerrainType_Script) && terrainTool->getImageMaskSelector()->isEnabled())
      _cursorType = CursorType::STAMP;
    break;
  case editing_mode::flatten_blur:
    radius = flattenTool->brushRadius();
    angle = flattenTool->angle();
    orientation = flattenTool->orientation();
    ref_pos = flattenTool->ref_pos();
    angled_mode = flattenTool->angled_mode();
    use_ref_pos = flattenTool->use_ref_pos();
    break;
  case editing_mode::paint:
    radius = texturingTool->brush_radius();
    inner_radius = texturingTool->hardness();
    if(texturingTool->getTexturingMode() == Noggit::Ui::texturing_mode::paint && texturingTool->getImageMaskSelector()->isEnabled())
      _cursorType = CursorType::STAMP;
    break;
  case editing_mode::stamp:
    radius = stampTool->getRadius();
    inner_radius = stampTool->getInnerRadius();
    if(stampTool->getActiveBrushItem() && stampTool->getActiveBrushItem()->isMaskEnabled())
      _cursorType = CursorType::STAMP;
    break;
  case editing_mode::water:
    radius = guiWater->brushRadius();
    angle = guiWater->angle();
    orientation = guiWater->orientation();
    ref_pos = guiWater->ref_pos();
    angled_mode = guiWater->angled_mode();
    use_ref_pos = guiWater->use_ref_pos();
    break;
  case editing_mode::mccv:
    radius = shaderTool->brushRadius();
      if(shaderTool->getImageMaskSelector()->isEnabled())
        _cursorType = CursorType::STAMP;
    break;
  case editing_mode::areaid:
    radius = ZoneIDBrowser->brushRadius();
    break;
  case editing_mode::holes:
    radius = holeTool->brushRadius();
    break;
  case editing_mode::object:
    radius = objectEditor->brushRadius();
    break;
  case editing_mode::creature:
  case editing_mode::gameobject:
    // No brush in the spawn tools -- show a fixed aim circle on the ground under the cursor.
    // Small on purpose: it marks a placement POINT, so a wide ring just obscures what you are
    // aiming at. (Was 3.5, then 0.7.)
    radius = 0.4f;
    break;
  case editing_mode::minimap:
    radius = minimapTool->brushRadius();
    break;
  case editing_mode::scripting:
    radius = scriptingTool->get_settings()->brushRadius();
    inner_radius = scriptingTool->get_settings()->innerRadius();
    break;
  default:
    break;
  }

  //! \note Select terrain below mouse, if no item selected or the item is map.
  if (!_capture_probe
    && !(_world->has_selection()
    || _locked_cursor_mode.get()))
  {
    noggit::perf::Scoped _prof_sel(noggit::perf::Phase::Selection);
    doSelection(true);
  }

  if (_camera_moved_since_last_draw)
  {
      // PERF (2026-07-21): _minimap->update() schedules a full repaint of the minimap widget, which Qt
      // runs in the event loop BETWEEN paintGL calls -- i.e. inside the frame-to-frame gap, invisible to
      // WorldDraw but counted in Frame. Firing it every camera-move frame (i.e. every frame while panning)
      // is the "massive performance issues" flagged in tick() above. 4 Hz is ample for an overview marker.
      static QElapsedTimer s_minimap_throttle;
      if (!s_minimap_throttle.isValid() || s_minimap_throttle.elapsed() > 250)
      {
        _minimap->update();
        s_minimap_throttle.restart();
      }
  }

  bool classic_ui = _settings->value("classicUI", true).toBool();
  bool show_unpaintable = classic_ui ? texturingTool->show_unpaintable_chunks() : _left_sec_toolbar->showUnpaintableChunk();
  _world->renderer()->draw (
                 model_view()
               , projection()
               , _cursor_pos
               , _cursorRotation
               , terrainMode == editing_mode::mccv ? shaderTool->shaderColor() : cursor_color
               , _cursorType
               , radius
               , show_unpaintable
               , _left_sec_toolbar->drawOnlyInsideSphereLight()
               , _left_sec_toolbar->drawWireframeSphereLight()
               , _left_sec_toolbar->getAlphaSphereLight()
               , inner_radius
               , ref_pos
               , angle
               , orientation
               , use_ref_pos
               , angled_mode
               , terrainMode == editing_mode::paint
               , terrainMode
               , _camera.position
               , _camera_moved_since_last_draw
               , _draw_mfbo.get()
               , _draw_terrain.get()
               , _draw_wmo.get()
               , _draw_water.get()
               , _draw_wmo_doodads.get()
               , _draw_models.get()
               , _draw_model_animations.get()
               , _draw_models_with_box.get()
               , _draw_hidden_models.get()
               , minimapTool->getMinimapRenderSettings()
               , _draw_fog.get()
               , terrainTool->_edit_type
               , _display_all_water_layers.get() ? -1 : _displayed_water_layer.get()
               , _display_mode
               , _draw_occlusion_boxes.get()
               ,false
               , _draw_wmo_exterior.get()
               , _draw_bloom.get()
               , _draw_ground_clutter.get()
               );

  // reset after each world::draw call
  _camera_moved_since_last_draw = false;
}

void MapView::setCameraForCapture(glm::vec3 const& position, math::degrees yaw, math::degrees pitch)
{
  _camera.position = position;
  _camera.yaw(yaw);
  _camera.pitch(pitch);
  _camera_moved_since_last_draw = true;
  _needs_redraw = true;
  update();
}

QImage MapView::grabRenderedFrameForCapture()
{
  if (width() <= 0 || height() <= 0)
  {
    return {};
  }

  if (capture_debug_enabled())
  {
    LogDebug << "MapView::grabRenderedFrameForCapture begin size="
             << width() << "x" << height()
             << " gl_initialized=" << _gl_initialized
             << std::endl;
  }
  makeCurrent();
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::grabRenderedFrameForCapture after makeCurrent" << std::endl;
  }
  OpenGL::context::scoped_setter const _(::gl, context());
  _needs_redraw = true;
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::grabRenderedFrameForCapture before paintGL" << std::endl;
  }
  paintGL();
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::grabRenderedFrameForCapture after paintGL" << std::endl;
  }
  glFinish();
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::grabRenderedFrameForCapture after glFinish" << std::endl;
  }

  QImage image(width(), height(), QImage::Format_RGBA8888);
  if (!image.isNull())
  {
    gl.readPixels(0, 0, width(), height(), GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    image = image.mirrored(false, true);
  }
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::grabRenderedFrameForCapture after readPixels null=" << image.isNull() << std::endl;
  }

  doneCurrent();
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::grabRenderedFrameForCapture end" << std::endl;
  }
  return image;
}

bool MapView::event(QEvent* e)
{
  // Suppress shortcut triggers for Z/X while in the creature/gameobject editor so they can raise/
  // lower the selected spawn instead of firing their menu shortcuts.
  if (e->type() == QEvent::ShortcutOverride
      && (terrainMode == editing_mode::creature || terrainMode == editing_mode::gameobject))
  {
    auto* ke = static_cast<QKeyEvent*>(e);
    if (ke->key() == Qt::Key_Z || ke->key() == Qt::Key_X)
    {
      e->accept();
      return true;
    }
  }
  return QOpenGLWidget::event(e);
}

bool MapView::eventFilter(QObject* obj, QEvent* e)
{
  // Keep the centred time globe from being shoved when the left secondary toolbar (patrol paths /
  // creature info, terrain-mode options) appears at the far left of the globe's row: mirror its width
  // into a spacer on the far right so the globe block stays centred on the true viewport centre.
  if (_globe_balance_spacer && _viewport_overlay_ui
      && obj == _viewport_overlay_ui->leftSecondaryToolbarHolder)
  {
    switch (e->type())
    {
      case QEvent::Resize:
      case QEvent::Show:
      case QEvent::Hide:
      {
        auto* holder = _viewport_overlay_ui->leftSecondaryToolbarHolder;
        int const w = holder->isVisible() ? std::max(holder->width(), holder->sizeHint().width()) : 0;
        if (_globe_balance_spacer->width() != w)
        {
          _globe_balance_spacer->setFixedWidth(w);
        }
        break;
      }
      default:
        break;
    }
  }
  return QOpenGLWidget::eventFilter(obj, e);
}

void MapView::keyPressEvent (QKeyEvent *event)
{
  // Creature / GameObject tools: X raises, Z lowers the selected spawn(s). Handled before the hotkey
  // loop so X/Z don't trigger their menu shortcuts (e.g. texture browser) while editing spawns.
  if ((terrainMode == editing_mode::creature || terrainMode == editing_mode::gameobject)
      && (event->key() == Qt::Key_X || event->key() == Qt::Key_Z))
  {
    bool const has_selection = terrainMode == editing_mode::creature
      ? _selected_creature_spawn_guid.has_value()
      : _selected_gameobject_spawn_guid.has_value();

    if (has_selection)
    {
      float const step = (event->modifiers() & Qt::ShiftModifier) ? 2.0f : 0.5f; // Shift = coarse
      glm::vec3 const delta(0.0f, event->key() == Qt::Key_X ? step : -step, 0.0f); // X up, Z down

      if (terrainMode == editing_mode::creature)
        translateSelectedCreatureSpawns(delta);
      else
        translateSelectedGameObjectSpawns(delta);

      _needs_redraw = true;
      return;
    }
  }

  size_t const modifier
    ( ((event->modifiers() & Qt::ShiftModifier) ? MOD_shift : 0)
    | ((event->modifiers() & Qt::ControlModifier) ? MOD_ctrl : 0)
    | ((event->modifiers() & Qt::AltModifier) ? MOD_alt : 0)
    | ((event->modifiers() & Qt::MetaModifier) ? MOD_meta : 0)
    | ((event->modifiers() & Qt::KeypadModifier) ? MOD_num : 0)
    | (_mod_space_down ? MOD_space : 0)
    );

  for (auto&& hotkey : hotkeys)
  {
    if (event->key() == hotkey.key && modifier == hotkey.modifiers && hotkey.condition())
    {
      makeCurrent();
      OpenGL::context::scoped_setter const _ (::gl, context());

      hotkey.function();
      return;
    }
  }

  if (event->key() == Qt::Key_Space)
    _mod_space_down = true;

  if (event->key() == Qt::Key_Z) _mod_z_down = true;
  if (event->key() == Qt::Key_X) _mod_x_down = true;

  checkInputsSettings();

  // movement
  if (event->key() == _inputs[0])
  {
    moving = 1.0f;
  }
  if (event->key() == _inputs[1])
  {
    moving = -1.0f;
  }

  if (event->key() == Qt::Key_Up)
  {
    lookat = 0.75f;
  }
  if (event->key() == Qt::Key_Down)
  {
    lookat = -0.75f;
  }

  if (event->key() == Qt::Key_Right)
  {
    turn = 0.75f;
  }
  if (event->key() == Qt::Key_Left)
  {
    turn = -0.75f;
  }

  if (event->key() == _inputs[2])
  {
    strafing = 1.0f;
  }
  if (event->key() == _inputs[3])
  {
    strafing = -1.0f;
  }

  if (event->key() == _inputs[4])
  {
    updown = 1.0f;
  }
  if (event->key() == _inputs[5])
  {
    updown = -1.0f;
  }

  if (event->key() == Qt::Key_2 && event->modifiers() & Qt::KeypadModifier)
  {
    keyx = 1;
  }
  if (event->key() == Qt::Key_8 && event->modifiers() & Qt::KeypadModifier)
  {
    keyx = -1;
  }

  if (event->key() == Qt::Key_4 && event->modifiers() & Qt::KeypadModifier)
  {
    keyz = 1;
  }
  if (event->key() == Qt::Key_6 && event->modifiers() & Qt::KeypadModifier)
  {
    keyz = -1;
  }

  if (event->key() == Qt::Key_3 && event->modifiers() & Qt::KeypadModifier)
  {
    keyy = 1;
  }
  if (event->key() == Qt::Key_1 && event->modifiers() & Qt::KeypadModifier)
  {
    keyy = -1;
  }

  if (event->key() == Qt::Key_7 && event->modifiers() & Qt::KeypadModifier)
  {
    keyr = 1;
  }
  if (event->key() == Qt::Key_9 && event->modifiers() & Qt::KeypadModifier)
  {
    keyr = -1;
  }

  if (event->key() == Qt::Key_Plus)
  {
    keys = 1;

    switch (terrainMode)
    {
      case editing_mode::mccv:
      {
        shaderTool->addColorToPalette();
        break;
      }
      default:
        break;
    }
  }
  if (event->key() == Qt::Key_Minus)
  {
    keys = -1;
  }
  if (event->key() == Qt::Key_Home)
  {
	  _camera.position = glm::vec3(_cursor_pos.x, _cursor_pos.y + 50, _cursor_pos.z);
    _camera_moved_since_last_draw = true;
  }

  if (event->key() == Qt::Key_L)
  {
    freelook = true;
  }

  if (_display_mode == display_mode::in_2D)
  {
    TileIndex cur_tile = TileIndex(_camera.position);

    if (event->key() == Qt::Key_Up)
    {
      auto next_z = cur_tile.z - 1;
      _camera.position = glm::vec3((cur_tile.x * TILESIZE) + (TILESIZE / 2), _camera.position.y, (next_z * TILESIZE) + (TILESIZE / 2));
      _camera_moved_since_last_draw = true;
    }
    else if (event->key() == Qt::Key_Down)
    {
      auto next_z = cur_tile.z + 1;
      _camera.position = glm::vec3((cur_tile.x * TILESIZE) + (TILESIZE / 2), _camera.position.y, (next_z * TILESIZE) + (TILESIZE / 2));
      _camera_moved_since_last_draw = true;
    }
    else if (event->key() == Qt::Key_Left)
    {
      auto next_x = cur_tile.x - 1;
      _camera.position = glm::vec3((next_x * TILESIZE) + (TILESIZE / 2), _camera.position.y, (cur_tile.z * TILESIZE) + (TILESIZE / 2));
      _camera_moved_since_last_draw = true;
    }
    else if (event->key() == Qt::Key_Right)
    {
      auto next_x = cur_tile.x + 1;
      _camera.position = glm::vec3((next_x * TILESIZE) + (TILESIZE / 2), _camera.position.y, (cur_tile.z * TILESIZE) + (TILESIZE / 2));
      _camera_moved_since_last_draw = true;
    }

  }

  if (_gizmo_on.get() && !_transform_gizmo.isUsing())
  {
    if (!_change_operation_mode && event->key() == Qt::Key_Space)
    {
      if (_gizmo_operation == ImGuizmo::OPERATION::TRANSLATE)
      {
        updateGizmoOverlay(ImGuizmo::OPERATION::ROTATE);
      }
      else if (_gizmo_operation == ImGuizmo::OPERATION::ROTATE)
      {
        updateGizmoOverlay(ImGuizmo::OPERATION::SCALE);
      }
      else
      {
        updateGizmoOverlay(ImGuizmo::OPERATION::TRANSLATE);
      }

      _change_operation_mode = true;
    }
  }
}

void MapView::keyReleaseEvent (QKeyEvent* event)
{
  if (event->key() == Qt::Key_Space)
    _mod_space_down = false;

  if (event->key() == Qt::Key_Z) _mod_z_down = false;
  if (event->key() == Qt::Key_X) _mod_x_down = false;

  if (_change_operation_mode && event->key() == Qt::Key_Space)
    _change_operation_mode = false;

  checkInputsSettings();

  // movement
  if (event->key() == _inputs[0] || event->key() == _inputs[1])
  {
    moving = 0.0f;
  }

  if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)
  {
    lookat = 0.0f;
  }

  if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Left)
  {
    turn  = 0.0f;
  }

  if (event->key() == _inputs[2] || event->key() == _inputs[3])
  {
    strafing  = 0.0f;
  }

  if (event->key() == _inputs[4] || event->key() == _inputs[5])
  {
    updown  = 0.0f;
  }
  

  if ((event->key() == Qt::Key_2 || event->key() == Qt::Key_8) && event->modifiers() & Qt::KeypadModifier)
  {
    keyx = 0.0f;
  }

  if ((event->key() == Qt::Key_4 || event->key() == Qt::Key_6) && event->modifiers() & Qt::KeypadModifier)
  {
    keyz = 0.0f;
  }

  if ((event->key() == Qt::Key_3 || event->key() == Qt::Key_1) && event->modifiers() & Qt::KeypadModifier)
  {
    keyy = 0.0f;
  }

  if ((event->key() == Qt::Key_7 || event->key() == Qt::Key_9) && event->modifiers() & Qt::KeypadModifier)
  {
    keyr  = 0.0f;
  }

  if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Minus)
  {
    keys = 0.0f;
  }

  if (event->key() == Qt::Key_L || event->key() == Qt::Key_Minus)
  {
    freelook = false;
  }

}

void MapView::checkInputsSettings()
{
  QString _locale = _settings->value("keyboard_locale", "QWERTY").toString();

  // default is QWERTY
  _inputs = std::array<Qt::Key, 6>{Qt::Key_W, Qt::Key_S, Qt::Key_D, Qt::Key_A, Qt::Key_Q, Qt::Key_E};

  if (_locale == "AZERTY")
  {
      _inputs = std::array<Qt::Key, 6>{Qt::Key_Z, Qt::Key_S, Qt::Key_D, Qt::Key_Q, Qt::Key_A, Qt::Key_E};
  }
}

void MapView::focusOutEvent (QFocusEvent*)
{
  _mod_alt_down = false;
  _mod_z_down = false;
  _mod_x_down = false;
  _mod_ctrl_down = false;
  _mod_shift_down = false;
  _mod_space_down = false;
  _mod_num_down = false;

  moving = 0.0f;
  lookat = 0.0f;
  turn = 0.0f;
  strafing = 0.0f;
  updown = 0.0f;

  keyx = 0;
  keyz = 0;
  keyy = 0;
  keyr = 0;
  keys = 0;

  leftMouse = false;
  rightMouse = false;
  MoveObj = false;
  look = false;
  freelook = false;
}

void MapView::mouseMoveEvent (QMouseEvent* event)
{
  //! \todo:  move the function call requiring a context in tick ?
  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());
  QLineF const relative_movement (_last_mouse_pos, event->pos());

  if ((look || freelook) && !(_mod_shift_down || _mod_ctrl_down || _mod_alt_down || _mod_space_down))
  {
    _camera.add_to_yaw(math::degrees(relative_movement.dx() / XSENS));
    _camera.add_to_pitch(math::degrees(mousedir * relative_movement.dy() / YSENS));
    _camera_moved_since_last_draw = true;
  }

  // Drag placement follows the nearest SOLID surface (WMO roof / doodad / terrain). Picked here
  // rather than reusing _cursor_pos because the per-frame cursor update bails out while the camera is
  // moving -- so dragging a spawn WHILE flying would otherwise freeze it at a stale point. The result
  // is written back to _cursor_pos so the aim circle tracks the drag too.
  if ((_dragging_creature_spawn || _dragging_gameobject_spawn) && leftMouse)
  {
    if (auto const surface = surface_pos_under_cursor())
    {
      _cursor_pos = *surface;
    }

    if (_dragging_creature_spawn)
    {
      updateSelectedCreatureSpawnPosition(_cursor_pos);
    }
    else
    {
      updateSelectedGameObjectSpawnPosition(_cursor_pos);
    }

    _last_mouse_pos = event->pos();
    return;
  }

  if (leftMouse
      && (terrainMode == editing_mode::creature || terrainMode == editing_mode::gameobject)
      && _area_selection->isVisible()
      && _display_mode == display_mode::in_3D
      && !ImGuizmo::IsUsing())
  {
    _needs_redraw = true;
    _area_selection->setGeometry(QRect(_drag_start_pos, event->pos()).normalized());
  }

  updateCreatureSpawnHover(event->globalPos());
  updateGameObjectSpawnHover(event->globalPos());

  if (MoveObj)
  {
    mh = -aspect_ratio()*relative_movement.dx() / static_cast<float>(width());
    mv = -relative_movement.dy() / static_cast<float>(height());
  }
  else
  {
    mh = 0.0f;
    mv = 0.0f;
  }

  if (_mod_shift_down || _mod_ctrl_down || _mod_alt_down || _mod_space_down)
  {
    rh = relative_movement.dx() / XSENS * 5.0f;
    rv = relative_movement.dy() / YSENS * 5.0f;
  }

  if (rightMouse && _mod_alt_down)
  {
    if (terrainMode == editing_mode::ground)
    {
      if (terrainTool->_edit_type == eTerrainType_Vertex)
      {
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                     Noggit::ActionModalityControllers::eALT | Noggit::ActionModalityControllers::eRMB);
        terrainTool->changeOrientation (-relative_movement.dx() / XSENS * 4.5f);
      }
      else
      {
        terrainTool->changeInnerRadius(relative_movement.dx() / 100.0f);
      }

    }
    else if (terrainMode == editing_mode::paint)
    {
      texturingTool->change_hardness(relative_movement.dx() / 300.0f);
    }
    else if (terrainMode == editing_mode::stamp)
    {
      stampTool->changeInnerRadius(relative_movement.dx() / 300.0f);
    }
  }

  if (rightMouse && _mod_shift_down)
  {
    if (terrainMode == editing_mode::ground)
    {
      if (terrainTool->_edit_type == eTerrainType_Vertex)
      {
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                       Noggit::ActionModalityControllers::eSHIFT | Noggit::ActionModalityControllers::eRMB);
        terrainTool->moveVertices (_world.get(), -relative_movement.dy() / YSENS);
      }
    }
  }

  if (rightMouse && _mod_ctrl_down)
  {
    if (terrainMode == editing_mode::ground)
    {
      if (terrainTool->_edit_type == eTerrainType_Vertex)
      {
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                       Noggit::ActionModalityControllers::eCTRL |
                                                       Noggit::ActionModalityControllers::eRMB);
        terrainTool->changeAngle(-relative_movement.dy() / YSENS * 4.f);
      }
    }
  }


  if (rightMouse && _mod_space_down)
  {
    if (terrainMode == editing_mode::ground)
    {
      if (terrainTool->_edit_type == eTerrainType_Vertex)
      {
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                       Noggit::ActionModalityControllers::eRMB
                                                       | Noggit::ActionModalityControllers::eSPACE);
        terrainTool->setOrientRelativeTo(_world.get(), _cursor_pos);
      }
      else if (terrainTool->getImageMaskSelector()->isEnabled())
      {
        auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eDO_NOT_WRITE_HISTORY,
                                                       Noggit::ActionModalityControllers::eRMB
                                                       | Noggit::ActionModalityControllers::eSPACE);
        terrainTool->getImageMaskSelector()->setRotation(-relative_movement.dx() / XSENS * 10.f);
        action->setBlockCursor(true);
      }

    }
    else if (terrainMode == editing_mode::paint)
    {
      if (texturingTool->getImageMaskSelector()->isEnabled())
      {
        auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eDO_NOT_WRITE_HISTORY,
                                                                     Noggit::ActionModalityControllers::eRMB
                                                                     | Noggit::ActionModalityControllers::eSPACE);
        texturingTool->getImageMaskSelector()->setRotation(-relative_movement.dx() / XSENS * 10.f);
        action->setBlockCursor(true);

      }

    }
    else if (terrainMode == editing_mode::mccv)
    {
      if (shaderTool->getImageMaskSelector()->isEnabled())
      {
        auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eDO_NOT_WRITE_HISTORY,
                                                                     Noggit::ActionModalityControllers::eRMB
                                                                     | Noggit::ActionModalityControllers::eSPACE);
        shaderTool->getImageMaskSelector()->setRotation(-relative_movement.dx() / XSENS * 10.f);
        action->setBlockCursor(true);

      }

    }
    else if (terrainMode == editing_mode::stamp)
    {

      auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eDO_NOT_WRITE_HISTORY,
                                                                   Noggit::ActionModalityControllers::eRMB
                                                                   | Noggit::ActionModalityControllers::eSPACE);
      stampTool->changeRotation(-relative_movement.dx() / XSENS * 10.f);
      action->setBlockCursor(true);
    }
  }

  if (leftMouse && _mod_alt_down)
  {
	switch (terrainMode)
    {
    case editing_mode::ground:
      terrainTool->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::flatten_blur:
      flattenTool->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::paint:
      texturingTool->change_radius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::water:
      guiWater->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::mccv:
      shaderTool->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::areaid:
      ZoneIDBrowser->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::holes:
      holeTool->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::object:
      objectEditor->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::minimap:
      minimapTool->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::stamp:
      stampTool->changeRadius(relative_movement.dx() / XSENS);
      break;
    default:
      break;
    }
  }

  if (leftMouse && _mod_space_down)
  {
    switch (terrainMode)
    {
    case editing_mode::ground:
      terrainTool->changeSpeed(relative_movement.dx() / 30.0f);
      break;
    case editing_mode::flatten_blur:
      flattenTool->changeSpeed(relative_movement.dx() / 30.0f);
      break;
    case editing_mode::paint:
      texturingTool->change_pressure(relative_movement.dx() / 300.0f);
      break;
    case editing_mode::mccv:
      shaderTool->changeSpeed(relative_movement.dx() / XSENS);
      break;
    case editing_mode::stamp:
      stampTool->changeSpeed(relative_movement.dx() / XSENS);
      break;
    default:
      break;
    }
  }

  if (leftMouse && (_mod_shift_down || _mod_ctrl_down))
  {
    if (terrainMode == editing_mode::object || terrainMode == editing_mode::minimap)
    {
      doSelection(false, true); // Required for radius selection in Object mode
    }
  }

  if (leftMouse && _mod_shift_down)
  {
    if (terrainMode == editing_mode::ground && _display_mode == display_mode::in_3D)
    {
      auto image_mask_selector = terrainTool->getImageMaskSelector();
      if (terrainTool->_edit_type != eTerrainType_Vertex && terrainTool->_edit_type != eTerrainType_Script &&
        image_mask_selector->isEnabled() && !image_mask_selector->getBrushMode())
      {
        auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                       Noggit::ActionModalityControllers::eSHIFT
                                                       | Noggit::ActionModalityControllers::eLMB);

        action->setPostCallback(&MapView::randomizeTerrainRotation);

        terrainTool->changeTerrain(_world.get(), _cursor_pos, relative_movement.dx() / 30.0f);
      }
    }
    else if (terrainMode == editing_mode::stamp && _display_mode == display_mode::in_3D && !stampTool->getBrushMode())
    {
      auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eNO_FLAG,
                                                                   Noggit::ActionModalityControllers::eSHIFT
                                                                   | Noggit::ActionModalityControllers::eLMB);

      action->setPostCallback(&MapView::randomizeStampRotation);
      action->setBlockCursor(true);

      stampTool->execute(_cursor_pos, _world.get(), relative_movement.dx() / 30.0f, _mod_shift_down, _mod_alt_down, _mod_ctrl_down, false);
    }

  }

  if (leftMouse && terrainMode == editing_mode::object && _display_mode == display_mode::in_3D && !ImGuizmo::IsUsing())
  {
      _needs_redraw = true;
      _area_selection->setGeometry(QRect(_drag_start_pos, event->pos()).normalized());
  }

  if (_display_mode == display_mode::in_2D && leftMouse && _mod_alt_down && _mod_shift_down)
  {
    strafing = ((relative_movement.dx() / XSENS) / -1) * 5.0f;
    moving = (relative_movement.dy() / YSENS) * 5.0f;
  }

  if (_display_mode == display_mode::in_2D && rightMouse && _mod_shift_down)
  {
    updown = (relative_movement.dy() / YSENS);
  }

  _last_mouse_pos = event->pos();
}

void MapView::change_selected_wmo_nameset(int set)
{
    auto last_entry = _world->get_last_selected_model();
    if (last_entry)
    {
        if (last_entry.value().index() != eEntry_Object)
        {
            return;
        }
        auto obj = std::get<selected_object_type>(last_entry.value());
        if (obj->which() == eWMO)
        {
            WMOInstance* wmo = static_cast<WMOInstance*>(obj);
            wmo->change_nameset(set);
            _world->updateTilesWMO(wmo, model_update::none); // needed?
            auto tiles = wmo->getTiles();
            for (auto tile : tiles)
            {
                tile->changed = true;
            }
        }
    }
}

void MapView::change_selected_wmo_doodadset(int set)
{
  for (auto& selection : _world->current_selection())
  {
    if (selection.index() != eEntry_Object)
      continue;

    auto obj = std::get<selected_object_type>(selection);

    if (obj->which() == eWMO)
    {
      auto wmo = static_cast<WMOInstance*>(obj);
      wmo->change_doodadset(set);
      _world->updateTilesWMO(wmo, model_update::none);
      auto tiles = wmo->getTiles();
      for (auto tile : tiles)
      {
        tile->changed = true;
      }
    }
  }
}

void MapView::mousePressEvent(QMouseEvent* event)
{
  if(event->source() == Qt::MouseEventNotSynthesized)
  {
    _tablet_manager->setIsActive(false);
  }

  makeCurrent();
  OpenGL::context::scoped_setter const _(::gl, context());

  switch (event->button())
  {
  case Qt::LeftButton:
    leftMouse = true;
    break;

  case Qt::RightButton:
    rightMouse = true;
    break;

  case Qt::MiddleButton:
    if (_world->has_selection())
    {
      MoveObj = true;
    }

    if(terrainMode == editing_mode::mccv)
    {
      shaderTool->pickColor(_world.get(), _cursor_pos);
    }
    break;

  default:
    break;
  }

  if (leftMouse && terrainMode == editing_mode::creature)
  {
      if (_mod_shift_down)
      {
        _drag_start_pos = event->pos();
        _needs_redraw = true;
        _area_selection->setGeometry(QRect(_drag_start_pos, QSize()));
        _area_selection->show();
        return;
      }

      tryStartCreatureSpawnDrag();
      _area_selection->hide();
      return;
  }

  if (leftMouse && terrainMode == editing_mode::gameobject)
  {
      if (_mod_shift_down)
      {
        _drag_start_pos = event->pos();
        _needs_redraw = true;
        _area_selection->setGeometry(QRect(_drag_start_pos, QSize()));
        _area_selection->show();
        return;
      }

      tryStartGameObjectSpawnDrag();
      _area_selection->hide();
      return;
  }

  if (leftMouse && ((terrainMode == editing_mode::object || terrainMode == editing_mode::minimap) && !_mod_ctrl_down))
  {
      if (_mod_shift_down && tryStartCreatureSpawnDrag())
      {
        _area_selection->hide();
        return;
      }

      _drag_start_pos = event->pos();
      _needs_redraw = true;
      _area_selection->setGeometry(QRect(_drag_start_pos, QSize()));
      _area_selection->show();
  }

  if (rightMouse)
  {
    _right_click_pos = event->pos();
    look = true;
  }
}

void MapView::wheelEvent (QWheelEvent* event)
{
  //! \todo: move the function call requiring a context in tick ?
  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());

  // While DRAGGING a spawn, the wheel rotates it instead of doing whatever the tool normally does.
  // Every dragged spawn turns about its own centre, so a multi-selection keeps its layout and each
  // member faces the new way. Wheel up = clockwise. Default 1 deg per notch, Shift = 10 deg,
  // Ctrl = 45 deg.
  if (_dragging_creature_spawn || _dragging_gameobject_spawn)
  {
    float const notches = static_cast<float>(event->angleDelta().y()) / 120.0f;
    if (notches != 0.0f)
    {
      float const step = _mod_shift_down ? 10.0f : (_mod_ctrl_down ? 45.0f : 1.0f);
      rotateDraggedSpawns(-notches * step); // negative: wheel up reads as clockwise on screen
    }

    event->accept();
    return;
  }

  auto&& delta_for_range
    ( [&] (float range)
      {
        //! \note / 8.f for degrees, / 40.f for smoothness
        return (_mod_ctrl_down ? 0.01f : 0.1f) 
          * range 
          // alt = horizontal delta
          * (_mod_alt_down ? event->angleDelta().x() : event->angleDelta().y())
          / 320.f
          ;
      }
    );

  if (terrainMode == editing_mode::paint)
  {
    if (_mod_space_down)
    {
      texturingTool->change_brush_level (delta_for_range (255.f));
    }
    else if (_mod_alt_down)
    {
      texturingTool->change_spray_size (delta_for_range (39.f));
    }
    else if (_mod_shift_down)
    {
      texturingTool->change_spray_pressure (delta_for_range (10.f));
    }
  }
  else if (terrainMode == editing_mode::flatten_blur)
  {
    if (_mod_alt_down)
    {
      flattenTool->changeOrientation (delta_for_range (360.f));
    }
    else if (_mod_shift_down)
    {
      flattenTool->changeAngle (delta_for_range (89.f));
    }
    else if (_mod_space_down)
    {
      //! \note not actual range
      flattenTool->changeHeight (delta_for_range (40.f));
    }
  }
  else if (terrainMode == editing_mode::water)
  {
    if (_mod_alt_down)
    {
      guiWater->changeOrientation (delta_for_range (360.f));
    }
    else if (_mod_shift_down)
    {
      guiWater->changeAngle (delta_for_range (89.f));
    }
    else if (_mod_space_down)
    {
      //! \note not actual range
      guiWater->change_height (delta_for_range (40.f));
    }
  }
}

void MapView::mouseReleaseEvent (QMouseEvent* event)
{
  makeCurrent();
  OpenGL::context::scoped_setter const _(::gl, context());

  switch (event->button())
  {
  case Qt::LeftButton:
    if (_dragging_creature_spawn)
    {
      leftMouse = false;
      _dragging_creature_spawn = false;
      _creature_drag_anchor_pos = std::optional<glm::vec3>();

      // Only the dragged spawns changed, so retext just their rows. A full rebuild here was a ~1s
      // freeze on mouse-up (it allocates an item per spawn in the world). Capture the guids before
      // clearing the drag state.
      std::vector<std::uint32_t> moved;
      moved.reserve(_creature_drag_initial_positions.size());
      for (auto const& drag_state : _creature_drag_initial_positions)
      {
        moved.push_back(drag_state.first);
      }
      _creature_drag_initial_positions.clear();

      updateDatabaseStatus();
      refreshCreatureBrowserItems(moved);
      refreshCreatureEditorKnobs();
      break;
    }

    if (_dragging_gameobject_spawn)
    {
      leftMouse = false;
      _dragging_gameobject_spawn = false;
      _gameobject_drag_anchor_pos = std::optional<glm::vec3>();

      std::vector<std::uint32_t> moved;
      moved.reserve(_gameobject_drag_initial_positions.size());
      for (auto const& drag_state : _gameobject_drag_initial_positions)
      {
        moved.push_back(drag_state.first);
      }
      _gameobject_drag_initial_positions.clear();

      updateGameObjectBrowserStatus();
      refreshGameObjectBrowserItems(moved);
      refreshGameObjectEditorKnobs();
      break;
    }

    leftMouse = false;

    if (_display_mode == display_mode::in_2D)
    {
      strafing = 0;
      moving = 0;
    }

    if (terrainMode == editing_mode::creature)
    {
      auto drag_end_pos = event->pos();

      if (_area_selection->isVisible())
      {
        if (_drag_start_pos != drag_end_pos && !ImGuizmo::IsUsing())
        {
          selectCreatureSpawnsInArea(QRect(_drag_start_pos, drag_end_pos), true);
        }
        else if (auto guid = findCreatureSpawnAtCursor())
        {
          addCreatureSpawnToSelection(*guid);
        }

        _area_selection->hide();
        break;
      }

      if (auto guid = findCreatureSpawnAtCursor())
      {
        setSelectedCreatureSpawn(guid);
      }
      else
      {
        setSelectedCreatureSpawn(std::optional<std::uint32_t>());
      }

      break;
    }

    if (terrainMode == editing_mode::gameobject)
    {
      auto drag_end_pos = event->pos();

      if (_area_selection->isVisible())
      {
        if (_drag_start_pos != drag_end_pos && !ImGuizmo::IsUsing())
        {
          selectGameObjectSpawnsInArea(QRect(_drag_start_pos, drag_end_pos), true);
        }
        else if (auto guid = findGameObjectSpawnAtCursor())
        {
          addGameObjectSpawnToSelection(*guid);
        }

        _area_selection->hide();
        break;
      }

      if (auto guid = findGameObjectSpawnAtCursor())
      {
        setSelectedGameObjectSpawn(guid);
      }
      else
      {
        setSelectedGameObjectSpawn(std::optional<std::uint32_t>());
      }

      break;
    }

    if ((terrainMode == editing_mode::object || terrainMode == editing_mode::minimap) && !_mod_ctrl_down)
    {
        auto drag_end_pos = event->pos();

        if (_drag_start_pos != drag_end_pos && !ImGuizmo::IsUsing())
        {
            const std::array<glm::vec2, 2> selection_box
            {
                glm::vec2(std::min(_drag_start_pos.x(), drag_end_pos.x()), std::min(_drag_start_pos.y(), drag_end_pos.y())),
                glm::vec2(std::max(_drag_start_pos.x(), drag_end_pos.x()), std::max(_drag_start_pos.y(), drag_end_pos.y()))
            };
            // _world->select_objects_in_area(selection_box, !_mod_shift_down, model_view(), projection(), width(), height(), objectEditor->drag_selection_depth(), _camera.position);
            _world->select_objects_in_area(selection_box, !_mod_shift_down, model_view(), projection(), width(), height(), 3000.0f, _camera.position);
        }
        else // Do normal selection when we just clicked
        {
            doSelection(false);
        }
        
        _area_selection->hide();
    }
    else 
    {
        doSelection(true);
    }

    break;

  case Qt::RightButton:
    rightMouse = false;

    look = false;

    if (_display_mode == display_mode::in_2D)
      updown = 0;

    // // may need to be done in constructor of widget
    // this->setContextMenuPolicy(Qt::CustomContextMenu); 
    // connect(this, SIGNAL(customContextMenuRequested(const QPoint&)),
    //     this, SLOT(ShowContextMenu(const QPoint&)));



    break;

  case Qt::MiddleButton:
    MoveObj = false;
    break;

  default:
    break;
  }
}

void MapView::save(save_mode mode)
{
  bool save = true;

  // Save minimap creator model filters
  minimapTool->saveFiltersToJSON();

  if (AsyncLoader::instance().important_object_failed_loading())
  {
    save = false;
    QPushButton *yes, *no;

    QMessageBox first_warning;
    first_warning.setIcon(QMessageBox::Critical);
    first_warning.setWindowIcon(QIcon (":/icon"));
    first_warning.setWindowTitle("Some models couldn't be loaded");
    first_warning.setText("Error:\nSome models could not be loaded and saving will cause collision and culling issues, would you still like to save ?");
    // roles are swapped to force the user to pay attention and both are "accept" roles so that escape does nothing
    no = first_warning.addButton("No", QMessageBox::ButtonRole::AcceptRole);
    yes = first_warning.addButton("Yes", QMessageBox::ButtonRole::YesRole);
    first_warning.setDefaultButton(no);

    first_warning.exec();

    if (first_warning.clickedButton() == yes)
    {
      QMessageBox second_warning;
      second_warning.setIcon(QMessageBox::Warning);
      second_warning.setWindowIcon(QIcon (":/icon"));
      second_warning.setWindowTitle("Are you sure ?");
      second_warning.setText( "If you save you will have to save again all the adt containing the defective/missing models once you've fixed said models to correct all the issues.\n"
                              "By clicking yes you accept to bear all the consequences of your action and forfeit the right to complain to the developers about any culling and collision issues.\n\n"
                              "So... do you REALLY want to save ?"
                            );
      no = second_warning.addButton("No", QMessageBox::ButtonRole::YesRole);
      yes = second_warning.addButton("Yes", QMessageBox::ButtonRole::AcceptRole);
      second_warning.setDefaultButton(no);

      second_warning.exec();

      if (second_warning.clickedButton() == yes)
      {
        save = true;
      }
    }
  }

  if ( mode == save_mode::current 
    && save 
    && (QMessageBox::warning
          (nullptr
          , "Save current map tile only"
          , "This can cause a collision bug when placing objects between two ADT borders!\n\n"
            "We recommend you to use the normal save function rather than "
            "this one to get the collisions right."
          , QMessageBox::Save | QMessageBox::Cancel
          , QMessageBox::Cancel
          ) == QMessageBox::Cancel
       )
     )
  {
    save = false;
  }

  if (save)
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _ (::gl, context());

    switch (mode)
    {
    case save_mode::current: _world->mapIndex.saveTile(TileIndex(_camera.position), _world.get()); break;
    case save_mode::changed: _world->mapIndex.saveChanged(_world.get()); break;
    case save_mode::all:     _world->mapIndex.saveall(_world.get()); break;
    }
    // write wdl, we update wdl data prior in the mapIndex saving fucntions above
    _world->horizon.save_wdl(_world.get());


    NOGGIT_ACTION_MGR->purge();
    AsyncLoader::instance().reset_object_fail();


    _main_window->statusBar()->showMessage("Map saved", 2000);

  }
  else
  {
    QMessageBox::warning
      ( nullptr
      , "Map NOT saved"
      , "The map was NOT saved, don't forget to save before leaving"
      , QMessageBox::Ok
      );
  }
}

void MapView::addHotkey(Qt::Key key, size_t modifiers, std::function<void()> function, std::function<bool()> condition)
{
  hotkeys.emplace_front (key, modifiers, function, condition);
}

void MapView::randomizeTerrainRotation()
{
  auto image_mask_selector = terrainTool->getImageMaskSelector();
  if (!image_mask_selector->getRandomizeRotation())
    return;

  unsigned int ms = static_cast<unsigned>(QDateTime::currentMSecsSinceEpoch());
  std::mt19937 gen(ms);
  std::uniform_int_distribution<> uid(0, 360);

  image_mask_selector->setRotation(uid(gen));
}

void MapView::randomizeTexturingRotation()
{
  auto image_mask_selector = texturingTool->getImageMaskSelector();
  if (!image_mask_selector->getRandomizeRotation())
    return;

  unsigned int ms = static_cast<unsigned>(QDateTime::currentMSecsSinceEpoch());
  std::mt19937 gen(ms);
  std::uniform_int_distribution<> uid(0, 360);

  image_mask_selector->setRotation(uid(gen));
}

void MapView::randomizeShaderRotation()
{
  auto image_mask_selector = shaderTool->getImageMaskSelector();
  if (!image_mask_selector->getRandomizeRotation())
    return;

  unsigned int ms = static_cast<unsigned>(QDateTime::currentMSecsSinceEpoch());
  std::mt19937 gen(ms);
  std::uniform_int_distribution<> uid(0, 360);

  image_mask_selector->setRotation(uid(gen));
}

void MapView::randomizeStampRotation()
{
  if (!stampTool->getRandomizeRotation())
    return;

  unsigned int ms = static_cast<unsigned>(QDateTime::currentMSecsSinceEpoch());
  std::mt19937 gen(ms);
  std::uniform_int_distribution<> uid(0, 360);

  stampTool->changeRotation(uid(gen));
}

void MapView::unloadOpenglData()
{
  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());

  ModelManager::unload_all(_context);
  WMOManager::unload_all(_context);
  TextureManager::unload_all(_context);

  for (MapTile* tile : _world->mapIndex.loaded_tiles())
  {
    tile->renderer()->unload();
    tile->Water.renderer()->unload();

    for (int i = 0; i < 16; ++i)
    {
      for (int j = 0; j < 16; ++j)
      {
        tile->getChunk(i, j)->unload();
      }
    }
  }

  _world->renderer()->unload();

  _buffers.unload();
  _gl_initialized = false;
}

QWidget* MapView::getSecondaryToolBar()
{
    return _viewport_overlay_ui->secondaryToolbarHolder;
}

QWidget* MapView::getLeftSecondaryToolbar()
{
    return _viewport_overlay_ui->leftSecondaryToolbarHolder;
}

QWidget* MapView::getActiveStampModeItem()
{
  auto item = stampTool->getActiveBrushItem();
  if (item)
    return item->getTool();
  else
    return nullptr;
}

void MapView::onSettingsSave()
{
  OpenGL::TerrainParamsUniformBlock* params = _world->renderer()->getTerrainParamsUniformBlock();
  params->wireframe_type = _settings->value("wireframe/type", 0).toInt();
  params->wireframe_radius = _settings->value("wireframe/radius", 1.5f).toFloat();
  params->wireframe_width = _settings->value ("wireframe/width", 1.f).toFloat();

  /* temporaryyyyyy */
  params->climb_value = 1.0f;

  QColor c = _settings->value("wireframe/color").value<QColor>();
  glm::vec4 wireframe_color(c.redF(), c.greenF(), c.blueF(), c.alphaF());
  params->wireframe_color = wireframe_color;

  _world->renderer()->markTerrainParamsUniformBlockDirty();
}

void MapView::ShowContextMenu(QPoint pos) 
{
    // QApplication::startDragDistance() is 10
    auto mouse_moved = QApplication::startDragDistance() < (_right_click_pos - pos).manhattanLength();;

    // don't show context menu if dragging mouse
    if (mouse_moved || ImGuizmo::IsUsing())
        return;

    // TODO : build the menu only once, store it and instead use setVisible ?

    QMenu* menu = new QMenu(this);

    // Undo
    QAction action_undo("Undo", this);
    menu->addAction(&action_undo);
    action_undo.setShortcut(QKeySequence::Undo);
    QObject::connect(&action_undo, &QAction::triggered, [=]()
        {
            if (terrainMode == editing_mode::creature && undoLastCreatureDelete()) { return; }
            if (terrainMode == editing_mode::gameobject && undoLastGameObjectDelete()) { return; }
            NOGGIT_ACTION_MGR->undo();
        });
    // Redo
    QAction action_redo("Redo", this);
    menu->addAction(&action_redo);
    action_redo.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z));
    QObject::connect(&action_redo, &QAction::triggered, [=]()
        {
            NOGGIT_ACTION_MGR->redo();
        });

    menu->addSeparator();

    if (terrainMode == editing_mode::object)
    {
        bool has_selected_objects = _world->get_selected_model_count();
        bool has_copied_objects = objectEditor->clipboardSize();

        // Copy
        QAction action_8("Copy Object(s)", this);
        menu->addAction(&action_8);
        action_8.setEnabled(has_selected_objects);
        action_8.setShortcut(QKeySequence::Copy);
        QObject::connect(&action_8, &QAction::triggered, [=]()
            {
                if (terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION)
                    objectEditor->copy_current_selection(_world.get());
            });

        // Paste
        QAction action_9("Paste Object(s)", this);
        menu->addAction(&action_9);
        action_9.setEnabled(has_copied_objects);
        action_9.setShortcut(QKeySequence::Paste); // (Qt::CTRL | Qt::Key_P)
        QObject::connect(&action_9, &QAction::triggered, [=]()
            {
                if (terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION)
                {
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_ADDED);
                    objectEditor->pasteObject(_cursor_pos, _camera.position, _world.get(), &_object_paste_params);
                    NOGGIT_ACTION_MGR->endAction();
                }
            });

        // Delete
        QAction action_10("Delete Object(s)", this);
        menu->addAction(&action_10);
        action_10.setEnabled(has_selected_objects);
        action_10.setShortcut(QKeySequence::Delete); // (Qt::CTRL | Qt::Key_P)
        QObject::connect(&action_10, &QAction::triggered, [=]()
            {
                if (terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION)
                {
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_REMOVED);
                    DeleteSelectedObjects();
                    NOGGIT_ACTION_MGR->endAction();
                }
            });

        // Duplicate
        QAction action_11("Duplicate Object(s)", this);
        menu->addAction(&action_11);
        action_11.setEnabled(has_copied_objects);
        action_11.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B)); // (Qt::CTRL | Qt::Key_P)
        QObject::connect(&action_11, &QAction::triggered, [=]()
            {
                if (terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION)
                {
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_ADDED);
                    objectEditor->copy_current_selection(_world.get());
                    objectEditor->pasteObject(_cursor_pos, _camera.position, _world.get(), &_object_paste_params);
                    NOGGIT_ACTION_MGR->endAction();
                }
            });

        menu->addSeparator();

        // selection stuff
        QAction action_1("Select all Like Selected", this); // select all objects with the same model
        action_1.setToolTip("Warning : Doing actions on models overlapping unloaded tiles can cause crash");
        menu->addAction(&action_1);
        action_1.setEnabled(_world->get_selected_model_count() == 1);
        QObject::connect(&action_1, &QAction::triggered, [=]()
            {
                auto last_entry = _world->get_last_selected_model();
                if (last_entry)
                {
                    if (!last_entry.value().index() == eEntry_Object)
                        return;

                    auto obj = std::get<selected_object_type>(last_entry.value());
                    auto model_name = obj->instance_model()->file_key().filepath();
                    // auto models = _world->get_models_by_filename()[model_name];

                    _world->reset_selection();

                    if (obj->which() == eMODEL)
                    {
                        _world->getModelInstanceStorage().for_each_m2_instance([&](ModelInstance& model_instance)
                            {
                                if (model_instance.instance_model()->file_key().filepath() == model_name)
                                {
                                    // objects_to_select.push_back(model_instance.uid);
                                    _world->add_to_selection(&model_instance);
                                }
                            });
                    }
                    else if (obj->which() == eWMO)
                        _world->getModelInstanceStorage().for_each_wmo_instance([&](WMOInstance& wmo_instance)
                            {
                                if (wmo_instance.instance_model()->file_key().filepath() == model_name)
                                {
                                    // objects_to_select.push_back(wmo_instance.uid);
                                    _world->add_to_selection(&wmo_instance);
                                }
                            });

                    // for (auto uid_it = objects_to_select.begin(); uid_it != objects_to_select.end(); uid_it++)
                    // {
                    //     auto instance = _world->getObjectInstance(*uid_it);
                    //     // if (!_world->is_selected(instance))
                    //         _world->add_to_selection(instance);
                    // }
                }
            });

        QAction action_2("Hide Selected Objects", this);
        menu->addAction(&action_2);
        action_2.setEnabled(has_selected_objects);
        action_2.setShortcut(Qt::Key_H);
        QObject::connect(&action_2, &QAction::triggered, [=]()
            {
                if (_world->has_selection())
                {
                    for (auto& obj : _world->get_selected_objects())
                    {
                        if (obj->which() == eMODEL)
                            static_cast<ModelInstance*>(obj)->model->hide();
                        else if (obj->which() == eWMO)
                            static_cast<WMOInstance*>(obj)->wmo->hide();
                    }
                }
            });

        QAction action_3("Hide Unselected Objects", this);


        // QAction action_2("Show Hidden", this);

        QAction action_palette_add("Add Object To Palette", this);
        menu->addAction(&action_palette_add);
        action_palette_add.setEnabled(_world->get_selected_model_count() == 1);
        QObject::connect(&action_palette_add, &QAction::triggered, [=]()
            {
                auto last_entry = _world->get_last_selected_model();
                if (last_entry)
                {
                    if (!last_entry.value().index() == eEntry_Object)
                        return;

                    getObjectPalette()->setVisible(true);
                    auto obj = std::get<selected_object_type>(last_entry.value());
                    auto model_name = obj->instance_model()->file_key().filepath();
                    _object_palette->addObjectByFilename(model_name.c_str());
                }

            });

        menu->addSeparator();

        // allow replacing all selected?
        QAction action_replace("Replace Models (By Clipboard)", this);
        menu->addAction(&action_replace);
        action_replace.setEnabled(has_selected_objects && objectEditor->clipboardSize() == 1);
        action_replace.setToolTip("Replace the currently selected objects by the object in the clipboard (There must only be one!). M2s can only be replaced by m2s");
        QObject::connect(&action_replace, &QAction::triggered, [=]()
            {
                if (terrainMode != editing_mode::object && NOGGIT_CUR_ACTION)
                    return;

                // verify this
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_ADDED | Noggit::ActionFlags::eOBJECTS_REMOVED); // Noggit::ActionFlags::eOBJECTS_TRANSFORMED
                // NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);

                // get the model to replace by
                auto replace_select = objectEditor->getClipboard().front();
                auto replace_obj = std::get<selected_object_type>(replace_select);
                // bool replace_is_wmo = replace_obj->which() == eWMO;
                auto replace_path = replace_obj->instance_model()->file_key();

                // iterate selection (objects to replace)
                for (auto& source_obj : _world->get_selected_objects())
                {

                        math::degrees::vec3 source_rot(math::degrees(0)._, math::degrees(0)._, math::degrees(0)._);
                        source_rot = source_obj->dir;
                        float source_scale = source_obj->scale;
                        auto source_pos = source_obj->pos;

                        if (source_obj->instance_model()->file_key().filepath() == replace_path)
                            continue;

                        // TODO : Test if this breaks if clipboard is empty

                        if (replace_obj->which() == eWMO)
                        {
                            // if (!replace_is_wmo)
                            //     continue;

                            // auto replace_wmo = static_cast<WMOInstance*>(replace_obj);
                            // auto source_wmo = static_cast<WMOInstance*>(source_obj);

                            auto new_obj = _world->addWMOAndGetInstance(replace_path, source_pos, source_rot);
                            new_obj->wmo->wait_until_loaded();
                            new_obj->wmo->waitForChildrenLoaded();
                            new_obj->recalcExtents();

                        }
                        else if (replace_obj->which() == eMODEL)
                        {
                            // if (replace_is_wmo)
                            //     continue;

                            // auto replace_m2 = static_cast<ModelInstance*>(replace_obj);
                            // auto source_m2 = static_cast<ModelInstance*>(source_obj);

                            // Just swapping model
                            // Issue : doesn't work with actions
                            // _world->updateTilesEntry(entry, model_update::remove);
                            // source_m2->model = scoped_model_reference(replace_path, _context);
                            // source_m2->recalcExtents();
                            // _world->updateTilesEntry(entry, model_update::add);
                            

                            auto new_obj = _world->addM2AndGetInstance(replace_path
                                , source_pos
                                , source_scale
                                , source_rot
                                , &_object_paste_params
                                , true
                            );
                            new_obj->model->wait_until_loaded();
                            new_obj->model->waitForChildrenLoaded();
                            new_obj->recalcExtents();
                        }
                }
                // can cause the usual crash of deleting models overlapping unloaded tiles.
                DeleteSelectedObjects();
                // NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_REMOVED);
                NOGGIT_ACTION_MGR->endAction();
            });

        QAction action_snap("Snap Selected To Ground", this);
        menu->addAction(&action_snap);
        action_snap.setEnabled(has_selected_objects);
        action_snap.setShortcut(Qt::Key_PageDown); // (Qt::CTRL | Qt::Key_P)
        QObject::connect(&action_snap, &QAction::triggered, [=]()
            {
                if (terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION)
                {
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
                    snap_selected_models_to_the_ground();
                    NOGGIT_ACTION_MGR->endAction();
                }
            });

        QAction action_save_obj_coords("Save objects coords(to file)", this);
        menu->addAction(&action_save_obj_coords);
        action_save_obj_coords.setEnabled(has_selected_objects);
        QObject::connect(&action_save_obj_coords, &QAction::triggered, [=]()
            {
                if (terrainMode == editing_mode::object)
                {
                    if (_world->has_selection() && _world->get_selected_model_count())
                    {
                        std::stringstream obj_data;
                        for (auto& obj : _world->get_selected_objects())
                        {
                            obj_data << "\"Object : " << obj->instance_model()->file_key().filepath() << "(UID :" << obj->uid << ")\"," << std::endl;
                            obj_data << "\"Scale : " << obj->scale << "\"," << std::endl;
                            // coords string in ts-wow format
                            obj_data << "\"Coords(server): {map:" << _world->getMapID() << ",x:" << (ZEROPOINT - obj->pos.z) << ",y:" << (ZEROPOINT - obj->pos.x)
                                << ",z:" << obj->pos.y << ",o:";

                            float server_rot = 2 * glm::pi<float>() - glm::pi<float>() / 180.0 * (float(obj->dir.y) < 0 ? fabs(float(obj->dir.y)) + 180.0 : fabs(float(obj->dir.y) - 180.0));
                            // float server_rot = glm::radians(obj->dir.y) + glm::radians(180.f);

                            obj_data << server_rot << "}\"," << std::endl;

                            /// converting db gobject rotation to noggit. Keep commented for later usage
                            /*
                            glm::quat test_db_quat = glm::quat(1.0, 1.0, 1.0, 1.0);
                            test_db_quat.x = 0.607692, test_db_quat.y = -0.361538, test_db_quat.z = 0.607693, test_db_quat.w = 0.361539;
                            glm::vec3 rot_euler = glm::eulerAngles(test_db_quat);
                            glm::vec3 rot_degrees = glm::degrees(rot_euler); 
                            rot_degrees = glm::vec3(rot_degrees.y, rot_degrees.z - 180.f, rot_degrees.x); // final noggit coords
                            */

                            glm::quat rot_quat = glm::quat(glm::vec3(glm::radians(obj->dir.z), glm::radians(obj->dir.x), server_rot));
                            auto normalized_quat = glm::normalize(rot_quat);

                            obj_data << "\"Rotation (server quaternion): {x:" << normalized_quat.x << ",y:" << normalized_quat.y << ",z:" << normalized_quat.z
                                << ",w:" << normalized_quat.w << "}\"," <<  std::endl << "\n";
                        }

                        std::ofstream f("saved_objects_data.txt", std::ios_base::app);
                        f << "\"Saved " << _world->get_selected_model_count() << " objects at : " << QDateTime::currentDateTime().toString("dd MMMM yyyy hh:mm:ss").toStdString() << "\"" << std::endl;
                        f << obj_data.str();
                        f.close();
                    }
                }
            });

        menu->addSeparator();
        // TODO
        QAction action_group("Group Selected Objects", this);
        menu->addAction(&action_group);
        // check if all selected objects are already grouped
        bool groupable = false; 
        if ( _world->has_multiple_model_selected())
        {
            // if there's no existing groups, that means it's always groupable
            if (!_world->_selection_groups.size())
                groupable = true;

            if (!groupable)
            {
                // check if there's any ungrouped object
                for (auto obj : _world->get_selected_objects())
                {
                    bool obj_ungrouped = true;
                    for (auto& group : _world->_selection_groups)
                    {
                        if (group.contains_object(obj))
                            obj_ungrouped = false;
                    }
                    if (obj_ungrouped)
                    {
                        groupable = true;
                        break;
                    }
                }
            }
        }
        action_group.setEnabled(groupable);
        QObject::connect(&action_group, &QAction::triggered, [=]()
            {
                // remove all groups the objects are already in and create a new one
                // for (auto obj : _world->get_selected_objects())
                // {
                //     for (auto& group : _world->_selection_groups)
                //     {
                //         if (group.contains_object(obj))
                //         {
                //             group.remove_group();
                //         }
                //     }
                // }
                for (auto& group : _world->_selection_groups)
                {
                    if (group.isSelected())
                    {
                        group.remove_group();
                    }
                }

                _world->add_object_group_from_selection();
            });


        QAction action_ungroup("Ungroup Selected Objects", this);
        menu->addAction(&action_ungroup);
        bool group_selected = false;
        for (auto& group : _world->_selection_groups)
        {
            if (group.isSelected())
            {
                group_selected = true;
                break;
            }
        }
        action_ungroup.setEnabled(group_selected);
        QObject::connect(&action_ungroup, &QAction::triggered, [=]()
            {
                _world->clear_selection_groups();
            });


        menu->exec(mapToGlobal(pos)); // synch
        // menu->popup(mapToGlobal(pos)); // asynch, needs to be preloaded to work
    };

}
