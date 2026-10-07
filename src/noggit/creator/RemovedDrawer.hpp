#pragma once
#include "History.hpp"
#include <QDateTime>
#include <QJsonArray>
#include <QObject>
#include <QWidget>
#include <functional>
class QComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;
namespace Noggit { class Action; }
namespace Noggit::Creator {
// "3 min ago", "yesterday 14:05"
QString relativeTime(QDateTime const& time);
// Something the designer removed, kept so it can be brought back at any time.
struct RemovedThing {
  enum class Kind { Scenery, Npc, Object } kind = Kind::Scenery;
  QString id, label;
  QDateTime time;
  unsigned map = 0;
  double x = 0, y = 0, z = 0; // Scenery: world position; placements: server position
  // Scenery (map files)
  QString file; quint32 fileDataId = 0; bool wmo = false;
  double rx = 0, ry = 0, rz = 0, scale = 1;
  // NPC and object placements (local database): the rows the removal took away.
  Id guid = 0;
  QJsonArray image;
};
// The Removed drawer's contents, persisted in Workspace/creator-removed.json.
// Scenery comes from removing map objects, placements from History steps that deleted them.
class RemovedStore final : public QObject {
  Q_OBJECT
public:
  static RemovedStore* instance(); // null outside a managed Creator runtime
  explicit RemovedStore(QString const& file, QObject* parent = nullptr);
  QVector<RemovedThing> const& things() const { return _things; }
  void scenery(Noggit::Action const* action, unsigned map, bool removed);
  void placements(HistoryStep const& step);
  void forget(QString const& id);
  // Drops placements that are back (an undo in History restored them).
  void prune(QueryFunction const& query);
  // Puts a removed placement back as a new History step (a new guid when another placement took its own).
  static void bringBack(RemovedThing const& thing);
signals:
  void changed();
private:
  QString _file;
  QVector<RemovedThing> _things;
  void add(RemovedThing thing);
  void save() const;
};
// The drawer: what was removed, newest first, with Bring back and Show me.
class RemovedDrawer final : public QWidget {
  Q_OBJECT
public:
  struct Actions {
    std::function<void(RemovedThing const&)> restoreScenery; // re-add the map object (an undoable map edit)
    std::function<void(RemovedThing const&)> show;           // fly the camera there
    std::function<void()> afterDatabase;                     // reload placements shown in the world
    std::function<unsigned()> currentMap;
  };
  RemovedDrawer(Actions actions, QWidget* parent = nullptr);
  void refresh();
protected:
  void showEvent(QShowEvent*) override;
private:
  Actions _actions;
  QLineEdit* _search;
  QComboBox* _filter;
  QListWidget* _list;
  QPushButton *_bring, *_show, *_forget;
  RemovedThing const* selected() const;
  void bringBack();
  void updateButtons();
};
}
