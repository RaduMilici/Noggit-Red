// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// Quest item drops: making a creature (loot) or a chest-like object ("Search the crate") give an item to
// players on a quest that needs it. Pure, see content/SqlText.
//
// vmangos/CMaNGOS mark quest-only loot with a NEGATIVE ChanceOrQuestChance; AzerothCore/TrinityCore use a
// positive Chance plus QuestRequired = 1. A creature's loot table is creature_template.loot_id; a chest's
// (gameobject type 3) is gameobject_template.data1. Creatures / chests sharing a loot table all get the
// drop -- every "Young Wolf" drops it, not one of them.

#include <noggit/content/SqlText.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Noggit::Quest
{
  struct LootTableSchema
  {
    std::string table;
    std::string entry_col, item_col, chance_col, group_col, min_count_col, max_count_col;
    std::string quest_required_col; // AzerothCore/TrinityCore only

    bool valid() const { return !entry_col.empty() && !item_col.empty() && !chance_col.empty(); }
  };

  LootTableSchema detectLootTable(std::string const& table, std::vector<std::string> const& columns);

  constexpr std::uint32_t CHEST_OBJECT_TYPE = 3;

  struct LootSchema
  {
    LootTableSchema creature; // creature_loot_template
    LootTableSchema object;   // gameobject_loot_template
    std::string creature_entry_col, creature_loot_col; // creature_template entry / loot_id
    std::string object_entry_col, object_type_col, object_loot_col; // gameobject_template entry / type / data1

    bool creatureDrops() const { return creature.valid() && !creature_loot_col.empty(); }
    bool objectDrops() const { return object.valid() && !object_loot_col.empty() && !object_type_col.empty(); }
  };

  LootSchema detectLootSchema(std::vector<std::string> const& creature_loot_columns,
                              std::vector<std::string> const& object_loot_columns,
                              std::vector<std::string> const& creature_template_columns,
                              std::vector<std::string> const& object_template_columns);

  struct QuestDrop
  {
    enum class Source
    {
      Creature,
      Object, // a chest (gameobject type 3)
    };
    Source source = Source::Creature;
    std::uint32_t source_entry = 0;
    std::uint32_t item = 0;
    float chance = 50.0f;       // percent
    std::uint32_t loot_id = 0;  // the source's current loot table; 0 = it has none yet (gets its own)
  };

  // Makes the source drop the item for players on a quest that needs it. If it already drops the item
  // normally, that is kept.
  std::vector<std::string> buildDropStatements(LootSchema const& loot, QuestDrop const& drop);

  // Removes a quest-only drop (normal drops of the same item are left alone).
  std::string buildDropRemoval(LootSchema const& loot, QuestDrop::Source source, std::uint32_t loot_id,
                               std::uint32_t item);

  // WHERE condition matching the quest-only rows of a loot table (for reads and snapshots).
  std::string questOnlyCondition(LootTableSchema const& table);
}
