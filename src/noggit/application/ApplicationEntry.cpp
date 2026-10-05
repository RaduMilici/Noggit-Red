#include <noggit/runtime/RuntimeManager.hpp>
#include <QProgressDialog>
// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/AsyncLoader.h>
#include <noggit/Log.h>
#include <noggit/DBC.h>
#include <noggit/World.h>
#include <noggit/errorHandling.h>
#include <noggit/Model.h>
#include <noggit/TileIndex.hpp>
#include <noggit/project/ApplicationProject.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <opengl/context.hpp>
#include <util/exception_to_string.hpp>
#include <external/framelesshelper/framelesswindowsmanager.h>
#include <string>
#include <string_view>
#include <QtCore/QSettings>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <qcommandlineparser.h>
#include <qcommandlineoption.h>
#include <QtWidgets/QApplication>
#include <QtGui/QSurfaceFormat>
#include <QtGui/QScreen>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>
#include <QSplashScreen>
#include <QStyleFactory>
#include <codecvt>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
  std::string normalizeProbeModelFilename(std::string filename)
  {
    filename = BlizzardArchive::ClientData::normalizeFilenameInternal(std::move(filename));

    std::size_t found;
    if ((found = filename.rfind(".mdx")) != std::string::npos)
    {
      filename.replace(found, 4, ".m2");
    }
    else if ((found = filename.rfind(".mdl")) != std::string::npos)
    {
      filename.replace(found, 4, ".m2");
    }
    else if (filename.rfind('.') == std::string::npos)
    {
      filename += ".m2";
    }

    return filename;
  }

  std::shared_ptr<Noggit::Project::NoggitProject> loadProbeProject(std::filesystem::path const& project_path)
  {
    auto noggit = Noggit::Application::NoggitApplication::instance();
    auto project_loader = Noggit::Project::ApplicationProject(noggit->getConfiguration());
    auto loaded_project = project_loader.loadProject(project_path);

    if (!loaded_project)
    {
      return nullptr;
    }

    noggit->setClientData(loaded_project->ClientData);
    Noggit::Project::CurrentProject::initialize(loaded_project.get());
    OpenDBs(loaded_project->ClientData);
    return loaded_project;
  }

  int probeClassicMapList(std::filesystem::path const& project_path)
  {
    auto loaded_project = loadProbeProject(project_path);

    if (!loaded_project)
    {
      LogError << "probe-classic-map-list: failed to load project" << std::endl;
      return 2;
    }

    for (DBCFile::Iterator iterator = gMapDB.begin(); iterator != gMapDB.end(); ++iterator)
    {
      auto record = *iterator;
      auto const map_id = record.getUInt(MapDB::MapID);
      auto const directory = std::string(record.getString(MapDB::InternalName));
      auto const map_type = record.getInt(MapDB::AreaType);
      auto const editable = World::IsEditableWorld(record);

      LogDebug << "probe-classic-map-list map id=" << map_id
               << " dir='" << directory << "'"
               << " type=" << map_type
               << " editable=" << editable;

      for (int locale = 0; locale < 8; ++locale)
      {
        auto const value = std::string(record.getLocalizedString(MapDB::Name, locale));
        if (!value.empty())
        {
          LogDebug << " loc" << locale << "='" << value << "'";
        }
      }

      LogDebug << std::endl;
    }

    return 0;
  }

  int probeModelLoad(std::filesystem::path const& project_path, std::string model_path)
  {
    auto loaded_project = loadProbeProject(project_path);
    if (!loaded_project)
    {
      LogError << "probe-model-load: failed to load project" << std::endl;
      return 2;
    }

    model_path = normalizeProbeModelFilename(std::move(model_path));
    LogDebug << "probe-model-load model='" << model_path << "'" << std::endl;

    try
    {
      Model model(model_path, Noggit::NoggitRenderContext::MAP_VIEW);
      model.finishLoading();
      LogDebug << "probe-model-load result model='" << model_path
               << "' finished=" << model.finishedLoading()
               << " failed=" << model.loading_failed()
               << " classic=" << model.usesClassicLayout()
               << std::endl;
      return model.finishedLoading() && !model.loading_failed() ? 0 : 3;
    }
    catch (std::exception const& e)
    {
      LogError << "probe-model-load exception model='" << model_path << "' error='" << e.what() << "'" << std::endl;
      return 4;
    }
    catch (...)
    {
      LogError << "probe-model-load unknown exception model='" << model_path << "'" << std::endl;
      return 5;
    }
  }

  int probeCreatureDisplayLoad(std::filesystem::path const& project_path, std::uint32_t display_id)
  {
    auto loaded_project = loadProbeProject(project_path);
    if (!loaded_project)
    {
      LogError << "probe-creature-display-load: failed to load project" << std::endl;
      return 2;
    }

    try
    {
      auto display = gCreatureDisplayInfoDB.getByID(display_id);
      auto model_id = display.getUInt(CreatureDisplayInfoDB::ModelID);
      auto model = gCreatureModelDataDB.getByID(model_id);
      auto model_path = normalizeProbeModelFilename(model.getString(CreatureModelDataDB::ModelName));
      LogDebug << "probe-creature-display-load display=" << display_id
               << " modelId=" << model_id
               << " model='" << model_path << "'" << std::endl;
      return probeModelLoad(project_path, std::move(model_path));
    }
    catch (DBCFile::NotFound const&)
    {
      LogError << "probe-creature-display-load missing DBC display=" << display_id << std::endl;
      return 3;
    }
  }

  int probeWorldCreatures(std::filesystem::path const& project_path, std::uint32_t map_id)
  {
    auto loaded_project = loadProbeProject(project_path);
    if (!loaded_project)
    {
      LogError << "probe-world-creatures: failed to load project" << std::endl;
      return 2;
    }

    try
    {
      for (DBCFile::Iterator iterator = gMapDB.begin(); iterator != gMapDB.end(); ++iterator)
      {
        auto record = *iterator;
        if (record.getUInt(MapDB::MapID) != map_id)
        {
          continue;
        }

        auto const map_name = std::string(record.getString(MapDB::InternalName));
        LogDebug << "probe-world-creatures mapId=" << map_id
                 << " map='" << map_name << "'" << std::endl;

        World world(map_name, static_cast<int>(map_id), Noggit::NoggitRenderContext::MAP_VIEW, false);
        bool const loaded = world.reloadCreatureSpawns();
        AsyncLoader::instance().wait_until_idle();

        LogDebug << "probe-world-creatures result mapId=" << map_id
                 << " loaded=" << loaded
                 << " spawnCount=" << world.creatureSpawnCount()
                 << " modelCount=" << world.creatureSpawnModelCount()
                 << " status='" << world.creatureSpawnStatus() << "'"
                 << " importantObjectFailed=" << AsyncLoader::instance().important_object_failed_loading()
                 << std::endl;

        int const exit_code = loaded && !AsyncLoader::instance().important_object_failed_loading() ? 0 : 3;
        std::fflush(nullptr);
        std::_Exit(exit_code);
      }
    }
    catch (std::exception const& e)
    {
      LogError << "probe-world-creatures exception mapId=" << map_id
               << " error='" << e.what() << "'" << std::endl;
      std::fflush(nullptr);
      std::_Exit(4);
    }
    catch (...)
    {
      LogError << "probe-world-creatures unknown exception mapId=" << map_id << std::endl;
      std::fflush(nullptr);
      std::_Exit(5);
    }

    LogError << "probe-world-creatures missing mapId=" << map_id << std::endl;
    return 3;
  }

  int probeWorldTileLoad(std::filesystem::path const& project_path,
                         std::uint32_t map_id,
                         std::uint32_t tile_x,
                         std::uint32_t tile_z)
  {
    auto loaded_project = loadProbeProject(project_path);
    if (!loaded_project)
    {
      LogError << "probe-world-tile: failed to load project" << std::endl;
      return 2;
    }

    try
    {
      for (DBCFile::Iterator iterator = gMapDB.begin(); iterator != gMapDB.end(); ++iterator)
      {
        auto record = *iterator;
        if (record.getUInt(MapDB::MapID) != map_id)
        {
          continue;
        }

        auto const map_name = std::string(record.getString(MapDB::InternalName));
        LogDebug << "probe-world-tile mapId=" << map_id
                 << " map='" << map_name << "'"
                 << " tile=" << tile_x << "," << tile_z
                 << std::endl;

        World world(map_name, static_cast<int>(map_id), Noggit::NoggitRenderContext::MAP_VIEW, false);
        MapTile* tile = world.mapIndex.loadTile(TileIndex(tile_x, tile_z));
        if (!tile)
        {
          LogError << "probe-world-tile loadTile returned null tile=" << tile_x << "," << tile_z << std::endl;
          std::fflush(nullptr);
          std::_Exit(3);
        }

        tile->wait_until_loaded();
        AsyncLoader::instance().wait_until_idle();

        LogDebug << "probe-world-tile result mapId=" << map_id
                 << " tile=" << tile_x << "," << tile_z
                 << " finished=" << tile->finishedLoading()
                 << " failed=" << tile->loading_failed()
                 << " importantObjectFailed=" << AsyncLoader::instance().important_object_failed_loading()
                 << std::endl;

        int const exit_code = tile->finishedLoading() && !tile->loading_failed() ? 0 : 4;
        std::fflush(nullptr);
        std::_Exit(exit_code);
      }
    }
    catch (std::exception const& e)
    {
      LogError << "probe-world-tile exception mapId=" << map_id
               << " tile=" << tile_x << "," << tile_z
               << " error='" << e.what() << "'" << std::endl;
      std::fflush(nullptr);
      std::_Exit(5);
    }
    catch (...)
    {
      LogError << "probe-world-tile unknown exception mapId=" << map_id
               << " tile=" << tile_x << "," << tile_z << std::endl;
      std::fflush(nullptr);
      std::_Exit(6);
    }

    LogError << "probe-world-tile missing mapId=" << map_id << std::endl;
    return 3;
  }

  int captureWorldCreatures(std::filesystem::path const& project_path,
                            std::uint32_t map_id,
                            QString const& output_path,
                            std::optional<glm::vec3> camera_position,
                            math::degrees camera_yaw,
                            math::degrees camera_pitch)
  {
    auto loaded_project = loadProbeProject(project_path);
    if (!loaded_project)
    {
      LogError << "capture-world-creatures: failed to load project" << std::endl;
      return 2;
    }

    auto noggit = Noggit::Application::NoggitApplication::instance();
    auto window = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(noggit->getConfiguration(),
                                                                      loaded_project);
    bool const captured = window->captureMapCreaturesToPng(static_cast<int>(map_id),
                                                           output_path,
                                                           1280,
                                                           800,
                                                           camera_position,
                                                           camera_yaw,
                                                           camera_pitch);
    std::fflush(nullptr);
    std::_Exit(captured ? 0 : 3);
  }
}

