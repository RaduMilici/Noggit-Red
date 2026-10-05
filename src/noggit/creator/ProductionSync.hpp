#pragma once
#include "ChangeExport.hpp"
#include "ProductionProfile.hpp"
#include <functional>
namespace Noggit::Creator {
// Before-image of every production row a sync can change. Restoring it puts those rows back exactly,
// which is the only rollback MyISAM world tables have.
struct ProductionBackup {
  QVector<QPair<QString, QString>> scopes; // (table, where), see ChangeTracker::footprint
  QVector<QJsonArray> rows;                // per scope
  static ProductionBackup take(QVector<QPair<QString, QString>> const& scopes, QueryFunction const& production);
  int rowCount() const;
  QJsonObject toJson() const;
  QStringList restoreStatements() const;
};
// Sync to Production: applies a change package to the production world database through an SSH tunnel.
// Blocking; run it on a worker thread. Production is only read before the backup is safely on disk.
class ProductionSync {
public:
  enum class Step { Connect, Backup, Apply, Restart };
  enum class State { Running, Done, Skipped, Failed };
  using Progress = std::function<void(Step, State, QString const& detail)>;
  struct Outcome {
    bool ok = false;
    enum class Rollback { NotNeeded, Restored, Failed } rollback = Rollback::NotNeeded;
    QString error, details; // what failed, and the server's own output
  };
  // `folder` already holds the package (manifest.json, changes.sql); the backup, restore script, log and
  // result are added to it.
  static Outcome run(ProductionProfile const&, QString const& password, ChangePackage const&, QString const& folder, Progress const&);

  struct Check { QStringList lines, warnings; };
  // Logs in over SSH and to the database, and reads (never changes) the production world. Throws
  // Ssh::Error or std::runtime_error.
  static Check test(ProductionProfile const&, QString const& password, QJsonObject const& localSource);

  // Package entities whose IDs production already uses for content Noggit did not put there.
  static QStringList conflicts(ChangePackage const&, QueryFunction const& production);
};
}
