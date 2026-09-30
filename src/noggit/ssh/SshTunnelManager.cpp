// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ssh/SshTunnelManager.hpp>
#include <noggit/Log.h>

#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QThread>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <algorithm>

namespace Noggit
{
  namespace Ssh
  {
    namespace
    {
      constexpr int max_stderr_bytes = 16 * 1024;
    }

    SshTunnelManager::SshTunnelManager(Options options, QObject* parent)
      : QObject(parent)
      , _options(std::move(options))
    {
      _probe_timer.setInterval(_options.probe_interval_ms);
      _start_timer.setSingleShot(true);
      _reconnect_timer.setSingleShot(true);
      connect(&_probe_timer, &QTimer::timeout, this, &SshTunnelManager::onProbeTick);
      connect(&_start_timer, &QTimer::timeout, this, &SshTunnelManager::onStartTimeout);
      connect(&_reconnect_timer, &QTimer::timeout, this, &SshTunnelManager::onReconnectTimer);
    }

    SshTunnelManager::~SshTunnelManager()
    {
      resetRuntime();
    }

    QString SshTunnelManager::stateName(State state)
    {
      switch (state)
      {
        case State::Disabled: return QStringLiteral("Disabled");
        case State::Starting: return QStringLiteral("Starting");
        case State::Connected: return QStringLiteral("Connected");
        case State::Reconnecting: return QStringLiteral("Reconnecting");
        case State::Failed: return QStringLiteral("Failed");
      }
      return QString();
    }

    quint16 SshTunnelManager::pickFreeLocalPort()
    {
      QTcpServer server;
      if (!server.listen(QHostAddress::LocalHost, 0))
      {
        return 0;
      }
      quint16 const port = server.serverPort();
      server.close();
      return port;
    }

    QString SshTunnelManager::knownHostsPath() const
    {
      if (!_options.known_hosts_path.isEmpty())
      {
        return _options.known_hosts_path;
      }
      return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/ssh/known_hosts");
    }

    bool SshTunnelManager::agentAvailable() const
    {
      if (_options.agent_available >= 0)
      {
        return _options.agent_available != 0;
      }
#ifdef _WIN32
      // Windows' ssh-agent is a named pipe service; SSH_AUTH_SOCK is usually unset there.
      return true;
#else
      return !qEnvironmentVariableIsEmpty("SSH_AUTH_SOCK");
#endif
    }

    QString SshTunnelManager::sshExecutable() const
    {
      return _options.ssh_executable.isEmpty() ? QStandardPaths::findExecutable(QStringLiteral("ssh"))
                                               : _options.ssh_executable;
    }

    QString SshTunnelManager::keyscanExecutable() const
    {
      return _options.keyscan_executable.isEmpty() ? QStandardPaths::findExecutable(QStringLiteral("ssh-keyscan"))
                                                   : _options.keyscan_executable;
    }

    qint64 SshTunnelManager::processId() const
    {
      return _process ? _process->processId() : 0;
    }

    QString SshTunnelManager::lastErrorMessage() const
    {
      if (_error == ErrorKind::InvalidSettings && !_error_details.isEmpty())
      {
        return _error_details; // names the offending field
      }
      QString message = describeError(_error, _config);
      if (_error == ErrorKind::KeyRejected && _key_encrypted)
      {
        message += QStringLiteral(" (This key is passphrase-protected: it must be unlocked in ssh-agent with ssh-add.)");
      }
      return message;
    }

    QString SshTunnelManager::statusText() const
    {
      switch (_state)
      {
        case State::Disabled:
          return QStringLiteral("SSH tunnel: off");
        case State::Starting:
          return QStringLiteral("SSH tunnel: connecting to %1...").arg(_config.displayName());
        case State::Connected:
          return QStringLiteral("SSH tunnel: connected via %1 (127.0.0.1:%2 -> %3:%4)")
              .arg(_config.displayName()).arg(_connected_port.load())
              .arg(_config.remote_db_host).arg(_config.remote_db_port);
        case State::Reconnecting:
          return QStringLiteral("SSH tunnel: connection lost, reconnecting (attempt %1 of %2)...")
              .arg(_reconnect_attempt).arg(_options.max_reconnect_attempts);
        case State::Failed:
          return QStringLiteral("SSH tunnel: failed. ") + lastErrorMessage();
      }
      return QString();
    }

