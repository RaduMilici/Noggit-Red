#include "History.hpp"
#include <noggit/runtime/RuntimeManager.hpp>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
void fail(QString const& s) { throw std::runtime_error(s.toStdString()); }
QString identifier(QString const& s) {
  if (s.isEmpty()) fail("Invalid history entry.");
  for (auto c : s) if (!c.isLetterOrNumber() && c != '_') fail("Invalid history entry.");
  return '`' + s + '`';
}
QString selection(QJsonObject const& entry) {
  if (entry.contains("where")) return "SELECT * FROM " + identifier(entry["table"].toString()) + " WHERE " + entry["where"].toString();
  return "SELECT * FROM " + identifier(entry["table"].toString()) + " WHERE " + identifier(entry["key"].toString()) + "="
       + QString::number(qulonglong(entry["id"].toDouble()));
}
// Rows as an order-independent list, so a re-read matches regardless of the server's row order.
QStringList canonical(QJsonArray const& rows) {
  QStringList result;
  for (auto const& row : rows) result << QString::fromUtf8(QJsonDocument(row.toObject()).toJson(QJsonDocument::Compact));
  result.sort(); return result;
}
QJsonObject readJson(QString const& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) fail("Cannot read " + path);
  QJsonParseError error; auto doc = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject()) fail("Invalid " + path);
  return doc.object();
}
void writeJson(QString const& path, QJsonObject const& object) {
  QSaveFile file(path); auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) fail("Cannot write " + path);
}
QString verb(ChangeAction action) {
  switch (action) {
    case ChangeAction::Create: return "Created";
    case ChangeAction::Delete: return "Deleted";
    case ChangeAction::Move: return "Moved";
    default: return "Edited";
  }
}
QString describe(TrackedChange const& c) {
  auto name = c.label.isEmpty() ? "#" + QString::number(c.entity) : c.label;
  if (c.type == EntityType::Spawn || c.type == EntityType::GameObjectSpawn) {
    if (c.action == ChangeAction::Create) return "Placed " + name;
    if (c.action == ChangeAction::Delete) return "Removed " + name;
    if (c.action == ChangeAction::Move) return "Moved " + name;
  }
  // summary() is "<sign> <Kind>: <name>[ · detail]"; keep its wording, swap the sign for a verb.
  auto text = c.summary();
  if (text.size() > 2 && (text[0] == '+' || text[0] == '-' || text[0] == '~') && text[1] == ' ') text = text.mid(2);
  return verb(c.action) + " " + text.replace(": ", " ");
}
}
HistoryStore* HistoryStore::instance() {
  auto runtime = Runtime::RuntimeManager::instance();
  if (!runtime) return nullptr;
  if (auto store = runtime->findChild<HistoryStore*>("creatorHistory", Qt::FindDirectChildrenOnly)) return store;
  auto store = new HistoryStore(runtime->root() + "/Workspace/creator-history", runtime);
  store->setObjectName("creatorHistory");
  return store;
}
HistoryStore::HistoryStore(QString const& folder, QObject* parent) : QObject(parent), _folder(folder) {
  try { load(); }
  catch (std::exception const& e) {
    // Keep the unreadable index for inspection and start a fresh timeline.
    _warning = QString("History could not be read and was set aside: %1").arg(e.what());
    QFile::rename(_folder + "/index.json", _folder + "/index.json.invalid-" + QDateTime::currentDateTimeUtc().toString("yyyyMMddHHmmss"));
    _steps.clear(); _cursor = 0;
  }
}
QString HistoryStore::stepPath(int id) const { return _folder + QString("/step-%1.json").arg(id); }
void HistoryStore::load() {
  if (!QFile::exists(_folder + "/index.json")) return;
  auto index = readJson(_folder + "/index.json");
  for (auto const& v : index["steps"].toArray()) {
    auto o = v.toObject(); HistoryStep s;
    s.id = o["id"].toInt(); s.label = o["label"].toString(); s.savepoint = o["savepoint"].toString();
    s.timestamp = QDateTime::fromString(o["timestamp"].toString(), Qt::ISODate);
    for (auto const& e : o["entities"].toArray()) s.entities.push_back({EntityType(e.toArray()[0].toInt()), Id(e.toArray()[1].toDouble()), e.toArray()[2].toString()});
    _steps.push_back(s);
  }
  _cursor = std::clamp(index["cursor"].toInt(), 0, int(_steps.size()));
  _next = std::max(index["next"].toInt(1), 1);
  for (auto const& s : _steps) _next = std::max(_next, s.id + 1);
}
void HistoryStore::writeIndex() const {
  QJsonArray steps;
  for (auto const& s : _steps) {
    QJsonArray entities;
    for (auto const& e : s.entities) entities.append(QJsonArray{int(e.type), double(e.id), e.label});
    QJsonObject o{{"id", s.id}, {"label", s.label}, {"timestamp", s.timestamp.toString(Qt::ISODate)}, {"entities", entities}};
    if (!s.savepoint.isEmpty()) o["savepoint"] = s.savepoint;
    steps.append(o);
  }
  writeJson(_folder + "/index.json", {{"cursor", _cursor}, {"next", _next}, {"steps", steps}});
}
void HistoryStore::writeStep(HistoryStep const& step) const { writeJson(stepPath(step.id), {{"before", step.before}, {"after", step.after}}); }
HistoryStep HistoryStore::load(int index) const {
  if (index < 0 || index >= _steps.size()) fail("No such history step.");
  auto step = _steps[index]; auto images = readJson(stepPath(step.id));
  step.before = images["before"].toArray(); step.after = images["after"].toArray();
  return step;
}
void HistoryStore::record(HistoryStep step) {
  if (!QDir().mkpath(_folder)) fail("Cannot create the history folder.");
  if (_cursor < _steps.size()) {
    // The undone steps are no longer reachable from the timeline: keep their files for safety.
    auto archive = _folder + "/discarded/" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz");
    QDir().mkpath(archive);
    QJsonArray labels;
    for (int i = _cursor; i < _steps.size(); ++i) {
      QFile::rename(stepPath(_steps[i].id), archive + QString("/step-%1.json").arg(_steps[i].id));
      labels.append(_steps[i].label);
    }
    writeJson(archive + "/steps.json", {{"labels", labels}});
    _steps.resize(_cursor);
  }
  step.id = _next++;
  if (!step.timestamp.isValid()) step.timestamp = QDateTime::currentDateTimeUtc();
  writeStep(step);
  step.before = {}; step.after = {};
  _steps.push_back(step); _cursor = _steps.size();
  writeIndex();
  emit recorded();
  emit changed();
}
QJsonArray HistoryStore::read(QueryFunction const& query, QJsonArray const& image) {
  QJsonArray result;
  for (auto const& v : image) {
    auto entry = v.toObject(); QJsonArray rows;
    for (auto const& row : query(selection(entry))) rows.append(QJsonObject::fromVariantMap(row));
    entry["rows"] = rows; result.append(entry);
  }
  return result;
}
bool HistoryStore::same(QJsonArray const& a, QJsonArray const& b) {
  if (a.size() != b.size()) return false;
  for (int i = 0; i < a.size(); ++i)
    if (canonical(a[i].toObject()["rows"].toArray()) != canonical(b[i].toObject()["rows"].toArray())) return false;
  return true;
}
QString HistoryStore::conflict(QueryFunction const& current, bool undo) const {
  int index = undo ? _cursor - 1 : _cursor;
  if (index < 0 || index >= _steps.size()) return {};
  auto step = load(index);
  // Undo expects the rows as the step left them; redo expects them as they were before it.
  auto const& expected = undo ? step.after : step.before;
  auto now = read(current, expected);
  for (int i = 0; i < expected.size(); ++i)
    if (canonical(expected[i].toObject()["rows"].toArray()) != canonical(now[i].toObject()["rows"].toArray()))
      return QString("\"%1\" was changed again afterwards (%2).").arg(step.label, expected[i].toObject()["table"].toString());
  return {};
}
bool HistoryStore::undo(HistoryApply const& apply) {
  if (!canUndo()) return false;
  auto step = load(_cursor - 1);
  apply(step.before, step.entities);
  --_cursor; writeIndex(); emit changed();
  return true;
}
bool HistoryStore::redo(HistoryApply const& apply) {
  if (!canRedo()) return false;
  auto step = load(_cursor);
  apply(step.after, step.entities);
  ++_cursor; writeIndex(); emit changed();
  return true;
}
void HistoryStore::setSavepoint(int index, QString const& name) {
  if (index < 0 || index >= _steps.size()) fail("No such history step.");
  _steps[index].savepoint = name.trimmed();
  writeIndex(); emit changed();
}
QString HistoryStore::label(QVector<TrackedChange> const& changes) {
  if (changes.isEmpty()) return "Local change";
  // Lead with what the designer made or edited rather than the placements it brought along.
  auto primary = std::find_if(changes.begin(), changes.end(), [](TrackedChange const& c) {
    return c.type != EntityType::Spawn && c.type != EntityType::GameObjectSpawn; });
  auto const& first = primary != changes.end() ? *primary : changes.front();
  auto text = describe(first);
  if (changes.size() > 1) text += QString(" and %1 more").arg(changes.size() - 1);
  return text;
}
}
