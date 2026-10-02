// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ssh/SshRemote.hpp>

#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>

#include <cstring>
#include <memory>

namespace Noggit
{
  namespace Ssh
  {
    QString defaultDeployDir()
    {
      return QStringLiteral("tortoise-deploy");
    }

    QString exportsMirrorSubdir()
    {
      return QStringLiteral("storage/database/custom-sql/noggit");
    }

    QString normalizeRemoteDir(QString const& dir)
    {
      QString d = dir.trimmed();
      if (d.startsWith(QStringLiteral("~/")))
      {
        d = d.mid(2); // remote commands start in the SSH user's home directory
      }
      while (d.size() > 1 && d.endsWith('/'))
      {
        d.chop(1);
      }
      static QRegularExpression const allowed(QStringLiteral("^/?[A-Za-z0-9._/-]{1,255}$"));
      if (d.isEmpty() || d == QStringLiteral("/") || !allowed.match(d).hasMatch())
      {
        return QString();
      }
      for (QString const& segment : d.split('/', Qt::SkipEmptyParts))
      {
        if (segment == QStringLiteral(".") || segment == QStringLiteral("..") || segment.startsWith('-'))
        {
          return QString();
        }
      }
      return d;
    }

    namespace
    {
      // Only called on normalizeRemoteDir output, which cannot contain a quote.
      QString quoted(QString const& path)
      {
        return QStringLiteral("'") + path + QStringLiteral("'");
      }
    }

    QString buildMirrorCommand(QString const& deploy_dir)
    {
      if (deploy_dir.isEmpty() || normalizeRemoteDir(deploy_dir) != deploy_dir)
      {
        return QString();
      }
      QString const base = deploy_dir + QStringLiteral("/storage/database/custom-sql");
      // Extract into a scratch folder first and swap it in, so a broken upload never leaves a half
      // written mirror behind for the next database start to replay.
      return QStringLiteral(
               "set -e; base=%1; "
               "if [ ! -d \"$base\" ]; then echo \"custom SQL folder not found: $base\" >&2; exit 3; fi; "
               "tmp=\"$base/.noggit-upload\"; rm -rf -- \"$tmp\"; mkdir -- \"$tmp\"; "
               "tar -xf - -C \"$tmp\"; rm -rf -- \"$base/noggit\"; mv -- \"$tmp\" \"$base/noggit\"")
        .arg(quoted(base));
    }

    QString buildRestartCommand(QString const& deploy_dir)
    {
      if (deploy_dir.isEmpty() || normalizeRemoteDir(deploy_dir) != deploy_dir)
      {
        return QString();
      }
      return QStringLiteral("cd -- %1 && docker compose restart mangosd").arg(quoted(deploy_dir));
    }

    QStringList buildRemoteCommandArguments(TunnelConfig const& c, QString const& known_hosts_path,
                                            QString const& host_key_algorithms, QString const& remote_command)
    {
      QStringList args;
      args << QStringLiteral("-T")
           << hardenedSshOptions(known_hosts_path, host_key_algorithms)
           << QStringLiteral("-i") << escapeSshIdentityPath(c.key_path)
           << QStringLiteral("-p") << QString::number(c.ssh_port)
           << QStringLiteral("-l") << c.ssh_user
           << QStringLiteral("--")
           << c.ssh_host
           << remote_command;
      return args;
    }

