// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/quest/QuestTemplateSql.hpp>

#include <algorithm>
#include <cstdlib>

namespace Noggit::Quest
{
  using namespace Noggit::Content;

  namespace
  {
    // Fills slot columns "<prefix>1".."<prefix>N" from the first prefix whose "1" column exists.
    template<std::size_t N>
    void slotColumns(std::vector<std::string> const& columns,
                     std::array<std::string, N>& out,
                     std::initializer_list<char const*> prefixes)
    {
      for (auto const* prefix : prefixes)
      {
        if (!hasColumn(columns, std::string(prefix) + "1"))
        {
          continue;
        }
        for (std::size_t i = 0; i < N; ++i)
        {
          std::string const column = std::string(prefix) + std::to_string(i + 1);
          out[i] = hasColumn(columns, column) ? column : std::string();
        }
        return;
      }
    }

    // Distributes `items` over the slots this schema has, in order; the remaining slots are cleared.
    template<std::size_t N, typename T>
    std::array<std::optional<T>, N> placeInSlots(std::array<std::string, N> const& id_cols, std::vector<T> const& items)
    {
      std::array<std::optional<T>, N> placed;
      std::size_t next = 0;
      for (auto const& item : items)
      {
        while (next < N && id_cols[next].empty())
        {
          ++next;
        }
        if (next >= N)
        {
          break;
        }
        placed[next++] = item;
      }
      return placed;
    }

    class ValueWriter
    {
    public:
      explicit ValueWriter(ColumnValues& out) : _out(out) {}

      template<typename T>
      void set(std::string const& column, std::optional<T> const& value)
      {
        if (!column.empty() && value)
        {
          _out[column] = literal(*value);
        }
      }

      template<typename T>
      void set(std::string const& column, T const& value)
      {
        if (!column.empty())
        {
          _out[column] = literal(value);
        }
      }

      void reputation(std::string const& faction_col, std::string const& value_col, std::optional<Reputation> const& rep)
      {
        if (rep)
        {
          set(faction_col, rep->faction);
          set(value_col, rep->faction ? rep->value : 0);
        }
      }

    private:
      ColumnValues& _out;
    };

    ColumnValues fieldValues(QuestSchema const& schema, QuestFields const& fields)
    {
      ColumnValues out;
      ValueWriter w(out);
      w.set(schema.title_col, fields.title);
      w.set(schema.details_col, fields.details);
      w.set(schema.objectives_col, fields.objectives);
      w.set(schema.request_items_col, fields.request_items);
      w.set(schema.offer_reward_col, fields.offer_reward);
      w.set(schema.level_col, fields.level);
      w.set(schema.min_level_col, fields.min_level);
      w.set(schema.zone_col, fields.zone);
      w.set(schema.type_col, fields.type);
      w.set(schema.suggested_players_col, fields.suggested_players);
      w.set(schema.time_limit_col, fields.time_limit);
      w.set(schema.quest_flags_col, fields.quest_flags);
      w.set(schema.special_flags_col, fields.special_flags);
      w.set(schema.races_col, fields.races);
      w.set(schema.classes_col, fields.classes);
      w.set(schema.skill_col, fields.skill);
      w.set(schema.skill_value_col, fields.skill_value);
      w.reputation(schema.min_rep_faction_col, schema.min_rep_value_col, fields.min_rep);
      w.reputation(schema.max_rep_faction_col, schema.max_rep_value_col, fields.max_rep);
      w.reputation(schema.rep_objective_faction_col, schema.rep_objective_value_col, fields.rep_objective);
      w.set(schema.prev_quest_col, fields.prev_quest);
      w.set(schema.next_quest_col, fields.next_quest);
      w.set(schema.exclusive_group_col, fields.exclusive_group);
      w.set(schema.next_in_chain_col, fields.next_in_chain);
      if (fields.source_item)
      {
        w.set(schema.source_item_col, fields.source_item->id);
        w.set(schema.source_item_count_col, fields.source_item->id ? std::max<std::uint32_t>(fields.source_item->count, 1) : 0u);
      }
      w.set(schema.xp_col, fields.xp);
      w.set(schema.money_col, fields.money);
      w.set(schema.reward_spell_col, fields.reward_spell);
      w.set(schema.reward_spell_cast_col, fields.reward_spell_cast);
      w.set(schema.start_script_col, fields.start_script);
      w.set(schema.complete_script_col, fields.complete_script);

      if (fields.targets)
      {
        auto const placed = placeInSlots(schema.target_cols, *fields.targets);
        for (std::size_t i = 0; i < TARGET_SLOTS; ++i)
        {
          Target const target = placed[i].value_or(Target{Target::Kind::Creature, 0, 0, {}, 0});
          std::int32_t const id = target.kind == Target::Kind::Object ? -static_cast<std::int32_t>(target.id)
                                                                      : static_cast<std::int32_t>(target.id);
          w.set(schema.target_cols[i], id);
          w.set(schema.target_count_cols[i], target.id ? std::max<std::uint32_t>(target.count, 1) : 0u);
          w.set(schema.target_text_cols[i], target.id ? target.text : std::string());
          w.set(schema.target_spell_cols[i], target.id ? target.spell : 0u);
        }
      }
      auto const item_slots = [&](auto const& id_cols, auto const& count_cols, std::optional<std::vector<ItemCount>> const& items)
      {
        if (!items)
        {
          return;
        }
        auto const placed = placeInSlots(id_cols, *items);
        for (std::size_t i = 0; i < placed.size(); ++i)
        {
          ItemCount const item = placed[i].value_or(ItemCount{0, 0});
          w.set(id_cols[i], item.id);
          w.set(count_cols[i], item.id ? std::max<std::uint32_t>(item.count, 1) : 0u);
        }
      };
      item_slots(schema.collect_cols, schema.collect_count_cols, fields.collect);
      item_slots(schema.reward_cols, schema.reward_count_cols, fields.rewards);
      item_slots(schema.choice_cols, schema.choice_count_cols, fields.choices);
      if (fields.rep_rewards)
      {
        auto const placed = placeInSlots(schema.rep_reward_faction_cols, *fields.rep_rewards);
        for (std::size_t i = 0; i < REPUTATION_SLOTS; ++i)
        {
          Reputation const rep = placed[i].value_or(Reputation{});
          w.set(schema.rep_reward_faction_cols[i], rep.faction);
          w.set(schema.rep_reward_value_cols[i], rep.faction ? rep.value : 0);
        }
      }
      return out;
    }
  }

