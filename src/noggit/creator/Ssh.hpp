#pragma once
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <stdexcept>
namespace Noggit::Creator::Ssh {
// The system OpenSSH client, run with an argument list (never a local shell), in batch mode (it never
// prompts: a passphrase-protected key must be unlocked in ssh-agent) and with strict host-key checking
// against the user's known_hosts plus Noggit's own, which holds keys the user confirmed in Noggit.
// Blocking: call from a worker thread.
struct Target {
  QString host, user, keyPath, knownHosts; // knownHosts: Noggit's own file
  unsigned port = 22;
};
enum class Problem { None, NotInstalled, BadSettings, KeyFile, HostUnknown, HostChanged, Auth, Unreachable, Forward, Timeout, Failed };
struct Error : std::runtime_error {
  Error(Problem problem, QString const& message, QString details = {})
    : std::runtime_error(message.toStdString()), problem(problem), details(std::move(details)) {}
  Problem problem;
  QString details; // ssh's own output, for a details box
};
// Empty when the target is usable; otherwise what to fix. Host and user cannot be read as ssh options.
QString validate(Target const&);
QStringList arguments(Target const&, QStringList const& options);
// What went wrong, from ssh's stderr.
Problem classify(QString const& output);
QString explain(Problem, Target const&);
// A local port forwarded to `remoteHost`:`remotePort` as seen from the SSH server; closed on destruction.
class Tunnel {
public:
  Tunnel(Target const&, QString const& remoteHost, unsigned remotePort, int timeoutMs = 20000);
  ~Tunnel();
  unsigned localPort() const { return _port; }
  // ssh's output so far; tells why connections through the tunnel failed ("open failed: ...").
  QString output();
private:
  std::unique_ptr<QProcess> _process;
  unsigned _port = 0;
  QString _output;
};
struct Result { int exitCode = -1; QString output; };
// Runs `command` in the server's shell (or the key's forced command). Throws Error when ssh itself fails;
// a non-zero exit of the command is returned.
Result run(Target const&, QString const& command, int timeoutMs);
struct HostKey { QString line, type, fingerprint; };
// The server's host keys as offered right now. Only trust them after the user compared the fingerprint.
QVector<HostKey> scanHostKeys(QString const& host, unsigned port, int timeoutMs = 15000);
QString fingerprint(QByteArray const& base64Blob); // "SHA256:..." as ssh-keygen -l prints it
void trust(QString const& knownHosts, QVector<HostKey> const& keys);
// Removes the keys Noggit trusted for host:port (never touches ~/.ssh/known_hosts).
void forget(QString const& knownHosts, QString const& host, unsigned port);
}
