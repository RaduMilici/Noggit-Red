// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// Unit tests for the MySQL SSH tunnel (src/noggit/ssh). No remote server is involved: the ssh and
// ssh-keyscan executables are replaced by fake_ssh.py / fake_keyscan.py, which reproduce OpenSSH's
// behavior (listen on the -L port, or print OpenSSH's exact failure text). Throwaway keys are generated
// with ssh-keygen into a temporary directory at runtime.

#include <noggit/MySqlSettings.hpp>
#include <noggit/ssh/SshTunnelConfig.hpp>
#include <noggit/ssh/SshTunnelManager.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QtTest>

#ifndef _WIN32
#include <signal.h>
#endif

using namespace Noggit::Ssh;
using State = SshTunnelManager::State;

namespace
{
  QByteArray fakeEd25519Blob(char fill)
  {
    QByteArray raw;
    raw.append(QByteArray::fromHex("0000000b"));
    raw.append("ssh-ed25519");
    raw.append(QByteArray::fromHex("00000020"));
    raw.append(QByteArray(32, fill));
    return raw.toBase64();
  }

  bool processAlive(qint64 pid)
  {
#ifndef _WIN32
    return pid > 0 && ::kill(static_cast<pid_t>(pid), 0) == 0;
#else
    Q_UNUSED(pid);
    return false;
#endif
  }

  bool portAccepts(quint16 port)
  {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    bool const ok = socket.waitForConnected(500);
    socket.abort();
    return ok;
  }
}

class SshTunnelTest : public QObject
{
  Q_OBJECT

  QTemporaryDir _dir;
  QString _key;          // unencrypted throwaway key, mode 0600
  QString _plan;
  QString _log;
  QString _keyscan_out;
  QString _known_hosts;
  bool _have_keygen = false;

  QString path(QString const& name) const { return _dir.filePath(name); }

  void writeFile(QString const& file, QByteArray const& contents)
  {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QFile f(file);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(contents);
  }

  void setPlan(QStringList const& modes)
  {
    writeFile(_plan, modes.join('\n').toUtf8() + '\n');
    QFile::remove(_plan + ".count");
    QFile::remove(_log);
  }

  QList<QStringList> invocations() const
  {
    QList<QStringList> result;
    QFile f(_log);
    if (!f.open(QIODevice::ReadOnly))
    {
      return result;
    }
    for (QByteArray const& line : f.readAll().split('\n'))
    {
      if (line.trimmed().isEmpty())
      {
        continue;
      }
      QStringList args;
      for (auto const& v : QJsonDocument::fromJson(line).object().value("args").toArray())
      {
        args << v.toString();
      }
      result << args;
    }
    return result;
  }

  TunnelConfig config() const
  {
    TunnelConfig c;
    c.enabled = true;
    c.ssh_host = "tunnel.example.test";
    c.ssh_port = 2222;
    c.ssh_user = "tester";
    c.key_path = _key;
    c.remote_db_host = "127.0.0.1";
    c.remote_db_port = 3306;
    c.project_id = "p1";
    return c;
  }

  TunnelOptions options() const
  {
    TunnelOptions o;
    o.ssh_executable = QFINDTESTDATA("fake_ssh.py");
    o.keyscan_executable = QFINDTESTDATA("fake_keyscan.py");
    o.known_hosts_path = _known_hosts;
    o.start_timeout_ms = 5000;
    o.probe_interval_ms = 50;
    o.reconnect_base_delay_ms = 50;
    o.reconnect_max_delay_ms = 200;
    o.max_reconnect_attempts = 3;
    o.agent_available = 0;
    return o;
  }

  void trustTestHost(char fill = 'A')
  {
    writeFile(_known_hosts, "[tunnel.example.test]:2222 ssh-ed25519 " + fakeEd25519Blob(fill) + "\n");
  }

