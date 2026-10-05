// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// The column lists of the vmangos world tables the content editors use, as the live vmangos database
// reports them (generated from the vmangos-database Docker image, build 5875). Lets the pure tests run
// against the real schema without a database.

#pragma once

#include <map>
#include <string>
#include <vector>

inline std::map<std::string, std::vector<std::string>> const& vmangosSchema()
{
  static std::map<std::string, std::vector<std::string>> const tables = {
    {"creature_template", {
      "entry", "patch", "name", "subname", "level_min", "level_max", "faction", "npc_flags",
      "gossip_menu_id", "display_id1", "display_id2", "display_id3", "display_id4", "display_scale1",
      "display_scale2", "display_scale3", "display_scale4", "display_probability1", "display_probability2",
      "display_probability3", "display_probability4", "display_total_probability", "mount_display_id",
      "speed_walk", "speed_run", "detection_range", "call_for_help_range", "leash_range", "type",
      "pet_family", "rank", "unit_class", "xp_multiplier", "health_multiplier", "mana_multiplier",
      "armor_multiplier", "damage_multiplier", "damage_variance", "damage_school", "base_attack_time",
      "ranged_attack_time", "holy_res", "fire_res", "nature_res", "frost_res", "shadow_res", "arcane_res",
      "trainer_type", "trainer_spell", "trainer_class", "trainer_race", "loot_id", "pickpocket_loot_id",
      "skinning_loot_id", "gold_min", "gold_max", "spell_list_id", "pet_spell_list_id", "spawn_spell_id",
      "totem_spell_id", "auras", "ai_name", "movement_type", "inhabit_type", "civilian", "racial_leader",
      "equipment_id", "trainer_id", "vendor_id", "mechanic_immune_mask", "school_immune_mask",
      "immunity_flags", "static_flags1", "static_flags2", "flags_extra", "script_name"
    }},
    {"npc_vendor", {"entry", "slot", "item", "maxcount", "incrtime", "itemflags", "condition_id"}},
    {"npc_trainer", {
      "entry", "spell", "spellcost", "reqskill", "reqskillvalue", "reqlevel", "build_min", "build_max"
    }},
    {"quest_template", {
      "entry", "patch", "Method", "ZoneOrSort", "MinLevel", "MaxLevel", "QuestLevel", "Type",
      "RequiredClasses", "RequiredRaces", "RequiredSkill", "RequiredSkillValue", "RequiredCondition",
      "RepObjectiveFaction", "RepObjectiveValue", "RequiredMinRepFaction", "RequiredMinRepValue",
      "RequiredMaxRepFaction", "RequiredMaxRepValue", "SuggestedPlayers", "LimitTime", "QuestFlags",
      "SpecialFlags", "PrevQuestId", "NextQuestId", "ExclusiveGroup", "BreadcrumbForQuestId",
      "NextQuestInChain", "SrcItemId", "SrcItemCount", "SrcSpell", "Title", "Details", "Objectives",
      "OfferRewardText", "RequestItemsText", "EndText", "ObjectiveText1", "ObjectiveText2", "ObjectiveText3",
      "ObjectiveText4", "ReqItemId1", "ReqItemId2", "ReqItemId3", "ReqItemId4", "ReqItemCount1",
      "ReqItemCount2", "ReqItemCount3", "ReqItemCount4", "ReqSourceId1", "ReqSourceId2", "ReqSourceId3",
      "ReqSourceId4", "ReqSourceCount1", "ReqSourceCount2", "ReqSourceCount3", "ReqSourceCount4",
      "ReqCreatureOrGOId1", "ReqCreatureOrGOId2", "ReqCreatureOrGOId3", "ReqCreatureOrGOId4",
      "ReqCreatureOrGOCount1", "ReqCreatureOrGOCount2", "ReqCreatureOrGOCount3", "ReqCreatureOrGOCount4",
      "ReqSpellCast1", "ReqSpellCast2", "ReqSpellCast3", "ReqSpellCast4", "RewChoiceItemId1",
      "RewChoiceItemId2", "RewChoiceItemId3", "RewChoiceItemId4", "RewChoiceItemId5", "RewChoiceItemId6",
      "RewChoiceItemCount1", "RewChoiceItemCount2", "RewChoiceItemCount3", "RewChoiceItemCount4",
      "RewChoiceItemCount5", "RewChoiceItemCount6", "RewItemId1", "RewItemId2", "RewItemId3", "RewItemId4",
      "RewItemCount1", "RewItemCount2", "RewItemCount3", "RewItemCount4", "RewRepFaction1", "RewRepFaction2",
      "RewRepFaction3", "RewRepFaction4", "RewRepFaction5", "RewRepValue1", "RewRepValue2", "RewRepValue3",
      "RewRepValue4", "RewRepValue5", "RewRepSpilloverMask", "RewXP", "RewOrReqMoney", "RewMoneyMaxLevel",
      "RewSpell", "RewSpellCast", "RewMailTemplateId", "RewMailDelaySecs", "RewMailMoney", "PointMapId",
      "PointX", "PointY", "PointOpt", "DetailsEmote1", "DetailsEmote2", "DetailsEmote3", "DetailsEmote4",
      "DetailsEmoteDelay1", "DetailsEmoteDelay2", "DetailsEmoteDelay3", "DetailsEmoteDelay4",
      "IncompleteEmote", "CompleteEmote", "OfferRewardEmote1", "OfferRewardEmote2", "OfferRewardEmote3",
      "OfferRewardEmote4", "OfferRewardEmoteDelay1", "OfferRewardEmoteDelay2", "OfferRewardEmoteDelay3",
      "OfferRewardEmoteDelay4", "StartScript", "CompleteScript"
    }},
    {"creature_questrelation", {"id", "quest", "patch_min", "patch_max"}},
    {"creature_involvedrelation", {"id", "quest", "patch_min", "patch_max"}},
    {"gameobject_questrelation", {"id", "quest", "patch_min", "patch_max"}},
    {"gameobject_involvedrelation", {"id", "quest", "patch_min", "patch_max"}},
    {"areatrigger_involvedrelation", {"id", "quest"}},
    {"item_template", {
      "entry", "patch", "class", "subclass", "name", "description", "display_id", "quality", "flags",
      "buy_count", "buy_price", "sell_price", "inventory_type", "allowable_class", "allowable_race",
      "item_level", "required_level", "required_skill", "required_skill_rank", "required_spell",
      "required_honor_rank", "required_city_rank", "required_reputation_faction", "required_reputation_rank",
      "max_count", "stackable", "container_slots", "stat_type1", "stat_value1", "stat_type2", "stat_value2",
      "stat_type3", "stat_value3", "stat_type4", "stat_value4", "stat_type5", "stat_value5", "stat_type6",
      "stat_value6", "stat_type7", "stat_value7", "stat_type8", "stat_value8", "stat_type9", "stat_value9",
      "stat_type10", "stat_value10", "delay", "range_mod", "ammo_type", "dmg_min1", "dmg_max1", "dmg_type1",
      "dmg_min2", "dmg_max2", "dmg_type2", "dmg_min3", "dmg_max3", "dmg_type3", "dmg_min4", "dmg_max4",
      "dmg_type4", "dmg_min5", "dmg_max5", "dmg_type5", "block", "armor", "holy_res", "fire_res",
      "nature_res", "frost_res", "shadow_res", "arcane_res", "spellid_1", "spelltrigger_1", "spellcharges_1",
      "spellppmrate_1", "spellcooldown_1", "spellcategory_1", "spellcategorycooldown_1", "spellid_2",
      "spelltrigger_2", "spellcharges_2", "spellppmrate_2", "spellcooldown_2", "spellcategory_2",
      "spellcategorycooldown_2", "spellid_3", "spelltrigger_3", "spellcharges_3", "spellppmrate_3",
      "spellcooldown_3", "spellcategory_3", "spellcategorycooldown_3", "spellid_4", "spelltrigger_4",
      "spellcharges_4", "spellppmrate_4", "spellcooldown_4", "spellcategory_4", "spellcategorycooldown_4",
      "spellid_5", "spelltrigger_5", "spellcharges_5", "spellppmrate_5", "spellcooldown_5",
      "spellcategory_5", "spellcategorycooldown_5", "bonding", "page_text", "page_language", "page_material",
      "start_quest", "lock_id", "material", "sheath", "random_property", "set_id", "max_durability",
      "area_bound", "map_bound", "duration", "bag_family", "disenchant_id", "food_type", "min_money_loot",
      "max_money_loot", "wrapped_gift", "extra_flags", "other_team_entry"
    }},
    {"creature_loot_template", {
      "entry", "item", "ChanceOrQuestChance", "groupid", "mincountOrRef", "maxcount", "condition_id",
      "patch_min", "patch_max"
    }},
    {"gameobject_loot_template", {
      "entry", "item", "ChanceOrQuestChance", "groupid", "mincountOrRef", "maxcount", "condition_id",
      "patch_min", "patch_max"
    }},
    {"gameobject_template", {
      "entry", "patch", "type", "displayId", "name", "icon", "faction", "flags", "size", "data0", "data1",
      "data2", "data3", "data4", "data5", "data6", "data7", "data8", "data9", "data10", "data11", "data12",
      "data13", "data14", "data15", "data16", "data17", "data18", "data19", "data20", "data21", "data22",
      "data23", "mingold", "maxgold", "script_name"
    }},
    {"quest_start_scripts", {
      "id", "delay", "priority", "command", "datalong", "datalong2", "datalong3", "datalong4",
      "target_param1", "target_param2", "target_type", "data_flags", "dataint", "dataint2", "dataint3",
      "dataint4", "x", "y", "z", "o", "condition_id", "comments"
    }},
    {"quest_end_scripts", {
      "id", "delay", "priority", "command", "datalong", "datalong2", "datalong3", "datalong4",
      "target_param1", "target_param2", "target_type", "data_flags", "dataint", "dataint2", "dataint3",
      "dataint4", "x", "y", "z", "o", "condition_id", "comments"
    }},
    {"broadcast_text", {
      "entry", "male_text", "female_text", "chat_type", "sound_id", "language_id", "emote_id1", "emote_id2",
      "emote_id3", "emote_delay1", "emote_delay2", "emote_delay3"
    }},
    {"npc_text", {
      "ID", "BroadcastTextID0", "Probability0", "BroadcastTextID1", "Probability1", "BroadcastTextID2",
      "Probability2", "BroadcastTextID3", "Probability3", "BroadcastTextID4", "Probability4",
      "BroadcastTextID5", "Probability5", "BroadcastTextID6", "Probability6", "BroadcastTextID7",
      "Probability7"
    }},
    {"gossip_menu", {"entry", "text_id", "script_id", "condition_id"}},
    {"quest_greeting", {
      "entry", "type", "content_default", "content_loc1", "content_loc2", "content_loc3", "content_loc4",
      "content_loc5", "content_loc6", "content_loc7", "content_loc8", "emote_id", "emote_delay"
    }},
    {"creature", {
      "guid", "id", "id2", "id3", "id4", "id5", "map", "position_x", "position_y", "position_z",
      "orientation", "spawntimesecsmin", "spawntimesecsmax", "wander_distance", "health_percent",
      "mana_percent", "movement_type", "spawn_flags", "visibility_mod", "patch_min", "patch_max"
    }},
    {"creature_addon", {
      "guid", "patch", "display_id", "mount_display_id", "equipment_id", "stand_state", "sheath_state",
      "emote_state", "auras"
    }},
    {"creature_movement", {
      "id", "point", "position_x", "position_y", "position_z", "orientation", "waittime", "wander_distance",
      "script_id", "path_id"
    }},
    {"game_event_creature", {"guid", "event"}},
    {"pool_creature", {"guid", "pool_entry", "chance", "description", "flags", "patch_min", "patch_max"}},
    {"creature_linking", {"guid", "master_guid", "flag"}},
  };
  return tables;
}

// ColumnsOf over vmangosSchema().
inline std::vector<std::string> vmangosColumns(std::string const& table)
{
  auto const found = vmangosSchema().find(table);
  return found == vmangosSchema().end() ? std::vector<std::string>{} : found->second;
}
