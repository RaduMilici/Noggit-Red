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
    // Extended spawn columns (tortoise-wow schema; adaptively mapped on mangos-style schemas, 0/default
    // when a column has no equivalent). Semantics RE'd from the live server source (Object.h /
    // Creature.h / RandomMovementGenerator / ObjectMgr::LoadCreatures):
    //  - id2..id4: alternate creature_template entries; one non-zero id is picked at RANDOM each spawn.
    //  - spawntimesecs min/max: respawn delay range, seconds (server rolls urand(min, max)).
    //  - wander_distance: YARDS -- radius around the spawn for movement_type 1 (Random) wandering.
    //  - health/mana percent: spawn at this % of maximum (Turtle-only columns).
    //  - movement_type: 0 Idle, 1 Random (wander), 2 Waypoint (path from creature_movement).
    //  - spawn_flags: bitmask, see SPAWN_FLAG_* (0x01 ACTIVE .. 0x100 NO_DYNAMIC_RESPAWN).
    //  - visibility_mod: YARDS -- creature is visible within max(normal visibility, this); 0 = normal.
    std::uint32_t id2 = 0;
    std::uint32_t id3 = 0;
    std::uint32_t id4 = 0;
    std::uint32_t spawntimesecs_min = 0;
    std::uint32_t spawntimesecs_max = 0;
    float         wander_distance = 0.0f;
    std::uint32_t health_percent = 100;
    std::uint32_t mana_percent = 100;
    std::uint32_t movement_type = 0;
    std::uint32_t spawn_flags = 0;
    float         visibility_mod = 0.0f;
    // cmangos-only columns: spawnMask = map-difficulty bitmask (bit k = spawned in Difficulty k;
    // open world = 1), phaseMask = WotLK phasing bitmask (default 1; 0 is invalid).
    std::uint32_t spawn_mask = 1;
    std::uint32_t phase_mask = 1;
  };

  // Which extended `creature` columns THIS database actually has, with their real column names
  // (Turtle: spawntimesecsmin/wander_distance/movement_type...; mangos-style: spawntimesecs/
  // spawndist/MovementType). Empty string = the schema has no equivalent -> the editor field is
  // disabled and the exporter omits the column.
  struct CreatureSpawnTableColumns
  {
    std::string entry_col = "id";  // "id" (mangos/Turtle) or "id1" (AzerothCore)
    std::string id2_col, id3_col, id4_col;
    std::string respawn_min_col, respawn_max_col;   // same column when the schema has one spawntimesecs
    std::string wander_col;
    std::string health_percent_col, mana_percent_col;
    // AzerothCore keeps ABSOLUTE curhealth/curmana (0 = spawn at full) instead of percents; when
    // those columns matched, this flags the editor to switch the fields to absolute semantics.
    bool health_mana_absolute = false;
    std::string movement_col;
    std::string spawn_flags_col;
    std::string visibility_col;
    std::string spawn_mask_col, phase_mask_col; // cmangos + AzerothCore (spawnMask / phaseMask)
    // cmangos-style alternative to id2..id4: the creature_spawn_entry (guid, entry) table --
    // creature.id = 0 + rows there = random entry pick per spawn (mangos-wotlk ObjectMgr).
    // Only set when the schema has no id2 column but DOES have this table.
    bool spawn_entry_table = false;
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

  // Result of running a multi-statement SQL script against the connected world DB.
  struct SqlScriptResult
  {
    bool ok = false;
    std::size_t statements_executed = 0;
    std::string error;            // driver error text on failure
    std::string failed_statement; // first ~300 chars of the statement that failed
  };

  // Execute a .sql script (multiple ;-terminated statements; --/#/C-style comments and quoted
  // strings are handled) against the project's connected database, inside one transaction:
  // any failure rolls the whole script back (DDL statements self-commit per MySQL rules, but
  // spawn exports are pure DML so the all-or-nothing guarantee holds for them).
  SqlScriptResult executeSqlScript(std::string const& script);

  // "user@host:port/schema" of the project connection (plus "via SSH user@host:port" in tunnel mode),
  // for confirmation dialogs.
  std::string connectionDescription();

  // Where MySQL connections go right now: the saved server/port, or 127.0.0.1:<local tunnel port> when
  // the project uses an SSH tunnel (waits for / starts the tunnel; GUI thread). For external tools such
  // as the mysql command line client.
  bool resolveEndpoint(std::string& host, unsigned int& port, std::string* error = nullptr);

  // Saved credentials of the active project (GUI thread). host/port are the SAVED direct-connection
  // values; tunnel mode callers substitute the tunnel endpoint themselves.
  struct ConnectionTarget
  {
    std::string host;
    std::string user;
    std::string password;
    std::string schema;
    unsigned int port = 3306;
  };
  ConnectionTarget currentConnectionTarget();

  enum class ProbeStatus
  {
    Ok,
    Unreachable,  // TCP connect / handshake failed
    AuthFailed,   // server answered, credentials rejected
    SchemaFailed, // logged in, but the World DB could not be created/selected/prepared
    Failed,
  };
  struct ProbeResult
  {
    ProbeStatus status = ProbeStatus::Failed;
    unsigned int error_code = 0;
    std::string error;
  };
  // Must be called once on the main thread before probeConnection runs on a worker thread.
  void initClientLibrary();
  // Runs exactly the steps every database call performs (connect, schema, UIDs table) against
  // `target`, without touching settings or the tunnel -- safe on a worker thread.
  ProbeResult probeConnection(ConnectionTarget const& target);

  bool testConnection(bool report_only_err = false);
  bool hasMaxUIDStoredDB(std::size_t mapID);
  std::uint32_t getGUIDFromDB(std::size_t mapID);
  void insertUIDinDB(std::size_t mapID, std::uint32_t NewUID);
  void updateUIDinDB (std::size_t mapID, std::uint32_t NewUID);
  std::vector<CreatureSpawnRecord> getCreatureSpawns(std::size_t mapID, std::string* error = nullptr,
                                                     CreatureSpawnTableColumns* columns_out = nullptr);
  std::vector<GameObjectSpawnRecord> getGameObjectSpawns(std::size_t mapID, std::string* error = nullptr);
  std::vector<GameEventRecord> getGameEvents(std::string* error = nullptr);
  std::vector<CreaturePatrolPoint> getCreaturePatrolPaths(std::size_t mapID, std::string* error = nullptr);
  std::vector<CreatureSpawnRecord> searchCreatureSpawns(std::string const& searchTerm, std::size_t limit = 200, std::string* error = nullptr);
  std::vector<CreatureTemplateRecord> getCreatureTemplates(std::size_t limit = 10000, std::string* error = nullptr);
  std::vector<GameObjectTemplateRecord> getGameObjectTemplates(std::size_t limit = 25000, std::string* error = nullptr);
  bool updateCreatureSpawn(std::uint32_t guid, float position_x, float position_y, float position_z, float orientation, std::string* error = nullptr);
};
