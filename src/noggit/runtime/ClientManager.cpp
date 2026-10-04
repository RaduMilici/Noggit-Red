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
#include <QCoreApplication>
#include <noggit/creator/Services.hpp>
#include <noggit/creator/AccountDialog.hpp>
#include <QComboBox>
#include <QStandardPaths>
#include <QVector>
#include <algorithm>
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
QString executable() { QSettings s(settingsFile(),QSettings::IniFormat);auto p=s.value("clientExecutable").toString();// cleanPath: sandboxed launchers (Flatpak Bottles) cannot follow "..", e.g. through the
// Creator folder they have no access to.
return p.isEmpty()?QString():QDir::cleanPath(QDir(root()).absoluteFilePath(p)); }
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
struct Launcher { QString id, name; };
// Ways to run the Windows client on Linux/macOS, in order of preference:
// bundled Wine, Bottles bottles (Flatpak or native), then Wine from PATH.
QVector<Launcher> launchers() {
  QVector<Launcher> result;
#ifndef Q_OS_WIN
  if(QFileInfo(root()+"/Runtime/Wine/bin/wine").isExecutable()) result.push_back({"wine:bundled","Bundled Wine"});
  auto bottles=[&](QString const& prefix,QString const& label,QString const& dir) {
    for(auto const& bottle:QDir(dir).entryList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name))
      if(QFileInfo::exists(dir+"/"+bottle+"/bottle.yml")) result.push_back({prefix+bottle,label+bottle});
  };
  if(!QStandardPaths::findExecutable("flatpak").isEmpty())
    bottles("flatpak-bottles:","Bottles: ",QDir::homePath()+"/.var/app/com.usebottles.bottles/data/bottles/bottles");
  if(!QStandardPaths::findExecutable("bottles-cli").isEmpty())
    bottles("bottles:","Bottles (native): ",QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+"/bottles/bottles");
  if(!QStandardPaths::findExecutable("wine").isEmpty()) result.push_back({"wine:system","System Wine"});
