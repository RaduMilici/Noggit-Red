#include "LocalServerPanel.hpp"
#include "RuntimeManager.hpp"
#include "ClientManager.hpp"
#include <noggit/creator/TestSessionService.hpp>
#include <QMessageBox>
#include <stdexcept>
#include <QToolButton>
#include <QStatusBar>
#include <QMenu>
#include <QWidgetAction>
#include <QPainter>
#include <QPixmap>
#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QCheckBox>
#include <QSettings>
#include <array>
#include <functional>
#include <QTimer>
namespace Noggit::Runtime {
void addLocalServerPanel(QMainWindow* window)
{
  auto manager = RuntimeManager::instance();
  if (!manager) return;
  if (window->findChild<QToolButton*>("localServerIndicator")) return;
  auto indicator = new QToolButton(window);
  indicator->setObjectName("localServerIndicator");
  indicator->setAccessibleName(QObject::tr("Local server controls"));
  indicator->setAutoRaise(true);
  indicator->setIconSize(QSize(38, 14));
  indicator->setFixedSize(48, 22);
  indicator->setPopupMode(QToolButton::InstantPopup);
  indicator->setStyleSheet("QToolButton::menu-indicator { image: none; }");
  auto menu = new QMenu(indicator);
  menu->setObjectName("localServerPopup");
  indicator->setMenu(menu);
  auto panel = new QWidget(menu);
  panel->setMinimumWidth(260);
  panel->setMaximumWidth(320);
  auto layout = new QVBoxLayout(panel);
  auto title = new QLabel(QObject::tr("LOCAL SERVER"), panel);
  layout->addWidget(title);
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
  auto session=Creator::TestSessionService::instance();
  QObject::connect(stop,&QPushButton::clicked,session,&Creator::TestSessionService::cancel);
  auto test = new QPushButton(QObject::tr("Test Local"), panel);
  auto production = new QPushButton(QObject::tr("Play Production"), panel);
  auto settings = new QPushButton(QObject::tr("Client Profiles…"), panel);
  auto cancel = new QPushButton(QObject::tr("Cancel Test"), panel);
  auto testStatus = new QLabel(panel);testStatus->setWordWrap(true);testStatus->setTextFormat(Qt::PlainText);
  for(auto button:{test,production,settings,cancel})layout->addWidget(button);
  layout->addWidget(testStatus);
  // Dialogs must not be opened from inside the popup: it keeps its input grab, and on Wayland
  // a native file chooser opened from there never receives input, blocking the editor. Close the
  // popup first and parent dialogs to the window.
  auto fromWindow=[menu,window](std::function<void()> action){menu->hide();QTimer::singleShot(0,window,std::move(action));};
  QObject::connect(test,&QPushButton::clicked,panel,[=]{fromWindow([window,session]{session->testLocal(window);});});
  QObject::connect(cancel,&QPushButton::clicked,session,&Creator::TestSessionService::cancel);
  QObject::connect(settings,&QPushButton::clicked,panel,[=]{fromWindow([window]{try{ClientManager::configure(window);}catch(std::exception const& e){QMessageBox::warning(window,"Client profiles",e.what());}});});
  QObject::connect(production,&QPushButton::clicked,panel,[=]{fromWindow([window,session]{
    if(session->busy()){QMessageBox::information(window,"Play Production","Cancel the pending local test before switching profiles.");return;}
    try {if(ClientManager::prepare(window,ClientManager::Profile::PlayProduction))ClientManager::launch(ClientManager::Profile::PlayProduction);}
    catch(std::exception const& e){QMessageBox::warning(window,"Play Production",e.what());}
  });});
  auto refreshTest=[session,testStatus,cancel,test,production,settings]{testStatus->setText(session->status());cancel->setEnabled(session->busy());test->setEnabled(!session->busy());production->setEnabled(!session->busy());settings->setEnabled(!session->busy());};
  QObject::connect(session,&Creator::TestSessionService::changed,panel,refreshTest);refreshTest();
  auto autostart = new QCheckBox(QObject::tr("Start with Noggit"), panel);
  QString settingsPath = manager->root() + "/Workspace/runtime.ini";
  autostart->setChecked(QSettings(settingsPath, QSettings::IniFormat).value("autostart", true).toBool());
  QObject::connect(autostart, &QCheckBox::toggled, panel, [settingsPath](bool enabled) {
    QSettings(settingsPath, QSettings::IniFormat).setValue("autostart", enabled);
  });
  layout->addWidget(autostart);
  auto refresh = [manager, labels, error, start, indicator, names, session] {
    qreal scale=indicator->devicePixelRatioF();
    QPixmap lights(QSize(38,14)*scale);lights.setDevicePixelRatio(scale);lights.fill(Qt::transparent);
    QPainter painter(&lights);painter.setRenderHint(QPainter::Antialiasing);
    QStringList description;
    for (int i = 0; i < 3; ++i) {
      auto state=manager->status(i);labels[i]->setText(state);description<<names[i]+": "+state;
      QColor color=state=="Running"?QColor("#69b582"):state=="Stopped"?QColor("#777d85"):QColor("#d5aa58");
      painter.setPen(manager->error().isEmpty()?QPen(Qt::NoPen):QPen(QColor("#df7777"),1));
      painter.setBrush(color);painter.drawEllipse(QRectF(2+i*12,3,8,8));
    }
    painter.end();indicator->setIcon(QIcon(lights));
    if(!manager->error().isEmpty())description<<manager->error();
    if(!session->status().isEmpty())description<<session->status();
    description<<QObject::tr("Click for server and client controls");
    indicator->setToolTip(description.join('\n'));indicator->setAccessibleDescription(description.join(". "));
    error->setText(manager->error()); error->setVisible(!manager->error().isEmpty());
    start->setEnabled(!manager->active() && !manager->stopping());
  };
  QObject::connect(manager, &RuntimeManager::changed, panel, refresh);
  QObject::connect(session, &Creator::TestSessionService::changed, panel, refresh); refresh();
  auto contents = new QWidgetAction(menu);contents->setDefaultWidget(panel);menu->addAction(contents);
  window->statusBar()->addPermanentWidget(indicator);
}
}
