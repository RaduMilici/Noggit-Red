// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <qt-color-widgets/color_selector.hpp>

#include <QtCore/QSettings>
#include <QMainWindow>
#include <QLineEdit>
#include <QtWidgets/QCheckBox>
#include <ui_SettingsPanel.h>

#include <functional>
#include <vector>
#include <tuple>

class QGroupBox;
class QLabel;
class QPushButton;
class QSpinBox;


namespace Noggit
{
  namespace Ui
  {
    class settings : public QMainWindow
    {
      Q_OBJECT
      QSettings* _settings;
      QLineEdit* _casc_listfile_field = nullptr; // Settings > Paths: CASC listfile (modern clients)
      ::Ui::SettingsPanel* ui;
      // Persistent render-feature toggles shown on the Graphics tab: {settings key, checkbox, default}.
      // Mirror the live View-menu/toolbar switches but survive restarts (applied by MapView at load).
      std::vector<std::tuple<QString, QCheckBox*, bool>> _render_toggles;
      // Runtime-added distance sliders + advanced WMO toggles collect here and land on their own
      // "Performance" tab (they squished the Graphics tab when appended to the Viewport group).
      class QVBoxLayout* _perf_layout = nullptr;
      void build_graphics_tab();

      // MySQL tab: "SSH tunnel" section + the (asynchronous) connection tests.
      QGroupBox* _ssh_box = nullptr;
      QLineEdit* _ssh_host = nullptr;
      QSpinBox* _ssh_port = nullptr;
      QLineEdit* _ssh_user = nullptr;
      QLineEdit* _ssh_key = nullptr;
      QLineEdit* _ssh_remote_host = nullptr;
      QSpinBox* _ssh_remote_port = nullptr;
      QLineEdit* _ssh_fingerprint = nullptr;
      QLabel* _ssh_status = nullptr;
      QPushButton* _ssh_test = nullptr;
      QPushButton* _ssh_forget = nullptr;
      bool _db_test_running = false;
      void build_ssh_section();
      void update_ssh_status();
      void update_direct_fields_enabled();
      void set_db_test_running(bool running);
      // Starts the project's tunnel and calls `on_connected` once it is up; reports failures itself
      // (including the host-key confirmation). Never blocks the UI thread.
      void with_tunnel(bool explicit_restart, std::function<void()> on_connected);
      void test_ssh_tunnel();
      void test_mysql_connection();
    public:
      settings(QWidget* parent = nullptr);
      void discard_changes();
      void save_changes();

    signals:
      void saved();
    };
  }
}
