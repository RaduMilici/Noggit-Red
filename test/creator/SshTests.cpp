#include <noggit/creator/Ssh.hpp>
#include <noggit/creator/ChangeTracker.hpp>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTextStream>
#include <stdexcept>
using namespace Noggit::Creator;
namespace {
void check(bool condition, char const* message) { if (!condition) throw std::runtime_error(message); }
Ssh::Target target() { return {"play.example.com", "noggit-sync", {}, "/tmp/noggit known_hosts", 2222}; }
void validation() {
  check(Ssh::validate(target()).isEmpty(), "A valid target was rejected");
  for (auto host : {"-oProxyCommand=touch x", "a b", "user@host", ""}) { auto t = target(); t.host = host; check(!Ssh::validate(t).isEmpty(), "An unsafe host was accepted"); }
  for (auto user : {"-l", "a b", "x;y", ""}) { auto t = target(); t.user = user; check(!Ssh::validate(t).isEmpty(), "An unsafe user was accepted"); }
  auto t = target(); t.port = 0; check(!Ssh::validate(t).isEmpty(), "Port 0 was accepted");
  QTemporaryDir dir; t = target();
  t.keyPath = dir.path() + "/key.pub"; QFile pub(t.keyPath); pub.open(QIODevice::WriteOnly); pub.write("ssh-ed25519 AAAA t\n"); pub.close();
  check(!Ssh::validate(t).isEmpty(), "A .pub file was accepted as the private key");
  t.keyPath = dir.path() + "/key"; QFile key(t.keyPath); key.open(QIODevice::WriteOnly); key.write("-----BEGIN OPENSSH PRIVATE KEY-----\n"); key.close();
  key.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  check(Ssh::validate(t).isEmpty(), "A private key readable only by its owner was rejected");
#ifndef Q_OS_WIN
  key.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadOther);
  check(Ssh::validate(t).contains("chmod 600"), "A world-readable key was not reported");
#endif
}
void arguments() {
  auto t = target(); t.keyPath = "/home/me/.ssh/sync";
  auto args = Ssh::arguments(t, {"-N"});
  check(args.last() == "noggit-sync@play.example.com" && args[args.size() - 2] == "--", "The destination does not follow --");
  check(args.contains("BatchMode=yes") && args.contains("StrictHostKeyChecking=yes") && args.contains("IdentitiesOnly=yes"), "Hardening options missing");
  check(args.contains("UserKnownHostsFile=\"/tmp/noggit known_hosts\" ~/.ssh/known_hosts"), "Known hosts path is not quoted");
  check(args.indexOf("-F") >= 0 && args[args.indexOf("-F") + 1] == "none" && args[args.indexOf("-p") + 1] == "2222" && args[args.indexOf("-i") + 1] == t.keyPath, "Port, key or config handling wrong");
  check(args.indexOf("-N") < args.indexOf("--"), "Options after the destination");
}
void classification() {
  using P = Ssh::Problem;
  check(Ssh::classify("No ED25519 host key is known for [x]:22 and you have requested strict checking.\nHost key verification failed.") == P::HostUnknown, "Unknown host");
  check(Ssh::classify("@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! @\nHost key verification failed.") == P::HostChanged, "Changed host key");
  check(Ssh::classify("noggit@x: Permission denied (publickey).") == P::Auth, "Rejected key");
  check(Ssh::classify("WARNING: UNPROTECTED PRIVATE KEY FILE!") == P::KeyFile, "Key permissions");
  check(Ssh::classify("ssh: connect to host x port 22: Connection refused") == P::Unreachable, "Unreachable");
  check(Ssh::classify("channel 2: open failed: connect failed: Connection refused") == P::Forward, "Database not reachable from the server");
  check(Ssh::classify("something else") == P::Failed, "Unknown output");
}
void hostKeys() {
  check(Ssh::fingerprint("AAAAC3NzaC1lZDI1NTE5AAAAIJZM72UC7llWjb4zl6+stI9diAoywalCZ3ruS5MBBPIm") == "SHA256:2OD9aGqrPP8lrNi/NDZG78H3Ww7j9k11kxRcdnpBeP0",
        "Fingerprint differs from ssh-keygen -l");
  QTemporaryDir dir; auto file = dir.path() + "/ssh/known_hosts";
  Ssh::trust(file, {{"[play.example.com]:2222 ssh-ed25519 AAAA1", "ssh-ed25519", {}}, {"other.example.com ssh-ed25519 AAAA2", "ssh-ed25519", {}}});
  Ssh::forget(file, "play.example.com", 2222);
  QFile kept(file); kept.open(QIODevice::ReadOnly);
  check(kept.readAll() == "other.example.com ssh-ed25519 AAAA2\n", "Forgetting a host removed the wrong keys");
}
// Names go into comments of the generated SQL; a line break must not smuggle in a statement.
void commentSafety() {
  TrackedChange c; c.type = EntityType::Item; c.entity = 1000000; c.action = ChangeAction::Create;
  c.label = "Pelt\nDROP TABLE account; --";
  c.after = {{"item_template", QJsonArray{QJsonObject{{"entry", "1000000"}, {"name", "Pelt\nline"}}}}};
  auto statements = ChangeTracker::statements(ChangeTracker::sql({c}));
  for (auto const& s : statements) check(!s.startsWith("DROP"), "A label line break became a statement");
  check(statements.filter("REPLACE INTO `item_template`").size() == 1 && statements.filter("REPLACE INTO `item_template`")[0].contains("'Pelt\\nline'"),
        "A value line break split its statement");
}
}
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    validation(); arguments(); classification(); hostKeys(); commentSafety();
    QTextStream(stdout) << "SSH and sync tests passed\n";
    return 0;
  } catch (std::exception const& e) { QTextStream(stderr) << e.what() << Qt::endl; return 1; }
}
