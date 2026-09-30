// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ssh/SshTunnelConfig.hpp>
#include <noggit/MySqlSettings.hpp>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QHostAddress>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <algorithm>

namespace Noggit
{
  namespace Ssh
  {
    TunnelConfig TunnelConfig::fromProjectSettings()
    {
      TunnelConfig config;
      config.enabled = Noggit::mysqlSetting(Keys::enabled(), false).toBool();
      config.ssh_host = Noggit::mysqlSetting(Keys::host(), QString()).toString().trimmed();
      config.ssh_port = Noggit::mysqlSetting(Keys::port(), 22).toInt();
      config.ssh_user = Noggit::mysqlSetting(Keys::user(), QString()).toString().trimmed();
      config.key_path = Noggit::mysqlSetting(Keys::keyPath(), QString()).toString().trimmed();
      config.remote_db_host = Noggit::mysqlSetting(Keys::remoteDbHost(), QStringLiteral("127.0.0.1")).toString().trimmed();
      config.remote_db_port = Noggit::mysqlSetting(Keys::remoteDbPort(), 3306).toInt();
      config.expected_fingerprint = Noggit::mysqlSetting(Keys::fingerprint(), QString()).toString().trimmed();
      config.project_id = Noggit::projectKeyId();

      if (config.remote_db_host.isEmpty())
      {
        config.remote_db_host = QStringLiteral("127.0.0.1");
      }
      if (!config.ssh_port)
      {
        config.ssh_port = 22;
      }
      if (!config.remote_db_port)
      {
        config.remote_db_port = 3306;
      }
      return config;
    }

    bool TunnelConfig::operator==(TunnelConfig const& o) const
    {
      return enabled == o.enabled
          && ssh_host == o.ssh_host
          && ssh_port == o.ssh_port
          && ssh_user == o.ssh_user
          && key_path == o.key_path
          && remote_db_host == o.remote_db_host
          && remote_db_port == o.remote_db_port
          && expected_fingerprint == o.expected_fingerprint
          && project_id == o.project_id;
    }

    QString TunnelConfig::displayName() const
    {
      return QStringLiteral("%1@%2:%3").arg(ssh_user, ssh_host).arg(ssh_port);
    }

    bool isRetryable(ErrorKind kind)
    {
      switch (kind)
      {
        case ErrorKind::HostNotFound:
        case ErrorKind::HostUnreachable:
        case ErrorKind::ForwardFailure:
        case ErrorKind::Timeout:
        case ErrorKind::ConnectionLost:
        case ErrorKind::Unknown:
          return true;
        default:
          return false;
      }
    }

