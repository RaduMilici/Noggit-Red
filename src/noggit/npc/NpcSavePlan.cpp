// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/npc/NpcSavePlan.hpp>

namespace Noggit::Npc
{
  using namespace Noggit::Content;

  std::vector<SpawnSchema::GuidTable> const& perSpawnTableCandidates()
  {
    static std::vector<SpawnSchema::GuidTable> const candidates = {
      {"creature_addon", "guid"},
      {"creature_movement", "id"},
      {"game_event_creature", "guid"},
      {"pool_creature", "guid"},
      {"creature_linking", "guid"},
      {"creature_battleground", "guid"},
      {"game_event_creature_data", "guid"},
      {"creature_groups", "member_guid"},
      {"npc_gossip", "npc_guid"}, // tortoise-wow: per-spawn dialogue override
    };
    return candidates;
  }

  NpcDatabase detectNpcDatabase(ColumnsOf const& columns_of)
  {
    NpcDatabase db;
    db.npc = detectNpcSchema(columns_of);
    db.dialogue = detectDialogueSchema(columns_of);
    db.quest_links = Noggit::Quest::detectLinkSchema(columns_of);
    auto const spawn_columns = columns_of("creature");
    if (hasColumn(spawn_columns, "guid"))
    {
      db.spawns.entry_col = firstColumn(spawn_columns, {"id", "id1"});
      for (auto const& candidate : perSpawnTableCandidates())
      {
        if (hasColumn(columns_of(candidate.table), candidate.guid_col))
        {
          db.spawns.per_spawn.push_back(candidate);
        }
      }
    }
    return db;
  }

  NpcPlan planNpcSave(NpcDatabase const& db,
                      NpcSaveMode mode,
                      std::uint32_t entry,
                      std::uint32_t source,
                      NpcContent const& content,
                      DialogueStored const& stored_dialogue,
                      DialogueIds const& ids)
  {
    NpcPlan plan;
    plan.entry = entry;
    if (mode == NpcSaveMode::Create)
    {
      CloneRequest request;
      request.source_entry = source;
      request.new_entry = entry;
      request.fields = content.fields;
      request.copy_related = content.copy_related;
      plan.statements = buildCloneStatements(db.npc, request);
      plan.require_first_row = true;
      plan.cleanup = buildDeleteStatements(db.npc, entry);
    }
    else if (auto update = buildUpdateStatement(db.npc, entry, content.fields); !update.empty())
    {
      plan.statements.push_back(std::move(update));
    }

    auto const dialogue = planDialogue(db.dialogue, db.npc, entry, content.dialogue, stored_dialogue, ids);
    plan.problems = dialogue.problems;
    plan.statements.insert(plan.statements.end(), dialogue.statements.begin(), dialogue.statements.end());
    if (mode == NpcSaveMode::Create && content.dialogue.greeting != stored_dialogue.dialogue.greeting
        && !content.dialogue.greeting.empty())
    {
      // The copy's own new menu, should the create fail after writing it.
      DialogueStored created;
      created.menu = ids.next_menu;
      created.npc_text = ids.next_text;
      created.broadcast = ids.next_broadcast;
      auto deletes = dialogueDeletes(db.dialogue, entry, created);
      plan.cleanup.insert(plan.cleanup.end(), deletes.begin(), deletes.end());
    }
    return plan;
  }

  NpcPlan planNpcDelete(NpcDatabase const& db,
                        std::uint32_t entry,
                        DialogueStored const& stored_dialogue,
                        std::size_t spawn_count,
                        std::size_t quest_links)
  {
    NpcPlan plan;
    plan.entry = entry;
    if (!isCustom(entry))
    {
      plan.problems.push_back("Only NPCs made with the NPC editor can be deleted.");
      return plan;
    }

    // Spawns first: the per-spawn tables find them through creature.id.
    if (db.spawns.valid())
    {
      std::string const spawn_guids = "SELECT `guid` FROM `creature` WHERE " + equals(db.spawns.entry_col, literal(entry));
      for (auto const& table : db.spawns.per_spawn)
      {
        plan.statements.push_back(deleteStatement(table.table, quoteIdentifier(table.guid_col) + " IN (" + spawn_guids + ")"));
      }
      plan.statements.push_back(deleteStatement("creature", equals(db.spawns.entry_col, literal(entry))));
      if (spawn_count)
      {
        plan.summary.push_back("Its " + std::to_string(spawn_count) + " spawn(s) in the world are removed too.");
      }
    }
    for (auto const* relation : {&db.quest_links.npc_starters, &db.quest_links.npc_enders})
    {
      if (relation->valid())
      {
        plan.statements.push_back(deleteStatement(relation->table, equals(relation->owner_col, literal(entry))));
      }
    }
    if (quest_links)
    {
      plan.summary.push_back("It stops giving / taking " + std::to_string(quest_links) + " quest(s).");
    }
    auto dialogue = dialogueDeletes(db.dialogue, entry, stored_dialogue);
    plan.statements.insert(plan.statements.end(), dialogue.begin(), dialogue.end());
    auto rows = buildDeleteStatements(db.npc, entry);
    plan.statements.insert(plan.statements.end(), rows.begin(), rows.end());
    return plan;
  }
}
