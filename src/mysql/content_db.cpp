// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <mysql/content_db.h>

#include <mysql/mysql_internal.hpp>

#include <algorithm>
#include <cstdlib>
#include <set>
#include <sstream>

namespace mysql::content
{
  using namespace Noggit::Content;
  namespace Quest = Noggit::Quest;
  namespace Npc = Noggit::Npc;
  namespace Item = Noggit::Item;

  namespace
  {
    std::uint32_t toUnsigned(std::optional<std::string> const& value)
    {
      return value ? static_cast<std::uint32_t>(std::strtoul(value->c_str(), nullptr, 10)) : 0;
    }

    std::int32_t toSigned(std::optional<std::string> const& value)
    {
      return value ? static_cast<std::int32_t>(std::strtol(value->c_str(), nullptr, 10)) : 0;
    }

    float toFloat(std::optional<std::string> const& value)
    {
      return value ? std::strtof(value->c_str(), nullptr) : 0.0f;
    }

    // The table's rows as column -> value maps (for the pure modules' *FromRow functions).
    std::vector<std::map<std::string, std::optional<std::string>>> namedRows(Database& db, std::string const& table,
                                                                            std::string const& where_and_order)
    {
      std::vector<std::map<std::string, std::optional<std::string>>> out;
      auto const& columns = db.columnsOf(table);
      if (columns.empty())
      {
        return out;
      }
      auto rows = db.rows("SELECT " + columnList(columns) + " FROM " + quoteIdentifier(table) + " " + where_and_order);
      if (!rows)
      {
        return out;
      }
      for (auto const& row : *rows)
      {
        std::map<std::string, std::optional<std::string>> named;
        for (std::size_t i = 0; i < columns.size() && i < row.size(); ++i)
        {
          named[columns[i]] = row[i];
        }
        out.push_back(std::move(named));
      }
      return out;
    }

    // "ORDER BY `patch` DESC LIMIT 1" when the table versions rows by patch: the newest version.
    std::string newest(Database& db, std::string const& table, char const* version_column = "patch")
    {
      return hasColumn(db.columnsOf(table), version_column)
           ? " ORDER BY " + quoteIdentifier(version_column) + " DESC LIMIT 1" : " LIMIT 1";
    }

    // "SELECT a, b FROM t" rows deduplicated on the first column, keeping the newest version.
    std::vector<SqlRow> latestRows(Database& db, std::string const& table, std::vector<std::string> const& columns,
                                   char const* version_column = "patch")
    {
      std::vector<SqlRow> out;
      auto const& table_columns = db.columnsOf(table);
      if (table_columns.empty() || std::any_of(columns.begin(), columns.end(),
                                               [&](std::string const& c) { return !hasColumn(table_columns, c); }))
      {
        return out;
      }
      std::string order = " ORDER BY " + quoteIdentifier(columns.front());
      if (hasColumn(table_columns, version_column))
      {
        order += ", " + quoteIdentifier(version_column);
      }
      auto rows = db.rows("SELECT " + columnList(columns) + " FROM " + quoteIdentifier(table) + order);
      if (!rows)
      {
        return out;
      }
      for (auto& row : *rows)
      {
        if (!out.empty() && out.back()[0] == row[0])
        {
          out.back() = std::move(row);
        }
        else
        {
          out.push_back(std::move(row));
        }
      }
      return out;
    }
  }

  // --- Database ---

  struct Database::Impl
  {
    detail::Connection connection;
    std::map<std::string, std::vector<std::string>> columns;
  };

  Database::Database() : _impl(std::make_unique<Impl>()) {}
  Database::~Database() = default;

  std::unique_ptr<Database> Database::open(std::string* error)
  {
    auto connection = detail::connect(error);
    if (!connection)
    {
      return nullptr;
    }
    std::unique_ptr<Database> db(new Database());
    db->_impl->connection = std::move(connection);
    // Every editor opens one of these first, so this is where the ID range follows the connected schema.
    setCustomIds(customIdsFor(db->columnsOf("creature_template")));
    return db;
  }

  std::vector<std::string> const& Database::columnsOf(std::string const& table)
  {
    auto const found = _impl->columns.find(table);
    if (found != _impl->columns.end())
    {
      return found->second;
    }
    std::vector<std::string> columns;
    if (auto result = rows("SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
                           "AND TABLE_NAME = " + literal(table) + " ORDER BY ORDINAL_POSITION"))
    {
      for (auto const& row : *result)
      {
        if (!row.empty() && row[0])
        {
          columns.push_back(*row[0]);
        }
      }
    }
    return _impl->columns.emplace(table, std::move(columns)).first->second;
  }

  Npc::ColumnsOf Database::columns()
  {
    return [this](std::string const& table) { return columnsOf(table); };
  }

