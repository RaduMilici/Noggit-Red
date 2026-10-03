#pragma once
#include "Services.hpp"
#include <QObject>
#include <QDateTime>
#include <QJsonObject>
#include <QStringList>
#include <functional>
#include <optional>
namespace Noggit::Creator {
enum class EntityType { Npc, Spawn, Quest, Item };
enum class ChangeAction { Create, Update, Delete, Move };
QString toString(EntityType);
QString toString(ChangeAction);
// One net change per entity. before/after hold the entity's rows grouped by table
// ({"creature_template":[{...}], ...}); an empty object means the entity did not exist.
struct TrackedChange {
  QString id;
  EntityType type = EntityType::Npc;
  Id entity = 0;
  ChangeAction action = ChangeAction::Create;
  QString label;
  QJsonObject before, after;
  QDateTime timestamp;
  QString summary() const;
};
using QueryFunction = std::function<QVector<Fields>(QString const&)>;
// Pending local changes, persisted in the Creator workspace (never in the world database).
// Database drives a two-phase write: stage() writes the merged list beside the recovery
// journal, the database commits by removing that journal, then promote() publishes the list.
// A staged list found while the journal still exists belongs to a rolled-back save.
class ChangeTracker final : public QObject {
  Q_OBJECT
public:
  static ChangeTracker* instance(); // null outside a managed Creator runtime
  explicit ChangeTracker(QString const& workspace, QObject* parent = nullptr);
  QVector<TrackedChange> const& changes() const { return _changes; }
  QString warning() const { return _warning; }
  void stage(QVector<TrackedChange> const& changes);
  void promote();
  void discard();
  void clear(QStringList const& ids);
  static void merge(QVector<TrackedChange>& list, TrackedChange change);
  static std::optional<ChangeAction> derive(EntityType, QJsonObject const& before, QJsonObject const& after);
  static QJsonObject capture(QueryFunction const& query, EntityType, Id, QString* label = nullptr);
  // Statements that reproduce the tracked after-states on another compatible world database.
  static QString sql(QVector<TrackedChange> const& changes);
signals:
  void changed();
private:
  QString _file, _pending, _journal, _warning;
  QVector<TrackedChange> _changes, _staged;
  bool _hasStaged = false;
  void load();
  void write(QString const& path, QVector<TrackedChange> const& changes) const;
};
}
