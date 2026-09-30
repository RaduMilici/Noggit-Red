// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// SQL for the "Create NPC" / "Edit NPC" tool: cloning a creature_template row (plus the per-entry tables
// that hang off it) into a new custom entry, and editing the handful of fields the tool exposes.
//
// Everything here is PURE -- no MySQL client dependency -- so it is unit-tested on its own
// (test/npc_template). mysql.cpp reads the live schema, feeds it in here, and executes the result.
//
// Cloning copies the source row with INSERT ... SELECT over the column list read from the live database,
// so every column the tool does not know about (stats, loot, spells, immunities, ...) is carried over
// unchanged on every core (vmangos / Turtle / CMaNGOS / AzerothCore / TrinityCore).

#include <noggit/content/SqlText.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Noggit::Npc
{
  // A per-entry table cloned alongside creature_template (vendor items, trainer spells, ...).
  struct RelatedTable
  {
    std::string table;
    std::string key_column;           // column holding the creature entry
    std::vector<std::string> columns; // every column, in table order
    std::string label;                // user-facing, e.g. "items sold"
    bool optional = false;            // offered as a checkbox; otherwise always part of the clone
  };

  // Which creature_template columns the tool's fields map to on THIS database. Empty = the schema has
  // no such column (the field is then disabled in the dialog and never written).
  struct TemplateSchema
  {
    std::vector<std::string> columns; // every creature_template column, in table order
    std::string entry_col;
    std::string name_col;
    std::string subname_col;
    std::string level_min_col, level_max_col;
    std::vector<std::string> faction_cols; // CMaNGOS keeps two (FactionAlliance/FactionHorde): both set
    std::string npc_flags_col;
    std::string rank_col;
    std::string script_col;
    std::string gossip_menu_col; // what the NPC says when clicked (see NpcDialogueSql)
    // Model: first display slot + the other slots and their weights (vmangos display_id1..4 /
    // display_probability1..4). Changing the model points slot 1 at it and clears the others, so the
    // NPC always uses exactly the chosen model.
    std::string display_col;
    std::vector<std::string> other_display_cols;
    std::string display_probability_col;
    std::vector<std::string> other_display_probability_cols;
    std::string display_total_probability_col;
    std::string scale_col;
    // vmangos keeps one creature_template row per content patch ((entry, patch) primary key). Clones
    // take the newest version and become a single patch-0 row, so they exist on every patch.
    std::string patch_col;
    std::vector<RelatedTable> related;

    bool valid() const { return !entry_col.empty() && !name_col.empty(); }
  };

  // Picks the tool's columns out of creature_template's column list (case-sensitive, as MySQL reports
  // them). Related tables are filled in separately (see relatedTableCandidates).
  TemplateSchema detectTemplateSchema(std::vector<std::string> const& columns);

  // Per-entry tables worth cloning, as (table, key column candidates, label). mysql.cpp keeps the ones
  // that exist with one of the key columns.
  struct RelatedTableCandidate
  {
    char const* table;
    std::vector<char const*> key_columns;
    char const* label;
    bool optional;
  };
  std::vector<RelatedTableCandidate> const& relatedTableCandidates();

  // A table's columns, in table order (empty when the table does not exist). The database layer reads
  // them from INFORMATION_SCHEMA; tests pass fixed lists.
  using ColumnsOf = std::function<std::vector<std::string>(std::string const&)>;

  // creature_template's schema plus the related per-entry tables this database has.
  TemplateSchema detectNpcSchema(ColumnsOf const& columns_of);

  // The fields the tool edits. std::nullopt = leave the column as it is (clone: copy from the source).
  struct NpcFields
  {
    std::optional<std::string> name;
    std::optional<std::string> subname;
    std::optional<std::uint32_t> level_min, level_max;
    std::optional<std::uint32_t> faction;
    std::optional<std::uint32_t> npc_flags;
    std::optional<std::uint32_t> rank;
    std::optional<std::uint32_t> display_id;
    std::optional<float> scale;
    bool clear_script = false; // blank the script name (clones must not run the source's boss script)
    std::optional<std::uint32_t> gossip_menu;
  };

  // Current values of a creature_template row, given (column -> value) as read from the database. Every
  // field the schema maps is set.
  NpcFields npcFieldsFromRow(TemplateSchema const& schema, std::map<std::string, std::optional<std::string>> const& row);

  struct CloneRequest
  {
    std::uint32_t source_entry = 0;
    std::uint32_t new_entry = 0;
    NpcFields fields;
    std::set<std::string> copy_related; // optional related tables to copy (required ones always are)
  };

  // Statements that create the clone, in execution order. [0] is the creature_template INSERT; the
  // rest copy related rows. The creature_template row is written first, so a failure there leaves
  // nothing behind; a later failure is undone with buildDeleteStatements (MyISAM cannot roll back).
  std::vector<std::string> buildCloneStatements(TemplateSchema const& schema,
                                                CloneRequest const& request);

  // UPDATE for an existing entry. Empty string when no field is set.
  std::string buildUpdateStatement(TemplateSchema const& schema,
                                   std::uint32_t entry,
                                   NpcFields const& fields);

  // Removes an entry from creature_template and every related table (clone cleanup, and the
  // "replace what is there" prefix of the snapshot file).
  std::vector<std::string> buildDeleteStatements(TemplateSchema const& schema, std::uint32_t entry);

  // NPC role flags (creature_template npc flags). The bit values differ between the vanilla and the
  // 3.3.5a clients.
  struct NpcRole
  {
    char const* label;
    char const* tooltip;
    std::uint32_t flag;
  };
  std::vector<NpcRole> const& npcRoles(bool vanilla);

  struct RankOption
  {
    char const* label;
    std::uint32_t rank;
  };
  std::vector<RankOption> const& rankOptions();
}
