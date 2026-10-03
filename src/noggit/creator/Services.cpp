#include "Services.hpp"
#include "Database.hpp"
#include <noggit/runtime/RuntimeManager.hpp>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QSaveFile>
#include <cmath>
#include <initializer_list>
#include <utility>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
void require(bool b, QString const& message) { if (!b) throw std::runtime_error(message.toStdString()); }
QString n(Id id) { return QString::number(id); }
Fields one(Database& db, QString table, Id id, QString key="entry") {
  auto rows=db.query("SELECT * FROM "+table+" WHERE "+key+"="+n(id));
  require(!rows.isEmpty(),"The selected content no longer exists. Refresh the list."); return rows[0];
}
QVector<Choice> choices(QVector<Fields> const& rows) {
  QVector<Choice> result; for (auto const& r:rows) result.push_back({r["entry"].toUInt(),r["name"].toString(),r.value("detail").toString()}); return result;
}
void update(Database& db, QString table, QString key, Id id, Fields const& fields) {
  QStringList sets; for(auto i=fields.begin();i!=fields.end();++i) sets << '`'+i.key()+"`="+db.quote(i.value());
  db.exec("UPDATE "+table+" SET "+sets.join(',')+" WHERE "+key+"="+n(id));
}
void equipmentValid(Database& db, std::array<Id,3> const& items) {
  const QVector<QVector<int>> types{{13,17,21},{13,14,22,23},{15,25,26,28}};
  for(int i=0;i<3;++i) if(items[i]) { auto row=one(db,"item_template",items[i]); require(types[i].contains(row["inventory_type"].toInt()),"This item cannot be shown in the selected weapon slot."); }
}
QString outfitFile() {
  auto runtime=Runtime::RuntimeManager::instance(); require(runtime,"Local Creator runtime is unavailable.");
  return runtime->root()+"/Workspace/creator-outfits.json";
}
}
QVector<Choice> CreatureService::search(QString const& text, bool ownedOnly) {
  Database db;
  return choices(db.query("SELECT entry,name,CONCAT('Level ',level_min,'–',level_max,' · ',CASE type WHEN 1 THEN 'Beast' WHEN 2 THEN 'Dragonkin' WHEN 3 THEN 'Demon' WHEN 4 THEN 'Elemental' WHEN 6 THEN 'Undead' WHEN 7 THEN 'Humanoid' ELSE 'Creature' END) AS detail FROM creature_template WHERE name LIKE "+db.quote('%'+text+'%')+(ownedOnly?" AND entry IN (SELECT entry FROM creator_content WHERE kind='npc')":"")+" ORDER BY name LIMIT 250"));
}
bool CreatureService::owned(Id entry) { Database db; return db.owned("npc",entry); }
Npc CreatureService::load(Id entry) {
  Database db; auto row=one(db,"creature_template",entry); Npc d;
  d.entry=entry; d.name=row["name"].toString(); d.display=row["display_id1"].toUInt(); d.faction=row["faction"].toUInt();
  d.level=row["level_min"].toInt(); d.health=row["health_min"].toInt(); d.mana=row["mana_min"].toInt(); d.armor=row["armor"].toInt();
  d.rank=row["rank"].toInt(); d.type=row["type"].toInt(); d.unitClass=row["unit_class"].toInt(); d.movement=row["movement_type"].toInt();
  d.attackMs=row["base_attack_time"].toInt(); d.damageMin=row["dmg_min"].toDouble(); d.damageMax=row["dmg_max"].toDouble();
  auto flags=row["npc_flags"].toUInt(); d.role=(flags&2)?1:(flags&4)?2:(flags&16)?3:(row["flags_extra"].toUInt()&0x400)?7:0;
  auto equipment=row["equipment_id"].toUInt();
  if(equipment) { auto eq=one(db,"creature_equip_template",equipment); for(int i=0;i<3;++i) d.equipment[i]=eq["equipentry"+QString::number(i+1)].toUInt(); }
  auto spawns=db.query("SELECT spawntimesecsmin FROM creature WHERE id="+n(entry)+" ORDER BY guid LIMIT 1");
  if(!spawns.isEmpty()) d.respawn=spawns[0]["spawntimesecsmin"].toInt();
  return d;
}
Id CreatureService::save(Npc const& d, Position const& pos, Id* spawn) {
  require(!d.name.trimmed().isEmpty() && d.name.size()<=100,"Enter an NPC name of up to 100 characters.");
  require(d.display && d.level>=1 && d.level<=63 && d.health>0 && d.damageMin>=0 && d.damageMax>=d.damageMin,"Choose an appearance and valid combat values.");
  require(std::isfinite(pos.x)&&std::isfinite(pos.y)&&std::isfinite(pos.z)&&std::isfinite(pos.orientation),"Choose a valid location in the world.");
  Database db; if(d.entry) require(db.owned("npc",d.entry),"Original NPCs are read-only here. Clone this NPC to make changes.");
  equipmentValid(db,d.equipment);
  Id entry=d.entry?d.entry:db.allocate("creature_template","entry",0x7fffff); Fields values;
  db.track(EntityType::Npc,entry);
  if(d.source && !d.entry) {
    auto source=one(db,"creature_template",d.source);
    auto copy=[&](bool enabled, QStringList const& keys){ if(enabled) for(auto const& key:keys) if(source.contains(key)) values[key]=source[key]; };
    copy(d.appearance,{"scale","mount_display_id"});
    copy(d.stats,{"health_max","mana_max","xp_multiplier"});
    copy(d.combat,{"attack_power","ranged_attack_power","ranged_dmg_min","ranged_dmg_max","dmg_school","dmg_multiplier","ranged_attack_time","holy_res","fire_res","nature_res","frost_res","shadow_res","arcane_res"});
    copy(d.motion,{"speed_walk","speed_run","inhabit_type"});
    copy(d.loot,{"loot_id","pickpocket_loot_id","skinning_loot_id","gold_min","gold_max"});
    copy(d.vendor,{"vendor_id"});
    copy(d.trainer,{"trainer_id","trainer_type","trainer_spell","trainer_class","trainer_race"});
    copy(d.gossip,{"gossip_menu_id"});
  }
  // Source templates are never updated; optional associations retain their existing definitions.
  values["name"]=d.name.trimmed(); values["display_id1"]=d.display;
  values["level_min"]=d.level; values["level_max"]=d.level; values["health_min"]=d.health; values["health_max"]=d.health;
  values["mana_min"]=d.mana; values["mana_max"]=d.mana; values["armor"]=d.armor;
  values["dmg_min"]=d.damageMin; values["dmg_max"]=d.damageMax; values["base_attack_time"]=d.attackMs;
  values["unit_class"]=d.unitClass; values["faction"]=d.faction; values["rank"]=d.rank; values["type"]=d.type;
  values["movement_type"]=d.movement; values["npc_flags"]=(d.role==1?2:d.role==2?4:d.role==3?16:0);
  if(d.source && !d.entry) values["npc_flags"]=values["npc_flags"].toUInt()|(d.vendor?4u:0u)|(d.trainer?16u:0u)|(d.gossip?1u:0u)|(d.quests?2u:0u);
  values["flags_extra"]=d.role==7?0x400:0;
  values["equipment_id"]=0;
  if(d.entry) { auto old=one(db,"creature_template",entry); values["npc_flags"]=values["npc_flags"].toUInt() | (old["npc_flags"].toUInt()&23); }
  if(d.equipment[0]||d.equipment[1]||d.equipment[2]) {
    auto equipment=db.allocate("creature_equip_template","entry"); db.snapshot("creature_equip_template","entry",equipment);
    db.insert("creature_equip_template",{{"entry",equipment},{"equipentry1",d.equipment[0]},{"equipentry2",d.equipment[1]},{"equipentry3",d.equipment[2]}});
    values["equipment_id"]=equipment;
  }
  db.snapshot("creature_template","entry",entry);
  if(d.entry) update(db,"creature_template","entry",entry,values);
  else { values["entry"]=entry; db.insert("creature_template",values); }
  if(d.source && !d.entry) {
    auto copyRows=[&](bool enabled,QString table,QString key) {
      if(!enabled)return;
      db.snapshot(table,key,entry);
      for(auto row:db.query("SELECT * FROM "+table+" WHERE "+key+"="+n(d.source))) {
        if(row.contains("quest")&&db.owned("quest",row["quest"].toUInt())) db.track(EntityType::Quest,row["quest"].toUInt());
        row[key]=entry; db.insert(table,row);
      }
    };
    copyRows(d.vendor,"npc_vendor","entry");
    copyRows(d.trainer,"npc_trainer","entry");
    copyRows(d.quests,"creature_questrelation","id");
    copyRows(d.quests,"creature_involvedrelation","id");
  }
  db.snapshot("creator_content","entry",entry); db.mark("npc",entry);
  if(!d.entry) {
    auto guid=db.allocate("creature","guid",0xfffffffe); db.track(EntityType::Spawn,guid); db.snapshot("creature","guid",guid);
    db.insert("creature",{{"guid",guid},{"id",entry},{"map",pos.map},{"position_x",pos.x},{"position_y",pos.y},{"position_z",pos.z},{"orientation",pos.orientation},{"spawntimesecsmin",d.respawn},{"spawntimesecsmax",d.respawn},{"movement_type",d.movement},{"wander_distance",d.movement==1?5:0}});
    db.snapshot("creator_content","entry",guid); db.mark("spawn",guid); if(spawn) *spawn=guid;
  }
  if(d.entry) {
    for(auto const& row:db.query("SELECT guid FROM creature WHERE id="+n(entry))) {
      auto guid=row["guid"].toUInt(); db.track(EntityType::Spawn,guid); db.snapshot("creature","guid",guid);
      update(db,"creature","guid",guid,{{"spawntimesecsmin",d.respawn},{"spawntimesecsmax",d.respawn},{"movement_type",d.movement},{"wander_distance",d.movement==1?5:0}});
    }
  }
  db.commit(); return entry;
}
void CreatureService::remove(Id entry) {
  Database db; require(db.owned("npc",entry),"Only Creator NPCs can be deleted. Original NPCs are never removed.");
  auto row=one(db,"creature_template",entry); auto id=n(entry);
  // A quest without its giver, ender or kill target could no longer be completed.
  auto quests=db.query("SELECT Title FROM quest_template WHERE entry IN (SELECT entry FROM creator_content WHERE kind='quest') AND (entry IN (SELECT quest FROM creature_questrelation WHERE id="+id+") OR entry IN (SELECT quest FROM creature_involvedrelation WHERE id="+id+") OR "+id+" IN (ReqCreatureOrGOId1,ReqCreatureOrGOId2,ReqCreatureOrGOId3,ReqCreatureOrGOId4)) LIMIT 1");
  require(quests.isEmpty(),"This NPC is used by the quest \""+(quests.isEmpty()?QString():quests[0]["Title"].toString())+"\". Edit or delete that quest first.");
  db.track(EntityType::Npc,entry);
  for(auto const& spawn:db.query("SELECT guid FROM creature WHERE id="+id)) {
    auto guid=spawn["guid"].toUInt(); db.track(EntityType::Spawn,guid);
    db.snapshot("creature","guid",guid); db.snapshot("creator_content","entry",guid);
    db.exec("DELETE FROM creature WHERE guid="+n(guid)); db.exec("DELETE FROM creator_content WHERE kind='spawn' AND entry="+n(guid));
  }
  for(auto [table,key]:std::initializer_list<std::pair<char const*,char const*>>{{"npc_vendor","entry"},{"npc_trainer","entry"},{"creature_questrelation","id"},{"creature_involvedrelation","id"}}) {
    db.snapshot(table,key,entry); db.exec("DELETE FROM "+QString(table)+" WHERE "+key+"="+id);
  }
  // Creator allocates a private equipment row per save; shared original rows are left alone.
  auto equipment=row["equipment_id"].toUInt();
  if(equipment>=1000000&&db.query("SELECT entry FROM creature_template WHERE equipment_id="+n(equipment)+" AND entry<>"+id+" LIMIT 1").isEmpty()) {
    db.snapshot("creature_equip_template","entry",equipment); db.exec("DELETE FROM creature_equip_template WHERE entry="+n(equipment));
  }
  db.snapshot("creature_template","entry",entry); db.exec("DELETE FROM creature_template WHERE entry="+id);
  db.snapshot("creator_content","entry",entry); db.exec("DELETE FROM creator_content WHERE kind='npc' AND entry="+id);
  db.commit();
}
QVector<Id> SpawnService::save(QVector<SpawnEdit> const& edits) {
  Database db; QVector<Id> saved;
  for(auto const& d:edits) {
    require(db.owned("npc",d.entry),"This selection includes an original NPC. Clone it before saving Creator changes.");
    require(std::isfinite(d.position.x)&&std::isfinite(d.position.y)&&std::isfinite(d.position.z)&&std::isfinite(d.position.orientation),"An NPC location is invalid.");
  }
  for(auto const& d:edits) {
    if(d.create&&d.remove) continue;
    Id guid=d.create?db.allocate("creature","guid",0xfffffffe):d.guid;
    if(!d.create) { auto old=one(db,"creature",guid,"guid"); require(old["id"].toUInt()==d.entry,"The NPC placement changed. Refresh before saving."); }
    db.track(EntityType::Spawn,guid); db.snapshot("creature","guid",guid);
    if(d.remove) db.exec("DELETE FROM creature WHERE guid="+n(guid));
    else {
      Fields values{{"id",d.entry},{"map",d.position.map},{"position_x",d.position.x},{"position_y",d.position.y},{"position_z",d.position.z},{"orientation",d.position.orientation},{"spawntimesecsmin",d.respawn},{"spawntimesecsmax",d.respawn}};
      if(d.create) { values["guid"]=guid; db.insert("creature",values); } else update(db,"creature","guid",guid,values);
    }
    db.snapshot("creator_content","entry",guid); db.mark("spawn",guid);
    if(!d.remove) saved.push_back(guid);
  }
  db.commit(); return saved;
}
QVector<Choice> EquipmentService::search(QString const& text,int slot) {
  Database db; QStringList filters{"13,17,21","13,14,22,23","15,25,26,28"};
  require(slot>=-1&&slot<3,"Unknown equipment slot.");
  return choices(db.query("SELECT entry,name FROM item_template WHERE name LIKE "+db.quote('%'+text+'%')+(slot<0?QString():" AND inventory_type IN ("+filters[slot]+")")+" ORDER BY name LIMIT 250"));
}
QString EquipmentService::name(Id item) { if(!item) return "None"; Database db; return one(db,"item_template",item)["name"].toString(); }
QVector<Outfit> EquipmentService::outfits() {
  QFile file(outfitFile()); if(!file.exists()) return {}; require(file.open(QIODevice::ReadOnly),"Cannot read saved outfits.");
  QJsonParseError error; auto doc=QJsonDocument::fromJson(file.readAll(),&error); require(error.error==QJsonParseError::NoError&&doc.isArray(),"Saved outfits file is invalid.");
  QVector<Outfit> result; for(auto const& value:doc.array()) { auto o=value.toObject(); Outfit outfit; outfit.name=o["name"].toString(); outfit.display=o["display"].toInt(); auto eq=o["equipment"].toArray(); for(int i=0;i<3&&i<eq.size();++i) outfit.equipment[i]=eq[i].toInt(); result.push_back(outfit); } return result;
}
void EquipmentService::saveOutfit(Outfit const& outfit) {
  require(!outfit.name.trimmed().isEmpty(),"Give the outfit a name."); auto saved=outfits();
  for(auto const& o:saved) require(o.name.compare(outfit.name,Qt::CaseInsensitive)!=0,"An outfit already has that name.");
  saved.push_back(outfit); QJsonArray array;
  for(auto const& o:saved) array.append(QJsonObject{{"name",o.name},{"display",int(o.display)},{"equipment",QJsonArray{int(o.equipment[0]),int(o.equipment[1]),int(o.equipment[2])}}});
  QSaveFile file(outfitFile()); auto bytes=QJsonDocument(array).toJson(); require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit(),"Cannot save the outfit.");
}
QVector<Choice> QuestService::search(QString const& text) {
  Database db; return choices(db.query("SELECT entry,Title AS name FROM quest_template WHERE entry IN (SELECT entry FROM creator_content WHERE kind='quest') AND Title LIKE "+db.quote('%'+text+'%')+" ORDER BY Title LIMIT 250"));
}
Quest QuestService::load(Id entry) {
  Database db; require(db.owned("quest",entry),"Only Creator quests can be edited here."); auto r=one(db,"quest_template",entry); Quest q;
  q.entry=entry; q.title=r["Title"].toString(); q.description=r["Details"].toString(); q.completion=r["OfferRewardText"].toString();
  q.level=r["QuestLevel"].toInt(); q.requiredLevel=r["MinLevel"].toInt(); q.xp=r["RewXP"].toInt(); q.money=r["RewOrReqMoney"].toInt();
  for(auto table:{"creature_questrelation","creature_involvedrelation"}) { auto rows=db.query("SELECT id FROM "+QString(table)+" WHERE quest="+n(entry)); if(!rows.isEmpty()) (QString(table)=="creature_questrelation"?q.giver:q.ender)=rows[0]["id"].toUInt(); }
  for(int i=1;i<=4;++i) { auto k=QString::number(i); if(r["ReqItemId"+k].toUInt()) q.objectives.push_back({Objective::Collect,r["ReqItemId"+k].toUInt(),r["ReqItemCount"+k].toInt(),r["ObjectiveText"+k].toString()}); else if(r["ReqCreatureOrGOId"+k].toUInt()) q.objectives.push_back({Objective::Kill,r["ReqCreatureOrGOId"+k].toUInt(),r["ReqCreatureOrGOCount"+k].toInt(),r["ObjectiveText"+k].toString()}); }
  if(q.objectives.isEmpty()) q.objectives.push_back({Objective::Talk,q.ender,1,"Speak to the quest ender"}); return q;
}
Id QuestService::save(Quest const& q) {
  require(!q.title.trimmed().isEmpty()&&q.title.size()<=255,"Enter a quest title of up to 255 characters.");
  require(q.giver&&q.ender&&q.requiredLevel>=1&&q.level>=1&&q.level<=63&&q.requiredLevel<=q.level,"Choose a giver, an ender, and valid quest levels.");
  require(q.xp>=0&&q.money>=0,"Rewards cannot be negative.");
  require(!q.objectives.isEmpty()&&q.objectives.size()<=4,"Choose one to four objectives.");
  Database db; if(q.entry) require(db.owned("quest",q.entry),"Only Creator quests can be changed.");
  one(db,"creature_template",q.giver); one(db,"creature_template",q.ender);
  Fields values{{"Title",q.title},{"Details",q.description},{"OfferRewardText",q.completion},{"MinLevel",q.requiredLevel},{"QuestLevel",q.level},{"RewXP",q.xp},{"RewOrReqMoney",q.money},{"Method",2}};
  QStringList objectives;
  for(int i=1;i<=4;++i) { auto k=QString::number(i); for(auto base:{"ReqItemId","ReqItemCount","ReqCreatureOrGOId","ReqCreatureOrGOCount","ReqSpellCast"}) values[QString(base)+k]=0; values["ObjectiveText"+k]=""; }
  for(int i=0;i<q.objectives.size();++i) {
    auto const& o=q.objectives[i]; require(o.target&&o.count>0&&o.count<=65535,"Select an objective target and a positive count.");
    require(o.type!=Objective::Kill||o.count<=63,"Vanilla supports at most 63 kills per objective.");
    auto target=one(db,o.type==Objective::Collect?"item_template":"creature_template",o.target);
    auto k=QString::number(i+1); QString label;
    if(o.type==Objective::Talk) { require(q.objectives.size()==1&&o.target==q.ender,"A Talk quest must have one objective, targeting its quest ender."); label="Speak to "+target["name"].toString(); }
    else { QString base=o.type==Objective::Collect?"ReqItem":"ReqCreatureOrGO"; values[base+"Id"+k]=o.target; values[base+"Count"+k]=o.count; label=(o.type==Objective::Collect?"Collect ":"Defeat ")+QString::number(o.count)+" × "+target["name"].toString(); }
    values["ObjectiveText"+k]=label; objectives<<label;
  }
  values["Objectives"]=objectives.join('\n');
  auto entry=q.entry?q.entry:db.allocate("quest_template","entry"); db.track(EntityType::Quest,entry); db.snapshot("quest_template","entry",entry);
  if(q.entry) update(db,"quest_template","entry",entry,values); else { values["entry"]=entry; db.insert("quest_template",values); }
  for(auto table:{"creature_questrelation","creature_involvedrelation"}) {
    db.snapshot(table,"quest",entry); db.exec("DELETE FROM "+QString(table)+" WHERE quest="+n(entry));
    db.insert(table,{{"id",QString(table)=="creature_questrelation"?q.giver:q.ender},{"quest",entry}});
  }
  for(auto npc:{q.giver,q.ender}) { db.snapshot("creature_template","entry",npc); db.exec("UPDATE creature_template SET npc_flags=npc_flags|2 WHERE entry="+n(npc)); }
  db.snapshot("creator_content","entry",entry); db.mark("quest",entry); db.commit(); return entry;
}
void QuestService::remove(Id entry) {
  Database db; require(db.owned("quest",entry),"Only Creator quests can be deleted."); one(db,"quest_template",entry);
  db.track(EntityType::Quest,entry);
  for(auto table:{"creature_questrelation","creature_involvedrelation"}) { db.snapshot(table,"quest",entry); db.exec("DELETE FROM "+QString(table)+" WHERE quest="+n(entry)); }
  db.snapshot("quest_template","entry",entry); db.exec("DELETE FROM quest_template WHERE entry="+n(entry));
  db.snapshot("creator_content","entry",entry); db.exec("DELETE FROM creator_content WHERE kind='quest' AND entry="+n(entry));
  db.commit();
}
}
