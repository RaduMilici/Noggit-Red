#if defined(_WIN32) && defined(USE_MYSQL_UID_STORAGE)
#include <winsock2.h>
#endif
#include "SqlConnection.hpp"
#include <stdexcept>
#ifdef USE_MYSQL_UID_STORAGE
#include <mysql.h>
#endif
namespace Noggit::Creator {
struct SqlConnection::Impl {
#ifdef USE_MYSQL_UID_STORAGE
  MYSQL* connection = nullptr;
  ~Impl() { if (connection) mysql_close(connection); }
#endif
};
void SqlConnection::fail(QString const& what) const {
#ifdef USE_MYSQL_UID_STORAGE
  if (_impl->connection && *mysql_error(_impl->connection))
    throw std::runtime_error((_context + ": " + QString::fromUtf8(mysql_error(_impl->connection))).toStdString());
#endif
  throw std::runtime_error((_context + ": " + what).toStdString());
}
SqlConnection::SqlConnection(Endpoint const& endpoint, QString context) : _impl(std::make_unique<Impl>()), _context(std::move(context)) {
#ifdef USE_MYSQL_UID_STORAGE
  _impl->connection = mysql_init(nullptr);
  if (!_impl->connection) fail("cannot initialize the database client");
  unsigned timeout = endpoint.timeoutSeconds;
  mysql_options(_impl->connection, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  mysql_options(_impl->connection, MYSQL_OPT_READ_TIMEOUT, &timeout);
  mysql_options(_impl->connection, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
  auto host = endpoint.host.toUtf8(), user = endpoint.user.toUtf8(), password = endpoint.password.toUtf8(), database = endpoint.database.toUtf8();
  if (!mysql_real_connect(_impl->connection, host.constData(), user.constData(), password.constData(),
                          database.isEmpty() ? nullptr : database.constData(), endpoint.port, nullptr, 0))
    fail("cannot connect");
  mysql_set_character_set(_impl->connection, "utf8mb4");
#else
  (void)endpoint;
  fail("this Noggit build does not include database support");
#endif
}
SqlConnection::~SqlConnection() = default;
QString SqlConnection::quote(QVariant const& value) const {
  if (value.isNull()) return "NULL";
#ifdef USE_MYSQL_UID_STORAGE
  QByteArray bytes = value.toString().toUtf8(), escaped(bytes.size() * 2 + 1, '\0');
  auto size = mysql_real_escape_string(_impl->connection, escaped.data(), bytes.constData(), bytes.size());
  escaped.resize(size); return "'" + QString::fromUtf8(escaped) + "'";
#else
  return {};
#endif
}
QVector<Fields> SqlConnection::query(QString const& sql) {
  QVector<Fields> rows;
#ifdef USE_MYSQL_UID_STORAGE
  auto bytes = sql.toUtf8();
  if (mysql_real_query(_impl->connection, bytes.constData(), bytes.size())) fail("query failed");
  MYSQL_RES* result = mysql_store_result(_impl->connection);
  if (!result) { if (mysql_field_count(_impl->connection)) fail("cannot read the result"); return rows; }
  auto fields = mysql_fetch_fields(result); unsigned count = mysql_num_fields(result);
  while (auto row = mysql_fetch_row(result)) {
    Fields values; auto lengths = mysql_fetch_lengths(result);
    for (unsigned i = 0; i < count; ++i) values[QString::fromUtf8(fields[i].name)] = row[i] ? QVariant(QString::fromUtf8(row[i], lengths[i])) : QVariant();
    rows.push_back(values);
  }
  mysql_free_result(result);
#endif
  return rows;
}
std::vector<std::vector<std::optional<std::string>>> SqlConnection::rows(std::string const& sql) {
  std::vector<std::vector<std::optional<std::string>>> rows;
#ifdef USE_MYSQL_UID_STORAGE
  if (mysql_real_query(_impl->connection, sql.data(), sql.size())) fail("query failed");
  MYSQL_RES* result = mysql_store_result(_impl->connection);
  if (!result) { if (mysql_field_count(_impl->connection)) fail("cannot read the result"); return rows; }
  unsigned count = mysql_num_fields(result);
  while (auto row = mysql_fetch_row(result)) {
    auto lengths = mysql_fetch_lengths(result); std::vector<std::optional<std::string>> values;
    for (unsigned i = 0; i < count; ++i) values.push_back(row[i] ? std::optional<std::string>(std::string(row[i], lengths[i])) : std::nullopt);
    rows.push_back(std::move(values));
  }
  mysql_free_result(result);
#endif
  return rows;
}
std::uint64_t SqlConnection::execute(std::string const& sql) {
#ifdef USE_MYSQL_UID_STORAGE
  if (mysql_real_query(_impl->connection, sql.data(), sql.size())) fail("statement failed");
  if (auto result = mysql_store_result(_impl->connection)) mysql_free_result(result);
  return mysql_affected_rows(_impl->connection);
#else
  (void)sql; return 0;
#endif
}
}
