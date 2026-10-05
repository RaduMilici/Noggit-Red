// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/quest/QuestScripts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>

namespace Noggit::Quest
{
  using namespace Noggit::Content;

  namespace
  {
    // vmangos dbscript commands (ScriptMgr.h), as the world database uses them.
    enum Command : std::uint32_t
    {
      TALK = 0,          // dataint = broadcast_text entry
      EMOTE = 1,         // datalong = Emotes.dbc id
      QUEST_EXPLORED = 7, // datalong = quest
      SUMMON_CREATURE = 10, // datalong = entry, datalong2 = despawn (ms), x/y/z/o
      CAST_SPELL = 15,   // datalong = spell
      CREATE_ITEM = 17,  // datalong = item, datalong2 = count
      SET_FACTION = 22,  // datalong = faction template, datalong2 = restore flags
      ATTACK_START = 26,
    };

    // Faction 14 ("Monster") is hostile to every player; restore it on respawn (flag 1).
    constexpr std::uint32_t HOSTILE_FACTION = 14;
    constexpr std::uint32_t RESTORE_ON_RESPAWN = 1;

    enum ChatType : std::uint32_t
    {
      CHAT_SAY = 0,
      CHAT_YELL = 1,
    };

    std::vector<char const*> const& requiredColumns()
    {
      static std::vector<char const*> const columns = {"id", "delay", "command", "datalong", "datalong2", "dataint",
                                                       "dataint3", "dataint4", "x", "y", "z", "o", "comments"};
      return columns;
    }

    std::string const& tableFor(ScriptSchema const& schema, ScriptWhen when)
    {
      return when == ScriptWhen::Accepted ? schema.start_table : schema.end_table;
    }

    long long number(ScriptRow const& row, char const* column)
    {
      auto const found = row.find(column);
      return found != row.end() && found->second ? std::strtoll(found->second->c_str(), nullptr, 10) : 0;
    }

    float decimal(ScriptRow const& row, char const* column)
    {
      auto const found = row.find(column);
      return found != row.end() && found->second ? std::strtof(found->second->c_str(), nullptr) : 0.0f;
    }

    std::string commentFor(ScriptAction const& action)
    {
      switch (action.kind)
      {
        case ScriptAction::Kind::Say: return "Noggit: say";
        case ScriptAction::Kind::Yell: return "Noggit: yell";
        case ScriptAction::Kind::Emote: return "Noggit: emote";
        case ScriptAction::Kind::CastSpell: return "Noggit: cast spell";
        case ScriptAction::Kind::GiveItem: return "Noggit: give item";
        case ScriptAction::Kind::SummonCreature: return "Noggit: summon creature";
        case ScriptAction::Kind::AttackPlayer: return "Noggit: attack player";
        case ScriptAction::Kind::CompleteQuest: return "Noggit: complete quest";
      }
      return "Noggit";
    }
  }

  ScriptSchema detectScriptSchema(std::function<std::vector<std::string>(std::string const&)> const& columns_of)
  {
    ScriptSchema schema;
    auto const start = columns_of("quest_start_scripts");
    auto const end = columns_of("quest_end_scripts");
    bool const layout_ok = std::all_of(requiredColumns().begin(), requiredColumns().end(), [&](char const* column)
    {
      return hasColumn(start, column) && hasColumn(end, column);
    });
    auto const texts = columns_of("broadcast_text");
    if (!layout_ok || !hasColumn(texts, "entry") || !hasColumn(texts, "male_text"))
    {
      return schema;
    }
    schema.start_table = "quest_start_scripts";
    schema.end_table = "quest_end_scripts";
    schema.columns = start;
    schema.text_entry_col = "entry";
    schema.text_male_col = "male_text";
    schema.text_female_col = firstColumn(texts, {"female_text"});
    schema.text_chat_type_col = firstColumn(texts, {"chat_type"});
    return schema;
  }

