#pragma once
#include "ChangeTracker.hpp"
#include <QDateTime>
#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>
namespace Noggit::Creator {
struct HistoryEntity { EntityType type = EntityType::Npc; Id id = 0; QString label; };
// One local save, undoable. before/after are row images in the recovery journal's format:
// [{table, key, id, rows}] or [{table, where, rows}]; applying one puts exactly those rows back.
struct HistoryStep {
  int id = 0;
  QString label, savepoint;
  QDateTime timestamp;
  QVector<HistoryEntity> entities;
  QJsonArray before, after;
};
// Puts an image's rows back in the local database and records the entities in Local Changes.
using HistoryApply = std::function<void(QJsonArray const& image, QVector<HistoryEntity> const& entities)>;
// Every local database save as a timeline the designer can walk back and forth.
// Persisted in Workspace/creator-history (index.json plus one file per step), never in the world database.
// cursor() is how many steps are applied: undo applies the step's before-image, redo its after-image.
// Recording a new step after undoing archives the undone ones in discarded/, so nothing is silently lost.
class HistoryStore final : public QObject {
  Q_OBJECT
public:
  static HistoryStore* instance(); // null outside a managed Creator runtime
  explicit HistoryStore(QString const& folder, QObject* parent = nullptr);
  // Steps without their images (loaded on demand by undo/redo).
  QVector<HistoryStep> const& steps() const { return _steps; }
  int cursor() const { return _cursor; }
  bool canUndo() const { return _cursor > 0; }
  bool canRedo() const { return _cursor < _steps.size(); }
  QString warning() const { return _warning; }
  void record(HistoryStep step);
  // What changed since the step was applied (empty: nothing), checked against `current`.
  QString conflict(QueryFunction const& current, bool undo) const;
  // Applies the next step back or forward. Returns false when there is none.
  bool undo(HistoryApply const& apply);
  bool redo(HistoryApply const& apply);
  void setSavepoint(int index, QString const& name);
  // Full step including its images.
  HistoryStep load(int index) const;
  // Rows an image entry selects now, in the same format (used for after-images and conflict checks).
  static QJsonArray read(QueryFunction const& query, QJsonArray const& image);
  // True when two images of the same entries hold the same rows (a save that changed nothing).
  static bool same(QJsonArray const& a, QJsonArray const& b);
  // Label for a save from its tracked changes ("Moved Grut", "Created NPC Grut and 2 more").
  static QString label(QVector<TrackedChange> const& changes);
signals:
  void changed();
  void recorded(); // a new step was added at the end (the previous redo steps are gone)
private:
  QString _folder, _warning;
  QVector<HistoryStep> _steps;
  int _cursor = 0, _next = 1;
  void load();
  void writeIndex() const;
  void writeStep(HistoryStep const& step) const;
  QString stepPath(int id) const;
};
}
