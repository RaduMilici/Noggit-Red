#include "ClientManager.hpp"
#include "RuntimeManager.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QFileDialog>
#include <QMessageBox>
#include <QLabel>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include <QRegularExpression>
#include <QProcess>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <signal.h>
#endif
namespace Noggit::Runtime {
namespace {
QString root() { auto runtime=RuntimeManager::instance();if(!runtime)throw std::runtime_error("Creator runtime is unavailable.");return runtime->root(); }
QString settingsFile() { return root()+"/Workspace/runtime.ini"; }
QString executable() { QSettings s(settingsFile(),QSettings::IniFormat);auto p=s.value("clientExecutable").toString();return p.isEmpty()?QString():QDir(root()).absoluteFilePath(p); }
void require(bool ok,char const* message) {if(!ok)throw std::runtime_error(message);}
// Remember only clients launched by Creator, including across editor restarts.
QString processIdentity(qint64 pid) {
  if(pid<=0)return {};
#ifdef Q_OS_WIN
  HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,DWORD(pid));if(!h)return {};
  FILETIME created,exited,kernel,user;DWORD code=0;
  bool running=GetExitCodeProcess(h,&code)&&code==STILL_ACTIVE&&GetProcessTimes(h,&created,&exited,&kernel,&user);
  CloseHandle(h);return running?QString::number(created.dwHighDateTime)+":"+QString::number(created.dwLowDateTime):QString();
#elif defined(Q_OS_LINUX)
  QFile file(QString("/proc/%1/stat").arg(pid));if(!file.open(QIODevice::ReadOnly))return {};
  auto data=file.readAll();auto fields=data.mid(data.lastIndexOf(')')+2).split(' ');
  return fields.size()>19&&fields[0]!="Z"?QString::fromLatin1(fields[19]):QString();
#else
  return ::kill(pid,0)==0?QString::number(pid):QString();
#endif
}
void checkClientClosed() {
  QSettings s(settingsFile(),QSettings::IniFormat);auto identity=s.value("clientProcessIdentity").toString();
  require(identity.isEmpty()||processIdentity(s.value("clientProcessId").toLongLong())!=identity,
    "Close the WoW client already launched by Creator before switching profiles or starting another test.");
}
QString program(QString const& exe) {
#ifndef Q_OS_WIN
  if(exe.endsWith(".exe",Qt::CaseInsensitive)) {
    QString wine=root()+"/Runtime/Wine/bin/wine";
    require(QFileInfo(wine).isExecutable(),"This installation needs bundled Wine at Runtime/Wine/bin/wine to launch the Windows client.");return wine;
  }
#endif
  require(QFileInfo(exe).isExecutable(),"The selected client is not executable.");return exe;
}
}
bool ClientManager::validProductionEndpoint(QString const& host) {
  static QRegularExpression syntax("^(?=.{1,253}$)[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?(?::[0-9]{1,5})?$");
  if(!syntax.match(host).hasMatch()) return false;
  auto parts=host.split(':');return parts.size()==1||(parts[1].toUInt()>0&&parts[1].toUInt()<=65535);
}
void ClientManager::switchRealm(QString const& exe,QString const& endpoint) {
  require(validProductionEndpoint(endpoint),"Enter a hostname or IPv4 address, optionally followed by a port.");
  QString path=QFileInfo(exe).absolutePath()+"/realmlist.wtf",text;
  if(QFileInfo::exists(path)) {
    QFile file(path);require(file.open(QIODevice::ReadOnly),"Cannot read realmlist.wtf.");text=QString::fromUtf8(file.readAll());file.close();
    require(QFileInfo::exists(path+".creator-backup")||QFile::copy(path,path+".creator-backup"),"Cannot back up realmlist.wtf.");
  }
  QRegularExpression pattern("^\\s*set\\s+realmlist\\s+.*$",QRegularExpression::MultilineOption|QRegularExpression::CaseInsensitiveOption);
  QString setting="set realmlist "+endpoint;
  if(text.contains(pattern))text.replace(pattern,setting);else text+='\n'+setting+'\n';
  QSaveFile file(path);auto bytes=text.toUtf8();require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit(),"Cannot update realmlist.wtf.");
}
bool ClientManager::configure(QWidget* parent) {
  QSettings settings(settingsFile(),QSettings::IniFormat);
  QDialog dialog(parent);dialog.setWindowTitle("Client profiles");auto layout=new QVBoxLayout(&dialog);auto form=new QFormLayout;layout->addLayout(form);
  auto client=new QLineEdit(executable());auto browse=new QPushButton("Choose WoW client…");form->addRow("Shared client",client);form->addRow(browse);
  auto host=new QLineEdit(settings.value("productionRealm").toString());host->setPlaceholderText("Your Lightsail hostname or IP");form->addRow("Play Production",host);
  auto character=new QLineEdit(settings.value("testCharacterName").toString());form->addRow("Local test character",character);
  auto note=new QLabel("Use an existing local character. Close WoW before switching profiles. No password is stored and no content is uploaded.");note->setWordWrap(true);layout->addWidget(note);
  auto buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);layout->addWidget(buttons);
  QObject::connect(browse,&QPushButton::clicked,&dialog,[&]{auto path=QFileDialog::getOpenFileName(&dialog,"Choose WoW client",root(),"WoW client (WoW.exe wow.exe);;All files (*)");if(!path.isEmpty())client->setText(path);});
  QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
  QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{
    if(!QFileInfo::exists(client->text())||QFileInfo(client->text()).isDir()){QMessageBox::warning(&dialog,"Client profiles","Choose the WoW executable.");return;}
    if(!host->text().trimmed().isEmpty()&&!validProductionEndpoint(host->text().trimmed())){QMessageBox::warning(&dialog,"Client profiles","Enter a hostname or IP, without a URL scheme or path.");return;}
    settings.setValue("clientExecutable",QDir(root()).relativeFilePath(client->text()));settings.setValue("productionRealm",host->text().trimmed());settings.setValue("testCharacterName",character->text().trimmed());settings.sync();dialog.accept();
  });return dialog.exec()==QDialog::Accepted;
}
bool ClientManager::prepare(QWidget* parent,Profile profile) {
  QSettings s(settingsFile(),QSettings::IniFormat);
  if(executable().isEmpty()||!QFileInfo::exists(executable())||(profile==Profile::PlayProduction&&s.value("productionRealm").toString().isEmpty()))
    if(!configure(parent))return false;
  checkClientClosed();program(executable());return true;
}
void ClientManager::launch(Profile profile) {
  checkClientClosed();auto exe=executable();auto binary=program(exe);QSettings s(settingsFile(),QSettings::IniFormat);
  QString realm=profile==Profile::TestLocal?"127.0.0.1:13724":s.value("productionRealm").toString();
  if(profile==Profile::TestLocal)require(RuntimeManager::instance()->status(2)=="Running","Wait for the local server to finish starting.");
  // Production deliberately has no save, restart, test request, or database operation.
  switchRealm(exe,realm);QStringList args;if(binary!=exe)args<<exe;qint64 pid=0;
  require(QProcess::startDetached(binary,args,QFileInfo(exe).absolutePath(),&pid),"Could not launch the WoW client.");
  s.setValue("clientProcessId",pid);s.setValue("clientProcessIdentity",processIdentity(pid));
}
}
