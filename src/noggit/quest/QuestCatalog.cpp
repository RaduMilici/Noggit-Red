// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/quest/QuestCatalog.hpp>

namespace Noggit::Quest
{
  std::uint32_t allianceRaces(bool vanilla)
  {
    // Human 1, Dwarf 4, Night Elf 8, Gnome 64 (+ Draenei 1024 from TBC on).
    return vanilla ? 77u : 1101u;
  }

  std::uint32_t hordeRaces(bool vanilla)
  {
    // Orc 2, Undead 16, Tauren 32, Troll 128 (+ Blood Elf 512 from TBC on).
    return vanilla ? 178u : 690u;
  }

  std::vector<PlayerClass> const& playerClasses(bool vanilla)
  {
    static std::vector<PlayerClass> const vanilla_classes = {
      {"Warrior", 1}, {"Paladin", 2}, {"Hunter", 3}, {"Rogue", 4}, {"Priest", 5},
      {"Shaman", 7}, {"Mage", 8}, {"Warlock", 9}, {"Druid", 11},
    };
    static std::vector<PlayerClass> const wotlk_classes = {
      {"Warrior", 1}, {"Paladin", 2}, {"Hunter", 3}, {"Rogue", 4}, {"Priest", 5}, {"Death Knight", 6},
      {"Shaman", 7}, {"Mage", 8}, {"Warlock", 9}, {"Druid", 11},
    };
    return vanilla ? vanilla_classes : wotlk_classes;
  }

  std::vector<Named> const& questTypes()
  {
    // Only the tags the vanilla data uses; 1 shows as "Elite" in 1.12 and "Group" in later clients.
    static std::vector<Named> const types = {
      {"Normal", 0}, {"Elite (group)", 1}, {"PvP", 41}, {"Raid", 62}, {"Dungeon", 81}, {"World event", 82},
    };
    return types;
  }

  std::vector<Named> const& professions(bool vanilla)
  {
    static std::vector<Named> const vanilla_skills = {
      {"Alchemy", 171}, {"Blacksmithing", 164}, {"Cooking", 185}, {"Enchanting", 333}, {"Engineering", 202},
      {"First Aid", 129}, {"Fishing", 356}, {"Herbalism", 182}, {"Leatherworking", 165}, {"Lockpicking", 633},
      {"Mining", 186}, {"Riding", 762}, {"Skinning", 393}, {"Tailoring", 197},
    };
    static std::vector<Named> const wotlk_skills = {
      {"Alchemy", 171}, {"Blacksmithing", 164}, {"Cooking", 185}, {"Enchanting", 333}, {"Engineering", 202},
      {"First Aid", 129}, {"Fishing", 356}, {"Herbalism", 182}, {"Inscription", 773}, {"Jewelcrafting", 755},
      {"Leatherworking", 165}, {"Lockpicking", 633}, {"Mining", 186}, {"Riding", 762}, {"Skinning", 393},
      {"Tailoring", 197},
    };
    return vanilla ? vanilla_skills : wotlk_skills;
  }

  std::vector<ReputationRank> const& reputationRanks()
  {
    static std::vector<ReputationRank> const ranks = {
      {"Hated", -42000}, {"Hostile", -6000}, {"Unfriendly", -3000}, {"Neutral", 0},
      {"Friendly", 3000}, {"Honored", 9000}, {"Revered", 21000}, {"Exalted", 42000},
    };
    return ranks;
  }

  std::size_t reputationRankOf(std::int32_t value)
  {
    auto const& ranks = reputationRanks();
    std::size_t rank = 0;
    for (std::size_t i = 0; i < ranks.size(); ++i)
    {
      if (value >= ranks[i].value)
      {
        rank = i;
      }
    }
    return rank;
  }

  std::vector<Named> const& emotes()
  {
    static std::vector<Named> const list = {
      {"Talk", 1}, {"Bow", 2}, {"Wave", 3}, {"Cheer", 4}, {"Exclaim", 5}, {"Question", 6}, {"Laugh", 11},
      {"Rude", 14}, {"Roar", 15}, {"Kneel", 16}, {"Cry", 18}, {"Beg", 20}, {"Applaud", 21}, {"Shout", 22},
      {"Flex", 23}, {"Shy", 24}, {"Point", 25}, {"Salute", 66}, {"Dance", 10},
    };
    return list;
  }
}
