#include "TestSessionService.hpp"
#include "Database.hpp"
#include "SpellService.hpp"
#include "TalentService.hpp"
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
#include <algorithm>
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
void TestSessionService::testCharacter(QWidget* parent,TestOptions const& options) {
  if(busy()){QMessageBox::information(parent,"Local test","A local test is already in progress. Finish or cancel it first.");return;}
  _options=options;begin(parent,CharacterTarget,0);
}
void TestSessionService::testEntity(QWidget* parent,bool object,Id entry,TestOptions const& options) {
  if(busy()){QMessageBox::information(parent,"Local test","A local test is already in progress. Finish or cancel it first.");return;}
  _object=object;_options=options;begin(parent,EntityTarget,entry);
}
void TestSessionService::begin(QWidget* parent,Target target,Id id,Position position) {
  if(busy()){QMessageBox::information(parent,"Local test","A local test is already in progress. Finish or cancel it first.");return;}
  _parent=parent;
  try {
    require(!_runtime->stopping(),"Wait for the local runtime to finish stopping, then try again.");
    if(!Runtime::ClientManager::prepare(parent,Runtime::ClientManager::Profile::TestLocal))return;
    if(target!=EntityTarget&&target!=CharacterTarget)_options={};
    _target=target;_id=id;_position=position;_token.clear();_loginHint.clear();_character=_account=0;
    _phase=WaitDatabase;_status="Starting local database…";_deadline.start();_moduleDeadline.invalidate();emit changed();
    _runtime->start();_timer.start();
  }catch(std::exception const& e){finish(QString::fromUtf8(e.what()),true);}
}
void TestSessionService::prepareRequest() {
  // Save callbacks belong to the world/editor. They run synchronously before any restart.
  require(_runtime->saveForTest(),"Local test cancelled because pending NPC changes could not be saved.");
  // Edited talent trees: the world server reads Talent.dbc when it starts, so it goes in before the restart.
  if(auto* talents=TalentStore::instance())if(!talents->serverCurrent())talents->installServer();
  if(_target==None || _target==CharacterTarget) {
    // Launches without a teleport must not replay an interrupted location test.
    auto path=_runtime->root()+"/Workspace/creator-test.request";
    require(!QFileInfo::exists(path)||QFile::remove(path),"Cannot clear the previous local test request.");
  }
  if(_target==None) {
    // A fresh installation has no login; offer one before launching WoW.
    if(AccountService::list().isEmpty())
      require(!createLocalAccount(_parent).isEmpty(),"Local test cancelled. Create a local account to log in to WoW.");
    return;
  }
  if(_target==CharacterTarget) {
    // No move: the character starts where it logged out, with its new items and spells.
    auto character=chooseCharacter();
    grant();
    _loginHint="Log in as "+character["name"].toString()+" (local account "+character["username"].toString()+").";
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
  if(_target==EntityTarget) {
    QVector<Fields> rows;
    { Database db;
      rows=_object?db.query("SELECT g.*,m.map_name,t.name AS npc_name FROM gameobject g JOIN gameobject_template t ON t.entry=g.id LEFT JOIN map_template m ON m.entry=g.map WHERE g.id="+QString::number(_id)+" ORDER BY g.map,g.guid LIMIT 100")
                  :db.query("SELECT c.*,m.map_name,t.name AS npc_name FROM creature c JOIN creature_template t ON t.entry=c.id LEFT JOIN map_template m ON m.entry=c.map WHERE c.id="+QString::number(_id)+" ORDER BY c.map,c.guid LIMIT 100");
    }
    require(!rows.isEmpty(),_object?"This object has no saved placement. Place it in the world first.":"This NPC has no saved placement. Place it in the world first.");
    int choice=0;
    if(rows.size()>1) {
      QStringList names;for(int i=0;i<rows.size();++i)names<<QString("%1 — %2 — placement %3").arg(rows[i]["npc_name"].toString(),rows[i]["map_name"].toString()).arg(i+1);
      bool ok=false;auto selected=QInputDialog::getItem(_parent,"Test","Choose the placement to test beside",names,0,false,&ok);
      if(!ok)throw std::runtime_error("Local test cancelled.");choice=names.indexOf(selected);
    }
    _position=positionOf(rows[choice]);
  }
  if(_target==NpcTarget||_target==QuestTarget||_target==EntityTarget) {
    _position.x+=2.f*std::cos(_position.orientation);_position.y+=2.f*std::sin(_position.orientation);
    _position.orientation=std::fmod(_position.orientation+3.14159265f,6.2831853f);
  }
  _position.z+=0.5f;
  _position.orientation=std::fmod(_position.orientation,6.2831853f);
  if(_position.orientation<0)_position.orientation+=6.2831853f;
  auto character=chooseCharacter();
  grant();
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
Fields TestSessionService::chooseCharacter() {
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
  _character=character["guid"].toUInt();_account=character["account"].toUInt();
  return character;
}
// Items and spells for the test character. Runs before the restart: the server takes the highest item GUID at
// startup, so items written now never clash with ones it makes later. Local character database only.
void TestSessionService::grant() { if(_character) grantTo(_character,_options); }
void TestSessionService::grantTo(Id character,TestOptions const& _options) {
  if(_options.items.isEmpty()&&_options.spells.isEmpty()&&_options.unlearn.isEmpty())return;
  for(auto spell:_options.spells)
    require(spell>0&&spell<=SpellService::idLimit,"The test spell ID must be between 1 and 65535. Clone older incompatible spells before testing.");
  Database db;
  auto guid=QString::number(character);
  require(db.query("SELECT online FROM characters.characters WHERE guid="+guid+" AND online=0").size()==1,"The test character is still online; log out and try again.");
  QSet<int> taken;
  for(auto const& r:db.query("SELECT slot FROM characters.character_inventory WHERE guid="+guid+" AND bag=0"))taken.insert(r["slot"].toInt());
  auto next=db.query("SELECT COALESCE(MAX(guid),0)+100 AS g FROM characters.item_instance")[0]["g"].toUInt();
  QStringList statements; // Validate every item and reserve all slots before writing any rows.
  if(!_options.unlearn.isEmpty()) {
    // The server counts spent talent points from the talent spells a character knows: removing them refunds the points.
    QStringList ids;for(auto spell:_options.unlearn)ids<<QString::number(spell);
    statements.push_back("DELETE FROM characters.character_spell WHERE guid="+guid+" AND spell IN ("+ids.join(',')+")");
  }
  int slot=23; // the backpack's 16 slots are 23..38
  for(auto const& wanted:_options.items) {
    auto item=wanted.first;auto count=wanted.second;
    auto proto=db.query("SELECT stackable,max_durability,duration,spellcharges_1,spellcharges_2,spellcharges_3,spellcharges_4,spellcharges_5 FROM item_template WHERE entry="+QString::number(item));
    require(!proto.isEmpty(),"The item to test no longer exists.");
    int stack=std::max(1,proto[0]["stackable"].toInt()),left=std::max(1,count);
    QString charges;for(int k=1;k<=5;++k)charges+=proto[0]["spellcharges_"+QString::number(k)].toString()+" ";
    QString enchantments;for(int k=0;k<21;++k)enchantments+="0 ";
    while(left>0) {
      while(slot<=38&&taken.contains(slot))++slot;
      require(slot<=38,"The test character's backpack is full. Make room in it (or test with another character) and try again.");
      int amount=std::min(left,stack);left-=amount;
      statements.push_back(QString("INSERT INTO characters.item_instance (guid,itemEntry,owner_guid,creatorGuid,giftCreatorGuid,count,duration,charges,flags,enchantments,randomPropertyId,transmogrifyId,durability,text,generated_loot) "
                      "VALUES (%1,%2,%3,0,0,%4,%5,%6,0,%7,0,0,%8,0,0)").arg(next).arg(item).arg(guid).arg(amount).arg(proto[0]["duration"].toInt())
                      .arg(db.quote(charges),db.quote(enchantments)).arg(proto[0]["max_durability"].toInt()));
      statements.push_back(QString("INSERT INTO characters.character_inventory (guid,bag,slot,item,item_template) VALUES (%1,0,%2,%3,%4)").arg(guid).arg(slot).arg(next).arg(item));
      taken.insert(slot);++next;
    }
  }
  for(auto spell:_options.spells)
    statements.push_back("INSERT INTO characters.character_spell (guid,spell,active,disabled) VALUES ("+guid+","+QString::number(spell)+",1,0) ON DUPLICATE KEY UPDATE active=1,disabled=0");
  for(auto const& statement:statements) db.exec(statement);
  db.commit();
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
      // The world server is back and nobody has logged in yet: the character's saved state is ours to change.
      applyOptions();
      Runtime::ClientManager::launch(Runtime::ClientManager::Profile::TestLocal);
      if(_token.isEmpty()){finish(_loginHint.isEmpty()?"Local client launched. Log in with your local account.":"Local client launched. "+_loginHint);return;}
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
void TestSessionService::applyOptions() {
  if(!_character||(!_options.money&&!_options.level&&!_options.developer))return;
  Database db;
  auto guid=QString::number(_character);
  // Only the bundled local databases: Database always connects to the local runtime.
  require(db.query("SELECT online FROM characters.characters WHERE guid="+guid+" AND online=0").size()==1,"The test character is still online; log out and try again.");
  if(_options.money>0) db.exec("UPDATE characters.characters SET money=LEAST(money+"+QString::number(_options.money)+",2147483647) WHERE guid="+guid);
  if(_options.level>0) db.exec("UPDATE characters.characters SET level="+QString::number(std::clamp(_options.level,1,60))+",xp=0 WHERE guid="+guid);
  if(_options.developer&&_account) db.exec("UPDATE realmd.account SET `rank`=GREATEST(`rank`,3) WHERE id="+QString::number(_account));
  db.commit();
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
