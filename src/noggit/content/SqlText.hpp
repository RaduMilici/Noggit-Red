// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// Shared SQL text building for the content editors (NPCs, quests, items, dialogue). Pure: no MySQL
// client dependency, so every module built on it is unit-tested on its own (test/npc_template).
//
// Values are passed around as ready-made SQL literals (see literal()), so a statement builder never
// has to know a column's type.

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Noggit::Content
{
  // New custom content (NPCs, quests, items, texts) gets IDs from here upwards, clear of every Blizzard
  // ID of the supported clients (vanilla creatures/quests end below 20000, 3.3.5a below 40000).
  constexpr std::uint32_t CUSTOM_ENTRY_START = 90000;

  // Texts the editors write (spoken lines, dialogue) get IDs from here up (the vmangos data ends near 100000).
  constexpr std::uint32_t TEXT_ID_START = 900000;

  // Turtle WoW / tortoise-wow ship their own content far above 90000 (creatures up to 2509000, quests
  // 1140820, texts 2593005, items 92052), so there the editors' range -- entries and texts alike -- starts
  // here: above all of it, and below 8388607, the limit of the signed quest-chain columns (PrevQuestId...).
  constexpr std::uint32_t TURTLE_CUSTOM_START = 6000000;

  // Where the editors' IDs start on the connected world database. Defaults to the vmangos range; the
  // database layer switches it when it connects (see customIdsFor).
  struct CustomIds
  {
    std::uint32_t entry_start = CUSTOM_ENTRY_START;
    std::uint32_t text_start = TEXT_ID_START;
  };
  CustomIds const& customIds();
  void setCustomIds(CustomIds ids);

  // The range for a world database, from its creature_template columns: the Turtle layout (vmangos-style
  // display_id1 without vmangos' patch versioning) gets TURTLE_CUSTOM_START.
  CustomIds customIdsFor(std::vector<std::string> const& creature_template_columns);

  // True for content the editors created (and so may change or delete).
  inline bool isCustom(std::uint32_t entry) { return entry >= customIds().entry_start; }
  inline bool isCustomText(std::uint32_t id) { return id >= customIds().text_start; }

  // First free ID at or above `start`, given the highest ID already used there (0 when none).
  std::uint32_t nextFreeId(std::uint32_t highest_used, std::uint32_t start = customIds().entry_start);

  // One row of a table, as returned by the server (nullopt = NULL).
  using SqlRow = std::vector<std::optional<std::string>>;

  // Escapes a string for use inside '...' -- the same escapes as mysql_real_escape_string on a utf8
  // connection (which does not consult the connection's sql_mode either).
  std::string escapeString(std::string const& value);

  // Backtick-quotes an identifier.
  std::string quoteIdentifier(std::string const& identifier);

  // SQL literals.
  std::string literal(std::string const& text);
  inline std::string literal(char const* text) { return literal(std::string(text)); }
  std::string literal(std::int64_t number);
  std::string literal(float number);
  inline std::string literal(std::uint32_t number) { return literal(static_cast<std::int64_t>(number)); }
  inline std::string literal(std::int32_t number) { return literal(static_cast<std::int64_t>(number)); }

  // (column, SQL literal) pairs, in the order they should appear.
  using Assignments = std::vector<std::pair<std::string, std::string>>;

  // "`col` = literal" -- the building block of WHERE clauses.
  std::string equals(std::string const& column, std::string const& literal_value);
  // Joins conditions with AND.
  std::string allOf(std::vector<std::string> const& conditions);

  std::string insertStatement(std::string const& table, Assignments const& values, bool ignore_duplicates = false);
  std::string updateStatement(std::string const& table, Assignments const& values, std::string const& where);
  std::string deleteStatement(std::string const& table, std::string const& where);

  // column -> SQL literal: the values an editor sets on a row, whatever order they were collected in.
  using ColumnValues = std::map<std::string, std::string>;

  // The values that exist in `columns`, in the table's column order (statements then read like the table).
  Assignments inTableOrder(std::vector<std::string> const& columns, ColumnValues const& values);

  // Copies one row of `table` into a new row: every column in `columns` is taken from the source row
  // (alias `src`) unless `overrides` gives it a value. `source_where` selects the source row and may use
  // the alias (see sourceColumn). With `newest_first` (vmangos' "patch"), the source row with the highest
  // value of that column is the one copied.
  std::string copyRowStatement(std::string const& table,
                               std::vector<std::string> const& columns,
                               ColumnValues const& overrides,
                               std::string const& source_where,
                               std::string const& newest_first = {});
  // "src.`column`" for copyRowStatement's source_where.
  std::string sourceColumn(std::string const& column);

  // "INSERT INTO `table` (`a`, `b`) VALUES ('1', NULL), (...);" for snapshot files. Empty when no rows.
  std::string buildInsertValues(std::string const& table,
                                std::vector<std::string> const& columns,
                                std::vector<SqlRow> const& rows);

  // "`a`, `b`, `c`"
  std::string columnList(std::vector<std::string> const& columns);
  // "1, 2, 3" (sorted, duplicates removed)
  std::string idList(std::vector<std::uint32_t> ids);

  // Column lookup in a table's column list (case-sensitive, as MySQL reports names).
  bool hasColumn(std::vector<std::string> const& columns, std::string const& column);
  // The first candidate the table has, or "" (the feature is then unavailable on this schema).
  std::string firstColumn(std::vector<std::string> const& columns, std::initializer_list<char const*> candidates);
}
