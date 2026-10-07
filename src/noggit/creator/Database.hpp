#pragma once
#include "Services.hpp"
#include "ChangeTracker.hpp"
#include "History.hpp"
#include <QJsonArray>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace Noggit::Creator {
// Dedicated loopback connection. Never uses a designer's saved external DB credentials.
class Database {
public:
  Database();
  ~Database();
  Database(Database const&) = delete;
  QString quote(QVariant const&) const;
  QVector<Fields> query(QString const& sql);
  void exec(QString const& sql);
  // Rows in column order, NULL as nullopt (the quest and item editors' SQL builders read by position).
  std::vector<std::vector<std::optional<std::string>>> rows(std::string const& sql);
  // Runs one statement and returns how many rows it changed.
  std::uint64_t execute(std::string const& sql);
  void insert(QString const& table, Fields const& fields);
  Id allocate(QString const& table, QString const& key, Id limit = 16777215);
  bool owned(QString const& kind, Id id);
  void mark(QString const& kind, Id id);
  void snapshot(QString const& table, QString const& key, Id id);
  // Before-image of every row matching `where`; it must select the same rows before and after the save.
  void snapshotWhere(QString const& table, QString const& where);
  // Record an entity in Local Changes. Call before modifying it; the after-state is captured on commit.
  void track(EntityType type, Id id);
  // Names this save in the History timeline (default: derived from the tracked changes).
  void setLabel(QString const& label) { _label = label; }
  // Puts an image's rows back (undo/redo of a History step), journaled and tracked like any save, then commits.
  // Recorded as a new History step only when named (bringing a removed thing back).
  void replay(QJsonArray const& image, QVector<HistoryEntity> const& entities, QString const& recordAs = {});
  void commit();
private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
  struct Tracked { EntityType type; Id id; QJsonObject before; QString label; };
  QVector<Tracked> _tracked;
  QJsonArray _undo;
  QString _journal;
  QString _label;
  bool _committed = false, _replaying = false;
  void restore(QJsonArray const&);
  void persist();
};
}
