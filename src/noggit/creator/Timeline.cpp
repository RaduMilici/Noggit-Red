#include "Timeline.hpp"
#include "Database.hpp"
#include "History.hpp"
#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <QFileInfo>
#include <QMessageBox>
#include <QRegularExpression>
#include <algorithm>
namespace Noggit::Creator {
namespace {
QString objectName(ObjectInstanceCache const& object) {
  auto const& key = object.file_key;
  return Timeline::objectName(key.hasFilepath() ? QString::fromStdString(key.filepath()) : QString(), object.type == ActionObjectTypes::WMO);
}
template <typename Objects>
QString objects(Objects const& list) {
  if (list.size() == 1) return objectName(list.front().second);
  return QString("%1 objects").arg(list.size());
}
}
Timeline::Timeline(QWidget* dialogParent, std::function<void()> afterDatabase, QObject* parent)
  : QObject(parent), _parent(dialogParent), _afterDatabase(std::move(afterDatabase)) {
  seed();
  if (auto store = HistoryStore::instance()) {
    connect(store, &HistoryStore::recorded, this, [this, store] {
      auto const& step = store->steps().back();
      append({Entry::Kind::Database, step.id, nullptr, step.label, {}, step.timestamp.toLocalTime()});
    });
    // Save point names and labels live in the store.
    connect(store, &HistoryStore::changed, this, &Timeline::changed);
  }
  auto manager = NOGGIT_ACTION_MGR;
  connect(manager, &ActionManager::currentActionChanged, this, [this](unsigned index) { _mapUndoIndex = index; });
  connect(manager, &ActionManager::addedAction, this, [this](Action* action) {
    // Clicks and selection changes also open actions; only ones that changed the map belong in history.
    if (!(action->getFlags() & ~(eVERTEX_SELECTION | eDO_NOT_WRITE_HISTORY))) return;
    append({Entry::Kind::Map, 0, action, describe(action), {}, QDateTime::currentDateTime()});
    if (!action->removedObjects().empty()) emit sceneryRemoved(action);
  });
  // Noggit dropped its undone actions because something new was done.
  connect(manager, &ActionManager::popBack, this, [this] {
    for (int i = _entries.size() - 1; i >= _cursor; --i) if (_entries[i].kind == Entry::Kind::Map) _entries.remove(i);
    emit changed();
  });
  // The oldest map action fell off Noggit's limited stack: it can no longer be undone.
  connect(manager, &ActionManager::popFront, this, [this] {
    for (int i = 0; i < _entries.size(); ++i) if (_entries[i].kind == Entry::Kind::Map) {
      _entries.remove(i); if (i < _cursor) --_cursor; break;
    }
    emit changed();
  });
  connect(manager, &ActionManager::purged, this, [this] {
    for (int i = _entries.size() - 1; i >= 0; --i) if (_entries[i].kind == Entry::Kind::Map) {
      _entries.remove(i); if (i < _cursor) --_cursor;
    }
    emit changed();
  });
}
void Timeline::seed() {
  auto store = HistoryStore::instance();
  if (!store) return;
  for (auto const& step : store->steps())
    _entries.push_back({Entry::Kind::Database, step.id, nullptr, step.label, {}, step.timestamp.toLocalTime()});
  _cursor = store->cursor();
}
void Timeline::append(Entry entry) {
  // Doing something new after undoing drops what was undone (the History store archives its part).
  _entries.resize(_cursor);
  _entries.push_back(std::move(entry));
  _cursor = _entries.size();
  emit changed();
}
QString Timeline::savepoint(int index) const {
  if (index < 0 || index >= _entries.size()) return {};
  auto const& entry = _entries[index];
  if (entry.kind == Entry::Kind::Map) return entry.savepoint;
  if (auto store = HistoryStore::instance())
    for (auto const& step : store->steps()) if (step.id == entry.step) return step.savepoint;
  return {};
}
bool Timeline::applyDatabase(Entry const& entry, bool undo) {
  auto store = HistoryStore::instance();
  int index = !store ? -1 : undo ? store->cursor() - 1 : store->cursor();
  if (!store || index < 0 || index >= store->steps().size() || store->steps()[index].id != entry.step) {
    QMessageBox::warning(_parent, "History", "This step can no longer be applied: the saved history changed outside this window.");
    return false;
  }
  try {
    Database db;
    auto query = [&db](QString const& sql) { return db.query(sql); };
    auto conflict = store->conflict(query, undo);
    if (!conflict.isEmpty() && QMessageBox::question(_parent, undo ? "Undo" : "Redo",
          conflict + QString("\n\n%1 anyway? Later changes to the same thing are replaced.").arg(undo ? "Undo" : "Redo"),
          QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
      return false;
    auto apply = [&db](QJsonArray const& image, QVector<HistoryEntity> const& entities) { db.replay(image, entities); };
    if (!(undo ? store->undo(apply) : store->redo(apply))) return false;
  } catch (std::exception const& e) {
    QMessageBox::warning(_parent, undo ? "Undo" : "Redo", e.what());
    return false;
  }
  _databaseChanged = true;
  if (!_batch && _afterDatabase) { _afterDatabase(); _databaseChanged = false; }
  return true;
}
// Undoes (or redoes) Noggit's actions up to and including `target`, passing over the empty ones history skips.
bool Timeline::walkMap(Action* target, bool undo) {
  auto manager = NOGGIT_ACTION_MGR;
  auto const& stack = *manager->getActionStack();
  for (;;) {
    int index = int(stack.size()) - int(_mapUndoIndex) - (undo ? 1 : 0);
    if (index < 0 || index >= int(stack.size())) return false;
    auto action = stack[index];
    undo ? manager->undo() : manager->redo();
    if (action == target) return true;
  }
}
bool Timeline::undo() {
  if (!_batch && _prepare && !_prepare()) return false;
  if (!canUndo() || NOGGIT_CUR_ACTION) return false;
  auto const entry = _entries[_cursor - 1]; // a copy: reloading the world may touch the list
  if (entry.kind == Entry::Kind::Map) {
    if (!walkMap(entry.action, true)) return false;
    if (!entry.action->removedObjects().empty()) emit sceneryRestored(entry.action);
  }
  else if (!applyDatabase(entry, true)) return false;
  --_cursor; emit changed();
  return true;
}
bool Timeline::redo() {
  if (!_batch && _prepare && !_prepare()) return false;
  if (!canRedo() || NOGGIT_CUR_ACTION) return false;
  auto const entry = _entries[_cursor];
  if (entry.kind == Entry::Kind::Map) {
    if (!walkMap(entry.action, false)) return false;
    if (!entry.action->removedObjects().empty()) emit sceneryRemoved(entry.action);
  }
  else if (!applyDatabase(entry, false)) return false;
  ++_cursor; emit changed();
  return true;
}
bool Timeline::jump(int cursor) {
  if (_prepare && !_prepare()) return false;
  cursor = std::clamp(cursor, 0, int(_entries.size()));
  _batch = true;
  bool ok = true;
  while (ok && _cursor > cursor) ok = undo();
  while (ok && _cursor < cursor) ok = redo();
  _batch = false;
  if (_databaseChanged && _afterDatabase) { _afterDatabase(); _databaseChanged = false; }
  return ok;
}
bool Timeline::addSavepoint(QString const& name) {
  if (_cursor == 0 || name.trimmed().isEmpty()) return false;
  auto& entry = _entries[_cursor - 1];
  if (entry.kind == Entry::Kind::Map) { entry.savepoint = name.trimmed(); emit changed(); return true; }
  auto store = HistoryStore::instance();
  for (int i = 0; store && i < store->steps().size(); ++i)
    if (store->steps()[i].id == entry.step) { store->setSavepoint(i, name); return true; }
  return false;
}
QString Timeline::objectName(QString const& path, bool wmo) {
  if (path.isEmpty()) return wmo ? "building" : "object";
  auto stem = QFileInfo(path).completeBaseName();
  stem.replace('_', ' ');
  stem.replace(QRegularExpression("([a-z])([A-Z0-9])"), "\\1 \\2");
  return stem.simplified();
}
QString Timeline::describe(Action const* action) {
  auto flags = const_cast<Action*>(action)->getFlags();
  if (!action->removedObjects().empty()) return "Removed " + objects(action->removedObjects());
  if (!action->addedObjects().empty()) return "Placed " + objects(action->addedObjects());
  if (flags & eOBJECTS_TRANSFORMED)
    return action->transformedObjectCount() == 1 ? "Moved an object" : QString("Moved %1 objects").arg(action->transformedObjectCount());
  if (flags & eCHUNKS_TERRAIN) return "Shaped terrain";
  if (flags & eCHUNKS_TEXTURE) return "Painted ground textures";
  if (flags & eCHUNKS_VERTEX_COLOR) return "Painted ground colours";
  if (flags & eCHUNKS_WATER) return "Edited water";
  if (flags & eCHUNKS_HOLES) return "Cut or filled holes";
  if (flags & eCHUNKS_AREAID) return "Changed zone areas";
  if (flags & eCHUNKS_FLAGS) return "Changed impassable areas";
  if (flags & eCHUNK_SHADOWS) return "Edited shadows";
  return "Map edit";
}
}
