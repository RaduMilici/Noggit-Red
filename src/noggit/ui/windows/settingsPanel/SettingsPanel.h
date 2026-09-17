// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <qt-color-widgets/color_selector.hpp>

#include <QtCore/QSettings>
#include <QMainWindow>
#include <QLineEdit>
#include <QtWidgets/QCheckBox>
#include <ui_SettingsPanel.h>

#include <vector>
#include <tuple>


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
    public:
      settings(QWidget* parent = nullptr);
      void discard_changes();
      void save_changes();

    signals:
      void saved();
    };
  }
}
