#include "ChangeTracker.hpp"
#include <noggit/runtime/RuntimeManager.hpp>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
void require(bool b, QString const& message) { if (!b) throw std::runtime_error(message.toStdString()); }
QString n(Id id) { return QString::number(id); }
QStringList const typeNames{"npc", "spawn", "quest", "item", "gameobject", "object_spawn", "loot", "object_loot", "vendor", "trainer", "gossip", "spell"};
QStringList const actionNames{"CREATE", "UPDATE", "DELETE", "MOVE"};
QStringList const relations{"creature_questrelation", "creature_involvedrelation"};
// Everything that links a quest to its givers, enders and exploration spot, keyed by quest.
QStringList const questLinks{"creature_questrelation", "creature_involvedrelation", "gameobject_questrelation",
                             "gameobject_involvedrelation", "areatrigger_involvedrelation"};
QStringList const scriptTables{"quest_start_scripts", "quest_end_scripts"};
QStringList const scriptColumns{"StartScript", "CompleteScript"};
QStringList const lootTables{"creature_loot_template", "gameobject_loot_template"};
// The editor's spoken lines (see Noggit::Quest::OWN_TEXT_START).
constexpr Id ownTextStart = 6000000;
QString collectedBy(QString const& item) {
  return item + " IN (ReqItemId1,ReqItemId2,ReqItemId3,ReqItemId4)";
}
QJsonObject firstRow(QJsonObject const& rows, QString const& table) { return rows[table].toArray().at(0).toObject(); }
Id field(QJsonObject const& row, QString const& key) { return row[key].toString().toUInt(); }
QString literal(QJsonValue const& value) {
  if (value.isNull() || value.isUndefined()) return "NULL";
  QString text = value.isString() ? value.toString() : value.isBool() ? QString::number(value.toBool()) : QString::number(value.toDouble(), 'g', 17);
  QString escaped;
  for (auto c : text) {
    switch (c.unicode()) {
      case 0: escaped += "\\0"; break;
      case '\'': escaped += "\\'"; break;
      case '\\': escaped += "\\\\"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case 0x1a: escaped += "\\Z"; break;
      default: escaped += c;
    }
  }
  return '\'' + escaped + '\'';
}
// Names go into "--" comments: a line break would turn the rest of the name into a statement.
QString oneLine(QString text) { return text.replace(QRegularExpression("[\\r\\n\\x{2028}\\x{2029}]+"), " "); }
QString identifier(QString name) { return '`' + name.replace('`', "``") + '`'; }
QString replaceRows(QString const& table, QJsonArray const& rows) {
  QString out;
  for (auto const& value : rows) {
    auto row = value.toObject(); QStringList keys, values;
    for (auto it = row.begin(); it != row.end(); ++it) { keys << identifier(it.key()); values << literal(it.value()); }
    out += "REPLACE INTO " + identifier(table) + " (" + keys.join(',') + ") VALUES (" + values.join(',') + ");\n";
  }
  return out;
}
// Rows written only when @owned is set: services of an NPC or object another database does not
// know as Creator content are left alone.
QString replaceRowsIfOwned(QString const& table, QJsonArray const& rows) {
  QString out;
  for (auto const& value : rows) {
    auto row = value.toObject(); QStringList keys, values;
    for (auto it = row.begin(); it != row.end(); ++it) { keys << identifier(it.key()); values << literal(it.value()); }
    out += "REPLACE INTO " + identifier(table) + " (" + keys.join(',') + ") SELECT " + values.join(',') + " FROM DUAL WHERE @owned>0;\n";
  }
  return out;
}
// Rows written only when @owned is set and no row with the same unique values exists (shared conditions).
QString insertIgnoreIfOwned(QString const& table, QJsonArray const& rows) {
  QString out;
  for (auto const& value : rows) {
    auto row = value.toObject(); QStringList keys, values;
    for (auto it = row.begin(); it != row.end(); ++it) { keys << identifier(it.key()); values << literal(it.value()); }
    out += "INSERT IGNORE INTO " + identifier(table) + " (" + keys.join(',') + ") SELECT " + values.join(',') + " FROM DUAL WHERE @owned>0;\n";
  }
  return out;
}
// A dialogue's tables, their keys and the creator_content kind marking the rows made in Noggit
// (conditions are shared by value and never removed).
struct GossipTable { char const *table, *key, *kind; };
GossipTable const gossipTables[] = {{"gossip_menu", "entry", "gossip_menu"}, {"npc_text", "ID", "npc_text"},
                                    {"broadcast_text", "entry", "gossip_text"}, {"gossip_scripts", "id", "gossip_script"}};