QCommandLineParser* ProcessCommandLine()
{
    QCommandLineParser* parser = new QCommandLineParser();
    parser->setApplicationDescription("Help");
    parser->addHelpOption();
    parser->addVersionOption();
    parser->addOptions({
        {"disable-update", QApplication::translate("main", "Disable the check for update.")},
      {"force-changelog", QApplication::translate("main", "Force displaying the changelog popup.")}
        });

    // [VULKAN 2026-08-29] self-run GL-vs-VK parity loop (off-screen, no user interaction)
    parser->addOption({"vk-parity-project", QApplication::translate("main", "Project path for --vk-parity-map-id."),
                       QApplication::translate("main", "project")});
    parser->addOption({"vk-parity-map-id", QApplication::translate("main", "Map id to open for the VK parity run."),
                       QApplication::translate("main", "id")});
    parser->addOption({"vk-parity-cams", QApplication::translate("main", "Camera list file (name x y z yaw pitch per line)."),
                       QApplication::translate("main", "file")});
    parser->addOption({"vk-parity-out", QApplication::translate("main", "Output directory for the parity PNGs."),
                       QApplication::translate("main", "dir")});
    parser->addOption({"probe-project-load",
               QApplication::translate("main", "Load a Noggit project and exit with code 0 on success."),
               QApplication::translate("main", "project")});

    parser->addOption({"probe-classic-map-list",
           QApplication::translate("main", "Load a classic Noggit project, open legacy DBCs, dump map rows, and exit."),
           QApplication::translate("main", "project")});

    parser->addOption({"probe-model-load-project",
           QApplication::translate("main", "Project path for --probe-model-load-path."),
           QApplication::translate("main", "project")});

    parser->addOption({"probe-model-load-path",
           QApplication::translate("main", "Synchronously load one M2 model and exit."),
           QApplication::translate("main", "model")});

    parser->addOption({"probe-creature-display-project",
           QApplication::translate("main", "Project path for --probe-creature-display-id."),
           QApplication::translate("main", "project")});

    parser->addOption({"probe-creature-display-id",
           QApplication::translate("main", "Resolve one CreatureDisplayInfo id, synchronously load its M2 model, and exit."),
           QApplication::translate("main", "display")});

    parser->addOption({"probe-world-creatures-project",
           QApplication::translate("main", "Project path for --probe-world-creatures-map-id."),
           QApplication::translate("main", "project")});

    parser->addOption({"probe-world-creatures-map-id",
           QApplication::translate("main", "Load one map's SQL creature spawns, wait for async model loads, and exit."),
           QApplication::translate("main", "mapId")});

    parser->addOption({"probe-world-tile-project",
           QApplication::translate("main", "Project path for --probe-world-tile-map-id."),
           QApplication::translate("main", "project")});

    parser->addOption({"probe-world-tile-map-id",
           QApplication::translate("main", "Load one map tile and exit."),
           QApplication::translate("main", "mapId")});

    parser->addOption({"probe-world-tile-x",
           QApplication::translate("main", "Tile X for --probe-world-tile-map-id."),
           QApplication::translate("main", "x")});

    parser->addOption({"probe-world-tile-z",
           QApplication::translate("main", "Tile Z for --probe-world-tile-map-id."),
           QApplication::translate("main", "z")});

    parser->addOption({"probe-world-creatures-capture-project",
           QApplication::translate("main", "Project path for --probe-world-creatures-capture-map-id."),
           QApplication::translate("main", "project")});

    parser->addOption({"probe-world-creatures-capture-map-id",
           QApplication::translate("main", "Open one map, render SQL creature spawns, capture a PNG, and exit."),
           QApplication::translate("main", "mapId")});

    parser->addOption({"probe-world-creatures-capture-output",
           QApplication::translate("main", "PNG output path for --probe-world-creatures-capture-map-id."),
           QApplication::translate("main", "png")});

    parser->addOption({"probe-world-creatures-capture-camera-x",
           QApplication::translate("main", "Internal client camera X for creature capture."),
           QApplication::translate("main", "x")});

    parser->addOption({"probe-world-creatures-capture-camera-y",
           QApplication::translate("main", "Internal client camera Y/height for creature capture."),
           QApplication::translate("main", "y")});

    parser->addOption({"probe-world-creatures-capture-camera-z",
           QApplication::translate("main", "Internal client camera Z for creature capture."),
           QApplication::translate("main", "z")});

    parser->addOption({"probe-world-creatures-capture-camera-yaw",
           QApplication::translate("main", "Camera yaw in degrees for creature capture."),
           QApplication::translate("main", "yaw")});

    parser->addOption({"probe-world-creatures-capture-camera-pitch",
           QApplication::translate("main", "Camera pitch in degrees for creature capture."),
           QApplication::translate("main", "pitch")});

    return parser;
}

