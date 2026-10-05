#include <noggit/runtime/ClientManager.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>
#include <stdexcept>
using Noggit::Runtime::ClientManager;
void check(bool b,char const* message){if(!b)throw std::runtime_error(message);}
QByteArray read(QString const& path){QFile f(path);check(f.open(QIODevice::ReadOnly),"Cannot read fixture");return f.readAll();}
int main(int argc,char** argv){QCoreApplication app(argc,argv);
  try {
    QTemporaryDir dir;check(dir.isValid(),"No temporary directory");auto realm=dir.filePath("realmlist.wtf");
    QFile file(realm);check(file.open(QIODevice::WriteOnly),"Cannot create fixture");file.write("SET realmlist old.example\nset patchlist patches.example\n");file.close();
    auto exe=dir.filePath("WoW.exe");ClientManager::switchRealm(exe,"127.0.0.1:13724");
    check(read(realm).contains("set realmlist 127.0.0.1:13724"),"Local profile failed");
    check(read(realm).contains("set patchlist patches.example"),"Other settings damaged");
    auto backup=read(realm+".creator-backup");ClientManager::switchRealm(exe,"lightsail.example:3724");
    check(read(realm).contains("set realmlist lightsail.example:3724"),"Production profile failed");
    check(read(realm+".creator-backup")==backup,"Original backup overwritten");
    auto previous=read(realm);bool rejected=false;
    try{ClientManager::switchRealm(exe,"bad.example\nset realmlist evil");}catch(std::exception const&){rejected=true;}
    check(rejected&&read(realm)==previous,"Invalid host altered realm file");
    check(!ClientManager::validProductionEndpoint("https://example.com/path"),"URL accepted");
    check(!ClientManager::validProductionEndpoint("example.com:65536"),"Invalid port accepted");
    check(ClientManager::validProductionEndpoint("192.0.2.1"),"IPv4 rejected");
    QTextStream(stdout)<<"Client profile checks passed\n";return 0;
  }catch(std::exception const& e){QTextStream(stderr)<<e.what()<<'\n';return 1;}
}