    void SshTunnelManager::setState(State state)
    {
      if (state == _state)
      {
        return;
      }
      _state = state;
      Log << "SSH tunnel state: " << stateName(state).toStdString() << std::endl;
      emit stateChanged(state);
    }

    // --- lifecycle --------------------------------------------------------------------------------

    void SshTunnelManager::killProcess()
    {
      _probe_timer.stop();
      _connected_port = 0;
      if (_probe)
      {
        _probe->abort();
        _probe->deleteLater();
        _probe = nullptr;
      }
      if (!_process)
      {
        return;
      }
      QProcess* process = _process;
      _process = nullptr;
      // Detach first so the exit is not treated as an "unexpected" disconnect.
      process->disconnect(this);
      if (process->state() != QProcess::NotRunning)
      {
        process->terminate(); // SIGTERM: ssh closes the forward and exits immediately
        if (!process->waitForFinished(1500))
        {
          process->kill();
          process->waitForFinished(1000);
        }
      }
      process->deleteLater();
    }

    void SshTunnelManager::killKeyscan()
    {
      if (!_keyscan)
      {
        return;
      }
      QProcess* keyscan = _keyscan;
      _keyscan = nullptr;
      keyscan->disconnect(this);
      if (keyscan->state() != QProcess::NotRunning)
      {
        keyscan->kill();
        keyscan->waitForFinished(1000);
      }
      keyscan->deleteLater();
    }

    void SshTunnelManager::resetRuntime()
    {
      _start_timer.stop();
      _reconnect_timer.stop();
      killKeyscan();
      killProcess();
      _port = 0;
      _probe_succeeded = false;
    }

    void SshTunnelManager::stop()
    {
      bool const was_running = _process || _keyscan || _state != State::Disabled;
      resetRuntime();
      _config = TunnelConfig();
      _error = ErrorKind::None;
      _error_details.clear();
      _pending_keys.clear();
      _reconnect_attempt = 0;
      if (was_running)
      {
        Log << "SSH tunnel stopped." << std::endl;
      }
      setState(State::Disabled);
    }

    void SshTunnelManager::applyConfig(TunnelConfig const& config)
    {
      if (!config.enabled)
      {
        stop();
        return;
      }
      if (config == _config && _state != State::Failed && _state != State::Disabled)
      {
        return; // unchanged and alive: keep the running tunnel
      }
      _host_key_declined = false;
      start(config); // differs from the running one (or it had failed): start() tears the old one down

    }

    void SshTunnelManager::projectOpened(TunnelConfig const& config)
    {
      _open_project_id = config.project_id;
      applyConfig(config);
    }

    void SshTunnelManager::projectClosed(QString const& project_id)
    {
      if (project_id != _open_project_id)
      {
        return;
      }
      _open_project_id.clear();
      if (_config.project_id == project_id)
      {
        stop();
      }
    }

    void SshTunnelManager::restart(TunnelConfig const& config)
    {
      _host_key_declined = false;
      resetRuntime();
      if (_state == State::Starting || _state == State::Connected || _state == State::Reconnecting)
      {
        setState(State::Disabled);
      }
      start(config);
    }

    void SshTunnelManager::start(TunnelConfig const& config)
    {
      bool const alive = _state == State::Starting || _state == State::Connected || _state == State::Reconnecting;
      if (alive && config == _config)
      {
        return; // one ssh per project: never spawn a second one for the same settings
      }

      resetRuntime();
      _config = config;
      _error = ErrorKind::None;
      _error_details.clear();
      _pending_keys.clear();
      _reconnect_attempt = 0;
      _port_retries = 0;
      _remote_refused = false;
      _host_key_algorithms.clear();

      if (!config.enabled)
      {
        setState(State::Disabled);
        return;
      }

      if (sshExecutable().isEmpty() || !QFileInfo(sshExecutable()).isExecutable())
      {
        fail(ErrorKind::SshMissing);
        return;
      }

      _key_encrypted = false;
      ErrorKind const invalid = validateConfig(config, agentAvailable(), &_key_encrypted);
      if (invalid != ErrorKind::None)
      {
        fail(invalid, invalid == ErrorKind::InvalidSettings ? invalidSettingsReason(config) : QString());
        return;
      }

      Log << "Starting SSH tunnel via " << config.displayName().toStdString() << " to "
          << config.remote_db_host.toStdString() << ":" << config.remote_db_port << std::endl;
      setState(State::Starting);
      _start_timer.start(_options.start_timeout_ms);
      beginHostKeyCheck();
    }

