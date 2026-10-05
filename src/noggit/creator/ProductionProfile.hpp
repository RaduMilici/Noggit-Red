#pragma once
#include "Ssh.hpp"
#include "SqlConnection.hpp"
namespace Noggit::Creator {
// Where Sync to Production goes: an SSH login, the world database as seen from that server (reached
// through an SSH tunnel, so the database port never has to be open to the internet), and optionally
// the command that restarts the world server. Kept in the workspace's runtime.ini; the database
// password is never written there.
struct ProductionProfile {
  enum class PasswordMode { Ask, Keyring };
  QString sshHost, sshUser, keyPath;
  unsigned sshPort = 22;
  QString dbHost = "127.0.0.1", database = "tw_world", dbUser;
  unsigned dbPort = 3306;
  PasswordMode passwordMode = PasswordMode::Ask;
  QString restartCommand;

  bool configured() const { return !sshHost.isEmpty() && !sshUser.isEmpty() && !dbUser.isEmpty() && !database.isEmpty(); }
  QString describe() const; // "tw_world on host"
  QString validate() const; // empty when usable, otherwise what to fix
  Ssh::Target ssh() const;
  Endpoint endpoint(unsigned localPort, QString const& password) const; // through a tunnel on localPort

  static ProductionProfile load();
  void save() const;
  static QString knownHostsFile();
};
// The production database password: in the system keyring when the user chose that and one is
// available (Linux: secret-tool from libsecret), otherwise asked for and kept in memory until Noggit exits.
class PasswordStore {
public:
  static bool keyringAvailable();
  static QString keyringHint();
  // Empty when no password is known yet.
  static QString lookup(ProductionProfile const&);
  // Remembers it for this session, and in the keyring when the profile says so. Throws when the keyring fails.
  static void store(ProductionProfile const&, QString const& password);
  static void forget(ProductionProfile const&);
};
}
