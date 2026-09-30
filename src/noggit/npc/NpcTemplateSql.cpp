// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/npc/NpcTemplateSql.hpp>

#include <cstdlib>


namespace Noggit::Npc
{
  using namespace Noggit::Content;

  namespace
  {
    // column -> SQL literal for every field that is set and mapped on this schema.
    ColumnValues fieldValues(TemplateSchema const& schema, NpcFields const& fields)
    {
      ColumnValues out;
      auto const set = [&](std::string const& column, auto const& value)
      {
        if (!column.empty() && value)
        {
          out[column] = literal(*value);
        }
      };

      set(schema.name_col, fields.name);
      set(schema.subname_col, fields.subname);
      set(schema.level_min_col, fields.level_min);
      set(schema.level_max_col, fields.level_max);
      for (auto const& column : schema.faction_cols)
      {
        set(column, fields.faction);
      }
      set(schema.npc_flags_col, fields.npc_flags);
      set(schema.rank_col, fields.rank);
      if (fields.display_id && !schema.display_col.empty())
      {
        out[schema.display_col] = literal(*fields.display_id);
        for (auto const& column : schema.other_display_cols)
        {
          out[column] = "0";
        }
        if (!schema.display_probability_col.empty())
        {
          out[schema.display_probability_col] = "100";
        }
        for (auto const& column : schema.other_display_probability_cols)
        {
          out[column] = "0";
        }
        if (!schema.display_total_probability_col.empty())
        {
          out[schema.display_total_probability_col] = "100";
        }
      }
      set(schema.scale_col, fields.scale);
      if (fields.clear_script && !schema.script_col.empty())
      {
        out[schema.script_col] = "''";
      }
      set(schema.gossip_menu_col, fields.gossip_menu);
      return out;
    }
  }

  TemplateSchema detectTemplateSchema(std::vector<std::string> const& columns)
  {
    TemplateSchema schema;
    schema.columns = columns;
    schema.entry_col = firstColumn(columns, {"entry", "Entry"});
    schema.name_col = firstColumn(columns, {"name", "Name"});
    schema.subname_col = firstColumn(columns, {"subname", "SubName"});
    schema.level_min_col = firstColumn(columns, {"level_min", "minlevel", "MinLevel"});
    schema.level_max_col = firstColumn(columns, {"level_max", "maxlevel", "MaxLevel"});
    for (auto const* candidate : {"faction", "FactionAlliance", "FactionHorde", "faction_A", "faction_H"})
    {
      if (hasColumn(columns, candidate))
      {
        schema.faction_cols.emplace_back(candidate);
      }
    }
    schema.npc_flags_col = firstColumn(columns, {"npc_flags", "npcflag", "NpcFlags"});
    schema.rank_col = firstColumn(columns, {"rank", "Rank"});
    schema.script_col = firstColumn(columns, {"script_name", "ScriptName"});
    schema.gossip_menu_col = firstColumn(columns, {"gossip_menu_id", "GossipMenuId"});
    schema.patch_col = firstColumn(columns, {"patch"});

    // Model slots: vmangos display_id1..4, CMaNGOS DisplayId1..4, older TrinityCore modelid1..4.
    // AzerothCore keeps models in creature_template_model instead: no inline column -> model not editable.
    for (auto const* prefix : {"display_id", "DisplayId", "modelid", "ModelId"})
    {
      std::string const first = std::string(prefix) + "1";
      if (!hasColumn(columns, first))
      {
        continue;
      }
      schema.display_col = first;
      for (int i = 2; i <= 4; ++i)
      {
        std::string const other = std::string(prefix) + std::to_string(i);
        if (hasColumn(columns, other))
        {
          schema.other_display_cols.push_back(other);
        }
      }
      break;
    }
    for (auto const* prefix : {"display_probability", "DisplayIdProbability"})
    {
      std::string const first = std::string(prefix) + "1";
      if (!hasColumn(columns, first))
      {
        continue;
      }
      schema.display_probability_col = first;
      for (int i = 2; i <= 4; ++i)
      {
        std::string const other = std::string(prefix) + std::to_string(i);
        if (hasColumn(columns, other))
        {
          schema.other_display_probability_cols.push_back(other);
        }
      }
      break;
    }
    schema.display_total_probability_col = firstColumn(columns, {"display_total_probability"});
    // vmangos scales each model slot (display_scale1, 0 = the model's own size); other cores (Turtle /
    // tortoise-wow included, which has display_id1..4 but no per-slot scales) have one "scale" for the
    // whole creature.
    schema.scale_col = firstColumn(columns, {"display_scale1", "scale", "Scale"});
    return schema;
  }

