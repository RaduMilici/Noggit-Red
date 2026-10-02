// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifdef USE_MYSQL_UID_STORAGE

#include <noggit/ui/content/ServerSync.hpp>

#include <noggit/MySqlSettings.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/ssh/SshRemote.hpp>
#include <noggit/ssh/SshTunnelManager.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStatusBar>

#include <algorithm>
#include <functional>

namespace Noggit::Ui
{
  namespace
  {
    using namespace Noggit::Ssh;

    constexpr int sync_delay_ms = 1500;      // batches the several writes one save can make
    constexpr int sync_timeout_ms = 120000;
    constexpr int restart_timeout_ms = 300000; // mangosd may take up to its 2 min stop grace period

    struct SyncState
    {
      bool running = false;
      bool pending = false;
      QTimer* timer = nullptr;
    };

    SyncState& state()
    {
      static SyncState s;
      return s;
    }

    void showStatus(QString const& message, int timeout_ms = 8000)
    {
      for (QWidget* widget : QApplication::topLevelWidgets())
      {
        if (auto* window = qobject_cast<QMainWindow*>(widget))
        {
          window->statusBar()->showMessage(message, timeout_ms);
        }
      }
    }

    // Non-blocking, so a failed background upload never interrupts editing with a nested event loop.
    void showWarning(QString const& title, QString const& text, QString const& details)
    {
      auto* box = new QMessageBox(QMessageBox::Warning, title, text, QMessageBox::Ok, QApplication::activeWindow());
      box->setAttribute(Qt::WA_DeleteOnClose);
      if (!details.isEmpty())
      {
        box->setDetailedText(details);
      }
      box->show();
    }

    QString deployDir()
    {
      return normalizeRemoteDir(Noggit::mysqlSetting(Keys::serverDeployDir(), defaultDeployDir()).toString());
    }

    // The tunnel's server, key and verified host key. False (with `why`) when it cannot be used.
    bool resolveTarget(RemoteTarget* target, QString* why)
    {
      TunnelConfig const config = TunnelConfig::fromProjectSettings();
      if (!config.enabled)
      {
        *why = QStringLiteral("This needs MySQL and the SSH tunnel enabled for this project "
                              "(Settings -> MySQL -> SSH tunnel).");
        return false;
      }
      QString const reason = invalidSettingsReason(config);
      if (!reason.isEmpty())
      {
        *why = reason;
        return false;
      }

      auto& tunnel = SshTunnelManager::instance();
      QVector<HostKey> const trusted = tunnel.trustedHostKeys(config);
      if (trusted.isEmpty())
      {
        *why = QStringLiteral("The server's SSH host key is not trusted yet. Use \"Test SSH tunnel\" in "
                              "Settings -> MySQL first.");
        return false;
      }

      QString algorithms;
      QString const pin = normalizeFingerprint(config.expected_fingerprint);
      if (!pin.isEmpty())
      {
        auto const it = std::find_if(trusted.begin(), trusted.end(),
                                     [&](HostKey const& k) { return k.fingerprint == pin; });
        if (it == trusted.end())
        {
          *why = describeError(ErrorKind::HostKeyPinMismatch, config);
          return false;
        }
        algorithms = hostKeyAlgorithmsFor(it->type);
      }

      target->config = config;
      target->ssh_executable = QStandardPaths::findExecutable(QStringLiteral("ssh"));
      target->known_hosts_path = tunnel.knownHostsPath();
      target->host_key_algorithms = algorithms;
      return true;
    }

    // <project>/sql_exports/**/*.sql as a tar archive, made replayable. False (with `why`) when there is
    // nothing to send: an empty upload would wipe the server's copy.
    bool buildExportArchive(QByteArray* archive, int* file_count, QString* why)
    {
      auto const* project = Noggit::Project::CurrentProject::get();
      if (!project)
      {
        *why = QStringLiteral("No project is open.");
        return false;
      }
      QDir const exports(QDir(QString::fromStdString(project->ProjectPath)).filePath(QStringLiteral("sql_exports")));
      QStringList paths;
      QDirIterator it(exports.absolutePath(), { QStringLiteral("*.sql") }, QDir::Files, QDirIterator::Subdirectories);
      while (it.hasNext())
      {
        paths << exports.relativeFilePath(it.next());
      }
      if (paths.isEmpty())
      {
        *why = QStringLiteral("There are no SQL exports in %1 yet.").arg(QDir::toNativeSeparators(exports.absolutePath()));
        return false;
      }
      std::sort(paths.begin(), paths.end()); // tortoise-deploy replays in path order too

      QVector<QPair<QString, QByteArray>> files;
      for (QString const& relative : paths)
      {
        QFile file(exports.filePath(relative));
        if (!file.open(QIODevice::ReadOnly))
        {
          *why = QStringLiteral("Could not read %1: %2").arg(QDir::toNativeSeparators(file.fileName()), file.errorString());
          return false;
        }
        files.push_back({ QDir::fromNativeSeparators(relative), makeReplayable(file.readAll()) });
      }

      QStringList skipped;
      *archive = buildTarArchive(files, &skipped);
      if (!skipped.isEmpty())
      {
        *why = QStringLiteral("These export paths are too long to upload; shorten them:\n%1").arg(skipped.join('\n'));
        return false;
      }
      *file_count = files.size();
      return true;
    }

