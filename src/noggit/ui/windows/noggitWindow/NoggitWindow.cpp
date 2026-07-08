#include <noggit/ui/windows/about/About.h>
#include <noggit/MySqlSettings.hpp>
#include <noggit/AsyncLoader.h>
#include <noggit/DBC.h>
#include <noggit/DBCFile.h>
#include <noggit/Log.h>
#include <noggit/World.h>
#include <noggit/ContextObject.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/MapView.h>
#include <noggit/ui/windows/settingsPanel/SettingsPanel.h>
#include <noggit/ui/minimap_widget.hpp>
#include <noggit/ui/UidFixWindow.hpp>
#include <noggit/uid_storage.hpp>
#include <noggit/ui/tools/MapCreationWizard/Ui/MapCreationWizard.hpp>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/FramelessWindow.hpp>
#include <noggit/ui/tools/UiCommon/StackedWidget.hpp>
#include <BlizzardDatabase.h>
#include <QtGui/QCloseEvent>
#include <QtGui/QImage>
#include <QtGui/QScreen>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QComboBox>
#include <noggit/ui/windows/noggitWindow/widgets/MapListItem.hpp>
#include <noggit/ui/windows/noggitWindow/widgets/MapBookmarkListItem.hpp>
#include <QtNetwork/QTcpSocket>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <QSysInfo>
#include <QStandardPaths>
#include <QDir>
#include <QIcon>
#include <QThread>
#include <QtCore/QSettings>
#include <noggit/ui/windows/noggitWindow/components/BuildMapListComponent.hpp>
#include <noggit/application/Utils.hpp>

#ifdef USE_MYSQL_UID_STORAGE
#include <mysql/mysql.h>

#include <QtCore/QSettings>
#endif

#include "revision.h"

#include "ui_TitleBar.h"
#include <external/framelesshelper/framelesswindowsmanager.h>
#include <noggit/ui/tools/ViewportManager/ViewportManager.hpp>

namespace Noggit::Ui::Windows
{
  namespace
  {
    bool imageLooksBlank(QImage const& image, bool includes_window_chrome = false)
    {
      if (image.isNull())
      {
        return true;
      }

      QRect sample_rect(0, 0, image.width(), image.height());
      if (includes_window_chrome)
      {
        int const left = std::min(96, image.width() / 4);
        int const top = std::min(72, image.height() / 4);
        sample_rect = QRect(left, top, image.width() - left, image.height() - top);
      }

      int const step_x = std::max(1, sample_rect.width() / 64);
      int const step_y = std::max(1, sample_rect.height() / 64);
      int non_dark = 0;
      int sampled = 0;

      for (int y = sample_rect.top(); y < sample_rect.bottom(); y += step_y)
      {
        for (int x = sample_rect.left(); x < sample_rect.right(); x += step_x)
        {
          QColor const color(image.pixel(x, y));
          if (color.red() > 8 || color.green() > 8 || color.blue() > 8)
          {
            ++non_dark;
          }
          ++sampled;
        }
      }

      return sampled == 0 || non_dark < sampled / 100;
    }

    bool captureLightingTraceEnabled()
    {
      if (char const* value = std::getenv("NOGGIT_CAPTURE_LIGHTING_TRACE"))
      {
        return std::string(value) != "0";
      }

      return false;
    }
  }

  void NoggitWindow::ensureSettingsWindow()
  {
    if (_settings)
      return;

    LogDebug << "NoggitWindow::ensureSettingsWindow begin" << std::endl;
    _settings = new settings(this);
    connect(_settings, &settings::saved, [this]()
    {
      if (_map_view)
        _map_view->onSettingsSave();
    });
    LogDebug << "NoggitWindow::ensureSettingsWindow end" << std::endl;
  }

  void NoggitWindow::ensureAboutWindow()
  {
    if (_about)
      return;

    LogDebug << "NoggitWindow::ensureAboutWindow begin" << std::endl;
    _about = new about(this);
    LogDebug << "NoggitWindow::ensureAboutWindow end" << std::endl;
  }

