// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/item/ItemTemplateSql.hpp>

#include <cstdlib>

namespace Noggit::Item
{
  using namespace Noggit::Content;

  namespace
  {
    ColumnValues fieldValues(ItemSchema const& schema, ItemFields const& fields)
    {
      ColumnValues out;
      auto const set = [&](std::string const& column, auto const& value)
      {
        if (!column.empty() && value)
        {
          out[column] = literal(*value);
        }
      };
      set(schema.name_col, fields.name);
      set(schema.description_col, fields.description);
      set(schema.display_col, fields.display_id);
      set(schema.quality_col, fields.quality);
      set(schema.class_col, fields.item_class);
      set(schema.subclass_col, fields.subclass);
      set(schema.bonding_col, fields.bonding);
      set(schema.stackable_col, fields.stackable);
      set(schema.max_count_col, fields.max_count);
      set(schema.sell_price_col, fields.sell_price);
      if (fields.sell_price && !schema.buy_price_col.empty())
      {
        out[schema.buy_price_col] = literal(*fields.sell_price * 4); // vendors sell at 4x what they pay
      }
      return out;
    }
  }

  ItemSchema detectItemSchema(std::vector<std::string> const& columns)
  {
    auto const first = [&](std::initializer_list<char const*> candidates) { return firstColumn(columns, candidates); };
    ItemSchema schema;
    schema.columns = columns;
    schema.entry_col = first({"entry"});
    schema.patch_col = first({"patch"});
    schema.name_col = first({"name"});
    schema.description_col = first({"description"});
    schema.display_col = first({"display_id", "displayid"});
    schema.quality_col = first({"quality", "Quality"});
    schema.class_col = first({"class"});
    schema.subclass_col = first({"subclass"});
    schema.bonding_col = first({"bonding"});
    schema.stackable_col = first({"stackable"});
    schema.max_count_col = first({"max_count", "maxcount"});
    schema.sell_price_col = first({"sell_price", "SellPrice"});
    schema.buy_price_col = first({"buy_price", "BuyPrice"});
    schema.start_quest_col = first({"start_quest", "startquest"});
    return schema;
  }

  std::vector<ItemKind> const& itemKinds()
  {
    // item_template class / subclass (ItemClass.dbc) and bonding.
    static std::vector<ItemKind> const kinds = {
      {"Quest item", "Only useful for a quest: binds when picked up, cannot be sold, shows \"Quest Item\".",
       12, 0, BONDING_QUEST_ITEM},
      {"Trade goods", "Materials like cloth or meat: can be traded and sold to vendors.", 7, 0, 0},
      {"Junk", "Grey vendor trash.", 15, 0, 0},
      {"Consumable", "Food, potions and other items that are used up.", 0, 0, 0},
    };
    return kinds;
  }

  std::vector<Quality> const& qualities()
  {
    static std::vector<Quality> const list = {
      {"Poor", 0, 0x9d9d9d}, {"Common", 1, 0xffffff}, {"Uncommon", 2, 0x1eff00},
      {"Rare", 3, 0x0070dd}, {"Epic", 4, 0xa335ee}, {"Legendary", 5, 0xff8000},
    };
    return list;
  }

  ItemFields itemFieldsFromRow(ItemSchema const& schema, std::vector<std::optional<std::string>> const& row)
  {
    auto const value = [&](std::string const& column) -> std::optional<std::string>
    {
      for (std::size_t i = 0; i < schema.columns.size() && i < row.size(); ++i)
      {
        if (schema.columns[i] == column)
        {
          return row[i] ? row[i] : std::optional<std::string>(std::string());
        }
      }
      return std::nullopt;
    };
    auto const number = [&](std::string const& column) -> std::optional<std::uint32_t>
    {
      auto const text = value(column);
      return text ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(std::strtoul(text->c_str(), nullptr, 10)))
                  : std::nullopt;
    };
    ItemFields fields;
    fields.name = value(schema.name_col);
    fields.description = value(schema.description_col);
    fields.display_id = number(schema.display_col);
    fields.quality = number(schema.quality_col);
    fields.item_class = number(schema.class_col);
    fields.subclass = number(schema.subclass_col);
    fields.bonding = number(schema.bonding_col);
    fields.stackable = number(schema.stackable_col);
    fields.max_count = number(schema.max_count_col);
    fields.sell_price = number(schema.sell_price_col);
    return fields;
  }

  std::string buildItemInsert(ItemSchema const& schema, std::uint32_t entry, ItemFields const& fields, std::uint32_t source)
  {
    if (!schema.valid())
    {
      return {};
    }
    auto values = fieldValues(schema, fields);
    values[schema.entry_col] = literal(entry);
    if (!schema.patch_col.empty())
    {
      values[schema.patch_col] = "0";
    }
    if (!schema.start_quest_col.empty())
    {
      values[schema.start_quest_col] = "0"; // quest links are set by the quest editor
    }
    if (!source)
    {
      return insertStatement("item_template", inTableOrder(schema.columns, values));
    }
    return copyRowStatement("item_template", schema.columns, values,
                            sourceColumn(schema.entry_col) + " = " + literal(source), schema.patch_col);
  }

  std::string buildItemUpdate(ItemSchema const& schema, std::uint32_t entry, ItemFields const& fields)
  {
    if (!schema.valid())
    {
      return {};
    }
    return updateStatement("item_template", inTableOrder(schema.columns, fieldValues(schema, fields)),
                           equals(schema.entry_col, literal(entry)));
  }

  std::string buildItemDelete(ItemSchema const& schema, std::uint32_t entry)
  {
    return schema.valid() ? deleteStatement("item_template", equals(schema.entry_col, literal(entry))) : std::string();
  }

  ItemDeletePlan planItemDelete(ItemSchema const& schema, std::uint32_t entry, ItemUses const& uses)
  {
    ItemDeletePlan plan;
    if (!isCustom(entry))
    {
      plan.problems.push_back("Only items made with the item editor can be deleted.");
      return plan;
    }
    if (!uses.quests.empty())
    {
      std::string list;
      for (auto const quest : uses.quests)
      {
        list += (list.empty() ? "#" : ", #") + std::to_string(quest);
      }
      plan.problems.push_back("Quests still use this item (" + list + "). Remove it from them first.");
      return plan;
    }
    for (auto const* table : {"creature_loot_template", "gameobject_loot_template"})
    {
      plan.statements.push_back(deleteStatement(table, equals("item", literal(entry))));
    }
    plan.statements.push_back(deleteStatement("npc_vendor", equals("item", literal(entry))));
    plan.statements.push_back(buildItemDelete(schema, entry));
    if (uses.loot_rows)
    {
      plan.summary.push_back("It stops dropping from " + std::to_string(uses.loot_rows) + " loot table(s).");
    }
    if (uses.vendor_rows)
    {
      plan.summary.push_back(std::to_string(uses.vendor_rows) + " vendor(s) stop selling it.");
    }
    return plan;
  }
}
