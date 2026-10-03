#include "../../src/noggit/runtime/RuntimeManager.hpp"
#include "../../src/noggit/runtime/LocalServerPanel.hpp"
#include <QApplication>
#include <QMainWindow>
#include <QToolButton>
#include <QMenu>
#include <QDockWidget>
#include <QTemporaryDir>
#include <QTextStream>
int main(int argc,char** argv) {
  QApplication app(argc,argv);QTemporaryDir root;
  Noggit::Runtime::RuntimeManager runtime(root.path(),&app);
  QMainWindow window;window.resize(640,480);
  Noggit::Runtime::addLocalServerPanel(&window);
  Noggit::Runtime::addLocalServerPanel(&window);
  auto lights=window.findChildren<QToolButton*>("localServerIndicator");
  if(lights.size()!=1||!lights[0]->menu()||lights[0]->menu()->isVisible())return 1;
  if(window.findChild<QDockWidget*>("localServerDock"))return 2;
  if(!lights[0]->toolTip().contains("Database: Stopped")||lights[0]->icon().isNull())return 3;
  window.show();app.processEvents();
  lights[0]->menu()->popup(lights[0]->mapToGlobal(QPoint(0,0)));app.processEvents();
  if(!lights[0]->menu()->isVisible())return 4;
  if(argc==2)lights[0]->menu()->grab().save(QString::fromLocal8Bit(argv[1]));
  lights[0]->menu()->hide();
  QTextStream(stdout)<<"Compact runtime indicator checks passed\n";
  return 0;
}
