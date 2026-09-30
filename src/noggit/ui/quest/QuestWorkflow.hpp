// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// Creating, copying, editing and deleting quests: loads the quest, runs the editor, plans the save
// (noggit/quest/QuestSavePlan), asks for confirmation, writes it, and refreshes the re-runnable copies in
// sql_exports/quests/. MySQL builds only.

#include <noggit/ui/quest/QuestPage.hpp>

#include <cstdint>
#include <map>

class QWidget;

namespace Noggit::Ui::Content
{
  class ContentSession;
}

namespace Noggit::Ui::Quest
{
  // Returns the saved quest's ID; 0 when cancelled or failed. `source`: the quest copied / edited;
  // `follows`: a new quest made to follow that one; `default_npc`: the new quest's giver and ender.
  std::uint32_t editQuest(Content::ContentSession& session, QWidget* parent, EditorMode mode, std::uint32_t source,
                          std::uint32_t follows = 0, std::uint32_t default_npc = 0);

  // Deletes a quest made with the editor, and every chain link other quests have to it.
  bool deleteQuest(Content::ContentSession& session, QWidget* parent, std::uint32_t quest);

  // Writes the chain links the chain editor changed. True when written.
  bool saveChainChanges(Content::ContentSession& session, QWidget* parent,
                        std::map<std::uint32_t, Noggit::Quest::ChainQuest> const& changes);
}
