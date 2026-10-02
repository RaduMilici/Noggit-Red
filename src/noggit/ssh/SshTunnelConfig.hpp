// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

// Per-project SSH tunnel settings for the MySQL connection, plus the pure (process-free) helpers the
// tunnel manager needs: input validation, private-key checks, OpenSSH stderr classification and
// host-key fingerprinting. Nothing in here talks to the network or spawns processes, so it is unit
// testable on its own (see tests/ssh_tunnel).
//
// Security notes for maintainers:
//  - Every value that ends up on the ssh command line is validated here first. ssh is started through
//    QProcess with a QStringList (no shell), but ssh itself still interprets a destination that starts
//    with '-' as an option and expands %tokens / ${ENV} in -i and *KnownHostsFile paths, so the
//    validators reject those shapes instead of trusting argv separation alone.
//  - Never log a TunnelConfig wholesale; the key PATH is fine to show, the key CONTENT is never read
//    beyond the header needed to detect "encrypted / public key / PuTTY format".
namespace Noggit
{
  namespace Ssh
  {
    // Setting suffixes, stored per project through Noggit::mysqlSettingKey (MySqlSettings.hpp).
    namespace Keys
    {
      inline QString enabled() { return QStringLiteral("ssh_enabled"); }
      inline QString host() { return QStringLiteral("ssh_host"); }
      inline QString port() { return QStringLiteral("ssh_port"); }
      inline QString user() { return QStringLiteral("ssh_user"); }
      inline QString keyPath() { return QStringLiteral("ssh_key_path"); }
      inline QString remoteDbHost() { return QStringLiteral("ssh_remote_db_host"); }
      inline QString remoteDbPort() { return QStringLiteral("ssh_remote_db_port"); }
      inline QString fingerprint() { return QStringLiteral("ssh_host_fingerprint"); }
      // Server-side helpers (SshRemote.hpp): mirror sql_exports/ after database writes, and the
      // tortoise-deploy folder on the server that holds compose.yaml and the custom SQL folder.
      inline QString serverSync() { return QStringLiteral("ssh_server_sync"); }
      inline QString serverDeployDir() { return QStringLiteral("ssh_server_deploy_dir"); }
    }

    struct TunnelConfig
    {
      bool enabled = false;
      QString ssh_host;
      int ssh_port = 22;
      QString ssh_user;
      QString key_path;
      QString remote_db_host = QStringLiteral("127.0.0.1");
      int remote_db_port = 3306;
      QString expected_fingerprint; // optional "SHA256:..." pin
      QString project_id;           // Noggit::projectKeyId() of the owning project

      // Reads the ACTIVE project's settings (MySqlSettings.hpp). `enabled` is true only when BOTH MySQL
      // and the SSH tunnel are enabled for the project. GUI thread only (QSettings is reentrant, but the
      // "active project" is a GUI-side notion).
      static TunnelConfig fromProjectSettings();

      bool operator==(TunnelConfig const& o) const;
      bool operator!=(TunnelConfig const& o) const { return !(*this == o); }

      // "user@host:port" -- safe for status text and logs (no key, no DB credentials).
      QString displayName() const;
    };

    enum class ErrorKind
    {
      None,
      InvalidSettings,
      SshMissing,
      KeyMissing,
      KeyNotRegularFile,
      KeyPermissions,
      KeyInvalidFormat,
      KeyEncryptedNoAgent,
      KeyRejected,
      HostKeyUnknown,
      HostKeyChanged,
      HostKeyPinMismatch,
      HostNotFound,
      HostUnreachable,
      ForwardFailure,       // could not listen on the local 127.0.0.1 port
      RemoteForwardRefused, // tunnel is up, but the SSH server could not reach remote DB host:port
      Timeout,
      ConnectionLost,
      Unknown,
    };

    // Errors that a later automatic attempt may fix by itself (network blips). Everything else
    // (bad key, host-key problems, bad settings) needs the user to act first.
    bool isRetryable(ErrorKind kind);

    // Short, actionable, secret-free explanation for the UI.
    QString describeError(ErrorKind kind, TunnelConfig const& config);