    QString describeError(ErrorKind kind, TunnelConfig const& c)
    {
      switch (kind)
      {
        case ErrorKind::None:
          return QString();
        case ErrorKind::InvalidSettings:
          return QStringLiteral("The SSH tunnel settings are incomplete or invalid. Check the SSH host, port, "
                                "username and the remote database host/port (letters, digits, '.', '-' and '_' only).");
        case ErrorKind::SshMissing:
#ifdef _WIN32
          return QStringLiteral("The OpenSSH client (ssh.exe) was not found. Enable the Windows optional feature "
                                "\"OpenSSH Client\" and restart Noggit.");
#else
          return QStringLiteral("The OpenSSH client (ssh) was not found. On Ubuntu install it with: "
                                "sudo apt install openssh-client");
#endif
        case ErrorKind::KeyMissing:
          return QStringLiteral("The private key file does not exist or cannot be read: %1").arg(c.key_path);
        case ErrorKind::KeyNotRegularFile:
          return QStringLiteral("The private key path is not a regular file: %1").arg(c.key_path);
        case ErrorKind::KeyPermissions:
          return QStringLiteral("The private key file can be read by other users, so SSH refuses to use it. "
                                "Fix it with: chmod 600 \"%1\"").arg(c.key_path);
        case ErrorKind::KeyInvalidFormat:
          return QStringLiteral("The selected file is not an OpenSSH private key. Select the PRIVATE key (usually "
                                "the file WITHOUT \".pub\"). PuTTY .ppk keys must first be exported to OpenSSH "
                                "format with PuTTYgen.");
        case ErrorKind::KeyEncryptedNoAgent:
          return QStringLiteral("This key is protected by a passphrase. Noggit never asks for passphrases; unlock "
                                "the key in ssh-agent first (run: ssh-add \"%1\"), then try again.").arg(c.key_path);
        case ErrorKind::KeyRejected:
          return QStringLiteral("The SSH server rejected the key for user '%1'. Check the SSH username, and ask "
                                "your administrator to confirm your public key was installed on the server. If the "
                                "key has a passphrase, make sure it is loaded with ssh-add.").arg(c.ssh_user);
        case ErrorKind::HostKeyUnknown:
          return QStringLiteral("The SSH server's identity is not trusted yet. Confirm its fingerprint with your "
                                "administrator, then trust it (use \"Test SSH tunnel\").");
        case ErrorKind::HostKeyChanged:
          return QStringLiteral("WARNING: the SSH server's host key has CHANGED since you last trusted it. This can "
                                "mean someone is intercepting the connection. Noggit refused to connect. Only if "
                                "your administrator confirms the server was rebuilt, use \"Forget trusted host "
                                "key\" and verify the new fingerprint.");
        case ErrorKind::HostKeyPinMismatch:
          return QStringLiteral("The SSH server's host key does not match the expected fingerprint configured for "
                                "this project. Noggit refused to connect. Double-check the fingerprint with your "
                                "administrator.");
        case ErrorKind::HostNotFound:
          return QStringLiteral("The SSH host name '%1' could not be resolved. Check the spelling or use the "
                                "server's IP address.").arg(c.ssh_host);
        case ErrorKind::HostUnreachable:
          return QStringLiteral("Could not reach the SSH server at %1 port %2. Check the address, that the server "
                                "is running, and that its firewall allows SSH from your IP.").arg(c.ssh_host).arg(c.ssh_port);
        case ErrorKind::ForwardFailure:
          return QStringLiteral("SSH could not open the local tunnel port on 127.0.0.1. Another program may be "
                                "using it; try again.");
        case ErrorKind::RemoteForwardRefused:
          return QStringLiteral("The SSH tunnel is up, but the server could not reach MySQL at %1:%2. Check the "
                                "remote database host/port and that MySQL is running on the server.")
              .arg(c.remote_db_host).arg(c.remote_db_port);
        case ErrorKind::Timeout:
          return QStringLiteral("Timed out while opening the SSH tunnel to %1.").arg(c.ssh_host);
        case ErrorKind::ConnectionLost:
          return QStringLiteral("The SSH connection to %1 was lost.").arg(c.ssh_host);
        case ErrorKind::Unknown:
          return QStringLiteral("SSH exited unexpectedly. See the details for the message from ssh.");
      }
      return QString();
    }