  void NoggitWindow::ensureMapCreationWizard()
  {
    if (_map_creation_wizard)
      return;

    LogDebug << "NoggitWindow::ensureMapCreationWizard begin" << std::endl;

    _map_creation_wizard = new Noggit::Ui::Tools::MapCreationWizard::Ui::MapCreationWizard(_project, _map_creation_wizard_host);

    auto host_layout = qobject_cast<QVBoxLayout*>(_map_creation_wizard_host->layout());
    if (!host_layout)
    {
      host_layout = new QVBoxLayout(_map_creation_wizard_host);
      host_layout->setContentsMargins(0, 0, 0, 0);
    }

    host_layout->addWidget(_map_creation_wizard);

    _map_wizard_connection = connect(_map_creation_wizard,
                                     &Noggit::Ui::Tools::MapCreationWizard::Ui::MapCreationWizard::map_dbc_updated, [=]
                                     {
                                       _buildMapListComponent->buildMapList(this);
                                     }
    );

    LogDebug << "NoggitWindow::ensureMapCreationWizard end" << std::endl;
  }

  NoggitWindow::NoggitWindow(std::shared_ptr<Noggit::Application::NoggitApplicationConfiguration> application,
                             std::shared_ptr<Noggit::Project::NoggitProject> project)
      : QMainWindow(nullptr)
      , _null_widget(new QWidget(this))
      , _applicationConfiguration(application)
      , _project(project)
      , _minimap(nullptr)
      , _settings(nullptr)
      , _about(nullptr)
      , _map_view(nullptr)
      , _stack_widget(nullptr)
      , _map_creation_wizard(nullptr)
      , _continents_table(nullptr)
      , _right_side(nullptr)
  {
    LogDebug << "NoggitWindow ctor begin" << std::endl;

    std::stringstream title;
    title << "Noggit - " << STRPRODUCTVER;
    setWindowTitle(QString::fromStdString(title.str()));
    setWindowIcon(QIcon(":/icon"));

    if (project->projectVersion == Project::ProjectVersion::CLASSIC
        || project->projectVersion == Project::ProjectVersion::WOTLK)
    {
      OpenDBs(project->ClientData);
    }
    LogDebug << "NoggitWindow ctor after OpenDBs" << std::endl;

    setCentralWidget(_null_widget);

    // The default value is AnimatedDocks | AllowTabbedDocks.
    setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks | GroupedDragging);

    _menuBar = menuBar();
    LogDebug << "NoggitWindow ctor after menuBar" << std::endl;

    QSettings settings;

    if (!settings.value("systemWindowFrame", true).toBool())
    {
      QWidget* widget = new QWidget(this);
      ::Ui::TitleBar* titleBarWidget = setupFramelessWindow(widget, this, minimumSize(), maximumSize(), true);
      titleBarWidget->horizontalLayout->insertWidget(2, _menuBar);
      setMenuWidget(widget);
    }

    _menuBar->setNativeMenuBar(settings.value("nativeMenubar", true).toBool());

    auto file_menu(_menuBar->addMenu("&Noggit"));

    auto settings_action(file_menu->addAction("Settings"));
    QObject::connect(settings_action, &QAction::triggered, [&]
                     {
                       ensureSettingsWindow();
                       _settings->show();
                     }
    );

    auto about_action(file_menu->addAction("About"));
    QObject::connect(about_action, &QAction::triggered, [&]
                     {
                       ensureAboutWindow();
                       _about->show();
                     }
    );

    auto mapmenu_action(file_menu->addAction("Exit"));
    QObject::connect(mapmenu_action, &QAction::triggered, [this]
                     {
                       close();
                     }
    );

    _menuBar->adjustSize();
    LogDebug << "NoggitWindow ctor after menu setup" << std::endl;

    _buildMapListComponent = std::make_unique<Component::BuildMapListComponent>();
    LogDebug << "NoggitWindow ctor before buildMenu" << std::endl;

