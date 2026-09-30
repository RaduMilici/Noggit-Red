// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// The connection hooks mysql.cpp shares with the other files of the MySQL layer (content_db.cpp). Not for
// use outside src/mysql.

#include <mysql.h>

#include <memory>
#include <string>

namespace mysql::detail
{
  struct ConnectionCloser
  {
    void operator()(MYSQL* connection) const;
  };
  using Connection = std::unique_ptr<MYSQL, ConnectionCloser>;

  // A connection to the project's world database (honours the MySQL toggle and the SSH tunnel), or null
  // with *error set.
  Connection connect(std::string* error = nullptr);

  bool execute(MYSQL* connection, std::string const& statement, std::string* error = nullptr);
}
