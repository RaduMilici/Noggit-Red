#include "ChangeTracker.hpp"
#include <noggit/runtime/RuntimeManager.hpp>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
void require(bool b, QString const& message) { if (!b) throw std::runtime_error(message.toStdString()); }
QString n(Id id) { return QString::number(id); }
QStringList const typeNames{"npc", "spawn", "quest"};
QStringList const actionNames{"CREATE", "UPDATE", "DELETE", "MOVE"};
QStringList const relations{"creature_questrelation", "creature_involvedrelation"};
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
  return sign + (type == EntityType::Npc ? " NPC: " : " Quest: ") + name;
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
  } else {
    add("quest_template", "entry=" + n(id));
    if (rows.isEmpty()) return rows;
    for (auto const& table : relations) add(table, "quest=" + n(id));
    if (label) *label = firstRow(rows, "quest_template")["Title"].toString();
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
    out += "\n-- Delete quest: " + c.label + " (#" + n(c.entity) + ")\n" + ownedGuard("quest", c.entity);
    for (auto const& table : relations) out += "DELETE FROM " + table + " WHERE quest=" + n(c.entity) + " AND @owned>0;\n";
    out += "DELETE FROM quest_template WHERE entry=" + n(c.entity) + " AND @owned>0;\n";
    out += "DELETE FROM creator_content WHERE kind='quest' AND entry=" + n(c.entity) + ";\n";
  }
  for (auto const& c : ordered(EntityType::Spawn, true)) {
    out += "\n-- Remove placement: " + c.label + " (#" + n(c.entity) + ")\n" + ownedGuard("spawn", c.entity);
    out += "DELETE FROM creature WHERE guid=" + n(c.entity) + " AND @owned>0;\n";
    out += "DELETE FROM creator_content WHERE kind='spawn' AND entry=" + n(c.entity) + ";\n";
  }
  for (auto const& c : ordered(EntityType::Npc, true)) {
    auto id = n(c.entity);
    out += "\n-- Delete NPC: " + c.label + " (#" + id + ")\n" + ownedGuard("npc", c.entity);
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
    out += "\n-- NPC: " + c.label + " (#" + id + ")\n";
    out += "DELETE FROM npc_vendor WHERE entry=" + id + ";\nDELETE FROM npc_trainer WHERE entry=" + id + ";\n";
    for (auto const& table : relations) out += "DELETE FROM " + table + " WHERE id=" + id + creatorOnlyQuests() + ";\n";
    for (auto const& table : {"creature_equip_template", "creature_template", "npc_vendor", "npc_trainer", "creature_questrelation", "creature_involvedrelation"})
      out += replaceRows(table, c.after[table].toArray());
    out += "INSERT IGNORE INTO creator_content(kind,entry) VALUES('npc'," + id + ");\n";
  }
  for (auto const& c : ordered(EntityType::Spawn, false)) {
    out += "\n-- Placement: " + c.label + " (#" + n(c.entity) + ")\n" + replaceRows("creature", c.after["creature"].toArray());
    out += "INSERT IGNORE INTO creator_content(kind,entry) VALUES('spawn'," + n(c.entity) + ");\n";
  }
  for (auto const& c : ordered(EntityType::Quest, false)) {
    auto id = n(c.entity);
    out += "\n-- Quest: " + c.label + " (#" + id + ")\n";
    for (auto const& table : relations) out += "DELETE FROM " + table + " WHERE quest=" + id + ";\n";
    out += replaceRows("quest_template", c.after["quest_template"].toArray());
    QStringList npcs;
    for (auto const& table : relations) {
      out += replaceRows(table, c.after[table].toArray());
      for (auto const& row : c.after[table].toArray()) npcs << n(field(row.toObject(), "id"));
    }
    npcs.removeDuplicates();
    if (!npcs.isEmpty()) out += "UPDATE creature_template SET npc_flags=npc_flags|2 WHERE entry IN (" + npcs.join(',') + ");\n";
    out += "INSERT IGNORE INTO creator_content(kind,entry) VALUES('quest'," + id + ");\n";
  }
  return out;
}
}