  static bool waitForState(SshTunnelManager& m, State s, int ms = 5000)
  {
    return QTest::qWaitFor([&] { return m.state() == s; }, ms);
  }

private slots:
  void initTestCase()
  {
#ifdef _WIN32
    QSKIP("The fake ssh helpers are POSIX scripts.");
#endif
    QVERIFY(_dir.isValid());
    QVERIFY(!QStandardPaths::findExecutable("python3").isEmpty());
    _plan = path("plan.txt");
    _log = path("invocations.jsonl");
    _keyscan_out = path("keyscan.txt");
    _known_hosts = path("ssh/known_hosts");
    qputenv("FAKE_SSH_PLAN", _plan.toUtf8());
    qputenv("FAKE_SSH_LOG", _log.toUtf8());
    qputenv("FAKE_KEYSCAN_OUT", _keyscan_out.toUtf8());

    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, path("settings"));

    _key = path("id_test");
    _have_keygen = !QStandardPaths::findExecutable("ssh-keygen").isEmpty();
    if (_have_keygen)
    {
      QCOMPARE(QProcess::execute("ssh-keygen", {"-q", "-t", "ed25519", "-N", "", "-C", "noggit-test", "-f", _key}), 0);
    }
    else
    {
      // Minimal unencrypted openssh-key-v1 header (cipher "none"); enough for the header sniff.
      QByteArray blob("openssh-key-v1\0", 15);
      blob += QByteArray::fromHex("00000004") + "none";
      writeFile(_key, "-----BEGIN OPENSSH PRIVATE KEY-----\n" + blob.toBase64() + "\n-----END OPENSSH PRIVATE KEY-----\n");
    }
    QFile::setPermissions(_key, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  }

  void init()
  {
    QFile::remove(_known_hosts);
    QFile::remove(_keyscan_out);
  }

  // --- validation / hardening ---------------------------------------------------------------------

  void validatesHostnames()
  {
    QVERIFY(isValidHostname("example.com"));
    QVERIFY(isValidHostname("203.0.113.7"));
    QVERIFY(isValidHostname("ip-10-0-0-1.ec2.internal"));
    QVERIFY(isValidHostname("::1"));
    QVERIFY(!isValidHostname(""));
    QVERIFY(!isValidHostname("-oProxyCommand=touch /tmp/pwned"));
    QVERIFY(!isValidHostname("host name"));
    QVERIFY(!isValidHostname("host;rm -rf ~"));
    QVERIFY(!isValidHostname("$(id)"));
    QVERIFY(!isValidHostname("user@host"));
    QVERIFY(!isValidHostname("fe80::1%eth0"));
    QVERIFY(!isValidHostname("a..b"));
  }

  void validatesUsernames()
  {
    QVERIFY(isValidUsername("ubuntu"));
    QVERIFY(isValidUsername("noggit_ro-1.user"));
    QVERIFY(!isValidUsername(""));
    QVERIFY(!isValidUsername("-l"));
    QVERIFY(!isValidUsername("a b"));
    QVERIFY(!isValidUsername("root;id"));
    QVERIFY(!isValidUsername("x@y"));
  }

  void normalizesFingerprints()
  {
    QString const bare = "uNiVztksCsDhcc0u9e8BujQXVUpKZIDTMczCvj3tD2s";
    QCOMPARE(normalizeFingerprint("SHA256:" + bare), "SHA256:" + bare);
    QCOMPARE(normalizeFingerprint("  " + bare + "  "), "SHA256:" + bare);
    QCOMPARE(normalizeFingerprint(""), QString());
    QCOMPARE(normalizeFingerprint("MD5:aa:bb"), QString());
    QCOMPARE(normalizeFingerprint("SHA256:short"), QString());
  }

  void quotesPathsForSsh()
  {
    QCOMPARE(quoteSshOptionPath("/home/a b/known_hosts"), QString("\"/home/a b/known_hosts\""));
    QCOMPARE(quoteSshOptionPath("/x/100%/k"), QString("\"/x/100%%/k\""));
    QCOMPARE(quoteSshOptionPath("C:\\Users\\k"), QString("\"C:\\\\Users\\\\k\""));
    QVERIFY(quoteSshOptionPath("/x/\"quoted\"").isEmpty());
    QVERIFY(quoteSshOptionPath("/x/${HOME}/k").isEmpty());
    QVERIFY(quoteSshOptionPath("/x/line\nbreak").isEmpty());
    QCOMPARE(escapeSshIdentityPath("/k/%d"), QString("/k/%%d"));
    QVERIFY(escapeSshIdentityPath("/k/${X}").isEmpty());
  }