    QByteArray makeReplayable(QByteArray const& sql)
    {
      static QRegularExpression const spawn_insert(
        QStringLiteral("^INSERT INTO (`?)(creature|gameobject|creature_spawn_entry)\\1 "),
        QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
      QString text = QString::fromUtf8(sql);
      text.replace(spawn_insert, QStringLiteral("REPLACE INTO \\1\\2\\1 "));
      return text.toUtf8();
    }

    namespace
    {
      void putOctal(char* field, int width, quint64 value)
      {
        // width-1 octal digits, zero padded, NUL terminated.
        QByteArray const digits = QByteArray::number(value, 8).rightJustified(width - 1, '0');
        std::memcpy(field, digits.constData(), width - 1);
        field[width - 1] = '\0';
      }

      // Splits `path` into ustar prefix (<= 155 bytes) and name (<= 100 bytes) at a '/'.
      bool splitUstarPath(QByteArray const& path, QByteArray* prefix, QByteArray* name)
      {
        if (path.size() <= 100)
        {
          *prefix = QByteArray();
          *name = path;
          return true;
        }
        for (int i = path.size() - 1; i > 0; --i)
        {
          if (path[i] == '/' && i <= 155 && path.size() - i - 1 <= 100 && path.size() - i - 1 > 0)
          {
            *prefix = path.left(i);
            *name = path.mid(i + 1);
            return true;
          }
        }
        return false;
      }
    }

    QByteArray buildTarArchive(QVector<QPair<QString, QByteArray>> const& files, QStringList* skipped)
    {
      QByteArray tar;
      quint64 const mtime = quint64(QDateTime::currentSecsSinceEpoch());
      for (auto const& file : files)
      {
        QByteArray const path = file.first.toUtf8();
        QByteArray prefix;
        QByteArray name;
        if (path.isEmpty() || path.startsWith('/') || path.contains("..") || !splitUstarPath(path, &prefix, &name))
        {
          if (skipped)
          {
            *skipped << file.first;
          }
          continue;
        }

        char header[512];
        std::memset(header, 0, sizeof(header));
        std::memcpy(header, name.constData(), name.size());
        putOctal(header + 100, 8, 0644);
        putOctal(header + 108, 8, 0);
        putOctal(header + 116, 8, 0);
        putOctal(header + 124, 12, quint64(file.second.size()));
        putOctal(header + 136, 12, mtime);
        header[156] = '0'; // regular file
        std::memcpy(header + 257, "ustar", 6);
        std::memcpy(header + 263, "00", 2);
        std::memcpy(header + 345, prefix.constData(), prefix.size());

        // Checksum: sum of all header bytes with the checksum field read as spaces.
        std::memset(header + 148, ' ', 8);
        quint32 sum = 0;
        for (unsigned char byte : header)
        {
          sum += byte;
        }
        QByteArray const checksum = QByteArray::number(sum, 8).rightJustified(6, '0');
        std::memcpy(header + 148, checksum.constData(), 6);
        header[154] = '\0';
        header[155] = ' ';

        tar.append(header, sizeof(header));
        tar.append(file.second);
        int const padding = (512 - file.second.size() % 512) % 512;
        tar.append(QByteArray(padding, '\0'));
      }
      tar.append(QByteArray(1024, '\0')); // end-of-archive marker
      return tar;
    }

    void runRemoteCommand(QObject* context, RemoteTarget const& target, QString const& remote_command,
                          QByteArray const& stdin_data, int timeout_ms, std::function<void(RemoteResult)> done)
    {
      auto finish = std::make_shared<std::function<void(RemoteResult)>>(std::move(done));
      auto report = [finish](RemoteResult result)
      {
        if (*finish)
        {
          auto callback = std::move(*finish);
          *finish = nullptr;
          callback(std::move(result));
        }
      };

      if (remote_command.isEmpty())
      {
        RemoteResult result;
        result.error = ErrorKind::InvalidSettings;
        result.message = QStringLiteral("The tortoise-deploy folder on the server is not a valid path.");
        report(result);
        return;
      }
      if (target.ssh_executable.isEmpty())
      {
        RemoteResult result;
        result.error = ErrorKind::SshMissing;
        result.message = describeError(ErrorKind::SshMissing, target.config);
        report(result);
        return;
      }

      auto* process = new QProcess(context);
      QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
      // BatchMode already forbids prompts; also make sure no askpass helper can pop up.
      env.remove(QStringLiteral("SSH_ASKPASS"));
      env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("never"));
      process->setProcessEnvironment(env);
      process->setProgram(target.ssh_executable);
      process->setArguments(buildRemoteCommandArguments(target.config, target.known_hosts_path,
                                                        target.host_key_algorithms, remote_command));

      auto* timer = new QTimer(process);
      timer->setSingleShot(true);
      auto timed_out = std::make_shared<bool>(false);
      QObject::connect(timer, &QTimer::timeout, process, [process, timed_out]
      {
        *timed_out = true;
        process->kill();
      });

      QObject::connect(process, &QProcess::started, process, [process, stdin_data]
      {
        process->write(stdin_data);
        process->closeWriteChannel();
      });

      QObject::connect(process, &QProcess::errorOccurred, process, [process, report, target](QProcess::ProcessError e)
      {
        if (e != QProcess::FailedToStart)
        {
          return; // a crash/kill also emits finished(), handled there
        }
        RemoteResult result;
        result.error = ErrorKind::SshMissing;
        result.message = describeError(ErrorKind::SshMissing, target.config);
        report(result);
        process->deleteLater();
      });

      QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process,
                       [process, report, target, timed_out](int exit_code, QProcess::ExitStatus status)
      {
        QString const out = QString::fromLocal8Bit(process->readAllStandardOutput());
        QString const err = QString::fromLocal8Bit(process->readAllStandardError());
        RemoteResult result;
        result.output = stderrExcerpt(out + QStringLiteral("\n") + err, 12);

        if (*timed_out)
        {
          result.error = ErrorKind::Timeout;
          result.message = QStringLiteral("The command on %1 did not finish in time.").arg(target.config.ssh_host);
        }
        else if (status == QProcess::NormalExit && exit_code == 0)
        {
          result.ok = true;
          result.exit_code = 0;
        }
        else if (status == QProcess::NormalExit && exit_code != 255)
        {
          // ssh connected; the remote command itself failed and returned its own exit code.
          result.exit_code = exit_code;
          result.message = QStringLiteral("The command on the server failed (exit code %1).").arg(exit_code);
        }
        else
        {
          // 255 (or a crash): ssh could not connect / authenticate / verify the host.
          result.error = classifySshStderr(err);
          if (result.error == ErrorKind::None)
          {
            result.error = ErrorKind::Unknown;
          }
          result.message = describeError(result.error, target.config);
        }
        report(result);
        process->deleteLater();
      });

      timer->start(timeout_ms);
      process->start();
    }
  }
}