    // One mirror run. `report` is false for the automatic background sync.
    void runSync(QWidget* parent, bool report)
    {
      auto& s = state();
      if (s.running)
      {
        s.pending = true;
        return;
      }

      RemoteTarget target;
      QByteArray archive;
      int file_count = 0;
      QString why;
      QString const dir = deployDir();
      if (dir.isEmpty())
      {
        why = QStringLiteral("\"tortoise-deploy folder on server\" is not a valid path (letters, digits, '.', "
                             "'_', '-' and '/' only).");
      }
      else if (resolveTarget(&target, &why))
      {
        buildExportArchive(&archive, &file_count, &why);
      }
      if (!why.isEmpty())
      {
        if (report)
        {
          QMessageBox::warning(parent, QStringLiteral("Copy SQL exports to server"), why);
        }
        else
        {
          showWarning(QStringLiteral("SQL exports not copied to the server"),
                      QStringLiteral("Your change is in the database, but its SQL copy was not uploaded to the "
                                     "server:\n\n%1").arg(why), QString());
        }
        return;
      }

      s.running = true;
      showStatus(QStringLiteral("Copying %1 SQL export file(s) to %2...").arg(file_count).arg(target.config.ssh_host), 0);
      QPointer<QWidget> guard(parent);
      runRemoteCommand(qApp, target, buildMirrorCommand(dir), archive, sync_timeout_ms,
                       [guard, report, file_count, dir, target](RemoteResult result)
      {
        auto& s = state();
        s.running = false;
        QString const where = dir + QStringLiteral("/") + exportsMirrorSubdir();
        if (result.ok)
        {
          QString const message = QStringLiteral("Copied %1 SQL export file(s) to %2:%3. They are re-applied on "
                                                 "every database start.").arg(file_count).arg(target.config.ssh_host, where);
          showStatus(message);
          if (report)
          {
            QMessageBox::information(guard, QStringLiteral("Copy SQL exports to server"), message);
          }
        }
        else
        {
          QString text = result.message;
          if (result.exit_code == 3)
          {
            text = QStringLiteral("The server has no %1/storage/database/custom-sql folder. Check \"tortoise-deploy "
                                  "folder on server\" in Settings -> MySQL.").arg(dir);
          }
          showStatus(QStringLiteral("Copying SQL exports to the server failed."));
          showWarning(QStringLiteral("SQL exports not copied to the server"),
                      QStringLiteral("Your changes are in the database, but the SQL copies were not uploaded:\n\n%1\n\n"
                                     "Retry with Assist -> Copy SQL exports to server.").arg(text),
                      result.output);
        }

        if (s.pending)
        {
          s.pending = false;
          scheduleServerExportSync();
        }
      });
    }
  }

  void scheduleServerExportSync()
  {
    if (!Noggit::mysqlSetting(Keys::serverSync(), false).toBool()
        || !TunnelConfig::fromProjectSettings().enabled)
    {
      return;
    }
    auto& s = state();
    if (!s.timer)
    {
      s.timer = new QTimer(qApp);
      s.timer->setSingleShot(true);
      QObject::connect(s.timer, &QTimer::timeout, qApp, [] { runSync(nullptr, false); });
    }
    s.timer->start(sync_delay_ms); // restarts the countdown, so a burst of writes uploads once
  }

  void syncExportsToServerNow(QWidget* parent)
  {
    if (state().running)
    {
      QMessageBox::information(parent, QStringLiteral("Copy SQL exports to server"),
                               QStringLiteral("An upload is already running; it will be followed by another one."));
      state().pending = true;
      return;
    }
    runSync(parent, true);
  }

  void restartWorldServer(QWidget* parent)
  {
    RemoteTarget target;
    QString why;
    QString const dir = deployDir();
    if (dir.isEmpty())
    {
      why = QStringLiteral("\"tortoise-deploy folder on server\" is not a valid path.");
    }
    else
    {
      resolveTarget(&target, &why);
    }
    if (!why.isEmpty())
    {
      QMessageBox::warning(parent, QStringLiteral("Restart world server"), why);
      return;
    }

    auto const answer = QMessageBox::question(
      parent, QStringLiteral("Restart world server"),
      QStringLiteral("Restart the world server (mangosd) on %1?\n\nEveryone online is disconnected. New spawns "
                     "appear once it is back up, which takes about 2-3 minutes.").arg(target.config.ssh_host),
      QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
    {
      return;
    }

    showStatus(QStringLiteral("Restarting the world server on %1...").arg(target.config.ssh_host), 0);
    QPointer<QWidget> guard(parent);
    runRemoteCommand(qApp, target, buildRestartCommand(dir), QByteArray(), restart_timeout_ms,
                     [guard, target](RemoteResult result)
    {
      if (result.ok)
      {
        QString const message = QStringLiteral("The world server on %1 was restarted. Players can log in again in "
                                               "about 2-3 minutes.").arg(target.config.ssh_host);
        showStatus(message);
        QMessageBox::information(guard, QStringLiteral("Restart world server"), message);
      }
      else
      {
        showStatus(QStringLiteral("Restarting the world server failed."));
        QMessageBox box(QMessageBox::Warning, QStringLiteral("Restart world server"),
                        QStringLiteral("The world server was not restarted:\n\n%1").arg(result.message),
                        QMessageBox::Ok, guard);
        if (!result.output.isEmpty())
        {
          box.setDetailedText(result.output);
        }
        box.exec();
      }
    });
  }
}

#endif
