// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// How quests depend on each other, and the rules for changing that. Used by the quest editor (one
// quest's prerequisites) and the chain editor (the whole graph). Pure, unit-tested in test/quest.
//
// The database encodes it in four quest_template columns (vmangos / CMaNGOS, the classic mangos rules):
//  - PrevQuestId = A           : needs A done (a single prerequisite; A may be a game quest).
//  - A.NextQuestId = C         : A is one of C's prerequisites; with several, C needs ANY one of them...
//  - ...ExclusiveGroup < 0     : ...or ALL of them, when they share the same negative group.
//  - ExclusiveGroup > 0        : quests sharing it are either/or -- doing one closes the others.
//  - A.NextQuestInChain = C    : handing A in offers C right away.
// A quest has one of each column, so it can unlock one quest through NextQuestId and belong to one group.
// Only custom quests are ever changed.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Noggit::Quest
{
  struct ChainQuest
  {
    std::uint32_t entry = 0;
    std::string title;
    std::uint32_t level = 0;
    std::int32_t prev_quest = 0;      // > 0 needs it done; < 0 needs it active (left alone)
    std::int32_t next_quest = 0;      // NextQuestId
    std::int32_t exclusive_group = 0;
    std::uint32_t next_in_chain = 0;
    bool editable = false;            // a custom quest
  };

  struct Prerequisites
  {
    enum class Mode
    {
      Any, // one of them is enough
      All, // every one of them
    };
    std::vector<std::uint32_t> quests;
    Mode mode = Mode::Any;
  };

  struct ChainLink
  {
    std::uint32_t from = 0;
    std::uint32_t to = 0;
    bool needs_done = false; // "to" needs "from" done first
    bool offers = false;     // handing "from" in offers "to"
  };

  // Why a change was refused (nothing is changed then).
  enum class ChainProblem
  {
    None,
    UnknownQuest,
    SameQuest,
    NotEditable,          // `quest` is a game quest and would have to change
    UnlocksAnotherQuest,  // `quest` already unlocks another quest (it has one NextQuestId)
    InAnotherGroup,       // `quest` is already in an either/or or all-of group
    WouldLoop,            // `quest` already depends on the other one
  };

  struct ChainResult
  {
    ChainProblem problem = ChainProblem::None;
    std::uint32_t quest = 0; // the quest the problem is about

    explicit operator bool() const { return problem == ChainProblem::None; }
  };

  class ChainModel
  {
  public:
    explicit ChainModel(std::vector<ChainQuest> quests);

    ChainQuest const* find(std::uint32_t entry) const;

    // --- dependencies ---
    Prerequisites prerequisitesOf(std::uint32_t quest) const;
    // Replaces the quest's prerequisites. One prerequisite is stored on the quest itself (any quest,
    // game quests included); several need every one of them to be a custom quest.
    ChainResult setPrerequisites(std::uint32_t quest, Prerequisites const& prerequisites);
    // Adds / removes one prerequisite, keeping the others and the mode.
    ChainResult addPrerequisite(std::uint32_t quest, std::uint32_t prerequisite,
                                Prerequisites::Mode mode_if_several = Prerequisites::Mode::All);
    ChainResult removePrerequisite(std::uint32_t quest, std::uint32_t prerequisite);

    // --- either/or groups ---
    // The other quests that close when this one is done.
    std::vector<std::uint32_t> exclusiveWith(std::uint32_t quest) const;
    // Makes these quests either/or (at least two, all custom). They leave any group they were in.
    ChainResult makeExclusive(std::vector<std::uint32_t> const& quests);
    ChainResult leaveExclusiveGroup(std::uint32_t quest);

    // --- "offer next right away" ---
    ChainResult setOffers(std::uint32_t from, std::uint32_t to); // to = 0: offers nothing

    // --- graph views ---
    // Every quest connected to `entry` through any link, `entry` included.
    std::set<std::uint32_t> chainOf(std::uint32_t entry) const;
    std::vector<ChainLink> links(std::set<std::uint32_t> const& shown) const;
    // Column (0 = leftmost) for each shown quest: the longest link path leading to it.
    std::map<std::uint32_t, int> layers(std::set<std::uint32_t> const& shown) const;

    // --- changes ---
    // Quests whose chain columns differ from the loaded values, with their new values.
    std::map<std::uint32_t, ChainQuest> changes() const;
    bool hasChanges() const { return !changes().empty(); }

  private:
    ChainQuest* findMutable(std::uint32_t entry);
    bool dependsOn(std::uint32_t quest, std::uint32_t other) const;
    std::vector<std::uint32_t> unlockedBy(std::uint32_t quest) const; // quests with NextQuestId = quest
    std::vector<std::uint32_t> groupMembers(std::int32_t group) const;

    std::map<std::uint32_t, ChainQuest> _quests;
    std::map<std::uint32_t, ChainQuest> _original;
  };
}
