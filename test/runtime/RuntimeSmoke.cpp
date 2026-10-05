#include "../../src/noggit/runtime/RuntimeManager.hpp"
#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc != 2) return 2;
  Noggit::Runtime::RuntimeManager manager(QString::fromLocal8Bit(argv[1]));
  bool running = false;
  QObject::connect(&manager, &Noggit::Runtime::RuntimeManager::changed, &app, [&] {
    QTextStream(stdout) << manager.status(0) << " / " << manager.status(1) << " / " << manager.status(2) << " " << manager.error() << Qt::endl;
    if (!manager.error().isEmpty()) app.exit(1);
    if (!running && manager.status(2) == "Running") {
      running = true; QTimer::singleShot(2000, &manager, &Noggit::Runtime::RuntimeManager::stop);
    }
    if (running && manager.status(0) == "Stopped" && !manager.stopping()) app.quit();
  });
  QObject::connect(&app, &QCoreApplication::aboutToQuit, &manager, &Noggit::Runtime::RuntimeManager::shutdown);
  QTimer::singleShot(0, &manager, &Noggit::Runtime::RuntimeManager::start);
  QTimer::singleShot(150000, &app, [&] { app.exit(3); });
  return app.exec();
}