    // --- validation -------------------------------------------------------------------------------

    bool isValidHostname(QString const& host);  // DNS name, IPv4 or bare IPv6; never starts with '-'
    bool isValidUsername(QString const& user);
    bool isValidPort(int port);
    // Accepts "SHA256:<43 base64>" or the bare 43 chars; returns the normalized "SHA256:..." form,
    // or an empty string when invalid. Empty input -> empty output (= no pin).
    QString normalizeFingerprint(QString const& fingerprint);

    // Expands a leading "~/" and makes the path absolute.
    QString expandKeyPath(QString const& path);

    struct KeyInspection
    {
      ErrorKind error = ErrorKind::None;
      bool encrypted = false; // passphrase protected -> must be unlocked in ssh-agent
    };
    // Existence, regular file, owner-only permissions (Unix), and a header sniff for
    // encrypted / public-key / PuTTY files. `agent_available` is whether SSH_AUTH_SOCK is set.
    KeyInspection inspectPrivateKey(QString const& path, bool agent_available);

    // Full settings validation (fields + key). Returns ErrorKind::None when the tunnel may start.
    ErrorKind validateConfig(TunnelConfig const& config, bool agent_available, bool* key_encrypted = nullptr);

    // Which field makes the settings invalid, in plain words (empty when the fields are fine).
    QString invalidSettingsReason(TunnelConfig const& config);

    // --- ssh output / host keys -------------------------------------------------------------------

    // Maps OpenSSH client stderr to an ErrorKind (ErrorKind::Unknown when nothing matched).
    ErrorKind classifySshStderr(QString const& stderr_text);

    // Keeps the last few non-empty stderr lines, length-limited, for a "details" box.
    QString stderrExcerpt(QString const& stderr_text, int max_lines = 6);

    struct HostKey
    {
      QString type;       // e.g. "ssh-ed25519"
      QByteArray blob;    // base64 key blob exactly as in known_hosts
      QString fingerprint; // "SHA256:..." (OpenSSH format, unpadded base64)
    };

    QString fingerprintOfBlob(QByteArray const& base64_blob);

    // known_hosts host field for host/port: "host" for 22, "[host]:port" otherwise.
    QString knownHostsPattern(QString const& host, int port);

    // Parses known_hosts / ssh-keyscan output. When `pattern` is non-empty only lines whose host field
    // contains exactly that pattern (comma-separated list) are returned; hashed (|1|) and marker
    // (@revoked / @cert-authority) lines are ignored.
    QVector<HostKey> parseKnownHosts(QByteArray const& text, QString const& pattern = QString());

    // "ssh-ed25519" -> "ssh-ed25519"; "ssh-rsa" -> "rsa-sha2-512,rsa-sha2-256"; used to restrict
    // negotiation to a pinned key's type.
    QString hostKeyAlgorithmsFor(QString const& key_type);

    // Quotes a path for an "-o Option=value" argument (ssh's own config tokenizer, NOT a shell):
    // wraps in double quotes, escapes backslashes and '%'. Returns empty when the path cannot be
    // represented safely (contains '"', control characters or "${").
    QString quoteSshOptionPath(QString const& path);
    // Escapes '%' for -i (ssh percent-expands identity paths). Empty when unsafe.
    QString escapeSshIdentityPath(QString const& path);

    // The -F/-o options every Noggit ssh connection uses: no user config, batch mode, strict host keys
    // against `known_hosts_path` only, public key auth only, no forwarding of agent/X11, no multiplexing.
    QStringList hardenedSshOptions(QString const& known_hosts_path, QString const& host_key_algorithms = QString());

    // Builds the full ssh argument list. Separate entries, never joined into a command string.
    // `host_key_algorithms` (from hostKeyAlgorithmsFor) restricts negotiation when a fingerprint is pinned.
    QStringList buildSshArguments(TunnelConfig const& config, quint16 local_port, QString const& known_hosts_path,
                                  QString const& host_key_algorithms = QString());
    QStringList buildKeyscanArguments(TunnelConfig const& config);
  }
}
