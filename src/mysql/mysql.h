// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <cinttypes>
#include <cstddef>
#include <map>
#include <set>
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
    // Space-separated permanent aura spell ids from creature_template.auras (Turtle/mangos) or
    // creature_template_addon.auras (AzerothCore). Empty when the schema has neither.
    std::string auras;
    std::uint32_t faction = 0; // creature_template faction (a FactionTemplate.dbc id) -> hostility
    // Seasonal game-event membership (game_event_creature.event). Signed: 0 = base world (always spawned),
    // >0 = spawns only while that event is active, <0 = spawns EXCEPT while abs(event) is active.
    std::int32_t event = 0;
    // NPC pose from creature_addon: stand_state = UnitStandState (0 stand, 1 sit, 3 sleep, 4/5/6 chair-sit,
    // 7 dead, 8 kneel); emote_state = an Emotes.dbc id (its AnimID overrides the stand pose when non-zero).
    std::uint8_t  stand_state = 0;
    std::uint32_t emote_state = 0;
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
    // Seasonal game-event membership (game_event_gameobject.event). See CreatureSpawnRecord::event.
    std::int32_t event = 0;
    float template_scale = 1.0f;
    float position_x = 0.0f;
    float position_y = 0.0f;
    float position_z = 0.0f;
    float orientation = 0.0f;
  };

  // One row of the game_event table: a seasonal event (Winter Veil, Hallow's End, Darkmoon Faire, ...).
  struct GameEventRecord
  {
    std::int32_t entry = 0;
    std::string description;
  };

  struct GameObjectTemplateRecord
  {
    std::uint32_t entry = 0;
    std::string name;
    std::uint32_t display_id = 0;
    std::uint32_t type = 0;
    float template_scale = 1.0f;
  };

  // Details of one spell for aura visual resolution + the creature-info UI. Pulled from the server's
  // spell_template (Turtle/vmangos); on schemas without it (AzerothCore) the map comes back empty.
  struct SpellInfoRecord
  {
    std::uint32_t entry = 0;
    std::uint32_t spell_visual = 0;
    std::uint32_t icon_id = 0;
    std::uint32_t school = 0;
    std::string name;
    std::string description;
    // For resolving description macros ($s1/$o1/$d/$x1/$a1/... incl. cross-spell $12345s1 refs)
    // into real numbers like the client does.
    std::int32_t effect_base_points[3] = { 0, 0, 0 };
    std::int32_t effect_die_sides[3] = { 0, 0, 0 };
    std::int32_t effect_amplitude[3] = { 0, 0, 0 };
    std::int32_t effect_chain_target[3] = { 0, 0, 0 };
    std::int32_t effect_radius_index[3] = { 0, 0, 0 };
    float effect_multiple_value[3] = { 0.0f, 0.0f, 0.0f };
    std::uint32_t duration_index = 0;
    std::uint32_t max_affected_targets = 0;
    std::uint32_t stack_amount = 0;
    std::uint32_t proc_charges = 0;
    std::uint32_t proc_chance = 0;
    std::uint32_t max_target_level = 0;
    // Blizzard-style tooltip header (cost / range / cast time).
    std::uint32_t mana_cost = 0;
    std::uint32_t power_type = 0;       // 0 mana, 1 rage (stored x10), 2 focus, 3 energy
    std::uint32_t range_index = 0;      // SpellRange.dbc
    std::uint32_t casting_time_index = 0; // SpellCastTimes.dbc
  };

  std::map<std::uint32_t, SpellInfoRecord> getSpellInfos(std::set<std::uint32_t> const& spell_ids, std::string* error = nullptr);

  // Full creature_template details for the creature-info UI (Turtle/vmangos schema; ok=false when the
  // schema doesn't match, e.g. AzerothCore).
  struct CreatureTemplateDetails
  {
    bool ok = false;
    std::uint32_t entry = 0;
    std::string name;
    std::string subname;
    std::uint32_t level_min = 0, level_max = 0;
    std::uint32_t rank = 0;
    std::uint32_t faction = 0;
    std::uint32_t npc_flags = 0;
    std::uint32_t health_min = 0, health_max = 0;
    std::uint32_t mana_min = 0, mana_max = 0;
    std::uint32_t gold_min = 0, gold_max = 0;
    float dmg_min = 0.0f, dmg_max = 0.0f;
    std::uint32_t armor = 0;
    std::int32_t holy_res = 0, fire_res = 0, nature_res = 0, frost_res = 0, shadow_res = 0, arcane_res = 0;
    std::uint32_t display_id = 0;
    std::uint32_t equipment_id = 0;
    std::uint32_t unit_class = 0;
    std::uint32_t type = 0;
    std::vector<std::uint32_t> spells;  // spell_id1-4 + creature_spells list entries
    std::vector<std::uint32_t> auras;   // permanent auras
  };

  CreatureTemplateDetails getCreatureTemplateDetails(std::uint32_t entry, std::string* error = nullptr);

  // Per-display bounding radius (creature_display_info_addon on Turtle, creature_model_info on
  // vmangos/AC). This is the server's UNIT_FIELD_BOUNDINGRADIUS source -- x creature scale it is the
  // exact radius the client uses for the ground selection circle.
  std::map<std::uint32_t, float> getCreatureBoundingRadii(std::string* error = nullptr);

  bool testConnection(bool report_only_err = false);
  bool hasMaxUIDStoredDB(std::size_t mapID);
  std::uint32_t getGUIDFromDB(std::size_t mapID);
  void insertUIDinDB(std::size_t mapID, std::uint32_t NewUID);
  void updateUIDinDB (std::size_t mapID, std::uint32_t NewUID);
  std::vector<CreatureSpawnRecord> getCreatureSpawns(std::size_t mapID, std::string* error = nullptr);
  std::vector<GameObjectSpawnRecord> getGameObjectSpawns(std::size_t mapID, std::string* error = nullptr);
  std::vector<GameEventRecord> getGameEvents(std::string* error = nullptr);
  std::vector<CreaturePatrolPoint> getCreaturePatrolPaths(std::size_t mapID, std::string* error = nullptr);
  std::vector<CreatureSpawnRecord> searchCreatureSpawns(std::string const& searchTerm, std::size_t limit = 200, std::string* error = nullptr);
  std::vector<CreatureTemplateRecord> getCreatureTemplates(std::size_t limit = 10000, std::string* error = nullptr);
  std::vector<GameObjectTemplateRecord> getGameObjectTemplates(std::size_t limit = 25000, std::string* error = nullptr);
  bool updateCreatureSpawn(std::uint32_t guid, float position_x, float position_y, float position_z, float orientation, std::string* error = nullptr);
};
