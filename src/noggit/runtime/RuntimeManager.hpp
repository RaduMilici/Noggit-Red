#pragma once
#include <QObject>
#include <QProcess>
#include "RuntimeProcess.hpp"
#include <QTimer>
#include <QElapsedTimer>
#include <QLockFile>
#include <array>
#include <memory>

namespace Noggit::Runtime
{
class GameDataDownload;
// Owns only children launched by this instance; never discovers or kills system services.
class RuntimeManager final : public QObject
{
  Q_OBJECT
public:
  explicit RuntimeManager(QString installationRoot, QObject* parent = nullptr);
  ~RuntimeManager() override;
  static RuntimeManager* instance();
  QString status(int service) const { return _status.at(service); }
  QString error() const { return _error; }
  bool active() const { return _active; }
  bool stopping() const { return _stopping >= 0; }
  bool saveForTest();
  QString root() const { return _root; }
public slots:
  void start();
  void stop();
  void restart();
  void shutdown();
  void testLocally();
signals:
  void changed();
  void gameDataProgress(qint64 received, qint64 total);
  void aboutToShutdown();
  void beforeLocalTest(bool* proceed);
private:
  QString path(QString const& relative) const;
  QString executable(QString const& relative) const;
  bool prepare();
  void launch(int index);
  void poll();
  void fail(QString const& message);
  void stopNext();
  void state(int index, QString const& value);
  QString _root, _error;
  std::array<RuntimeProcess, 3> _processes;
  std::array<QString, 3> _status{{"Stopped", "Stopped", "Stopped"}};
  RuntimeProcess _probe;
  std::unique_ptr<GameDataDownload> _gameData;
  QTimer _timer;
  QElapsedTimer _deadline;
  std::unique_ptr<QLockFile> _lock;
  int _starting = -1, _stopping = -1;
  bool _testWhenRunning = false;
  bool _active = false, _restart = false, _shuttingDown = false;
};
}
