#pragma once
#include "Services.hpp"
#include <QObject>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QPair>
#include <QStringList>
#include <functional>
#include <optional>
namespace Noggit::Creator {
// Loot, ObjectLoot, Vendor, Trainer and Gossip (dialogue) are keyed by the NPC or GameObject entry that owns them.
enum class EntityType { Npc, Spawn, Quest, Item, GameObject, GameObjectSpawn, Loot, ObjectLoot, Vendor, Trainer, Gossip };
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
  // Removes entries that reached production. An entry edited again since `synced` was taken stays listed.
  void markSynced(QVector<TrackedChange> const& synced);
  static void merge(QVector<TrackedChange>& list, TrackedChange change);
  static std::optional<ChangeAction> derive(EntityType, QJsonObject const& before, QJsonObject const& after);
  static QJsonObject capture(QueryFunction const& query, EntityType, Id, QString* label = nullptr);
  // Statements that reproduce the tracked after-states on another compatible world database.
  static QString sql(QVector<TrackedChange> const& changes);
  // Every row sql(changes) can change on `target`, as (table, where) on columns the script never updates,
  // so the same rows match before and after it runs. `target` is read for its current state of the entities.
  static QVector<QPair<QString, QString>> footprint(QVector<TrackedChange> const& changes, QueryFunction const& target);
  // REPLACE statements for rows as capture() returns them.
  static QString upsert(QString const& table, QJsonArray const& rows);
  // The statements of sql(): one per line, since values escape line breaks and comments are single lines.
  static QStringList statements(QString const& sql);
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
