#include <noggit/creator/History.hpp>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <algorithm>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextStream>
#include <stdexcept>
using namespace Noggit::Creator;
namespace {
void check(bool condition, char const* message) { if (!condition) throw std::runtime_error(message); }
// A tiny stand-in for the local world database: rows per table, read back the way HistoryStore asks.
struct FakeDatabase {
  QMap<QString, QVector<Fields>> tables;
  QVector<Fields> query(QString const& sql) const {
    static QRegularExpression select("SELECT \\* FROM `(\\w+)` WHERE `(\\w+)`=(\\d+)");
    auto m = select.match(sql);
    if (!m.hasMatch()) throw std::runtime_error("Unexpected query");
    QVector<Fields> rows;
    for (auto const& row : tables.value(m.captured(1))) if (row.value(m.captured(2)).toString() == m.captured(3)) rows.push_back(row);
    return rows;
  }
  void apply(QJsonArray const& image) {
    for (auto const& v : image) {
      auto entry = v.toObject(); auto& rows = tables[entry["table"].toString()];
      auto key = entry["key"].toString(); auto id = QString::number(qulonglong(entry["id"].toDouble()));
      rows.erase(std::remove_if(rows.begin(), rows.end(), [&](Fields const& r) { return r.value(key).toString() == id; }), rows.end());
      for (auto const& r : entry["rows"].toArray()) rows.push_back(r.toObject().toVariantMap());
    }
  }
  QueryFunction reader() const { return [this](QString const& sql) { return query(sql); }; }
  HistoryApply applier() { return [this](QJsonArray const& image, QVector<HistoryEntity> const&) { apply(image); }; }
};
Fields spawn(QString x) { return {{"guid", "2000000"}, {"id", "1000000"}, {"position_x", x}}; }
QJsonArray image(QVector<Fields> const& rows) {
  QJsonArray array; for (auto const& r : rows) array.append(QJsonObject::fromVariantMap(r));
  return {QJsonObject{{"table", "creature"}, {"key", "guid"}, {"id", 2000000.0}, {"rows", array}}};
}
HistoryStep step(QString label, QVector<Fields> before, QVector<Fields> after) {
  HistoryStep s; s.label = label; s.before = image(before); s.after = image(after);
  s.entities = {{EntityType::Spawn, 2000000, "Grut"}};
  return s;
}
TrackedChange change(EntityType type, ChangeAction action, QString label) {
  TrackedChange c; c.type = type; c.action = action; c.entity = 1000000; c.label = label; return c;
}
void timeline() {
  QTemporaryDir dir; check(dir.isValid(), "No temporary directory");
  FakeDatabase db;
  HistoryStore store(dir.path() + "/history");
  check(!store.canUndo() && !store.canRedo(), "A new history has steps");

  // Placed, then moved: the database follows the steps.
  db.tables["creature"] = {spawn("5")};
  store.record(step("Placed Grut", {}, {spawn("5")}));
  db.tables["creature"] = {spawn("6")};
  store.record(step("Moved Grut", {spawn("5")}, {spawn("6")}));
  check(store.steps().size() == 2 && store.cursor() == 2, "Steps were not recorded");
  check(store.steps()[1].before.isEmpty(), "Images should be kept on disk, not in the index");

  check(store.conflict(db.reader(), true).isEmpty(), "Unchanged rows reported a conflict");
  check(store.undo(db.applier()) && db.tables["creature"] == QVector<Fields>{spawn("5")}, "Undo did not put the old position back");
  check(store.undo(db.applier()) && db.tables["creature"].isEmpty(), "Undo of a placement did not remove it");
  check(!store.undo(db.applier()), "Undo past the start succeeded");
  check(store.redo(db.applier()) && db.tables["creature"] == QVector<Fields>{spawn("5")}, "Redo did not place it again");

  // Something else changed the row since: undo warns first.
  db.tables["creature"] = {spawn("9")};
  check(!store.conflict(db.reader(), true).isEmpty(), "A later change was not reported as a conflict");
  db.tables["creature"] = {spawn("5")};

  // New work after undoing sets the undone step aside, never silently deleting it.
  store.record(step("Moved Grut", {spawn("5")}, {spawn("7")}));
  check(store.steps().size() == 2 && store.cursor() == 2 && !store.canRedo(), "Undone steps were not dropped from the timeline");
  auto discarded = QDir(dir.path() + "/history/discarded").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
  check(discarded.size() == 1 && QDir(dir.path() + "/history/discarded/" + discarded[0]).exists("steps.json"), "Undone steps were not archived");

  store.setSavepoint(0, "  Before the ambush  ");
  {
    HistoryStore reopened(dir.path() + "/history");
    check(reopened.steps().size() == 2 && reopened.cursor() == 2, "History did not survive a restart");
    check(reopened.steps()[0].savepoint == "Before the ambush", "Save point was not kept");
    check(reopened.steps()[0].entities.size() == 1 && reopened.steps()[0].entities[0].label == "Grut", "Entity label was not kept");
    check(reopened.load(1).after == image({spawn("7")}), "Step images were not kept");
  }
  {
    // Cursor survives too: undo, reopen, redo.
    store.undo(db.applier());
    HistoryStore reopened(dir.path() + "/history");
    check(reopened.cursor() == 1 && reopened.canRedo(), "Undo position did not survive a restart");
  }
}
void comparison() {
  check(HistoryStore::same(image({spawn("5"), spawn("6")}), image({spawn("6"), spawn("5")})), "Row order made images differ");
  check(!HistoryStore::same(image({spawn("5")}), image({spawn("6")})), "Different rows compared equal");
}
void labels() {
  check(HistoryStore::label({change(EntityType::Spawn, ChangeAction::Move, "Grut")}) == "Moved Grut", "Move label");
  check(HistoryStore::label({change(EntityType::Spawn, ChangeAction::Delete, "Grut")}) == "Removed Grut", "Removal label");
  check(HistoryStore::label({change(EntityType::Npc, ChangeAction::Create, "Grut")}) == "Created NPC Grut", "Creation label");
  // The thing made leads, not the placement that came with it.
  check(HistoryStore::label({change(EntityType::GameObjectSpawn, ChangeAction::Create, "Chest"), change(EntityType::GameObject, ChangeAction::Create, "Chest")})
          .startsWith("Created") , "Primary change does not lead the label");
  check(HistoryStore::label({change(EntityType::Spawn, ChangeAction::Move, "A"), change(EntityType::Spawn, ChangeAction::Move, "B")}) == "Moved A and 1 more", "Multi-change label");
  check(HistoryStore::label({}) == "Local change", "Empty label");
}
void corrupt() {
  QTemporaryDir dir; QDir().mkpath(dir.path() + "/history");
  QFile index(dir.path() + "/history/index.json"); index.open(QIODevice::WriteOnly); index.write("{not json"); index.close();
  HistoryStore store(dir.path() + "/history");
  check(!store.warning().isEmpty() && store.steps().isEmpty(), "An unreadable history was not set aside");
  check(QDir(dir.path() + "/history").entryList({"index.json.invalid-*"}).size() == 1, "The unreadable index was not kept for inspection");
}
}
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try { timeline(); comparison(); labels(); corrupt(); }
  catch (std::exception const& e) { QTextStream(stderr) << "FAIL: " << e.what() << '\n'; return 1; }
  QTextStream(stdout) << "History tests passed\n";
  return 0;
}
