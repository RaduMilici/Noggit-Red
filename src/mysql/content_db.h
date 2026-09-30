// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// The database side of the content editors (NPCs, quests, items): reading what the editors show, the
// facts their save plans need, re-runnable snapshots, and running the plans. The SQL itself is built by
// the pure modules (noggit/npc, quest, item); this layer only reads and executes.

#include <mysql/mysql.h>
#include <noggit/content/SqlText.hpp>
#include <noggit/item/ItemTemplateSql.hpp>
#include <noggit/npc/NpcSavePlan.hpp>
#include <noggit/quest/QuestSavePlan.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mysql::content
{
  using Noggit::Content::SqlRow;

  // One connection for a series of reads and writes (an editor makes dozens), with a cache of table
  // layouts. Open it for one dialog / one save; not thread-safe.
  class Database
  {
  public:
    // Null with *error set when the project has no reachable database.
    static std::unique_ptr<Database> open(std::string* error = nullptr);
    ~Database();

    // A table's columns in table order; empty when the table does not exist.
    std::vector<std::string> const& columnsOf(std::string const& table);
    // columnsOf() as a callback, for the detect*() functions of the pure modules.
    Noggit::Npc::ColumnsOf columns();

    std::optional<std::vector<SqlRow>> rows(std::string const& sql, std::string* error = nullptr);
    // The first column of the first row as a number; 0 when there is none.
    std::uint32_t number(std::string const& sql);
    // First free value of `column` at or above `start` across the given (table, column) pairs.
    std::uint32_t nextFreeId(std::vector<std::pair<std::string, std::string>> const& columns, std::uint32_t start);

    // Runs statements in order, in a transaction. When a create fails part-way, `cleanup` removes what was
    // written (MyISAM tables cannot roll back) -- only after the first statement succeeded, so nothing that
    // existed before is touched. With require_first_row, a first statement that writes no row fails.
    SqlScriptResult execute(std::vector<std::string> const& statements,
                            std::vector<std::string> const& cleanup = {},
                            bool require_first_row = false);

    // "INSERT INTO ... VALUES ..." recreating the rows of `table` matching `where` ("" when none).
    std::string dumpRows(std::string const& table, std::string const& where);

  private:
    Database();
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };

  // --- lists for the pickers ---

  struct NamedEntry
  {
    std::uint32_t entry = 0;
    std::string name;
    std::uint32_t kind = 0;    // items: quality; objects: gameobject type; creatures: npc flags
    std::uint32_t display = 0; // items: display id (for the icon)
  };
  std::vector<NamedEntry> creatures(Database& db); // kind: npc flags
  std::vector<NamedEntry> items(Database& db);
  std::vector<NamedEntry> objects(Database& db);
  std::vector<NamedEntry> spells(Database& db); // empty when the world database has no spell table

  struct AreaTrigger
  {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t map = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f, radius = 0.0f;
    std::uint32_t quest = 0; // the quest it completes now
    bool other_use = false;  // teleporter, inn, scripted event, battleground entrance
  };
  std::vector<AreaTrigger> areaTriggers(Database& db);

  struct QuestLink
  {
    Noggit::Quest::Giver giver;
    std::uint32_t quest = 0;
    bool starts = false; // gives the quest (true) or takes it back
  };
  struct QuestList
  {
    std::vector<Noggit::Quest::ChainQuest> quests; // one per quest (newest patch version), chain columns included
    std::vector<QuestLink> links;
  };
  bool questList(Database& db, QuestList& out, std::string* error = nullptr);

  // --- NPCs ---

  struct NpcEditorData
  {
    Noggit::Npc::NpcDatabase db;
    Noggit::Npc::NpcFields values;   // every field the schema maps
    std::string script_name;
    std::map<std::string, std::size_t> related_rows; // rows the NPC has in each related table
    Noggit::Npc::DialogueStored dialogue;
    Noggit::Npc::DialogueIds dialogue_ids;
    std::uint32_t next_entry = 0;
    std::size_t spawns = 0;
    std::size_t quest_links = 0;
  };
  bool loadNpc(Database& db, std::uint32_t entry, NpcEditorData& out, std::string* error = nullptr);
  std::string npcSnapshot(Database& db, Noggit::Npc::NpcDatabase const& schema, std::uint32_t entry);

  // --- quests ---

  struct QuestEditorData
  {
    Noggit::Quest::QuestDatabase db;
    Noggit::Quest::QuestContent content; // as stored; empty for a new quest
    Noggit::Quest::QuestStored stored;
    std::uint32_t next_entry = 0;
    std::map<std::uint32_t, std::vector<std::uint32_t>> xp_by_level; // XP of the game's quests, per level
  };
  // entry 0: a new quest (schemas and IDs only).
  bool loadQuest(Database& db, std::uint32_t entry, QuestList const& list, QuestEditorData& out,
                 std::string* error = nullptr);

  // The facts planQuestSave needs about what `content` touches. Fills in the drops' current loot tables.
  Noggit::Quest::SaveInputs saveInputs(Database& db, QuestEditorData const& data, QuestList const& list,
                                       Noggit::Quest::QuestContent& content);

  std::string questSnapshot(Database& db, Noggit::Quest::QuestDatabase const& schema, std::uint32_t entry);

  // --- items ---

  struct ItemEditorData
  {
    Noggit::Item::ItemSchema schema;
    Noggit::Item::ItemFields values; // entry 0: empty
    std::uint32_t next_entry = 0;
    Noggit::Item::ItemUses uses;
  };
  bool loadItem(Database& db, std::uint32_t entry, ItemEditorData& out, std::string* error = nullptr);
  std::string itemSnapshot(Database& db, Noggit::Item::ItemSchema const& schema, std::uint32_t entry);
}
