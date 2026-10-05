#include <noggit/creator/Services.hpp>
#include <noggit/creator/Database.hpp>
#include <noggit/creator/ChangeExport.hpp>
#include <noggit/creator/ContentStore.hpp>
#include <noggit/creator/ProductionSync.hpp>
#include <noggit/creator/LootService.hpp>
#include <noggit/creator/VendorService.hpp>
#include <noggit/creator/TrainerService.hpp>
#include <noggit/creator/GossipService.hpp>
#include <noggit/creator/SpellService.hpp>
#include <noggit/creator/ItemDesignService.hpp>
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
  // GameObjects and patrols use the same journal, tracker and export service.
  auto models=GameObjectService::search(""); check(!models.isEmpty(),"Seed has no GameObject models");
  GameObject object; object.name="Ancient Chest"; object.type=10; object.display=GameObjectService::load(models[0].id).display;
  auto objectEntry=GameObjectService::save(object,position);
  auto objectRows=query("SELECT * FROM gameobject WHERE id="+QString::number(objectEntry));
  check(objectRows.size()==1,"Object was not placed"); auto objectGuid=objectRows[0]["guid"].toUInt();
  check(tracked(EntityType::GameObject,objectEntry)&&tracked(EntityType::GameObjectSpawn,objectGuid),"Object changes missing");
  auto moved=position; moved.x+=4; moved.orientation=1.5f;
  GameObjectService::placements({{objectGuid,objectEntry,moved,120,false,false}});
  check(query("SELECT position_x FROM gameobject WHERE guid="+QString::number(objectGuid))[0]["position_x"].toFloat()==moved.x,"Object move was lost");
  Patrol patrol; patrol.loop=false;
  Waypoint first; first.position=position; first.position.orientation=100; first.run=true; first.waitMs=2000;
  Waypoint last=first; last.position.x+=5; last.run=false;
  patrol.points={first,last}; PatrolService::save(guid,patrol);
  check(PatrolService::load(guid)==patrol,"Patrol walk/run/wait/loop did not round-trip");
  auto patrolBefore=tracked(EntityType::Spawn,guid)->after;
  PatrolService::save(guid,patrol);
  check(tracked(EntityType::Spawn,guid)->after==patrolBefore,"Unchanged patrol save allocated new scripts");
  CreatureService::save(saved);
  check(query("SELECT movement_type FROM creature WHERE guid="+QString::number(guid))[0]["movement_type"].toInt()==2,"NPC template edit disabled its patrol");
  auto originals=query("SELECT guid,map,position_x,position_y,position_z FROM creature WHERE id="+original["entry"].toString()+" LIMIT 1");
  if(!originals.isEmpty()) {
    auto r=originals[0]; auto originalGuid=r["guid"].toUInt(); Patrol path;
    Waypoint node; node.position={r["map"].toUInt(),r["position_x"].toFloat(),r["position_y"].toFloat(),r["position_z"].toFloat(),100}; node.run=true; path.points={node};
    PatrolService::save(originalGuid,path); check(PatrolService::load(originalGuid)==path,"Original NPC patrol could not be authored");
  }
  // Quests: the ssh-tunnel editor's pure plans, written through ContentStore.
  Layouts layouts;
  auto data=loadQuest(layouts,0,loadQuestList(layouts));
  QuestSaveRequest request; request.entry=data.next_entry;
  auto& fields=request.content.fields;
  fields.title="Creator integration quest"; fields.level=5; fields.min_level=1; fields.xp=100; fields.money=42;
  Q::Target kill; kill.id=entry; kill.count=2; Q::Target use; use.kind=Q::Target::Kind::Object; use.id=objectEntry; use.count=1; use.text="Interact with Ancient Chest"; fields.targets=std::vector<Q::Target>{kill,use};
  request.content.links.starters={{Q::Giver::Kind::Npc,entry}}; request.content.links.enders={{Q::Giver::Kind::Npc,second}};
  Q::ScriptAction say; say.kind=Q::ScriptAction::Kind::Say; say.text="Welcome, O'Brien"; request.content.on_accept={say};
  saveQuest(layouts,data,loadQuestList(layouts),request); auto questId=request.entry;
  check(questId>=1000000&&!query("SELECT entry FROM creator_content WHERE kind='quest' AND entry="+QString::number(questId)).isEmpty(),"Quest was not created as Creator content");
  check(query("SELECT npc_flags FROM creature_template WHERE entry="+QString::number(entry))[0]["npc_flags"].toUInt()&2,"Giver did not become a quest giver");
  auto spoken=query("SELECT t.entry,t.male_text FROM quest_start_scripts s JOIN broadcast_text t ON t.entry=s.dataint WHERE s.id="+QString::number(questId)+" AND s.command=0");
  check(!spoken.isEmpty()&&spoken[0]["entry"].toUInt()>=Q::OWN_TEXT_START&&spoken[0]["male_text"].toString()=="Welcome, O'Brien","Accept event was not written");
  auto loaded=loadQuest(layouts,questId,loadQuestList(layouts));
  check(loaded.content.fields.targets->at(0).id==entry&&loaded.content.fields.targets->at(0).count==2&&loaded.content.on_accept.at(0).text=="Welcome, O'Brien","Quest did not round-trip");
  check(loaded.content.fields.targets->at(1).kind==Q::Target::Kind::Object && loaded.content.fields.targets->at(1).id==objectEntry,"GameObject objective did not round-trip");
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

  // Loot, vendor and trainer: saved through their services, listed in Local Changes, original NPCs untouched.
  {
    auto loot=LootService::load({LootOwner::Kind::Npc,entry});
    check(loot.editable,"A Creator NPC's loot is read-only");
    LootRow bone; bone.item=items[0].id; bone.chance=25; bone.minCount=1; bone.maxCount=2; loot.rows.push_back(bone); loot.moneyMin=10; loot.moneyMax=50;
    LootService::save(loot);
    auto saved=LootService::load({LootOwner::Kind::Npc,entry});
    bool found=false; for(auto const& r:saved.rows) found=found||(r.item==items[0].id&&r.chance==25&&r.maxCount==2);
    check(found&&saved.moneyMax==50&&query("SELECT loot_id FROM creature_template WHERE entry="+QString::number(entry))[0]["loot_id"].toUInt()==entry,"Loot did not round-trip");
    check(tracked(EntityType::Loot,entry)&&tracked(EntityType::Loot,entry)->summary().contains("Loot:"),"Loot change not tracked");
    auto referenceRows=query("SELECT DISTINCT entry FROM reference_loot_template LIMIT 1");
    check(!referenceRows.isEmpty(), "Seed has no reference loot table");
    LootRow reference; reference.item=15000000; reference.reference=referenceRows[0]["entry"].toUInt();
    saved.rows.push_back(reference);
    auto secondReference=reference; secondReference.item=15000001; saved.rows.push_back(secondReference);
    LootService::save(saved);
    auto storedReferences=query("SELECT item,mincountOrRef FROM creature_loot_template WHERE entry="+QString::number(entry)+" AND item IN (15000000,15000001) AND mincountOrRef<0 ORDER BY item");
    check(storedReferences.size()==2&&storedReferences[0]["item"].toUInt()==15000000&&storedReferences[1]["item"].toUInt()==15000001,
          "Saving shared loot references changed their row keys");
    auto original=LootService::load({LootOwner::Kind::Npc,sources[0]["entry"].toUInt()});
    bool refused=false; try { LootService::save(original); } catch(std::exception const&) { refused=true; }
    check(refused&&!original.editable,"An original NPC's loot was editable");
    Vendor vendor=VendorService::load(entry); vendor.sells=true; vendor.items={{items[0].id,3,900,0,3}};
    VendorService::save(vendor);
    check(VendorService::load(entry).items.value(0).stock==3&&(query("SELECT npc_flags FROM creature_template WHERE entry="+QString::number(entry))[0]["npc_flags"].toUInt()&4),"Vendor did not round-trip");
    auto reopenedVendor = VendorService::load(entry);
    check(reopenedVendor.items.value(0).flags == 3, "Vendor restock flags lost on load/save");
    reopenedVendor.items[0].stock = 4;
    VendorService::save(reopenedVendor);
    check(query("SELECT itemflags FROM npc_vendor WHERE entry="+QString::number(entry))[0]["itemflags"].toUInt()==3,
          "Editing vendor stock erased restock flags");
    check(tracked(EntityType::Vendor,entry),"Vendor change not tracked");
    auto spells=TrainerService::search("Fireball");
    check(!spells.isEmpty(),"No learnable spell found");
    Trainer trainer=TrainerService::load(entry); trainer.teaches=true; trainer.spells={{spells[0].teach,100,spells[0].suggestedLevel}};
    TrainerService::save(trainer);
    check(TrainerService::load(entry).spells.value(0).spell==spells[0].teach,"Trainer did not round-trip");
    check(tracked(EntityType::Trainer,entry)&&tracked(EntityType::Trainer,entry)->summary().contains("Trainer"),"Trainer change not tracked");

    // Dialogue: a greeting that continues (for level 5+), opens the shop and training, then ends with a spell.
    using Action=DialogueResponse::Action;
    Dialogue talk=GossipService::load(entry);
    DialogueNode greeting; greeting.text="Creator test: well met, $N. It's \"quiet\" here.";
    DialogueResponse more; more.text="Tell me more."; more.action=Action::Continue; more.target=1; more.conditions={{DialogueCondition::Kind::MinLevel,5}};
    DialogueResponse shop; shop.text="Show me your goods."; shop.action=Action::Vendor;
    DialogueResponse train; train.text="Teach me."; train.action=Action::Trainer;
    DialogueNode story; story.text="There is little more to say.";
    DialogueResponse bye; bye.text="Farewell."; bye.action=Action::Close;
    DialogueEffect cast; cast.kind=DialogueEffect::Kind::CastSpell; cast.id=spells[0].teach; bye.effects={cast};
    greeting.responses={more,shop,train}; story.responses={bye}; talk.nodes={greeting,story};
    GossipService::save(talk);
    auto strip=[](QVector<DialogueNode> nodes){ for(auto& n:nodes) n.menu=0; return nodes; };
    auto heard=GossipService::load(entry);
    check(strip(heard.nodes)==talk.nodes&&heard.dropped.isEmpty()&&!heard.shared,"Dialogue did not round-trip");
    check(query("SELECT npc_flags FROM creature_template WHERE entry="+QString::number(entry))[0]["npc_flags"].toUInt()&1,"Dialogue did not set the gossip flag");
    GossipService::save(heard);
    check(GossipService::load(entry).nodes.value(1).menu==heard.nodes[1].menu,"An unchanged dialogue save moved its rows");
    check(tracked(EntityType::Gossip,entry)&&tracked(EntityType::Gossip,entry)->summary().startsWith("+ Dialogue"),"Dialogue change not tracked");
    bool readOnly=false; try { GossipService::save(GossipService::load(sources[0]["entry"].toUInt())); } catch(std::exception const&) { readOnly=true; }
    check(readOnly,"An original NPC's dialogue was editable");
    // A clone gets its own copy; deleting it removes only that copy.
    Npc twin=CreatureService::load(entry); twin.entry=0; twin.source=entry; twin.gossip=true; twin.name="Creator test: twin";
    auto twinEntry=CreatureService::save(twin);
    auto copied=GossipService::load(twinEntry);
    check(!copied.shared&&copied.nodes.value(0).menu!=heard.nodes[0].menu&&strip(copied.nodes)==talk.nodes,"A cloned NPC shares or lost its dialogue");
    CreatureService::remove(twinEntry);
    check(query("SELECT entry FROM gossip_menu WHERE entry="+QString::number(copied.nodes[0].menu)).isEmpty()&&strip(GossipService::load(entry).nodes)==talk.nodes,
          "Deleting a clone left its dialogue or changed the original's");

    // Spells and items: a teachable heal and its next rank, a potion that casts it, a cloned weapon.
    auto heal=SpellService::blank(); SpellCatalog::apply(SpellCatalog::Template::Heal,heal,0);
    heal.name="Creator test: Mending"; heal.rank="Rank 1"; heal.level=10; heal.teachable=true;
    auto healId=SpellService::save(heal);
    check(healId>0&&healId<=65535&&SpellService::load(healId).teach<=65535,"Spell and teaching IDs must fit the client spellbook protocol");
    check(SpellService::load(healId).teachable&&!TrainerService::search("Creator test: Mending").isEmpty(),"A teachable spell is not offered to trainers");
    auto nextId=SpellService::save(SpellService::nextRank(healId,16,1.5,100,0));
    check(nextId>0&&nextId<=65535,"Next rank ID must fit the client spellbook protocol");
    check(SpellService::chain(nextId).size()==2&&SpellService::load(nextId).previous==healId,"The next rank is not chained");
    auto potion=ItemDesignService::blank(ItemDesignService::Template::Consumable); potion.name="Creator test: Draught"; potion.spells[0].spell=healId;
    auto potionId=ItemDesignService::save(potion);
    check(ItemDesignService::load(potionId).spells[0].spell==healId&&!SpellService::uses(healId).isEmpty(),"The potion does not cast the spell");
    auto blade=ItemDesignService::clone(query("SELECT entry FROM item_template WHERE class=2 AND display_id>0 LIMIT 1")[0]["entry"].toUInt());
    blade.name="Creator test: Blade"; auto bladeId=ItemDesignService::save(blade);
    check(tracked(EntityType::Spell,healId)&&tracked(EntityType::Spell,nextId)&&tracked(EntityType::Item,potionId)&&tracked(EntityType::Item,bladeId),"Spells and items are not in Local Changes");
    bool spellBlocked=false; try { SpellService::remove(healId); } catch(std::exception const&) { spellBlocked=true; }
    check(spellBlocked,"A spell an item and a next rank use was deleted");
  }

  // Sync to Production's rollback: back up the footprint, apply a package that deletes, moves and creates
  // across every kind, then restore. The checksums cover every table the generated SQL can write, so a
  // row changed outside the footprint shows up too.
  {
    auto q=[](QString const& sql){ return query(sql); };
    auto state=[&](EntityType type,Id id){ return ChangeTracker::capture(q,type,id); };
    auto edited=[](QJsonObject rows,QString const& table,QString const& column,QString const& value){
      auto list=rows[table].toArray(); auto row=list[0].toObject(); row[column]=value; list[0]=row; rows[table]=list; return rows;
    };
    QVector<TrackedChange> scenario;
    auto add=[&](EntityType type,Id id,QJsonObject const& before,QJsonObject const& after){
      TrackedChange c; c.type=type; c.entity=id; c.before=before; c.after=after; c.label="Sync test"; c.action=*ChangeTracker::derive(type,before,after);
      scenario.push_back(c);
    };
    add(EntityType::Quest,questId,state(EntityType::Quest,questId),{});
    add(EntityType::Item,itemId,state(EntityType::Item,itemId),{});
    add(EntityType::Spawn,guid,state(EntityType::Spawn,guid),edited(state(EntityType::Spawn,guid),"creature","position_x","1234"));
    add(EntityType::Npc,copyEntry,state(EntityType::Npc,copyEntry),{});
    add(EntityType::Gossip,entry,state(EntityType::Gossip,entry),{});
    add(EntityType::Npc,1999999,{},edited(state(EntityType::Npc,entry),"creature_template","entry","1999999"));
    QString const tables="creature_template,creature_equip_template,npc_vendor,npc_trainer,creature_questrelation,creature_involvedrelation,"
      "gameobject_questrelation,gameobject_involvedrelation,areatrigger_involvedrelation,creature,item_template,quest_template,"
      "quest_start_scripts,quest_end_scripts,broadcast_text,creature_loot_template,gameobject_loot_template,gameobject_template,creator_content,"
      "gossip_menu,gossip_menu_option,npc_text,gossip_scripts,conditions";
    auto checksum=[&]{ return query("CHECKSUM TABLE "+tables); };
    auto pristine=checksum();
    auto backup=ProductionBackup::take(ChangeTracker::footprint(scenario,q),q);
    { Database db; for (auto const& s : ChangeTracker::statements(ChangeTracker::sql(scenario))) db.execute(s.toStdString()); }
    check(checksum()!=pristine&&query("SELECT entry FROM quest_template WHERE entry="+QString::number(questId)).isEmpty()
          &&!query("SELECT entry FROM creature_template WHERE entry=1999999").isEmpty(),"The sync test package changed nothing");
    { Database db; for (auto const& s : backup.restoreStatements()) db.execute(s.toStdString()); }
    check(checksum()==pristine,"Restoring the sync backup did not put every row back");
    // An ID production uses for content it did not get from Noggit is refused; content synced before is not.
    ChangePackage clash; TrackedChange c; c.type=EntityType::Npc; c.action=ChangeAction::Update; c.label="Mine";
    c.entity=sources[0]["entry"].toUInt(); clash.changes={c}; clash.tracked=1; clash.owned={true};
    check(ProductionSync::conflicts(clash,q).size()==1,"A game NPC with the same ID was not reported");
    clash.changes[0].entity=entry;
    check(ProductionSync::conflicts(clash,q).isEmpty(),"Content synced before was reported as a conflict");
  }

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
  auto greetingMenu=GossipService::load(entry).nodes.value(0).menu;
  CreatureService::remove(entry);
  check(greetingMenu&&query("SELECT entry FROM gossip_menu WHERE entry="+QString::number(greetingMenu)).isEmpty()&&tracked(EntityType::Gossip,entry),"NPC deletion left its dialogue");
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
