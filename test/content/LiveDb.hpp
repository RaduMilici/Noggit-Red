// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// A scratch copy of a real world database for the live tests. Enabled by NOGGIT_NPC_TEST_DB =
// "host:port:user:password:source_schema" (e.g. a vmangos "mangos" schema). The tables the tests need are
// created LIKE the source's and filled with a few of its rows in the scratch schema noggit_content_test,
// which is dropped at the end. The source schema is only ever read.

#pragma once

#include <noggit/content/SqlText.hpp>

#include <mysql.h>

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QtGlobal>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// QSKIP returns from the calling function, so it has to be expanded in the test body.
#define REQUIRE_LIVE_DB(db)                                                            \
  do                                                                                   \
  {                                                                                    \
    if (!(db).connected())                                                             \
    {                                                                                  \
      QSKIP("NOGGIT_NPC_TEST_DB not set (host:port:user:password:source_schema)");     \
    }                                                                                  \
  } while (false)

class LiveDb
{
public:
  static constexpr char const* scratch = "noggit_content_test";

  // Connects and creates an empty scratch schema. Returns an error text, or "" (also when not enabled).
  std::string open()
  {
    QByteArray const spec = qgetenv("NOGGIT_NPC_TEST_DB");
    if (spec.isEmpty())
    {
      return {};
    }
    QStringList const parts = QString::fromUtf8(spec).split(':');
    if (parts.size() != 5)
    {
      return "NOGGIT_NPC_TEST_DB must be host:port:user:password:source_schema";
    }
    _source = parts[4].toStdString();
    MYSQL* connection = mysql_init(nullptr);
    if (!mysql_real_connect(connection, parts[0].toUtf8().constData(), parts[2].toUtf8().constData(),
                            parts[3].toUtf8().constData(), nullptr, parts[1].toUInt(), nullptr, 0))
    {
      std::string const error = mysql_error(connection);
      mysql_close(connection);
      return "connect failed: " + error;
    }
    mysql_set_character_set(connection, "utf8");
    _db.reset(connection);
    std::string const s = scratch;
    if (!exec("DROP DATABASE IF EXISTS `" + s + "`") || !exec("CREATE DATABASE `" + s + "`") || !exec("USE `" + s + "`"))
    {
      return "could not create the scratch schema";
    }
    return {};
  }

  void close()
  {
    if (_db)
    {
      exec("DROP DATABASE IF EXISTS `" + std::string(scratch) + "`");
      _db.reset();
    }
  }

  bool connected() const { return static_cast<bool>(_db); }
  std::string const& source() const { return _source; }

  // Creates `table` like the source's, with the source rows matching `where` ("" = none).
  bool copyTable(std::string const& table, std::string const& where = {})
  {
    if (!exec("CREATE TABLE `" + table + "` LIKE `" + _source + "`.`" + table + "`"))
    {
      return false;
    }
    return where.empty() || exec("INSERT INTO `" + table + "` SELECT * FROM `" + _source + "`.`" + table + "` WHERE " + where);
  }

  bool exec(std::string const& sql)
  {
    if (mysql_query(_db.get(), sql.c_str()) != 0)
    {
      qWarning("SQL failed: %s\n  %s", mysql_error(_db.get()), sql.substr(0, 500).c_str());
      return false;
    }
    if (MYSQL_RES* result = mysql_store_result(_db.get()))
    {
      mysql_free_result(result);
    }
    return true;
  }

  bool execAll(std::vector<std::string> const& statements)
  {
    for (auto const& statement : statements)
    {
      if (!statement.empty() && !exec(statement))
      {
        return false;
      }
    }
    return true;
  }

  std::vector<Noggit::Content::SqlRow> rows(std::string const& sql)
  {
    std::vector<Noggit::Content::SqlRow> out;
    if (mysql_query(_db.get(), sql.c_str()) != 0)
    {
      qWarning("SQL failed: %s\n  %s", mysql_error(_db.get()), sql.c_str());
      return out;
    }
    MYSQL_RES* result = mysql_store_result(_db.get());
    if (!result)
    {
      return out;
    }
    unsigned int const fields = mysql_num_fields(result);
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
      unsigned long const* lengths = mysql_fetch_lengths(result);
      Noggit::Content::SqlRow values;
      for (unsigned int i = 0; i < fields; ++i)
      {
        values.push_back(row[i] ? std::optional<std::string>(std::string(row[i], lengths[i])) : std::nullopt);
      }
      out.push_back(std::move(values));
    }
    mysql_free_result(result);
    return out;
  }

  std::string scalar(std::string const& sql)
  {
    auto result = rows(sql);
    return result.empty() || result[0].empty() || !result[0][0] ? std::string() : *result[0][0];
  }

  // The scratch schema's columns of `table` (empty when it does not exist): a ColumnsOf.
  std::vector<std::string> columnsOf(std::string const& table)
  {
    std::vector<std::string> out;
    for (auto const& row : rows("SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = '"
                                + std::string(scratch) + "' AND TABLE_NAME = '" + table + "' ORDER BY ORDINAL_POSITION"))
    {
      out.push_back(*row[0]);
    }
    return out;
  }

private:
  struct Closer
  {
    void operator()(MYSQL* connection) const { mysql_close(connection); }
  };
  std::unique_ptr<MYSQL, Closer> _db;
  std::string _source;
};