QStringList const gossipWritten{"gossip_menu", "gossip_menu_option", "npc_text", "broadcast_text", "gossip_scripts"};
bool isService(EntityType type) {
  return type == EntityType::Loot || type == EntityType::ObjectLoot || type == EntityType::Vendor || type == EntityType::Trainer;
}
// A service's owner row (synthetic: the owner's columns the service sets) and its own rows.
QString headTable(EntityType type) {
  return type == EntityType::Loot ? "creature_loot" : type == EntityType::ObjectLoot ? "gameobject_loot"
       : type == EntityType::Vendor ? "creature_vendor" : "creature_trainer";
}
QString rowTable(EntityType type) {
  return type == EntityType::Loot ? "creature_loot_template" : type == EntityType::ObjectLoot ? "gameobject_loot_template"
       : type == EntityType::Vendor ? "npc_vendor" : "npc_trainer";
}
bool chestType(Id type) { return type == 3 || type == 25; } // chest, fishing hole: data1 is the loot table
// Guards deletions so another database's original content with the same ID is never removed.
QString ownedGuard(QString const& kind, Id id) {
  return "SET @owned := (SELECT COUNT(*) FROM creator_content WHERE kind='" + kind + "' AND entry=" + n(id) + ");\n";
}
QString creatorOnlyQuests() { return " AND quest NOT IN (SELECT entry FROM creator_content WHERE kind='quest')"; }
QJsonObject withoutPosition(QJsonObject rows) {
  auto list = rows["creature"].toArray(); QJsonArray stripped;
  for (auto const& value : list) {
    auto row = value.toObject();
    for (auto key : {"position_x", "position_y", "position_z", "orientation"}) row.remove(key);
    stripped.append(row);
  }
  rows["creature"] = stripped; return rows;
}
QJsonObject toJson(TrackedChange const& c) {
  return {{"id", c.id}, {"type", toString(c.type)}, {"entity", double(c.entity)}, {"action", toString(c.action)}, {"label", c.label},
          {"timestamp", c.timestamp.toString(Qt::ISODateWithMs)}, {"before", c.before}, {"after", c.after}};
}
TrackedChange fromJson(QJsonObject const& o) {
  TrackedChange c; c.id = o["id"].toString(); c.entity = Id(o["entity"].toDouble()); c.label = o["label"].toString();
  auto type = typeNames.indexOf(o["type"].toString()), action = actionNames.indexOf(o["action"].toString());
  require(type >= 0 && action >= 0 && !c.id.isEmpty() && c.entity, "The local changes file contains an unknown entry.");
  c.type = EntityType(type); c.action = ChangeAction(action);
  c.timestamp = QDateTime::fromString(o["timestamp"].toString(), Qt::ISODateWithMs);
  c.before = o["before"].toObject(); c.after = o["after"].toObject(); return c;
}
QStringList ids(QJsonArray const& rows, QString const& key) {
  QStringList result; for (auto const& row : rows) result << n(field(row.toObject(), key));
  result.removeDuplicates(); return result;
}
// Removes a dialogue's rows of both versions, but only those the target database also marks as made in
// Noggit (a game NPC's dialogue is never removed), then the marks the new version no longer has.
QString gossipRemoval(QJsonObject const& before, QJsonObject const& after) {
  QString out;
  auto guard = [](QString const& kind, QString const& column) {
    return " AND " + column + " IN (SELECT entry FROM creator_content WHERE kind='" + kind + "') AND @owned>0;\n";
  };
  auto both = [&](QString const& table, QString const& key) {
    auto list = ids(before[table].toArray(), key) + ids(after[table].toArray(), key); list.removeDuplicates(); list.removeAll("0"); return list;
  };
  if (auto menus = both("gossip_menu", "entry"); !menus.isEmpty())
    out += "DELETE FROM gossip_menu_option WHERE menu_id IN (" + menus.join(',') + ")" + guard("gossip_menu", "menu_id");
  for (auto const& t : gossipTables)
    if (auto list = both(t.table, t.key); !list.isEmpty())
      out += "DELETE FROM " + QString(t.table) + " WHERE " + t.key + " IN (" + list.join(',') + ")" + guard(t.kind, t.key);
  QSet<QString> kept;
  for (auto const& m : after["creator_marks"].toArray()) kept.insert(m.toObject()["kind"].toString() + "/" + m.toObject()["entry"].toString());
  for (auto const& value : before["creator_marks"].toArray()) {
    auto m = value.toObject();
    if (kept.contains(m["kind"].toString() + "/" + m["entry"].toString())) continue;
    out += "DELETE FROM creator_content WHERE kind=" + literal(m["kind"]) + " AND entry=" + n(field(m, "entry")) + " AND @owned>0;\n";
  }
  return out;
}
// Rows of `before` whose key is no longer in `after` (e.g. a drop or spoken line the edit removed).
QJsonArray removedRows(QJsonObject const& before, QJsonObject const& after, QString const& table, QStringList const& key) {
  auto keyOf = [&](QJsonObject const& row) { QStringList parts; for (auto const& k : key) parts << row[k].toString(); return parts.join('/'); };
  QStringList kept; for (auto const& row : after[table].toArray()) kept << keyOf(row.toObject());
  QJsonArray removed; for (auto const& row : before[table].toArray()) if (!kept.contains(keyOf(row.toObject()))) removed.append(row);
  return removed;
}
// Quest-only drop rows are removed unless another quest still collects the item, as the editor does.
QString dropRemoval(QString const& table, QJsonObject const& row, Id quest, QString const& guard) {
  auto item = n(field(row, "item"));
  return "DELETE FROM " + table + " WHERE entry=" + n(field(row, "entry")) + " AND item=" + item + " AND ChanceOrQuestChance<0" + guard
       + " AND NOT EXISTS (SELECT 1 FROM quest_template WHERE entry<>" + n(quest) + " AND " + collectedBy(item) + ");\n";
}
}
QString toString(EntityType type) { return typeNames[int(type)]; }
QString toString(ChangeAction action) { return actionNames[int(action)]; }
QString TrackedChange::summary() const {
  QString name = label.isEmpty() ? "#" + n(entity) : label;
  QString sign = action == ChangeAction::Create ? "+" : action == ChangeAction::Delete ? "-" : "~";
  if(type==EntityType::Spawn && action==ChangeAction::Update && before["creature_movement"]!=after["creature_movement"])
    return sign+" Patrol: "+name+QString(" · %1 waypoints").arg(after["creature_movement"].toArray().size());
  if (type == EntityType::Spawn || type == EntityType::GameObjectSpawn) {
    QString what = action == ChangeAction::Create ? "placed" : action == ChangeAction::Move ? "moved"
                 : action == ChangeAction::Delete ? "placement removed" : "placement edited";
    return sign + " Spawn: " + name + " " + what;
  }
  if (type == EntityType::Gossip) {
    auto had = before["gossip_menu"].toArray().size(), has = after["gossip_menu"].toArray().size();
    QString mark = action == ChangeAction::Delete || (had && !has) ? "-" : !had && has ? "+" : "~";
    return mark + " Dialogue: " + name + (has ? QString(" · %1 %2").arg(has).arg(has == 1 ? "node" : "nodes") : QString());
  }
  if (isService(type)) {
    auto rows = [&](QJsonObject const& state) { QMap<QString, QJsonObject> map; for (auto const& r : state[rowTable(type)].toArray())
      map[r.toObject()[type == EntityType::Trainer ? "spell" : "item"].toString() + "/" + r.toObject()["groupid"].toString()] = r.toObject(); return map; };
    auto had = rows(before), has = rows(after);
    if (type == EntityType::Trainer && before[headTable(type)] == after[headTable(type)]) {
      QStringList added, removed; bool changed = false;
      for (auto it = has.begin(); it != has.end(); ++it) if (!had.contains(it.key())) added << it.key(); else if (had[it.key()] != *it) changed = true;
      for (auto it = had.begin(); it != had.end(); ++it) if (!has.contains(it.key())) removed << it.key();
      auto spellName = [&](QString const& key) {
        for (auto const* state : {&after, &before}) for (auto const& r : (*state)["spell_names"].toArray())
          if (r.toObject()["spell"].toString() + "/" == key)
            return r.toObject()["name"].toString() + (r.toObject()["rank"].toString().isEmpty() ? QString() : " " + r.toObject()["rank"].toString());
        return "spell " + key.section('/', 0, 0);
      };
      if (!changed && added.size() + removed.size() == 1)
        return (added.isEmpty() ? "- Trainer spell: " + spellName(removed[0]) : "+ Trainer spell: " + spellName(added[0])) + " (" + name + ")";
    }
    QString mark = had.isEmpty() && !has.isEmpty() ? "+" : has.isEmpty() && !had.isEmpty() ? "-" : "~";
    return mark + (type == EntityType::Vendor ? " Vendor: " : type == EntityType::Trainer ? " Trainer: " : " Loot: ") + name;
  }
  if (type == EntityType::Spell) {
    auto row = firstRow(action == ChangeAction::Delete ? before : after, "spell_template");
    bool teaches = field(row, "effect1") == 36 && row["description"].toString().startsWith("Teaches");
    return sign + (teaches ? " Spell (trainer lesson): " : " Spell: ") + name;
  }
  return sign + (type == EntityType::Npc ? " NPC: " : type == EntityType::Item ? " Item: " : type == EntityType::GameObject ? " GameObject: " : " Quest: ") + name;
}
ChangeTracker* ChangeTracker::instance() {
  auto runtime = Runtime::RuntimeManager::instance();
  if (!runtime) return nullptr;
  if (auto tracker = runtime->findChild<ChangeTracker*>("creatorChanges", Qt::FindDirectChildrenOnly)) return tracker;
  auto tracker = new ChangeTracker(runtime->root() + "/Workspace", runtime);
  tracker->setObjectName("creatorChanges");
  return tracker;
}
ChangeTracker::ChangeTracker(QString const& workspace, QObject* parent)
  : QObject(parent), _file(workspace + "/creator-changes.json"), _pending(workspace + "/creator-changes.pending.json"),
    _journal(workspace + "/creator-recovery.json") {
  QDir().mkpath(workspace);
  load();
}
void ChangeTracker::load() {
  if (QFile::exists(_pending)) {
    if (QFile::exists(_journal)) QFile::remove(_pending);
    else { QFile::remove(_file); QFile::rename(_pending, _file); }
  }
  QFile file(_file);
  if (!file.exists()) return;
  try {
    require(file.open(QIODevice::ReadOnly), "Cannot read the local changes list.");
    QJsonParseError error; auto doc = QJsonDocument::fromJson(file.readAll(), &error); file.close();
    require(error.error == QJsonParseError::NoError && doc.isObject() && doc.object()["format"].toInt() == 1, "The local changes list is invalid.");
    for (auto const& value : doc.object()["changes"].toArray()) _changes.push_back(fromJson(value.toObject()));
  } catch (std::exception const& e) {
    // Keep the unreadable list for inspection instead of silently overwriting it.
    _changes.clear(); file.close();
    auto kept = _file + ".invalid-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
    QFile::rename(_file, kept);
    _warning = QString::fromUtf8(e.what()) + " It was kept as " + QFileInfo(kept).fileName() + ".";
  }
}
void ChangeTracker::write(QString const& path, QVector<TrackedChange> const& changes) const {
  QJsonArray array; for (auto const& c : changes) array.append(toJson(c));
  auto bytes = QJsonDocument(QJsonObject{{"format", 1}, {"changes", array}}).toJson();
  QSaveFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(), "Cannot record this change in the local changes list.");
}
void ChangeTracker::stage(QVector<TrackedChange> const& changes) {
  auto merged = _changes;
  for (auto const& c : changes) merge(merged, c);
  write(_pending, merged);
  _staged = merged; _hasStaged = true;
}
void ChangeTracker::promote() {
  if (!_hasStaged) return;
  // The database is already committed. If the rename fails the staged list is promoted on next load.
  QFile::remove(_file); QFile::rename(_pending, _file);
  _changes = _staged; _staged.clear(); _hasStaged = false;
  emit changed();
}
void ChangeTracker::discard() { QFile::remove(_pending); _staged.clear(); _hasStaged = false; }
void ChangeTracker::clear(QStringList const& ids) {
  QVector<TrackedChange> kept;
  for (auto const& c : _changes) if (!ids.contains(c.id)) kept.push_back(c);
  write(_file, kept); _changes = kept;
  emit changed();
}
void ChangeTracker::markSynced(QVector<TrackedChange> const& synced) {
  QStringList done;
  for (auto const& c : _changes)
    if (std::any_of(synced.begin(), synced.end(), [&](TrackedChange const& s) { return s.id == c.id && s.timestamp == c.timestamp && s.after == c.after; }))
      done << c.id;
  clear(done);
}
std::optional<ChangeAction> ChangeTracker::derive(EntityType type, QJsonObject const& before, QJsonObject const& after) {
  if (before.isEmpty() && after.isEmpty()) return {};
  if (before.isEmpty()) return ChangeAction::Create;
  if (after.isEmpty()) return ChangeAction::Delete;
  if (before == after) return {};
  if (type == EntityType::Spawn && withoutPosition(before) == withoutPosition(after)) return ChangeAction::Move;
  return ChangeAction::Update;
}
void ChangeTracker::merge(QVector<TrackedChange>& list, TrackedChange change) {
  auto it = std::find_if(list.begin(), list.end(), [&](TrackedChange const& c) { return c.type == change.type && c.entity == change.entity; });
  // Keep the oldest before-state so repeated edits collapse into one net change.
  if (it != list.end()) change.before = it->before;
  auto action = derive(change.type, change.before, change.after);
  if (!action) { if (it != list.end()) list.erase(it); return; }
  change.action = *action;
  if (!change.timestamp.isValid()) change.timestamp = QDateTime::currentDateTimeUtc();
  if (it != list.end()) {
    change.id = it->id;
    if (change.label.isEmpty()) change.label = it->label;
    *it = change;
  } else {
    if (change.id.isEmpty()) change.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    list.push_back(change);
  }
}
QJsonObject ChangeTracker::capture(QueryFunction const& query, EntityType type, Id id, QString* label) {
  QJsonObject rows;
  auto add = [&](QString const& table, QString const& where) {
    QJsonArray array;
    for (auto const& row : query("SELECT * FROM " + table + " WHERE " + where)) array.append(QJsonObject::fromVariantMap(row));
    if (!array.isEmpty()) rows[table] = array;
  };
  if (type == EntityType::Npc) {
    add("creature_template", "entry=" + n(id));
    if (rows.isEmpty()) return rows;
    auto main = firstRow(rows, "creature_template");
    if (auto equipment = field(main, "equipment_id")) add("creature_equip_template", "entry=" + n(equipment));
    // Its own combat spell list (Creator keys it by the NPC entry; a shared game list is not part of the NPC).
    if (field(main, "spell_list_id") == id) add("creature_spells", "entry=" + n(id));
    // Vendor and trainer rows are Vendor and Trainer changes.
    // Relations to Creator quests belong to those quests' own changes.
    for (auto const& table : relations) add(table, "id=" + n(id) + creatorOnlyQuests());
    if (label) *label = main["name"].toString();
  } else if (type == EntityType::GameObject || type == EntityType::GameObjectSpawn) {
    QString table = type == EntityType::GameObject ? "gameobject_template" : "gameobject";
    add(table, (type == EntityType::GameObject ? "entry=" : "guid=") + n(id));
    if (!rows.isEmpty() && label) {
      auto main = firstRow(rows, table);
      if (type == EntityType::GameObject) *label = main["name"].toString();
      else { auto names=query("SELECT name FROM gameobject_template WHERE entry="+n(field(main,"id"))); if(!names.isEmpty()) *label=names[0]["name"].toString(); }
    }
  } else if (isService(type)) {
    bool object = type == EntityType::ObjectLoot;
    QString columns = type == EntityType::Loot ? "loot_id,gold_min,gold_max" : object ? "type,data1,mingold,maxgold"
                    : type == EntityType::Vendor ? "vendor_id,npc_flags" : "trainer_id,trainer_type,trainer_class,npc_flags";
    auto heads = query("SELECT entry,name," + columns + " FROM " + (object ? "gameobject_template" : "creature_template") + " WHERE entry=" + n(id));
    if (heads.isEmpty()) return rows;
    auto head = heads[0];
    if (label) *label = head["name"].toString();
    head.remove("name");
    // Only the role's own flag: the NPC's other roles belong to its NPC change.
    if (head.contains("npc_flags")) {
      auto flag = type == EntityType::Vendor ? 4u : 16u;
      head[type == EntityType::Vendor ? "vendor" : "trainer"] = QString::number((head["npc_flags"].toUInt() & flag) ? 1 : 0);
      head.remove("npc_flags");
    }
    rows[headTable(type)] = QJsonArray{QJsonObject::fromVariantMap(head)};
    if (type == EntityType::Loot) { if (auto loot = head["loot_id"].toUInt()) add(rowTable(type), "entry=" + n(loot)); }
    else if (object) { if (auto loot = head["data1"].toUInt(); loot && chestType(head["type"].toUInt())) add(rowTable(type), "entry=" + n(loot)); }
    else add(rowTable(type), "entry=" + n(id));
    if (type == EntityType::Trainer) {
      auto spells = ids(rows["npc_trainer"].toArray(), "spell");
      if (!spells.isEmpty()) {
        QJsonArray names;
        for (auto const& row : query("SELECT t.entry AS spell,s.name,s.nameSubtext AS `rank` FROM spell_template t JOIN spell_template s ON s.entry=t.effectTriggerSpell1 WHERE t.entry IN (" + spells.join(',') + ")"))
          names.append(QJsonObject::fromVariantMap(row));
        if (!names.isEmpty()) rows["spell_names"] = names;
      }
    }
  } else if (type == EntityType::Gossip) {
    auto heads = query("SELECT entry,name,gossip_menu_id,npc_flags FROM creature_template WHERE entry=" + n(id));
    if (heads.isEmpty()) return rows;
    auto head = heads[0];
    if (label) *label = head["name"].toString();
    head.remove("name");
    // Only the gossip flag: the NPC's other roles belong to its NPC change.
    head["gossip"] = QString::number((head["npc_flags"].toUInt() & 1) ? 1 : 0);
    head.remove("npc_flags");
    rows["creature_gossip"] = QJsonArray{QJsonObject::fromVariantMap(head)};
    // Only rows made in Noggit: a game NPC's dialogue it still uses stays original content.
    QJsonArray marks;
    auto owned = [&](QString const& kind, QStringList list) {
      list.removeDuplicates(); list.removeAll("0"); QStringList result;
      if (list.isEmpty()) return result;
      for (auto const& row : query("SELECT entry FROM creator_content WHERE kind='" + kind + "' AND entry IN (" + list.join(',') + ") ORDER BY entry")) {
        result << row["entry"].toString(); marks.append(QJsonObject{{"kind", kind}, {"entry", row["entry"].toString()}});
      }
      return result;
    };
    QStringList menus, frontier = owned("gossip_menu", {head["gossip_menu_id"].toString()});
    while (!frontier.isEmpty() && menus.size() < 256) {
      menus << frontier;
      QStringList next;
      for (auto const& row : query("SELECT action_menu_id FROM gossip_menu_option WHERE option_id=1 AND action_menu_id>0 AND menu_id IN (" + frontier.join(',') + ") ORDER BY menu_id,id"))
        if (auto target = row["action_menu_id"].toString(); !menus.contains(target) && !next.contains(target)) next << target;
      frontier = owned("gossip_menu", next);
    }
    if (!menus.isEmpty()) {
      add("gossip_menu", "entry IN (" + menus.join(',') + ") ORDER BY entry,text_id");
      add("gossip_menu_option", "menu_id IN (" + menus.join(',') + ") ORDER BY menu_id,id");
      if (auto texts = owned("npc_text", ids(rows["gossip_menu"].toArray(), "text_id")); !texts.isEmpty()) add("npc_text", "ID IN (" + texts.join(',') + ") ORDER BY ID");
      QStringList lines;
      for (auto const& row : rows["npc_text"].toArray()) for (int i = 0; i < 8; ++i) lines << n(field(row.toObject(), "BroadcastTextID" + n(i)));
      if (lines = owned("gossip_text", lines); !lines.isEmpty()) add("broadcast_text", "entry IN (" + lines.join(',') + ") ORDER BY entry");
      auto scripts = owned("gossip_script", ids(rows["gossip_menu_option"].toArray(), "action_script_id") + ids(rows["gossip_menu"].toArray(), "script_id"));
      if (!scripts.isEmpty()) add("gossip_scripts", "id IN (" + scripts.join(',') + ") ORDER BY id,delay,priority");
      // Conditions made in Noggit, and those a combined one names. Ones the game already had are assumed present.
      QStringList conditions = ids(rows["gossip_menu_option"].toArray(), "condition_id") + ids(rows["gossip_menu"].toArray(), "condition_id"), seen;
      QVector<QJsonObject> found;
      while (true) {
        QStringList fresh; for (auto const& c : conditions) if (!seen.contains(c)) fresh << c;
        if ((fresh = owned("condition", fresh)).isEmpty()) break;
        seen << fresh; conditions.clear();
        for (auto const& row : query("SELECT * FROM conditions WHERE condition_entry IN (" + fresh.join(',') + ")")) {
          found.push_back(QJsonObject::fromVariantMap(row));
          if (row["type"].toInt() < 0) for (int i = 1; i <= 4; ++i) conditions << row["value" + n(i)].toString();
        }
      }
      std::sort(found.begin(), found.end(), [](QJsonObject const& a, QJsonObject const& b) { return field(a, "condition_entry") < field(b, "condition_entry"); });
      QJsonArray array; for (auto const& c : found) array.append(c);
      if (!array.isEmpty()) rows["conditions"] = array;
    }
    if (!marks.isEmpty()) rows["creator_marks"] = marks;
  } else if (type == EntityType::Spawn) {
    add("creature", "guid=" + n(id));
    if (rows.isEmpty()) return rows;
    add("creature_movement", "id=" + n(id));
    auto scripts=ids(rows["creature_movement"].toArray(),"script_id"); scripts.removeAll("0");
    if(!scripts.isEmpty()) add("creature_movement_scripts","id IN ("+scripts.join(',')+")");
    if (rows.isEmpty() || !label) return rows;
    auto names = query("SELECT name FROM creature_template WHERE entry=" + n(field(firstRow(rows, "creature"), "id")));
    if (!names.isEmpty()) *label = names[0]["name"].toString();
  } else if (type == EntityType::Spell) {
    add("spell_template", "entry=" + n(id));
    if (rows.isEmpty()) return rows;
    add("spell_chain", "spell_id=" + n(id));
    if (label) {
      auto main = firstRow(rows, "spell_template"); auto rank = main["nameSubtext"].toString();
      *label = main["name"].toString() + (rank.isEmpty() ? QString() : " (" + rank + ")");
    }
  } else if (type == EntityType::Item) {
    add("item_template", "entry=" + n(id));
    if (!rows.isEmpty() && label) *label = firstRow(rows, "item_template")["name"].toString();
  } else {
    add("quest_template", "entry=" + n(id));
    if (rows.isEmpty()) return rows;
    auto quest = firstRow(rows, "quest_template");
    for (auto const& table : questLinks) add(table, "quest=" + n(id));
    QStringList texts;
    for (int i = 0; i < 2; ++i) {
      auto script = field(quest, scriptColumns[i]);
      if (!script) continue;
      add(scriptTables[i], "id=" + n(script));
      for (auto const& row : rows[scriptTables[i]].toArray())
        if (field(row.toObject(), "command") == 0 && field(row.toObject(), "dataint") >= ownTextStart) texts << n(field(row.toObject(), "dataint"));
    }
    if (!texts.isEmpty()) add("broadcast_text", "entry IN (" + texts.join(',') + ")");
    // Quest-only drops of the items it collects, and the loot tables the editor gave their sources.
    QStringList items;
    for (int i = 1; i <= 4; ++i) if (auto item = field(quest, "ReqItemId" + n(i))) items << n(item);
    if (!items.isEmpty()) {
      for (auto const& table : lootTables) add(table, "item IN (" + items.join(',') + ") AND ChanceOrQuestChance<0");
      auto link = [&](QString const& name, QString const& sql) {
        QJsonArray array; for (auto const& row : query(sql)) array.append(QJsonObject::fromVariantMap(row));
        if (!array.isEmpty()) rows[name] = array;
      };
      if (auto loot = ids(rows["creature_loot_template"].toArray(), "entry"); !loot.isEmpty())
        link("creature_loot_link", "SELECT entry FROM creature_template WHERE entry=loot_id AND loot_id IN (" + loot.join(',') + ")");
      if (auto loot = ids(rows["gameobject_loot_template"].toArray(), "entry"); !loot.isEmpty())
        link("gameobject_loot_link", "SELECT entry FROM gameobject_template WHERE type=3 AND entry=data1 AND data1 IN (" + loot.join(',') + ")");
    }
    QJsonArray starts; for (auto const& row : query("SELECT entry FROM item_template WHERE start_quest=" + n(id))) starts.append(QJsonObject::fromVariantMap(row));
    if (!starts.isEmpty()) rows["item_start_link"] = starts;
    if (label) *label = quest["Title"].toString();
  }
  return rows;
}
QString ChangeTracker::sql(QVector<TrackedChange> const& changes) {
  QString out =
    "-- Generated by Noggit Creator. Apply to a compatible Tortoise world (mangos) database.\n"
    "-- Contains only the exported changes, not a database dump.\n"
    "SET NAMES utf8mb4;\n"
    "CREATE TABLE IF NOT EXISTS creator_content (kind VARCHAR(20) NOT NULL, entry INT UNSIGNED NOT NULL, PRIMARY KEY(kind,entry)) ENGINE=InnoDB;\n";
  auto ordered = [&](EntityType type, bool deleted) {
    QVector<TrackedChange> result;
    for (auto const& c : changes) if (c.type == type && (c.action == ChangeAction::Delete) == deleted) result.push_back(c);
    return result;
  };
  auto ownerGuard = [](TrackedChange const& c) { return ownedGuard(c.type == EntityType::ObjectLoot ? "gameobject" : "npc", c.entity); };
  // A loot table is only written when it is the owner's own (the loot editor gives it one).
  auto ownTable = [](TrackedChange const& c, QJsonObject const& state) {
    auto head = firstRow(state, headTable(c.type));
    if (c.type == EntityType::Loot) return field(head, "loot_id") == c.entity;
    if (c.type == EntityType::ObjectLoot) return field(head, "data1") == c.entity && chestType(field(head, "type"));
    return true;
  };
  // Services go before their owners, while the owner's Creator mark still guards them.
  for (auto type : {EntityType::Loot, EntityType::ObjectLoot, EntityType::Vendor, EntityType::Trainer})
    for (auto const& c : ordered(type, true)) {
      out += "\n-- Remove " + QString(type == EntityType::Vendor ? "vendor" : type == EntityType::Trainer ? "trainer" : "loot") + ": " + oneLine(c.label) + " (#" + n(c.entity) + ")\n" + ownerGuard(c);
      if (ownTable(c, c.before)) out += "DELETE FROM " + rowTable(type) + " WHERE entry=" + n(c.entity) + " AND @owned>0;\n";
    }
  for (auto const& c : ordered(EntityType::Gossip, true))
    out += "\n-- Remove dialogue: " + oneLine(c.label) + " (#" + n(c.entity) + ")\n" + ownedGuard("npc", c.entity) + gossipRemoval(c.before, {});
  // Remove dependents before their NPCs, then recreate NPCs before what refers to them.
  for (auto const& c : ordered(EntityType::Quest, true)) {
    auto id = n(c.entity);
    out += "\n-- Delete quest: " + oneLine(c.label) + " (#" + id + ")\n" + ownedGuard("quest", c.entity);
    for (auto const& table : questLinks) out += "DELETE FROM " + table + " WHERE quest=" + id + " AND @owned>0;\n";
    auto quest = firstRow(c.before, "quest_template");
    for (int i = 0; i < 2; ++i)
      if (auto script = field(quest, scriptColumns[i])) out += "DELETE FROM " + scriptTables[i] + " WHERE id=" + n(script) + " AND @owned>0;\n";
    if (auto texts = ids(c.before["broadcast_text"].toArray(), "entry"); !texts.isEmpty())
      out += "DELETE FROM broadcast_text WHERE entry IN (" + texts.join(',') + ") AND @owned>0;\n";
    for (auto const& table : lootTables)
      for (auto const& row : c.before[table].toArray()) out += dropRemoval(table, row.toObject(), c.entity, " AND @owned>0");
    out += "UPDATE item_template SET start_quest=0 WHERE start_quest=" + id + " AND @owned>0;\n";
    out += "DELETE FROM quest_template WHERE entry=" + id + " AND @owned>0;\n";
    out += "DELETE FROM creator_content WHERE kind='quest' AND entry=" + id + ";\n";
  }
  for (auto const& c : ordered(EntityType::Spell, true)) {
    auto id = n(c.entity);
    out += "\n-- Delete spell: " + oneLine(c.label) + " (#" + id + ")\n" + ownedGuard("spell", c.entity);
    out += "DELETE FROM spell_chain WHERE spell_id=" + id + " AND @owned>0;\n";
    out += "DELETE FROM spell_template WHERE entry=" + id + " AND @owned>0;\n";
    out += "DELETE FROM creator_content WHERE kind='spell' AND entry=" + id + ";\n";
  }
  for (auto const& c : ordered(EntityType::Item, true)) {
    auto id = n(c.entity);
    out += "\n-- Delete item: " + oneLine(c.label) + " (#" + id + ")\n" + ownedGuard("item", c.entity);
    for (auto const& table : QStringList{"creature_loot_template", "gameobject_loot_template", "npc_vendor"})
      out += "DELETE FROM " + table + " WHERE item=" + id + " AND @owned>0;\n";
    out += "DELETE FROM item_template WHERE entry=" + id + " AND @owned>0;\n";
    out += "DELETE FROM creator_content WHERE kind='item' AND entry=" + id + ";\n";
  }
  for (auto type : {EntityType::GameObject, EntityType::GameObjectSpawn}) {
    QString table=type==EntityType::GameObject?"gameobject_template":"gameobject";
    QString key=type==EntityType::GameObject?"entry":"guid";
    for(auto const& c:ordered(type,false)) {
      out+=replaceRows(table,c.after[table].toArray());
      out+="INSERT IGNORE INTO creator_content(kind,entry) VALUES('"+toString(type)+"',"+n(c.entity)+");\n";
    }
    for(auto const& c:ordered(type,true)) {
      out+=ownedGuard(toString(type),c.entity);
      out+="DELETE FROM "+table+" WHERE "+key+"="+n(c.entity)+" AND @owned>0;\n";
      out+="DELETE FROM creator_content WHERE kind='"+toString(type)+"' AND entry="+n(c.entity)+";\n";
    }
  }
  auto removePath=[&](TrackedChange const& c) {
    QString sql;

    sql+="DELETE FROM creature_movement WHERE id="+n(c.entity)+";\n";
    return sql;
  };
  for (auto const& c : ordered(EntityType::Spawn, true)) {
    out += "\n-- Remove placement: " + oneLine(c.label) + " (#" + n(c.entity) + ")\n" + ownedGuard("spawn", c.entity);
    out += "DELETE FROM creature_movement WHERE id="+n(c.entity)+" AND @owned>0;\n";
    out += "DELETE FROM creature WHERE guid=" + n(c.entity) + " AND @owned>0;\n";
    out += "DELETE FROM creator_content WHERE kind='spawn' AND entry=" + n(c.entity) + ";\n";
  }
  for (auto const& c : ordered(EntityType::Npc, true)) {
    auto id = n(c.entity);
    out += "\n-- Delete NPC: " + oneLine(c.label) + " (#" + id + ")\n" + ownedGuard("npc", c.entity);
    out += "DELETE FROM creator_content WHERE kind='spawn' AND @owned>0 AND entry IN (SELECT guid FROM creature WHERE id=" + id + ");\n";
    out += "DELETE FROM creature_movement WHERE id IN (SELECT guid FROM creature WHERE id="+id+") AND @owned>0;\n";
    out += "DELETE FROM creature WHERE id=" + id + " AND @owned>0;\n";
    out += "DELETE FROM npc_vendor WHERE entry=" + id + " AND @owned>0;\n";
    out += "DELETE FROM npc_trainer WHERE entry=" + id + " AND @owned>0;\n";
    if (c.before.contains("creature_spells")) out += "DELETE FROM creature_spells WHERE entry=" + id + " AND @owned>0;\n";
    for (auto const& table : relations) out += "DELETE FROM " + table + " WHERE id=" + id + " AND @owned>0;\n";
    if (auto equipment = field(firstRow(c.before, "creature_template"), "equipment_id"); equipment >= 1000000)
      out += "DELETE FROM creature_equip_template WHERE entry=" + n(equipment) + " AND @owned>0 AND NOT EXISTS (SELECT 1 FROM creature_template WHERE equipment_id=" + n(equipment) + " AND entry<>" + id + ");\n";
    out += "DELETE FROM creature_template WHERE entry=" + id + " AND @owned>0;\n";
    out += "DELETE FROM creator_content WHERE kind='npc' AND entry=" + id + ";\n";
  }
  for (auto const& c : ordered(EntityType::Npc, false)) {
    auto id = n(c.entity);
    out += "\n-- NPC: " + oneLine(c.label) + " (#" + id + ")\n";
    // Vendor and trainer rows are Vendor/Trainer changes now; entries recorded before that still carry them.
    bool legacy = false;
    for (auto table : {"npc_vendor", "npc_trainer"}) legacy = legacy || c.before.contains(table) || c.after.contains(table);
    if (legacy) out += "DELETE FROM npc_vendor WHERE entry=" + id + ";\nDELETE FROM npc_trainer WHERE entry=" + id + ";\n";
    for (auto const& table : relations) out += "DELETE FROM " + table + " WHERE id=" + id + creatorOnlyQuests() + ";\n";
    // The NPC's own combat spell list is replaced whole (removed when the edit dropped every spell).
    if (c.before.contains("creature_spells") || c.after.contains("creature_spells"))
      out += "DELETE FROM creature_spells WHERE entry=" + id + ";\n" + replaceRows("creature_spells", c.after["creature_spells"].toArray());
    for (auto const& table : {"creature_equip_template", "creature_template", "npc_vendor", "npc_trainer", "creature_questrelation", "creature_involvedrelation"})
      if (legacy || (QString(table) != "npc_vendor" && QString(table) != "npc_trainer")) out += replaceRows(table, c.after[table].toArray());
    out += "INSERT IGNORE INTO creator_content(kind,entry) VALUES('npc'," + id + ");\n";
  }
  // Spells before what uses them (items, trainers, quests); their client rows are not part of this script.
  for (auto const& c : ordered(EntityType::Spell, false)) {
    auto id = n(c.entity);
    out += "\n-- Spell: " + oneLine(c.label) + " (#" + id + ")\n" + replaceRows("spell_template", c.after["spell_template"].toArray());
    out += "DELETE FROM spell_chain WHERE spell_id=" + id + ";\n" + replaceRows("spell_chain", c.after["spell_chain"].toArray());
    out += "INSERT IGNORE INTO creator_content(kind,entry) VALUES('spell'," + id + ");\n";
  }
  for (auto const& c : ordered(EntityType::Item, false)) {
    out += "\n-- Item: " + oneLine(c.label) + " (#" + n(c.entity) + ")\n" + replaceRows("item_template", c.after["item_template"].toArray());
    out += "INSERT IGNORE INTO creator_content(kind,entry) VALUES('item'," + n(c.entity) + ");\n";
  }
  for (auto const& c : ordered(EntityType::Spawn, false)) {
    out += removePath(c);
    for(auto const& script:ids(c.after["creature_movement_scripts"].toArray(),"id"))
      out += "DELETE FROM creature_movement_scripts WHERE id="+script+";\n";
    out += replaceRows("creature_movement_scripts", c.after["creature_movement_scripts"].toArray());
    out += replaceRows("creature_movement", c.after["creature_movement"].toArray());
    out += "\n-- Placement: " + oneLine(c.label) + " (#" + n(c.entity) + ")\n" + replaceRows("creature", c.after["creature"].toArray());
    out += "INSERT IGNORE INTO creator_content(kind,entry) VALUES('spawn'," + n(c.entity) + ");\n";
  }
  for (auto type : {EntityType::Loot, EntityType::ObjectLoot, EntityType::Vendor, EntityType::Trainer})
    for (auto const& c : ordered(type, false)) {
      auto id = n(c.entity);
      auto head = firstRow(c.after, headTable(type));
      out += "\n-- " + QString(type == EntityType::Vendor ? "Vendor" : type == EntityType::Trainer ? "Trainer" : "Loot") + ": " + oneLine(c.label) + " (#" + id + ")\n" + ownerGuard(c);
      QStringList sets;
      auto set = [&](QString const& column) { sets << identifier(column) + "=" + literal(head[column]); };
      if (type == EntityType::Loot) for (auto column : {"loot_id", "gold_min", "gold_max"}) set(column);
      else if (type == EntityType::ObjectLoot) for (auto column : {"mingold", "maxgold"}) set(column);
      else if (type == EntityType::Vendor) { set("vendor_id"); sets << QString("`npc_flags`=(`npc_flags`&~4)|%1").arg(field(head, "vendor") ? 4 : 0); }
      else { for (auto column : {"trainer_id", "trainer_type", "trainer_class"}) set(column); sets << QString("`npc_flags`=(`npc_flags`&~16)|%1").arg(field(head, "trainer") ? 16 : 0); }
      if (type == EntityType::ObjectLoot && chestType(field(head, "type"))) set("data1");
      out += "UPDATE " + QString(type == EntityType::ObjectLoot ? "gameobject_template" : "creature_template") + " SET " + sets.join(',') + " WHERE entry=" + id + " AND @owned>0;\n";
      if (!ownTable(c, c.after)) continue;
      out += "DELETE FROM " + rowTable(type) + " WHERE entry=" + id + " AND @owned>0;\n";
      out += replaceRowsIfOwned(rowTable(type), c.after[rowTable(type)].toArray());
    }
  // Dialogues replace their previous version whole; conditions are shared by value, so only missing ones are added.
  for (auto const& c : ordered(EntityType::Gossip, false)) {
    auto head = firstRow(c.after, "creature_gossip");
    out += "\n-- Dialogue: " + oneLine(c.label) + " (#" + n(c.entity) + ")\n" + ownedGuard("npc", c.entity) + gossipRemoval(c.before, c.after);
    out += insertIgnoreIfOwned("conditions", c.after["conditions"].toArray());
    for (auto const& table : gossipWritten) out += replaceRowsIfOwned(table, c.after[table].toArray());
    for (auto const& value : c.after["creator_marks"].toArray()) {
      auto m = value.toObject();
      out += "INSERT IGNORE INTO creator_content(kind,entry) SELECT " + literal(m["kind"]) + "," + n(field(m, "entry")) + " FROM DUAL WHERE @owned>0;\n";
    }
    out += "UPDATE creature_template SET `gossip_menu_id`=" + n(field(head, "gossip_menu_id")) + QString(",`npc_flags`=(`npc_flags`&~1)|%1").arg(field(head, "gossip") ? 1 : 0)
         + " WHERE entry=" + n(c.entity) + " AND @owned>0;\n";
  }
  for (auto const& c : ordered(EntityType::Quest, false)) {
    auto id = n(c.entity);
    out += "\n-- Quest: " + oneLine(c.label) + " (#" + id + ")\n";
    for (auto const& table : questLinks) out += "DELETE FROM " + table + " WHERE quest=" + id + ";\n";
    // Scripts are replaced whole; spoken lines, drops and the start item the edit dropped are removed.
    QStringList scripts[2];
    for (auto const* rows : {&c.before, &c.after})
      for (int i = 0; i < 2; ++i) if (auto script = field(firstRow(*rows, "quest_template"), scriptColumns[i])) scripts[i] << n(script);
    for (int i = 0; i < 2; ++i) {
      scripts[i].removeDuplicates();
      for (auto const& script : scripts[i]) out += "DELETE FROM " + scriptTables[i] + " WHERE id=" + script + ";\n";
    }
    if (auto texts = ids(removedRows(c.before, c.after, "broadcast_text", {"entry"}), "entry"); !texts.isEmpty())
      out += "DELETE FROM broadcast_text WHERE entry IN (" + texts.join(',') + ");\n";
    for (auto const& table : lootTables)
      for (auto const& row : removedRows(c.before, c.after, table, {"entry", "item"})) out += dropRemoval(table, row.toObject(), c.entity, "");
    for (auto const& row : removedRows(c.before, c.after, "item_start_link", {"entry"}))
      out += "UPDATE item_template SET start_quest=0 WHERE entry=" + n(field(row.toObject(), "entry")) + " AND start_quest=" + id + ";\n";
    out += replaceRows("quest_template", c.after["quest_template"].toArray());
    for (auto const& table : questLinks) out += replaceRows(table, c.after[table].toArray());
    for (auto const& table : scriptTables) out += replaceRows(table, c.after[table].toArray());
    out += replaceRows("broadcast_text", c.after["broadcast_text"].toArray());
    for (auto const& table : lootTables) out += replaceRows(table, c.after[table].toArray());
    for (auto const& row : c.after["creature_loot_link"].toArray()) {
      auto entry = n(field(row.toObject(), "entry"));
      out += "UPDATE creature_template SET loot_id=" + entry + " WHERE entry=" + entry + " AND loot_id=0;\n";
    }
    for (auto const& row : c.after["gameobject_loot_link"].toArray()) {
      auto entry = n(field(row.toObject(), "entry"));
      out += "UPDATE gameobject_template SET data1=" + entry + " WHERE entry=" + entry + " AND type=3 AND data1=0;\n";
    }
    for (auto const& row : c.after["item_start_link"].toArray())
      out += "UPDATE item_template SET start_quest=" + id + " WHERE entry=" + n(field(row.toObject(), "entry")) + ";\n";
    QStringList npcs;
    for (auto const& table : relations) npcs << ids(c.after[table].toArray(), "id");
    npcs.removeDuplicates();
    if (!npcs.isEmpty()) out += "UPDATE creature_template SET npc_flags=npc_flags|2 WHERE entry IN (" + npcs.join(',') + ");\n";
    out += "INSERT IGNORE INTO creator_content(kind,entry) VALUES('quest'," + id + ");\n";
  }
  return out;
}
QString ChangeTracker::upsert(QString const& table, QJsonArray const& rows) { return replaceRows(table, rows); }
QVector<QPair<QString, QString>> ChangeTracker::footprint(QVector<TrackedChange> const& changes, QueryFunction const& target) {
  QVector<QPair<QString, QString>> scopes;
  auto add = [&](QString const& table, QString const& column, QStringList ids) {
    ids.removeDuplicates(); ids.removeAll("0");
    if (ids.isEmpty()) return;
    QPair<QString, QString> scope{table, column + (ids.size() == 1 ? "=" + ids[0] : " IN (" + ids.join(',') + ")")};
    if (!scopes.contains(scope)) scopes.push_back(scope);
  };
  auto owned = [&](QString const& kind, QStringList const& ids) { add("creator_content", "kind='" + kind + "' AND entry", ids); };
  for (auto const& c : changes) {
    auto id = n(c.entity);
    QJsonObject const states[] = {c.before, c.after, capture(target, c.type, c.entity)};
    auto values = [&](QString const& table, QString const& column) {
      QStringList result;
      for (auto const& state : states) for (auto const& row : state[table].toArray()) result << n(field(row.toObject(), column));
      return result;
    };
    owned(typeNames[int(c.type)], {id});
    if (c.type == EntityType::Npc) {
      add("creature_template", "entry", {id});
      add("creature_equip_template", "entry", values("creature_template", "equipment_id"));
      add("npc_vendor", "entry", {id});
      add("npc_trainer", "entry", {id});
      add("creature_spells", "entry", {id});
      for (auto const& table : relations) add(table, "id", {id});
      if (c.action == ChangeAction::Delete) {
        QStringList guids;
        for (auto const& row : target("SELECT guid FROM creature WHERE id=" + id)) guids << row["guid"].toString();
        add("creature", "guid", guids);
        add("creature_movement", "id", guids);
        owned("spawn", guids);
      }
    } else if (isService(c.type)) {
      add(c.type == EntityType::ObjectLoot ? "gameobject_template" : "creature_template", "entry", {id});
      if (c.type == EntityType::Loot) add(rowTable(c.type), "entry", values(headTable(c.type), "loot_id"));
      else if (c.type == EntityType::ObjectLoot) add(rowTable(c.type), "entry", values(headTable(c.type), "data1"));
      else add(rowTable(c.type), "entry", {id});
    } else if (c.type == EntityType::Gossip) {
      add("creature_template", "entry", {id});
      auto menus = values("gossip_menu", "entry");
      add("gossip_menu_option", "menu_id", menus);
      for (auto const& t : gossipTables) add(t.table, t.key, values(t.table, t.key));
      add("conditions", "condition_entry", values("conditions", "condition_entry"));
      QMap<QString, QStringList> marks;
      for (auto const& state : states)
        for (auto const& m : state["creator_marks"].toArray()) marks[m.toObject()["kind"].toString()] << m.toObject()["entry"].toString();
      for (auto it = marks.begin(); it != marks.end(); ++it) owned(it.key(), it.value());
    } else if (c.type == EntityType::GameObject || c.type == EntityType::GameObjectSpawn) {
      add(c.type==EntityType::GameObject?"gameobject_template":"gameobject",c.type==EntityType::GameObject?"entry":"guid",{id});
    } else if (c.type == EntityType::Spawn) {
      add("creature", "guid", {id});
      add("creature_movement", "id", {id});
      add("creature_movement_scripts", "id", values("creature_movement_scripts", "id"));
    } else if (c.type == EntityType::Spell) {
      add("spell_template", "entry", {id});
      add("spell_chain", "spell_id", {id});
    } else if (c.type == EntityType::Item) {
      add("item_template", "entry", {id});
      if (c.action == ChangeAction::Delete)
        for (auto const& table : QStringList{"creature_loot_template", "gameobject_loot_template", "npc_vendor"}) add(table, "item", {id});
    } else {
      add("quest_template", "entry", {id});
      for (auto const& table : questLinks) add(table, "quest", {id});
      for (int i = 0; i < 2; ++i) add(scriptTables[i], "id", values("quest_template", scriptColumns[i]));
      add("broadcast_text", "entry", values("broadcast_text", "entry"));
      QStringList items;
      for (int i = 1; i <= 4; ++i) items << values("quest_template", "ReqItemId" + n(i));
      for (auto const& table : lootTables) add(table, "item", items);
      // Loot links and the giver flag are set on these templates.
      QStringList creatures = values("creature_loot_link", "entry");
      for (auto const& table : relations) creatures << values(table, "id");
      add("creature_template", "entry", creatures);
      add("gameobject_template", "entry", values("gameobject_loot_link", "entry"));
      add("item_template", "entry", values("item_start_link", "entry"));
    }
  }
  return scopes;
}
QStringList ChangeTracker::statements(QString const& sql) {
  QStringList result;
  for (auto const& line : sql.split('\n')) {
    auto statement = line.trimmed();
    if (!statement.isEmpty() && !statement.startsWith("--")) result << statement;
  }
  return result;
}
}
