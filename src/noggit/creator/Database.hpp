#pragma once
#include "Services.hpp"
#include <QJsonArray>
#include <memory>
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
  void insert(QString const& table, Fields const& fields);
  Id allocate(QString const& table, QString const& key, Id limit = 16777215);
  bool owned(QString const& kind, Id id);
  void mark(QString const& kind, Id id);
  void snapshot(QString const& table, QString const& key, Id id);
  void commit();
private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
  QJsonArray _undo;
  QString _journal;
  bool _committed = false;
  void restore(QJsonArray const&);
  void persist();
};
}
