// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/npc/NpcDialogueSql.hpp>

#include <noggit/npc/NpcTemplateSql.hpp>

namespace Noggit::Npc
{
  using namespace Noggit::Content;

  DialogueSchema detectDialogueSchema(std::function<std::vector<std::string>(std::string const&)> const& columns_of)
  {
    DialogueSchema schema;
    auto const menu = columns_of("gossip_menu");
    schema.menu_entry_col = firstColumn(menu, {"entry", "MenuID"});
    schema.menu_text_col = firstColumn(menu, {"text_id", "TextID"});
    if (schema.menu_text_col.empty())
    {
      schema.menu_entry_col.clear();
    }
    auto const texts = columns_of("npc_text");
    schema.text_id_col = firstColumn(texts, {"ID"});
    schema.text_inline_col = firstColumn(texts, {"text0_0"});
    schema.text_broadcast_col = firstColumn(texts, {"BroadcastTextID0"});
    schema.text_probability_col = firstColumn(texts, {"Probability0", "prob0"});
    auto const broadcast = columns_of("broadcast_text");
    schema.broadcast_entry_col = firstColumn(broadcast, {"entry", "ID"});
    schema.broadcast_male_col = firstColumn(broadcast, {"male_text", "MaleText"});
    schema.broadcast_female_col = firstColumn(broadcast, {"female_text", "FemaleText"});
    if (schema.broadcast_male_col.empty())
    {
      schema.broadcast_entry_col.clear();
    }
    auto const greeting = columns_of("quest_greeting");
    schema.greeting_entry_col = firstColumn(greeting, {"entry", "ID"});
    schema.greeting_type_col = firstColumn(greeting, {"type", "Type"});
    schema.greeting_text_col = firstColumn(greeting, {"content_default", "Greeting"});
    if (schema.greeting_type_col.empty() || schema.greeting_text_col.empty())
    {
      schema.greeting_entry_col.clear();
    }
    return schema;
  }

  bool ownsMenu(DialogueStored const& stored)
  {
    return stored.menu && !stored.menu_shared && isCustomText(stored.npc_text);
  }

  namespace
  {
    std::vector<std::string> menuDeletes(DialogueSchema const& schema, DialogueStored const& stored)
    {
      std::vector<std::string> statements;
      if (!ownsMenu(stored))
      {
        return statements;
      }
      statements.push_back(deleteStatement("gossip_menu", equals(schema.menu_entry_col, literal(stored.menu))));
      statements.push_back(deleteStatement("npc_text", equals(schema.text_id_col, literal(stored.npc_text))));
      if (isCustomText(stored.broadcast) && !schema.broadcast_entry_col.empty())
      {
        statements.push_back(deleteStatement("broadcast_text", equals(schema.broadcast_entry_col, literal(stored.broadcast))));
      }
      return statements;
    }

    std::vector<std::string> questGreetingStatements(DialogueSchema const& schema, std::uint32_t npc, std::string const& text)
    {
      std::vector<std::string> statements;
      if (!schema.questGreetingSupported())
      {
        return statements;
      }
      // type 0 = a creature (1 = a gameobject).
      std::string const where = allOf({equals(schema.greeting_entry_col, literal(npc)), equals(schema.greeting_type_col, "0")});
      statements.push_back(deleteStatement("quest_greeting", where));
      if (!text.empty())
      {
        statements.push_back(insertStatement("quest_greeting", {{schema.greeting_entry_col, literal(npc)},
                                                                {schema.greeting_type_col, "0"},
                                                                {schema.greeting_text_col, literal(text)}}));
      }
      return statements;
    }
  }

  DialoguePlan planDialogue(DialogueSchema const& schema,
                            TemplateSchema const& npc_schema,
                            std::uint32_t npc,
                            Dialogue const& wanted,
                            DialogueStored const& stored,
                            DialogueIds const& ids)
  {
    DialoguePlan plan;
    auto& statements = plan.statements;

    if (wanted.greeting != stored.dialogue.greeting && schema.greetingSupported() && !npc_schema.gossip_menu_col.empty())
    {
      // Replace the NPC's own menu, or stop using a shared one. A new menu is built from scratch.
      auto deletes = menuDeletes(schema, stored);
      statements.insert(statements.end(), deletes.begin(), deletes.end());
      std::uint32_t menu = 0;
      if (!wanted.greeting.empty())
      {
        menu = ownsMenu(stored) ? stored.menu : ids.next_menu;
        if (menu > MAX_MENU_ID)
        {
          plan.problems.push_back("The database has no free dialogue IDs left (gossip_menu is full).");
          return plan;
        }
        std::uint32_t const text_id = ownsMenu(stored) ? stored.npc_text : ids.next_text;
        Assignments text = {{schema.text_id_col, literal(text_id)}};
        if (!schema.text_inline_col.empty())
        {
          text.emplace_back(schema.text_inline_col, literal(wanted.greeting));
        }
        else
        {
          std::uint32_t const broadcast = ownsMenu(stored) && isCustomText(stored.broadcast) ? stored.broadcast
                                                                                                : ids.next_broadcast;
          Assignments line = {{schema.broadcast_entry_col, literal(broadcast)},
                              {schema.broadcast_male_col, literal(wanted.greeting)}};
          if (!schema.broadcast_female_col.empty())
          {
            line.emplace_back(schema.broadcast_female_col, literal(wanted.greeting));
          }
          statements.push_back(insertStatement("broadcast_text", line));
          text.emplace_back(schema.text_broadcast_col, literal(broadcast));
        }
        if (!schema.text_probability_col.empty())
        {
          text.emplace_back(schema.text_probability_col, "1");
        }
        statements.push_back(insertStatement("npc_text", text));
        statements.push_back(insertStatement("gossip_menu", {{schema.menu_entry_col, literal(menu)},
                                                             {schema.menu_text_col, literal(text_id)}}));
      }
      if (menu != stored.menu)
      {
        statements.push_back(updateStatement("creature_template", {{npc_schema.gossip_menu_col, literal(menu)}},
                                             equals(npc_schema.entry_col, literal(npc))));
      }
    }

    if (wanted.quest_greeting != stored.dialogue.quest_greeting)
    {
      auto more = questGreetingStatements(schema, npc, wanted.quest_greeting);
      statements.insert(statements.end(), more.begin(), more.end());
    }
    return plan;
  }

  std::vector<std::string> dialogueDeletes(DialogueSchema const& schema, std::uint32_t npc, DialogueStored const& stored)
  {
    auto statements = menuDeletes(schema, stored);
    auto greeting = questGreetingStatements(schema, npc, {});
    statements.insert(statements.end(), greeting.begin(), greeting.end());
    return statements;
  }
}
