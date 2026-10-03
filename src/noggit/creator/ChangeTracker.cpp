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
QStringList const typeNames{"npc", "spawn", "quest", "item"};
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
  if (type == EntityType::Spawn) {
    QString what = action == ChangeAction::Create ? "placed" : action == ChangeAction::Move ? "moved"
                 : action == ChangeAction::Delete ? "placement removed" : "placement edited";
    return sign + " Spawn: " + name + " " + what;
  }
  return sign + (type == EntityType::Npc ? " NPC: " : type == EntityType::Item ? " Item: " : " Quest: ") + name;
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
    add("npc_vendor", "entry=" + n(id));
    add("npc_trainer", "entry=" + n(id));
    // Relations to Creator quests belong to those quests' own changes.
    for (auto const& table : relations) add(table, "id=" + n(id) + creatorOnlyQuests());
    if (label) *label = main["name"].toString();
  } else if (type == EntityType::Spawn) {
    add("creature", "guid=" + n(id));
    if (rows.isEmpty() || !label) return rows;
    auto names = query("SELECT name FROM creature_template WHERE entry=" + n(field(firstRow(rows, "creature"), "id")));
    if (!names.isEmpty()) *label = names[0]["name"].toString();
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
  for (auto const& c : ordered(EntityType::Item, true)) {
    auto id = n(c.entity);
    out += "\n-- Delete item: " + oneLine(c.label) + " (#" + id + ")\n" + ownedGuard("item", c.entity);
    for (auto const& table : QStringList{"creature_loot_template", "gameobject_loot_template", "npc_vendor"})
      out += "DELETE FROM " + table + " WHERE item=" + id + " AND @owned>0;\n";
    out += "DELETE FROM item_template WHERE entry=" + id + " AND @owned>0;\n";
    out += "DELETE FROM creator_content WHERE kind='item' AND entry=" + id + ";\n";
  }
  for (auto const& c : ordered(EntityType::Spawn, true)) {
    out += "\n-- Remove placement: " + oneLine(c.label) + " (#" + n(c.entity) + ")\n" + ownedGuard("spawn", c.entity);
    out += "DELETE FROM creature WHERE guid=" + n(c.entity) + " AND @owned>0;\n";
    out += "DELETE FROM creator_content WHERE kind='spawn' AND entry=" + n(c.entity) + ";\n";
  }
  for (auto const& c : ordered(EntityType::Npc, true)) {
    auto id = n(c.entity);
    out += "\n-- Delete NPC: " + oneLine(c.label) + " (#" + id + ")\n" + ownedGuard("npc", c.entity);
    out += "DELETE FROM creator_content WHERE kind='spawn' AND @owned>0 AND entry IN (SELECT guid FROM creature WHERE id=" + id + ");\n";
    out += "DELETE FROM creature WHERE id=" + id + " AND @owned>0;\n";
    out += "DELETE FROM npc_vendor WHERE entry=" + id + " AND @owned>0;\n";
    out += "DELETE FROM npc_trainer WHERE entry=" + id + " AND @owned>0;\n";
    for (auto const& table : relations) out += "DELETE FROM " + table + " WHERE id=" + id + " AND @owned>0;\n";
    if (auto equipment = field(firstRow(c.before, "creature_template"), "equipment_id"); equipment >= 1000000)
      out += "DELETE FROM creature_equip_template WHERE entry=" + n(equipment) + " AND @owned>0 AND NOT EXISTS (SELECT 1 FROM creature_template WHERE equipment_id=" + n(equipment) + " AND entry<>" + id + ");\n";
    out += "DELETE FROM creature_template WHERE entry=" + id + " AND @owned>0;\n";
    out += "DELETE FROM creator_content WHERE kind='npc' AND entry=" + id + ";\n";
  }
  for (auto const& c : ordered(EntityType::Npc, false)) {
    auto id = n(c.entity);
    out += "\n-- NPC: " + oneLine(c.label) + " (#" + id + ")\n";
    out += "DELETE FROM npc_vendor WHERE entry=" + id + ";\nDELETE FROM npc_trainer WHERE entry=" + id + ";\n";
    for (auto const& table : relations) out += "DELETE FROM " + table + " WHERE id=" + id + creatorOnlyQuests() + ";\n";
    for (auto const& table : {"creature_equip_template", "creature_template", "npc_vendor", "npc_trainer", "creature_questrelation", "creature_involvedrelation"})
      out += replaceRows(table, c.after[table].toArray());
    out += "INSERT IGNORE INTO creator_content(kind,entry) VALUES('npc'," + id + ");\n";
  }
  for (auto const& c : ordered(EntityType::Item, false)) {
    out += "\n-- Item: " + oneLine(c.label) + " (#" + n(c.entity) + ")\n" + replaceRows("item_template", c.after["item_template"].toArray());
    out += "INSERT IGNORE INTO creator_content(kind,entry) VALUES('item'," + n(c.entity) + ");\n";
  }
  for (auto const& c : ordered(EntityType::Spawn, false)) {
    out += "\n-- Placement: " + oneLine(c.label) + " (#" + n(c.entity) + ")\n" + replaceRows("creature", c.after["creature"].toArray());
    out += "INSERT IGNORE INTO creator_content(kind,entry) VALUES('spawn'," + n(c.entity) + ");\n";
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
      for (auto const& table : relations) add(table, "id", {id});
      if (c.action == ChangeAction::Delete) {
        QStringList guids;
        for (auto const& row : target("SELECT guid FROM creature WHERE id=" + id)) guids << row["guid"].toString();
        add("creature", "guid", guids);
        owned("spawn", guids);
      }
    } else if (c.type == EntityType::Spawn) {
      add("creature", "guid", {id});
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
