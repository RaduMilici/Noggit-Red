#include "ProductionProfile.hpp"
#include <noggit/runtime/RuntimeManager.hpp>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
QString workspace() { return Runtime::RuntimeManager::instance()->root() + "/Workspace"; }
QHash<QString, QString>& session() { static QHash<QString, QString> passwords; return passwords; }
QString account(ProductionProfile const& p) { return p.dbUser + "@" + p.sshHost + ":" + QString::number(p.sshPort) + "/" + p.database; }
QStringList attributes(ProductionProfile const& p) { return {"service", "noggit-creator-production", "account", account(p)}; }
QString secretTool() { return QStandardPaths::findExecutable("secret-tool"); }
}
QString ProductionProfile::describe() const { return database + " on " + sshHost; }
QString ProductionProfile::validate() const {
  if (auto problem = Ssh::validate(ssh()); !problem.isEmpty()) return problem;
  if (!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9._:-]*$").match(dbHost).hasMatch())
    return "Enter the database host as the SSH server sees it (usually 127.0.0.1).";
  if (dbPort < 1 || dbPort > 65535) return "Enter a database port between 1 and 65535.";
  if (!QRegularExpression("^[A-Za-z0-9_$]{1,64}$").match(database).hasMatch()) return "Enter the world database name (for tortoise-deploy: tw_world).";
  if (dbUser.isEmpty()) return "Enter the database user name.";
  if (restartCommand.contains('\n') || restartCommand.contains('\r')) return "Write the restart command on one line.";
  return {};
}
Ssh::Target ProductionProfile::ssh() const { return {sshHost, sshUser, keyPath, knownHostsFile(), sshPort}; }
Endpoint ProductionProfile::endpoint(unsigned localPort, QString const& password) const {
  return {"127.0.0.1", localPort, dbUser, password, database, 15};
}
QString ProductionProfile::knownHostsFile() { return workspace() + "/ssh/known_hosts"; }
ProductionProfile ProductionProfile::load() {
  QSettings settings(workspace() + "/runtime.ini", QSettings::IniFormat);
  settings.beginGroup("production");
  ProductionProfile p;
  p.sshHost = settings.value("sshHost").toString();
  p.sshPort = settings.value("sshPort", p.sshPort).toUInt();
  p.sshUser = settings.value("sshUser").toString();
  p.keyPath = settings.value("keyPath").toString();
  p.dbHost = settings.value("dbHost", p.dbHost).toString();
  p.dbPort = settings.value("dbPort", p.dbPort).toUInt();
  p.database = settings.value("database", p.database).toString();
  p.dbUser = settings.value("dbUser").toString();
  p.passwordMode = settings.value("password").toString() == "keyring" ? PasswordMode::Keyring : PasswordMode::Ask;
  p.restartCommand = settings.value("restartCommand").toString();
  return p;
}
void ProductionProfile::save() const {
  QSettings settings(workspace() + "/runtime.ini", QSettings::IniFormat);
  settings.beginGroup("production");
  settings.setValue("sshHost", sshHost); settings.setValue("sshPort", sshPort);
  settings.setValue("sshUser", sshUser); settings.setValue("keyPath", keyPath);
  settings.setValue("dbHost", dbHost); settings.setValue("dbPort", dbPort);
  settings.setValue("database", database); settings.setValue("dbUser", dbUser);
  settings.setValue("password", passwordMode == PasswordMode::Keyring ? "keyring" : "ask");
  settings.setValue("restartCommand", restartCommand);
  settings.sync();
  if (settings.status() != QSettings::NoError) throw std::runtime_error("Cannot save the production server settings.");
}
bool PasswordStore::keyringAvailable() { return !secretTool().isEmpty(); }
QString PasswordStore::keyringHint() {
#ifdef Q_OS_LINUX
  return "Install secret-tool to save it in your system keyring (Ubuntu: sudo apt install libsecret-tools).";
#else
  return "Saving in the system keyring is not available on this system yet.";
#endif
}
QString PasswordStore::lookup(ProductionProfile const& profile) {
  if (auto it = session().find(account(profile)); it != session().end()) return *it;
  if (profile.passwordMode != ProductionProfile::PasswordMode::Keyring || !keyringAvailable()) return {};
  QProcess process; process.start(secretTool(), QStringList{"lookup"} + attributes(profile));
  if (!process.waitForFinished(10000) || process.exitCode() != 0) return {};
  auto password = QString::fromUtf8(process.readAllStandardOutput());
  if (password.endsWith('\n')) password.chop(1);
  if (!password.isEmpty()) session()[account(profile)] = password;
  return password;
}
void PasswordStore::store(ProductionProfile const& profile, QString const& password) {
  session()[account(profile)] = password;
  if (profile.passwordMode != ProductionProfile::PasswordMode::Keyring) return;
  if (!keyringAvailable()) throw std::runtime_error(keyringHint().toStdString());
  // The password goes through stdin, never the command line.
  QProcess process;
  process.start(secretTool(), QStringList{"store", "--label=Noggit Creator production database"} + attributes(profile));
  process.write(password.toUtf8()); process.closeWriteChannel();
  if (!process.waitForFinished(30000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    throw std::runtime_error("The system keyring did not save the password. It is remembered until Noggit closes.");
}
void PasswordStore::forget(ProductionProfile const& profile) {
  session().remove(account(profile));
  if (!keyringAvailable()) return;
  QProcess process; process.start(secretTool(), QStringList{"clear"} + attributes(profile));
  process.waitForFinished(10000);
}
}
