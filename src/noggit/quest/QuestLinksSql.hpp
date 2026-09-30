// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// Who and what starts and ends a quest: NPCs (the yellow ! and ?), objects (a wanted poster), an item
// ("This item begins a quest"), and the area trigger that completes an exploration quest. Pure, see
// content/SqlText.

#include <noggit/content/SqlText.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Noggit::Npc
{
  struct TemplateSchema;
}

namespace Noggit::Quest
{
  // A (thing, quest) link table, e.g. creature_questrelation (id, quest).
  struct RelationTable
  {
    std::string table;
    std::string owner_col; // the NPC / object / area trigger
    std::string quest_col;

    bool valid() const { return !table.empty(); }
  };

  struct LinkSchema
  {
    RelationTable npc_starters;    // creature_questrelation / creature_queststarter
    RelationTable npc_enders;      // creature_involvedrelation / creature_questender
    RelationTable object_starters; // gameobject_questrelation / gameobject_queststarter
    RelationTable object_enders;   // gameobject_involvedrelation / gameobject_questender
    RelationTable area_triggers;   // areatrigger_involvedrelation: one quest per trigger
    // item_template column naming the quest an item starts.
    std::string item_entry_col, item_start_quest_col;
  };

  // `columns_of(table)` returns a table's columns (empty when it does not exist).
  LinkSchema detectLinkSchema(std::function<std::vector<std::string>(std::string const&)> const& columns_of);

  struct Giver
  {
    enum class Kind
    {
      Npc,
      Object,
    };
    Kind kind = Kind::Npc;
    std::uint32_t entry = 0;

    bool operator==(Giver const& other) const { return kind == other.kind && entry == other.entry; }
    bool operator<(Giver const& other) const
    {
      return kind != other.kind ? kind < other.kind : entry < other.entry;
    }
  };

  struct QuestLinks
  {
    std::vector<Giver> starters;   // offer the quest
    std::vector<Giver> enders;     // take it back
    std::uint32_t start_item = 0;  // an item that begins the quest when looted (0 = none)
    std::uint32_t area_trigger = 0; // reaching it completes the quest (exploration; 0 = none)
  };

  // Makes the quest's links exactly `links`. `previous_start_item` is the item that started the quest
  // before, which stops doing so if it changed.
  std::vector<std::string> buildLinkStatements(LinkSchema const& schema,
                                               std::uint32_t quest,
                                               QuestLinks const& links,
                                               std::uint32_t previous_start_item);

  // Removes every link of the quest (deleting it, cleanup after a failed create, snapshot prefix).
  std::vector<std::string> buildLinkDeletes(LinkSchema const& schema, std::uint32_t quest);

  // Gives these NPCs the quest-giver role (npc flag 0x2 on every supported client). Empty when none.
  constexpr std::uint32_t QUEST_GIVER_FLAG = 0x2;
  std::string buildQuestGiverFlagStatement(Noggit::Npc::TemplateSchema const& npc_schema,
                                           std::vector<std::uint32_t> const& npcs);

  // The NPC entries among `givers`.
  std::vector<std::uint32_t> npcEntries(std::vector<Giver> const& givers);
}
