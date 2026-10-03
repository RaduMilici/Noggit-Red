#include <noggit/creator/Services.hpp>
#include <noggit/creator/Database.hpp>
#include <noggit/runtime/RuntimeManager.hpp>
#include <QCoreApplication>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>
#include <stdexcept>
using namespace Noggit::Creator;
namespace {
void check(bool condition, char const* message) { if(!condition) throw std::runtime_error(message); }
QVector<Fields> query(QString const& sql) { Database db; return db.query(sql); }
void runChecks() {
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
  Quest quest; quest.title="Creator integration quest"; quest.giver=entry;quest.ender=second;quest.xp=100;quest.money=42;
  quest.objectives={{Objective::Kill,entry,2,{}}}; auto questId=QuestService::save(quest);
  Npc questCopy=CreatureService::load(entry);questCopy.entry=0;questCopy.source=entry;questCopy.quests=true;questCopy.name="Opt-in quest copy";
  auto copyEntry=CreatureService::save(questCopy,position);
  check(query("SELECT quest FROM creature_questrelation WHERE id="+QString::number(copyEntry))[0]["quest"].toUInt()==questId,"Opt-in quest relation was not copied");
  auto loaded=QuestService::load(questId);check(loaded.giver==entry&&loaded.ender==second&&loaded.objectives[0].count==2&&loaded.xp==100,"Kill quest did not round-trip");
  auto tooManyKills=loaded;tooManyKills.objectives[0].count=64;bool unsafeRejected=false;
  try {QuestService::save(tooManyKills);}catch(std::exception const&){unsafeRejected=true;}
  check(unsafeRejected,"Unsafe Vanilla kill count was accepted");
  loaded.objectives={{Objective::Talk,second,1,{}}}; QuestService::save(loaded);
  check(QuestService::load(questId).objectives[0].type==Objective::Talk,"Talk quest retained kill requirements");
  auto items=EquipmentService::search("",-1);check(!items.isEmpty(),"Seed has no items");
  loaded.objectives={{Objective::Collect,items[0].id,3,{}}};QuestService::save(loaded);
  check(QuestService::load(questId).objectives[0].type==Objective::Collect,"Collect quest did not round-trip");
  bool rejected=false;loaded.objectives[0].count=0;
  try { QuestService::save(loaded); } catch(std::exception const&) {rejected=true;}
  check(rejected&&QuestService::load(questId).objectives[0].count==3,"Invalid quest damaged the saved quest");
  // Exercise rollback on the native MyISAM world table, not a mock transaction.
  {
    Database db;db.snapshot("creature_template","entry",entry);
    db.exec("UPDATE creature_template SET name='Interrupted save' WHERE entry="+QString::number(entry));
  }
  check(CreatureService::load(entry).name==npc.name,"Recovery journal failed to restore an unfinished save");
  SpawnService::save({{secondGuid,second,position,120,true,false}});
  check(query("SELECT guid FROM creature WHERE guid="+QString::number(secondGuid)).isEmpty(),"Placement deletion failed");
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