    // --- host keys --------------------------------------------------------------------------------

    QByteArray SshTunnelManager::readKnownHosts() const
    {
      QFile file(knownHostsPath());
      if (!file.open(QIODevice::ReadOnly))
      {
        return QByteArray();
      }
      return file.readAll();
    }

    bool SshTunnelManager::writeKnownHosts(QByteArray const& contents, QString* error)
    {
      QString const path = knownHostsPath();
      QString const dir = QFileInfo(path).absolutePath();
      if (!QDir().mkpath(dir))
      {
        if (error)
        {
          *error = QStringLiteral("Cannot create %1").arg(dir);
        }
        return false;
      }
      QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

      QSaveFile file(path);
      if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit())
      {
        if (error)
        {
          *error = QStringLiteral("Cannot write %1").arg(path);
        }
        return false;
      }
      QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
      return true;
    }

    QVector<HostKey> SshTunnelManager::trustedHostKeys(TunnelConfig const& config) const
    {
      return parseKnownHosts(readKnownHosts(), knownHostsPattern(config.ssh_host, config.ssh_port));
    }

    bool SshTunnelManager::forgetHostKeys(TunnelConfig const& config, QString* error)
    {
      QString const pattern = knownHostsPattern(config.ssh_host, config.ssh_port);
      QByteArray out;
      for (QByteArray const& line : readKnownHosts().split('\n'))
      {
        if (line.trimmed().isEmpty())
        {
          continue;
        }
        QByteArray const hosts = line.trimmed().split(' ').value(0).toLower();
        if (hosts.split(',').contains(pattern.toLower().toUtf8()))
        {
          continue;
        }
        out += line + '\n';
      }
      Log << "SSH tunnel: forgot trusted host keys for " << pattern.toStdString() << std::endl;
      return writeKnownHosts(out, error);
    }

    bool SshTunnelManager::trustPendingHostKeys(QString* error)
    {
      if (_pending_keys.isEmpty())
      {
        if (error)
        {
          *error = QStringLiteral("There is no host key waiting for confirmation.");
        }
        return false;
      }
      QString const pattern = knownHostsPattern(_config.ssh_host, _config.ssh_port);
      QByteArray contents = readKnownHosts();
      if (!contents.isEmpty() && !contents.endsWith('\n'))
      {
        contents += '\n';
      }
      for (HostKey const& key : _pending_keys)
      {
        contents += pattern.toUtf8() + ' ' + key.type.toUtf8() + ' ' + key.blob + '\n';
        Log << "SSH tunnel: trusted host key " << key.type.toStdString() << " " << key.fingerprint.toStdString()
            << " for " << pattern.toStdString() << std::endl;
      }
      _pending_keys.clear();
      return writeKnownHosts(contents, error);
    }

    void SshTunnelManager::beginHostKeyCheck()
    {
      QString const pin = normalizeFingerprint(_config.expected_fingerprint);
      QVector<HostKey> const known = trustedHostKeys(_config);

      if (!pin.isEmpty())
      {
        auto const it = std::find_if(known.begin(), known.end(), [&](HostKey const& k) { return k.fingerprint == pin; });
        if (it != known.end())
        {
          _host_key_algorithms = hostKeyAlgorithmsFor(it->type);
          launchSsh();
          return;
        }
        runKeyscan(); // the pin decides; see onKeyscanFinished
        return;
      }

      if (!known.isEmpty())
      {
        launchSsh(); // ssh (StrictHostKeyChecking=yes) verifies; a changed key is a hard failure
        return;
      }
      runKeyscan();
    }

