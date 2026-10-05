#include "Ssh.hpp"
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <algorithm>
namespace Noggit::Creator::Ssh {
namespace {
QRegularExpression const hostPattern("^[A-Za-z0-9][A-Za-z0-9._:-]*$");
QRegularExpression const userPattern("^[A-Za-z0-9_][A-Za-z0-9._-]*$");
QString program(QString const& name) {
  auto bundled = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../Runtime/OpenSSH/" + name);
#ifdef Q_OS_WIN
  bundled += ".exe";
#endif
  if (QFileInfo(bundled).isExecutable()) return bundled;
  auto path = QStandardPaths::findExecutable(name);
  if (path.isEmpty()) throw Error(Problem::NotInstalled, explain(Problem::NotInstalled, {}));
  return path;
}
// ssh splits option values like its config files: quote paths, and use '/' so '\' is never an escape.
QString configPath(QString const& path) { return '"' + QDir::fromNativeSeparators(path) + '"'; }
QString forwardHost(QString const& host) { return host.contains(':') ? '[' + host + ']' : host; }
void check(Target const& target) {
  if (auto problem = validate(target); !problem.isEmpty()) throw Error(Problem::BadSettings, problem);
}
}
QString validate(Target const& t) {
  if (!hostPattern.match(t.host).hasMatch()) return "Enter the SSH host as a name or IP address, without spaces or a user name.";
  if (!userPattern.match(t.user).hasMatch()) return "Enter the SSH user name (letters, digits, '.', '_' or '-').";
  if (t.port < 1 || t.port > 65535) return "Enter an SSH port between 1 and 65535.";
  if (t.knownHosts.isEmpty()) return "Noggit's known hosts file is not set.";
  if (t.keyPath.isEmpty()) return {};
  QFileInfo key(t.keyPath);
  if (!key.isFile()) return "The private key file does not exist: " + QDir::toNativeSeparators(t.keyPath);
  if (key.suffix() == "pub" || key.suffix() == "ppk")
    return "Choose the private key file in OpenSSH format, not the .pub or PuTTY .ppk file.";
  QFile file(t.keyPath);
  if (!file.open(QIODevice::ReadOnly)) return "The private key file cannot be read.";
  if (!file.readLine(200).contains("PRIVATE KEY")) return "That file is not an OpenSSH private key.";
#ifndef Q_OS_WIN
  if (key.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther))
    return "The private key can be read by other users, so ssh refuses it. Run: chmod 600 " + t.keyPath;
#endif
  return {};
}
QStringList arguments(Target const& t, QStringList const& options) {
  QStringList args{"-F", "none", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
                   "-o", "UserKnownHostsFile=" + configPath(t.knownHosts) + " ~/.ssh/known_hosts",
                   "-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3",
                   "-p", QString::number(t.port)};
  if (!t.keyPath.isEmpty()) args << "-i" << QDir::fromNativeSeparators(t.keyPath) << "-o" << "IdentitiesOnly=yes";
  return args << options << "--" << t.user + "@" + t.host;
}
Problem classify(QString const& output) {
  auto text = output.toLower();
  auto any = [&](std::initializer_list<char const*> needles) {
    return std::any_of(needles.begin(), needles.end(), [&](char const* n) { return text.contains(QLatin1String(n)); });
  };
  if (any({"remote host identification has changed"})) return Problem::HostChanged;
  if (any({"host key verification failed", "host key is known for"})) return Problem::HostUnknown;
  if (any({"unprotected private key file", "load key", "invalid format", "bad permissions", "no such identity"})) return Problem::KeyFile;
  if (any({"permission denied", "too many authentication failures"})) return Problem::Auth;
  if (any({"open failed", "administratively prohibited", "cannot listen", "address already in use", "forwarding failed"})) return Problem::Forward;
  if (any({"could not resolve hostname", "connection refused", "connection timed out", "no route to host",
           "network is unreachable", "operation timed out", "connection closed by", "connection reset"})) return Problem::Unreachable;
  return Problem::Failed;
}
QString explain(Problem problem, Target const& t) {
  switch (problem) {
    case Problem::NotInstalled: return "The OpenSSH client (ssh) was not found. Install it (Ubuntu: sudo apt install openssh-client; "
                                       "Windows: the optional feature \"OpenSSH Client\").";
    case Problem::KeyFile: return "ssh cannot use the private key. Check that it is the OpenSSH private key, only readable by you "
                                  "(chmod 600), and if it has a passphrase, unlock it first with: ssh-add " + t.keyPath;
    case Problem::HostUnknown: return "The server's host key has not been verified yet. Use Test Connection in Production Server "
                                      "settings to check its fingerprint and trust it.";
    case Problem::HostChanged: return "The server's host key has CHANGED since it was trusted. This can mean someone is "
                                      "intercepting the connection. Do not continue: ask the server's administrator. Only if they "
                                      "confirm the server was rebuilt, forget the old key and verify the new fingerprint.";
    case Problem::Auth: return "The server did not accept the key for user \"" + t.user + "\". Check the user name, and that the "
                               "public key is in that user's authorized_keys on the server.";
    case Problem::Unreachable: return "Could not reach " + t.host + ":" + QString::number(t.port) + ". Check the address and port, "
                                      "that the server is running, and that its firewall allows SSH from your network.";
    case Problem::Forward: return "SSH works, but the server could not open the connection to the database. Check the database "
                                  "host and port as seen from the server, and that the SSH key may forward to it.";
    case Problem::Timeout: return "The server did not answer in time.";
    case Problem::BadSettings: case Problem::Failed: case Problem::None: break;
  }
  return "ssh failed.";
}
Tunnel::Tunnel(Target const& target, QString const& remoteHost, unsigned remotePort, int timeoutMs) {
  check(target);
  if (!hostPattern.match(remoteHost).hasMatch() || remotePort < 1 || remotePort > 65535)
    throw Error(Problem::BadSettings, "Enter the database host and port as seen from the SSH server (usually 127.0.0.1 and 3306).");
  auto ssh = program("ssh");
  {
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) throw Error(Problem::Forward, "No free local port for the database tunnel.");
    _port = probe.serverPort();
  }
  _process = std::make_unique<QProcess>();
  _process->setProcessChannelMode(QProcess::MergedChannels);
  auto forward = "127.0.0.1:" + QString::number(_port) + ":" + forwardHost(remoteHost) + ":" + QString::number(remotePort);
  _process->start(ssh, arguments(target, {"-N", "-o", "ExitOnForwardFailure=yes", "-L", forward}));
  if (!_process->waitForStarted(5000)) throw Error(Problem::NotInstalled, explain(Problem::NotInstalled, target));
  _process->closeWriteChannel();
  QElapsedTimer elapsed; elapsed.start();
  // ssh opens the local port only after logging in, so a connection means the tunnel is up.
  for (;;) {
    if (_process->state() == QProcess::NotRunning) {
      auto out = output(); auto problem = classify(out);
      throw Error(problem, problem == Problem::Failed ? "ssh stopped: " + out.trimmed().section('\n', -1) : explain(problem, target), out);
    }
    QTcpSocket socket; socket.connectToHost(QHostAddress::LocalHost, _port);
    if (socket.waitForConnected(250)) { socket.abort(); return; }
    if (elapsed.elapsed() > timeoutMs) {
      _process->kill(); _process->waitForFinished(2000);
      throw Error(Problem::Timeout, explain(Problem::Unreachable, target), output());
    }
    _process->waitForFinished(250);
  }
}
Tunnel::~Tunnel() {
  if (_process && _process->state() != QProcess::NotRunning) { _process->kill(); _process->waitForFinished(2000); }
}
QString Tunnel::output() {
  if (_process) _output += QString::fromLocal8Bit(_process->readAll());
  return _output;
}
Result run(Target const& target, QString const& command, int timeoutMs) {
  check(target);
  QProcess process; process.setProcessChannelMode(QProcess::MergedChannels);
  process.start(program("ssh"), arguments(target, {"-T"}) << command);
  if (!process.waitForStarted(5000)) throw Error(Problem::NotInstalled, explain(Problem::NotInstalled, target));
  process.closeWriteChannel();
  if (!process.waitForFinished(timeoutMs)) {
    process.kill(); process.waitForFinished(2000);
    throw Error(Problem::Timeout, "The command on the server did not finish within " + QString::number(timeoutMs / 1000) + " seconds.",
                QString::fromLocal8Bit(process.readAll()));
  }
  Result result{process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1, QString::fromLocal8Bit(process.readAll())};
  // 255 is ssh's own failure; a command exiting with 255 is told apart by ssh's messages.
  if (result.exitCode == 255)
    if (auto problem = classify(result.output); problem != Problem::Failed) throw Error(problem, explain(problem, target), result.output);
  return result;
}
QVector<HostKey> scanHostKeys(QString const& host, unsigned port, int timeoutMs) {
  if (!hostPattern.match(host).hasMatch()) throw Error(Problem::BadSettings, "Enter the SSH host as a name or IP address.");
  QProcess process;
  process.start(program("ssh-keyscan"), {"-T", "10", "-p", QString::number(port), "--", host});
  if (!process.waitForStarted(5000) || !process.waitForFinished(timeoutMs)) {
    process.kill(); process.waitForFinished(2000);
    throw Error(Problem::Unreachable, "Could not read the server's host key from " + host + ".");
  }
  QVector<HostKey> keys;
  for (auto const& line : QString::fromLocal8Bit(process.readAllStandardOutput()).split('\n')) {
    auto parts = line.trimmed().split(QRegularExpression("\\s+"));
    if (parts.size() < 3 || parts[0].startsWith('#')) continue;
    keys.push_back({line.trimmed(), parts[1], fingerprint(parts[2].toLatin1())});
  }
  if (keys.isEmpty())
    throw Error(Problem::Unreachable, "Could not read the server's host key from " + host + ":" + QString::number(port) + ".",
                QString::fromLocal8Bit(process.readAllStandardError()));
  return keys;
}
QString fingerprint(QByteArray const& base64Blob) {
  auto digest = QCryptographicHash::hash(QByteArray::fromBase64(base64Blob), QCryptographicHash::Sha256);
  return "SHA256:" + QString::fromLatin1(digest.toBase64(QByteArray::OmitTrailingEquals));
}
void trust(QString const& knownHosts, QVector<HostKey> const& keys) {
  QDir().mkpath(QFileInfo(knownHosts).absolutePath());
  QByteArray existing;
  if (QFile file(knownHosts); file.open(QIODevice::ReadOnly)) existing = file.readAll();
  if (!existing.isEmpty() && !existing.endsWith('\n')) existing += '\n';
  for (auto const& key : keys) existing += key.line.toUtf8() + '\n';
  QSaveFile file(knownHosts);
  if (!file.open(QIODevice::WriteOnly) || file.write(existing) != existing.size() || !file.commit())
    throw Error(Problem::Failed, "Cannot save the trusted host key.");
}
void forget(QString const& knownHosts, QString const& host, unsigned port) {
  QFile file(knownHosts);
  if (!file.open(QIODevice::ReadOnly)) return;
  auto name = port == 22 ? host : "[" + host + "]:" + QString::number(port);
  QByteArray kept;
  for (auto const& line : file.readAll().split('\n'))
    if (!line.trimmed().isEmpty() && !QString::fromUtf8(line).section(' ', 0, 0).split(',').contains(name)) kept += line + '\n';
  file.close();
  QSaveFile out(knownHosts);
  if (!out.open(QIODevice::WriteOnly) || out.write(kept) != kept.size() || !out.commit())
    throw Error(Problem::Failed, "Cannot update Noggit's known hosts file.");
}
}
