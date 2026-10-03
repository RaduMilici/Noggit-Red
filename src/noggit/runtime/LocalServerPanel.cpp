#include "LocalServerPanel.hpp"
#include "RuntimeManager.hpp"
#include <QDockWidget>
#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QCheckBox>
#include <QSettings>
#include <array>
namespace Noggit::Runtime {
void addLocalServerPanel(QMainWindow* window)
{
  auto manager = RuntimeManager::instance();
  if (!manager) return;
  auto dock = new QDockWidget(QObject::tr("LOCAL SERVER"), window);
  dock->setObjectName("localServerDock");
  dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
  auto panel = new QWidget(dock);
  auto layout = new QVBoxLayout(panel);
  auto form = new QFormLayout;
  std::array<QLabel*, 3> labels;
  QStringList names{QObject::tr("Database"), QObject::tr("Realm Server"), QObject::tr("World Server")};
  for (int i = 0; i < 3; ++i) { labels[i] = new QLabel(panel); form->addRow(names[i], labels[i]); }
  layout->addLayout(form);
  auto error = new QLabel(panel); error->setWordWrap(true); error->setTextFormat(Qt::PlainText);
  layout->addWidget(error);
  auto start = new QPushButton(QObject::tr("Start"), panel);
  auto stop = new QPushButton(QObject::tr("Stop"), panel);
  auto restart = new QPushButton(QObject::tr("Restart"), panel);
  for (auto b : {start, stop, restart}) layout->addWidget(b);
  QObject::connect(start, &QPushButton::clicked, manager, &RuntimeManager::start);
  QObject::connect(stop, &QPushButton::clicked, manager, &RuntimeManager::stop);
  QObject::connect(restart, &QPushButton::clicked, manager, &RuntimeManager::restart);
  auto autostart = new QCheckBox(QObject::tr("Start with Noggit"), panel);
  QString settingsPath = manager->root() + "/Workspace/runtime.ini";
  autostart->setChecked(QSettings(settingsPath, QSettings::IniFormat).value("autostart", true).toBool());
  QObject::connect(autostart, &QCheckBox::toggled, panel, [settingsPath](bool enabled) {
    QSettings(settingsPath, QSettings::IniFormat).setValue("autostart", enabled);
  });
  layout->addWidget(autostart);
  auto refresh = [manager, labels, error, start] {
    for (int i = 0; i < 3; ++i) labels[i]->setText(manager->status(i));
    error->setText(manager->error()); error->setVisible(!manager->error().isEmpty());
    start->setEnabled(!manager->active() && !manager->stopping());
  };
  QObject::connect(manager, &RuntimeManager::changed, panel, refresh); refresh();
  dock->setWidget(panel); window->addDockWidget(Qt::RightDockWidgetArea, dock);
}
}
