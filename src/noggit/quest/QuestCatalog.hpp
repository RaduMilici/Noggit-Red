// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// Game constants the quest editor offers by name: races, classes, quest types, professions, reputation
// ranks, emotes. Pure data. Values that differ between the vanilla (1.12) and 3.3.5a clients take a
// `vanilla` flag.

#include <cstdint>
#include <vector>

namespace Noggit::Quest
{
  struct Named
  {
    char const* label;
    std::uint32_t id;
  };

  // Race masks for "who can take it".
  std::uint32_t allianceRaces(bool vanilla);
  std::uint32_t hordeRaces(bool vanilla);

  // Playable classes; a class mask has bit (id - 1) set for each class.
  struct PlayerClass
  {
    char const* name;
    std::uint32_t id;
    std::uint32_t mask() const { return 1u << (id - 1); }
  };
  std::vector<PlayerClass> const& playerClasses(bool vanilla);

  // QuestInfo.dbc: the tag shown in the quest log ("Elite", "Dungeon", ...). 0 = a normal quest.
  std::vector<Named> const& questTypes();

  // SkillLine.dbc IDs of the professions and secondary skills quests can require.
  std::vector<Named> const& professions(bool vanilla);

  // Reputation ranks and the value each starts at (Friendly = 3000, Exalted = 42000).
  struct ReputationRank
  {
    char const* label;
    std::int32_t value;
  };
  std::vector<ReputationRank> const& reputationRanks();
  // The rank a value falls in (index into reputationRanks()).
  std::size_t reputationRankOf(std::int32_t value);

  // Emotes.dbc IDs of emotes that read well in scripted events.
  std::vector<Named> const& emotes();
}
