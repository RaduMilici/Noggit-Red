// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// Turns one quest editor result into everything that has to be written -- the quest row, its links, item
// drops, prerequisites (which can change other quests), scripted events, and the quest-giver role -- as an
// ordered list of statements with a cleanup list and a plain-language summary. Also plans deleting a
// quest. Pure: the database layer gathers the facts it needs (SaveInputs) and runs the result.

#include <noggit/quest/QuestChain.hpp>
#include <noggit/quest/QuestLinksSql.hpp>
#include <noggit/quest/QuestLootSql.hpp>
#include <noggit/quest/QuestScripts.hpp>
#include <noggit/quest/QuestTemplateSql.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Noggit::Quest
{
  // The quest-related tables of the connected database.
  struct QuestDatabase
  {
    QuestSchema quest;
    LinkSchema links;
    LootSchema loot;
    ScriptSchema scripts;
    NpcFlagSchema npc;
  };

  // A table's columns, in table order (empty when the table does not exist).
  using ColumnsOf = std::function<std::vector<std::string>(std::string const&)>;

  QuestDatabase detectQuestDatabase(ColumnsOf const& columns_of);

  // Everything one quest consists of, as the editor shows it.
  struct QuestContent
  {
    QuestFields fields;
    QuestLinks links;
    std::vector<QuestDrop> drops;               // quest-only drops of the collect items
    Prerequisites prerequisites;
    std::vector<std::uint32_t> exclusive_with;  // either/or partners
    std::uint32_t offers_next = 0;              // offered right after this one is handed in
    std::vector<ScriptAction> on_accept, on_complete;
    bool scripts_complete = true;               // false: the stored scripts have steps the editor cannot show
  };

  // What the database holds for a quest besides its content (the loaded version of an edit / copy).
  struct QuestStored
  {
    std::set<std::uint32_t> shared_items;         // collect items other quests need too: never un-dropped
    std::vector<std::uint32_t> accept_text_ids;   // the editor's spoken lines in the stored scripts
    std::vector<std::uint32_t> complete_text_ids;
  };

  // Names for the summary.
  struct NameLookup
  {
    std::function<std::string(std::uint32_t)> npc, object, item, quest;
  };

  // Facts about other rows the plan depends on.
  struct SaveInputs
  {
    std::vector<ChainQuest> chain;                          // every quest's chain columns
    std::map<std::uint32_t, std::uint32_t> npc_flags;       // of the NPCs the quest links (missing = unknown)
    std::map<std::uint32_t, std::uint32_t> trigger_quests;  // area trigger -> the quest it completes now
    std::uint32_t next_text_id = OWN_TEXT_START;
    NameLookup names;
  };

  enum class SaveMode
  {
    Create,
    Copy,
    Edit,
  };

  struct SavePlan
  {
    std::uint32_t quest = 0;
    std::vector<std::string> statements;
    std::vector<std::string> cleanup;           // undoes a partly written create / copy
    bool require_first_row = false;             // the first statement must write a row (copy of a vanished quest)
    std::vector<std::string> summary;           // one sentence per consequence, for the confirmation
    std::vector<std::string> problems;          // non-empty: the save is refused
    std::set<std::uint32_t> quests_changed;     // their re-runnable copies need refreshing
    std::vector<std::uint32_t> new_quest_givers;
  };

  SavePlan planQuestSave(QuestDatabase const& db,
                         SaveMode mode,
                         std::uint32_t entry,        // the quest written (new ID for create / copy)
                         std::uint32_t source,       // the quest copied (copy only)
                         QuestContent const& content,
                         QuestContent const& before, // the stored content (edit / copy), empty for create
                         QuestStored const& stored,
                         SaveInputs const& inputs);

  // Deleting a quest made with the editor: its row, links, scripts, the drops only it needed, and every chain link other
  // quests have to it.
  SavePlan planQuestDelete(QuestDatabase const& db,
                           std::uint32_t entry,
                           QuestContent const& content,
                           QuestStored const& stored,
                           SaveInputs const& inputs);

  // Removes the quest's own rows (row, links, scripts): the prefix of its re-runnable copy.
  std::vector<std::string> questRowsDelete(QuestDatabase const& db, std::uint32_t entry, QuestStored const& stored);

  // A user-facing sentence for a refused chain change.
  std::string describe(ChainResult const& result, NameLookup const& names);
}
