// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/quest/QuestLootSql.hpp>

#include <algorithm>

namespace Noggit::Quest
{
  using namespace Noggit::Content;

  LootTableSchema detectLootTable(std::string const& table, std::vector<std::string> const& columns)
  {
    LootTableSchema loot;
    if (columns.empty())
    {
      return loot;
    }
    loot.table = table;
    loot.entry_col = firstColumn(columns, {"entry", "Entry"});
    loot.item_col = firstColumn(columns, {"item", "Item"});
    loot.chance_col = firstColumn(columns, {"ChanceOrQuestChance", "Chance"});
    loot.group_col = firstColumn(columns, {"groupid", "GroupId"});
    loot.min_count_col = firstColumn(columns, {"mincountOrRef", "MinCount"});
    loot.max_count_col = firstColumn(columns, {"maxcount", "MaxCount"});
    loot.quest_required_col = firstColumn(columns, {"QuestRequired"});
    return loot;
  }

  LootSchema detectLootSchema(std::vector<std::string> const& creature_loot_columns,
                              std::vector<std::string> const& object_loot_columns,
                              std::vector<std::string> const& creature_template_columns,
                              std::vector<std::string> const& object_template_columns)
  {
    LootSchema loot;
    loot.creature = detectLootTable("creature_loot_template", creature_loot_columns);
    loot.object = detectLootTable("gameobject_loot_template", object_loot_columns);
    loot.creature_entry_col = firstColumn(creature_template_columns, {"entry", "Entry"});
    loot.creature_loot_col = firstColumn(creature_template_columns, {"loot_id", "LootId", "lootid"});
    loot.object_entry_col = firstColumn(object_template_columns, {"entry", "Entry"});
    loot.object_type_col = firstColumn(object_template_columns, {"type", "Type"});
    loot.object_loot_col = firstColumn(object_template_columns, {"data1", "Data1"});
    return loot;
  }

  std::string questOnlyCondition(LootTableSchema const& table)
  {
    return table.quest_required_col.empty() ? quoteIdentifier(table.chance_col) + " < 0"
                                            : equals(table.quest_required_col, "1");
  }

  namespace
  {
    LootTableSchema const& tableOf(LootSchema const& loot, QuestDrop::Source source)
    {
      return source == QuestDrop::Source::Creature ? loot.creature : loot.object;
    }
  }

  std::string buildDropRemoval(LootSchema const& loot, QuestDrop::Source source, std::uint32_t loot_id,
                               std::uint32_t item)
  {
    auto const& table = tableOf(loot, source);
    return deleteStatement(table.table, allOf({equals(table.entry_col, literal(loot_id)),
                                               equals(table.item_col, literal(item)),
                                               questOnlyCondition(table)}));
  }

  std::vector<std::string> buildDropStatements(LootSchema const& loot, QuestDrop const& drop)
  {
    std::vector<std::string> statements;
    bool const creature = drop.source == QuestDrop::Source::Creature;
    if (!drop.item || !drop.source_entry || !(creature ? loot.creatureDrops() : loot.objectDrops()))
    {
      return statements;
    }
    auto const& table = tableOf(loot, drop.source);

    // A source without a loot table gets its own, keyed by its entry.
    std::uint32_t const loot_id = drop.loot_id ? drop.loot_id : drop.source_entry;
    if (!drop.loot_id)
    {
      if (creature)
      {
        statements.push_back(updateStatement("creature_template", {{loot.creature_loot_col, literal(loot_id)}},
                                             allOf({equals(loot.creature_entry_col, literal(drop.source_entry)),
                                                    equals(loot.creature_loot_col, "0")})));
      }
      else
      {
        statements.push_back(updateStatement("gameobject_template", {{loot.object_loot_col, literal(loot_id)}},
                                             allOf({equals(loot.object_entry_col, literal(drop.source_entry)),
                                                    equals(loot.object_type_col, literal(CHEST_OBJECT_TYPE)),
                                                    equals(loot.object_loot_col, "0")})));
      }
    }
    statements.push_back(buildDropRemoval(loot, drop.source, loot_id, drop.item));

    float const chance = std::clamp(drop.chance, 0.01f, 100.0f);
    Assignments values = {{table.entry_col, literal(loot_id)}, {table.item_col, literal(drop.item)}};
    values.emplace_back(table.chance_col, literal(table.quest_required_col.empty() ? -chance : chance));
    if (!table.quest_required_col.empty()) values.emplace_back(table.quest_required_col, "1");
    if (!table.group_col.empty()) values.emplace_back(table.group_col, "0");
    if (!table.min_count_col.empty()) values.emplace_back(table.min_count_col, "1");
    if (!table.max_count_col.empty()) values.emplace_back(table.max_count_col, "1");
    // IGNORE: if the source already drops the item normally (same key), keep that drop.
    statements.push_back(insertStatement(table.table, values, /*ignore_duplicates*/ true));
    return statements;
  }
}
