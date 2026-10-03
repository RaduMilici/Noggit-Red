#include "Services.hpp"
#include "Database.hpp"
#include <noggit/runtime/RuntimeManager.hpp>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QRegularExpression>
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
QSet<Id> CreatureService::ownedEntries() {
  Database db; QSet<Id> result;
  for(auto const& row:db.query("SELECT entry FROM creator_content WHERE kind='npc'")) result.insert(row["entry"].toUInt());
  return result;
}
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
Id CreatureService::save(Npc const& d, std::optional<Position> const& place, Id* spawn) {
  require(!d.name.trimmed().isEmpty() && d.name.size()<=100,"Enter an NPC name of up to 100 characters.");
  require(d.display && d.level>=1 && d.level<=63 && d.health>0 && d.damageMin>=0 && d.damageMax>=d.damageMin,"Choose an appearance and valid combat values.");
  if(place) require(std::isfinite(place->x)&&std::isfinite(place->y)&&std::isfinite(place->z)&&std::isfinite(place->orientation),"Choose a valid location in the world.");
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
  if(!d.entry && place) {
    auto const& pos=*place; auto guid=db.allocate("creature","guid",0xfffffffe); db.track(EntityType::Spawn,guid); db.snapshot("creature","guid",guid);
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
QStringList AccountService::list() {
  Database db; QStringList names;
  for(auto const& row:db.query("SELECT username FROM realmd.account ORDER BY username")) names<<row["username"].toString();
  return names;
}
void AccountService::create(QString const& username,QString const& password) {
  // Vanilla clients send names and passwords upper-cased, limited to 16 characters.
  require(QRegularExpression("^[A-Za-z0-9]{3,16}$").match(username).hasMatch(),"Use 3 to 16 letters or digits for the account name.");
  require(QRegularExpression("^[\\x21-\\x7E]{4,16}$").match(password).hasMatch(),"Use 4 to 16 characters for the password, without spaces or accents.");
  auto name=username.toUpper();
  // Same hash as Tortoise's AccountMgr::CreateAccount: SHA1("NAME:PASSWORD"), upper-case hex.
  auto hash=QString(QCryptographicHash::hash((name+':'+password.toUpper()).toUtf8(),QCryptographicHash::Sha1).toHex()).toUpper();
  Database db;
  require(db.query("SELECT id FROM realmd.account WHERE username="+db.quote(name)).isEmpty(),"That account name is already taken.");
  db.exec("INSERT INTO realmd.account(username,sha_pass_hash,joindate) VALUES("+db.quote(name)+","+db.quote(hash)+",NOW())");
  db.exec("REPLACE INTO realmd.realmcharacters(realmid,acctid,numchars) SELECT realmlist.id,account.id,0 FROM realmd.realmlist,realmd.account LEFT JOIN realmd.realmcharacters ON acctid=account.id WHERE acctid IS NULL");
  db.commit();
}
}