#endif
  return result;
}
// Program and arguments that start the client, honoring the launcher chosen in Client profiles.
QStringList command(QString const& exe) {
#ifndef Q_OS_WIN
  if(exe.endsWith(".exe",Qt::CaseInsensitive)) {
    auto available=launchers();
    require(!available.isEmpty(),"No way to run the Windows client was found. Install Bottles or Wine, or bundle Wine at Runtime/Wine/bin/wine.");
    auto chosen=QSettings(settingsFile(),QSettings::IniFormat).value("clientLauncher").toString();
    auto launcher=available.front();
    for(auto const& l:available) if(l.id==chosen) launcher=l;
    auto type=launcher.id.section(':',0,0), detail=launcher.id.section(':',1);
    if(type=="flatpak-bottles")
      return {QStandardPaths::findExecutable("flatpak"),"run","--command=bottles-cli","com.usebottles.bottles","run","-b",detail,"-e",exe};
    if(type=="bottles") return {QStandardPaths::findExecutable("bottles-cli"),"run","-b",detail,"-e",exe};
    return {detail=="bundled"?root()+"/Runtime/Wine/bin/wine":QStandardPaths::findExecutable("wine"),exe};
  }
#endif
  require(QFileInfo(exe).isExecutable(),"The selected client is not executable.");return {exe};
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
  auto launcher=new QComboBox;auto available=launchers();
  for(auto const& l:available)launcher->addItem(l.name,l.id);
  launcher->setCurrentIndex(std::max(0,launcher->findData(settings.value("clientLauncher"))));
#ifndef Q_OS_WIN
  form->addRow("Run with",launcher);
  if(available.isEmpty())form->addRow(new QLabel("Install Bottles or Wine to run the Windows client."));
#else
  launcher->hide();
#endif
  auto host=new QLineEdit(settings.value("productionRealm").toString());host->setPlaceholderText("Your Lightsail hostname or IP");form->addRow("Play Production",host);
  auto character=new QLineEdit(settings.value("testCharacterName").toString());form->addRow("Local test character",character);
  auto accounts=new QLabel;accounts->setWordWrap(true);auto createAccount=new QPushButton("Create Local Account…");
  auto refreshAccounts=[accounts,createAccount]{
    bool ready=qApp->property("creatorDatabaseReady").toBool();createAccount->setEnabled(ready);
    if(!ready){accounts->setText("Start the local server to see or create accounts.");return;}
    try{auto names=Creator::AccountService::list();accounts->setText(names.isEmpty()?"No local account yet. Create one to log in to WoW.":names.join(", "));}
    catch(std::exception const& e){accounts->setText(QString::fromUtf8(e.what()));}
  };
  refreshAccounts();form->addRow("Local account",accounts);form->addRow(createAccount);
  QObject::connect(RuntimeManager::instance(),&RuntimeManager::changed,&dialog,refreshAccounts);
  QObject::connect(createAccount,&QPushButton::clicked,&dialog,[&]{if(!Creator::createLocalAccount(&dialog).isEmpty())refreshAccounts();});
  auto note=new QLabel("Use an existing local character. Close WoW before switching profiles. No password is stored and no content is uploaded.");note->setWordWrap(true);layout->addWidget(note);
  auto buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);layout->addWidget(buttons);
  // Qt's own dialog: the GTK chooser opened from this modal dialog stays invisible on GNOME Wayland.
  QObject::connect(browse,&QPushButton::clicked,&dialog,[&]{
    auto start=client->text().isEmpty()?root():QFileInfo(client->text()).absolutePath();
    auto path=QFileDialog::getOpenFileName(&dialog,"Choose WoW client",start,"WoW client (*.exe *.EXE);;All files (*)",nullptr,QFileDialog::DontUseNativeDialog);
    if(!path.isEmpty())client->setText(path);
  });
  QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
  QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{
    if(!QFileInfo::exists(client->text())||QFileInfo(client->text()).isDir()){QMessageBox::warning(&dialog,"Client profiles","Choose the WoW executable.");return;}
    if(!host->text().trimmed().isEmpty()&&!validProductionEndpoint(host->text().trimmed())){QMessageBox::warning(&dialog,"Client profiles","Enter a hostname or IP, without a URL scheme or path.");return;}
    settings.setValue("clientExecutable",QDir(root()).relativeFilePath(client->text()));settings.setValue("productionRealm",host->text().trimmed());settings.setValue("testCharacterName",character->text().trimmed());settings.setValue("clientLauncher",launcher->currentData().toString());settings.sync();dialog.accept();
  });return dialog.exec()==QDialog::Accepted;
}
bool ClientManager::prepare(QWidget* parent,Profile profile) {
  QSettings s(settingsFile(),QSettings::IniFormat);
  if(executable().isEmpty()||!QFileInfo::exists(executable())||(profile==Profile::PlayProduction&&s.value("productionRealm").toString().isEmpty()))
    if(!configure(parent))return false;
  checkClientClosed();command(executable());return true;
}
void ClientManager::launch(Profile profile) {
  checkClientClosed();auto exe=executable();auto args=command(exe);auto binary=args.takeFirst();QSettings s(settingsFile(),QSettings::IniFormat);
  QString realm=profile==Profile::TestLocal?"127.0.0.1:13724":s.value("productionRealm").toString();
  if(profile==Profile::TestLocal)require(RuntimeManager::instance()->status(2)=="Running","Wait for the local server to finish starting.");
  // Production deliberately has no save, restart, test request, or database operation.
  if(beforeLaunch)beforeLaunch(profile);
  switchRealm(exe,realm);qint64 pid=0;
  require(QProcess::startDetached(binary,args,QFileInfo(exe).absolutePath(),&pid),"Could not launch the WoW client.");
  s.setValue("clientProcessId",pid);s.setValue("clientProcessIdentity",processIdentity(pid));
}
}
