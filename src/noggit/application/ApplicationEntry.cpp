// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/DBC.h>
#include <noggit/World.h>
#include <noggit/errorHandling.h>
#include <noggit/project/ApplicationProject.h>
#include <noggit/project/CurrentProject.hpp>
#include <opengl/context.hpp>
#include <util/exception_to_string.hpp>
#include <external/framelesshelper/framelesswindowsmanager.h>
#include <string>
#include <string_view>
#include <QtCore/QSettings>
#include <QtCore/QDir>
#include <qcommandlineparser.h>
#include <qcommandlineoption.h>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>
#include <QSplashScreen>
#include <QStyleFactory>
#include <codecvt>
#include <string>

namespace
{
  int probeClassicMapList(std::filesystem::path const& project_path)
  {
    auto noggit = Noggit::Application::NoggitApplication::instance();
    auto project_loader = Noggit::Project::ApplicationProject(noggit->getConfiguration());
    auto loaded_project = project_loader.loadProject(project_path);

    if (!loaded_project)
    {
      LogError << "probe-classic-map-list: failed to load project" << std::endl;
      return 2;
    }

    noggit->setClientData(loaded_project->ClientData);
    Noggit::Project::CurrentProject::initialize(loaded_project.get());
    OpenDBs(loaded_project->ClientData);

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

    parser->addOption({"probe-project-load",
               QApplication::translate("main", "Load a Noggit project and exit with code 0 on success."),
               QApplication::translate("main", "project")});

    parser->addOption({"probe-classic-map-list",
           QApplication::translate("main", "Load a classic Noggit project, open legacy DBCs, dump map rows, and exit."),
           QApplication::translate("main", "project")});

    return parser;
}

int main(int argc, char *argv[])
{
  Noggit::RegisterErrorHandlers();
  std::set_terminate(Noggit::Application::NoggitApplication::terminationHandler);

  QApplication::setStyle(QStyleFactory::create("Fusion"));
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
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

  auto project_selection = new Noggit::Ui::Windows::NoggitProjectSelectionWindow(noggit);
  project_selection->show();

  return q_application.exec();
}