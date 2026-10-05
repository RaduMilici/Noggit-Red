// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/quest/QuestLinksSql.hpp>

#include <set>

namespace Noggit::Quest
{
  using namespace Noggit::Content;

  namespace
  {
    RelationTable pick(std::function<std::vector<std::string>(std::string const&)> const& columns_of,
                       std::initializer_list<RelationTable> candidates)
    {
      for (auto const& candidate : candidates)
      {
        auto const columns = columns_of(candidate.table);
        if (hasColumn(columns, candidate.owner_col) && hasColumn(columns, candidate.quest_col))
        {
          return candidate;
        }
      }
      return {};
    }

    std::vector<std::string> replaceLinks(RelationTable const& relation, std::uint32_t quest,
                                          std::vector<std::uint32_t> const& owners)
    {
      std::vector<std::string> statements;
      if (!relation.valid())
      {
        return statements;
      }
      statements.push_back(deleteStatement(relation.table, equals(relation.quest_col, literal(quest))));
      for (auto const owner : std::set<std::uint32_t>(owners.begin(), owners.end()))
      {
        statements.push_back(insertStatement(relation.table, {{relation.owner_col, literal(owner)},
                                                              {relation.quest_col, literal(quest)}}));
      }
      return statements;
    }

    std::vector<std::uint32_t> entriesOf(std::vector<Giver> const& givers, Giver::Kind kind)
    {
      std::vector<std::uint32_t> out;
      for (auto const& giver : givers)
      {
        if (giver.kind == kind)
        {
          out.push_back(giver.entry);
        }
      }
      return out;
    }
  }

  LinkSchema detectLinkSchema(std::function<std::vector<std::string>(std::string const&)> const& columns_of)
  {
    LinkSchema schema;
    schema.npc_starters = pick(columns_of, {{"creature_questrelation", "id", "quest"}, {"creature_queststarter", "id", "quest"}});
    schema.npc_enders = pick(columns_of, {{"creature_involvedrelation", "id", "quest"}, {"creature_questender", "id", "quest"}});
    schema.object_starters = pick(columns_of, {{"gameobject_questrelation", "id", "quest"}, {"gameobject_queststarter", "id", "quest"}});
    schema.object_enders = pick(columns_of, {{"gameobject_involvedrelation", "id", "quest"}, {"gameobject_questender", "id", "quest"}});
    schema.area_triggers = pick(columns_of, {{"areatrigger_involvedrelation", "id", "quest"}});
    auto const item_columns = columns_of("item_template");
    schema.item_entry_col = firstColumn(item_columns, {"entry"});
    schema.item_start_quest_col = firstColumn(item_columns, {"start_quest", "startquest", "StartQuest"});
    return schema;
  }

  std::vector<std::string> buildLinkStatements(LinkSchema const& schema,
                                               std::uint32_t quest,
                                               QuestLinks const& links,
                                               std::uint32_t previous_start_item)
  {
    std::vector<std::string> statements;
    auto const append = [&](std::vector<std::string> more)
    {
      statements.insert(statements.end(), more.begin(), more.end());
    };
    append(replaceLinks(schema.npc_starters, quest, entriesOf(links.starters, Giver::Kind::Npc)));
    append(replaceLinks(schema.npc_enders, quest, entriesOf(links.enders, Giver::Kind::Npc)));
    append(replaceLinks(schema.object_starters, quest, entriesOf(links.starters, Giver::Kind::Object)));
    append(replaceLinks(schema.object_enders, quest, entriesOf(links.enders, Giver::Kind::Object)));
    if (schema.area_triggers.valid())
    {
      append(replaceLinks(schema.area_triggers, quest,
                          links.area_trigger ? std::vector<std::uint32_t>{links.area_trigger} : std::vector<std::uint32_t>{}));
    }
    if (!schema.item_start_quest_col.empty())
    {
      if (previous_start_item && previous_start_item != links.start_item)
      {
        statements.push_back(updateStatement("item_template", {{schema.item_start_quest_col, "0"}},
                                             allOf({equals(schema.item_entry_col, literal(previous_start_item)),
                                                    equals(schema.item_start_quest_col, literal(quest))})));
      }
      if (links.start_item)
      {
        statements.push_back(updateStatement("item_template", {{schema.item_start_quest_col, literal(quest)}},
                                             equals(schema.item_entry_col, literal(links.start_item))));
      }
    }
    return statements;
  }

  std::vector<std::string> buildLinkDeletes(LinkSchema const& schema, std::uint32_t quest)
  {
    std::vector<std::string> statements;
    for (auto const* relation : {&schema.npc_starters, &schema.npc_enders, &schema.object_starters,
                                 &schema.object_enders, &schema.area_triggers})
    {
      if (relation->valid())
      {
        statements.push_back(deleteStatement(relation->table, equals(relation->quest_col, literal(quest))));
      }
    }
    if (!schema.item_start_quest_col.empty())
    {
      statements.push_back(updateStatement("item_template", {{schema.item_start_quest_col, "0"}},
                                           equals(schema.item_start_quest_col, literal(quest))));
    }
    return statements;
  }

  NpcFlagSchema detectNpcFlagSchema(std::vector<std::string> const& creature_template_columns)
  {
    return {firstColumn(creature_template_columns, {"entry", "Entry"}),
            firstColumn(creature_template_columns, {"npc_flags", "npcflag", "NpcFlags"})};
  }

  std::string buildQuestGiverFlagStatement(NpcFlagSchema const& npc_schema, std::vector<std::uint32_t> const& npcs)
  {
    if (npcs.empty() || !npc_schema.valid())
    {
      return {};
    }
    auto const flags = quoteIdentifier(npc_schema.npc_flags_col);
    return "UPDATE `creature_template` SET " + flags + " = " + flags + " | " + std::to_string(QUEST_GIVER_FLAG)
         + " WHERE " + quoteIdentifier(npc_schema.entry_col) + " IN (" + idList(npcs) + ")";
  }

  std::vector<std::uint32_t> npcEntries(std::vector<Giver> const& givers)
  {
    return entriesOf(givers, Giver::Kind::Npc);
  }
}
