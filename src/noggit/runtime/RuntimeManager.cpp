#include "RuntimeManager.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSaveFile>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QRegularExpression>
#include <QProcessEnvironment>
#include <stdexcept>

namespace Noggit::Runtime
{
namespace {
constexpr int ports[] = {13306, 13724, 18085};
QString const programs[] = {"Runtime/MariaDB/bin/mariadbd", "Runtime/realmd/realmd", "Runtime/mangosd/mangosd"};
bool writeFile(QString const& name, QByteArray const& data)
{
  QSaveFile file(name);
  return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}
void copyTree(QString const& source, QString const& target)
{
  QDirIterator it(source, QDir::AllEntries | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
  if (!QDir().mkpath(target)) throw std::runtime_error("Cannot create database directory");
  while (it.hasNext()) {
    QString from = it.next(), to = target + '/' + QDir(source).relativeFilePath(from);
    if (it.fileInfo().isSymLink()) throw std::runtime_error("Database seed contains a symbolic link");
    if (it.fileInfo().isDir()) {
      if (!QDir().mkpath(to)) throw std::runtime_error("Cannot create database directory");
    } else if (!QFile::copy(from, to)) throw std::runtime_error("Cannot copy database seed");
  }
}
}
RuntimeManager* RuntimeManager::instance()
{
  return qApp->findChild<RuntimeManager*>("localRuntime", Qt::FindDirectChildrenOnly);
}
RuntimeManager::RuntimeManager(QString root, QObject* parent)
  : QObject(parent), _root(QDir(root).absolutePath())
{
  setObjectName("localRuntime");
  if (QFileInfo::exists(path("Runtime/creator-runtime.json")))
    qApp->setProperty("creatorRuntimeManaged", true);
  auto environment = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_LINUX
  environment.insert("LD_LIBRARY_PATH", path("Runtime/lib") + ':' + path("Runtime/MariaDB/lib"));
#endif
  for (auto& process : _processes) process.setProcessEnvironment(environment);
  _probe.setProcessEnvironment(environment);
  _timer.setInterval(250);
  connect(&_timer, &QTimer::timeout, this, &RuntimeManager::poll);
  for (int i = 0; i < 3; ++i) {
    auto& p = _processes[i];
    connect(&p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
      [this, i](int code, QProcess::ExitStatus) {
        if (_shuttingDown) return;
        if (_stopping >= 0) return;
        if (_active) fail(QString("%1 exited (code %2). See Workspace/logs.").arg(programs[i]).arg(code));
      });
    connect(&p, &QProcess::errorOccurred, this, [this, i](QProcess::ProcessError e) {
      if (!_shuttingDown && _stopping < 0 && e == QProcess::FailedToStart)
        fail(programs[i] + ": " + _processes[i].errorString());
    });
  }
  connect(&_probe, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
    [this](int code, QProcess::ExitStatus exit) {
      _probe.readAll();
      if (_starting == 0 && _stopping < 0 && code == 0 && exit == QProcess::NormalExit) {
        qApp->setProperty("creatorDatabaseReady", true);
        state(0, "Running"); launch(1);
      }
    });
}
RuntimeManager::~RuntimeManager() { shutdown(); }
QString RuntimeManager::path(QString const& relative) const { return QDir(_root).filePath(relative); }
QString RuntimeManager::executable(QString const& relative) const {
#ifdef Q_OS_WIN
  return path(relative + ".exe");
#else
  return path(relative);
#endif
}
void RuntimeManager::state(int i, QString const& value) { _status[i] = value; emit changed(); }
bool RuntimeManager::prepare()
{
  for (auto const& program : programs)
    if (!QFileInfo(executable(program)).isExecutable()) { _error = "Runtime bundle is missing: " + program; return false; }
  if (!QFileInfo(executable("Runtime/MariaDB/bin/mariadb")).isExecutable()) {
    _error = "Runtime bundle is missing the MariaDB client."; return false;
  }
  for (int port : ports) {
    QTcpServer reservation;
    if (!reservation.listen(QHostAddress::LocalHost, port)) {
      _error = QString("Local port %1 is in use. Stop the other local server and retry.").arg(port); return false;
    }
  }
  if (!QDir().mkpath(path("Workspace/logs")) || !QDir().mkpath(path("Database"))) {
    _error = "Creator installation is not writable."; return false;
  }
  _lock = std::make_unique<QLockFile>(path("Database/runtime.lock"));
  if (!_lock->tryLock(0)) { _error = "This runtime is already managed by another Creator instance."; return false; }
  try {
    if (!QFileInfo::exists(path("Database/data"))) {
      if (!QFileInfo::exists(path("Runtime/DatabaseSeed/mysql")))
        throw std::runtime_error("Runtime bundle is missing its initialized database seed");
      // Interrupted first-run copies are discarded; an existing live database is never overwritten.
      QDir(path("Database/initializing")).removeRecursively();
      copyTree(path("Runtime/DatabaseSeed"), path("Database/initializing"));
      if (!QDir().rename(path("Database/initializing"), path("Database/data")))
        throw std::runtime_error("Cannot activate initialized database");
    }
    auto put = [this](QString const& name, QByteArray const& data) {
      if (!writeFile(path(name), data)) throw std::runtime_error("Cannot write runtime configuration");
    };
    put("Workspace/mariadb.cnf", "[mysqld]\nbasedir=../Runtime/MariaDB\ndatadir=../Database/data\nsocket=mariadb.sock\npid-file=mariadb.pid\nbind-address=127.0.0.1\nport=13306\nskip-name-resolve\nconsole\n");
    put("Workspace/client.cnf", "[client]\nprotocol=tcp\nhost=127.0.0.1\nport=13306\nuser=creator\npassword=creator-local\n");
    QFile::setPermissions(path("Workspace/client.cnf"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    // Compiled-in Tortoise modules load configs next to the generated world config.
    // Seed defaults once; preserve local module settings on subsequent starts.
    QDir moduleDefaults(path("Runtime/mangosd/modules"));
    for (QString const& file : moduleDefaults.entryList({"*.conf.dist"}, QDir::Files)) {
      QString destination = path("Workspace/modules/") + file.left(file.size() - 5);
      if (!QFileInfo::exists(destination)) {
        if (!QDir().mkpath(path("Workspace/modules"))
            || !QFile::copy(moduleDefaults.filePath(file), destination))
          throw std::runtime_error("Cannot initialize bundled module configuration");
      }
    }
    for (QString name : {QString("realmd"), QString("mangosd")}) {
      QFile source(path("Runtime/" + name + '/' + name + ".conf.dist"));
      if (!source.open(QIODevice::ReadOnly)) throw std::runtime_error("Runtime bundle is missing matching server config templates");
      QString config = QString::fromUtf8(source.readAll());
      auto set = [&config](QString key, QString value) {
        QRegularExpression re("^\\s*" + QRegularExpression::escape(key) + "\\s*=.*$", QRegularExpression::MultilineOption);
        if (config.contains(re)) config.replace(re, key + " = " + value);
        else config += '\n' + key + " = " + value + '\n';
      };
      set(name == "realmd" ? "LoginDatabaseInfo" : "LoginDatabase.Info", "\"127.0.0.1;13306;creator;creator-local;realmd\"");
      set("BindIP", "\"127.0.0.1\"");
      set("LogsDir", "\"logs\"");
      if (name == "realmd") set("RealmServerPort", "13724");
      else {
        set("WorldDatabase.Info", "\"127.0.0.1;13306;creator;creator-local;mangos\"");
        set("CharacterDatabase.Info", "\"127.0.0.1;13306;creator;creator-local;characters\"");
        set("LogsDatabase.Info", "\"127.0.0.1;13306;creator;creator-local;logs\"");
        set("DataDir", "\"../Runtime/mangosd/data\"");
        set("WorldServerPort", "18085"); set("RealmID", "1");
        set("Database.AutoUpdate.Enabled", "0");
        set("HttpApi.Enable", "0");
        set("Console.Enable", "1"); set("Ra.Enable", "0"); set("SOAP.Enabled", "0");
      }
      put("Workspace/" + name + ".conf", config.toUtf8());
    }
  } catch (std::exception const& e) { _error = QString::fromUtf8(e.what()); return false; }
  return true;
}
void RuntimeManager::start()
{
  if (_active || _stopping >= 0 || _shuttingDown) return;
  _error.clear();
  if (!prepare()) { _lock.reset(); emit changed(); return; }
  qApp->setProperty("creatorRuntimeManaged", true);
  _active = true; _timer.start(); launch(0);
}
void RuntimeManager::launch(int i)
{
  _starting = i; _deadline.restart(); state(i, "Starting");
  auto& p = _processes[i];
  p.setWorkingDirectory(path("Workspace"));
  p.setProcessChannelMode(QProcess::MergedChannels);
  p.setStandardOutputFile(path("Workspace/logs/" + QString::number(i) + ".log"), QIODevice::Append);
  p.start(executable(programs[i]), i == 0
    ? QStringList{"--defaults-file=" + path("Workspace/mariadb.cnf")}
    : QStringList{"-c", path(i == 1 ? "Workspace/realmd.conf" : "Workspace/mangosd.conf")});
}
void RuntimeManager::poll()
{
  if (_stopping >= 0) {
    auto& p = _processes[_stopping];
    if (p.state() == QProcess::NotRunning) { state(_stopping, "Stopped"); --_stopping; stopNext(); }
    else if (_deadline.elapsed() > 15000) { p.kill(); }
    return;
  }
  if (_starting < 0) return;
  if (_deadline.elapsed() > 120000) { fail("Server startup timed out. See Workspace/logs."); return; }
  if (_processes[_starting].state() != QProcess::Running) return;
  if (_starting == 0) {
    if (_probe.state() == QProcess::NotRunning) {
      _probe.setWorkingDirectory(path("Workspace"));
      _probe.setProcessChannelMode(QProcess::MergedChannels);
      _probe.start(executable("Runtime/MariaDB/bin/mariadb"),
        {"--defaults-file=" + path("Workspace/client.cnf"), "--connect-timeout=1", "--batch", "--execute=SELECT 1", "mangos"});
    }
  } else {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, ports[_starting]);
    if (socket.waitForConnected(10)) {
      int i = _starting; state(i, "Running");
      if (i == 1) launch(2); else _starting = -1;
    }
  }
}
void RuntimeManager::fail(QString const& message) { _error = message; _restart = false; stop(); }
void RuntimeManager::stop()
{
  _restart = false;
  if (_stopping >= 0) return;
  qApp->setProperty("creatorDatabaseReady", false);
  _starting = -1; _active = false;
  _probe.kill(); _probe.waitForFinished(1000);
  _stopping = 2; _timer.start(); stopNext();
}
void RuntimeManager::stopNext()
{
  if (_stopping < 0) {
    _timer.stop(); _lock.reset(); emit changed();
    if (_restart) { _restart = false; start(); }
    return;
  }
  auto& p = _processes[_stopping];
  _deadline.restart();
  if (p.state() != QProcess::NotRunning) {
    state(_stopping, "Stopping");
    if (_stopping == 2) p.write("server shutdown 0\n");
    else if (_stopping == 0) {
      _probe.start(executable("Runtime/MariaDB/bin/mariadb"),
        {"--defaults-file=" + path("Workspace/client.cnf"), "--connect-timeout=1", "--execute=SHUTDOWN"});
    } else p.terminate();
  }
}
void RuntimeManager::restart() { if (_shuttingDown) return; stop(); _restart = true; }
void RuntimeManager::shutdown()
{
  _shuttingDown = true; _restart = false; _timer.stop();
  if (qApp) qApp->setProperty("creatorDatabaseReady", false);
  _probe.kill(); _probe.waitForFinished(1000);
  for (int i = 2; i >= 0; --i) {
    auto& p = _processes[i];
    if (p.state() == QProcess::NotRunning) continue;
    if (i == 2) { p.write("server shutdown 0\n"); p.waitForBytesWritten(1000); }
    else if (i == 0) {
      _probe.start(executable("Runtime/MariaDB/bin/mariadb"),
        {"--defaults-file=" + path("Workspace/client.cnf"), "--connect-timeout=1", "--execute=SHUTDOWN"});
      if (!_probe.waitForFinished(3000)) { _probe.kill(); _probe.waitForFinished(1000); }
    } else p.terminate();
    if (!p.waitForFinished(15000)) { p.kill(); p.waitForFinished(3000); }
  }
  _lock.reset();
}
}
