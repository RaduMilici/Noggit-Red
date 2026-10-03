#include "../../src/noggit/runtime/RuntimeManager.hpp"
#include "../../src/noggit/runtime/LocalServerPanel.hpp"
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QMainWindow>
#include <QDockWidget>
#include <QToolButton>
#include <QMenu>
#include <QTcpServer>
#include <QTest>
using Noggit::Runtime::RuntimeManager;
class RuntimeTests : public QObject {
  Q_OBJECT
  void put(QString const& path, QByteArray const& data) {
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(data); file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  }
  void bundle(QString const& root) {
    QByteArray script = R"PY(#!/usr/bin/env python3
import sys,socket,time,os,signal
name=os.path.basename(sys.argv[0])
if name=='mariadb':
    try:
        s=socket.create_connection(('127.0.0.1',13306),.2)
        if '--execute=SHUTDOWN' in sys.argv: s.sendall(b'stop')
        s.close(); sys.exit(0)
    except OSError: sys.exit(1)
port={'mariadbd':13306,'realmd':13724,'mangosd':18085}[name]
s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(('127.0.0.1',port));s.listen();s.settimeout(.1)
import select
while True:
    if name=='mangosd' and select.select([sys.stdin],[],[],0)[0]:
        if 'shutdown' in sys.stdin.readline(): break
    try:
        c,a=s.accept();c.settimeout(.05)
        try:
            if c.recv(64)==b'stop': break
        except socket.timeout: pass
        c.close()
    except socket.timeout: pass
)PY";
    for (QString name : {"MariaDB/bin/mariadbd", "MariaDB/bin/mariadb", "realmd/realmd", "mangosd/mangosd"}) put(root+"/Runtime/"+name, script);
    put(root+"/Runtime/DatabaseSeed/mysql/seed", "seed");
    put(root+"/Runtime/realmd/realmd.conf.dist", "[RealmdConf]\nConfVersion=1\nBindIP = \"0.0.0.0\"\n");
    put(root+"/Runtime/mangosd/mangosd.conf.dist", "[MangosdConf]\nConfVersion=1\n");
  }
private slots:
  void missingBundle() {
    QTemporaryDir dir; RuntimeManager m(dir.path()); m.start();
    QVERIFY(!m.active()); QVERIFY(m.error().contains("missing"));
  }
  void occupiedPort() {
    QTemporaryDir dir; bundle(dir.path()); QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost,13306));
    RuntimeManager m(dir.path()); m.start(); QVERIFY(m.error().contains("in use"));
  }
  void lifecycle() {
    QTemporaryDir dir(QDir::tempPath()+"/creator space-XXXXXX"); bundle(dir.path());
    RuntimeManager m(dir.path(), qApp);
    QMainWindow window; Noggit::Runtime::addLocalServerPanel(&window);
    auto indicator=window.findChild<QToolButton*>("localServerIndicator");
    QVERIFY(indicator);QVERIFY(indicator->menu());QVERIFY(!indicator->menu()->isVisible());
    QVERIFY(!window.findChild<QDockWidget*>("localServerDock"));
    QVERIFY(indicator->toolTip().contains("Database: Stopped"));
    m.start(); m.start();
    QTRY_COMPARE_WITH_TIMEOUT(m.status(2), QString("Running"), 10000);
    QVERIFY(qApp->property("creatorDatabaseReady").toBool());
    QVERIFY(indicator->toolTip().contains("World Server: Running"));
    put(dir.path()+"/Database/data/designer-work", "preserve");
    m.restart();
    QTRY_COMPARE_WITH_TIMEOUT(m.status(2), QString("Running"), 10000);
    QVERIFY(QFileInfo::exists(dir.path()+"/Database/data/designer-work"));
    QFile config(dir.path()+"/Workspace/mangosd.conf"); QVERIFY(config.open(QIODevice::ReadOnly));
    auto data=config.readAll(); QVERIFY(data.contains("127.0.0.1;13306")); QVERIFY(!data.contains(dir.path().toUtf8()));
    m.stop();
    QTRY_COMPARE_WITH_TIMEOUT(m.status(0), QString("Stopped"), 10000);
    QVERIFY(!qApp->property("creatorDatabaseReady").toBool());
    m.start(); QTRY_COMPARE_WITH_TIMEOUT(m.status(2), QString("Running"), 10000);
    m.shutdown();
    for (int port : {13306,13724,18085}) { QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost,port)); }
  }
  void testLocallySavesAfterColdStartAndCanCancel() {
    QTemporaryDir dir; bundle(dir.path()); RuntimeManager m(dir.path());
    int saves=0; bool allow=false;
    connect(&m,&RuntimeManager::beforeLocalTest,this,[&](bool* proceed) {
      ++saves; QVERIFY(qApp->property("creatorDatabaseReady").toBool()); *proceed=allow;
    });
    m.testLocally();
    QTRY_COMPARE_WITH_TIMEOUT(saves,1,10000);
    QCOMPARE(m.status(2),QString("Running")); QVERIFY(!m.stopping());
    allow=true;m.testLocally();QCOMPARE(saves,2);
    QTRY_COMPARE_WITH_TIMEOUT(m.status(2),QString("Running"),10000);
    m.stop();QTRY_VERIFY_WITH_TIMEOUT(!m.stopping(),10000);
    QCOMPARE(m.status(0),QString("Stopped"));
  }
  void lockedInstallation() {
    QTemporaryDir dir; bundle(dir.path());
    QVERIFY(QDir().mkpath(dir.path()+"/Database"));
    QLockFile lock(dir.path()+"/Database/runtime.lock"); QVERIFY(lock.tryLock());
    RuntimeManager m(dir.path()); m.start();
    QVERIFY(!m.active()); QVERIFY(m.error().contains("another Creator"));
  }
  void childFailureRollsBack() {
    QTemporaryDir dir; bundle(dir.path());
    put(dir.path()+"/Runtime/mangosd/mangosd", "#!/bin/sh\nexit 7\n");
    RuntimeManager m(dir.path()); m.start();
    QTRY_VERIFY_WITH_TIMEOUT(!m.error().isEmpty(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!m.stopping(), 10000);
    QCOMPARE(m.status(0), QString("Stopped")); QCOMPARE(m.status(1), QString("Stopped"));
    QVERIFY(!qApp->property("creatorDatabaseReady").toBool());
  }
  void shutdownDuringStartupIsFinal() {
    QTemporaryDir dir; bundle(dir.path()); RuntimeManager m(dir.path());
    m.start();
    QTRY_COMPARE_WITH_TIMEOUT(m.status(0), QString("Running"), 10000);
    m.shutdown(); m.shutdown(); // Explicit window cleanup plus destructor is safe.
    QTest::qWait(500);
    for (int port : {13306,13724,18085}) { QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost,port)); }
    m.start(); // A queued startup cannot resurrect a shutting-down runtime.
    QTest::qWait(300);
    for (int port : {13306,13724,18085}) { QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost,port)); }
  }
  void stopDuringStartup() {
    QTemporaryDir dir; bundle(dir.path()); RuntimeManager m(dir.path());
    m.start(); m.stop(); QTest::qWait(1500);
    QCOMPARE(m.status(0), QString("Stopped")); QCOMPARE(m.status(2), QString("Stopped"));
  }
};
QTEST_MAIN(RuntimeTests)
#include "RuntimeTests.moc"
