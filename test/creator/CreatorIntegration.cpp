#include <noggit/creator/Services.hpp>
#include <noggit/creator/Database.hpp>
#include <noggit/creator/ChangeExport.hpp>
#include <noggit/creator/ContentStore.hpp>
#include <noggit/runtime/RuntimeManager.hpp>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <stdexcept>
using namespace Noggit::Creator;
namespace Q = Noggit::Quest;
namespace {
void check(bool condition, char const* message) { if(!condition) throw std::runtime_error(message); }
QVector<Fields> query(QString const& sql) { Database db; return db.query(sql); }
TrackedChange const* tracked(EntityType type, Id id) {
  for (auto const& c : ChangeTracker::instance()->changes()) if (c.type==type && c.entity==id) return &c;
  return nullptr;
}
void clearTracked() { QStringList ids; for (auto const& c : ChangeTracker::instance()->changes()) ids << c.id; ChangeTracker::instance()->clear(ids); }
void runChecks() {
  clearTracked();
  auto sources=query("SELECT * FROM creature_template WHERE display_id1>0 AND type=7 AND scale>0 ORDER BY entry LIMIT 1");
  check(!sources.isEmpty(),"Seed has no NPCs"); auto original=sources[0];
  Npc npc; npc.name="Creator test: O'Brien — Militia"; npc.display=original["display_id1"].toUInt(); npc.role=1; npc.source=original["entry"].toUInt(); npc.stats=false; npc.combat=false; npc.motion=false;
  Position position{0, -9465, 64, 56, 1}; Id guid=0;
  auto entry=CreatureService::save(npc,position,&guid);
  check(entry!=original["entry"].toUInt()&&guid,"Fresh identifiers were not allocated");
  auto saved=CreatureService::load(entry); check(saved.name==npc.name,"Unicode/quoted name did not round-trip");
  auto sourceAfter=query("SELECT * FROM creature_template WHERE entry="+original["entry"].toString());
  check(sourceAfter[0]==original,"Creation modified the source NPC");
  check(query("SELECT * FROM creature_questrelation WHERE id="+QString::number(entry)).isEmpty(),"Fresh NPC inherited quests");
  Id secondGuid=0; auto second=CreatureService::save(npc,position,&secondGuid);
  check(second!=entry&&secondGuid!=guid,"Local identifiers collided");
  saved.respawn=321; CreatureService::save(saved,position);
  check(query("SELECT spawntimesecsmin FROM creature WHERE guid="+QString::number(guid))[0]["spawntimesecsmin"].toInt()==321,"Respawn edit was lost");
  position.x+=2; SpawnService::save({{guid,entry,position,321,false,false}});
  check(query("SELECT position_x FROM creature WHERE guid="+QString::number(guid))[0]["position_x"].toFloat()==position.x,"Movement was lost");
  // Quests: the ssh-tunnel editor's pure plans, written through ContentStore.
  Layouts layouts;
  auto data=loadQuest(layouts,0,loadQuestList(layouts));
  QuestSaveRequest request; request.entry=data.next_entry;
  auto& fields=request.content.fields;
  fields.title="Creator integration quest"; fields.level=5; fields.min_level=1; fields.xp=100; fields.money=42;
  Q::Target kill; kill.id=entry; kill.count=2; fields.targets=std::vector<Q::Target>{kill};
  request.content.links.starters={{Q::Giver::Kind::Npc,entry}}; request.content.links.enders={{Q::Giver::Kind::Npc,second}};
  Q::ScriptAction say; say.kind=Q::ScriptAction::Kind::Say; say.text="Welcome, O'Brien"; request.content.on_accept={say};
  saveQuest(layouts,data,loadQuestList(layouts),request); auto questId=request.entry;
  check(questId>=1000000&&!query("SELECT entry FROM creator_content WHERE kind='quest' AND entry="+QString::number(questId)).isEmpty(),"Quest was not created as Creator content");
  check(query("SELECT npc_flags FROM creature_template WHERE entry="+QString::number(entry))[0]["npc_flags"].toUInt()&2,"Giver did not become a quest giver");
  auto spoken=query("SELECT t.entry,t.male_text FROM quest_start_scripts s JOIN broadcast_text t ON t.entry=s.dataint WHERE s.id="+QString::number(questId)+" AND s.command=0");
  check(!spoken.isEmpty()&&spoken[0]["entry"].toUInt()>=Q::OWN_TEXT_START&&spoken[0]["male_text"].toString()=="Welcome, O'Brien","Accept event was not written");
  auto loaded=loadQuest(layouts,questId,loadQuestList(layouts));
  check(loaded.content.fields.targets->at(0).id==entry&&loaded.content.fields.targets->at(0).count==2&&loaded.content.on_accept.at(0).text=="Welcome, O'Brien","Quest did not round-trip");
  auto items=EquipmentService::search("",-1);check(!items.isEmpty(),"Seed has no items");
  // A collect objective whose item a Creator NPC without loot drops for the quest: it gets its own loot table.
  QuestSaveRequest edit=request; edit.mode=Q::SaveMode::Edit; edit.content=loaded.content;
  Q::ItemCount collect; collect.id=items[0].id; collect.count=3; edit.content.fields.collect=std::vector<Q::ItemCount>{collect};
  Q::QuestDrop drop; drop.source_entry=entry; drop.item=items[0].id; drop.chance=40; edit.content.drops={drop};
  saveQuest(layouts,loaded,loadQuestList(layouts),edit);
  check(query("SELECT loot_id FROM creature_template WHERE entry="+QString::number(entry))[0]["loot_id"].toUInt()==entry,"Drop source got no loot table");
  check(!query("SELECT item FROM creature_loot_template WHERE entry="+QString::number(entry)+" AND item="+QString::number(items[0].id)+" AND ChanceOrQuestChance<0").isEmpty(),"Quest drop was not written");
  // A save the plan refuses changes nothing.
  auto refused=edit; refused.content.links.area_trigger=0; refused.content.prerequisites={{questId},Q::Prerequisites::Mode::Any};
  bool selfRejected=false;
  try { saveQuest(layouts,loadQuest(layouts,questId,loadQuestList(layouts)),loadQuestList(layouts),refused); } catch(std::exception const&) { selfRejected=true; }
  check(selfRejected&&loadQuestList(layouts).quests.size()>0,"A quest depending on itself was accepted");
  // Items made in Noggit: created, changed, and protected while a quest uses them.
  auto itemData=loadItem(layouts,0); Noggit::Item::ItemFields itemFields; itemFields.name="Creator test pelt"; itemFields.quality=1;
  auto itemId=createItem(layouts,itemData,itemFields);
  check(itemId>=1000000&&!query("SELECT entry FROM creator_content WHERE kind='item' AND entry="+QString::number(itemId)).isEmpty(),"Item was not created as Creator content");
  itemFields.name="Creator test pelt (renamed)"; updateItem(layouts,loadItem(layouts,itemId),itemId,itemFields);
  check(query("SELECT name FROM item_template WHERE entry="+QString::number(itemId))[0]["name"].toString()=="Creator test pelt (renamed)","Item edit was lost");
  auto rewarded=loadQuest(layouts,questId,loadQuestList(layouts)); auto reward=edit; reward.content=rewarded.content;
  reward.content.fields.rewards=std::vector<Q::ItemCount>{{itemId,1}}; saveQuest(layouts,rewarded,loadQuestList(layouts),reward);
  bool itemProtected=false;
  try { deleteItem(layouts,loadItem(layouts,itemId),itemId); } catch(std::exception const&) { itemProtected=true; }
  check(itemProtected,"An item a quest rewards was deleted");
  // A follow-up quest: chain links are written on both quests.
  auto followData=loadQuest(layouts,0,loadQuestList(layouts)); QuestSaveRequest follow; follow.entry=followData.next_entry;
  follow.content.fields.title="Creator integration follow-up"; follow.content.fields.level=6; follow.content.fields.min_level=1;
  follow.content.links.starters={{Q::Giver::Kind::Npc,second}}; follow.content.links.enders={{Q::Giver::Kind::Npc,second}};
  follow.content.prerequisites={{questId},Q::Prerequisites::Mode::Any};
  saveQuest(layouts,followData,loadQuestList(layouts),follow);
  check(query("SELECT PrevQuestId FROM quest_template WHERE entry="+QString::number(follow.entry))[0]["PrevQuestId"].toUInt()==questId
        &&query("SELECT NextQuestInChain FROM quest_template WHERE entry="+QString::number(questId))[0]["NextQuestInChain"].toUInt()==follow.entry,"Follow-up chain was not linked");
  Npc questCopy=CreatureService::load(entry);questCopy.entry=0;questCopy.source=entry;questCopy.quests=true;questCopy.name="Opt-in quest copy";
  auto copyEntry=CreatureService::save(questCopy,position);
  check(query("SELECT quest FROM creature_questrelation WHERE id="+QString::number(copyEntry))[0]["quest"].toUInt()==questId,"Opt-in quest relation was not copied");
  // Exercise rollback on the native MyISAM world tables, not a mock transaction.
  {
    Database db;db.snapshot("creature_template","entry",entry);
    db.snapshotWhere("broadcast_text","entry>="+QString::number(Q::OWN_TEXT_START));
    db.exec("UPDATE creature_template SET name='Interrupted save' WHERE entry="+QString::number(entry));
    db.exec("DELETE FROM broadcast_text WHERE entry>="+QString::number(Q::OWN_TEXT_START));
  }
  check(CreatureService::load(entry).name==npc.name,"Recovery journal failed to restore an unfinished save");
  check(!query("SELECT entry FROM broadcast_text WHERE entry>="+QString::number(Q::OWN_TEXT_START)).isEmpty(),"Recovery journal failed to restore deleted rows");
  SpawnService::save({{secondGuid,second,position,120,true,false}});
  check(query("SELECT guid FROM creature WHERE guid="+QString::number(secondGuid)).isEmpty(),"Placement deletion failed");

  // Local Changes: net results of everything above, without the rolled-back save.
  check(tracked(EntityType::Npc,entry)&&tracked(EntityType::Npc,entry)->action==ChangeAction::Create,"NPC creation not tracked");
  check(tracked(EntityType::Npc,entry)->after["creature_template"].toArray()[0].toObject()["name"].toString()==npc.name,"Tracked NPC is not its latest state");
  check(tracked(EntityType::Spawn,guid)&&tracked(EntityType::Spawn,guid)->action==ChangeAction::Create,"Moved new placement is not a creation");
  check(!tracked(EntityType::Spawn,secondGuid),"Created-then-deleted placement still listed");
  check(tracked(EntityType::Quest,questId)&&tracked(EntityType::Quest,questId)->action==ChangeAction::Create,"Quest creation not tracked");
  check(tracked(EntityType::Quest,questId)->after.contains("quest_start_scripts")&&tracked(EntityType::Quest,questId)->after.contains("creature_loot_template"),"Quest events or drops not tracked");
  check(tracked(EntityType::Item,itemId)&&tracked(EntityType::Item,itemId)->action==ChangeAction::Create,"Item creation not tracked");
  clearTracked();
  position.x+=3; SpawnService::save({{guid,entry,position,321,false,false}});
  check(tracked(EntityType::Spawn,guid)&&tracked(EntityType::Spawn,guid)->action==ChangeAction::Move,"Placement movement is not a move");
  check(ChangeTracker::instance()->changes().size()==1,"Unrelated entries tracked");
  auto current=loadQuest(layouts,questId,loadQuestList(layouts)); auto retitle=edit; retitle.content=current.content;
  retitle.content.fields.title="Creator integration quest (edited)"; saveQuest(layouts,current,loadQuestList(layouts),retitle);
  check(tracked(EntityType::Quest,questId)->action==ChangeAction::Update,"Quest edit is not an update");

  // Export: the spawn's and quest's Creator NPCs travel with the package.
  QTemporaryDir out; check(out.isValid(),"No export folder");
  auto beforeExport=query("SELECT * FROM creature_template WHERE entry="+QString::number(entry));
  auto result=ExportService::exportChanges("Haunted Mill","Integration",out.path(),ChangeTracker::instance()->changes());
  check(result.folder==out.path()+"/Haunted-Mill"&&result.changes==2&&result.dependencies>=3,"Unexpected export result (NPCs, the rewarded item, the follow-up quest)");
  QFile manifestFile(result.folder+"/manifest.json"),sqlFile(result.folder+"/changes.sql");
  check(manifestFile.open(QIODevice::ReadOnly)&&sqlFile.open(QIODevice::ReadOnly),"Package files missing");
  auto manifest=QJsonDocument::fromJson(manifestFile.readAll()).object();
  check(manifest["name"]=="Haunted Mill"&&manifest["author"]=="Integration"&&manifest["entities"].toArray().size()==2+result.dependencies,"Manifest incomplete");
  auto script=QString::fromUtf8(sqlFile.readAll());
  check(!script.contains("characters")&&script.contains("REPLACE INTO `quest_template`"),"Package is not limited to the changes");
  bool duplicateExport=false;
  try { ExportService::exportChanges("Haunted Mill","Integration",out.path(),ChangeTracker::instance()->changes()); } catch(std::exception const&) { duplicateExport=true; }
  check(duplicateExport,"Existing package was overwritten");
  // Replaying the package on the database it came from must succeed and change nothing.
  {
    Database db;
    for (auto const& line : script.split('\n')) if (!line.isEmpty() && !line.startsWith("--")) db.exec(line);
  }
  check(query("SELECT * FROM creature_template WHERE entry="+QString::number(entry))==beforeExport,"Replaying the package changed the NPC");
  check(query("SELECT Title FROM quest_template WHERE entry="+QString::number(questId))[0]["Title"].toString()=="Creator integration quest (edited)","Replaying the package lost the quest");
  check(query("SELECT COUNT(*) AS n FROM quest_start_scripts WHERE id="+QString::number(questId))[0]["n"].toInt()==1,"Replaying the package duplicated the quest's events");

  // Deletions.
  bool blocked=false;
  try { CreatureService::remove(entry); } catch(std::exception const&) { blocked=true; }
  check(blocked&&!query("SELECT entry FROM creature_template WHERE entry="+QString::number(entry)).isEmpty(),"NPC used by a quest was deleted");
  for (auto quest : {follow.entry,questId}) {
    auto stored=loadQuest(layouts,quest,loadQuestList(layouts));
    deleteQuest(layouts,stored,loadQuestList(layouts),quest,{});
  }
  check(query("SELECT entry FROM quest_template WHERE entry="+QString::number(questId)).isEmpty()
        &&query("SELECT id FROM quest_start_scripts WHERE id="+QString::number(questId)).isEmpty()
        &&query("SELECT item FROM creature_loot_template WHERE entry="+QString::number(entry)+" AND ChanceOrQuestChance<0").isEmpty(),"Quest deletion left rows behind");
  deleteItem(layouts,loadItem(layouts,itemId),itemId);
  check(query("SELECT entry FROM item_template WHERE entry="+QString::number(itemId)).isEmpty(),"Item deletion failed");
  check(tracked(EntityType::Quest,questId)->action==ChangeAction::Delete,"Quest deletion not tracked");
  CreatureService::remove(entry);
  check(query("SELECT guid FROM creature WHERE id="+QString::number(entry)).isEmpty()&&query("SELECT entry FROM creature_template WHERE entry="+QString::number(entry)).isEmpty(),"NPC deletion failed");
  check(tracked(EntityType::Npc,entry)->action==ChangeAction::Delete&&tracked(EntityType::Spawn,guid)->action==ChangeAction::Delete,"NPC deletion not tracked");
  check(!ChangeTracker::sql(ChangeTracker::instance()->changes()).isEmpty(),"Deletion export failed");

  // Local login accounts use the server's own hash and get a realm character slot.
  auto account=QString("ct%1").arg(QDateTime::currentSecsSinceEpoch()%100000000);
  AccountService::create(account,"Secret1");
  auto stored=query("SELECT id,sha_pass_hash FROM realmd.account WHERE username='"+account.toUpper()+"'");
  check(!stored.isEmpty()&&stored[0]["sha_pass_hash"].toString()==query("SELECT UPPER(SHA1('"+account.toUpper()+":SECRET1')) AS h")[0]["h"].toString(),"Account hash does not match the server's");
  check(!query("SELECT acctid FROM realmd.realmcharacters WHERE acctid="+stored[0]["id"].toString()).isEmpty(),"Account has no realm character slot");
  check(AccountService::list().contains(account.toUpper()),"New account is not listed");
  bool duplicate=false,invalid=false;
  try { AccountService::create(account.toLower(),"Other1"); } catch(std::exception const&) { duplicate=true; }
  try { AccountService::create("a b","Secret1"); } catch(std::exception const&) { invalid=true; }
  check(duplicate&&invalid,"Duplicate or invalid account was accepted");
}
}
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);
  if(argc!=2||!QFileInfo::exists(QString::fromLocal8Bit(argv[1])+"/CREATOR_TEST_DISPOSABLE")) {
    QTextStream(stderr)<<"Usage: creator_integration <disposable runtime root containing CREATOR_TEST_DISPOSABLE>\n";return 2;
  }
  Noggit::Runtime::RuntimeManager runtime(QString::fromLocal8Bit(argv[1]),&app);
  bool scheduled=false;int result=1;
  QObject::connect(&runtime,&Noggit::Runtime::RuntimeManager::changed,&app,[&]{
    if(!runtime.error().isEmpty()){QTextStream(stderr)<<runtime.error()<<Qt::endl;app.exit(1);return;}
    if(!scheduled&&runtime.status(2)=="Running") {
      scheduled=true;QTimer::singleShot(0,&app,[&]{
        try {runChecks();QTextStream(stdout)<<"Creator integration checks passed\n";result=0;}
        catch(std::exception const& e){QTextStream(stderr)<<e.what()<<Qt::endl;}
        runtime.stop();
      });
    }
    if(scheduled&&runtime.status(0)=="Stopped"&&!runtime.stopping())app.exit(result);
  });
  QObject::connect(&app,&QCoreApplication::aboutToQuit,&runtime,&Noggit::Runtime::RuntimeManager::shutdown);
  QTimer::singleShot(0,&runtime,&Noggit::Runtime::RuntimeManager::start);
  QTimer::singleShot(180000,&app,[&]{QTextStream(stderr)<<"Integration test timed out\n";app.exit(3);});
  return app.exec();
}
