// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// Turns an NPC editor result into the statements to write (create as a copy / edit), and plans deleting a
// custom NPC with everything that hangs off it. Pure: the database layer gathers the inputs and runs it.

#include <noggit/npc/NpcDialogueSql.hpp>
#include <noggit/npc/NpcTemplateSql.hpp>
#include <noggit/quest/QuestLinksSql.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace Noggit::Npc
{
  // Spawns of an NPC and the per-spawn tables keyed by the spawn's guid.
  struct SpawnSchema
  {
    std::string entry_col; // creature.id (mangos) / id1 (AzerothCore)
    struct GuidTable
    {
      std::string table;
      std::string guid_col;
    };
    std::vector<GuidTable> per_spawn; // creature_addon, creature_movement, game_event_creature, ...

    bool valid() const { return !entry_col.empty(); }
  };

  // Candidate per-spawn tables and their guid column; the database layer keeps the ones that exist.
  std::vector<SpawnSchema::GuidTable> const& perSpawnTableCandidates();

  struct NpcDatabase
  {
    TemplateSchema npc;
    DialogueSchema dialogue;
    SpawnSchema spawns;
    Noggit::Quest::LinkSchema quest_links;
  };

  NpcDatabase detectNpcDatabase(ColumnsOf const& columns_of);

  struct NpcContent
  {
    NpcFields fields;
    std::set<std::string> copy_related; // optional tables to copy (create)
    Dialogue dialogue;
  };

  enum class NpcSaveMode
  {
    Create, // a copy of `source`
    Edit,
  };

  struct NpcPlan
  {
    std::uint32_t entry = 0;
    std::vector<std::string> statements;
    std::vector<std::string> cleanup;
    bool require_first_row = false;
    std::vector<std::string> summary;
    std::vector<std::string> problems;
  };

  NpcPlan planNpcSave(NpcDatabase const& db,
                      NpcSaveMode mode,
                      std::uint32_t entry,
                      std::uint32_t source,
                      NpcContent const& content,
                      DialogueStored const& stored_dialogue,
                      DialogueIds const& ids);

  // Deleting a custom NPC: its template rows, dialogue, quest links, and its spawns in the world.
  NpcPlan planNpcDelete(NpcDatabase const& db,
                        std::uint32_t entry,
                        DialogueStored const& stored_dialogue,
                        std::size_t spawn_count,
                        std::size_t quest_links);
}
