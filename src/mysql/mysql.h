// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <cinttypes>
#include <cstddef>
#include <string>
#include <vector>

namespace mysql
{
  struct CreatureSpawnRecord
  {
    std::uint32_t guid = 0;
    std::uint32_t entry = 0;
    std::uint32_t map = 0;
    std::uint32_t display_id = 0;
    std::uint32_t mount_display_id = 0;
    std::uint32_t mainhand_display_id = 0;
    std::uint32_t offhand_display_id = 0;
    std::uint32_t ranged_display_id = 0;
    std::uint32_t mainhand_inventory_type = 0;
    std::uint32_t offhand_inventory_type = 0;
    std::uint32_t ranged_inventory_type = 0;
    std::string name;
    float template_scale = 1.0f;
    float position_x = 0.0f;
    float position_y = 0.0f;
    float position_z = 0.0f;
    float orientation = 0.0f;
  };

  struct CreatureTemplateRecord
  {
    std::uint32_t entry = 0;
    std::uint32_t faction = 0;
    std::uint32_t creature_type = 0;
    std::uint32_t rank = 0;
    std::uint32_t npc_flags = 0;
    std::uint32_t type_flags = 0;
    std::uint32_t flags_extra = 0;
    std::uint32_t display_id = 0;
    std::string name;
    float template_scale = 1.0f;
  };

  struct CreaturePatrolPoint
  {
    std::uint32_t guid = 0;   // creature.guid this waypoint belongs to
    std::uint32_t point = 0;  // waypoint order index
    float position_x = 0.0f;
    float position_y = 0.0f;
    float position_z = 0.0f;
  };

  struct GameObjectSpawnRecord
  {
    std::uint32_t guid = 0;
    std::uint32_t entry = 0;
    std::uint32_t map = 0;
    std::uint32_t display_id = 0;
    std::string name;
    float template_scale = 1.0f;
    float position_x = 0.0f;
    float position_y = 0.0f;
    float position_z = 0.0f;
    float orientation = 0.0f;
  };

  struct GameObjectTemplateRecord
  {
    std::uint32_t entry = 0;
    std::string name;
    std::uint32_t display_id = 0;
    std::uint32_t type = 0;
    float template_scale = 1.0f;
  };

  bool testConnection(bool report_only_err = false);
  bool hasMaxUIDStoredDB(std::size_t mapID);
  std::uint32_t getGUIDFromDB(std::size_t mapID);
  void insertUIDinDB(std::size_t mapID, std::uint32_t NewUID);
  void updateUIDinDB (std::size_t mapID, std::uint32_t NewUID);
  std::vector<CreatureSpawnRecord> getCreatureSpawns(std::size_t mapID, std::string* error = nullptr);
  std::vector<GameObjectSpawnRecord> getGameObjectSpawns(std::size_t mapID, std::string* error = nullptr);
  std::vector<CreaturePatrolPoint> getCreaturePatrolPaths(std::size_t mapID, std::string* error = nullptr);
  std::vector<CreatureSpawnRecord> searchCreatureSpawns(std::string const& searchTerm, std::size_t limit = 200, std::string* error = nullptr);
  std::vector<CreatureTemplateRecord> getCreatureTemplates(std::size_t limit = 10000, std::string* error = nullptr);
  std::vector<GameObjectTemplateRecord> getGameObjectTemplates(std::size_t limit = 25000, std::string* error = nullptr);
  bool updateCreatureSpawn(std::uint32_t guid, float position_x, float position_y, float position_z, float orientation, std::string* error = nullptr);
};