  std::optional<std::vector<SqlRow>> Database::rows(std::string const& sql, std::string* error)
  {
    MYSQL* connection = _impl->connection.get();
    if (!detail::execute(connection, sql, error))
    {
      return std::nullopt;
    }
    MYSQL_RES* result = mysql_store_result(connection);
    if (!result)
    {
      if (error)
      {
        *error = mysql_error(connection);
      }
      return std::nullopt;
    }
    std::vector<SqlRow> out;
    unsigned int const fields = mysql_num_fields(result);
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
      unsigned long const* lengths = mysql_fetch_lengths(result);
      SqlRow values;
      values.reserve(fields);
      for (unsigned int i = 0; i < fields; ++i)
      {
        values.push_back(row[i] ? std::optional<std::string>(std::string(row[i], lengths[i])) : std::nullopt);
      }
      out.push_back(std::move(values));
    }
    mysql_free_result(result);
    return out;
  }

  std::uint32_t Database::number(std::string const& sql)
  {
    auto result = rows(sql);
    return result && !result->empty() && !result->front().empty() ? toUnsigned(result->front()[0]) : 0;
  }

  std::uint32_t Database::nextFreeId(std::vector<std::pair<std::string, std::string>> const& columns, std::uint32_t start)
  {
    std::uint32_t highest = 0;
    for (auto const& [table, column] : columns)
    {
      if (!hasColumn(columnsOf(table), column))
      {
        continue;
      }
      highest = std::max(highest, number("SELECT COALESCE(MAX(" + quoteIdentifier(column) + "), 0) FROM "
                                         + quoteIdentifier(table) + " WHERE " + quoteIdentifier(column) + " >= "
                                         + literal(start)));
    }
    return Noggit::Content::nextFreeId(highest, start);
  }

  SqlScriptResult Database::execute(std::vector<std::string> const& statements,
                                    std::vector<std::string> const& cleanup,
                                    bool require_first_row)
  {
    SqlScriptResult result;
    MYSQL* connection = _impl->connection.get();
    if (statements.empty())
    {
      result.error = "Nothing to write.";
      return result;
    }
    if (!detail::execute(connection, "START TRANSACTION", &result.error))
    {
      return result;
    }
    for (auto const& statement : statements)
    {
      bool ok = detail::execute(connection, statement, &result.error);
      if (ok && require_first_row && result.statements_executed == 0 && mysql_affected_rows(connection) == 0)
      {
        ok = false;
        result.error = "The row to copy no longer exists in the database.";
      }
      if (!ok)
      {
        result.failed_statement = statement.substr(0, 300);
        detail::execute(connection, "ROLLBACK");
        if (result.statements_executed > 0)
        {
          for (auto const& undo : cleanup)
          {
            detail::execute(connection, undo);
          }
        }
        return result;
      }
      ++result.statements_executed;
    }
    if (!detail::execute(connection, "COMMIT", &result.error))
    {
      detail::execute(connection, "ROLLBACK");
      return result;
    }
    // The next reads must see the new layout if a statement changed it (it does not today; cheap anyway).
    _impl->columns.clear();
    result.ok = true;
    return result;
  }

  std::string Database::dumpRows(std::string const& table, std::string const& where)
  {
    auto const& columns = columnsOf(table);
    if (columns.empty())
    {
      return {};
    }
    auto result = rows("SELECT " + columnList(columns) + " FROM " + quoteIdentifier(table) + " WHERE " + where);
    return result ? buildInsertValues(table, columns, *result) : std::string();
  }

  // --- lists ---

  std::vector<NamedEntry> creatures(Database& db)
  {
    std::vector<NamedEntry> out;
    auto const schema = Npc::detectTemplateSchema(db.columnsOf("creature_template"));
    if (!schema.valid())
    {
      return out;
    }
    auto const flags = schema.npc_flags_col.empty() ? schema.entry_col : schema.npc_flags_col;
    for (auto const& row : latestRows(db, "creature_template", {schema.entry_col, schema.name_col, flags}))
    {
      out.push_back({toUnsigned(row[0]), row[1].value_or(std::string()),
                     schema.npc_flags_col.empty() ? 0u : toUnsigned(row[2]), 0});
    }
    return out;
  }

  std::vector<NamedEntry> items(Database& db)
  {
    std::vector<NamedEntry> out;
    auto const& columns = db.columnsOf("item_template");
    auto const quality = firstColumn(columns, {"quality", "Quality"});
    auto const display = firstColumn(columns, {"display_id", "displayid"});
    // Missing optional columns read the entry instead, and are ignored.
    for (auto const& row : latestRows(db, "item_template", {"entry", "name", quality.empty() ? "entry" : quality,
                                                            display.empty() ? "entry" : display}))
    {
      out.push_back({toUnsigned(row[0]), row[1].value_or(std::string()), quality.empty() ? 1u : toUnsigned(row[2]),
                     display.empty() ? 0u : toUnsigned(row[3])});
    }
    return out;
  }

  std::vector<NamedEntry> objects(Database& db)
  {
    std::vector<NamedEntry> out;
    for (auto const& row : latestRows(db, "gameobject_template", {"entry", "name", "type"}))
    {
      out.push_back({toUnsigned(row[0]), row[1].value_or(std::string()), toUnsigned(row[2]), 0});
    }
    return out;
  }

  std::vector<NamedEntry> spells(Database& db)
  {
    std::vector<NamedEntry> out;
    for (auto const& row : latestRows(db, "spell_template", {"entry", "name"}, "build"))
    {
      out.push_back({toUnsigned(row[0]), row[1].value_or(std::string()), 0, 0});
    }
    return out;
  }

  std::vector<AreaTrigger> areaTriggers(Database& db)
  {
    std::vector<AreaTrigger> out;
    // tortoise-wow's areatrigger_template has no name column: read the id in its place, named below.
    bool const named = hasColumn(db.columnsOf("areatrigger_template"), "name");
    for (auto const& row : latestRows(db, "areatrigger_template", {"id", named ? "name" : "id", "map_id", "x", "y", "z", "radius"},
                                      "build"))
    {
      AreaTrigger trigger;
      trigger.id = toUnsigned(row[0]);
      trigger.name = named ? row[1].value_or(std::string()) : "Area trigger";
      trigger.map = toUnsigned(row[2]);
      trigger.x = toFloat(row[3]);
      trigger.y = toFloat(row[4]);
      trigger.z = toFloat(row[5]);
      trigger.radius = toFloat(row[6]);
      out.push_back(std::move(trigger));
    }
    std::map<std::uint32_t, AreaTrigger*> by_id;
    for (auto& trigger : out)
    {
      by_id[trigger.id] = &trigger;
    }
    if (auto links = db.rows("SELECT `id`, `quest` FROM `areatrigger_involvedrelation`"))
    {
      for (auto const& row : *links)
      {
        if (auto found = by_id.find(toUnsigned(row[0])); found != by_id.end())
        {
          found->second->quest = toUnsigned(row[1]);
        }
      }
    }
    // Triggers with another job: using them for a quest would not work (or would break that job).
    for (auto const* table : {"areatrigger_teleport", "areatrigger_tavern", "areatrigger_scripts", "scripted_areatrigger",
                              "areatrigger_bg_entrance"})
    {
      auto const& columns = db.columnsOf(table);
      auto const id = firstColumn(columns, {"id", "entry"});
      if (id.empty())
      {
        continue;
      }
      // The teleport / inn tables name their triggers: used when the template has no names (tortoise-wow).
      auto const name = named ? std::string() : firstColumn(columns, {"name"});
      if (auto used = db.rows("SELECT " + quoteIdentifier(id) + ", " + quoteIdentifier(name.empty() ? id : name)
                              + " FROM " + quoteIdentifier(table)))
      {
        for (auto const& row : *used)
        {
          if (auto found = by_id.find(toUnsigned(row[0])); found != by_id.end())
          {
            found->second->other_use = true;
            if (!name.empty() && row[1] && !row[1]->empty())
            {
              found->second->name = *row[1];
            }
          }
        }
      }
    }
    return out;
  }

  bool questList(Database& db, QuestList& out, std::string* error)
  {
    auto const schema = Quest::detectQuestSchema(db.columnsOf("quest_template"));
    if (!schema.valid())
    {
      if (error)
      {
        *error = "This database has no quest_template table the editor understands.";
      }
      return false;
    }
    std::vector<std::string> columns = {schema.entry_col, schema.title_col};
    for (auto const* column : {&schema.level_col, &schema.prev_quest_col, &schema.next_quest_col,
                               &schema.exclusive_group_col, &schema.next_in_chain_col})
    {
      columns.push_back(column->empty() ? schema.entry_col : *column); // missing: read the entry, ignored below
    }
    out = {};
    for (auto const& row : latestRows(db, "quest_template", columns))
    {
      Quest::ChainQuest quest;
      quest.entry = toUnsigned(row[0]);
      quest.title = row[1].value_or(std::string());
      quest.level = schema.level_col.empty() ? 0 : toUnsigned(row[2]);
      quest.prev_quest = schema.prev_quest_col.empty() ? 0 : toSigned(row[3]);
      quest.next_quest = schema.next_quest_col.empty() ? 0 : toSigned(row[4]);
      quest.exclusive_group = schema.exclusive_group_col.empty() ? 0 : toSigned(row[5]);
      quest.next_in_chain = schema.next_in_chain_col.empty() ? 0 : toUnsigned(row[6]);
      quest.editable = isCustom(quest.entry);
      out.quests.push_back(std::move(quest));
    }

    auto const links = Quest::detectLinkSchema(db.columns());
    auto const read = [&](Quest::RelationTable const& relation, Quest::Giver::Kind kind, bool starts)
    {
      if (!relation.valid())
      {
        return;
      }
      if (auto rows = db.rows("SELECT " + quoteIdentifier(relation.owner_col) + ", " + quoteIdentifier(relation.quest_col)
                              + " FROM " + quoteIdentifier(relation.table)))
      {
        for (auto const& row : *rows)
        {
          out.links.push_back({{kind, toUnsigned(row[0])}, toUnsigned(row[1]), starts});
        }
      }
    };
    read(links.npc_starters, Quest::Giver::Kind::Npc, true);
    read(links.npc_enders, Quest::Giver::Kind::Npc, false);
    read(links.object_starters, Quest::Giver::Kind::Object, true);
    read(links.object_enders, Quest::Giver::Kind::Object, false);
    return true;
  }

  // --- NPCs ---

  namespace
  {
    Npc::DialogueStored loadDialogue(Database& db, Npc::NpcDatabase const& schema, std::uint32_t entry,
                                     std::uint32_t menu)
    {
      Npc::DialogueStored stored;
      auto const& dialogue = schema.dialogue;
      stored.menu = menu;
      if (menu && dialogue.greetingSupported())
      {
        auto const menu_where = equals(dialogue.menu_entry_col, literal(menu));
        auto const menu_rows = db.number("SELECT COUNT(*) FROM `gossip_menu` WHERE " + menu_where);
        stored.npc_text = db.number("SELECT " + quoteIdentifier(dialogue.menu_text_col) + " FROM `gossip_menu` WHERE "
                                    + menu_where + " LIMIT 1");
        auto const other_npcs = db.number("SELECT COUNT(*) FROM `creature_template` WHERE "
                                          + equals(schema.npc.gossip_menu_col, literal(menu)) + " AND "
                                          + quoteIdentifier(schema.npc.entry_col) + " <> " + literal(entry));
        auto const options = hasColumn(db.columnsOf("gossip_menu_option"), "menu_id")
                           ? db.number("SELECT COUNT(*) FROM `gossip_menu_option` WHERE `menu_id` = " + literal(menu)) : 0;
        stored.menu_shared = other_npcs > 0 || menu_rows > 1 || options > 0;
        auto const text_where = equals(dialogue.text_id_col, literal(stored.npc_text));
        if (!dialogue.text_inline_col.empty())
        {
          if (auto rows = db.rows("SELECT " + quoteIdentifier(dialogue.text_inline_col) + " FROM `npc_text` WHERE " + text_where))
          {
            stored.dialogue.greeting = rows->empty() ? std::string() : rows->front()[0].value_or(std::string());
          }
        }
        else
        {
          stored.broadcast = db.number("SELECT " + quoteIdentifier(dialogue.text_broadcast_col) + " FROM `npc_text` WHERE " + text_where);
          if (auto rows = db.rows("SELECT " + quoteIdentifier(dialogue.broadcast_male_col) + " FROM `broadcast_text` WHERE "
                                  + equals(dialogue.broadcast_entry_col, literal(stored.broadcast))))
          {
            stored.dialogue.greeting = rows->empty() ? std::string() : rows->front()[0].value_or(std::string());
          }
        }
      }
      if (dialogue.questGreetingSupported())
      {
        if (auto rows = db.rows("SELECT " + quoteIdentifier(dialogue.greeting_text_col) + " FROM `quest_greeting` WHERE "
                                + allOf({equals(dialogue.greeting_entry_col, literal(entry)), equals(dialogue.greeting_type_col, "0")})))
        {
          stored.dialogue.quest_greeting = rows->empty() ? std::string() : rows->front()[0].value_or(std::string());
        }
      }
      return stored;
    }
  }

  bool loadNpc(Database& db, std::uint32_t entry, NpcEditorData& out, std::string* error)
  {
    out.db = Npc::detectNpcDatabase(db.columns());
    auto const& schema = out.db.npc;
    if (!schema.valid())
    {
      if (error)
      {
        *error = "This database has no creature_template table the editor understands.";
      }
      return false;
    }
    auto const rows = namedRows(db, "creature_template", "WHERE " + equals(schema.entry_col, literal(entry))
                                                           + newest(db, "creature_template"));
    if (rows.empty())
    {
      if (error)
      {
        *error = "There is no NPC " + std::to_string(entry) + " in the database.";
      }
      return false;
    }
    out.values = Npc::npcFieldsFromRow(schema, rows.front());
    auto const script = schema.script_col.empty() ? rows.front().end() : rows.front().find(schema.script_col);
    out.script_name = script != rows.front().end() ? script->second.value_or(std::string()) : std::string();

    out.related_rows.clear();
    std::vector<std::pair<std::string, std::string>> id_columns = {{"creature_template", schema.entry_col}};
    for (auto const& related : schema.related)
    {
      out.related_rows[related.table] = db.number("SELECT COUNT(*) FROM " + quoteIdentifier(related.table) + " WHERE "
                                                  + equals(related.key_column, literal(entry)));
      // Leftovers of a deleted custom NPC would be inherited by a new one with the same entry.
      id_columns.emplace_back(related.table, related.key_column);
    }
    out.next_entry = db.nextFreeId(id_columns, customIds().entry_start);

    out.dialogue = loadDialogue(db, out.db, entry, out.values.gossip_menu.value_or(0));
    if (out.db.dialogue.greetingSupported())
    {
      out.dialogue_ids.next_menu = db.number("SELECT COALESCE(MAX(" + quoteIdentifier(out.db.dialogue.menu_entry_col)
                                             + "), 0) + 1 FROM `gossip_menu`");
      out.dialogue_ids.next_text = db.nextFreeId({{"npc_text", out.db.dialogue.text_id_col}}, customIds().text_start);
      out.dialogue_ids.next_broadcast = out.db.dialogue.broadcast_entry_col.empty()
                                      ? 0 : db.nextFreeId({{"broadcast_text", out.db.dialogue.broadcast_entry_col}}, customIds().text_start);
    }

    out.spawns = out.db.spawns.valid()
               ? db.number("SELECT COUNT(*) FROM `creature` WHERE " + equals(out.db.spawns.entry_col, literal(entry))) : 0;
    out.quest_links = 0;
    for (auto const* relation : {&out.db.quest_links.npc_starters, &out.db.quest_links.npc_enders})
    {
      if (relation->valid())
      {
        out.quest_links += db.number("SELECT COUNT(*) FROM " + quoteIdentifier(relation->table) + " WHERE "
                                     + equals(relation->owner_col, literal(entry)));
      }
    }
    return true;
  }

  std::string npcSnapshot(Database& db, Npc::NpcDatabase const& schema, std::uint32_t entry)
  {
    std::stringstream sql;
    sql << "-- Custom NPC " << entry << ", exported by Noggit.\n"
        << "-- Safe to run more than once: it replaces the NPC with exactly this definition.\n\n";

    auto const menu = db.number("SELECT " + quoteIdentifier(schema.npc.gossip_menu_col.empty() ? schema.npc.entry_col
                                                                                             : schema.npc.gossip_menu_col)
                                + " FROM `creature_template` WHERE " + equals(schema.npc.entry_col, literal(entry)) + " LIMIT 1");
    auto const dialogue = schema.npc.gossip_menu_col.empty() ? Npc::DialogueStored{} : loadDialogue(db, schema, entry, menu);
    std::vector<std::string> deletes = Npc::buildDeleteStatements(schema.npc, entry);
    auto dialogue_deletes = Npc::dialogueDeletes(schema.dialogue, entry, dialogue);
    deletes.insert(deletes.end(), dialogue_deletes.begin(), dialogue_deletes.end());
    for (auto const& statement : deletes)
    {
      sql << statement << ";\n";
    }
    sql << "\n";

    auto const dump = [&](std::string const& table, std::string const& where)
    {
      if (auto insert = db.dumpRows(table, where); !insert.empty())
      {
        sql << insert << "\n\n";
      }
    };
    dump("creature_template", equals(schema.npc.entry_col, literal(entry)));
    for (auto const& related : schema.npc.related)
    {
      dump(related.table, equals(related.key_column, literal(entry)));
    }
    if (Npc::ownsMenu(dialogue))
    {
      auto const& d = schema.dialogue;
      if (!d.broadcast_entry_col.empty() && dialogue.broadcast)
      {
        dump("broadcast_text", equals(d.broadcast_entry_col, literal(dialogue.broadcast)));
      }
      dump("npc_text", equals(d.text_id_col, literal(dialogue.npc_text)));
      dump("gossip_menu", equals(d.menu_entry_col, literal(dialogue.menu)));
    }
    if (schema.dialogue.questGreetingSupported())
    {
      dump("quest_greeting", allOf({equals(schema.dialogue.greeting_entry_col, literal(entry)),
                                    equals(schema.dialogue.greeting_type_col, "0")}));
    }
    return sql.str();
  }

  // --- quests ---

  namespace
  {
    // NPC / object entries linked to the quest through a relation table.
    std::vector<Quest::Giver> linkedGivers(Database& db, Quest::RelationTable const& relation, Quest::Giver::Kind kind,
                                           std::uint32_t quest)
    {
      std::vector<Quest::Giver> out;
      if (!relation.valid())
      {
        return out;
      }
      if (auto rows = db.rows("SELECT " + quoteIdentifier(relation.owner_col) + " FROM " + quoteIdentifier(relation.table)
                              + " WHERE " + equals(relation.quest_col, literal(quest)) + " ORDER BY 1"))
      {
        for (auto const& row : *rows)
        {
          out.push_back({kind, toUnsigned(row[0])});
        }
      }
      return out;
    }

    // A creature or chest that drops `item` for quests, with its chance (creatures first).
    std::optional<Quest::QuestDrop> findDrop(Database& db, Quest::LootSchema const& loot, std::uint32_t item)
    {
      auto const query = [&](Quest::LootTableSchema const& table, std::string const& source_table,
                             std::string const& source_entry, std::string const& join) -> std::optional<SqlRow>
      {
        auto rows = db.rows("SELECT s." + quoteIdentifier(source_entry) + ", l." + quoteIdentifier(table.entry_col)
                            + ", ABS(l." + quoteIdentifier(table.chance_col) + ") FROM " + quoteIdentifier(table.table)
                            + " l JOIN " + quoteIdentifier(source_table) + " s ON " + join + " WHERE l."
                            + quoteIdentifier(table.item_col) + " = " + literal(item) + " AND l."
                            + Noggit::Quest::questOnlyCondition(table) + " ORDER BY s." + quoteIdentifier(source_entry)
                            + " LIMIT 1");
        return rows && !rows->empty() ? std::optional<SqlRow>(rows->front()) : std::nullopt;
      };
      if (loot.creatureDrops())
      {
        if (auto row = query(loot.creature, "creature_template", loot.creature_entry_col,
                             "s." + quoteIdentifier(loot.creature_loot_col) + " = l." + quoteIdentifier(loot.creature.entry_col)))
        {
          return Quest::QuestDrop{Quest::QuestDrop::Source::Creature, toUnsigned((*row)[0]), item, toFloat((*row)[2]),
                                  toUnsigned((*row)[1])};
        }
      }
      if (loot.objectDrops())
      {
        if (auto row = query(loot.object, "gameobject_template", loot.object_entry_col,
                             "s." + quoteIdentifier(loot.object_loot_col) + " = l." + quoteIdentifier(loot.object.entry_col)
                             + " AND s." + quoteIdentifier(loot.object_type_col) + " = " + literal(Quest::CHEST_OBJECT_TYPE)))
        {
          return Quest::QuestDrop{Quest::QuestDrop::Source::Object, toUnsigned((*row)[0]), item, toFloat((*row)[2]),
                                  toUnsigned((*row)[1])};
        }
      }
      return std::nullopt;
    }

    Quest::ParsedScript loadScript(Database& db, Quest::ScriptSchema const& schema, Quest::ScriptWhen when,
                                   std::uint32_t script_id)
    {
      if (!schema.valid() || !script_id)
      {
        return {};
      }
      auto const& table = when == Quest::ScriptWhen::Accepted ? schema.start_table : schema.end_table;
      std::vector<Quest::ScriptRow> rows;
      for (auto& row : namedRows(db, table, "WHERE `id` = " + literal(script_id)))
      {
        rows.push_back(std::move(row));
      }
      std::map<std::uint32_t, Quest::SpokenText> texts;
      auto const ids = Quest::spokenTextIds(rows);
      if (!ids.empty())
      {
        std::string const chat = schema.text_chat_type_col.empty() ? std::string("0") : quoteIdentifier(schema.text_chat_type_col);
        if (auto found = db.rows("SELECT " + quoteIdentifier(schema.text_entry_col) + ", " + quoteIdentifier(schema.text_male_col)
                                 + ", " + chat + " FROM `broadcast_text` WHERE " + quoteIdentifier(schema.text_entry_col)
                                 + " IN (" + idList(ids) + ")"))
        {
          for (auto const& row : *found)
          {
            texts[toUnsigned(row[0])] = {row[1].value_or(std::string()), toUnsigned(row[2])};
          }
        }
      }
      return Quest::parseScript(rows, texts);
    }

    std::string collectItemsCondition(Quest::QuestSchema const& schema, std::uint32_t item)
    {
      std::string condition;
      for (auto const& column : schema.collect_cols)
      {
        if (!column.empty())
        {
          condition += (condition.empty() ? "" : " OR ") + equals(column, literal(item));
        }
      }
      return condition;
    }
  }

  bool loadQuest(Database& db, std::uint32_t entry, QuestList const& list, QuestEditorData& out, std::string* error)
  {
    out = {};
    out.db = Quest::detectQuestDatabase(db.columns());
    auto const& schema = out.db.quest;
    if (!schema.valid())
    {
      if (error)
      {
        *error = "This database has no quest_template table the editor understands.";
      }
      return false;
    }

    std::vector<std::pair<std::string, std::string>> id_columns = {{"quest_template", schema.entry_col}};
    for (auto const* relation : {&out.db.links.npc_starters, &out.db.links.npc_enders, &out.db.links.object_starters,
                                 &out.db.links.object_enders})
    {
      if (relation->valid())
      {
        id_columns.emplace_back(relation->table, relation->quest_col);
      }
    }
    out.next_entry = db.nextFreeId(id_columns, customIds().entry_start);

    if (!schema.xp_col.empty() && !schema.level_col.empty())
    {
      if (auto rows = db.rows("SELECT " + quoteIdentifier(schema.level_col) + ", " + quoteIdentifier(schema.xp_col)
                              + " FROM `quest_template` WHERE " + quoteIdentifier(schema.xp_col) + " > 0 AND "
                              + quoteIdentifier(schema.entry_col) + " < " + literal(customIds().entry_start)))
      {
        for (auto const& row : *rows)
        {
          out.xp_by_level[toUnsigned(row[0])].push_back(toUnsigned(row[1]));
        }
      }
    }
    if (!entry)
    {
      return true;
    }

    auto const rows = namedRows(db, "quest_template", "WHERE " + equals(schema.entry_col, literal(entry))
                                                        + newest(db, "quest_template"));
    if (rows.empty())
    {
      if (error)
      {
        *error = "There is no quest " + std::to_string(entry) + " in the database.";
      }
      return false;
    }
    auto& content = out.content;
    content.fields = Quest::questFieldsFromRow(schema, rows.front());

    auto const& links = out.db.links;
    content.links.starters = linkedGivers(db, links.npc_starters, Quest::Giver::Kind::Npc, entry);
    auto objects = linkedGivers(db, links.object_starters, Quest::Giver::Kind::Object, entry);
    content.links.starters.insert(content.links.starters.end(), objects.begin(), objects.end());
    content.links.enders = linkedGivers(db, links.npc_enders, Quest::Giver::Kind::Npc, entry);
    objects = linkedGivers(db, links.object_enders, Quest::Giver::Kind::Object, entry);
    content.links.enders.insert(content.links.enders.end(), objects.begin(), objects.end());
    if (!links.item_start_quest_col.empty())
    {
      content.links.start_item = db.number("SELECT " + quoteIdentifier(links.item_entry_col) + " FROM `item_template` WHERE "
                                           + equals(links.item_start_quest_col, literal(entry)) + " LIMIT 1");
    }
    if (links.area_triggers.valid())
    {
      content.links.area_trigger = db.number("SELECT " + quoteIdentifier(links.area_triggers.owner_col) + " FROM "
                                             + quoteIdentifier(links.area_triggers.table) + " WHERE "
                                             + equals(links.area_triggers.quest_col, literal(entry)) + " LIMIT 1");
    }

    Quest::ChainModel chain(list.quests);
    content.prerequisites = chain.prerequisitesOf(entry);
    content.exclusive_with = chain.exclusiveWith(entry);
    content.offers_next = content.fields.next_in_chain.value_or(0);

    for (auto const& collect : content.fields.collect.value_or(std::vector<Quest::ItemCount>{}))
    {
      if (auto drop = findDrop(db, out.db.loot, collect.id))
      {
        content.drops.push_back(*drop);
      }
      auto const condition = collectItemsCondition(schema, collect.id);
      if (!condition.empty() && db.number("SELECT COUNT(*) FROM `quest_template` WHERE " + quoteIdentifier(schema.entry_col)
                                          + " <> " + literal(entry) + " AND (" + condition + ")") > 0)
      {
        out.stored.shared_items.insert(collect.id);
      }
    }

    auto const accept = loadScript(db, out.db.scripts, Quest::ScriptWhen::Accepted, content.fields.start_script.value_or(0));
    auto const complete = loadScript(db, out.db.scripts, Quest::ScriptWhen::Completed, content.fields.complete_script.value_or(0));
    content.on_accept = accept.actions;
    content.on_complete = complete.actions;
    content.scripts_complete = accept.complete && complete.complete;
    out.stored.accept_text_ids = accept.custom_text_ids;
    out.stored.complete_text_ids = complete.custom_text_ids;
    return true;
  }

  Quest::SaveInputs saveInputs(Database& db, QuestEditorData const& data, QuestList const& list,
                               Quest::QuestContent& content)
  {
    Quest::SaveInputs inputs;
    inputs.chain = list.quests;

    std::vector<Quest::Giver> linked = content.links.starters;
    linked.insert(linked.end(), content.links.enders.begin(), content.links.enders.end());
    auto const npcs = Quest::npcEntries(linked);
    auto const& npc = data.db.npc;
    if (!npcs.empty() && !npc.npc_flags_col.empty())
    {
      if (auto rows = db.rows("SELECT " + quoteIdentifier(npc.entry_col) + ", " + quoteIdentifier(npc.npc_flags_col)
                              + " FROM `creature_template` WHERE " + quoteIdentifier(npc.entry_col) + " IN (" + idList(npcs) + ")"))
      {
        for (auto const& row : *rows)
        {
          inputs.npc_flags[toUnsigned(row[0])] = toUnsigned(row[1]);
        }
      }
    }

    auto const& triggers = data.db.links.area_triggers;
    if (content.links.area_trigger && triggers.valid())
    {
      inputs.trigger_quests[content.links.area_trigger] =
        db.number("SELECT " + quoteIdentifier(triggers.quest_col) + " FROM " + quoteIdentifier(triggers.table) + " WHERE "
                  + equals(triggers.owner_col, literal(content.links.area_trigger)));
    }

    auto const& loot = data.db.loot;
    for (auto& drop : content.drops)
    {
      bool const creature = drop.source == Quest::QuestDrop::Source::Creature;
      drop.loot_id = creature
        ? db.number("SELECT " + quoteIdentifier(loot.creature_loot_col) + " FROM `creature_template` WHERE "
                    + equals(loot.creature_entry_col, literal(drop.source_entry)) + " LIMIT 1")
        : db.number("SELECT " + quoteIdentifier(loot.object_loot_col) + " FROM `gameobject_template` WHERE "
                    + equals(loot.object_entry_col, literal(drop.source_entry)) + " LIMIT 1");
    }

    if (data.db.scripts.valid())
    {
      inputs.next_text_id = db.nextFreeId({{"broadcast_text", data.db.scripts.text_entry_col}}, customIds().text_start);
    }
    return inputs;
  }

  std::string questSnapshot(Database& db, Quest::QuestDatabase const& schema, std::uint32_t entry)
  {
    QuestList list;
    QuestEditorData data;
    if (!questList(db, list) || !loadQuest(db, entry, list, data))
    {
      return {};
    }
    auto const& content = data.content;
    std::stringstream sql;
    sql << "-- Custom quest " << entry << ", exported by Noggit.\n"
        << "-- Safe to run more than once: it replaces the quest with exactly this definition.\n\n";
    for (auto const& statement : Quest::questRowsDelete(schema, entry, data.stored))
    {
      sql << statement << ";\n";
    }
    sql << "\n";

    auto const dump = [&](std::string const& table, std::string const& where)
    {
      if (auto insert = db.dumpRows(table, where); !insert.empty())
      {
        sql << insert << "\n\n";
      }
    };
    dump("quest_template", equals(schema.quest.entry_col, literal(entry)));
    auto const& links = schema.links;
    for (auto const* relation : {&links.npc_starters, &links.npc_enders, &links.object_starters, &links.object_enders,
                                 &links.area_triggers})
    {
      if (relation->valid())
      {
        dump(relation->table, equals(relation->quest_col, literal(entry)));
      }
    }
    if (content.links.start_item)
    {
      sql << updateStatement("item_template", {{links.item_start_quest_col, literal(entry)}},
                             equals(links.item_entry_col, literal(content.links.start_item))) << ";\n\n";
    }
    if (schema.scripts.valid())
    {
      std::vector<std::uint32_t> texts = data.stored.accept_text_ids;
      texts.insert(texts.end(), data.stored.complete_text_ids.begin(), data.stored.complete_text_ids.end());
      if (!texts.empty())
      {
        dump("broadcast_text", quoteIdentifier(schema.scripts.text_entry_col) + " IN (" + idList(texts) + ")");
      }
      dump(schema.scripts.start_table, "`id` = " + literal(entry));
      dump(schema.scripts.end_table, "`id` = " + literal(entry));
    }

    std::vector<Quest::Giver> linked = content.links.starters;
    linked.insert(linked.end(), content.links.enders.begin(), content.links.enders.end());
    if (auto flags = Quest::buildQuestGiverFlagStatement(schema.npc, Quest::npcEntries(linked)); !flags.empty())
    {
      sql << "-- The NPCs above need the quest-giver role.\n" << flags << ";\n\n";
    }
    for (auto const& drop : content.drops)
    {
      sql << "-- " << (drop.source == Quest::QuestDrop::Source::Creature ? "Creature " : "Object ") << drop.source_entry
          << " drops item " << drop.item << " for the quest.\n";
      // A loot table keyed by the source's own entry may be one the editor gave it (it had none): replaying
      // on a rebuilt database has to give it again.
      std::uint32_t const loot_id = drop.loot_id == drop.source_entry ? 0 : drop.loot_id;
      for (auto const& statement : Quest::buildDropStatements(schema.loot, {drop.source, drop.source_entry, drop.item,
                                                                            drop.chance, loot_id}))
      {
        sql << statement << ";\n";
      }
      sql << "\n";
    }
    return sql.str();
  }

  // --- items ---

  bool loadItem(Database& db, std::uint32_t entry, ItemEditorData& out, std::string* error)
  {
    out = {};
    out.schema = Item::detectItemSchema(db.columnsOf("item_template"));
    if (!out.schema.valid())
    {
      if (error)
      {
        *error = "This database has no item_template table the editor understands.";
      }
      return false;
    }
    out.next_entry = db.nextFreeId({{"item_template", out.schema.entry_col}}, customIds().entry_start);
    if (!entry)
    {
      return true;
    }
    auto rows = db.rows("SELECT " + columnList(out.schema.columns) + " FROM `item_template` WHERE "
                        + equals(out.schema.entry_col, literal(entry)) + newest(db, "item_template"), error);
    if (!rows || rows->empty())
    {
      if (error && rows)
      {
        *error = "There is no item " + std::to_string(entry) + " in the database.";
      }
      return false;
    }
    out.values = Item::itemFieldsFromRow(out.schema, rows->front());

    // Where it is used.
    auto const quest = Quest::detectQuestSchema(db.columnsOf("quest_template"));
    std::string uses;
    for (auto const* columns : {&quest.collect_cols, &quest.reward_cols})
    {
      for (auto const& column : *columns)
      {
        if (!column.empty())
        {
          uses += (uses.empty() ? "" : " OR ") + equals(column, literal(entry));
        }
      }
    }
    for (auto const& column : quest.choice_cols)
    {
      if (!column.empty())
      {
        uses += (uses.empty() ? "" : " OR ") + equals(column, literal(entry));
      }
    }
    if (!quest.source_item_col.empty())
    {
      uses += (uses.empty() ? "" : " OR ") + equals(quest.source_item_col, literal(entry));
    }
    if (!uses.empty())
    {
      if (auto found = db.rows("SELECT DISTINCT " + quoteIdentifier(quest.entry_col) + " FROM `quest_template` WHERE " + uses))
      {
        for (auto const& row : *found)
        {
          out.uses.quests.push_back(toUnsigned(row[0]));
        }
      }
    }
    for (auto const* table : {"creature_loot_template", "gameobject_loot_template"})
    {
      if (hasColumn(db.columnsOf(table), "item"))
      {
        out.uses.loot_rows += db.number("SELECT COUNT(*) FROM " + quoteIdentifier(table) + " WHERE `item` = " + literal(entry));
      }
    }
    if (hasColumn(db.columnsOf("npc_vendor"), "item"))
    {
      out.uses.vendor_rows = db.number("SELECT COUNT(*) FROM `npc_vendor` WHERE `item` = " + literal(entry));
    }
    return true;
  }

  std::string itemSnapshot(Database& db, Item::ItemSchema const& schema, std::uint32_t entry)
  {
    std::stringstream sql;
    sql << "-- Custom item " << entry << ", exported by Noggit.\n"
        << "-- Safe to run more than once: it replaces the item with exactly this definition.\n\n"
        << Item::buildItemDelete(schema, entry) << ";\n\n"
        << db.dumpRows("item_template", equals(schema.entry_col, literal(entry))) << "\n";
    return sql.str();
  }
}