  std::vector<std::uint32_t> spokenTextIds(std::vector<ScriptRow> const& rows)
  {
    std::vector<std::uint32_t> ids;
    for (auto const& row : rows)
    {
      if (number(row, "command") == TALK && number(row, "dataint") > 0)
      {
        ids.push_back(static_cast<std::uint32_t>(number(row, "dataint")));
      }
    }
    return ids;
  }

  ParsedScript parseScript(std::vector<ScriptRow> const& rows, std::map<std::uint32_t, SpokenText> const& texts)
  {
    ParsedScript parsed;
    // Rows run by their delay (seconds from the start); keep the table order within one second.
    std::vector<ScriptRow const*> ordered;
    for (auto const& row : rows)
    {
      ordered.push_back(&row);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](ScriptRow const* a, ScriptRow const* b)
    {
      return number(*a, "delay") < number(*b, "delay");
    });

    long long previous_delay = 0;
    for (std::size_t i = 0; i < ordered.size(); ++i)
    {
      auto const& row = *ordered[i];
      long long const delay = number(row, "delay");
      ScriptAction action;
      action.wait = static_cast<std::uint32_t>(std::max(0ll, delay - previous_delay));
      auto const command = static_cast<std::uint32_t>(number(row, "command"));
      auto const datalong = static_cast<std::uint32_t>(number(row, "datalong"));
      switch (command)
      {
        case TALK:
        {
          auto const text_id = static_cast<std::uint32_t>(number(row, "dataint"));
          auto const text = texts.find(text_id);
          if (text == texts.end())
          {
            parsed.complete = false;
            continue;
          }
          action.kind = text->second.chat_type == CHAT_YELL ? ScriptAction::Kind::Yell : ScriptAction::Kind::Say;
          action.text = text->second.text;
          if (isOwnText(text_id))
          {
            parsed.custom_text_ids.push_back(text_id);
          }
          break;
        }
        case EMOTE:
          action.kind = ScriptAction::Kind::Emote;
          action.id = datalong;
          break;
        case CAST_SPELL:
          action.kind = ScriptAction::Kind::CastSpell;
          action.id = datalong;
          break;
        case CREATE_ITEM:
          action.kind = ScriptAction::Kind::GiveItem;
          action.id = datalong;
          action.count = std::max<std::uint32_t>(static_cast<std::uint32_t>(number(row, "datalong2")), 1);
          break;
        case SUMMON_CREATURE:
          action.kind = ScriptAction::Kind::SummonCreature;
          action.id = datalong;
          action.count = static_cast<std::uint32_t>(number(row, "datalong2") / 1000);
          action.x = decimal(row, "x");
          action.y = decimal(row, "y");
          action.z = decimal(row, "z");
          action.o = decimal(row, "o");
          break;
        case QUEST_EXPLORED:
          action.kind = ScriptAction::Kind::CompleteQuest;
          action.id = datalong;
          break;
        case SET_FACTION:
          // The editor's "attack" is SET_FACTION(hostile) + ATTACK_START in the same second.
          if (i + 1 < ordered.size() && number(*ordered[i + 1], "command") == ATTACK_START
              && number(*ordered[i + 1], "delay") == delay)
          {
            continue;
          }
          parsed.complete = false;
          continue;
        case ATTACK_START:
          action.kind = ScriptAction::Kind::AttackPlayer;
          break;
        default:
          parsed.complete = false;
          continue;
      }
      previous_delay = delay;
      parsed.actions.push_back(std::move(action));
    }
    return parsed;
  }

  std::vector<std::string> buildScriptStatements(ScriptSchema const& schema,
                                                 ScriptWhen when,
                                                 std::uint32_t script_id,
                                                 std::uint32_t quest,
                                                 std::vector<ScriptAction> const& actions,
                                                 std::vector<std::uint32_t> const& old_text_ids,
                                                 std::uint32_t& next_text_id)
  {
    std::vector<std::string> statements = buildScriptDeletes(schema, when, script_id, old_text_ids);
    if (!schema.valid())
    {
      return statements;
    }
    auto const& table = tableFor(schema, when);

    std::uint32_t delay = 0;
    auto const row = [&](std::uint32_t command, Assignments values, ScriptAction const& action)
    {
      Assignments full = {{"id", literal(script_id)}, {"delay", literal(delay)}, {"command", literal(command)}};
      full.insert(full.end(), values.begin(), values.end());
      full.emplace_back("comments", literal(commentFor(action)));
      statements.push_back(insertStatement(table, full));
    };

    for (auto const& action : actions)
    {
      delay += action.wait;
      switch (action.kind)
      {
        case ScriptAction::Kind::Say:
        case ScriptAction::Kind::Yell:
        {
          std::uint32_t const text_id = next_text_id++;
          Assignments text = {{schema.text_entry_col, literal(text_id)}, {schema.text_male_col, literal(action.text)}};
          if (!schema.text_female_col.empty())
          {
            text.emplace_back(schema.text_female_col, literal(action.text));
          }
          if (!schema.text_chat_type_col.empty())
          {
            text.emplace_back(schema.text_chat_type_col,
                              literal(action.kind == ScriptAction::Kind::Yell ? CHAT_YELL : CHAT_SAY));
          }
          statements.push_back(insertStatement("broadcast_text", text));
          row(TALK, {{"dataint", literal(text_id)}}, action);
          break;
        }
        case ScriptAction::Kind::Emote:
          row(EMOTE, {{"datalong", literal(action.id)}}, action);
          break;
        case ScriptAction::Kind::CastSpell:
          row(CAST_SPELL, {{"datalong", literal(action.id)}}, action);
          break;
        case ScriptAction::Kind::GiveItem:
          row(CREATE_ITEM, {{"datalong", literal(action.id)}, {"datalong2", literal(std::max<std::uint32_t>(action.count, 1))}},
              action);
          break;
        case ScriptAction::Kind::SummonCreature:
          // dataint3 = -1 / dataint4 = 1 as in every summon row of the vmangos data.
          row(SUMMON_CREATURE, {{"datalong", literal(action.id)}, {"datalong2", literal(action.count * 1000)},
                                {"dataint3", "-1"}, {"dataint4", "1"},
                                {"x", literal(action.x)}, {"y", literal(action.y)}, {"z", literal(action.z)},
                                {"o", literal(action.o)}}, action);
          break;
        case ScriptAction::Kind::AttackPlayer:
          row(SET_FACTION, {{"datalong", literal(HOSTILE_FACTION)}, {"datalong2", literal(RESTORE_ON_RESPAWN)}}, action);
          row(ATTACK_START, {}, action);
          break;
        case ScriptAction::Kind::CompleteQuest:
          row(QUEST_EXPLORED, {{"datalong", literal(quest)}}, action);
          break;
      }
    }
    return statements;
  }

  std::vector<std::string> buildScriptDeletes(ScriptSchema const& schema,
                                              ScriptWhen when,
                                              std::uint32_t script_id,
                                              std::vector<std::uint32_t> const& text_ids)
  {
    std::vector<std::string> statements;
    if (!schema.valid())
    {
      return statements;
    }
    statements.push_back(deleteStatement(tableFor(schema, when), equals("id", literal(script_id))));
    std::vector<std::uint32_t> own;
    std::copy_if(text_ids.begin(), text_ids.end(), std::back_inserter(own), [](std::uint32_t id) { return isOwnText(id); });
    if (!own.empty())
    {
      statements.push_back(deleteStatement("broadcast_text", quoteIdentifier(schema.text_entry_col) + " IN (" + idList(own) + ")"));
    }
    return statements;
  }

  bool completesQuest(std::vector<ScriptAction> const& actions)
  {
    return std::any_of(actions.begin(), actions.end(), [](ScriptAction const& action)
    {
      return action.kind == ScriptAction::Kind::CompleteQuest;
    });
  }
}