  std::vector<RelatedTableCandidate> const& relatedTableCandidates()
  {
    // creature_equip_template is keyed by the creature entry only on AzerothCore/TrinityCore; on
    // vmangos/CMaNGOS its "entry" is an equipment id shared through creature_template.equipment_id,
    // which the clone copies anyway.
    static std::vector<RelatedTableCandidate> const candidates = {
      {"npc_vendor", {"entry"}, "items sold", true},
      {"npc_trainer", {"entry", "ID"}, "spells taught", true},
      {"creature_template_addon", {"entry"}, "appearance extras", false},
      {"creature_template_model", {"CreatureID"}, "models", false},
      {"creature_equip_template", {"CreatureID"}, "weapons", false},
      {"creature_template_resistance", {"CreatureID"}, "resistances", false},
      {"creature_template_spell", {"CreatureID"}, "spells", false},
    };
    return candidates;
  }

  TemplateSchema detectNpcSchema(ColumnsOf const& columns_of)
  {
    auto schema = detectTemplateSchema(columns_of("creature_template"));
    for (auto const& candidate : relatedTableCandidates())
    {
      auto columns = columns_of(candidate.table);
      for (auto const* key : candidate.key_columns)
      {
        if (hasColumn(columns, key))
        {
          schema.related.push_back({candidate.table, key, std::move(columns), candidate.label, candidate.optional});
          break;
        }
      }
    }
    return schema;
  }

