// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <noggit/ssh/SshTunnelConfig.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QPair>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <functional>

class QObject;

// One-shot commands on the SSH server the MySQL tunnel already uses (same key, port, user and verified
// host key -- no new trust decisions). Used to mirror the project's sql_exports/ folder into a
// tortoise-deploy server's custom SQL folder, so the database changes survive the world database being
// re-created, and to restart the world server so new spawns show up in game.
//
// Widget-free and process-free except for runRemoteCommand, so the builders are unit testable
// (tests/ssh_tunnel). Every value placed in a remote command is validated first: the remote side runs
// it through a shell, unlike the local ssh argument list.
namespace Noggit
{
  namespace Ssh
  {
    // Default for Keys::serverDeployDir(): relative to the SSH user's home directory.
    QString defaultDeployDir();
    // Where the mirror goes, inside the deploy folder. Only this folder is ever replaced.
    QString exportsMirrorSubdir();

    // A deploy folder as typed by the user ("tortoise-deploy", "~/tortoise-deploy/", "/srv/tortoise-deploy")
    // -> the form used in remote commands ("tortoise-deploy", "/srv/tortoise-deploy"). Empty when it is
    // not safe to put in a shell command: only letters, digits, '.', '_', '-' and '/', no ".." or "."
    // segment, no segment starting with '-'.
    QString normalizeRemoteDir(QString const& dir);

    // Remote shell commands. `deploy_dir` must come from normalizeRemoteDir (asserted by returning an
    // empty string otherwise).
    //  - mirror: reads a tar archive on stdin and atomically replaces <deploy>/<exportsMirrorSubdir()>
    //    with its contents; fails (exit 3) when <deploy>/storage/database/custom-sql does not exist.
    //  - restart: `docker compose restart mangosd` inside the deploy folder.
    QString buildMirrorCommand(QString const& deploy_dir);
    QString buildRestartCommand(QString const& deploy_dir);

    // ssh arguments for running `remote_command` (same hardening as buildSshArguments, minus the
    // port forward). Separate entries, never joined locally.
    QStringList buildRemoteCommandArguments(TunnelConfig const& config, QString const& known_hosts_path,
                                            QString const& host_key_algorithms, QString const& remote_command);

    // Makes an export file safe to run repeatedly: spawn INSERTs (creature, gameobject,
    // creature_spawn_entry) from older Noggit versions become REPLACE. Other statements are untouched.
    QByteArray makeReplayable(QByteArray const& sql);

    // A POSIX ustar archive of (relative path with '/', contents) pairs, mode 0644. Paths that do not fit
    // the ustar name/prefix fields are reported in `skipped` instead of being truncated.
    QByteArray buildTarArchive(QVector<QPair<QString, QByteArray>> const& files, QStringList* skipped = nullptr);

    struct RemoteTarget
    {
      TunnelConfig config;
      QString ssh_executable;      // full path (QStandardPaths::findExecutable("ssh"))
      QString known_hosts_path;    // the tunnel's dedicated known_hosts file
      QString host_key_algorithms; // non-empty when a fingerprint is pinned
    };

    struct RemoteResult
    {
      bool ok = false;
      ErrorKind error = ErrorKind::None; // ssh-level problem (connection, key, host key)
      int exit_code = -1;                // remote command's exit code when ssh itself worked
      QString message;                   // short, secret-free text for the UI
      QString output;                    // stdout + stderr excerpt for a details box
    };

    // Runs `remote_command` on the server, feeding `stdin_data`, without blocking the caller. `done` is
    // called exactly once on `context`'s thread (not at all if `context` is destroyed first).
    void runRemoteCommand(QObject* context, RemoteTarget const& target, QString const& remote_command,
                          QByteArray const& stdin_data, int timeout_ms, std::function<void(RemoteResult)> done);
  }
}