    bool isValidHostname(QString const& host)
    {
      if (host.isEmpty() || host.size() > 253 || host.startsWith('-'))
      {
        return false;
      }

      // Bare IPv6 (no brackets, no %scope -- '%' would be token-expanded by ssh).
      if (host.contains(':'))
      {
        QHostAddress address;
        return !host.contains('%') && address.setAddress(host)
            && address.protocol() == QAbstractSocket::IPv6Protocol;
      }

      static QRegularExpression const re(
        QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*\\.?$"));
      return re.match(host).hasMatch();
    }

    bool isValidUsername(QString const& user)
    {
      static QRegularExpression const re(QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9._-]{0,63}$"));
      return re.match(user).hasMatch();
    }

    bool isValidPort(int port)
    {
      return port >= 1 && port <= 65535;
    }

    QString normalizeFingerprint(QString const& fingerprint)
    {
      QString fp = fingerprint.trimmed();
      if (fp.isEmpty())
      {
        return QString();
      }
      if (fp.startsWith(QStringLiteral("SHA256:"), Qt::CaseInsensitive))
      {
        fp = fp.mid(7);
      }
      while (fp.endsWith('='))
      {
        fp.chop(1);
      }
      static QRegularExpression const re(QStringLiteral("^[A-Za-z0-9+/]{43}$"));
      if (!re.match(fp).hasMatch())
      {
        return QString();
      }
      return QStringLiteral("SHA256:") + fp;
    }

    QString expandKeyPath(QString const& path)
    {
      QString p = path.trimmed();
      if (p == QStringLiteral("~"))
      {
        p = QDir::homePath();
      }
      else if (p.startsWith(QStringLiteral("~/")))
      {
        p = QDir::homePath() + p.mid(1);
      }
      if (p.isEmpty())
      {
        return p;
      }
      return QDir::cleanPath(QFileInfo(p).absoluteFilePath());
    }

    namespace
    {
      bool hasUnsafeCharacters(QString const& s)
      {
        if (s.contains(QStringLiteral("${")))
        {
          return true;
        }
        return std::any_of(s.begin(), s.end(), [](QChar ch) { return ch.unicode() < 0x20 || ch.unicode() == 0x7f; });
      }

      // cipher name of an "openssh-key-v1" private key blob ("none" when unencrypted), empty on error.
      QByteArray opensshKeyCipher(QByteArray const& pem)
      {
        static QByteArray const begin = "-----BEGIN OPENSSH PRIVATE KEY-----";
        static QByteArray const end = "-----END OPENSSH PRIVATE KEY-----";
        int const b = pem.indexOf(begin);
        int const e = pem.indexOf(end, b);
        if (b < 0 || e < 0)
        {
          return QByteArray();
        }
        QByteArray body = pem.mid(b + begin.size(), e - b - begin.size()).simplified();
        body.replace(' ', QByteArray());
        QByteArray blob = QByteArray::fromBase64(body);

        static QByteArray const magic("openssh-key-v1\0", 15);
        QByteArray cipher;
        if (blob.startsWith(magic) && blob.size() >= magic.size() + 4)
        {
          auto const* p = reinterpret_cast<unsigned char const*>(blob.constData() + magic.size());
          quint32 const len = (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
          if (len > 0 && len < 64 && magic.size() + 4 + int(len) <= blob.size())
          {
            cipher = blob.mid(magic.size() + 4, int(len));
          }
        }
        blob.fill('\0');
        body.fill('\0');
        return cipher;
      }
    }

    KeyInspection inspectPrivateKey(QString const& path, bool agent_available)
    {
      KeyInspection result;
      QFileInfo const info(path);
      if (path.isEmpty() || !info.exists())
      {
        result.error = ErrorKind::KeyMissing;
        return result;
      }
      if (!info.isFile())
      {
        result.error = ErrorKind::KeyNotRegularFile;
        return result;
      }

#ifndef _WIN32
      struct stat st;
      if (::stat(QFile::encodeName(path).constData(), &st) != 0)
      {
        result.error = ErrorKind::KeyMissing;
        return result;
      }
      if (!S_ISREG(st.st_mode))
      {
        result.error = ErrorKind::KeyNotRegularFile;
        return result;
      }
      // Same rule OpenSSH applies ("UNPROTECTED PRIVATE KEY FILE"): no group/other access at all.
      if ((st.st_mode & 077) != 0)
      {
        result.error = ErrorKind::KeyPermissions;
        return result;
      }
#endif

      QFile file(path);
      if (!file.open(QIODevice::ReadOnly))
      {
        result.error = ErrorKind::KeyMissing;
        return result;
      }
      // Only the header/cipher field is inspected; the buffer is wiped before returning.
      QByteArray head = file.read(64 * 1024);
      file.close();

      QByteArray const trimmed = head.trimmed();
      bool const is_public =
        trimmed.startsWith("ssh-") || trimmed.startsWith("ecdsa-") || trimmed.startsWith("sk-")
        || head.contains("BEGIN SSH2 PUBLIC KEY") || head.contains("BEGIN OPENSSH PUBLIC KEY")
        || head.contains("BEGIN PUBLIC KEY");

      if (trimmed.startsWith("PuTTY-User-Key-File") || is_public)
      {
        result.error = ErrorKind::KeyInvalidFormat;
      }
      else if (head.contains("-----BEGIN OPENSSH PRIVATE KEY-----"))
      {
        QByteArray const cipher = opensshKeyCipher(head);
        if (cipher.isEmpty())
        {
          result.error = ErrorKind::KeyInvalidFormat;
        }
        result.encrypted = !cipher.isEmpty() && cipher != "none";
      }
      else if (head.contains("-----BEGIN ENCRYPTED PRIVATE KEY-----") || head.contains("Proc-Type: 4,ENCRYPTED"))
      {
        result.encrypted = true;
      }
      else if (!(head.contains("-----BEGIN ") && head.contains("PRIVATE KEY-----")))
      {
        result.error = ErrorKind::KeyInvalidFormat;
      }
      head.fill('\0');

      if (result.error == ErrorKind::None && result.encrypted && !agent_available)
      {
        result.error = ErrorKind::KeyEncryptedNoAgent;
      }
      return result;
    }

    ErrorKind validateConfig(TunnelConfig const& c, bool agent_available, bool* key_encrypted)
    {
      if (!isValidHostname(c.ssh_host) || !isValidUsername(c.ssh_user) || !isValidPort(c.ssh_port)
          || !isValidHostname(c.remote_db_host) || !isValidPort(c.remote_db_port))
      {
        return ErrorKind::InvalidSettings;
      }
      if (!c.expected_fingerprint.isEmpty() && normalizeFingerprint(c.expected_fingerprint).isEmpty())
      {
        return ErrorKind::InvalidSettings;
      }
      if (c.key_path.isEmpty())
      {
        return ErrorKind::KeyMissing;
      }
      if (escapeSshIdentityPath(c.key_path).isEmpty())
      {
        return ErrorKind::InvalidSettings;
      }
      KeyInspection const key = inspectPrivateKey(c.key_path, agent_available);
      if (key_encrypted)
      {
        *key_encrypted = key.encrypted;
      }
      return key.error;
    }

    ErrorKind classifySshStderr(QString const& text)
    {
      auto has = [&](char const* needle) { return text.contains(QLatin1String(needle), Qt::CaseInsensitive); };

      // Host-key problems first: they are security relevant and must never be masked by a later line.
      if (has("REMOTE HOST IDENTIFICATION HAS CHANGED") || has("POSSIBLE DNS SPOOFING")
          || (has("Host key for") && has("has changed")))
      {
        return ErrorKind::HostKeyChanged;
      }
      if (has("no matching host key type found"))
      {
        return ErrorKind::HostKeyPinMismatch;
      }
      if (has("Host key verification failed") || (has("host key is known for")))
      {
        return ErrorKind::HostKeyUnknown;
      }
      if (has("UNPROTECTED PRIVATE KEY FILE") || has("bad permissions"))
      {
        return ErrorKind::KeyPermissions;
      }
      if (has("Load key") && has("invalid format"))
      {
        return ErrorKind::KeyInvalidFormat;
      }
      if (has("Load key") && has("passphrase"))
      {
        return ErrorKind::KeyEncryptedNoAgent;
      }
      if (has("no such identity"))
      {
        return ErrorKind::KeyMissing;
      }
      if (has("Permission denied") || has("Too many authentication failures"))
      {
        return ErrorKind::KeyRejected;
      }
      if (has("Could not resolve hostname") || has("Name or service not known")
          || has("Temporary failure in name resolution") || has("nodename nor servname"))
      {
        return ErrorKind::HostNotFound;
      }
      if (has("Address already in use") || has("cannot listen to port") || has("Could not request local forwarding"))
      {
        return ErrorKind::ForwardFailure;
      }

      // Per line from here on: "channel N: open failed: connect failed: Connection refused" is a remote
      // forward problem (MySQL side), not "SSH host unreachable".
      bool remote_refused = false;
      ErrorKind line_kind = ErrorKind::Unknown;
      for (QString const& line : text.split('\n'))
      {
        auto line_has = [&](char const* needle) { return line.contains(QLatin1String(needle), Qt::CaseInsensitive); };
        if (line_has("open failed") || line_has("administratively prohibited"))
        {
          remote_refused = true;
          continue;
        }
        if (line_has("Connection refused") || line_has("No route to host") || line_has("Network is unreachable")
            || line_has("Connection timed out") || line_has("Operation timed out"))
        {
          return ErrorKind::HostUnreachable;
        }
        if (line_has("Timeout, server") || line_has("Connection reset") || line_has("Broken pipe")
            || line_has("Connection closed") || line_has("client_loop"))
        {
          line_kind = ErrorKind::ConnectionLost;
        }
      }
      if (line_kind != ErrorKind::Unknown)
      {
        return line_kind;
      }
      return remote_refused ? ErrorKind::RemoteForwardRefused : ErrorKind::Unknown;
    }

    QString stderrExcerpt(QString const& text, int max_lines)
    {
      QStringList lines;
      for (QString const& raw : text.split('\n'))
      {
        QString const line = raw.trimmed();
        if (!line.isEmpty())
        {
          lines << (line.size() > 200 ? line.left(200) + QStringLiteral("...") : line);
        }
      }
      while (lines.size() > max_lines)
      {
        lines.removeFirst();
      }
      return lines.join('\n');
    }

    QString fingerprintOfBlob(QByteArray const& base64_blob)
    {
      QByteArray const raw = QByteArray::fromBase64(base64_blob);
      if (raw.isEmpty())
      {
        return QString();
      }
      QByteArray const digest = QCryptographicHash::hash(raw, QCryptographicHash::Sha256);
      return QStringLiteral("SHA256:") + QString::fromLatin1(digest.toBase64(QByteArray::OmitTrailingEquals));
    }

    QString knownHostsPattern(QString const& host, int port)
    {
      if (port == 22)
      {
        return host.toLower();
      }
      return QStringLiteral("[%1]:%2").arg(host.toLower()).arg(port);
    }

    QVector<HostKey> parseKnownHosts(QByteArray const& text, QString const& pattern)
    {
      static QRegularExpression const type_re(QStringLiteral("^[a-z0-9@.-]+$"));
      static QRegularExpression const blob_re(QStringLiteral("^[A-Za-z0-9+/]+=*$"));

      QVector<HostKey> keys;
      for (QByteArray const& raw : text.split('\n'))
      {
        QString const line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith('@') || line.startsWith('|'))
        {
          continue;
        }
        QStringList const fields = line.simplified().split(QLatin1Char(' '));
        if (fields.size() < 3 || !type_re.match(fields[1]).hasMatch() || !blob_re.match(fields[2]).hasMatch())
        {
          continue;
        }
        if (!pattern.isEmpty())
        {
          QStringList const hosts = fields[0].toLower().split(',');
          if (!hosts.contains(pattern.toLower()))
          {
            continue;
          }
        }
        HostKey key;
        key.type = fields[1];
        key.blob = fields[2].toLatin1();
        key.fingerprint = fingerprintOfBlob(key.blob);
        if (!key.fingerprint.isEmpty())
        {
          keys.push_back(key);
        }
      }
      return keys;
    }

    QString hostKeyAlgorithmsFor(QString const& key_type)
    {
      if (key_type == QStringLiteral("ssh-rsa"))
      {
        return QStringLiteral("rsa-sha2-512,rsa-sha2-256");
      }
      return key_type;
    }

    QString quoteSshOptionPath(QString const& path)
    {
      if (path.isEmpty() || path.contains('"') || hasUnsafeCharacters(path))
      {
        return QString();
      }
      QString escaped = path;
      escaped.replace('\\', QStringLiteral("\\\\"));
      escaped.replace('%', QStringLiteral("%%"));
      return QStringLiteral("\"") + escaped + QStringLiteral("\"");
    }

    QString escapeSshIdentityPath(QString const& path)
    {
      if (path.isEmpty() || hasUnsafeCharacters(path))
      {
        return QString();
      }
      QString escaped = path;
      escaped.replace('%', QStringLiteral("%%"));
      return escaped;
    }

    namespace
    {
      QString bracketIfIpv6(QString const& host)
      {
        return host.contains(':') ? QStringLiteral("[") + host + QStringLiteral("]") : host;
      }
    }

    QStringList buildSshArguments(TunnelConfig const& c, quint16 local_port, QString const& known_hosts_path,
                                  QString const& host_key_algorithms)
    {
      QString const known_hosts = quoteSshOptionPath(known_hosts_path);

      QStringList args;
      args << QStringLiteral("-N") << QStringLiteral("-T")
           // Ignore ~/.ssh/config and /etc/ssh/ssh_config: a ProxyCommand/LocalCommand/ControlMaster set
           // there must not change what this tunnel does.
           << QStringLiteral("-F") << QStringLiteral("none")
           << QStringLiteral("-o") << QStringLiteral("BatchMode=yes")
           << QStringLiteral("-o") << QStringLiteral("ExitOnForwardFailure=yes")
           << QStringLiteral("-o") << QStringLiteral("ServerAliveInterval=30")
           << QStringLiteral("-o") << QStringLiteral("ServerAliveCountMax=3")
           << QStringLiteral("-o") << QStringLiteral("ConnectTimeout=15")
           << QStringLiteral("-o") << QStringLiteral("StrictHostKeyChecking=yes")
           << QStringLiteral("-o") << (QStringLiteral("UserKnownHostsFile=") + known_hosts)
           << QStringLiteral("-o") << (QStringLiteral("GlobalKnownHostsFile=") + known_hosts)
           << QStringLiteral("-o") << QStringLiteral("UpdateHostKeys=no")
           << QStringLiteral("-o") << QStringLiteral("HashKnownHosts=no")
           << QStringLiteral("-o") << QStringLiteral("CheckHostIP=no")
           << QStringLiteral("-o") << QStringLiteral("IdentitiesOnly=yes")
           << QStringLiteral("-o") << QStringLiteral("PreferredAuthentications=publickey")
           << QStringLiteral("-o") << QStringLiteral("PasswordAuthentication=no")
           << QStringLiteral("-o") << QStringLiteral("KbdInteractiveAuthentication=no")
           << QStringLiteral("-o") << QStringLiteral("ForwardAgent=no")
           << QStringLiteral("-o") << QStringLiteral("ForwardX11=no")
           << QStringLiteral("-o") << QStringLiteral("ControlMaster=no")
           << QStringLiteral("-o") << QStringLiteral("ControlPath=none")
           << QStringLiteral("-o") << QStringLiteral("PermitLocalCommand=no");

      if (!host_key_algorithms.isEmpty())
      {
        // Pinned fingerprint: only negotiate the pinned key's type, so ssh verifies exactly that key.
        args << QStringLiteral("-o") << (QStringLiteral("HostKeyAlgorithms=") + host_key_algorithms);
      }

      args << QStringLiteral("-i") << escapeSshIdentityPath(c.key_path)
           << QStringLiteral("-p") << QString::number(c.ssh_port)
           << QStringLiteral("-l") << c.ssh_user
           << QStringLiteral("-L")
           << QStringLiteral("127.0.0.1:%1:%2:%3").arg(local_port).arg(bracketIfIpv6(c.remote_db_host)).arg(c.remote_db_port)
           << QStringLiteral("--")
           << c.ssh_host;
      return args;
    }

    QStringList buildKeyscanArguments(TunnelConfig const& c)
    {
      return QStringList()
          << QStringLiteral("-T") << QStringLiteral("10")
          << QStringLiteral("-t") << QStringLiteral("rsa,ecdsa,ed25519")
          << QStringLiteral("-p") << QString::number(c.ssh_port)
          << QStringLiteral("--")
          << c.ssh_host;
    }
  }
}