int main(int argc, char *argv[])
{
  Noggit::RegisterErrorHandlers();
  std::set_terminate(Noggit::Application::NoggitApplication::terminationHandler);

  QApplication::setStyle(QStyleFactory::create("Fusion"));
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  // Share GL objects (buffers/textures/shaders) across ALL contexts. The asset-browser preview
  // renders in its own OFFSCREEN context but draws models whose GL buffers were uploaded in the
  // main window context; without sharing those objects are invalid in the offscreen context and
  // the NVIDIA driver __fastfails when the draw is flushed (the asset-browser crash-on-load). Must
  // be set BEFORE the QApplication is constructed.
  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

  // [PERF 2026-07-21] Frame-to-frame time carries a ~15ms CONSTANT gap beyond the CPU world-draw
  // (independent of scene cost) = the present/vsync wait. NOGGIT_NO_VSYNC=1 sets the GL swap interval to 0
  // so fps can exceed the display refresh (toward the 165fps goal); tearing is the tradeoff. Default keeps
  // vsync ON (interval 1). Must be set BEFORE the QApplication, like the attributes above.
  {
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setSwapInterval(std::getenv("NOGGIT_NO_VSYNC") != nullptr ? 0 : 1);
    QSurfaceFormat::setDefaultFormat(fmt);
  }

  QApplication q_application (argc, argv);
  q_application.setApplicationName ("Noggit");
  q_application.setOrganizationName ("Noggit");

  auto parser = ProcessCommandLine();
  parser->process(q_application);

  std::vector<bool> Command;
  Command.push_back(parser->isSet("disable-update"));
  Command.push_back(parser->isSet("force-changelog"));

  auto noggit = Noggit::Application::NoggitApplication::instance();
  noggit->initalize(argc, argv, Command);

  if (parser->isSet("probe-project-load"))
  {
    auto const project_path = parser->value("probe-project-load").toStdString();
    auto project_loader = Noggit::Project::ApplicationProject(noggit->getConfiguration());
    auto loaded_project = project_loader.loadProject(std::filesystem::path(project_path));
    return loaded_project ? 0 : 2;
  }

  if (parser->isSet("probe-classic-map-list"))
  {
    auto const project_path = parser->value("probe-classic-map-list").toStdString();
    return probeClassicMapList(std::filesystem::path(project_path));
  }

  if (parser->isSet("probe-model-load-project") || parser->isSet("probe-model-load-path"))
  {
    if (!parser->isSet("probe-model-load-project") || !parser->isSet("probe-model-load-path"))
    {
      LogError << "probe-model-load requires --probe-model-load-project and --probe-model-load-path" << std::endl;
      return 2;
    }

    return probeModelLoad(std::filesystem::path(parser->value("probe-model-load-project").toStdString()),
                          parser->value("probe-model-load-path").toStdString());
  }

  if (parser->isSet("probe-creature-display-project") || parser->isSet("probe-creature-display-id"))
  {
    if (!parser->isSet("probe-creature-display-project") || !parser->isSet("probe-creature-display-id"))
    {
      LogError << "probe-creature-display-load requires --probe-creature-display-project and --probe-creature-display-id" << std::endl;
      return 2;
    }

    bool ok = false;
    auto const display_id = parser->value("probe-creature-display-id").toUInt(&ok);
    if (!ok)
    {
      LogError << "probe-creature-display-load display id must be an integer" << std::endl;
      return 2;
    }

    return probeCreatureDisplayLoad(std::filesystem::path(parser->value("probe-creature-display-project").toStdString()),
                                    display_id);
  }

  if (parser->isSet("probe-world-creatures-project") || parser->isSet("probe-world-creatures-map-id"))
  {
    if (!parser->isSet("probe-world-creatures-project") || !parser->isSet("probe-world-creatures-map-id"))
    {
      LogError << "probe-world-creatures requires --probe-world-creatures-project and --probe-world-creatures-map-id" << std::endl;
      return 2;
    }

    bool ok = false;
    auto const map_id = parser->value("probe-world-creatures-map-id").toUInt(&ok);
    if (!ok)
    {
      LogError << "probe-world-creatures map id must be an integer" << std::endl;
      return 2;
    }

    return probeWorldCreatures(std::filesystem::path(parser->value("probe-world-creatures-project").toStdString()),
                               map_id);
  }

  if (parser->isSet("probe-world-tile-project")
      || parser->isSet("probe-world-tile-map-id")
      || parser->isSet("probe-world-tile-x")
      || parser->isSet("probe-world-tile-z"))
  {
    if (!parser->isSet("probe-world-tile-project")
        || !parser->isSet("probe-world-tile-map-id")
        || !parser->isSet("probe-world-tile-x")
        || !parser->isSet("probe-world-tile-z"))
    {
      LogError << "probe-world-tile requires --probe-world-tile-project,"
               << " --probe-world-tile-map-id, --probe-world-tile-x, and --probe-world-tile-z"
               << std::endl;
      return 2;
    }

    bool ok_map = false;
    bool ok_x = false;
    bool ok_z = false;
    auto const map_id = parser->value("probe-world-tile-map-id").toUInt(&ok_map);
    auto const tile_x = parser->value("probe-world-tile-x").toUInt(&ok_x);
    auto const tile_z = parser->value("probe-world-tile-z").toUInt(&ok_z);
    if (!ok_map || !ok_x || !ok_z || tile_x >= 64 || tile_z >= 64)
    {
      LogError << "probe-world-tile map id and tile coordinates must be valid integers" << std::endl;
      return 2;
    }

    return probeWorldTileLoad(std::filesystem::path(parser->value("probe-world-tile-project").toStdString()),
                              map_id,
                              tile_x,
                              tile_z);
  }

  if (parser->isSet("probe-world-creatures-capture-project")
      || parser->isSet("probe-world-creatures-capture-map-id")
      || parser->isSet("probe-world-creatures-capture-output"))
  {
    if (!parser->isSet("probe-world-creatures-capture-project")
        || !parser->isSet("probe-world-creatures-capture-map-id")
        || !parser->isSet("probe-world-creatures-capture-output"))
    {
      LogError << "capture-world-creatures requires --probe-world-creatures-capture-project,"
               << " --probe-world-creatures-capture-map-id, and --probe-world-creatures-capture-output"
               << std::endl;
      return 2;
    }

    bool ok = false;
    auto const map_id = parser->value("probe-world-creatures-capture-map-id").toUInt(&ok);
    if (!ok)
    {
      LogError << "capture-world-creatures map id must be an integer" << std::endl;
      return 2;
    }

    std::optional<glm::vec3> camera_position;
    if (parser->isSet("probe-world-creatures-capture-camera-x")
        || parser->isSet("probe-world-creatures-capture-camera-y")
        || parser->isSet("probe-world-creatures-capture-camera-z"))
    {
      if (!parser->isSet("probe-world-creatures-capture-camera-x")
          || !parser->isSet("probe-world-creatures-capture-camera-y")
          || !parser->isSet("probe-world-creatures-capture-camera-z"))
      {
        LogError << "capture-world-creatures explicit camera requires x, y, and z" << std::endl;
        return 2;
      }

      bool ok_x = false;
      bool ok_y = false;
      bool ok_z = false;
      float const x = parser->value("probe-world-creatures-capture-camera-x").toFloat(&ok_x);
      float const y = parser->value("probe-world-creatures-capture-camera-y").toFloat(&ok_y);
      float const z = parser->value("probe-world-creatures-capture-camera-z").toFloat(&ok_z);
      if (!ok_x || !ok_y || !ok_z)
      {
        LogError << "capture-world-creatures explicit camera x/y/z must be numbers" << std::endl;
        return 2;
      }
      camera_position = glm::vec3(x, y, z);
    }

    bool ok_yaw = true;
    bool ok_pitch = true;
    math::degrees camera_yaw(90.f);
    math::degrees camera_pitch(30.f);
    if (parser->isSet("probe-world-creatures-capture-camera-yaw"))
    {
      camera_yaw = math::degrees(parser->value("probe-world-creatures-capture-camera-yaw").toFloat(&ok_yaw));
    }
    if (parser->isSet("probe-world-creatures-capture-camera-pitch"))
    {
      camera_pitch = math::degrees(parser->value("probe-world-creatures-capture-camera-pitch").toFloat(&ok_pitch));
    }
    if (!ok_yaw || !ok_pitch)
    {
      LogError << "capture-world-creatures camera yaw/pitch must be numbers" << std::endl;
      return 2;
    }

    return captureWorldCreatures(std::filesystem::path(parser->value("probe-world-creatures-capture-project").toStdString()),
                                 map_id,
                                 parser->value("probe-world-creatures-capture-output"),
                                 camera_position,
                                 camera_yaw,
                                 camera_pitch);
  }

  // [VULKAN 2026-08-29] --vk-parity-*: off-screen GL-vs-VK parity run, then exit (0 = harness finished)
  if (parser->isSet("vk-parity-project"))
  {
    if (!parser->isSet("vk-parity-map-id") || !parser->isSet("vk-parity-cams"))
    {
      LogError << "vk-parity: need --vk-parity-map-id and --vk-parity-cams" << std::endl;
      return 2;
    }
    auto loaded_project = loadProbeProject(std::filesystem::path(parser->value("vk-parity-project").toStdString()));
    if (!loaded_project)
    {
      LogError << "vk-parity: failed to load project" << std::endl;
      return 2;
    }
    auto window = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(noggit->getConfiguration(), loaded_project);
    bool const ok = window->runVkParity(parser->value("vk-parity-map-id").toInt(),
                                        parser->value("vk-parity-cams"),
                                        parser->isSet("vk-parity-out") ? parser->value("vk-parity-out") : QString("vk_diff"),
                                        // [2026-09-05] NOGGIT_PARITY_SIZE=WxH: the harness was pinned at
                                        // 1600x900 while the user runs 2288x1329, so window size was the
                                        // one environmental delta never tested against their session.
                                        []{ char const* v = std::getenv("NOGGIT_PARITY_SIZE");
                                            int w = 1600, h = 900;
                                            if (v && std::sscanf(v, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) return w;
                                            return 1600; }(),
                                        []{ char const* v = std::getenv("NOGGIT_PARITY_SIZE");
                                            int w = 1600, h = 900;
                                            if (v && std::sscanf(v, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) return h;
                                            return 900; }());
    std::fflush(nullptr);
    std::_Exit(ok ? 0 : 3);
  }

  // Runtime ownership is scoped to QApplication, after command-line probes have returned.
  // Installed layout is Creator/Noggit/noggit; a development build (build/bin/noggit) uses the
  // prepared bundle in build/Creator when it has no Runtime of its own.
  QString runtime_root = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("..");
  if (!QFileInfo::exists(runtime_root + "/Runtime/creator-runtime.json")
      && QFileInfo::exists(runtime_root + "/Creator/Runtime/creator-runtime.json"))
    runtime_root += "/Creator";
  auto runtime = new Noggit::Runtime::RuntimeManager(runtime_root, &q_application);
  QObject::connect(&q_application, &QCoreApplication::aboutToQuit,
                   runtime, &Noggit::Runtime::RuntimeManager::shutdown);
  auto dataProgress = new QProgressDialog("Downloading game data…", "Cancel", 0, 100);
  dataProgress->setWindowTitle("Creator setup");
  dataProgress->setAutoClose(false);
  dataProgress->setAutoReset(false);
  dataProgress->setMinimumDuration(0);
  dataProgress->hide();
  QObject::connect(runtime, &Noggit::Runtime::RuntimeManager::gameDataProgress,
                   dataProgress, [dataProgress](qint64 received, qint64 total) {
    dataProgress->setLabelText(QString("Downloading game data: %1 / %2 MB\nCompleted files are kept if you cancel.")
        .arg(received / (1024 * 1024)).arg(total / (1024 * 1024)));
    dataProgress->setValue(total ? int(received * 100 / total) : 0);
    if (received < total) dataProgress->show();
  });
  QObject::connect(dataProgress, &QProgressDialog::canceled, runtime, &Noggit::Runtime::RuntimeManager::stop);
  QObject::connect(runtime, &Noggit::Runtime::RuntimeManager::changed, dataProgress, [runtime, dataProgress] {
    if (!runtime->status(2).startsWith("Downloading")) dataProgress->hide();
  });
  QObject::connect(&q_application, &QCoreApplication::aboutToQuit, dataProgress, &QObject::deleteLater);
  if (QSettings(runtime->root() + "/Workspace/runtime.ini", QSettings::IniFormat)
        .value("autostart", true).toBool())
    QTimer::singleShot(0, runtime, &Noggit::Runtime::RuntimeManager::start);

  auto project_selection = new Noggit::Ui::Windows::NoggitProjectSelectionWindow(noggit);
  // Always open on the PRIMARY display (Qt's default places new windows on whichever screen holds
  // the mouse cursor -- user-rejected). Same rule for the main editor window (NoggitProjectSelectionWindow).
  if (QScreen* primary_screen = QGuiApplication::primaryScreen())
  {
    project_selection->move(primary_screen->availableGeometry().center() - project_selection->rect().center());
  }
  project_selection->show();

  return q_application.exec();
}
