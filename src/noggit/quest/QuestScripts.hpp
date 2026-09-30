// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// Scripted quest events: what happens when a quest is accepted or handed in -- the NPC says something,
// emotes, casts a spell, gives an item, summons a creature, attacks, or completes an event quest. Stored
// as vmangos dbscripts (quest_start_scripts / quest_end_scripts, keyed by quest_template.StartScript /
// CompleteScript) with spoken lines in broadcast_text. Pure, see content/SqlText.
//
// Command layouts follow the vmangos world database's own rows. Scripts run with the quest giver (or
// ender) as the source and the player as the target.

#include <noggit/content/SqlText.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Noggit::Quest
{
  using Noggit::Content::TEXT_ID_START;

  struct ScriptSchema
  {
    std::string start_table, end_table;
    std::vector<std::string> columns; // of the script tables
    std::string text_entry_col, text_male_col, text_female_col, text_chat_type_col; // broadcast_text

    bool valid() const { return !start_table.empty() && !end_table.empty() && !text_entry_col.empty(); }
  };

  // Only vmangos-layout dbscripts are recognised (CMaNGOS and AzerothCore script differently).
  ScriptSchema detectScriptSchema(std::function<std::vector<std::string>(std::string const&)> const& columns_of);

  enum class ScriptWhen
  {
    Accepted,  // quest_start_scripts
    Completed, // quest_end_scripts
  };

  struct ScriptAction
  {
    enum class Kind
    {
      Say,
      Yell,
      Emote,          // id = Emotes.dbc
      CastSpell,      // id = spell, cast on the player
      GiveItem,       // id = item, count
      SummonCreature, // id = creature, count = seconds before it despawns, at x/y/z/o
      AttackPlayer,   // the NPC turns hostile and attacks
      CompleteQuest,  // completes the quest for the player (event quests)
    };
    Kind kind = Kind::Say;
    std::uint32_t wait = 0; // seconds after the previous action
    std::string text;       // Say / Yell
    std::uint32_t id = 0;
    std::uint32_t count = 1;
    float x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f;

    bool operator==(ScriptAction const& other) const
    {
      return kind == other.kind && wait == other.wait && text == other.text && id == other.id && count == other.count
          && x == other.x && y == other.y && z == other.z && o == other.o;
    }
    bool operator!=(ScriptAction const& other) const { return !(*this == other); }
  };

  struct ParsedScript
  {
    std::vector<ScriptAction> actions;
    bool complete = true; // false: the script has steps the editor cannot show (saving replaces them)
    std::vector<std::uint32_t> custom_text_ids; // the editor's own texts it uses (replaced on save)
  };

  using ScriptRow = std::map<std::string, std::optional<std::string>>;

  struct SpokenText
  {
    std::string text;
    std::uint32_t chat_type = 0; // broadcast_text.chat_type: 0 say, 1 yell
  };

  // `texts`: broadcast_text entry -> its line, for the rows' spoken lines.
  ParsedScript parseScript(std::vector<ScriptRow> const& rows, std::map<std::uint32_t, SpokenText> const& texts);

  // The broadcast_text entries the rows speak (to look their texts up before parsing).
  std::vector<std::uint32_t> spokenTextIds(std::vector<ScriptRow> const& rows);

  // Replaces script `script_id` by `actions`. Spoken lines get new broadcast_text entries from
  // `next_text_id` up (advanced past the ones used); `old_text_ids` (the editor's texts of the previous
  // version) are removed. `quest` is the quest CompleteQuest completes.
  std::vector<std::string> buildScriptStatements(ScriptSchema const& schema,
                                                 ScriptWhen when,
                                                 std::uint32_t script_id,
                                                 std::uint32_t quest,
                                                 std::vector<ScriptAction> const& actions,
                                                 std::vector<std::uint32_t> const& old_text_ids,
                                                 std::uint32_t& next_text_id);

  // Removes a script and the editor's texts it used.
  std::vector<std::string> buildScriptDeletes(ScriptSchema const& schema,
                                              ScriptWhen when,
                                              std::uint32_t script_id,
                                              std::vector<std::uint32_t> const& text_ids);

  // True when the actions complete the quest themselves (the quest then needs the "event" flag).
  bool completesQuest(std::vector<ScriptAction> const& actions);
}
