#include "Services.hpp"
#include "Database.hpp"
#include "GossipService.hpp"
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
NpcLook CreatureService::look(Id entry) {
  Database db; auto row=one(db,"creature_template",entry); NpcLook look;
  look.display=row["display_id1"].toUInt(); look.scale=row["scale"].toDouble(); look.name=row["name"].toString();
  if(auto equipment=row["equipment_id"].toUInt()) {
    auto rows=db.query("SELECT e.equipentry1,e.equipentry2,e.equipentry3,i1.display_id AS d1,i2.display_id AS d2,i2.inventory_type AS t2,i3.display_id AS d3 FROM creature_equip_template e "
                       "LEFT JOIN item_template i1 ON i1.entry=e.equipentry1 LEFT JOIN item_template i2 ON i2.entry=e.equipentry2 LEFT JOIN item_template i3 ON i3.entry=e.equipentry3 WHERE e.entry="+n(equipment));
    if(!rows.isEmpty()) { look.mainhand=rows[0]["d1"].toUInt(); look.offhand=rows[0]["d2"].toUInt(); look.offhandType=rows[0]["t2"].toInt(); look.ranged=rows[0]["d3"].toUInt(); }
  }
  return look;
}
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
  // A dialogue made in Noggit is copied, never shared: editing one NPC's must not change another's.
  bool ownDialogue=false;
  if(d.source && !d.entry && d.gossip) {
    auto root=one(db,"creature_template",d.source)["gossip_menu_id"].toUInt();
    ownDialogue=root && db.owned("gossip_menu",root);
    if(ownDialogue) db.track(EntityType::Gossip,entry);
  }
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
    if(ownDialogue) values["gossip_menu_id"]=0;
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
    // Copied goods and spells are the new NPC's own Vendor and Trainer changes.
    if(d.vendor) db.track(EntityType::Vendor,entry);
    if(d.trainer) db.track(EntityType::Trainer,entry);
    copyRows(d.vendor,"npc_vendor","entry");
    copyRows(d.trainer,"npc_trainer","entry");
    copyRows(d.quests,"creature_questrelation","id");
    copyRows(d.quests,"creature_involvedrelation","id");
    if(ownDialogue) GossipService::copyWith(db,d.source,entry);
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
      update(db,"creature","guid",guid,{{"spawntimesecsmin",d.respawn},{"spawntimesecsmax",d.respawn},{"movement_type",db.query("SELECT point FROM creature_movement WHERE id="+n(guid)+" LIMIT 1").isEmpty()?d.movement:2},{"wander_distance",d.movement==1?5:0}});
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
  for(auto type:{EntityType::Loot,EntityType::Vendor,EntityType::Trainer}) db.track(type,entry);
  GossipService::removeWith(db,entry);
  // Its own loot table goes with it (a table it still shares with the NPC it was copied from stays).
  if(row["loot_id"].toUInt()==entry) { db.snapshotWhere("creature_loot_template","entry="+id); db.exec("DELETE FROM creature_loot_template WHERE entry="+id); }
  for(auto const& spawn:db.query("SELECT guid FROM creature WHERE id="+id)) {
    auto guid=spawn["guid"].toUInt(); db.track(EntityType::Spawn,guid);
    db.snapshot("creature","guid",guid); db.snapshot("creature_movement","id",guid); db.exec("DELETE FROM creature_movement WHERE id="+n(guid)); db.snapshot("creator_content","entry",guid);
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
    if(d.remove) { db.snapshot("creature_movement","id",guid); db.exec("DELETE FROM creature_movement WHERE id="+n(guid)); db.exec("DELETE FROM creature WHERE guid="+n(guid)); }
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

namespace Noggit::Creator {
QVector<Choice> GameObjectService::search(QString const& text) {
  Database db;
  return choices(db.query("SELECT entry,name,CONCAT(CASE type WHEN 0 THEN 'Door' WHEN 3 THEN 'Chest / resource' WHEN 5 THEN 'Decoration' WHEN 6 THEN 'Trap' WHEN 10 THEN 'Interactive quest object' ELSE 'Custom object' END,' · Display ',displayId) AS detail FROM gameobject_template WHERE displayId>0 AND name LIKE "+db.quote('%'+text+'%')+" ORDER BY name LIMIT 250"));
}
GameObject GameObjectService::load(Id entry, Id guid) {
  Database db; auto r=one(db,"gameobject_template",entry); GameObject d;
  d.entry=entry; d.display=r["displayId"].toUInt(); d.name=r["name"].toString(); d.type=r["type"].toInt();
  d.size=r["size"].toDouble(); d.faction=r["faction"].toUInt();
  d.quest=(d.type==10||d.type==3)?r[d.type==10?"data1":"data8"].toUInt():0;
  if(guid) { auto p=one(db,"gameobject",guid,"guid"); d.state=p["state"].toInt(); d.respawn=p["spawntimesecsmin"].toInt(); }
  return d;
}
Id GameObjectService::save(GameObject const& d, std::optional<Position> place) {
  require(!d.name.trimmed().isEmpty() && d.name.size()<=100 && d.display && std::isfinite(d.size) && d.size>0 && d.size<=100,"Choose a model, name and valid size.");
  require(d.state>=0 && d.state<=2 && d.respawn>=0,"Invalid state or respawn time.");
  Database db;
  if(d.entry) require(db.owned("gameobject",d.entry),"Original objects are read-only. Create a new object from its model.");
  if(d.quest) { one(db,"quest_template",d.quest); require(d.type==3||d.type==10,"Quest relations require a chest or quest object."); }
  Id entry=d.entry?d.entry:db.allocate("gameobject_template","entry",0x7fffff);
  db.track(EntityType::GameObject,entry); db.snapshot("gameobject_template","entry",entry);
  Fields f;
  // Custom objects explicitly inherit their existing behavior; standard types start clean.
  if(d.source) { f=one(db,"gameobject_template",d.source); f.remove("entry"); }
  else if(!d.entry || one(db,"gameobject_template",d.entry)["type"].toInt()!=d.type)
    for(int i=0;i<24;++i) f["data"+QString::number(i)]=0;
  f["name"]=d.name.trimmed(); f["displayId"]=d.display; f["type"]=d.type; f["size"]=d.size; f["faction"]=d.faction;
  if(d.type==10) { f["data1"]=d.quest; f["data3"]=3000; }
  if(d.type==3) f["data8"]=d.quest;
  if(d.entry) update(db,"gameobject_template","entry",entry,f); else { f["entry"]=entry; db.insert("gameobject_template",f); }
  db.snapshot("creator_content","entry",entry); db.mark("gameobject",entry);
  if(place) {
    auto p=*place; require(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::isfinite(p.orientation),"Invalid placement.");
    auto guid=db.allocate("gameobject","guid",0xfffffffe); db.track(EntityType::GameObjectSpawn,guid); db.snapshot("gameobject","guid",guid);
    db.insert("gameobject",{{"guid",guid},{"id",entry},{"map",p.map},{"position_x",p.x},{"position_y",p.y},{"position_z",p.z},{"orientation",p.orientation},{"rotation2",std::sin(p.orientation/2)},{"rotation3",std::cos(p.orientation/2)},{"state",d.state},{"spawntimesecsmin",d.respawn},{"spawntimesecsmax",d.respawn}});
    db.snapshot("creator_content","entry",guid); db.mark("object_spawn",guid);
  }
  if(d.entry) for(auto const& r:db.query("SELECT guid FROM gameobject WHERE id="+n(entry))) {
    auto guid=r["guid"].toUInt(); db.track(EntityType::GameObjectSpawn,guid); db.snapshot("gameobject","guid",guid);
    update(db,"gameobject","guid",guid,{{"state",d.state},{"spawntimesecsmin",d.respawn},{"spawntimesecsmax",d.respawn}});
  }
  db.commit(); return entry;
}
void GameObjectService::placements(QVector<SpawnEdit> const& edits) {
  Database db;
  for(auto const& d:edits) {
    require(db.owned("gameobject",d.entry),"Create a Creator object before editing its placements.");
    auto p=d.position; require(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::isfinite(p.orientation),"Invalid object position.");
    if(d.create&&d.remove) continue;
    auto guid=d.create?db.allocate("gameobject","guid",0xfffffffe):d.guid;
    if(!d.create) require(one(db,"gameobject",guid,"guid")["id"].toUInt()==d.entry,"Object placement changed. Refresh first.");
    db.track(EntityType::GameObjectSpawn,guid); db.snapshot("gameobject","guid",guid);
    if(d.remove) db.exec("DELETE FROM gameobject WHERE guid="+n(guid));
    else {
      Fields f{{"id",d.entry},{"map",p.map},{"position_x",p.x},{"position_y",p.y},{"position_z",p.z},{"orientation",p.orientation},{"rotation0",0},{"rotation1",0},{"rotation2",std::sin(p.orientation/2)},{"rotation3",std::cos(p.orientation/2)}};
      if(d.create) { f["guid"]=guid; f["state"]=1; f["spawntimesecsmin"]=d.respawn; f["spawntimesecsmax"]=d.respawn; db.insert("gameobject",f); }
      else update(db,"gameobject","guid",guid,f);
    }
    db.snapshot("creator_content","entry",guid);
    if(d.remove) db.exec("DELETE FROM creator_content WHERE kind='object_spawn' AND entry="+n(guid)); else db.mark("object_spawn",guid);
  }
  db.commit();
}
Patrol PatrolService::load(Id guid) {
  Database db; auto spawn=one(db,"creature",guid,"guid"); Patrol result;
  auto nodes=db.query("SELECT * FROM creature_movement WHERE id="+n(guid)+" ORDER BY point");
  if(nodes.isEmpty() && spawn["movement_type"].toInt()==2)
    nodes=db.query("SELECT * FROM creature_movement_template WHERE entry="+spawn["id"].toString()+" ORDER BY point");
  for(auto const& r:nodes) {
    Waypoint w; w.position={spawn["map"].toUInt(),r["position_x"].toFloat(),r["position_y"].toFloat(),r["position_z"].toFloat(),r["orientation"].toFloat()}; w.waitMs=r["waittime"].toInt();
    for(auto const& script:db.query("SELECT * FROM creature_movement_scripts WHERE id="+r["script_id"].toString())) {
      if(script["command"].toInt()!=25 && script["comments"].toString()!="Creator patrol stop") { auto preserved=script; preserved.remove("id"); w.scripts.push_back(preserved); }
      if(script["command"].toInt()==25) {
        w.run=script["datalong"].toBool();
        if(script["comments"].toString()=="Creator patrol movement" && w.waitMs==1) w.waitMs=0;
      }
      if(script["command"].toInt()==20 && script["datalong"].toInt()==0) result.loop=false;
    }
    result.points.push_back(w);
  }
  return result;
}
void PatrolService::save(Id guid, Patrol const& patrol) {
  if(load(guid)==patrol) return;
  Database db; auto spawn=one(db,"creature",guid,"guid");

  db.track(EntityType::Spawn,guid); db.snapshot("creature","guid",guid); db.snapshot("creature_movement","id",guid);
  db.exec("DELETE FROM creature_movement WHERE id="+n(guid));
  for(int i=0;i<patrol.points.size();++i) {
    auto const& w=patrol.points[i]; auto p=w.position;
    require(p.map==spawn["map"].toUInt() && std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::isfinite(p.orientation)&&w.waitMs>=0,"Invalid waypoint.");
    auto script=db.allocate("creature_movement_scripts","id"); db.snapshot("creature_movement_scripts","id",script);
    for(auto row:w.scripts) { row["id"]=script; db.insert("creature_movement_scripts",row); }
    db.insert("creature_movement_scripts",{{"id",script},{"command",25},{"datalong",w.run?1:0},{"comments","Creator patrol movement"}});
    if(!patrol.loop && i==patrol.points.size()-1) db.insert("creature_movement_scripts",{{"id",script},{"delay",0},{"command",20},{"datalong",0},{"comments","Creator patrol stop"}});
    db.snapshot("creator_content","entry",script); db.mark("patrol_script",script);
    // Scripts execute at the end of the map tick. Yield before launching the next spline.
    db.insert("creature_movement",{{"id",guid},{"point",i+1},{"position_x",p.x},{"position_y",p.y},{"position_z",p.z},{"orientation",p.orientation},{"waittime",w.waitMs==0?1:w.waitMs},{"script_id",script}});
  }
  update(db,"creature","guid",guid,{{"movement_type",patrol.points.isEmpty()?0:2},{"wander_distance",0}});
  db.snapshot("creator_content","entry",guid); db.mark("spawn",guid); db.commit();
}
}