  void buildsHardenedArguments()
  {
    TunnelConfig c = config();
    c.remote_db_host = "::1";
    QStringList const args = buildSshArguments(c, 40000, "/tmp/kh dir/known_hosts");

    for (QString const& required : {"BatchMode=yes", "ExitOnForwardFailure=yes", "ServerAliveInterval=30",
                                    "ServerAliveCountMax=3", "StrictHostKeyChecking=yes", "ForwardAgent=no",
                                    "ForwardX11=no", "IdentitiesOnly=yes", "PasswordAuthentication=no"})
    {
      QVERIFY2(args.contains(required), qPrintable(required));
    }
    QVERIFY(args.contains("-N"));
    QVERIFY(args.contains("-T"));
    QVERIFY(!args.join(' ').contains("StrictHostKeyChecking=no"));
    QVERIFY(args.contains("UserKnownHostsFile=\"/tmp/kh dir/known_hosts\""));
    QCOMPARE(args.at(args.indexOf("-L") + 1), QString("127.0.0.1:40000:[::1]:3306"));
    QCOMPARE(args.at(args.indexOf("-p") + 1), QString("2222"));
    QCOMPARE(args.at(args.indexOf("-l") + 1), QString("tester"));
    QCOMPARE(args.at(args.indexOf("-i") + 1), _key);
    // Destination is last and preceded by "--", so it can never be parsed as an option.
    QCOMPARE(args.at(args.size() - 2), QString("--"));
    QCOMPARE(args.last(), QString("tunnel.example.test"));

    QStringList const pinned = buildSshArguments(c, 40000, "/k", hostKeyAlgorithmsFor("ssh-rsa"));
    QVERIFY(pinned.contains("HostKeyAlgorithms=rsa-sha2-512,rsa-sha2-256"));
  }

  void rejectsInjectionInConfig()
  {
    TunnelConfig c = config();
    c.ssh_host = "-oProxyCommand=sh";
    QCOMPARE(validateConfig(c, false), ErrorKind::InvalidSettings);
    c = config();
    c.remote_db_host = "127.0.0.1:22:evil";
    QCOMPARE(validateConfig(c, false), ErrorKind::InvalidSettings);
    c = config();
    c.ssh_port = 0;
    QCOMPARE(validateConfig(c, false), ErrorKind::InvalidSettings);
    c = config();
    c.expected_fingerprint = "not-a-fingerprint";
    QCOMPARE(validateConfig(c, false), ErrorKind::InvalidSettings);
    QCOMPARE(validateConfig(config(), false), ErrorKind::None);
  }

  void explainsInvalidSettings()
  {
    TunnelConfig c = config();
    c.ssh_host.clear();
    QVERIFY(invalidSettingsReason(c).contains("SSH host"));
    SshTunnelManager m(options());
    m.start(c);
    QCOMPARE(m.lastError(), ErrorKind::InvalidSettings);
    QVERIFY(m.lastErrorMessage().contains("Enter the SSH host"));
    QVERIFY(invalidSettingsReason(config()).isEmpty());
  }

  // --- private key checks -------------------------------------------------------------------------