    buildMenu();
    LogDebug << "NoggitWindow ctor after buildMenu" << std::endl;
  }

  void NoggitWindow::check_uid_then_enter_map
      (glm::vec3 pos, math::degrees camera_pitch, math::degrees camera_yaw, bool from_bookmark
      )
  {
    QSettings settings;
#ifdef USE_MYSQL_UID_STORAGE
    bool use_mysql = Noggit::mysqlSetting("enabled", false).toBool();

    bool valid_conn = false;
    if (use_mysql)
    {
        valid_conn = mysql::testConnection(true);
    }

    if ((valid_conn && mysql::hasMaxUIDStoredDB(_world->getMapID()))
      || uid_storage::hasMaxUIDStored(_world->getMapID())
       )
    {

      _world->mapIndex.loadMaxUID();
      enterMapAt(pos, camera_pitch, camera_yaw, uid_fix_mode::none, from_bookmark);
    }
#else
    if (uid_storage::hasMaxUIDStored(_world->getMapID()))
    {
      if (settings.value("uid_startup_check", true).toBool())
      {
        enterMapAt(pos, camera_pitch, camera_yaw, uid_fix_mode::max_uid, from_bookmark);
      } else
      {
        _world->mapIndex.loadMaxUID();
        enterMapAt(pos, camera_pitch, camera_yaw, uid_fix_mode::none, from_bookmark);
      }
    }
#endif
    else
    {
      auto uid_fix_window(new UidFixWindow(pos, camera_pitch, camera_yaw));
      uid_fix_window->show();

      connect(uid_fix_window, &Noggit::Ui::UidFixWindow::fix_uid, [this, from_bookmark]
                  (glm::vec3 pos, math::degrees camera_pitch, math::degrees camera_yaw, uid_fix_mode uid_fix
                  )
              {
                enterMapAt(pos, camera_pitch, camera_yaw, uid_fix, from_bookmark);
              }
      );
    }
  }

  void
  NoggitWindow::enterMapAt(glm::vec3 pos, math::degrees camera_pitch, math::degrees camera_yaw, uid_fix_mode uid_fix,
                           bool from_bookmark,
                           bool capture_probe
  )
  {
      LogDebug << "NoggitWindow::enterMapAt begin" << std::endl;
      if (_world->mapIndex.hasAGlobalWMO())
      {
          // enter at mdoel's position
          // pos = glm::vec3(_world->mWmoEntry[0], _world->mWmoEntry.pos[1], _world->mWmoEntry.pos[2]);

          // better, enter at model's max extent, facing toward min extent
          auto min_extent = glm::vec3(_world->mWmoEntry.extents[0][0], _world->mWmoEntry.extents[0][1], _world->mWmoEntry.extents[0][2]);
          auto max_extent = glm::vec3(_world->mWmoEntry.extents[1][0], _world->mWmoEntry.extents[1][1] * 2, _world->mWmoEntry.extents[1][2]);
          float dx = min_extent.x - max_extent.x;
          float dy = min_extent.z - max_extent.z; // flipping z and y works better for some reason
          float dz = min_extent.y - max_extent.y;

          pos = max_extent;

          camera_yaw = math::degrees(math::radians(std::atan2(dx, dy)));

          float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
          camera_pitch = math::degrees(math::radians(std::asin(dz / distance)));

      }


    if (_map_creation_wizard)
      _map_creation_wizard->destroyFakeWorld();
  LogDebug << "NoggitWindow::enterMapAt before MapView ctor" << std::endl;
    _map_view = (new MapView(camera_yaw, camera_pitch, pos, this, _project, std::move(_world), uid_fix, from_bookmark, capture_probe));
  LogDebug << "NoggitWindow::enterMapAt after MapView ctor" << std::endl;
    connect(_map_view, &MapView::uid_fix_failed, [this]()
    { promptUidFixFailure(); });

    _stack_widget->addWidget(_map_view);
    _stack_widget->setCurrentIndex(1);

    map_loaded = true;
    LogDebug << "NoggitWindow::enterMapAt end" << std::endl;

  }

  void NoggitWindow::applyFilterSearch(const QString &name, int type, int expansion, bool wmo_maps)
  {
      for (int i = 0; i < _continents_table->count(); ++i)
      {
          auto item_widget = _continents_table->item(i);
          auto widget = qobject_cast<Noggit::Ui::Widget::MapListItem*>(_continents_table->itemWidget(item_widget));

          if (!widget)
              continue;

          item_widget->setHidden(false);

          if (!widget->name().contains(name, Qt::CaseInsensitive))
          {
              item_widget->setHidden(true);
              continue;
          }

          if (!(widget->type() == (type - 2)) && type != 0)
          {
              item_widget->setHidden(true);
              continue;
          }

          if (!(widget->expansion() == (expansion - 1)) && expansion != 0)
          {
              item_widget->setHidden(true);
          }

            // The "Display WMO maps (No terrain)" toggle is meant to hide terrain-less *world* maps
            // (clutter / test continents). Instanced content (dungeons, raids, BGs, arenas) is usually
            // WMO-only too -- e.g. Deadmines, Blackrock Depths, Gnomeregan -- and must NOT be hidden by
            // this toggle, otherwise selecting "Dungeon" drops every WMO dungeon and switching back to
            // "All" loses them as well (the initial list shows them, but any filter pass would hide them).
            // So only gate WMO *world* maps (type 0) on the checkbox.
            if (!wmo_maps && widget->wmo_map() && widget->type() == 0)
          {
              item_widget->setHidden(true);
          }
      }
  }

  void NoggitWindow::loadMap(int map_id)
  {
    _minimap->world(nullptr);

    _world.reset();

    auto table = _project->ClientDatabase->LoadTable("Map", readFileAsIMemStream);
    auto record = table.Record(map_id);
    auto directory_itr = record.Columns.find("Directory");
    if (directory_itr != record.Columns.end() && !directory_itr->second.Value.empty())
    {
      _world = std::make_unique<World>(directory_itr->second.Value, map_id, Noggit::NoggitRenderContext::MAP_VIEW);
      _minimap->world(_world.get());
      _project->ClientDatabase->UnloadTable("Map");
      return;
    }

    _project->ClientDatabase->UnloadTable("Map");

    if (gMapDB.getRecordCount() > 0)
    {
      try
      {
        auto dbc_record = gMapDB.getByID(map_id);
        _world = std::make_unique<World>(dbc_record.getString(MapDB::InternalName), map_id, Noggit::NoggitRenderContext::MAP_VIEW);
        _minimap->world(_world.get());
      }
      catch (DBCFile::NotFound const&)
      {
      }
    }
  }

  void NoggitWindow::jumpToMapPosition(int map_id,
                                       glm::vec3 pos,
                                       math::degrees camera_pitch,
                                       math::degrees camera_yaw,
                                       bool from_bookmark)
  {
    if (!_world || _world->getMapID() != static_cast<unsigned int>(map_id))
    {
      loadMap(map_id);
    }

    check_uid_then_enter_map(pos, camera_pitch, camera_yaw, from_bookmark);
  }

  bool NoggitWindow::captureMapCreaturesToPng(int map_id,
                                              QString const& output_path,
                                              int width,
                                              int height,
                                              std::optional<glm::vec3> camera_position,
                                              math::degrees camera_yaw,
                                              math::degrees camera_pitch)
  {
    setAnimated(false);
    setDockOptions(AllowNestedDocks | AllowTabbedDocks | GroupedDragging);
    QApplication::setEffectEnabled(Qt::UI_AnimateMenu, false);
    QApplication::setEffectEnabled(Qt::UI_FadeMenu, false);
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);
    QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
    QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);
    QApplication::setEffectEnabled(Qt::UI_AnimateToolBox, false);
    resize(width, height);
    show();
    qApp->processEvents();

    loadMap(map_id);
    if (!_world)
    {
      LogError << "capture-world-creatures: failed to load mapId=" << map_id << std::endl;
      return false;
    }

    LogDebug << "capture-world-creatures: enter map begin" << std::endl;
    enterMapAt(camera_position.value_or(glm::vec3(0.f, 50.f, 0.f)),
               camera_position ? camera_pitch : math::degrees(25.f),
               camera_position ? camera_yaw : math::degrees(0.f),
               uid_fix_mode::none,
               camera_position.has_value(),
               true);
    LogDebug << "capture-world-creatures: enter map done" << std::endl;
    if (!_map_view)
    {
      LogError << "capture-world-creatures: failed to create map view mapId=" << map_id << std::endl;
      return false;
    }

    bool const capture_creatures = []()
    {
      char const* value = std::getenv("NOGGIT_CAPTURE_CREATURES");
      return !value || !*value || std::strcmp(value, "0") != 0;
    }();

    bool loaded = !capture_creatures;
    if (capture_creatures)
    {
      _map_view->_draw_creature_spawns.set(true);
      _map_view->getWorld()->setDrawCreatureSpawns(true);

      loaded = !_map_view->getWorld()->creatureSpawns().empty();
      if (loaded)
      {
        LogDebug << "capture-world-creatures: reusing loaded creature spawns count="
                 << _map_view->getWorld()->creatureSpawns().size() << std::endl;
      }
      else
      {
        LogDebug << "capture-world-creatures: reload creature spawns begin" << std::endl;
        loaded = _map_view->getWorld()->reloadCreatureSpawns();
        LogDebug << "capture-world-creatures: reload creature spawns done loaded=" << loaded << std::endl;
      }
    }
    else
    {
      _map_view->_draw_creature_spawns.set(false);
      _map_view->getWorld()->setDrawCreatureSpawns(false);
      LogDebug << "capture-world-creatures: creature overlay disabled by NOGGIT_CAPTURE_CREATURES=0" << std::endl;
    }
    LogDebug << "capture-world-creatures: wait async begin" << std::endl;
    AsyncLoader::instance().wait_until_idle();
    LogDebug << "capture-world-creatures: wait async done" << std::endl;

    if (!loaded)
    {
      LogError << "capture-world-creatures: failed to load creature spawns mapId=" << map_id
               << " status='" << _map_view->getWorld()->creatureSpawnStatus() << "'" << std::endl;
      return false;
    }

    auto const& spawns = _map_view->getWorld()->creatureSpawns();
    if (camera_position)
    {
      _map_view->setCameraForCapture(*camera_position, camera_yaw, camera_pitch);
      LogDebug << "capture-world-creatures camera explicit position=("
               << camera_position->x << ", " << camera_position->y << ", " << camera_position->z
               << ") yaw=" << camera_yaw._ << " pitch=" << camera_pitch._ << std::endl;
    }
    else if (!spawns.empty())
    {
      auto const target = spawns.front().pos;
      _map_view->setCameraForCapture(target + glm::vec3(0.f, 12.f, -35.f),
                                     math::degrees(0.f),
                                     math::degrees(18.f));
      LogDebug << "capture-world-creatures camera firstSpawn guid=" << spawns.front().guid
               << " target=(" << target.x << ", " << target.y << ", " << target.z << ")"
               << " camera=(" << target.x << ", " << (target.y + 12.f) << ", " << (target.z - 35.f) << ")"
               << std::endl;
    }

    if (char const* value = std::getenv("NOGGIT_CAPTURE_DRAW_FOG"))
    {
      _map_view->_draw_fog.set(std::string(value) != "0");
    }

    if (camera_position)
    {
      unsigned int const wmo_area_id = _map_view->getWorld()->getWMOAreaID(*camera_position);
      unsigned int const terrain_area_id = _map_view->getWorld()->getAreaID(*camera_position);
      unsigned int const area_id = wmo_area_id != static_cast<unsigned int>(-1) ? wmo_area_id : terrain_area_id;
      int area_light_id = 0;

      if (area_id != static_cast<unsigned int>(-1)
          && gAreaDB.getFieldCount() > AreaDB::LightId
          && gAreaDB.CheckIfIdExists(area_id))
      {
        area_light_id = gAreaDB.getByID(area_id).getInt(AreaDB::LightId);
      }

      std::ofstream trace("I:\\Twow-local\\server_dev\\noggit_captures\\lighting_trace.txt", std::ios::app);
      trace << "capture-pre-render"
            << " trace_env=" << (captureLightingTraceEnabled() ? 1 : 0)
            << " pos=(" << camera_position->x << "," << camera_position->y << "," << camera_position->z << ")"
            << " draw_fog=" << (_map_view->_draw_fog.get() ? 1 : 0)
            << " wmo_area=" << wmo_area_id
            << " terrain_area=" << terrain_area_id
            << " final_area=" << area_id
            << " area_light=" << area_light_id
            << '\n';
    }

    LogDebug << "capture-world-creatures: render frame begin" << std::endl;
    bool const capture_debug = []()
    {
      if (char const* value = std::getenv("NOGGIT_CAPTURE_DEBUG"))
      {
        return std::string(value) != "0";
      }
      return false;
    }();
    if (capture_debug)
    {
      LogDebug << "capture-world-creatures direct framebuffer render" << std::endl;
    }
    _map_view->setCameraDirty();
    for (int i = 0; i < 40; ++i)
    {
      _map_view->getWorld()->animtime += 100.0f;
      _map_view->getWorld()->update_models_emitters(0.1f);
      qApp->processEvents();
    }
    _map_view->setCameraDirty();
    LogDebug << "capture-world-creatures: render frame done" << std::endl;

    QImage image = _map_view->grabRenderedFrameForCapture();
    char const* capture_source = "mapview-readpixels";

    if (imageLooksBlank(image))
    {
      if (auto* screen = _map_view->screen())
      {
        image = screen->grabWindow(_map_view->winId()).toImage();
        capture_source = "screen-map-view";
      }
    }

    if (image.isNull())
    {
      LogError << "capture-world-creatures: framebuffer capture was empty mapId=" << map_id << std::endl;
      return false;
    }

    if (imageLooksBlank(image))
    {
      LogError << "capture-world-creatures: captured image is blank mapId=" << map_id
               << " source=" << capture_source << std::endl;
      return false;
    }

    bool const saved = image.save(output_path);
    LogDebug << "capture-world-creatures result mapId=" << map_id
             << " saved=" << saved
             << " source=" << capture_source
             << " path='" << output_path.toStdString() << "'"
             << " size=" << image.width() << "x" << image.height()
             << " status='" << _map_view->getWorld()->creatureSpawnStatus() << "'"
             << " importantObjectFailed=" << AsyncLoader::instance().important_object_failed_loading()
             << std::endl;

    return saved && !AsyncLoader::instance().important_object_failed_loading();
  }

  void NoggitWindow::buildMenu()
  {
    LogDebug << "NoggitWindow::buildMenu begin" << std::endl;
    _stack_widget = new StackedWidget(this);
    _stack_widget->setAutoResize(true);

    setCentralWidget(_stack_widget);

    auto widget(new QWidget(_stack_widget));
    _stack_widget->addWidget(widget);

    auto layout(new QHBoxLayout(widget));
    layout->setAlignment(Qt::AlignLeft);
    QListWidget* bookmarks_table(new QListWidget(widget));
    _continents_table = new QListWidget(widget);
    QObject::connect(_continents_table, &QListWidget::itemClicked, [this](QListWidgetItem* item)
                     {
                       loadMap(item->data(Qt::UserRole).toInt());
                     }
    );


    QTabWidget* entry_points_tabs(new QTabWidget(widget));
    //entry_points_tabs->addTab(_continents_table, "Maps");

     auto add_btn = new QPushButton("Add New Map", this);
     add_btn->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::plus));
     add_btn->setAccessibleName("map_wizard_add_button");

    /* set-up widget for seaching etc... through _continents_table */
    {
        QWidget* _first_tab = new QWidget(this);
        QVBoxLayout* _first_tab_layout = new QVBoxLayout();
        _first_tab->setLayout(_first_tab_layout);

        QGroupBox* _group_search = new QGroupBox(tr("Search"), this);

        QLineEdit* _line_edit_search = new QLineEdit(this);
        QComboBox* _combo_search = new QComboBox(this);
        _combo_search->addItems(QStringList() <<
                                tr("All") <<
                                tr("Unknown") <<
                                tr("Continent") <<
                                tr("Dungeon") <<
                                tr("Raid") <<
                                tr("Battleground") <<
                                tr("Arena") <<
                                tr("Scenario"));
        _combo_search->setCurrentIndex(0);

        QComboBox* _combo_exp_search = new QComboBox(this);
        _combo_exp_search->addItem(tr("All"));
        _combo_exp_search->addItem(QIcon(":/icon-classic"), tr("Classic"));
        _combo_exp_search->addItem(QIcon(":/icon-burning"), tr("Burning Cursade"));
        _combo_exp_search->addItem(QIcon(":/icon-wrath"), tr("Wrath of the Lich King"));
        _combo_exp_search->addItem(QIcon(":/icon-cata"), tr("Cataclism"));
        _combo_exp_search->addItem(QIcon(":/icon-panda"), tr("Mist of Pandaria"));
        _combo_exp_search->addItem(QIcon(":/icon-warlords"), tr("Warlords of Draenor"));
        _combo_exp_search->addItem(QIcon(":/icon-legion"), tr("Legion"));
        _combo_exp_search->addItem(QIcon(":/icon-battle"), tr("Battle for Azeroth"));
        _combo_exp_search->addItem(QIcon(":/icon-shadow"), tr("Shadowlands"));
        _combo_exp_search->setCurrentIndex(0);

        QCheckBox* _wmo_maps_search = new QCheckBox("Display WMO maps (No terrain)", this);

        QObject::connect(_line_edit_search, QOverload<const QString&>::of(&QLineEdit::textChanged), [this, _combo_search, _combo_exp_search, _wmo_maps_search](const QString &name)
                         {
                             applyFilterSearch(name, _combo_search->currentIndex(), _combo_exp_search->currentIndex(), _wmo_maps_search->isChecked());
                         });

        QObject::connect(_combo_search, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, _line_edit_search, _combo_exp_search, _wmo_maps_search](int index)
                         {
                             applyFilterSearch(_line_edit_search->text(), index, _combo_exp_search->currentIndex(), _wmo_maps_search->isChecked());
                         });

        QObject::connect(_combo_exp_search, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, _line_edit_search, _combo_search, _wmo_maps_search](int index)
                         {
                             applyFilterSearch(_line_edit_search->text(), _combo_search->currentIndex(), index, _wmo_maps_search->isChecked());
                         });

        QObject::connect(_wmo_maps_search, &QCheckBox::stateChanged, [this, _line_edit_search, _combo_search, _combo_exp_search](bool b)
                         {
                             applyFilterSearch(_line_edit_search->text(), _combo_search->currentIndex(), _combo_exp_search->currentIndex(), b);
                         });

        QFormLayout* _group_layout = new QFormLayout();
        _group_layout->addRow(tr("Name : "), _line_edit_search);
        _group_layout->addRow(tr("Type : "), _combo_search);
        _group_layout->addRow(tr("Expansion : "), _combo_exp_search);
        _group_layout->addRow( _wmo_maps_search);
        _group_search->setLayout(_group_layout);

        _first_tab_layout->addWidget(_group_search);
        _first_tab_layout->addSpacing(5);
        _first_tab_layout->addWidget(_continents_table);
        _first_tab_layout->addWidget(add_btn);

        entry_points_tabs->addTab(_first_tab, tr("Maps"));
    }

    entry_points_tabs->addTab(bookmarks_table, "Bookmarks");
    entry_points_tabs->setFixedWidth(310);
    layout->addWidget(entry_points_tabs);
    LogDebug << "NoggitWindow::buildMenu before buildMapList" << std::endl;
    _buildMapListComponent->buildMapList(this);
    LogDebug << "NoggitWindow::buildMenu after buildMapList" << std::endl;

    if (char const* autoload_map = std::getenv("NOGGIT_AUTOLOAD_MAP"))
    {
      QString const autoload_value = QString::fromUtf8(autoload_map).trimmed();
      int autoload_map_id = -1;

      bool ok = false;
      int const parsed_id = autoload_value.toInt(&ok);
      if (ok)
      {
        autoload_map_id = parsed_id;
      }
      else
      {
        for (DBCFile::Iterator it = gMapDB.begin(); it != gMapDB.end(); ++it)
        {
          QString const internal_name = QString::fromUtf8(it->getString(MapDB::InternalName));
          if (internal_name.compare(autoload_value, Qt::CaseInsensitive) == 0)
          {
            autoload_map_id = it->getInt(MapDB::MapID);
            break;
          }
        }
      }

      if (autoload_map_id >= 0)
      {
        QTimer::singleShot(0, this, [this, autoload_map_id]
        {
          loadMap(autoload_map_id);
          if (_world)
          {
            check_uid_then_enter_map(glm::vec3(0.0f, 0.0f, 0.0f), math::degrees(30.f), math::degrees(90.f), false);
          }
        });
      }
    }

    qulonglong bookmark_index(0);
    for (auto entry: _project->Bookmarks)
    {
      auto item = new QListWidgetItem(bookmarks_table);

      auto bookmark_data = Widget::MapListBookmarkData();
      bookmark_data.MapName = QString::fromStdString(entry.name);
      bookmark_data.Position = entry.position;

      auto map_bookmark_item = new Widget::MapListBookmarkItem(bookmark_data, bookmarks_table);

      item->setData(Qt::UserRole, QVariant(bookmark_index++));
      item->setSizeHint(map_bookmark_item->minimumSizeHint());
      bookmarks_table->setItemWidget(item, map_bookmark_item);
    }

    QObject::connect(bookmarks_table, &QListWidget::itemDoubleClicked, [this](QListWidgetItem* item)
                     {

                       auto& entry(_project->Bookmarks.at(item->data(Qt::UserRole).toInt()));

                       _world.reset();

                       for (DBCFile::Iterator it = gMapDB.begin(); it != gMapDB.end(); ++it)
                       {
                         if (it->getInt(MapDB::MapID) == entry.map_id)
                         {
                           _world = std::make_unique<World>(it->getString(MapDB::InternalName),
                                                            entry.map_id, Noggit::NoggitRenderContext::MAP_VIEW);
                           check_uid_then_enter_map(entry.position, math::degrees(entry.camera_pitch), math::degrees(entry.camera_yaw),
                                                    true
                           );
                           return;
                         }
                       }
                     }
    );


    _minimap = new minimap_widget(this);
    _minimap->draw_boundaries(true);
    LogDebug << "NoggitWindow::buildMenu after minimap" << std::endl;
    //_minimap->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    QObject::connect(_minimap, &minimap_widget::map_clicked, [this](::glm::vec3 const& pos)
                     {
                        if (_world->mapIndex.hasAGlobalWMO()) // skip uid check
                            enterMapAt(pos, math::degrees(30.f), math::degrees(90.f), uid_fix_mode::none, false);
                        else
                            check_uid_then_enter_map(pos, math::degrees(30.f), math::degrees(90.f));
                     }
    );

    _right_side = new QTabWidget(this);
    LogDebug << "NoggitWindow::buildMenu after right_side" << std::endl;

    auto minimap_holder = new QScrollArea(this);
    minimap_holder->setWidgetResizable(true);
    minimap_holder->setAlignment(Qt::AlignCenter);
    minimap_holder->setWidget(_minimap);

    _right_side->addTab(minimap_holder, "Enter map");
    minimap_holder->setAccessibleName("main_menu_minimap_holder");
    LogDebug << "NoggitWindow::buildMenu after enter-map tab" << std::endl;

    _map_creation_wizard_host = new QWidget(this);
    auto map_creation_wizard_host_layout = new QVBoxLayout(_map_creation_wizard_host);
    map_creation_wizard_host_layout->setContentsMargins(0, 0, 0, 0);
    _right_side->addTab(_map_creation_wizard_host, "Edit map");
    connect(_right_side, qOverload<int>(&QTabWidget::currentChanged), this, [this](int index)
    {
      if (index == 1)
        ensureMapCreationWizard();
    });
    LogDebug << "NoggitWindow::buildMenu after edit-map tab" << std::endl;

    layout->addWidget(_right_side);

    connect(add_btn, &QPushButton::clicked
        , [&]()
        {
            ensureMapCreationWizard();
            _right_side->setCurrentIndex(1);
            _map_creation_wizard->addNewMap();
        });

    //setCentralWidget (_stack_widget);

    _minimap->adjustSize();
    LogDebug << "NoggitWindow::buildMenu end" << std::endl;
  }

  void NoggitWindow::closeEvent(QCloseEvent* event)
  {
    if (map_loaded)
    {
      event->ignore();
      promptExit(event);
    } else
    {
      event->accept();
      // No map open (project menu) -> nothing to save, so exit the process for real. Closing the
      // window alone was leaving the app running in the background on some systems.
      forceQuit();
    }
  }

  void NoggitWindow::forceQuit()
  {
    // Flush user settings (volume, window state, etc.) to disk while we still can. QSettings normally
    // syncs on destruction, which the hard exit below skips.
    QSettings().sync();

    // Terminate immediately. This kills the render-loop timer, the AsyncLoader worker threads, and the
    // QMediaPlayer DirectShow/WMF audio thread all at once -- none of them can keep the process alive or
    // keep growing memory once the process is gone. std::_Exit runs no destructors (so no GL-context or
    // async-loader teardown can hang), and we already persisted what matters above.
    std::_Exit(0);
  }

  void NoggitWindow::handleEventMapListContextMenuPinMap(int mapId, std::string MapName)
  {
    _project->pinMap(mapId, MapName);
    _buildMapListComponent->buildMapList(this);
  }

  void NoggitWindow::handleEventMapListContextMenuUnpinMap(int mapId)
  {
    _project->unpinMap(mapId);
    _buildMapListComponent->buildMapList(this);
  }

  void NoggitWindow::promptExit(QCloseEvent* event)
  {
    emit exitPromptOpened();

    QMessageBox prompt;
    prompt.setModal(true);
    prompt.setIcon(QMessageBox::Warning);
    prompt.setText("Exit?");
    prompt.setInformativeText("Any unsaved changes will be lost.");
    prompt.addButton("Exit", QMessageBox::DestructiveRole);
    prompt.addButton("Return to menu", QMessageBox::AcceptRole);
    prompt.setDefaultButton(prompt.addButton("Cancel", QMessageBox::RejectRole));
    prompt.setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowStaysOnTopHint);

    prompt.exec();

    switch (prompt.buttonRole(prompt.clickedButton()))
    {
      case QMessageBox::AcceptRole:
        _stack_widget->setCurrentIndex(0);
        _stack_widget->removeLast();
        delete _map_view;
        _map_view = nullptr;
        _minimap->world(nullptr);

        map_loaded = false;
        break;
      case QMessageBox::DestructiveRole:
        // User chose Exit (unsaved changes already warned as lost). Don't rely on the Qt/GL teardown to
        // unwind cleanly -- it has been leaving the process alive in the background, leaking memory and
        // still playing zone music. Just terminate the process.
        event->accept();
        forceQuit();
        break;
      default:
        event->ignore();
        break;
    }
  }

  void NoggitWindow::promptUidFixFailure()
  {
    _stack_widget->setCurrentIndex(0);

    QMessageBox::critical
        (nullptr, "UID fix failed", "The UID fix couldn't be done because some models were missing or fucked up.\n"
                                    "The models are listed in the log file.", QMessageBox::Ok
        );
  }
}
