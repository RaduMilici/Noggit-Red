#pragma once
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>
class QWidget;
namespace Noggit { class Action; }
namespace Noggit::Creator {
// The one undo history a designer sees: local database saves (History steps, kept across sessions)
// and map edits (terrain and scenery, Noggit's ActionManager, this session only), in the order they happened.
// Ctrl+Z / Ctrl+Y walk it whatever tool is active.
class Timeline final : public QObject {
  Q_OBJECT
public:
  struct Entry {
    enum class Kind { Database, Map } kind = Kind::Database;
    int step = 0;                     // Database: History step id
    Noggit::Action* action = nullptr; // Map: the ActionManager action
    QString label, savepoint;         // savepoint: Map entries only (Database ones live in the History store)
    QDateTime time;
  };
  // afterDatabase runs after undo/redo changed the local database (reload what the world shows).
  Timeline(QWidget* dialogParent, std::function<void()> afterDatabase, QObject* parent = nullptr);
  // Runs before any undo/redo/jump (save edits still in progress); returning false cancels it.
  void setPrepare(std::function<bool()> prepare) { _prepare = std::move(prepare); }
  QVector<Entry> const& entries() const { return _entries; }
  int cursor() const { return _cursor; } // entries before it are applied
  QString savepoint(int index) const;
  bool canUndo() const { return _cursor > 0; }
  bool canRedo() const { return _cursor < _entries.size(); }
  bool undo();
  bool redo();
  // Walks to `cursor` (0: before everything). Stops at the first step that cannot be applied.
  bool jump(int cursor);
  // Names the current point (the last applied entry). False when nothing has been done yet.
  bool addSavepoint(QString const& name);
  static QString describe(Noggit::Action const* action);
  // A scenery model's readable name: "World/.../ElwynnTreeMid01.m2" -> "Elwynn Tree Mid 01".
  static QString objectName(QString const& path, bool wmo);
signals:
  void changed();
  // A map action that removed scenery was done (redo=true also for redo) or undone.
  void sceneryRemoved(Noggit::Action const* action);
  void sceneryRestored(Noggit::Action const* action);
private:
  QWidget* _parent;
  std::function<void()> _afterDatabase;
  std::function<bool()> _prepare;
  QVector<Entry> _entries;
  int _cursor = 0;
  unsigned _mapUndoIndex = 0; // ActionManager's count of undone actions
  bool walkMap(Noggit::Action* target, bool undo);
  bool _batch = false, _databaseChanged = false;
  void seed();
  void append(Entry entry);
  bool applyDatabase(Entry const& entry, bool undo);
};
}