  void inspectsPrivateKeys()
  {
    QCOMPARE(inspectPrivateKey(path("missing"), false).error, ErrorKind::KeyMissing);
    QCOMPARE(inspectPrivateKey(_dir.path(), false).error, ErrorKind::KeyNotRegularFile);
    QCOMPARE(inspectPrivateKey(_key, false).error, ErrorKind::None);
    QVERIFY(!inspectPrivateKey(_key, false).encrypted);

    QString const loose = path("id_loose");
    QFile::copy(_key, loose);
    QFile::setPermissions(loose, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    QCOMPARE(inspectPrivateKey(loose, false).error, ErrorKind::KeyPermissions);

    QString const pub = path("id_pub_selected");
    writeFile(pub, "ssh-ed25519 " + fakeEd25519Blob('Z') + " someone@host\n");
    QFile::setPermissions(pub, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QCOMPARE(inspectPrivateKey(pub, false).error, ErrorKind::KeyInvalidFormat);

    QString const ppk = path("id.ppk");
    writeFile(ppk, "PuTTY-User-Key-File-3: ssh-ed25519\nEncryption: none\n");
    QFile::setPermissions(ppk, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QCOMPARE(inspectPrivateKey(ppk, false).error, ErrorKind::KeyInvalidFormat);

    QString const pem = path("id_pem_enc");
    writeFile(pem, "-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: AES-128-CBC,00\n\nAAAA\n-----END RSA PRIVATE KEY-----\n");
    QFile::setPermissions(pem, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QCOMPARE(inspectPrivateKey(pem, false).error, ErrorKind::KeyEncryptedNoAgent);
    QCOMPARE(inspectPrivateKey(pem, true).error, ErrorKind::None);
    QVERIFY(inspectPrivateKey(pem, true).encrypted);
  }

  void detectsEncryptedOpensshKey()
  {
    if (!_have_keygen)
    {
      QSKIP("ssh-keygen not available");
    }
    QString const enc = path("id_encrypted");
    QCOMPARE(QProcess::execute("ssh-keygen", {"-q", "-t", "ed25519", "-N", "test-only-passphrase", "-f", enc}), 0);
    QCOMPARE(inspectPrivateKey(enc, false).error, ErrorKind::KeyEncryptedNoAgent);
    KeyInspection const with_agent = inspectPrivateKey(enc, true);
    QCOMPARE(with_agent.error, ErrorKind::None);
    QVERIFY(with_agent.encrypted);
  }

  void fingerprintMatchesSshKeygen()
  {
    if (!_have_keygen)
    {
      QSKIP("ssh-keygen not available");
    }
    QFile pub(_key + ".pub");
    QVERIFY(pub.open(QIODevice::ReadOnly));
    QByteArray const blob = pub.readAll().split(' ').value(1);
    QProcess keygen;
    keygen.start("ssh-keygen", {"-l", "-E", "sha256", "-f", _key + ".pub"});
    QVERIFY(keygen.waitForFinished());
    QString const expected = QString::fromUtf8(keygen.readAllStandardOutput()).split(' ').value(1);
    QCOMPARE(fingerprintOfBlob(blob), expected);
  }

  // --- stderr classification ----------------------------------------------------------------------

  void classifiesSshErrors_data()
  {
    QTest::addColumn<QString>("text");
    QTest::addColumn<int>("kind");
    auto row = [](char const* name, char const* text, ErrorKind k) { QTest::newRow(name) << QString(text) << int(k); };
    row("auth", "ubuntu@1.2.3.4: Permission denied (publickey).", ErrorKind::KeyRejected);
    row("changed", "WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!\nHost key verification failed.", ErrorKind::HostKeyChanged);
    row("unknown", "No ED25519 host key is known for [h]:22 and you have requested strict checking.\nHost key verification failed.", ErrorKind::HostKeyUnknown);
    row("pin-type", "Unable to negotiate with 1.2.3.4 port 22: no matching host key type found. Their offer: ssh-rsa", ErrorKind::HostKeyPinMismatch);
    row("dns", "ssh: Could not resolve hostname nope.invalid: Name or service not known", ErrorKind::HostNotFound);
    row("refused", "ssh: connect to host 1.2.3.4 port 22: Connection refused", ErrorKind::HostUnreachable);
    row("timeout", "ssh: connect to host 1.2.3.4 port 22: Connection timed out", ErrorKind::HostUnreachable);
    row("bind", "bind [127.0.0.1]:5000: Address already in use\nCould not request local forwarding.", ErrorKind::ForwardFailure);
    row("perm", "@ WARNING: UNPROTECTED PRIVATE KEY FILE! @\nLoad key \"/k\": bad permissions", ErrorKind::KeyPermissions);
    row("format", "Load key \"/k.pub\": invalid format", ErrorKind::KeyInvalidFormat);
    row("remote", "channel 2: open failed: connect failed: Connection refused", ErrorKind::RemoteForwardRefused);
    row("lost", "Timeout, server 1.2.3.4 not responding.", ErrorKind::ConnectionLost);
    row("other", "something unexpected", ErrorKind::Unknown);
  }

  void classifiesSshErrors()
  {
    QFETCH(QString, text);
    QFETCH(int, kind);
    QCOMPARE(int(classifySshStderr(text)), kind);
  }

  void retryPolicy()
  {
    QVERIFY(isRetryable(ErrorKind::ConnectionLost));
    QVERIFY(isRetryable(ErrorKind::HostUnreachable));
    QVERIFY(!isRetryable(ErrorKind::KeyRejected));
    QVERIFY(!isRetryable(ErrorKind::HostKeyChanged));
    QVERIFY(!isRetryable(ErrorKind::HostKeyUnknown));
    QVERIFY(!isRetryable(ErrorKind::HostKeyPinMismatch));
  }

  // --- manager ------------------------------------------------------------------------------------

  void successfulStartup()
  {
    trustTestHost();
    setPlan({"ok"});
    SshTunnelManager m(options());
    QList<State> states;
    connect(&m, &SshTunnelManager::stateChanged, [&](State s) { states << s; });
    m.start(config());
    QCOMPARE(m.state(), State::Starting);
    QVERIFY(waitForState(m, State::Connected));
    QVERIFY(m.localPort() > 0);
    QVERIFY(portAccepts(m.localPort()));
    QCOMPARE(invocations().size(), 1);
    QCOMPARE(states, (QList<State>{State::Starting, State::Connected}));
    QVERIFY(m.statusText().contains("connected"));
    m.stop();
  }

  void ensureReadyWaitsWithoutBlocking()
  {
    trustTestHost();
    setPlan({"ok"});
    SshTunnelManager m(options());
    quint16 port = 0;
    QString error;
    QVERIFY2(m.ensureReady(config(), 5000, &port, &error), qPrintable(error));
    QVERIFY(port > 0);
    QCOMPARE(port, m.localPort());
    // Already connected: returns immediately, no second process.
    QVERIFY(m.ensureReady(config(), 5000, &port, &error));
    QCOMPARE(invocations().size(), 1);
    m.stop();
  }

  void authenticationFailure()
  {
    trustTestHost();
    setPlan({"auth"});
    SshTunnelManager m(options());
    m.start(config());
    QVERIFY(waitForState(m, State::Failed));
    QCOMPARE(m.lastError(), ErrorKind::KeyRejected);
    QVERIFY(m.lastErrorMessage().contains("tester"));
    QTest::qWait(300);
    QCOMPARE(invocations().size(), 1); // not retried
    QCOMPARE(m.localPort(), quint16(0));
  }

  void hostKeyChangedIsHardFailure()
  {
    trustTestHost();
    setPlan({"hostkey_changed"});
    SshTunnelManager m(options());
    m.start(config());
    QVERIFY(waitForState(m, State::Failed));
    QCOMPARE(m.lastError(), ErrorKind::HostKeyChanged);
    QVERIFY(m.lastErrorMessage().contains("CHANGED"));
    QTest::qWait(300);
    QCOMPARE(invocations().size(), 1);
    // ensureReady must not silently retry it either.
    QString error;
    QVERIFY(!m.ensureReady(config(), 1000, nullptr, &error));
    QCOMPARE(invocations().size(), 1);
  }

  void unknownHostRequiresConfirmation()
  {
    writeFile(_keyscan_out, "# tunnel.example.test:2222 SSH-2.0-OpenSSH\n[tunnel.example.test]:2222 ssh-ed25519 " + fakeEd25519Blob('K') + "\n");
    setPlan({"ok"});
    SshTunnelManager m(options());
    QSignalSpy prompt_spy(&m, &SshTunnelManager::hostKeyConfirmationRequired);
    m.start(config());
    QVERIFY(waitForState(m, State::Failed));
    QCOMPARE(m.lastError(), ErrorKind::HostKeyUnknown);
    QCOMPARE(prompt_spy.size(), 1);
    QCOMPARE(invocations().size(), 0); // ssh never ran against an untrusted host
    QCOMPARE(m.pendingHostKeys().size(), 1);
    QString const fp = m.pendingHostKeys().first().fingerprint;
    QVERIFY(m.lastErrorDetails().contains(fp));

    // Declining through ensureReady keeps it failed and does not write known_hosts.
    int prompts = 0;
    m.setHostKeyPrompt([&](TunnelConfig const&, QVector<HostKey> const& keys)
    {
      ++prompts;
      return keys.first().fingerprint == "never";
    });
    QString error;
    QVERIFY(!m.ensureReady(config(), 2000, nullptr, &error));
    QCOMPARE(prompts, 1);
    QVERIFY(m.trustedHostKeys(config()).isEmpty());
    QVERIFY(!m.ensureReady(config(), 2000, nullptr, &error));
    QCOMPARE(prompts, 1); // declined once: not nagged again until an explicit restart

    // Explicit confirmation writes the key and the tunnel comes up.
    m.setHostKeyPrompt([](TunnelConfig const&, QVector<HostKey> const&) { return true; });
    m.restart(config());
    quint16 port = 0;
    QVERIFY2(m.ensureReady(config(), 5000, &port, &error), qPrintable(error));
    QCOMPARE(m.trustedHostKeys(config()).size(), 1);
    QCOMPARE(m.trustedHostKeys(config()).first().fingerprint, fp);
    QVERIFY(QFileInfo(_known_hosts).permissions() == (QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser));
    m.stop();

    QVERIFY(m.forgetHostKeys(config(), &error));
    QVERIFY(m.trustedHostKeys(config()).isEmpty());
  }

  void pinnedFingerprint()
  {
    QByteArray const blob = fakeEd25519Blob('P');
    writeFile(_keyscan_out, "[tunnel.example.test]:2222 ssh-ed25519 " + blob + "\n");

    TunnelConfig wrong = config();
    wrong.expected_fingerprint = fingerprintOfBlob(fakeEd25519Blob('Q'));
    setPlan({"ok"});
    SshTunnelManager m(options());
    m.start(wrong);
    QVERIFY(waitForState(m, State::Failed));
    QCOMPARE(m.lastError(), ErrorKind::HostKeyPinMismatch);
    QCOMPARE(invocations().size(), 0);

    TunnelConfig right = config();
    right.expected_fingerprint = fingerprintOfBlob(blob);
    m.start(right);
    QVERIFY(waitForState(m, State::Connected));
    QVERIFY(invocations().first().contains("HostKeyAlgorithms=ssh-ed25519"));
    QCOMPARE(m.trustedHostKeys(right).size(), 1);
    m.stop();

    // A stale trusted key that disagrees with the pin is replaced by the pinned one (the pin is the
    // user's explicit trust decision); ssh then verifies the server holds that key.
    trustTestHost('S');
    m.start(right);
    QVERIFY(waitForState(m, State::Connected));
    QCOMPARE(m.trustedHostKeys(right).first().fingerprint, right.expected_fingerprint);
    m.stop();
  }

  void localPortCollisionIsRetried()
  {
    trustTestHost();
    setPlan({"bind", "ok"});
    SshTunnelManager m(options());
    m.start(config());
    QVERIFY(waitForState(m, State::Connected));
    auto const calls = invocations();
    QCOMPARE(calls.size(), 2);
    QVERIFY(calls[0].at(calls[0].indexOf("-L") + 1).startsWith("127.0.0.1:"));
    m.stop();
  }

  void localPortCollisionGivesUp()
  {
    trustTestHost();
    setPlan({"bind"});
    TunnelOptions o = options();
    o.max_port_retries = 2;
    SshTunnelManager m(o);
    m.start(config());
    QVERIFY(waitForState(m, State::Failed));
    QCOMPARE(m.lastError(), ErrorKind::ForwardFailure);
    QCOMPARE(invocations().size(), 3);
  }

  void unexpectedExitReconnects()
  {
    trustTestHost();
    setPlan({"die:400", "ok"});
    SshTunnelManager m(options());
    QList<State> states;
    connect(&m, &SshTunnelManager::stateChanged, [&](State s) { states << s; });
    m.start(config());
    QVERIFY(waitForState(m, State::Connected));
    QVERIFY(waitForState(m, State::Reconnecting));
    QVERIFY(waitForState(m, State::Connected));
    QVERIFY(states.contains(State::Reconnecting));
    QCOMPARE(invocations().size(), 2);
    QVERIFY(portAccepts(m.localPort()));
    m.stop();
  }

  void reconnectBackoffIsLimited()
  {
    trustTestHost();
    setPlan({"die:300", "unreachable"});
    SshTunnelManager m(options()); // max 3 reconnect attempts
    m.start(config());
    QVERIFY(waitForState(m, State::Connected));
    QVERIFY(waitForState(m, State::Failed, 10000));
    QCOMPARE(m.lastError(), ErrorKind::HostUnreachable);
    QCOMPARE(invocations().size(), 1 + 3);
  }

  void startTimeout()
  {
    trustTestHost();
    setPlan({"hang"});
    TunnelOptions o = options();
    o.start_timeout_ms = 800;
    SshTunnelManager m(o);
    m.start(config());
    QTest::qWait(200);
    qint64 const pid = m.processId();
    QVERIFY(processAlive(pid));
    QVERIFY(waitForState(m, State::Failed));
    QCOMPARE(m.lastError(), ErrorKind::Timeout);
    QVERIFY(QTest::qWaitFor([&] { return !processAlive(pid); }, 3000));
  }

  void cleanShutdown()
  {
    trustTestHost();
    setPlan({"ok"});
    SshTunnelManager m(options());
    m.start(config());
    QVERIFY(waitForState(m, State::Connected));
    qint64 const pid = m.processId();
    quint16 const port = m.localPort();
    QVERIFY(processAlive(pid));
    m.stop();
    QCOMPARE(m.state(), State::Disabled);
    QCOMPARE(m.localPort(), quint16(0));
    QVERIFY(QTest::qWaitFor([&] { return !processAlive(pid); }, 3000));
    QVERIFY(!portAccepts(port));
    QTest::qWait(300);
    QCOMPARE(invocations().size(), 1); // a requested stop is not "unexpected": no reconnect
  }

  void destructorStopsProcess()
  {
    trustTestHost();
    setPlan({"ok"});
    qint64 pid = 0;
    {
      SshTunnelManager m(options());
      m.start(config());
      QVERIFY(waitForState(m, State::Connected));
      pid = m.processId();
    }
    QVERIFY(QTest::qWaitFor([&] { return !processAlive(pid); }, 3000));
  }

  void neverStartsTwoProcesses()
  {
    trustTestHost();
    setPlan({"ok"});
    SshTunnelManager m(options());
    m.start(config());
    m.start(config());
    m.applyConfig(config());
    QVERIFY(waitForState(m, State::Connected));
    m.start(config());
    m.applyConfig(config());
    QTest::qWait(200);
    QCOMPARE(invocations().size(), 1);

    // Settings change: the old process is stopped before the new one starts.
    qint64 const old_pid = m.processId();
    TunnelConfig changed = config();
    changed.remote_db_port = 3307;
    m.applyConfig(changed);
    QVERIFY(!processAlive(old_pid) || QTest::qWaitFor([&] { return !processAlive(old_pid); }, 3000));
    QVERIFY(waitForState(m, State::Connected));
    QCOMPARE(invocations().size(), 2);
    QVERIFY(invocations().last().contains(QString("127.0.0.1:%1:127.0.0.1:3307").arg(m.localPort())));

    // Disabling in settings stops it.
    changed.enabled = false;
    m.applyConfig(changed);
    QCOMPARE(m.state(), State::Disabled);
  }

  void projectLifecycle()
  {
    trustTestHost();
    setPlan({"ok"});
    SshTunnelManager m(options());
    TunnelConfig a = config();
    m.projectOpened(a);
    QVERIFY(waitForState(m, State::Connected));

    // Window replacement order: project B opens before project A's window is destroyed.
    TunnelConfig b = config();
    b.project_id = "p2";
    b.remote_db_port = 3310;
    m.projectOpened(b);
    m.projectClosed(a.project_id);
    QVERIFY(waitForState(m, State::Connected));
    QCOMPARE(m.config().project_id, QString("p2"));

    m.projectClosed(b.project_id);
    QCOMPARE(m.state(), State::Disabled);
  }

  void missingSshExecutable()
  {
    TunnelOptions o = options();
    o.ssh_executable = path("does-not-exist/ssh");
    SshTunnelManager m(o);
    m.start(config());
    QCOMPARE(m.state(), State::Failed);
    QCOMPARE(m.lastError(), ErrorKind::SshMissing);
  }

  void badKeyFailsBeforeLaunch()
  {
    setPlan({"ok"});
    TunnelConfig c = config();
    c.key_path = path("no-such-key");
    SshTunnelManager m(options());
    m.start(c);
    QCOMPARE(m.state(), State::Failed);
    QCOMPARE(m.lastError(), ErrorKind::KeyMissing);
    QCOMPARE(invocations().size(), 0);
  }

  // Runs the REAL OpenSSH client against a closed loopback port. No server is contacted; this only
  // proves the generated options (quoting, -F none, known-hosts paths) are accepted by ssh itself.
  void realSshAcceptsArguments()
  {
    QString const ssh = QStandardPaths::findExecutable("ssh");
    if (ssh.isEmpty())
    {
      QSKIP("OpenSSH client not installed");
    }
    quint16 const closed_port = SshTunnelManager::pickFreeLocalPort();
    QString const odd_dir = path("dir with space %p");
    QDir().mkpath(odd_dir);
    TunnelOptions o = options();
    o.ssh_executable = ssh;
    o.known_hosts_path = odd_dir + "/known_hosts";
    writeFile(o.known_hosts_path, QString("[127.0.0.1]:%1 ssh-ed25519 ").arg(closed_port).toUtf8() + fakeEd25519Blob('R') + "\n");

    TunnelConfig c = config();
    c.ssh_host = "127.0.0.1";
    c.ssh_port = closed_port;
    SshTunnelManager m(o);
    m.start(c);
    QVERIFY(waitForState(m, State::Failed, 10000));
    QVERIFY2(m.lastError() == ErrorKind::HostUnreachable, qPrintable(m.lastErrorDetails()));
    QVERIFY(!m.lastErrorDetails().contains("Bad configuration option"));
    QVERIFY(!m.lastErrorDetails().contains("usage:"));
  }

  // --- per-project settings -----------------------------------------------------------------------

  void settingsArePerProject()
  {
    QSettings settings;
    settings.clear();
    settings.setValue("project/current_path", "/projects/turtle");
    settings.setValue(Noggit::mysqlSettingKey("enabled"), true);
    settings.setValue(Noggit::mysqlSettingKey(Keys::enabled()), true);
    settings.setValue(Noggit::mysqlSettingKey(Keys::host()), "203.0.113.10");
    settings.setValue(Noggit::mysqlSettingKey(Keys::user()), "alice");
    settings.setValue(Noggit::mysqlSettingKey(Keys::keyPath()), "/home/alice/.ssh/noggit_turtle");
    settings.setValue(Noggit::mysqlSettingKey("server"), "10.0.0.5");

    settings.setValue("project/current_path", "/projects/wotlk");
    settings.setValue(Noggit::mysqlSettingKey(Keys::enabled()), true); // but MySQL itself is off here
    settings.setValue(Noggit::mysqlSettingKey(Keys::host()), "198.51.100.20");
    settings.setValue(Noggit::mysqlSettingKey(Keys::port()), 2200);
    settings.sync();

    TunnelConfig const wotlk = TunnelConfig::fromProjectSettings();
    QVERIFY(!wotlk.enabled);
    QCOMPARE(wotlk.ssh_host, QString("198.51.100.20"));
    QCOMPARE(wotlk.ssh_port, 2200);
    QCOMPARE(wotlk.remote_db_host, QString("127.0.0.1"));
    QCOMPARE(wotlk.remote_db_port, 3306);
    QVERIFY(wotlk.key_path.isEmpty());

    settings.setValue("project/current_path", "/projects/turtle");
    TunnelConfig const turtle = TunnelConfig::fromProjectSettings();
    QVERIFY(turtle.enabled);
    QCOMPARE(turtle.ssh_host, QString("203.0.113.10"));
    QCOMPARE(turtle.ssh_port, 22);
    QCOMPARE(turtle.ssh_user, QString("alice"));
    QVERIFY(turtle != wotlk);
    QCOMPARE(Noggit::mysqlSetting("server", "127.0.0.1").toString(), QString("10.0.0.5"));

    // The legacy global group never switches the tunnel on for a project.
    settings.setValue("project/mysql/enabled", true);
    settings.setValue("project/mysql/ssh_enabled", true);
    settings.setValue("project/current_path", "/projects/fresh");
    QVERIFY(!TunnelConfig::fromProjectSettings().enabled);
  }
};

QTEST_GUILESS_MAIN(SshTunnelTest)
#include "test_ssh_tunnel.moc"
