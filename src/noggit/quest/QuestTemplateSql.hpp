// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// The quest_template row: which of its columns the quest editor maps to on this database, the editor's
// field values, and the INSERT / UPDATE / DELETE statements for the row. Pure (see content/SqlText) and
// unit-tested in test/quest.
//
// The other parts of a quest live in their own modules: who gives / takes it (QuestLinksSql), quest item
// drops (QuestLootSql), prerequisites (QuestPrerequisites), scripted events (QuestScripts); QuestSavePlan
// puts them together.
//
// Column names are read from the live table, so vmangos / CMaNGOS (Title, ReqCreatureOrGOId1) and
// AzerothCore / TrinityCore (LogTitle, RequiredNpcOrGo1) both work; a field whose column the database lacks
// is disabled in the editor.

#include <noggit/content/SqlText.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Noggit::Quest
{
  constexpr std::size_t TARGET_SLOTS = 4;     // creatures to kill / objects to use (shared slots)
  constexpr std::size_t COLLECT_SLOTS = 4;
  constexpr std::size_t REWARD_SLOTS = 4;
  constexpr std::size_t CHOICE_SLOTS = 6;
  constexpr std::size_t REPUTATION_SLOTS = 5;

  // quest_template.SpecialFlags (vmangos / CMaNGOS; AzerothCore keeps them in quest_template_addon).
  constexpr std::uint32_t SPECIAL_REPEATABLE = 0x1;
  constexpr std::uint32_t SPECIAL_EXPLORATION_OR_EVENT = 0x2; // completed by an area trigger or a script
  // quest_template.QuestFlags
  constexpr std::uint32_t FLAG_SHARABLE = 0x8;

  struct QuestSchema
  {
    std::vector<std::string> columns; // every quest_template column, in table order
    std::string entry_col;
    std::string patch_col;            // vmangos: one row per content patch, like creature_template
    std::string title_col;
    std::string details_col;          // the story the giver tells
    std::string objectives_col;       // the short "what to do" line in the quest log
    std::string request_items_col;    // said by the ender while the quest is unfinished
    std::string offer_reward_col;     // said by the ender when turning it in
    std::string level_col, min_level_col;
    std::string zone_col;
    std::string type_col, suggested_players_col, time_limit_col;
    std::string quest_flags_col, special_flags_col;
    std::string races_col, classes_col;
    std::string skill_col, skill_value_col;
    std::string min_rep_faction_col, min_rep_value_col;
    std::string max_rep_faction_col, max_rep_value_col;
    std::string rep_objective_faction_col, rep_objective_value_col;
    // Chain links: written through QuestPrerequisites, which keeps them consistent across quests.
    std::string prev_quest_col, next_quest_col, exclusive_group_col, next_in_chain_col;
    std::string source_item_col, source_item_count_col; // handed to the player on accepting
    std::string xp_col, money_col;
    std::string reward_spell_col, reward_spell_cast_col;
    std::string start_script_col, complete_script_col;
    // Per-slot columns; an empty string = that slot does not exist on this schema.
    std::array<std::string, TARGET_SLOTS> target_cols, target_count_cols, target_text_cols, target_spell_cols;
    std::array<std::string, COLLECT_SLOTS> collect_cols, collect_count_cols;
    std::array<std::string, REWARD_SLOTS> reward_cols, reward_count_cols;
    std::array<std::string, CHOICE_SLOTS> choice_cols, choice_count_cols;
    std::array<std::string, REPUTATION_SLOTS> rep_reward_faction_cols, rep_reward_value_cols;
    // Cleared on copies: scripts and chain links belong to the original quest.
    std::vector<std::string> copy_cleared_cols;

    bool valid() const { return !entry_col.empty() && !title_col.empty(); }
  };

  QuestSchema detectQuestSchema(std::vector<std::string> const& columns);

  // How many of a slot group's columns this schema has (e.g. 4 target slots on vmangos).
  template<std::size_t N>
  int slotCount(std::array<std::string, N> const& columns)
  {
    int count = 0;
    for (auto const& column : columns)
    {
      count += !column.empty();
    }
    return count;
  }

  // Something to kill (a creature) or to use (an object), `count` times. Stored in ReqCreatureOrGOId*,
  // objects as NEGATIVE entries.
  struct Target
  {
    enum class Kind
    {
      Creature,
      Object,
    };
    Kind kind = Kind::Creature;
    std::uint32_t id = 0;
    std::uint32_t count = 1;
    std::string text;        // custom quest log line; empty = "<name> slain" / "<name> used"
    std::uint32_t spell = 0; // the target only counts when this spell is cast on it (0 = killed / used)
  };

  // An item and how many (collect objectives, rewards, the item handed out on accepting).
  struct ItemCount
  {
    std::uint32_t id = 0;
    std::uint32_t count = 1;
  };

  // A faction (Faction.dbc ID) and a reputation value (e.g. 250 points, or 9000 = Honored).
  struct Reputation
  {
    std::uint32_t faction = 0;
    std::int32_t value = 0;
  };

  // The editor's quest_template fields. std::nullopt = leave the column as it is (copy: take the source's).
  // Slot lists are written whole: slots beyond the list are cleared.
  struct QuestFields
  {
    std::optional<std::string> title, details, objectives, request_items, offer_reward;
    std::optional<std::uint32_t> level, min_level;
    std::optional<std::int32_t> zone;
    std::optional<std::uint32_t> type, suggested_players, time_limit; // time limit in seconds
    std::optional<std::uint32_t> quest_flags, special_flags;
    std::optional<std::uint32_t> races, classes;
    std::optional<std::uint32_t> skill, skill_value;
    std::optional<Reputation> min_rep, max_rep, rep_objective;
    std::optional<std::int32_t> prev_quest, next_quest, exclusive_group;
    std::optional<std::uint32_t> next_in_chain;
    std::optional<ItemCount> source_item;
    std::optional<std::uint32_t> xp;
    std::optional<std::int32_t> money; // copper; negative = the quest costs money
    std::optional<std::uint32_t> reward_spell, reward_spell_cast;
    std::optional<std::uint32_t> start_script, complete_script;
    std::optional<std::vector<Target>> targets;
    std::optional<std::vector<ItemCount>> collect, rewards, choices;
    std::optional<std::vector<Reputation>> rep_rewards;
  };

  // Current values of a quest row, given (column -> value) as read from the database.
  QuestFields questFieldsFromRow(QuestSchema const& schema,
                                 std::map<std::string, std::optional<std::string>> const& row);

  // New quest. source_entry = 0: from scratch (unset columns take the table defaults); otherwise a copy
  // of that quest's newest version with scripts and chain links cleared (unless `fields` sets them).
  std::string buildQuestInsert(QuestSchema const& schema,
                               std::uint32_t new_entry,
                               QuestFields const& fields,
                               std::uint32_t source_entry = 0);

  // UPDATE for an existing quest. Empty when no field is set.
  std::string buildQuestUpdate(QuestSchema const& schema, std::uint32_t entry, QuestFields const& fields);

  // Removes the quest_template row.
  std::string buildQuestRowDelete(QuestSchema const& schema, std::uint32_t entry);

  // Typical XP for a quest level, from (level -> XP of existing quests). 0 when there is no data.
  std::uint32_t suggestedXp(std::map<std::uint32_t, std::vector<std::uint32_t>> const& xp_by_level,
                            std::uint32_t level);
}
