// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// The item_template row for the item editor: new quest items ("8 Wolf Pelts"), from scratch or as a copy
// of an existing item (whose look, stats and use effects it keeps). Pure, see content/SqlText.

#include <noggit/content/SqlText.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Noggit::Item
{
  using Noggit::Content::CUSTOM_ENTRY_START;

  struct ItemSchema
  {
    std::vector<std::string> columns;
    std::string entry_col, patch_col, name_col, description_col, display_col, quality_col;
    std::string class_col, subclass_col, bonding_col, stackable_col, max_count_col;
    std::string sell_price_col, buy_price_col, start_quest_col;

    bool valid() const { return !entry_col.empty() && !name_col.empty(); }
  };

  ItemSchema detectItemSchema(std::vector<std::string> const& columns);

  // What kind of item it is: sets item_template class / subclass / bonding together.
  struct ItemKind
  {
    char const* label;
    char const* tooltip;
    std::uint32_t item_class, subclass, bonding;
  };
  std::vector<ItemKind> const& itemKinds();
  constexpr std::uint32_t BONDING_QUEST_ITEM = 4;

  struct Quality
  {
    char const* label;
    std::uint32_t id;
    std::uint32_t rgb; // the game's quality colour
  };
  std::vector<Quality> const& qualities();

  struct ItemFields
  {
    std::optional<std::string> name, description;
    std::optional<std::uint32_t> display_id, quality;
    std::optional<std::uint32_t> item_class, subclass, bonding;
    std::optional<std::uint32_t> stackable, max_count;
    std::optional<std::uint32_t> sell_price; // copper
  };

  ItemFields itemFieldsFromRow(ItemSchema const& schema, std::vector<std::optional<std::string>> const& row);

  // New item: from scratch (source 0; unset columns take the table defaults) or a copy of the source's
  // newest version (vmangos keeps one row per patch).
  std::string buildItemInsert(ItemSchema const& schema, std::uint32_t entry, ItemFields const& fields,
                              std::uint32_t source = 0);
  std::string buildItemUpdate(ItemSchema const& schema, std::uint32_t entry, ItemFields const& fields);
  std::string buildItemDelete(ItemSchema const& schema, std::uint32_t entry);

  // Where an item is used, for deleting it.
  struct ItemUses
  {
    std::vector<std::uint32_t> quests; // need it, reward it or start from it
    std::size_t loot_rows = 0;         // creature / object loot
    std::size_t vendor_rows = 0;       // sold by vendors
  };

  struct ItemDeletePlan
  {
    std::vector<std::string> statements;
    std::vector<std::string> summary;
    std::vector<std::string> problems;
  };

  // Deleting a custom item: refused while a quest uses it; its loot and vendor rows go with it.
  ItemDeletePlan planItemDelete(ItemSchema const& schema, std::uint32_t entry, ItemUses const& uses);
}
