#ifndef NOGGIT_WINDOW_NOGGIT_HPP
#define NOGGIT_WINDOW_NOGGIT_HPP

#include <math/trig.hpp>
#include <noggit/World.h>
#include <noggit/MapView.h>
#include <noggit/ui/UidFixWindow.hpp>
#include <noggit/ui/tools/MapCreationWizard/Ui/MapCreationWizard.hpp>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/ui/windows/noggitWindow/components/BuildMapListComponent.hpp>
#include <noggit/project/ApplicationProject.h>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QStackedWidget>
#include <string>
#include <memory>
#include <optional>
#include <unordered_set>
#include <QWidget>

class StackedWidget;
class QString;

namespace Noggit::Ui
{
    class minimap_widget;
    class settings;
    class about;
}

namespace Noggit::Ui::Windows
{
    class NoggitWindow : public QMainWindow
    {
      Q_OBJECT

      friend class Noggit::Ui::Component::BuildMapListComponent;

    public:
      NoggitWindow(std::shared_ptr<Noggit::Application::NoggitApplicationConfiguration> application,
          std::shared_ptr<Noggit::Project::NoggitProject> project);

      void promptExit(QCloseEvent* event);

      // Persist settings, silence audio, then terminate the process outright. Used on the user-confirmed
      // exit paths. The normal Qt/OpenGL/AsyncLoader teardown has repeatedly failed to actually exit on
      // Windows -- the event loop kept running so memory grew and zone music kept playing in the
      // background after the window "closed". A hard exit once the user has committed to quitting is the
      // one thing that can't get stuck. (Return-to-menu does NOT go through here -- only real exit does.)
      [[noreturn]] void forceQuit();
      void promptUidFixFailure();
      void jumpToMapPosition(int map_id,
                             glm::vec3 pos,
                             math::degrees camera_pitch = math::degrees(30.f),
                             math::degrees camera_yaw = math::degrees(90.f),
                             bool from_bookmark = false);
      bool captureMapCreaturesToPng(int map_id,
                                    QString const& output_path,
                                    int width = 1280,
                                    int height = 800,
                                    std::optional<glm::vec3> camera_position = std::nullopt,
                                    math::degrees camera_yaw = math::degrees(90.f),
                                    math::degrees camera_pitch = math::degrees(30.f));
      // [VULKAN 2026-08-29] self-run parity loop: open map_id OFF-SCREEN (window never activates or
      // appears on a monitor), force Graphics API = Vulkan + parity check, step the camera list, write
      // vk_diff/<cam>_{gl,vk,diff}.png + the [VK-DIFF] lines, return when the harness reports done.
      bool runVkParity(int map_id, QString const& cams_file, QString const& out_dir, int width, int height);

      QMenuBar* _menuBar;

      std::unordered_set<QWidget*> displayed_widgets;
      void buildMenu();
    signals:
      void exitPromptOpened();
      void mapSelected(int map_id);


    private:
    	std::unique_ptr<Component::BuildMapListComponent> _buildMapListComponent;
        std::shared_ptr<Application::NoggitApplicationConfiguration> _applicationConfiguration;
        std::shared_ptr<Project::NoggitProject> _project;


        void handleEventMapListContextMenuPinMap(int mapId, std::string MapName);
        void handleEventMapListContextMenuUnpinMap(int mapId);


      void loadMap (int map_id);

      void check_uid_then_enter_map ( glm::vec3 pos
                                    , math::degrees camera_pitch
                                    , math::degrees camera_yaw
                                    , bool from_bookmark = false
                                    );

      void enterMapAt ( glm::vec3 pos
                      , math::degrees camera_pitch
                      , math::degrees camera_yaw
                      , uid_fix_mode uid_fix = uid_fix_mode::none
                      , bool from_bookmark = false
                      , bool capture_probe = false
                      );

      minimap_widget* _minimap;
      settings* _settings;
      about* _about;
      QWidget* _null_widget;
      MapView* _map_view;
      StackedWidget* _stack_widget;

      Noggit::Ui::Tools::MapCreationWizard::Ui::MapCreationWizard* _map_creation_wizard;
      QMetaObject::Connection _map_wizard_connection;

      QListWidget* _continents_table;
      QString _filter_name;
      QTabWidget* _right_side;

      void applyFilterSearch(const QString& name, int type, int expansion, bool wmo_maps);
      void ensureMapCreationWizard();
      void ensureSettingsWindow();
      void ensureAboutWindow();

      std::unique_ptr<World> _world;
      QWidget* _map_creation_wizard_host = nullptr;

      bool map_loaded = false;

      virtual void closeEvent (QCloseEvent*) override;
    };
}
#endif // NOGGIT_WINDOW_NOGGIT_HPP
