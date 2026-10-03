#include "../../src/noggit/runtime/RuntimeProcess.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QElapsedTimer>
#include <QTextStream>
#include <cstdlib>
#ifdef Q_OS_LINUX
#include <signal.h>
#include <unistd.h>
#endif
// Self-contained fake child: no database, server ports, or external commands.
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);
#if !defined(Q_OS_LINUX) && !defined(Q_OS_WIN)
  return 0;
#else
  if(argc==3 && QString(argv[1])=="child") {
    QFile file(QString::fromLocal8Bit(argv[2]));
    if(!file.open(QIODevice::WriteOnly))return 2;
    file.write(QByteArray::number(QCoreApplication::applicationPid()));file.close();
    return app.exec();
  }
  if(argc==3 && QString(argv[1])=="parent") {
    Noggit::Runtime::RuntimeProcess child;
    child.start(QCoreApplication::applicationFilePath(),{"child",QString::fromLocal8Bit(argv[2])});
    if(!child.waitForStarted(3000))return 3;
    return app.exec();
  }
  QTemporaryDir dir;QString path=dir.filePath("child.pid");QProcess parent;
  parent.start(QCoreApplication::applicationFilePath(),{"parent",path});
  if(!parent.waitForStarted(3000))return 4;
  QElapsedTimer timer;timer.start();qint64 pid=0;
  while(timer.elapsed()<3000&&!pid) {
    QFile file(path);if(file.open(QIODevice::ReadOnly))pid=file.readAll().toLongLong();
    if(!pid)QThread::msleep(10);
  }
#ifdef Q_OS_WIN
  HANDLE childHandle=pid?OpenProcess(SYNCHRONIZE|PROCESS_TERMINATE,FALSE,DWORD(pid)):nullptr;
#endif
  parent.kill();parent.waitForFinished(3000);
  if(!pid)return 5;
#ifdef Q_OS_WIN
  if(!childHandle) {QTextStream(stderr)<<"Could not observe child process\n";return 7;}
  bool exited=WaitForSingleObject(childHandle,3000)==WAIT_OBJECT_0;
  if(!exited)TerminateProcess(childHandle,1);
  CloseHandle(childHandle);
  QTextStream(exited?stdout:stderr)<<(exited?"Windows job cleanup passed\n":"Child survived parent termination\n");
  return exited?0:6;
#else
  timer.restart();
  while(timer.elapsed()<3000) {
    QFile stat(QString("/proc/%1/stat").arg(pid));
    if(!stat.open(QIODevice::ReadOnly)) {QTextStream(stdout)<<"Parent-death cleanup passed\n";return 0;}
    auto bytes=stat.readAll();
    if(bytes.mid(bytes.lastIndexOf(')')+2,1)=="Z") {QTextStream(stdout)<<"Parent-death cleanup passed (child exited)\n";return 0;}
    QThread::msleep(10);
  }
  ::kill(pid,SIGKILL);QTextStream(stderr)<<"Child survived parent termination\n";return 6;
#endif
#endif
}