  QuestSchema detectQuestSchema(std::vector<std::string> const& columns)
  {
    auto const first = [&](std::initializer_list<char const*> candidates) { return firstColumn(columns, candidates); };

    QuestSchema schema;
    schema.columns = columns;
    schema.entry_col = first({"entry", "ID", "Id"});
    schema.patch_col = first({"patch"});
    schema.title_col = first({"Title", "LogTitle"});
    schema.details_col = first({"Details", "QuestDescription"});
    schema.objectives_col = first({"Objectives", "LogDescription"});
    // AzerothCore/TrinityCore keep these two in quest_request_items / quest_offer_reward instead.
    schema.request_items_col = first({"RequestItemsText"});
    schema.offer_reward_col = first({"OfferRewardText"});
    schema.level_col = first({"QuestLevel"});
    schema.min_level_col = first({"MinLevel"});
    schema.zone_col = first({"ZoneOrSort", "QuestSortID"});
    schema.type_col = first({"Type", "QuestInfoID"});
    schema.suggested_players_col = first({"SuggestedPlayers", "SuggestedGroupNum"});
    schema.time_limit_col = first({"LimitTime", "TimeAllowed"});
    schema.quest_flags_col = first({"QuestFlags", "Flags"});
    schema.special_flags_col = first({"SpecialFlags"});
    schema.races_col = first({"RequiredRaces", "AllowableRaces"});
    // AzerothCore/TrinityCore keep AllowableClasses, skills, reputation requirements and the chain links
    // in quest_template_addon: those fields are not editable there.
    schema.classes_col = first({"RequiredClasses", "AllowableClasses"});
    schema.skill_col = first({"RequiredSkill"});
    schema.skill_value_col = first({"RequiredSkillValue"});
    schema.min_rep_faction_col = first({"RequiredMinRepFaction"});
    schema.min_rep_value_col = first({"RequiredMinRepValue"});
    schema.max_rep_faction_col = first({"RequiredMaxRepFaction"});
    schema.max_rep_value_col = first({"RequiredMaxRepValue"});
    schema.rep_objective_faction_col = first({"RepObjectiveFaction"});
    schema.rep_objective_value_col = first({"RepObjectiveValue"});
    schema.prev_quest_col = first({"PrevQuestId"});
    schema.next_quest_col = first({"NextQuestId"});
    schema.exclusive_group_col = first({"ExclusiveGroup"});
    schema.next_in_chain_col = first({"NextQuestInChain", "RewardNextQuest"});
    schema.source_item_col = first({"SrcItemId", "StartItem"});
    schema.source_item_count_col = first({"SrcItemCount"});
    schema.xp_col = first({"RewXP"});
    schema.money_col = first({"RewOrReqMoney", "RewardMoney"});
    schema.reward_spell_col = first({"RewSpell", "RewardDisplaySpell"});
    schema.reward_spell_cast_col = first({"RewSpellCast", "RewardSpell"});
    schema.start_script_col = first({"StartScript"});
    schema.complete_script_col = first({"CompleteScript"});
    slotColumns(columns, schema.target_cols, {"ReqCreatureOrGOId", "RequiredNpcOrGo"});
    slotColumns(columns, schema.target_count_cols, {"ReqCreatureOrGOCount", "RequiredNpcOrGoCount"});
    slotColumns(columns, schema.target_text_cols, {"ObjectiveText"});
    slotColumns(columns, schema.target_spell_cols, {"ReqSpellCast"});
    slotColumns(columns, schema.collect_cols, {"ReqItemId", "RequiredItemId"});
    slotColumns(columns, schema.collect_count_cols, {"ReqItemCount", "RequiredItemCount"});
    slotColumns(columns, schema.reward_cols, {"RewItemId", "RewardItem"});
    slotColumns(columns, schema.reward_count_cols, {"RewItemCount", "RewardAmount"});
    slotColumns(columns, schema.choice_cols, {"RewChoiceItemId", "RewardChoiceItemID"});
    slotColumns(columns, schema.choice_count_cols, {"RewChoiceItemCount", "RewardChoiceItemQuantity"});
    slotColumns(columns, schema.rep_reward_faction_cols, {"RewRepFaction", "RewardFactionID"});
    slotColumns(columns, schema.rep_reward_value_cols, {"RewRepValue", "RewardFactionValue"});
    for (auto const* column : {"StartScript", "CompleteScript", "PrevQuestId", "NextQuestId", "ExclusiveGroup",
                               "NextQuestInChain", "RewardNextQuest", "BreadcrumbForQuestId"})
    {
      if (hasColumn(columns, column))
      {
        schema.copy_cleared_cols.emplace_back(column);
      }
    }
    return schema;
  }

