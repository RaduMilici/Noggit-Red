// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// What an NPC says: the greeting in its chat window when clicked (creature_template.gossip_menu_id ->
// gossip_menu -> npc_text -> broadcast_text on vmangos, inline npc_text.text0_0 elsewhere) and the line
// above its quest list (quest_greeting). Pure, see content/SqlText.
//
// Copy-on-write: a menu or text that anything else uses -- e.g. the game NPC a clone was copied from -- is
// never changed; the NPC gets its own instead. Only rows the editor made (IDs from customIds().text_start up) are
// changed or deleted.

#include <noggit/content/SqlText.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Noggit::Npc
{
  struct TemplateSchema;

  struct DialogueSchema
  {
    std::string menu_entry_col, menu_text_col;          // gossip_menu (entry, text_id)
    std::string text_id_col;                            // npc_text.ID
    std::string text_broadcast_col, text_probability_col; // vmangos: BroadcastTextID0 / Probability0
    std::string text_inline_col;                        // CMaNGOS/AzerothCore: text0_0
    std::string broadcast_entry_col, broadcast_male_col, broadcast_female_col;
    std::string greeting_entry_col, greeting_type_col, greeting_text_col; // quest_greeting

    bool greetingSupported() const
    {
      return !menu_entry_col.empty() && !text_id_col.empty()
          && (!text_inline_col.empty() || (!text_broadcast_col.empty() && !broadcast_entry_col.empty()));
    }
    bool questGreetingSupported() const { return !greeting_entry_col.empty(); }
  };

  DialogueSchema detectDialogueSchema(std::function<std::vector<std::string>(std::string const&)> const& columns_of);

  // gossip_menu.entry is a SMALLINT on vmangos: menus cannot use the 90000+ range and simply take the next
  // free ID; the texts behind them do.
  constexpr std::uint32_t MAX_MENU_ID = 65535;

  struct Dialogue
  {
    std::string greeting;       // the chat window text (empty: the NPC has none)
    std::string quest_greeting; // above the quest list (empty: none)
  };

  // The NPC's dialogue as stored.
  struct DialogueStored
  {
    Dialogue dialogue;
    std::uint32_t menu = 0;         // creature_template.gossip_menu_id
    std::uint32_t npc_text = 0;     // its first text
    std::uint32_t broadcast = 0;    // that text's broadcast_text (vmangos)
    bool menu_shared = false;       // another NPC uses the menu, or it has several texts / options
  };

  struct DialogueIds
  {
    std::uint32_t next_menu = 0;      // first free gossip_menu entry
    std::uint32_t next_text = 0;      // first free npc_text ID >= customIds().text_start
    std::uint32_t next_broadcast = 0; // first free broadcast_text entry >= customIds().text_start
  };

  struct DialoguePlan
  {
    std::vector<std::string> statements;
    std::vector<std::string> problems;
  };

  // Makes the NPC say `wanted`. Sets creature_template.gossip_menu_id when the NPC gets a new menu.
  DialoguePlan planDialogue(DialogueSchema const& schema,
                            TemplateSchema const& npc_schema,
                            std::uint32_t npc,
                            Dialogue const& wanted,
                            DialogueStored const& stored,
                            DialogueIds const& ids);

  // Removes the dialogue rows only this NPC used (deleting the NPC).
  std::vector<std::string> dialogueDeletes(DialogueSchema const& schema, std::uint32_t npc, DialogueStored const& stored);

  // True when the menu and its text are the editor's own and only this NPC uses them.
  bool ownsMenu(DialogueStored const& stored);
}
