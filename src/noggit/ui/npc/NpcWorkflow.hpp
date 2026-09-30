// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// Creating (as a copy), editing and deleting NPCs: the NPC editor, the confirmation, the write, the
// re-runnable copy in sql_exports/npcs/. MySQL builds only.

#include <noggit/npc/NpcSavePlan.hpp>

#include <cstdint>
#include <optional>

class QWidget;

namespace Noggit::Ui::Content
{
  class ContentSession;
}

namespace Noggit::Ui::Npc
{
  struct NpcSaved
  {
    std::uint32_t entry = 0;
    Noggit::Npc::NpcContent content;          // what was written
    Noggit::Npc::NpcFields before;            // the copied / edited NPC's values
  };

  // `editing`: change `npc` (a custom NPC); otherwise create a copy of it.
  std::optional<NpcSaved> editNpc(Content::ContentSession& session, QWidget* parent, bool editing, std::uint32_t npc);

  // Deletes a custom NPC with its spawns, dialogue and quest links. True when deleted.
  bool deleteNpc(Content::ContentSession& session, QWidget* parent, std::uint32_t npc);
}