  NpcFields npcFieldsFromRow(TemplateSchema const& schema, std::map<std::string, std::optional<std::string>> const& row)
  {
    auto const text = [&](std::string const& column) -> std::optional<std::string>
    {
      auto const found = column.empty() ? row.end() : row.find(column);
      return found == row.end() ? std::nullopt : std::optional<std::string>(found->second.value_or(std::string()));
    };
    auto const number = [&](std::string const& column) -> std::optional<std::uint32_t>
    {
      auto const value = text(column);
      return value ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(std::strtoul(value->c_str(), nullptr, 10)))
                   : std::nullopt;
    };
    NpcFields fields;
    fields.name = text(schema.name_col);
    fields.subname = text(schema.subname_col);
    fields.level_min = number(schema.level_min_col);
    fields.level_max = number(schema.level_max_col);
    if (!schema.faction_cols.empty())
    {
      fields.faction = number(schema.faction_cols.front());
    }
    fields.npc_flags = number(schema.npc_flags_col);
    fields.rank = number(schema.rank_col);
    fields.display_id = number(schema.display_col);
    if (auto const scale = text(schema.scale_col))
    {
      fields.scale = std::strtof(scale->c_str(), nullptr);
    }
    fields.gossip_menu = number(schema.gossip_menu_col);
    return fields;
  }

  std::vector<std::string> buildCloneStatements(TemplateSchema const& schema,
                                                CloneRequest const& request)
  {
    std::vector<std::string> statements;
    if (!schema.valid())
    {
      return statements;
    }

    auto overrides = fieldValues(schema, request.fields);
    overrides[schema.entry_col] = literal(request.new_entry);
    if (!schema.patch_col.empty())
    {
      overrides[schema.patch_col] = "0";
    }
    statements.push_back(copyRowStatement("creature_template", schema.columns, overrides,
                                          sourceColumn(schema.entry_col) + " = " + literal(request.source_entry),
                                          schema.patch_col));

    for (auto const& related : schema.related)
    {
      if (related.optional && !request.copy_related.count(related.table))
      {
        continue;
      }
      statements.push_back(copyRowStatement(related.table, related.columns,
                                            {{related.key_column, literal(request.new_entry)}},
                                            sourceColumn(related.key_column) + " = " + literal(request.source_entry)));
    }
    return statements;
  }

  std::string buildUpdateStatement(TemplateSchema const& schema,
                                   std::uint32_t entry,
                                   NpcFields const& fields)
  {
    if (!schema.valid())
    {
      return {};
    }
    return updateStatement("creature_template", inTableOrder(schema.columns, fieldValues(schema, fields)),
                           equals(schema.entry_col, literal(entry)));
  }

  std::vector<std::string> buildDeleteStatements(TemplateSchema const& schema, std::uint32_t entry)
  {
    std::vector<std::string> statements;
    if (!schema.valid())
    {
      return statements;
    }
    statements.push_back(deleteStatement("creature_template", equals(schema.entry_col, literal(entry))));
    for (auto const& related : schema.related)
    {
      statements.push_back(deleteStatement(related.table, equals(related.key_column, literal(entry))));
    }
    return statements;
  }

  std::vector<NpcRole> const& npcRoles(bool vanilla)
  {
    // vmangos/Turtle: Unit.h NPCFlags (1.12). CMaNGOS-WotLK/AzerothCore/TrinityCore: 3.3.5a NPCFlags.
    static std::vector<NpcRole> const vanilla_roles = {
      {"Can talk", "Shows a chat window when clicked (gossip).", 0x1},
      {"Quest giver", "Offers and accepts quests. Added automatically when you assign a quest to the NPC.", 0x2},
      {"Vendor", "Sells items.", 0x4},
      {"Repairs items", "Repairs players' gear (use together with Vendor).", 0x4000},
      {"Trainer", "Teaches spells or professions.", 0x10},
      {"Flight master", "Offers flight paths.", 0x8},
      {"Innkeeper", "Lets players set their hearthstone.", 0x80},
      {"Banker", "Opens the player's bank.", 0x100},
      {"Auctioneer", "Opens the auction house.", 0x1000},
      {"Stable master", "Stables hunter pets.", 0x2000},
      {"Guild master", "Sells guild charters.", 0x200},
      {"Tabard designer", "Designs guild tabards.", 0x400},
      {"Battlemaster", "Queues players for battlegrounds.", 0x800},
      {"Spirit healer", "Resurrects dead players.", 0x20},
    };
    static std::vector<NpcRole> const wotlk_roles = {
      {"Can talk", "Shows a chat window when clicked (gossip).", 0x1},
      {"Quest giver", "Offers and accepts quests. Added automatically when you assign a quest to the NPC.", 0x2},
      {"Vendor", "Sells items.", 0x80},
      {"Repairs items", "Repairs players' gear (use together with Vendor).", 0x1000},
      {"Trainer", "Teaches spells or professions.", 0x10},
      {"Flight master", "Offers flight paths.", 0x2000},
      {"Innkeeper", "Lets players set their hearthstone.", 0x10000},
      {"Banker", "Opens the player's bank.", 0x20000},
      {"Auctioneer", "Opens the auction house.", 0x200000},
      {"Stable master", "Stables hunter pets.", 0x400000},
      {"Guild master", "Sells guild charters.", 0x40000},
      {"Tabard designer", "Designs guild tabards.", 0x80000},
      {"Battlemaster", "Queues players for battlegrounds.", 0x100000},
      {"Spirit healer", "Resurrects dead players.", 0x4000},
    };
    return vanilla ? vanilla_roles : wotlk_roles;
  }

  std::vector<RankOption> const& rankOptions()
  {
    static std::vector<RankOption> const options = {
      {"Normal", 0},
      {"Elite", 1},
      {"Rare elite", 2},
      {"Boss", 3},
      {"Rare", 4},
    };
    return options;
  }
}
