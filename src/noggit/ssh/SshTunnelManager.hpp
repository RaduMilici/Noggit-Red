// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <noggit/ssh/SshTunnelConfig.hpp>

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QProcess>
#include <QtCore/QTimer>

#include <atomic>
#include <functional>

class QTcpSocket;

namespace Noggit
{
  namespace Ssh
  {
    struct TunnelOptions
    {
      QString ssh_executable;     // empty: "ssh" from PATH
      QString keyscan_executable; // empty: "ssh-keyscan" from PATH
      QString known_hosts_path;   // empty: <AppData>/ssh/known_hosts
      int start_timeout_ms = 30000;
      int probe_interval_ms = 250;
      int reconnect_base_delay_ms = 1000;
      int reconnect_max_delay_ms = 30000;
      int max_reconnect_attempts = 5;
      int max_port_retries = 3;
      int retry_cooldown_ms = 15000; // ensureReady re-tries a transient failure after this long
      int agent_available = -1;      // -1: detect (SSH_AUTH_SOCK); 0/1: forced (tests)
    };

    // Owns at most ONE long-lived `ssh -N -L 127.0.0.1:<auto>:<db host>:<db port>` process (the system
    // OpenSSH client, started through QProcess with an argument list -- never a shell) and reports its
    // state. Lives on the GUI thread and never blocks it: process start, the host-key scan and the
    // readiness probe are all signal driven. `ensureReady` is the one synchronous entry point for the
    // existing (synchronous) MySQL code; it waits in a local event loop that keeps repainting but does
    // not process user input.
    //
    // Host keys are verified against a DEDICATED known_hosts file (never ~/.ssh/known_hosts, never
    // StrictHostKeyChecking=no). Unknown hosts are scanned with ssh-keyscan and must be confirmed by
    // the user (or match the project's pinned SHA256 fingerprint) before they are written there; a
    // changed key is a hard failure that is never retried automatically.
    class SshTunnelManager : public QObject
    {
      Q_OBJECT
    public:
      enum class State
      {
        Disabled,
        Starting,
        Connected,
        Reconnecting,
        Failed,
      };
      Q_ENUM(State)

      using Options = TunnelOptions;

      // Asked (on the GUI thread) whether to trust a not-yet-known host. Return true to trust.
      using HostKeyPrompt = std::function<bool(TunnelConfig const&, QVector<HostKey> const&)>;

      explicit SshTunnelManager(Options options = Options(), QObject* parent = nullptr);
      ~SshTunnelManager() override;

      // Process-wide instance (defined in SshTunnelGlobal.cpp): installs the GUI host-key prompt and
      // stops the tunnel when the application quits.
      static SshTunnelManager& instance();

      // Starts the tunnel for `config` asynchronously. A no-op while a tunnel for the same config is
      // already starting/connected/reconnecting, so repeated calls never spawn a second ssh.
      void start(TunnelConfig const& config);
      // Stops and forgets the tunnel (project closed / Noggit exiting). Terminates ssh and waits briefly.
      void stop();
      // Settings saved or project switched: restarts only if something changed (or it had failed).
      void applyConfig(TunnelConfig const& config);
      // Explicit user action ("Test SSH tunnel"): always starts over, clearing a declined host-key prompt.
      void restart(TunnelConfig const& config);

      // Project lifecycle. projectClosed only stops the tunnel if it belongs to that project: when a
      // project window is replaced, the new window is created before the old one is destroyed.
      void projectOpened(TunnelConfig const& config);
      void projectClosed(QString const& project_id);
      QString openProjectId() const { return _open_project_id; }

      // Makes sure the tunnel for `config` is Connected, waiting up to `timeout_ms`. On success fills
      // `local_port`. When `interactive`, an unknown host key triggers the host-key prompt. Must be
      // called on the GUI thread; from other threads it only reports an already connected tunnel.
      bool ensureReady(TunnelConfig const& config, int timeout_ms, quint16* local_port, QString* error,
                       bool interactive = true);

      State state() const { return _state; }
      ErrorKind lastError() const { return _error; }
      QString lastErrorMessage() const;
      QString lastErrorDetails() const { return _error_details; }
      QString statusText() const;
      TunnelConfig const& config() const { return _config; }
      quint16 localPort() const { return _connected_port.load(); } // 0 unless Connected; any thread
      bool remoteForwardRefused() const { return _remote_refused; }
      int processStartCount() const { return _process_starts; }
      qint64 processId() const;

      QVector<HostKey> pendingHostKeys() const { return _pending_keys; }
      bool trustPendingHostKeys(QString* error);
      bool forgetHostKeys(TunnelConfig const& config, QString* error);
      QVector<HostKey> trustedHostKeys(TunnelConfig const& config) const;
      QString knownHostsPath() const;

      void setHostKeyPrompt(HostKeyPrompt prompt) { _prompt = std::move(prompt); }

      static QString stateName(State state);
      // A currently free TCP port on 127.0.0.1 (0 on failure). ssh re-binds it; a lost race shows up as
      // an ExitOnForwardFailure exit and is retried with a new port.
      static quint16 pickFreeLocalPort();

    signals:
      void stateChanged(Noggit::Ssh::SshTunnelManager::State state);
      void hostKeyConfirmationRequired();

    private:
      void beginHostKeyCheck();
      void runKeyscan();
      void onKeyscanFinished();
      void launchSsh();
      void onProbeTick();
      void onProcessFinished();
      void onStartTimeout();
      void onReconnectTimer();
      void markConnected();
      void fail(ErrorKind kind, QString const& details = QString());
      void scheduleReconnect(ErrorKind kind);
      void setState(State state);
      void killProcess();
      void killKeyscan();
      void resetRuntime();
      bool agentAvailable() const;
      QString sshExecutable() const;
      QString keyscanExecutable() const;
      bool writeKnownHosts(QByteArray const& contents, QString* error);
      QByteArray readKnownHosts() const;

      Options _options;
      TunnelConfig _config;
      QString _open_project_id;
      State _state = State::Disabled;
      ErrorKind _error = ErrorKind::None;
      QString _error_details;
      QString _stderr;

      QProcess* _process = nullptr;
      QProcess* _keyscan = nullptr;
      QTcpSocket* _probe = nullptr;
      QTimer _probe_timer;
      QTimer _start_timer;
      QTimer _reconnect_timer;

      quint16 _port = 0;
      std::atomic<quint16> _connected_port{0};
      bool _probe_succeeded = false;
      int _reconnect_attempt = 0;
      int _port_retries = 0;
      int _process_starts = 0;
      bool _key_encrypted = false;
      bool _remote_refused = false;
      bool _host_key_declined = false;
      bool _waiting = false;
      QString _host_key_algorithms;
      QVector<HostKey> _pending_keys;
      QElapsedTimer _failed_at;
      HostKeyPrompt _prompt;
    };
  }
}
