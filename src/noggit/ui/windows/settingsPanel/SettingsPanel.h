// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <qt-color-widgets/color_selector.hpp>

#include <QtCore/QSettings>
#include <QMainWindow>
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
      ::Ui::SettingsPanel* ui;
      // Persistent render-feature toggles shown on the Graphics tab: {settings key, checkbox, default}.
      // Mirror the live View-menu/toolbar switches but survive restarts (applied by MapView at load).
      std::vector<std::tuple<QString, QCheckBox*, bool>> _render_toggles;
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
