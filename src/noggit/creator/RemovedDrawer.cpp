#include "RemovedDrawer.hpp"
#include "Database.hpp"
#include "Timeline.hpp"
#include <noggit/Action.hpp>
#include <noggit/runtime/RuntimeManager.hpp>
#include <noggit/ui/FontAwesome.hpp>
#include <QComboBox>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
using Icon = Ui::FontAwesome::Icons;
constexpr int keep = 500; // the drawer keeps this many removed things
void fail(QString const& s) { throw std::runtime_error(s.toStdString()); }
QString table(RemovedThing::Kind kind) { return kind == RemovedThing::Kind::Npc ? "creature" : "gameobject"; }
QJsonObject entry(QJsonArray const& image, QString const& name) {
  for (auto const& v : image) if (v.toObject()["table"].toString() == name) return v.toObject();
  return {};
}
QJsonObject keyed(QJsonArray const& image, QString const& name, Id id) {
  for (auto const& v : image) {
    auto o = v.toObject();
    if (o["table"].toString() == name && !o.contains("where") && Id(o["id"].toDouble()) == id) return o;
  }
  return {};
}
Id number(QJsonValue const& v) { return Id(v.toVariant().toULongLong()); }
QJsonValue renumbered(QJsonValue const& v, Id id) { return v.isString() ? QJsonValue(QString::number(id)) : QJsonValue(double(id)); }
QString ownership(QString const& kind, Id guid) { return QString("kind='%1' AND entry=%2").arg(kind).arg(guid); }
// The placement's rows moved to another guid (its own was taken by a newer placement).
QJsonArray renumber(QJsonArray const& image, Id from, Id to) {
  QJsonArray result;
  for (auto const& v : image) {
    auto o = v.toObject(); auto name = o["table"].toString();
    QString field = name == "creature_movement" ? "id" : name == "creator_content" ? "entry" : "guid";
    if (o.contains("where")) o["where"] = o["where"].toString().replace(QString("entry=%1").arg(from), QString("entry=%1").arg(to));
    else if (Id(o["id"].toDouble()) == from) o["id"] = double(to);
    QJsonArray rows;
    for (auto const& r : o["rows"].toArray()) { auto row = r.toObject(); if (number(row[field]) == from) row[field] = renumbered(row[field], to); rows.append(row); }
    o["rows"] = rows; result.append(o);
  }
  return result;
}
QJsonObject toJson(RemovedThing const& t) {
  return {{"id", t.id}, {"kind", int(t.kind)}, {"label", t.label}, {"time", t.time.toString(Qt::ISODate)}, {"map", double(t.map)},
          {"pos", QJsonArray{t.x, t.y, t.z}}, {"file", t.file}, {"fileDataId", double(t.fileDataId)}, {"wmo", t.wmo},
          {"dir", QJsonArray{t.rx, t.ry, t.rz}}, {"scale", t.scale}, {"guid", double(t.guid)}, {"image", t.image}};
}
RemovedThing fromJson(QJsonObject const& o) {
  RemovedThing t;
  t.id = o["id"].toString(); t.kind = RemovedThing::Kind(o["kind"].toInt()); t.label = o["label"].toString();
  t.time = QDateTime::fromString(o["time"].toString(), Qt::ISODate); t.map = unsigned(o["map"].toDouble());
  auto pos = o["pos"].toArray(); t.x = pos[0].toDouble(); t.y = pos[1].toDouble(); t.z = pos[2].toDouble();
  t.file = o["file"].toString(); t.fileDataId = quint32(o["fileDataId"].toDouble()); t.wmo = o["wmo"].toBool();
  auto dir = o["dir"].toArray(); t.rx = dir[0].toDouble(); t.ry = dir[1].toDouble(); t.rz = dir[2].toDouble();
  t.scale = o["scale"].toDouble(1); t.guid = Id(o["guid"].toDouble()); t.image = o["image"].toArray();
  return t;
}
bool samePlace(RemovedThing const& a, RemovedThing const& b) {
  return a.kind == b.kind && a.map == b.map && a.file == b.file && a.fileDataId == b.fileDataId
      && std::abs(a.x - b.x) < 0.01 && std::abs(a.y - b.y) < 0.01 && std::abs(a.z - b.z) < 0.01;
}
}
QString relativeTime(QDateTime const& time) {
  auto seconds = time.secsTo(QDateTime::currentDateTime());
  if (seconds < 45) return "just now";
  if (seconds < 3600) return QString("%1 min ago").arg(std::max<qint64>(1, seconds / 60));
  auto local = time.toLocalTime();
  if (local.date() == QDate::currentDate()) return local.toString("HH:mm");
  if (local.date() == QDate::currentDate().addDays(-1)) return "yesterday " + local.toString("HH:mm");
  return local.toString("d MMM HH:mm");
}
RemovedStore* RemovedStore::instance() {
  auto runtime = Runtime::RuntimeManager::instance();
  if (!runtime) return nullptr;
  if (auto store = runtime->findChild<RemovedStore*>("creatorRemoved", Qt::FindDirectChildrenOnly)) return store;
  auto store = new RemovedStore(runtime->root() + "/Workspace/creator-removed.json", runtime);
  store->setObjectName("creatorRemoved");
  // Placements a save deleted (the History step holds their rows).
  if (auto history = HistoryStore::instance())
    connect(history, &HistoryStore::recorded, store, [store, history] {
      try { store->placements(history->load(history->steps().size() - 1)); } catch (std::exception const&) {}
    });
  return store;
}
RemovedStore::RemovedStore(QString const& file, QObject* parent) : QObject(parent), _file(file) {
  QFile in(_file);
  if (!in.open(QIODevice::ReadOnly)) return;
  auto doc = QJsonDocument::fromJson(in.readAll());
  for (auto const& v : doc.array()) _things.push_back(fromJson(v.toObject()));
}
void RemovedStore::save() const {
  QJsonArray all; for (auto const& t : _things) all.append(toJson(t));
  QSaveFile out(_file); auto bytes = QJsonDocument(all).toJson(QJsonDocument::Compact);
  if (out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size()) out.commit();
}
void RemovedStore::add(RemovedThing thing) {
  for (auto const& t : _things) if (samePlace(t, thing) && thing.kind == RemovedThing::Kind::Scenery) return;
  thing.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (!thing.time.isValid()) thing.time = QDateTime::currentDateTime();
  _things.prepend(thing);
  if (_things.size() > keep) _things.resize(keep);
}
void RemovedStore::scenery(Action const* action, unsigned map, bool removed) {
  for (auto const& [uid, object] : action->removedObjects()) {
    RemovedThing t; t.kind = RemovedThing::Kind::Scenery; t.map = map;
    t.file = object.file_key.hasFilepath() ? QString::fromStdString(object.file_key.filepath()) : QString();
    t.fileDataId = object.file_key.fileDataID(); t.wmo = object.type == ActionObjectTypes::WMO;
    t.label = Timeline::objectName(t.file, t.wmo);
    t.x = object.pos.x; t.y = object.pos.y; t.z = object.pos.z;
    t.rx = object.dir.x; t.ry = object.dir.y; t.rz = object.dir.z; t.scale = object.scale;
    if (removed) add(t);
    else for (int i = _things.size() - 1; i >= 0; --i) if (samePlace(_things[i], t)) _things.remove(i);
  }
  save(); emit changed();
}
void RemovedStore::placements(HistoryStep const& step) {
  bool any = false;
  for (auto const& e : step.entities) {
    if (e.type != EntityType::Spawn && e.type != EntityType::GameObjectSpawn) continue;
    auto kind = e.type == EntityType::Spawn ? RemovedThing::Kind::Npc : RemovedThing::Kind::Object;
    auto before = keyed(step.before, table(kind), e.id), after = keyed(step.after, table(kind), e.id);
    if (before["rows"].toArray().isEmpty() || !after["rows"].toArray().isEmpty()) continue;
    RemovedThing t; t.kind = kind; t.guid = e.id; t.time = step.timestamp.toLocalTime();
    t.label = e.label.isEmpty() ? (kind == RemovedThing::Kind::Npc ? "NPC" : "Object") : e.label;
    auto row = before["rows"].toArray()[0].toObject();
    t.map = unsigned(number(row["map"]));
    t.x = row["position_x"].toVariant().toDouble(); t.y = row["position_y"].toVariant().toDouble(); t.z = row["position_z"].toVariant().toDouble();
    t.image.append(before);
    if (kind == RemovedThing::Kind::Npc) if (auto path = keyed(step.before, "creature_movement", e.id); !path.isEmpty()) t.image.append(path);
    // Only this placement's ownership row: creator_content also holds NPCs and objects under the same number.
    auto kindName = kind == RemovedThing::Kind::Npc ? QString("spawn") : QString("object_spawn");
    QJsonArray owned;
    for (auto const& r : keyed(step.before, "creator_content", e.id)["rows"].toArray())
      if (r.toObject()["kind"].toString() == kindName) owned.append(r);
    t.image.append(QJsonObject{{"table", "creator_content"}, {"where", ownership(kindName, e.id)}, {"rows", owned}});
    add(t); any = true;
  }
  if (any) { save(); emit changed(); }
}
void RemovedStore::forget(QString const& id) {
  for (int i = 0; i < _things.size(); ++i) if (_things[i].id == id) { _things.remove(i); save(); emit changed(); return; }
}
void RemovedStore::prune(QueryFunction const& query) {
  bool any = false;
  for (int i = _things.size() - 1; i >= 0; --i) {
    auto const& t = _things[i];
    if (t.kind == RemovedThing::Kind::Scenery) continue;
    auto rows = entry(t.image, table(t.kind))["rows"].toArray();
    if (rows.isEmpty()) continue;
    auto now = query(QString("SELECT id FROM %1 WHERE guid=%2").arg(table(t.kind)).arg(t.guid));
    if (!now.isEmpty() && Id(now[0]["id"].toULongLong()) == number(rows[0].toObject()["id"])) { _things.remove(i); any = true; }
  }
  if (any) { save(); emit changed(); }
}
void RemovedStore::bringBack(RemovedThing const& thing) {
  auto name = table(thing.kind);
  auto rows = entry(thing.image, name)["rows"].toArray();
  if (rows.isEmpty()) fail("Nothing to bring back.");
  Database db;
  auto templateTable = thing.kind == RemovedThing::Kind::Npc ? "creature_template" : "gameobject_template";
  if (db.query(QString("SELECT entry FROM %1 WHERE entry=%2").arg(templateTable).arg(number(rows[0].toObject()["id"]))).isEmpty())
    fail(QString("%1 itself was deleted. Undo that deletion in History to get it back.").arg(thing.label));
  auto image = thing.image; Id guid = thing.guid;
  if (!db.query(QString("SELECT guid FROM %1 WHERE guid=%2").arg(name).arg(guid)).isEmpty()) {
    guid = db.allocate(name, "guid", 0xfffffffe);
    image = renumber(image, thing.guid, guid);
  }
  auto type = thing.kind == RemovedThing::Kind::Npc ? EntityType::Spawn : EntityType::GameObjectSpawn;
  db.replay(image, {{type, guid, thing.label}}, "Brought back " + thing.label);
}

