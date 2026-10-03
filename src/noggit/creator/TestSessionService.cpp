#include "TestSessionService.hpp"
#include "Database.hpp"
#include "AccountDialog.hpp"
#include <noggit/runtime/RuntimeManager.hpp>
#include <noggit/runtime/ClientManager.hpp>
#include "../../../etc/creator-test/mod-creator-test/src/CreatorTestProtocol.hpp"
#include <QCoreApplication>
#include <QWidget>
#include <QInputDialog>
#include <QMessageBox>
#include <QSettings>
#include <QSaveFile>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <QDateTime>
#include <cmath>
#include <sstream>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
void require(bool condition,char const* message){if(!condition)throw std::runtime_error(message);}
void put(QString const& path,QByteArray const& bytes) {
  QSaveFile file(path);require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit(),"Cannot record the local test request.");
}
Position positionOf(Fields const& r) {return {r["map"].toUInt(),r["position_x"].toFloat(),r["position_y"].toFloat(),r["position_z"].toFloat(),r["orientation"].toFloat()};}
}
TestSessionService* TestSessionService::instance() {
  auto runtime=Runtime::RuntimeManager::instance();if(!runtime)return nullptr;
  auto service=runtime->findChild<TestSessionService*>("testSession",Qt::FindDirectChildrenOnly);
  return service?service:new TestSessionService(runtime);
}
TestSessionService::TestSessionService(Runtime::RuntimeManager* runtime):QObject(runtime),_runtime(runtime) {
  setObjectName("testSession");_timer.setInterval(250);
  connect(&_timer,&QTimer::timeout,this,&TestSessionService::tick);
  connect(runtime,&Runtime::RuntimeManager::aboutToShutdown,this,&TestSessionService::cancel);
}
void TestSessionService::testHere(QWidget* parent,Position const& p){begin(parent,Here,0,p);}
void TestSessionService::testGameObject(QWidget* parent,Position const& p){begin(parent,NpcTarget,0,p);}
void TestSessionService::testNpc(QWidget* parent,Position const& p){begin(parent,NpcTarget,0,p);}
void TestSessionService::testQuest(QWidget* parent,Id quest){begin(parent,QuestTarget,quest);}
void TestSessionService::testLocal(QWidget* parent){begin(parent,None,0);}
void TestSessionService::begin(QWidget* parent,Target target,Id id,Position position) {
  if(busy()){QMessageBox::information(parent,"Local test","A local test is already in progress. Finish or cancel it first.");return;}
  _parent=parent;
  try {
    require(!_runtime->stopping(),"Wait for the local runtime to finish stopping, then try again.");
    if(!Runtime::ClientManager::prepare(parent,Runtime::ClientManager::Profile::TestLocal))return;
    _target=target;_id=id;_position=position;_token.clear();_loginHint.clear();
    _phase=WaitDatabase;_status="Starting local database…";_deadline.start();_moduleDeadline.invalidate();emit changed();
    _runtime->start();_timer.start();
  }catch(std::exception const& e){finish(QString::fromUtf8(e.what()),true);}
}
void TestSessionService::prepareRequest() {
  // Save callbacks belong to the world/editor. They run synchronously before any restart.
  require(_runtime->saveForTest(),"Local test cancelled because pending NPC changes could not be saved.");
  if(_target==None) {
    // A plain local launch must not replay a request left by an interrupted session.
    auto path=_runtime->root()+"/Workspace/creator-test.request";
    require(!QFileInfo::exists(path)||QFile::remove(path),"Cannot clear the previous local test request.");
    // A fresh installation has no login; offer one before launching WoW.
    if(AccountService::list().isEmpty())
      require(!createLocalAccount(_parent).isEmpty(),"Local test cancelled. Create a local account to log in to WoW.");
    return;
  }
  if(_target==QuestTarget) {
    QVector<Fields> rows;
    { Database db;
      QString where="c.id IN (SELECT id FROM creature_questrelation WHERE quest="+QString::number(_id)+")";
      rows=db.query("SELECT c.*,m.map_name,t.name AS npc_name FROM creature c JOIN creature_template t ON t.entry=c.id LEFT JOIN map_template m ON m.entry=c.map WHERE "+where+" ORDER BY c.map,c.guid LIMIT 100");
    }
    require(!rows.isEmpty(),"This NPC or quest giver has no saved placement. Place it in the world first.");
    int choice=0;
    if(rows.size()>1) {
      QStringList names;for(int i=0;i<rows.size();++i)names<<QString("%1 — %2 — placement %3").arg(rows[i]["npc_name"].toString(),rows[i]["map_name"].toString()).arg(i+1);
      bool ok=false;auto selected=QInputDialog::getItem(_parent,"Test Quest","Choose the giver placement to test",names,0,false,&ok);
      if(!ok)throw std::runtime_error("Local test cancelled.");choice=names.indexOf(selected);
    }
    _position=positionOf(rows[choice]);
  }
  if(_target==NpcTarget||_target==QuestTarget) {
    _position.x+=2.f*std::cos(_position.orientation);_position.y+=2.f*std::sin(_position.orientation);
    _position.orientation=std::fmod(_position.orientation+3.14159265f,6.2831853f);
  }
  _position.z+=0.5f;
  _position.orientation=std::fmod(_position.orientation,6.2831853f);
  if(_position.orientation<0)_position.orientation+=6.2831853f;
  QSettings settings(_runtime->root()+"/Workspace/runtime.ini",QSettings::IniFormat);
  auto name=settings.value("testCharacterName").toString();
  QVector<Fields> characters;
  { Database db;characters=db.query("SELECT c.guid,c.account,c.name,a.username FROM characters.characters c JOIN realmd.account a ON a.id=c.account WHERE c.name<>'' ORDER BY c.name"); }
  require(!characters.isEmpty(),"No local character exists yet. Use Test Local to launch the client, create a character, close WoW, then use Test Here again.");
  int selected=-1;for(int i=0;i<characters.size();++i)if(characters[i]["name"].toString().compare(name,Qt::CaseInsensitive)==0)selected=i;
  if(selected<0) {
    QStringList names;for(auto const& c:characters)names<<c["name"].toString()+" — account "+c["username"].toString();
    bool ok=false;auto chosen=QInputDialog::getItem(_parent,"Local test character","Log in with this existing local character",names,0,false,&ok);
    if(!ok)throw std::runtime_error("Local test cancelled.");selected=names.indexOf(chosen);
  }
  auto character=characters[selected];settings.setValue("testCharacterName",character["name"].toString());
  _loginHint="Log in as "+character["name"].toString()+" (local account "+character["username"].toString()+"). The test expires in 15 minutes.";
  _token=QUuid::createUuid().toString(QUuid::Id128);
  _expires=QDateTime::currentSecsSinceEpoch()+900;
  CreatorTest::Request request{_token.toStdString(),character["guid"].toUInt(),character["account"].toUInt(),_position.map,
    _position.x,_position.y,_position.z,_position.orientation,_expires};
  require(CreatorTest::valid(request,QDateTime::currentSecsSinceEpoch()),"The selected test location is invalid.");
  std::ostringstream out;CreatorTest::write(out,request);
  QFile::remove(_runtime->root()+"/Workspace/creator-test.ready");QFile::remove(_runtime->root()+"/Workspace/creator-test.result");
  put(_runtime->root()+"/Workspace/creator-test.request",QByteArray::fromStdString(out.str()));
}
void TestSessionService::tick() {
  if(_phase==Idle||_phase==Preparing)return;
  try {
    require(_runtime->error().isEmpty(),qPrintable(_runtime->error()));
    if(_phase!=WaitLogin)require(_deadline.elapsed()<180000,"Local testing timed out while starting the servers.");
    if(_phase==WaitDatabase&&qApp->property("creatorDatabaseReady").toBool()) {
      _phase=Preparing;prepareRequest();_phase=WaitRestart;
      _status="Restarting the local server with saved changes…";emit changed();_runtime->restart();return;
    }
    if(_phase==WaitRestart&&_runtime->status(2)=="Running"&&!_runtime->stopping()) {
      if(!_token.isEmpty()) {
        QFile ready(_runtime->root()+"/Workspace/creator-test.ready");
        bool armed=ready.open(QIODevice::ReadOnly)&&QString::fromUtf8(ready.readAll()).trimmed()==_token;
        if(!armed){if(!_moduleDeadline.isValid())_moduleDeadline.start();require(_moduleDeadline.elapsed()<5000,"The local worldserver does not acknowledge creator-test. Build and package the supplied module, then retry.");return;}
      }
      Runtime::ClientManager::launch(Runtime::ClientManager::Profile::TestLocal);
      if(_token.isEmpty()){finish("Local client launched. Log in with your local account.");return;}
      _phase=WaitLogin;_deadline.restart();_status=_loginHint;emit changed();return;
    }
    if(_phase==WaitLogin) {
      require(_runtime->status(2)=="Running","The local server stopped; the test was cancelled.");
      require(QDateTime::currentSecsSinceEpoch()<_expires,"The test request expired. Click Test Here or Test NPC again.");
      QFile result(_runtime->root()+"/Workspace/creator-test.result");
      if(result.open(QIODevice::ReadOnly)) {
        auto lines=QString::fromUtf8(result.readAll()).split('\n');
        if(lines.value(0)==_token){require(lines.value(1)=="OK",qPrintable(lines.value(1)));finish("The server sent your test character to the selected location.");}
      }
    }
  }catch(std::exception const& e){finish(QString::fromUtf8(e.what()),true);}
}
void TestSessionService::removeRequest() {
  if(_token.isEmpty())return;QString path=_runtime->root()+"/Workspace/creator-test.request";QFile file(path);
  if(file.open(QIODevice::ReadOnly)&&QString::fromUtf8(file.readAll()).split('\n').value(1)==_token){file.close();QFile::remove(path);}
}
void TestSessionService::finish(QString const& message,bool error) {
  _timer.stop();removeRequest();_phase=Idle;_status=message;emit changed();
  if(error)QMessageBox::warning(_parent,"Local test",message);
}
void TestSessionService::cancel(){if(busy())finish("Local test cancelled.");}
}
