#include "LocalClientLauncher.hpp"
#include "RuntimeManager.hpp"
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include <QRegularExpression>
#include <QProcess>
#include <stdexcept>
namespace Noggit::Runtime {
void launchLocalClient(QWidget* parent) {
  try {
    auto manager=RuntimeManager::instance();
    if(!manager||manager->status(2)!="Running") throw std::runtime_error("Use Test Locally and wait until all three services are Running, then launch the client.");
    QSettings settings(manager->root()+"/Workspace/runtime.ini",QSettings::IniFormat);
    auto relative=settings.value("clientExecutable").toString();
    QString executable=relative.isEmpty()?QString():QDir(manager->root()).absoluteFilePath(relative);
    if(executable.isEmpty()||!QFileInfo::exists(executable)) {
      executable=QFileDialog::getOpenFileName(parent,"Choose your local Vanilla / Tortoise WoW client",manager->root(),"WoW client (WoW.exe wow.exe);;All files (*)");
      if(executable.isEmpty())return;
      settings.setValue("clientExecutable",QDir(manager->root()).relativeFilePath(executable));
    }
    auto directory=QFileInfo(executable).absolutePath();auto realmlist=QDir(directory).filePath("realmlist.wtf");
    QString text;
    if(QFileInfo::exists(realmlist)) {
      QFile file(realmlist);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot read the client's realm configuration.");text=QString::fromUtf8(file.readAll());file.close();
      if(!QFileInfo::exists(realmlist+".creator-backup")&&!QFile::copy(realmlist,realmlist+".creator-backup"))throw std::runtime_error("Cannot back up the client's realm configuration.");
    }
    auto pattern=QRegularExpression("^\\s*set\\s+realmlist\\s+.*$",QRegularExpression::MultilineOption|QRegularExpression::CaseInsensitiveOption);
    QString local="set realmlist 127.0.0.1:13724";
    if(text.contains(pattern))text.replace(pattern,local);else text+='\n'+local+'\n';
    QString program=executable;QStringList args;
#ifndef Q_OS_WIN
    if(executable.endsWith(".exe",Qt::CaseInsensitive)) {
      program=manager->root()+"/Runtime/Wine/bin/wine";args<<executable;
      if(!QFileInfo(program).isExecutable())throw std::runtime_error("This Linux installation needs a bundled Wine launcher at Runtime/Wine/bin/wine to launch the Windows WoW client. The local server can still be tested with an existing client launcher.");
    }
#endif
    QSaveFile output(realmlist);auto bytes=text.toUtf8();if(!output.open(QIODevice::WriteOnly)||output.write(bytes)!=bytes.size()||!output.commit())throw std::runtime_error("Cannot set the client's local realm.");
    if(!QProcess::startDetached(program,args,directory))throw std::runtime_error("The local WoW client could not be launched.");
  }catch(std::exception const& e){QMessageBox::warning(parent,"Launch local client",e.what());}
}
}