RemovedDrawer::RemovedDrawer(Actions actions, QWidget* parent) : QWidget(parent), _actions(std::move(actions)) {
  auto layout = new QVBoxLayout(this); layout->setContentsMargins(6, 6, 6, 6); layout->setSpacing(4);
  auto top = new QHBoxLayout;
  _search = new QLineEdit(this); _search->setPlaceholderText("Search removed things…"); _search->setClearButtonEnabled(true);
  _filter = new QComboBox(this); _filter->addItems({"Everything", "Scenery", "NPCs", "Objects"});
  top->addWidget(_search, 1); top->addWidget(_filter);
  layout->addLayout(top);
  _list = new QListWidget(this); _list->setAlternatingRowColors(true);
  layout->addWidget(_list, 1);
  auto buttons = new QHBoxLayout;
  _bring = new QPushButton(Ui::FontAwesomeIcon(Icon::undo), "Bring back", this);
  _bring->setToolTip("Put it back exactly where it was. You can undo this too.");
  _show = new QPushButton(Ui::FontAwesomeIcon(Icon::eye), "Show me", this);
  _show->setToolTip("Fly the camera to where it was.");
  _forget = new QPushButton(Ui::FontAwesomeIcon(Icon::times), "Forget", this);
  _forget->setToolTip("Remove it from this list. It stays removed from the world.");
  buttons->addWidget(_bring); buttons->addWidget(_show); buttons->addStretch(1); buttons->addWidget(_forget);
  layout->addLayout(buttons);
  connect(_search, &QLineEdit::textChanged, this, &RemovedDrawer::refresh);
  connect(_filter, qOverload<int>(&QComboBox::currentIndexChanged), this, &RemovedDrawer::refresh);
  connect(_list, &QListWidget::currentRowChanged, this, &RemovedDrawer::updateButtons);
  connect(_list, &QListWidget::itemDoubleClicked, this, [this] { if (auto t = selected(); t && _actions.show) _actions.show(*t); });
  connect(_bring, &QPushButton::clicked, this, &RemovedDrawer::bringBack);
  connect(_show, &QPushButton::clicked, this, [this] { if (auto t = selected(); t && _actions.show) _actions.show(*t); });
  connect(_forget, &QPushButton::clicked, this, [this] { if (auto t = selected()) RemovedStore::instance()->forget(t->id); });
  if (auto store = RemovedStore::instance()) connect(store, &RemovedStore::changed, this, &RemovedDrawer::refresh);
  refresh();
}
void RemovedDrawer::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  // Placements an undo brought back no longer belong here.
  if (auto store = RemovedStore::instance()) {
    try { Database db; store->prune([&db](QString const& sql) { return db.query(sql); }); } catch (std::exception const&) {}
  }
  refresh();
}
RemovedThing const* RemovedDrawer::selected() const {
  auto item = _list->currentItem(); auto store = RemovedStore::instance();
  if (!item || !store) return nullptr;
  auto id = item->data(Qt::UserRole).toString();
  for (auto const& t : store->things()) if (t.id == id) return &t;
  return nullptr;
}
void RemovedDrawer::refresh() {
  auto store = RemovedStore::instance();
  auto current = _list->currentItem() ? _list->currentItem()->data(Qt::UserRole).toString() : QString();
  _list->clear();
  if (!store) return;
  auto map = _actions.currentMap ? _actions.currentMap() : 0u;
  for (auto const& t : store->things()) {
    int filter = _filter->currentIndex();
    if (filter == 1 && t.kind != RemovedThing::Kind::Scenery) continue;
    if (filter == 2 && t.kind != RemovedThing::Kind::Npc) continue;
    if (filter == 3 && t.kind != RemovedThing::Kind::Object) continue;
    if (!_search->text().isEmpty() && !t.label.contains(_search->text(), Qt::CaseInsensitive)) continue;
    auto icon = t.kind == RemovedThing::Kind::Npc ? Icon::user : t.kind == RemovedThing::Kind::Object ? Icon::cube : t.wmo ? Icon::map : Icon::tree;
    auto text = QString("%1    ·    removed %2").arg(t.label, relativeTime(t.time));
    if (t.map != map) text += "    ·    other map";
    auto item = new QListWidgetItem(Ui::FontAwesomeIcon(icon), text, _list);
    item->setData(Qt::UserRole, t.id);
    if (t.id == current) _list->setCurrentItem(item);
  }
  if (!_list->count()) {
    auto empty = new QListWidgetItem("Nothing removed. Anything you delete shows up here, so you can always bring it back.", _list);
    empty->setFlags(Qt::NoItemFlags);
  }
  updateButtons();
}
void RemovedDrawer::updateButtons() {
  auto t = selected();
  bool here = t && (!_actions.currentMap || t->map == _actions.currentMap());
  _bring->setEnabled(here); _show->setEnabled(here); _forget->setEnabled(t);
  _bring->setToolTip(t && !here ? "Open the map it was on to bring it back." : "Put it back exactly where it was. You can undo this too.");
}
void RemovedDrawer::bringBack() {
  auto t = selected(); if (!t) return;
  auto thing = *t; // the store changes below
  try {
    if (thing.kind == RemovedThing::Kind::Scenery) { if (_actions.restoreScenery) _actions.restoreScenery(thing); }
    else { RemovedStore::bringBack(thing); if (_actions.afterDatabase) _actions.afterDatabase(); }
    RemovedStore::instance()->forget(thing.id);
  } catch (std::exception const& e) { QMessageBox::warning(this, "Bring back", e.what()); }
}
}
