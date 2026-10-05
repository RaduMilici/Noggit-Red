#pragma once
#include "Services.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace Noggit::Creator {
struct Endpoint {
  QString host = "127.0.0.1";
  unsigned port = 3306;
  QString user, password, database;
  unsigned timeoutSeconds = 3;
};
// One MySQL/MariaDB client connection. Errors throw std::runtime_error with `context` and the server's message.
class SqlConnection {
public:
  SqlConnection(Endpoint const& endpoint, QString context);
  ~SqlConnection();
  SqlConnection(SqlConnection const&) = delete;
  QString quote(QVariant const&) const;
  QVector<Fields> query(QString const& sql);
  // Rows in column order, NULL as nullopt.
  std::vector<std::vector<std::optional<std::string>>> rows(std::string const& sql);
  // Runs one statement and returns how many rows it changed.
  std::uint64_t execute(std::string const& sql);
private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
  QString _context;
  [[noreturn]] void fail(QString const& what) const;
};
}
