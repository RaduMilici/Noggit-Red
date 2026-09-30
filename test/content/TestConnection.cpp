// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// The connection hooks of src/mysql for the tests: content_db.cpp runs against the live tests' scratch
// schema (see LiveDb.hpp) instead of the project's configured database.

#include "LiveDb.hpp"

#include <mysql/mysql_internal.hpp>

namespace mysql::detail
{
  void ConnectionCloser::operator()(MYSQL* connection) const
  {
    if (connection)
    {
      mysql_close(connection);
    }
  }

  Connection connect(std::string* error)
  {
    QStringList const parts = QString::fromUtf8(qgetenv("NOGGIT_NPC_TEST_DB")).split(':');
    if (parts.size() != 5)
    {
      if (error)
      {
        *error = "NOGGIT_NPC_TEST_DB not set";
      }
      return nullptr;
    }
    MYSQL* connection = mysql_init(nullptr);
    if (!mysql_real_connect(connection, parts[0].toUtf8().constData(), parts[2].toUtf8().constData(),
                            parts[3].toUtf8().constData(), LiveDb::scratch, parts[1].toUInt(), nullptr, 0))
    {
      if (error)
      {
        *error = mysql_error(connection);
      }
      mysql_close(connection);
      return nullptr;
    }
    mysql_set_character_set(connection, "utf8");
    return Connection(connection);
  }

  bool execute(MYSQL* connection, std::string const& statement, std::string* error)
  {
    if (mysql_query(connection, statement.c_str()) == 0)
    {
      return true;
    }
    if (error)
    {
      *error = mysql_error(connection);
    }
    return false;
  }
}