  QuestFields questFieldsFromRow(QuestSchema const& schema,
                                 std::map<std::string, std::optional<std::string>> const& row)
  {
    auto const raw = [&](std::string const& column) -> std::optional<std::string>
    {
      if (column.empty())
      {
        return std::nullopt;
      }
      auto const found = row.find(column);
      if (found == row.end())
      {
        return std::nullopt;
      }
      return found->second ? found->second : std::optional<std::string>(std::string());
    };
    auto const number = [&](std::string const& column) -> std::optional<long long>
    {
      auto const value = raw(column);
      return value ? std::optional<long long>(std::strtoll(value->c_str(), nullptr, 10)) : std::nullopt;
    };
    auto const as_unsigned = [&](std::string const& column) -> std::optional<std::uint32_t>
    {
      auto const value = number(column);
      return value ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*value)) : std::nullopt;
    };
    auto const as_signed = [&](std::string const& column) -> std::optional<std::int32_t>
    {
      auto const value = number(column);
      return value ? std::optional<std::int32_t>(static_cast<std::int32_t>(*value)) : std::nullopt;
    };
    auto const reputation = [&](std::string const& faction_col, std::string const& value_col) -> std::optional<Reputation>
    {
      auto const faction = as_unsigned(faction_col);
      return faction ? std::optional<Reputation>(Reputation{*faction, as_signed(value_col).value_or(0)}) : std::nullopt;
    };
    auto const items = [&](auto const& id_cols, auto const& count_cols)
    {
      std::vector<ItemCount> out;
      for (std::size_t i = 0; i < id_cols.size(); ++i)
      {
        if (auto const id = as_unsigned(id_cols[i]).value_or(0))
        {
          out.push_back({id, as_unsigned(count_cols[i]).value_or(1)});
        }
      }
      return out;
    };

    QuestFields fields;
    fields.title = raw(schema.title_col);
    fields.details = raw(schema.details_col);
    fields.objectives = raw(schema.objectives_col);
    fields.request_items = raw(schema.request_items_col);
    fields.offer_reward = raw(schema.offer_reward_col);
    fields.level = as_unsigned(schema.level_col);
    fields.min_level = as_unsigned(schema.min_level_col);
    fields.zone = as_signed(schema.zone_col);
    fields.type = as_unsigned(schema.type_col);
    fields.suggested_players = as_unsigned(schema.suggested_players_col);
    fields.time_limit = as_unsigned(schema.time_limit_col);
    fields.quest_flags = as_unsigned(schema.quest_flags_col);
    fields.special_flags = as_unsigned(schema.special_flags_col);
    fields.races = as_unsigned(schema.races_col);
    fields.classes = as_unsigned(schema.classes_col);
    fields.skill = as_unsigned(schema.skill_col);
    fields.skill_value = as_unsigned(schema.skill_value_col);
    fields.min_rep = reputation(schema.min_rep_faction_col, schema.min_rep_value_col);
    fields.max_rep = reputation(schema.max_rep_faction_col, schema.max_rep_value_col);
    fields.rep_objective = reputation(schema.rep_objective_faction_col, schema.rep_objective_value_col);
    fields.prev_quest = as_signed(schema.prev_quest_col);
    fields.next_quest = as_signed(schema.next_quest_col);
    fields.exclusive_group = as_signed(schema.exclusive_group_col);
    fields.next_in_chain = as_unsigned(schema.next_in_chain_col);
    if (auto const id = as_unsigned(schema.source_item_col))
    {
      fields.source_item = ItemCount{*id, as_unsigned(schema.source_item_count_col).value_or(*id ? 1u : 0u)};
    }
    fields.xp = as_unsigned(schema.xp_col);
    fields.money = as_signed(schema.money_col);
    fields.reward_spell = as_unsigned(schema.reward_spell_col);
    fields.reward_spell_cast = as_unsigned(schema.reward_spell_cast_col);
    fields.start_script = as_unsigned(schema.start_script_col);
    fields.complete_script = as_unsigned(schema.complete_script_col);

    std::vector<Target> targets;
    for (std::size_t i = 0; i < TARGET_SLOTS; ++i)
    {
      auto const id = as_signed(schema.target_cols[i]).value_or(0);
      if (!id)
      {
        continue;
      }
      Target target;
      target.kind = id < 0 ? Target::Kind::Object : Target::Kind::Creature;
      target.id = static_cast<std::uint32_t>(id < 0 ? -id : id);
      target.count = as_unsigned(schema.target_count_cols[i]).value_or(1);
      target.text = raw(schema.target_text_cols[i]).value_or(std::string());
      target.spell = as_unsigned(schema.target_spell_cols[i]).value_or(0);
      targets.push_back(std::move(target));
    }
    fields.targets = std::move(targets);
    fields.collect = items(schema.collect_cols, schema.collect_count_cols);
    fields.rewards = items(schema.reward_cols, schema.reward_count_cols);
    fields.choices = items(schema.choice_cols, schema.choice_count_cols);
    std::vector<Reputation> rep_rewards;
    for (std::size_t i = 0; i < REPUTATION_SLOTS; ++i)
    {
      if (auto const faction = as_unsigned(schema.rep_reward_faction_cols[i]).value_or(0))
      {
        rep_rewards.push_back({faction, as_signed(schema.rep_reward_value_cols[i]).value_or(0)});
      }
    }
    fields.rep_rewards = std::move(rep_rewards);
    return fields;
  }

  std::string buildQuestInsert(QuestSchema const& schema,
                               std::uint32_t new_entry,
                               QuestFields const& fields,
                               std::uint32_t source_entry)
  {
    if (!schema.valid())
    {
      return {};
    }
    auto values = fieldValues(schema, fields);
    if (source_entry)
    {
      for (auto const& column : schema.copy_cleared_cols)
      {
        values.emplace(column, "0"); // unless the editor set it
      }
    }
    values[schema.entry_col] = literal(new_entry);
    if (!schema.patch_col.empty())
    {
      values[schema.patch_col] = "0";
    }
    if (!source_entry)
    {
      return insertStatement("quest_template", inTableOrder(schema.columns, values));
    }
    return copyRowStatement("quest_template", schema.columns, values,
                            sourceColumn(schema.entry_col) + " = " + literal(source_entry), schema.patch_col);
  }

  std::string buildQuestUpdate(QuestSchema const& schema, std::uint32_t entry, QuestFields const& fields)
  {
    if (!schema.valid())
    {
      return {};
    }
    return updateStatement("quest_template", inTableOrder(schema.columns, fieldValues(schema, fields)),
                           equals(schema.entry_col, literal(entry)));
  }

  std::string buildQuestRowDelete(QuestSchema const& schema, std::uint32_t entry)
  {
    return schema.valid() ? deleteStatement("quest_template", equals(schema.entry_col, literal(entry))) : std::string();
  }

  std::uint32_t suggestedXp(std::map<std::uint32_t, std::vector<std::uint32_t>> const& xp_by_level,
                            std::uint32_t level)
  {
    auto found = xp_by_level.find(level);
    if (found == xp_by_level.end() || found->second.empty())
    {
      return 0;
    }
    // Median, rounded to 5 like Blizzard's numbers.
    auto values = found->second;
    std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
    std::uint32_t const median = values[values.size() / 2];
    return (median + 2) / 5 * 5;
  }
}
