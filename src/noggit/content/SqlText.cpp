// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/content/SqlText.hpp>

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace Noggit::Content
{
  namespace
  {
    // Set and read on the UI thread (the editors and the database layer they open).
    CustomIds& currentIds()
    {
      static CustomIds ids;
      return ids;
    }
  }

  CustomIds const& customIds()
  {
    return currentIds();
  }

  void setCustomIds(CustomIds ids)
  {
    currentIds() = ids;
  }

  CustomIds customIdsFor(std::vector<std::string> const& creature_template_columns)
  {
    if (hasColumn(creature_template_columns, "display_id1") && !hasColumn(creature_template_columns, "patch"))
    {
      return {TURTLE_CUSTOM_START, TURTLE_CUSTOM_START};
    }
    return {};
  }

  std::uint32_t nextFreeId(std::uint32_t highest_used, std::uint32_t start)
  {
    return std::max(start, highest_used + 1);
  }

  std::string escapeString(std::string const& value)
  {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value)
    {
      switch (c)
      {
        case '\0':   out += "\\0"; break;
        case '\n':   out += "\\n"; break;
        case '\r':   out += "\\r"; break;
        case '\x1a': out += "\\Z"; break;
        case '\\':   out += "\\\\"; break;
        case '\'':   out += "\\'"; break;
        case '"':    out += "\\\""; break;
        default:     out += c; break;
      }
    }
    return out;
  }

  std::string quoteIdentifier(std::string const& identifier)
  {
    std::string out = "`";
    for (char c : identifier)
    {
      if (c == '`')
      {
        out += '`';
      }
      out += c;
    }
    return out + "`";
  }

  std::string literal(std::string const& text)
  {
    return "'" + escapeString(text) + "'";
  }

  std::string literal(std::int64_t number)
  {
    return std::to_string(number);
  }

  std::string literal(float number)
  {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", number);
    return buffer;
  }

  std::string equals(std::string const& column, std::string const& literal_value)
  {
    return quoteIdentifier(column) + " = " + literal_value;
  }

  std::string allOf(std::vector<std::string> const& conditions)
  {
    std::string out;
    for (auto const& condition : conditions)
    {
      out += (out.empty() ? "" : " AND ") + condition;
    }
    return out;
  }

  std::string insertStatement(std::string const& table, Assignments const& values, bool ignore_duplicates)
  {
    std::string columns, literals;
    for (auto const& [column, value] : values)
    {
      columns += (columns.empty() ? "" : ", ") + quoteIdentifier(column);
      literals += (literals.empty() ? "" : ", ") + value;
    }
    return std::string(ignore_duplicates ? "INSERT IGNORE INTO " : "INSERT INTO ") + quoteIdentifier(table)
         + " (" + columns + ") VALUES (" + literals + ")";
  }

  std::string updateStatement(std::string const& table, Assignments const& values, std::string const& where)
  {
    if (values.empty())
    {
      return {};
    }
    std::string set;
    for (auto const& [column, value] : values)
    {
      set += (set.empty() ? "" : ", ") + equals(column, value);
    }
    return "UPDATE " + quoteIdentifier(table) + " SET " + set + " WHERE " + where;
  }

  std::string deleteStatement(std::string const& table, std::string const& where)
  {
    return "DELETE FROM " + quoteIdentifier(table) + " WHERE " + where;
  }

  Assignments inTableOrder(std::vector<std::string> const& columns, ColumnValues const& values)
  {
    Assignments out;
    for (auto const& column : columns)
    {
      if (auto const found = values.find(column); found != values.end())
      {
        out.emplace_back(column, found->second);
      }
    }
    return out;
  }

  std::string sourceColumn(std::string const& column)
  {
    return "src." + quoteIdentifier(column);
  }

  std::string copyRowStatement(std::string const& table,
                               std::vector<std::string> const& columns,
                               ColumnValues const& overrides,
                               std::string const& source_where,
                               std::string const& newest_first)
  {
    std::string select;
    for (auto const& column : columns)
    {
      auto const found = overrides.find(column);
      select += (select.empty() ? "" : ", ") + (found != overrides.end() ? found->second : sourceColumn(column));
    }
    std::string statement = "INSERT INTO " + quoteIdentifier(table) + " (" + columnList(columns) + ") SELECT "
                          + select + " FROM " + quoteIdentifier(table) + " src WHERE " + source_where;
    if (!newest_first.empty())
    {
      statement += " ORDER BY " + sourceColumn(newest_first) + " DESC LIMIT 1";
    }
    return statement;
  }

  std::string buildInsertValues(std::string const& table,
                                std::vector<std::string> const& columns,
                                std::vector<SqlRow> const& rows)
  {
    if (rows.empty())
    {
      return {};
    }
    std::stringstream insert;
    insert << "INSERT INTO " << quoteIdentifier(table) << " (" << columnList(columns) << ") VALUES";
    for (std::size_t r = 0; r < rows.size(); ++r)
    {
      insert << (r ? ",\n  (" : "\n  (");
      for (std::size_t c = 0; c < rows[r].size(); ++c)
      {
        auto const& value = rows[r][c];
        insert << (c ? ", " : "") << (value ? literal(*value) : std::string("NULL"));
      }
      insert << ")";
    }
    insert << ";";
    return insert.str();
  }

  std::string columnList(std::vector<std::string> const& columns)
  {
    std::string out;
    for (auto const& column : columns)
    {
      out += (out.empty() ? "" : ", ") + quoteIdentifier(column);
    }
    return out;
  }

  std::string idList(std::vector<std::uint32_t> ids)
  {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::string out;
    for (auto const id : ids)
    {
      out += (out.empty() ? "" : ", ") + std::to_string(id);
    }
    return out;
  }

  bool hasColumn(std::vector<std::string> const& columns, std::string const& column)
  {
    return std::find(columns.begin(), columns.end(), column) != columns.end();
  }

  std::string firstColumn(std::vector<std::string> const& columns, std::initializer_list<char const*> candidates)
  {
    for (auto const* candidate : candidates)
    {
      if (hasColumn(columns, candidate))
      {
        return candidate;
      }
    }
    return {};
  }
}