    void SshTunnelManager::runKeyscan()
    {
      QString const exe = keyscanExecutable();
      if (exe.isEmpty())
      {
        fail(ErrorKind::SshMissing, QStringLiteral("ssh-keyscan was not found (it is part of the OpenSSH client)."));
        return;
      }
      _keyscan = new QProcess(this);
      _keyscan->setProgram(exe);
      _keyscan->setArguments(buildKeyscanArguments(_config));
      _keyscan->setStandardInputFile(QProcess::nullDevice());
      connect(_keyscan, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &SshTunnelManager::onKeyscanFinished);
      connect(_keyscan, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e)
      {
        if (e == QProcess::FailedToStart)
        {
          fail(ErrorKind::SshMissing, QStringLiteral("ssh-keyscan could not be started."));
        }
      });
      _keyscan->start();
    }

    void SshTunnelManager::onKeyscanFinished()
    {
      if (!_keyscan)
      {
        return;
      }
      QByteArray const out = _keyscan->readAllStandardOutput();
      QString const err = QString::fromLocal8Bit(_keyscan->readAllStandardError());
      _keyscan->deleteLater();
      _keyscan = nullptr;

      QVector<HostKey> const scanned = parseKnownHosts(out);
      if (scanned.isEmpty())
      {
        ErrorKind const kind = classifySshStderr(err);
        fail(kind == ErrorKind::HostNotFound ? kind : ErrorKind::HostUnreachable, stderrExcerpt(err));
        return;
      }

      QString const pin = normalizeFingerprint(_config.expected_fingerprint);
      if (!pin.isEmpty())
      {
        auto const it = std::find_if(scanned.begin(), scanned.end(), [&](HostKey const& k) { return k.fingerprint == pin; });
        if (it == scanned.end())
        {
          QStringList offered;
          for (HostKey const& k : scanned)
          {
            offered << k.type + QStringLiteral(" ") + k.fingerprint;
          }
          fail(ErrorKind::HostKeyPinMismatch, QStringLiteral("Expected %1\nServer offered:\n%2").arg(pin, offered.join('\n')));
          return;
        }
        // The pinned key is an explicit trust decision: replace whatever was stored for this host with
        // exactly that key. ssh still verifies the server really holds it.
        QString error;
        if (!forgetHostKeys(_config, &error))
        {
          fail(ErrorKind::Unknown, error);
          return;
        }
        _pending_keys = { *it };
        if (!trustPendingHostKeys(&error))
        {
          fail(ErrorKind::Unknown, error);
          return;
        }
        _host_key_algorithms = hostKeyAlgorithmsFor(it->type);
        launchSsh();
        return;
      }

      _pending_keys = scanned;
      QStringList fingerprints;
      for (HostKey const& k : scanned)
      {
        fingerprints << k.type + QStringLiteral(" ") + k.fingerprint;
      }
      fail(ErrorKind::HostKeyUnknown, fingerprints.join('\n')); // keeps _pending_keys for the prompt
      emit hostKeyConfirmationRequired();
    }

    // --- ssh process ------------------------------------------------------------------------------

    void SshTunnelManager::launchSsh()
    {
      killProcess();
      _stderr.clear();
      _probe_succeeded = false;
      _port = pickFreeLocalPort();
      if (!_port)
      {
        fail(ErrorKind::ForwardFailure, QStringLiteral("No free local TCP port on 127.0.0.1."));
        return;
      }

      QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
      // BatchMode already forbids prompts; also make sure no askpass helper can pop up.
      env.remove(QStringLiteral("SSH_ASKPASS"));
      env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("never"));

      _process = new QProcess(this);
      _process->setProgram(sshExecutable());
      _process->setArguments(buildSshArguments(_config, _port, knownHostsPath(), _host_key_algorithms));
      _process->setProcessEnvironment(env);
      _process->setStandardInputFile(QProcess::nullDevice());
      _process->setStandardOutputFile(QProcess::nullDevice());

      connect(_process, &QProcess::readyReadStandardError, this, [this]
      {
        if (!_process)
        {
          return;
        }
        QString const chunk = QString::fromLocal8Bit(_process->readAllStandardError());
        _stderr += chunk;
        if (_stderr.size() > max_stderr_bytes)
        {
          _stderr = _stderr.right(max_stderr_bytes);
        }
        if (chunk.contains(QLatin1String("open failed"), Qt::CaseInsensitive)
            || chunk.contains(QLatin1String("administratively prohibited"), Qt::CaseInsensitive))
        {
          _remote_refused = true;
        }
      });
      connect(_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &SshTunnelManager::onProcessFinished);
      connect(_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e)
      {
        if (e == QProcess::FailedToStart)
        {
          fail(ErrorKind::SshMissing);
        }
      });

      // Never log the argument list: it is harmless today, but must stay that way if it ever changes.
      Log << "SSH tunnel: launching ssh (local 127.0.0.1:" << _port << ")" << std::endl;
      ++_process_starts;
      _process->start();
      _probe_timer.start(_options.probe_interval_ms);
    }

    void SshTunnelManager::onProbeTick()
    {
      if (!_process || _process->state() != QProcess::Running)
      {
        return; // not started yet, or exiting (onProcessFinished handles it)
      }
      if (_probe_succeeded)
      {
        // One interval after the port answered, ssh is still alive: it owns the listener (had another
        // program won the port race, ExitOnForwardFailure would have made ssh exit by now).
        markConnected();
        return;
      }
      if (!_probe)
      {
        _probe = new QTcpSocket(this);
        connect(_probe, &QTcpSocket::connected, this, [this]
        {
          _probe->abort();
          _probe_succeeded = true;
        });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        auto const socket_error = &QAbstractSocket::errorOccurred;
#else
        auto const socket_error = qOverload<QAbstractSocket::SocketError>(&QAbstractSocket::error);
#endif
        connect(_probe, socket_error, this, [this](QAbstractSocket::SocketError)
        {
          _probe->abort();
        });
      }
      if (_probe->state() == QAbstractSocket::UnconnectedState)
      {
        _probe->connectToHost(QHostAddress::LocalHost, _port);
      }
    }

    void SshTunnelManager::markConnected()
    {
      _probe_timer.stop();
      _start_timer.stop();
      if (_probe)
      {
        _probe->deleteLater();
        _probe = nullptr;
      }
      _reconnect_attempt = 0;
      _port_retries = 0;
      _error = ErrorKind::None;
      _error_details.clear();
      _connected_port = _port;
      Log << "SSH tunnel connected: 127.0.0.1:" << _port << " -> " << _config.remote_db_host.toStdString() << ":"
          << _config.remote_db_port << " via " << _config.displayName().toStdString() << std::endl;
      setState(State::Connected);
    }

    void SshTunnelManager::onProcessFinished()
    {
      if (!_process)
      {
        return;
      }
      _stderr += QString::fromLocal8Bit(_process->readAllStandardError());
      int const exit_code = _process->exitCode();
      _process->deleteLater();
      _process = nullptr;
      _probe_timer.stop();
      _connected_port = 0;
      if (_probe)
      {
        _probe->abort();
      }

      ErrorKind kind = classifySshStderr(_stderr);
      Log << "SSH tunnel: ssh exited (code " << exit_code << ") while " << stateName(_state).toStdString() << std::endl;
      LogDebug << "SSH tunnel stderr: " << stderrExcerpt(_stderr).toStdString() << std::endl;

      switch (_state)
      {
        case State::Starting:
          if (kind == ErrorKind::ForwardFailure && _port_retries < _options.max_port_retries)
          {
            ++_port_retries;
            Log << "SSH tunnel: local port was taken, retrying with another port" << std::endl;
            launchSsh();
            return;
          }
          fail(kind);
          return;
        case State::Connected:
          if (kind == ErrorKind::Unknown || kind == ErrorKind::RemoteForwardRefused)
          {
            kind = ErrorKind::ConnectionLost;
          }
          scheduleReconnect(kind);
          return;
        case State::Reconnecting:
          if (kind == ErrorKind::ForwardFailure && _port_retries < _options.max_port_retries)
          {
            ++_port_retries;
            launchSsh();
            return;
          }
          scheduleReconnect(kind);
          return;
        default:
          return;
      }
    }

    void SshTunnelManager::scheduleReconnect(ErrorKind kind)
    {
      _start_timer.stop();
      if (!isRetryable(kind) || _reconnect_attempt >= _options.max_reconnect_attempts)
      {
        fail(kind);
        return;
      }
      int const delay = std::min(_options.reconnect_max_delay_ms,
                                 _options.reconnect_base_delay_ms * (1 << std::min(_reconnect_attempt, 16)));
      ++_reconnect_attempt;
      _error = kind;
      _error_details = stderrExcerpt(_stderr);
      Log << "SSH tunnel: reconnect attempt " << _reconnect_attempt << " in " << delay << " ms" << std::endl;
      _reconnect_timer.start(delay);
      if (_state != State::Reconnecting)
      {
        setState(State::Reconnecting);
      }
      else
      {
        emit stateChanged(_state); // attempt counter changed; refresh status displays
      }
    }

    void SshTunnelManager::onReconnectTimer()
    {
      if (_state != State::Reconnecting)
      {
        return;
      }
      _port_retries = 0;
      _start_timer.start(_options.start_timeout_ms);
      launchSsh();
    }

    void SshTunnelManager::onStartTimeout()
    {
      if (_state == State::Reconnecting)
      {
        killKeyscan();
        killProcess();
        scheduleReconnect(ErrorKind::Timeout);
        return;
      }
      if (_state == State::Starting)
      {
        QString details = stderrExcerpt(_stderr);
        fail(ErrorKind::Timeout, details);
      }
    }

    void SshTunnelManager::fail(ErrorKind kind, QString const& details)
    {
      QVector<HostKey> const pending = _pending_keys;
      resetRuntime();
      _pending_keys = kind == ErrorKind::HostKeyUnknown ? pending : QVector<HostKey>();
      _error = kind;
      _error_details = details.isEmpty() ? stderrExcerpt(_stderr) : details;
      _failed_at.start();
      LogError << "SSH tunnel failed: " << describeError(kind, _config).toStdString() << std::endl;
      if (!_error_details.isEmpty())
      {
        LogDebug << "SSH tunnel failure details: " << _error_details.toStdString() << std::endl;
      }
      if (_state == State::Failed)
      {
        emit stateChanged(_state);
      }
      setState(State::Failed);
    }

    // --- synchronous entry point ------------------------------------------------------------------

    bool SshTunnelManager::ensureReady(TunnelConfig const& config, int timeout_ms, quint16* local_port,
                                       QString* error, bool interactive)
    {
      auto set_error = [&](QString const& e)
      {
        if (error)
        {
          *error = e;
        }
        return false;
      };

      if (!config.enabled)
      {
        return set_error(QStringLiteral("The SSH tunnel is disabled for this project."));
      }

      if (QThread::currentThread() != thread())
      {
        quint16 const port = _connected_port.load();
        if (!port)
        {
          return set_error(QStringLiteral("The SSH tunnel is not connected."));
        }
        if (local_port)
        {
          *local_port = port;
        }
        return true;
      }

      if (config != _config || _state == State::Disabled)
      {
        start(config);
      }
      else if (_state == State::Failed && isRetryable(_error) && _failed_at.isValid()
               && _failed_at.elapsed() >= _options.retry_cooldown_ms)
      {
        restart(config);
      }

      auto wait = [&]
      {
        if (_state != State::Starting && _state != State::Reconnecting)
        {
          return true;
        }
        if (_waiting)
        {
          return false; // re-entered from our own event loop: don't nest another wait
        }
        _waiting = true;
        QEventLoop loop;
        auto const conn = connect(this, &SshTunnelManager::stateChanged, &loop, [&loop](State s)
        {
          if (s == State::Connected || s == State::Failed || s == State::Disabled)
          {
            loop.quit();
          }
        });
        QTimer::singleShot(timeout_ms, &loop, &QEventLoop::quit);
        // Keeps timers/painting/process I/O alive (no frozen window) without letting the user fire a
        // second database action into the middle of this one.
        loop.exec(QEventLoop::ExcludeUserInputEvents);
        disconnect(conn);
        _waiting = false;
        return true;
      };

      if (!wait())
      {
        return set_error(QStringLiteral("The SSH tunnel is still starting."));
      }

      if (_state == State::Failed && _error == ErrorKind::HostKeyUnknown && interactive && _prompt
          && !_host_key_declined && !_pending_keys.isEmpty())
      {
        if (_prompt(_config, _pending_keys))
        {
          QString trust_error;
          if (!trustPendingHostKeys(&trust_error))
          {
            return set_error(trust_error);
          }
          restart(config);
          wait();
        }
        else
        {
          _host_key_declined = true;
        }
      }

      if (_state == State::Connected)
      {
        if (local_port)
        {
          *local_port = _connected_port.load();
        }
        return true;
      }
      if (_state == State::Failed)
      {
        return set_error(lastErrorMessage());
      }
      return set_error(QStringLiteral("Timed out waiting for the SSH tunnel to %1.").arg(config.displayName()));
    }
  }
}
