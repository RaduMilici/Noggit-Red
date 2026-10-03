#include <noggit/creator/ChangeTracker.hpp>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTextStream>
#include <stdexcept>
using namespace Noggit::Creator;
namespace {
void check(bool condition, char const* message) { if (!condition) throw std::runtime_error(message); }
QJsonObject rows(QString const& table, QJsonObject row) { return {{table, QJsonArray{row}}}; }
QJsonObject spawn(QString x, QString respawn = "120") {
  return rows("creature", {{"guid", "2000000"}, {"id", "1000000"}, {"position_x", x}, {"position_y", "1"}, {"position_z", "2"}, {"orientation", "0"}, {"spawntimesecsmin", respawn}});
}
QJsonObject npc(QString name) { return rows("creature_template", {{"entry", "1000000"}, {"name", name}, {"equipment_id", "0"}}); }
TrackedChange change(EntityType type, Id id, QJsonObject before, QJsonObject after, QString label = "Restless Miller") {
  TrackedChange c; c.type = type; c.entity = id; c.before = before; c.after = after; c.label = label; return c;
}
void merging() {
  QVector<TrackedChange> list;
  ChangeTracker::merge(list, change(EntityType::Npc, 1000000, {}, npc("Restless Miller")));
  check(list.size() == 1 && list[0].action == ChangeAction::Create && !list[0].id.isEmpty(), "Creation was not tracked");
  auto id = list[0].id;
  ChangeTracker::merge(list, change(EntityType::Npc, 1000000, npc("Restless Miller"), npc("Restless Miller II")));
  check(list.size() == 1 && list[0].action == ChangeAction::Create && list[0].id == id && list[0].after == npc("Restless Miller II"), "Edit after creation did not stay a creation");
  ChangeTracker::merge(list, change(EntityType::Npc, 1000000, npc("Restless Miller II"), {}));
  check(list.isEmpty(), "Creating then deleting should leave no change");

  ChangeTracker::merge(list, change(EntityType::Spawn, 2000000, spawn("5"), spawn("6")));
  check(list.size() == 1 && list[0].action == ChangeAction::Move, "Position-only edit is not a move");
  ChangeTracker::merge(list, change(EntityType::Spawn, 2000000, spawn("6"), spawn("6", "300")));
  check(list[0].action == ChangeAction::Update && list[0].before == spawn("5"), "Respawn edit did not become an update with the original before-state");
  ChangeTracker::merge(list, change(EntityType::Spawn, 2000000, spawn("6", "300"), spawn("5")));
  check(list.isEmpty(), "Reverting to the original state should leave no change");
  ChangeTracker::merge(list, change(EntityType::Spawn, 2000000, spawn("5"), spawn("5")));
  check(list.isEmpty(), "Unchanged save was tracked");

  ChangeTracker::merge(list, change(EntityType::Quest, 1000001, rows("quest_template", {{"entry", "1000001"}, {"Title", "A"}}), rows("quest_template", {{"entry", "1000001"}, {"Title", "B"}})));
  ChangeTracker::merge(list, change(EntityType::Quest, 1000001, rows("quest_template", {{"entry", "1000001"}, {"Title", "B"}}), {}, QString()));
  check(list.size() == 1 && list[0].action == ChangeAction::Delete && list[0].before["quest_template"].toArray()[0].toObject()["Title"] == "A" && list[0].label == "Restless Miller",
        "Update then delete did not keep the original before-state and label");
}
void persistence() {
  QTemporaryDir dir; check(dir.isValid(), "No temporary directory");
  auto journal = dir.path() + "/creator-recovery.json";
  {
    ChangeTracker tracker(dir.path());
    tracker.stage({change(EntityType::Npc, 1000000, {}, npc("Restless Miller"))});
    tracker.promote();
    check(tracker.changes().size() == 1, "Promoted change missing");
  }
  { ChangeTracker tracker(dir.path()); check(tracker.changes().size() == 1 && tracker.changes()[0].summary() == "+ NPC: Restless Miller", "Change did not survive a restart"); }
  // Crash before the database commit: the journal still exists, so the staged list is dropped.
  {
    ChangeTracker tracker(dir.path());
    tracker.stage({change(EntityType::Quest, 1000001, {}, rows("quest_template", {{"entry", "1000001"}, {"Title", "Beneath the Mill"}}))});
    QFile(journal).open(QIODevice::WriteOnly);
  }
  { ChangeTracker tracker(dir.path()); check(tracker.changes().size() == 1, "Rolled-back save was listed"); }
  // Crash after the database commit: no journal, so the staged list is published.
  QFile::remove(journal);
  { ChangeTracker tracker(dir.path()); tracker.stage({change(EntityType::Quest, 1000001, {}, rows("quest_template", {{"entry", "1000001"}, {"Title", "Beneath the Mill"}}))}); }
  {
    ChangeTracker tracker(dir.path());
    check(tracker.changes().size() == 2, "Committed save was lost");
    tracker.clear({tracker.changes()[0].id});
    check(tracker.changes().size() == 1 && tracker.changes()[0].type == EntityType::Quest, "Clear removed the wrong entry");
  }
  { ChangeTracker tracker(dir.path()); check(tracker.changes().size() == 1, "Clear was not persisted"); }
  QFile corrupt(dir.path() + "/creator-changes.json"); corrupt.open(QIODevice::WriteOnly); corrupt.write("{"); corrupt.close();
  { ChangeTracker tracker(dir.path()); check(tracker.changes().isEmpty() && !tracker.warning().isEmpty() && !QFile::exists(dir.path() + "/creator-changes.json"), "Corrupt list was not set aside"); }
}
void sql() {
  auto created = change(EntityType::Npc, 1000000, {}, npc("O'Brien \\ Miller"), "O'Brien");
  created.action = ChangeAction::Create;
  auto quest = change(EntityType::Quest, 1000001, {}, QJsonObject{
    {"quest_template", QJsonArray{QJsonObject{{"entry", "1000001"}, {"Title", "Beneath the Mill"}, {"Details", "Line 1\nLine 2"}, {"RequiredRaces", QJsonValue::Null}}}},
    {"creature_questrelation", QJsonArray{QJsonObject{{"id", "1000000"}, {"quest", "1000001"}}}}}, "Beneath the Mill");
  quest.action = ChangeAction::Create;
  auto removed = change(EntityType::Spawn, 2000000, spawn("5"), {}); removed.action = ChangeAction::Delete;
  auto text = ChangeTracker::sql({quest, created, removed});
  check(text.contains("REPLACE INTO `creature_template` (`entry`,`equipment_id`,`name`) VALUES ('1000000','0','O\\'Brien \\\\ Miller');"), "NPC row is not escaped correctly");
  check(text.contains("'Line 1\\nLine 2'") && text.contains("NULL"), "Newline or NULL not encoded");
  check(text.contains("DELETE FROM creature WHERE guid=2000000 AND @owned>0;"), "Placement deletion is not guarded");
  check(text.contains("UPDATE creature_template SET npc_flags=npc_flags|2 WHERE entry IN (1000000);"), "Quest giver flag missing");
  check(text.indexOf("-- NPC:") < text.indexOf("-- Quest:") && text.indexOf("-- Remove placement") < text.indexOf("-- NPC:"), "Statements are out of dependency order");
  check(!text.contains("creature_questrelation WHERE id=1000000;"), "NPC export touches Creator quest relations");
}
}
// Quests carry their scripts, spoken lines, drops and start item; what an edit removes is removed on import.
void questContentSql() {
  auto row = [](std::initializer_list<std::pair<QString, QString>> fields) {
    QJsonObject o; for (auto const& [k, v] : fields) o[k] = v; return o;
  };
  QJsonObject before{
    {"quest_template", QJsonArray{row({{"entry", "1000001"}, {"Title", "Beneath the Mill"}, {"StartScript", "1000001"}, {"ReqItemId1", "1000005"}})}},
    {"quest_start_scripts", QJsonArray{row({{"id", "1000001"}, {"command", "0"}, {"dataint", "6000000"}})}},
    {"broadcast_text", QJsonArray{row({{"entry", "6000000"}, {"male_text", "Old line"}})}},
    {"creature_loot_template", QJsonArray{row({{"entry", "300"}, {"item", "1000005"}, {"ChanceOrQuestChance", "-50"}})}},
    {"item_start_link", QJsonArray{row({{"entry", "1000006"}})}}};
  QJsonObject after{
    {"quest_template", QJsonArray{row({{"entry", "1000001"}, {"Title", "Beneath the Mill"}, {"StartScript", "1000001"}, {"ReqItemId1", "1000005"}})}},
    {"quest_start_scripts", QJsonArray{row({{"id", "1000001"}, {"command", "0"}, {"dataint", "6000001"}})}},
    {"broadcast_text", QJsonArray{row({{"entry", "6000001"}, {"male_text", "New line"}})}},
    {"gameobject_loot_template", QJsonArray{row({{"entry", "400"}, {"item", "1000005"}, {"ChanceOrQuestChance", "-25"}})}},
    {"gameobject_loot_link", QJsonArray{row({{"entry", "400"}})}},
    {"gameobject_questrelation", QJsonArray{row({{"id", "500"}, {"quest", "1000001"}})}}};
  auto edited = change(EntityType::Quest, 1000001, before, after, "Beneath the Mill"); edited.action = ChangeAction::Update;
  auto text = ChangeTracker::sql({edited});
  check(text.contains("DELETE FROM quest_start_scripts WHERE id=1000001;") && text.contains("'New line'"), "Scripts are not replaced");
  check(text.contains("DELETE FROM broadcast_text WHERE entry IN (6000000);"), "Removed spoken line is kept");
  check(text.contains("DELETE FROM creature_loot_template WHERE entry=300 AND item=1000005 AND ChanceOrQuestChance<0 AND NOT EXISTS"), "Removed drop is kept");
  check(text.contains("UPDATE gameobject_template SET data1=400 WHERE entry=400 AND type=3 AND data1=0;"), "Chest loot table is not given");
  check(text.contains("UPDATE item_template SET start_quest=0 WHERE entry=1000006 AND start_quest=1000001;"), "Old start item still starts the quest");
  check(text.contains("REPLACE INTO `gameobject_questrelation`") && !text.contains("WHERE entry IN (500)"), "Object giver missing or flagged as an NPC");

  auto removed = change(EntityType::Quest, 1000001, after, {}); removed.action = ChangeAction::Delete;
  text = ChangeTracker::sql({removed});
  check(text.contains("DELETE FROM quest_start_scripts WHERE id=1000001 AND @owned>0;") && text.contains("DELETE FROM broadcast_text WHERE entry IN (6000001) AND @owned>0;"), "Deleted quest leaves its scripts");
  check(text.contains("DELETE FROM gameobject_loot_template WHERE entry=400 AND item=1000005 AND ChanceOrQuestChance<0 AND @owned>0 AND NOT EXISTS"), "Deleted quest leaves its drops");

  auto item = change(EntityType::Item, 1000005, {}, rows("item_template", {{"entry", "1000005"}, {"name", "Wolf Pelt"}}), "Wolf Pelt");
  item.action = ChangeAction::Create;
  text = ChangeTracker::sql({edited, item});
  check(item.summary() == "+ Item: Wolf Pelt" && text.indexOf("-- Item:") < text.indexOf("-- Quest:") && text.contains("VALUES('item',1000005)"), "Item is not exported before its quest");
}
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try { merging(); persistence(); sql(); questContentSql(); }
  catch (std::exception const& e) { QTextStream(stderr) << e.what() << Qt::endl; return 1; }
  QTextStream(stdout) << "Change tracker checks passed\n";
  return 0;
}
