#include "ContentStore.hpp"
#include "Database.hpp"

#include <QString>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace Noggit::Creator
{
  using namespace Noggit::Content;
  namespace Q = Noggit::Quest;
  namespace I = Noggit::Item;

  namespace
  {
    // Quest IDs stay within the signed quest-chain columns (PrevQuestId, NextQuestId, ...).
    constexpr std::uint32_t QUEST_ID_LIMIT = 8388607;
    constexpr std::uint32_t ITEM_ID_LIMIT = 16777215;
    constexpr std::uint32_t FIRST_ID = 1000000;

    void require(bool condition, std::string const& message)
    {
      if (!condition)
      {
        throw std::runtime_error(message);
      }
    }

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

    QString q(std::string const& text)
    {
      return QString::fromStdString(text);
    }

    // One connection with the session's layout cache.
    class Reader
    {
    public:
      Reader(Database& db, Layouts& layouts) : _db(db), _layouts(layouts) {}

      Database& db() { return _db; }
      std::vector<std::string> const& columnsOf(std::string const& table) { return _layouts.columnsOf(_db, table); }
      Q::ColumnsOf columns()
      {
        return [this](std::string const& table) { return columnsOf(table); };
      }
      std::vector<SqlRow> rows(std::string const& sql) { return _db.rows(sql); }
      std::uint32_t number(std::string const& sql)
      {
        auto const result = rows(sql);
        return !result.empty() && !result.front().empty() ? toUnsigned(result.front()[0]) : 0;
      }

    private:
      Database& _db;
      Layouts& _layouts;
    };

    // The table's rows as column -> value maps (for the pure modules' *FromRow functions).
    std::vector<std::map<std::string, std::optional<std::string>>> namedRows(Reader& reader, std::string const& table,
                                                                            std::string const& where_and_order)
    {
      std::vector<std::map<std::string, std::optional<std::string>>> out;
      auto const& columns = reader.columnsOf(table);
      if (columns.empty())
      {
        return out;
      }
      for (auto const& row : reader.rows("SELECT " + columnList(columns) + " FROM " + quoteIdentifier(table) + " " + where_and_order))
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
    std::string newest(Reader& reader, std::string const& table)
    {
      return hasColumn(reader.columnsOf(table), "patch") ? " ORDER BY `patch` DESC LIMIT 1" : " LIMIT 1";
    }

    // "SELECT a, b FROM t" rows deduplicated on the first column, keeping the newest version.
    std::vector<SqlRow> latestRows(Reader& reader, std::string const& table, std::vector<std::string> const& columns,
                                   char const* version_column = "patch")
    {
      std::vector<SqlRow> out;
      auto const& table_columns = reader.columnsOf(table);
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
      for (auto& row : reader.rows("SELECT " + columnList(columns) + " FROM " + quoteIdentifier(table) + order))
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

    std::set<std::uint32_t> owned(Reader& reader, char const* kind)
    {
      std::set<std::uint32_t> out;
      for (auto const& row : reader.rows(std::string("SELECT entry FROM creator_content WHERE kind='") + kind + "'"))
      {
        out.insert(toUnsigned(row[0]));
      }
      return out;
    }

    // First free ID at or above FIRST_ID across the given (table, column) pairs.
    std::uint32_t nextId(Reader& reader, std::vector<std::pair<std::string, std::string>> const& columns, std::uint32_t start,
                         std::uint32_t limit)
    {
      std::uint32_t highest = 0;
      for (auto const& [table, column] : columns)
      {
        if (hasColumn(reader.columnsOf(table), column))
        {
          highest = std::max(highest, reader.number("SELECT COALESCE(MAX(" + quoteIdentifier(column) + "), 0) FROM "
                                                    + quoteIdentifier(table)));
        }
      }
      std::uint32_t const next = std::max(start, highest + 1);
      require(next <= limit, "There is no free local identifier available for this content.");
      return next;
    }

    std::uint32_t nextQuestId(Reader& reader, Q::QuestDatabase const& db)
    {
      std::vector<std::pair<std::string, std::string>> columns = {{"quest_template", db.quest.entry_col}};
      for (auto const* relation : {&db.links.npc_starters, &db.links.npc_enders, &db.links.object_starters, &db.links.object_enders})
      {
        if (relation->valid())
        {
          columns.emplace_back(relation->table, relation->quest_col);
        }
      }
      return nextId(reader, columns, FIRST_ID, QUEST_ID_LIMIT);
    }

    QuestList readQuestList(Reader& reader)
    {
      auto const schema = Q::detectQuestSchema(reader.columnsOf("quest_template"));
      require(schema.valid(), "The local world database has no quest_template table the quest editor understands.");
      auto const own = owned(reader, "quest");
      std::vector<std::string> columns = {schema.entry_col, schema.title_col};
      for (auto const* column : {&schema.level_col, &schema.prev_quest_col, &schema.next_quest_col,
                                 &schema.exclusive_group_col, &schema.next_in_chain_col})
      {
        columns.push_back(column->empty() ? schema.entry_col : *column); // missing: read the entry, ignored below
      }
      QuestList out;
      for (auto const& row : latestRows(reader, "quest_template", columns))
      {
        Q::ChainQuest quest;
        quest.entry = toUnsigned(row[0]);
        quest.title = row[1].value_or(std::string());
        quest.level = schema.level_col.empty() ? 0 : toUnsigned(row[2]);
        quest.prev_quest = schema.prev_quest_col.empty() ? 0 : toSigned(row[3]);
        quest.next_quest = schema.next_quest_col.empty() ? 0 : toSigned(row[4]);
        quest.exclusive_group = schema.exclusive_group_col.empty() ? 0 : toSigned(row[5]);
        quest.next_in_chain = schema.next_in_chain_col.empty() ? 0 : toUnsigned(row[6]);
        quest.editable = own.count(quest.entry) > 0;
        out.quests.push_back(std::move(quest));
      }

      auto const links = Q::detectLinkSchema(reader.columns());
      auto const read = [&](Q::RelationTable const& relation, Q::Giver::Kind kind, bool starts)
      {
        if (!relation.valid())
        {
          return;
        }
        for (auto const& row : reader.rows("SELECT " + quoteIdentifier(relation.owner_col) + ", "
                                           + quoteIdentifier(relation.quest_col) + " FROM " + quoteIdentifier(relation.table)))
        {
          out.links.push_back({{kind, toUnsigned(row[0])}, toUnsigned(row[1]), starts});
        }
      };
      read(links.npc_starters, Q::Giver::Kind::Npc, true);
      read(links.npc_enders, Q::Giver::Kind::Npc, false);
      read(links.object_starters, Q::Giver::Kind::Object, true);
      read(links.object_enders, Q::Giver::Kind::Object, false);
      return out;
    }

    std::vector<AreaTrigger> readAreaTriggers(Reader& reader)
    {
      std::vector<AreaTrigger> out;
      // tortoise-wow's areatrigger_template has no name column: read the id in its place, named below.
      bool const named = hasColumn(reader.columnsOf("areatrigger_template"), "name");
      for (auto const& row : latestRows(reader, "areatrigger_template", {"id", named ? "name" : "id", "map_id", "x", "y", "z", "radius"},
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
      if (!reader.columnsOf("areatrigger_involvedrelation").empty())
      {
        for (auto const& row : reader.rows("SELECT `id`, `quest` FROM `areatrigger_involvedrelation`"))
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
        auto const& columns = reader.columnsOf(table);
        auto const id = firstColumn(columns, {"id", "entry"});
        if (id.empty())
        {
          continue;
        }
        // The teleport / inn tables name their triggers: used when the template has no names (tortoise-wow).
        auto const name = named ? std::string() : firstColumn(columns, {"name"});
        for (auto const& row : reader.rows("SELECT " + quoteIdentifier(id) + ", " + quoteIdentifier(name.empty() ? id : name)
                                           + " FROM " + quoteIdentifier(table)))
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
      return out;
    }

    // NPC / object entries linked to the quest through a relation table.
    std::vector<Q::Giver> linkedGivers(Reader& reader, Q::RelationTable const& relation, Q::Giver::Kind kind, std::uint32_t quest)
    {
      std::vector<Q::Giver> out;
      if (!relation.valid())
      {
        return out;
      }
      for (auto const& row : reader.rows("SELECT " + quoteIdentifier(relation.owner_col) + " FROM " + quoteIdentifier(relation.table)
                                         + " WHERE " + equals(relation.quest_col, literal(quest)) + " ORDER BY 1"))
      {
        out.push_back({kind, toUnsigned(row[0])});
      }
      return out;
    }

    // A creature or chest that drops `item` for quests, with its chance (creatures first).
    std::optional<Q::QuestDrop> findDrop(Reader& reader, Q::LootSchema const& loot, std::uint32_t item)
    {
      auto const query = [&](Q::LootTableSchema const& table, std::string const& source_table, std::string const& source_entry,
                             std::string const& join) -> std::optional<SqlRow>
      {
        auto rows = reader.rows("SELECT s." + quoteIdentifier(source_entry) + ", l." + quoteIdentifier(table.entry_col)
                                + ", ABS(l." + quoteIdentifier(table.chance_col) + ") FROM " + quoteIdentifier(table.table)
                                + " l JOIN " + quoteIdentifier(source_table) + " s ON " + join + " WHERE l."
                                + quoteIdentifier(table.item_col) + " = " + literal(item) + " AND l."
                                + Q::questOnlyCondition(table) + " ORDER BY s." + quoteIdentifier(source_entry) + " LIMIT 1");
        return !rows.empty() ? std::optional<SqlRow>(rows.front()) : std::nullopt;
      };
      if (loot.creatureDrops())
      {
        if (auto row = query(loot.creature, "creature_template", loot.creature_entry_col,
                             "s." + quoteIdentifier(loot.creature_loot_col) + " = l." + quoteIdentifier(loot.creature.entry_col)))
        {
          return Q::QuestDrop{Q::QuestDrop::Source::Creature, toUnsigned((*row)[0]), item, toFloat((*row)[2]), toUnsigned((*row)[1])};
        }
      }
      if (loot.objectDrops())
      {
        if (auto row = query(loot.object, "gameobject_template", loot.object_entry_col,
                             "s." + quoteIdentifier(loot.object_loot_col) + " = l." + quoteIdentifier(loot.object.entry_col)
                             + " AND s." + quoteIdentifier(loot.object_type_col) + " = " + literal(Q::CHEST_OBJECT_TYPE)))
        {
          return Q::QuestDrop{Q::QuestDrop::Source::Object, toUnsigned((*row)[0]), item, toFloat((*row)[2]), toUnsigned((*row)[1])};
        }
      }
      return std::nullopt;
    }

    Q::ParsedScript loadScript(Reader& reader, Q::ScriptSchema const& schema, Q::ScriptWhen when, std::uint32_t script_id)
    {
      if (!schema.valid() || !script_id)
      {
        return {};
      }
      auto const& table = when == Q::ScriptWhen::Accepted ? schema.start_table : schema.end_table;
      std::vector<Q::ScriptRow> rows;
      for (auto& row : namedRows(reader, table, "WHERE `id` = " + literal(script_id)))
      {
        rows.push_back(std::move(row));
      }
      std::map<std::uint32_t, Q::SpokenText> texts;
      auto const ids = Q::spokenTextIds(rows);
      if (!ids.empty())
      {
        std::string const chat = schema.text_chat_type_col.empty() ? std::string("0") : quoteIdentifier(schema.text_chat_type_col);
        for (auto const& row : reader.rows("SELECT " + quoteIdentifier(schema.text_entry_col) + ", " + quoteIdentifier(schema.text_male_col)
                                           + ", " + chat + " FROM `broadcast_text` WHERE " + quoteIdentifier(schema.text_entry_col)
                                           + " IN (" + idList(ids) + ")"))
        {
          texts[toUnsigned(row[0])] = {row[1].value_or(std::string()), toUnsigned(row[2])};
        }
      }
      return Q::parseScript(rows, texts);
    }

    std::string collectItemsCondition(Q::QuestSchema const& schema, std::uint32_t item)
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

    // The facts planQuestSave needs about what `content` touches. Fills in the drops' current loot tables.
    Q::SaveInputs saveInputs(Reader& reader, QuestEditorData const& data, QuestList const& list, Q::QuestContent& content)
    {
      Q::SaveInputs inputs;
      inputs.chain = list.quests;

      std::vector<Q::Giver> linked = content.links.starters;
      linked.insert(linked.end(), content.links.enders.begin(), content.links.enders.end());
      auto const npcs = Q::npcEntries(linked);
      auto const& npc = data.db.npc;
      if (!npcs.empty() && !npc.npc_flags_col.empty())
      {
        for (auto const& row : reader.rows("SELECT " + quoteIdentifier(npc.entry_col) + ", " + quoteIdentifier(npc.npc_flags_col)
                                           + " FROM `creature_template` WHERE " + quoteIdentifier(npc.entry_col) + " IN (" + idList(npcs) + ")"))
        {
          inputs.npc_flags[toUnsigned(row[0])] = toUnsigned(row[1]);
        }
      }

      auto const& triggers = data.db.links.area_triggers;
      if (content.links.area_trigger && triggers.valid())
      {
        inputs.trigger_quests[content.links.area_trigger] =
          reader.number("SELECT " + quoteIdentifier(triggers.quest_col) + " FROM " + quoteIdentifier(triggers.table) + " WHERE "
                        + equals(triggers.owner_col, literal(content.links.area_trigger)));
      }

      auto const& loot = data.db.loot;
      for (auto& drop : content.drops)
      {
        bool const creature = drop.source == Q::QuestDrop::Source::Creature;
        drop.loot_id = creature
          ? reader.number("SELECT " + quoteIdentifier(loot.creature_loot_col) + " FROM `creature_template` WHERE "
                          + equals(loot.creature_entry_col, literal(drop.source_entry)) + " LIMIT 1")
          : reader.number("SELECT " + quoteIdentifier(loot.object_loot_col) + " FROM `gameobject_template` WHERE "
                          + equals(loot.object_entry_col, literal(drop.source_entry)) + " LIMIT 1");
      }

      if (data.db.scripts.valid())
      {
        inputs.next_text_id = nextId(reader, {{"broadcast_text", data.db.scripts.text_entry_col}}, Q::OWN_TEXT_START, UINT32_MAX);
      }
      return inputs;
    }

    Q::SavePlan plan(Reader& reader, QuestEditorData const& data, QuestList const& list, QuestSaveRequest const& request,
                     Q::QuestContent& content, Q::SaveInputs& inputs)
    {
      content = request.content;
      inputs = saveInputs(reader, data, list, content);
      inputs.names = request.names;
      return Q::planQuestSave(data.db, request.mode, request.entry, request.mode == Q::SaveMode::Copy ? request.source : 0,
                              content, data.content, data.stored, inputs);
    }

    void refuse(Q::SavePlan const& plan)
    {
      std::string message;
      for (auto const& problem : plan.problems)
      {
        message += (message.empty() ? "" : "\n") + problem;
      }
      require(plan.problems.empty(), message);
    }

    std::size_t spokenLines(std::vector<Q::ScriptAction> const& actions)
    {
      return std::count_if(actions.begin(), actions.end(), [](Q::ScriptAction const& action)
      {
        return action.kind == Q::ScriptAction::Kind::Say || action.kind == Q::ScriptAction::Kind::Yell;
      });
    }

    void snapshot(Database& db, std::string const& table, std::string const& where)
    {
      db.snapshotWhere(q(table), q(where));
    }

    std::string inList(std::string const& column, std::vector<std::uint32_t> const& ids)
    {
      return quoteIdentifier(column) + " IN (" + idList(ids) + ")";
    }

    // Before-images of every row the quest plan can touch; each condition selects the same rows before and
    // after the save, so restoring them undoes it exactly.
    void snapshotQuestRows(Database& db, Q::QuestDatabase const& schema, std::uint32_t entry, Q::SavePlan const& plan,
                           Q::QuestContent const& content, Q::QuestContent const& before, Q::QuestStored const& stored,
                           Q::SaveInputs const& inputs, std::vector<std::uint32_t> const& start_items)
    {
      std::vector<std::uint32_t> quests(plan.quests_changed.begin(), plan.quests_changed.end());
      quests.push_back(entry);
      snapshot(db, "quest_template", inList(schema.quest.entry_col, quests));

      auto const& links = schema.links;
      for (auto const* relation : {&links.npc_starters, &links.npc_enders, &links.object_starters, &links.object_enders,
                                   &links.area_triggers})
      {
        if (relation->valid())
        {
          snapshot(db, relation->table, equals(relation->quest_col, literal(entry)));
        }
      }
      if (links.area_triggers.valid())
      {
        for (auto const trigger : {content.links.area_trigger, before.links.area_trigger})
        {
          if (trigger)
          {
            snapshot(db, links.area_triggers.table, equals(links.area_triggers.owner_col, literal(trigger)));
          }
        }
      }
      if (!links.item_start_quest_col.empty() && !start_items.empty())
      {
        snapshot(db, "item_template", inList(links.item_entry_col, start_items));
      }

      auto const& loot = schema.loot;
      std::vector<std::uint32_t> creatures, objects;
      for (auto const* drops : {&content.drops, &before.drops})
      {
        for (auto const& drop : *drops)
        {
          bool const creature = drop.source == Q::QuestDrop::Source::Creature;
          auto const& table = creature ? loot.creature : loot.object;
          if (!table.valid())
          {
            continue;
          }
          std::uint32_t const loot_id = drop.loot_id ? drop.loot_id : drop.source_entry;
          snapshot(db, table.table, allOf({equals(table.entry_col, literal(loot_id)), equals(table.item_col, literal(drop.item))}));
          if (!drop.loot_id)
          {
            (creature ? creatures : objects).push_back(drop.source_entry);
          }
        }
      }
      creatures.insert(creatures.end(), plan.new_quest_givers.begin(), plan.new_quest_givers.end());
      if (!creatures.empty())
      {
        snapshot(db, "creature_template", inList(loot.creature_entry_col.empty() ? schema.npc.entry_col : loot.creature_entry_col, creatures));
      }
      if (!objects.empty())
      {
        snapshot(db, "gameobject_template", inList(loot.object_entry_col, objects));
      }

      if (schema.scripts.valid())
      {
        snapshot(db, schema.scripts.start_table, equals("id", literal(entry)));
        snapshot(db, schema.scripts.end_table, equals("id", literal(entry)));
        std::vector<std::uint32_t> texts = stored.accept_text_ids;
        texts.insert(texts.end(), stored.complete_text_ids.begin(), stored.complete_text_ids.end());
        auto const written = spokenLines(content.on_accept) + spokenLines(content.on_complete);
        for (std::uint32_t id = inputs.next_text_id; id < inputs.next_text_id + written; ++id)
        {
          texts.push_back(id);
        }
        if (!texts.empty())
        {
          snapshot(db, "broadcast_text", inList(schema.scripts.text_entry_col, texts));
        }
      }
    }

    // Records the quests, items and NPCs a quest plan changes in Local Changes (before anything is written).
    void trackQuestPlan(Database& db, std::uint32_t entry, Q::SavePlan const& plan, Q::QuestContent const& content,
                        std::vector<std::uint32_t> const& start_items)
    {
      db.track(EntityType::Quest, entry);
      for (auto const quest : plan.quests_changed)
      {
        if (db.owned("quest", quest))
        {
          db.track(EntityType::Quest, quest);
        }
      }
      for (auto const item : start_items)
      {
        if (db.owned("item", item))
        {
          db.track(EntityType::Item, item);
        }
      }
      std::vector<std::uint32_t> npcs = plan.new_quest_givers;
      for (auto const& drop : content.drops)
      {
        if (drop.source == Q::QuestDrop::Source::Creature && !drop.loot_id)
        {
          npcs.push_back(drop.source_entry);
        }
      }
      for (auto const npc : npcs)
      {
        if (db.owned("npc", npc))
        {
          db.track(EntityType::Npc, npc);
        }
      }
    }

    void run(Database& db, std::vector<std::string> const& statements, bool require_first_row, std::string const& vanished)
    {
      bool first = true;
      for (auto const& statement : statements)
      {
        auto const changed = db.execute(statement);
        require(!(first && require_first_row && changed == 0), vanished);
        first = false;
      }
    }

    std::vector<std::uint32_t> nonZero(std::initializer_list<std::uint32_t> ids)
    {
      std::vector<std::uint32_t> out;
      for (auto const id : ids)
      {
        if (id)
        {
          out.push_back(id);
        }
      }
      return out;
    }
  }

  std::vector<std::string> const& Layouts::columnsOf(Database& db, std::string const& table)
  {
    if (auto const found = _columns.find(table); found != _columns.end())
    {
      return found->second;
    }
    std::vector<std::string> columns;
    for (auto const& row : db.rows("SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
                                   "AND TABLE_NAME = " + literal(table) + " ORDER BY ORDINAL_POSITION"))
    {
      if (!row.empty() && row[0])
      {
        columns.push_back(*row[0]);
      }
    }
    return _columns.emplace(table, std::move(columns)).first->second;
  }

  ContentLists loadContentLists(Layouts& layouts)
  {
    Database db;
    Reader reader(db, layouts);
    ContentLists lists;

    auto const& creature_columns = reader.columnsOf("creature_template");
    auto const npc = Q::detectNpcFlagSchema(creature_columns);
    auto const name = firstColumn(creature_columns, {"name", "Name"});
    if (!npc.entry_col.empty() && !name.empty())
    {
      auto const flags = npc.npc_flags_col.empty() ? npc.entry_col : npc.npc_flags_col;
      for (auto const& row : latestRows(reader, "creature_template", {npc.entry_col, name, flags}))
      {
        lists.creatures.push_back({toUnsigned(row[0]), row[1].value_or(std::string()), npc.npc_flags_col.empty() ? 0u : toUnsigned(row[2]), 0});
      }
    }

    auto const& item_columns = reader.columnsOf("item_template");
    auto const quality = firstColumn(item_columns, {"quality", "Quality"});
    auto const display = firstColumn(item_columns, {"display_id", "displayid"});
    // Missing optional columns read the entry instead, and are ignored.
    for (auto const& row : latestRows(reader, "item_template", {"entry", "name", quality.empty() ? "entry" : quality,
                                                                display.empty() ? "entry" : display}))
    {
      lists.items.push_back({toUnsigned(row[0]), row[1].value_or(std::string()), quality.empty() ? 1u : toUnsigned(row[2]),
                             display.empty() ? 0u : toUnsigned(row[3])});
    }

    for (auto const& row : latestRows(reader, "gameobject_template", {"entry", "name", "type"}))
    {
      lists.objects.push_back({toUnsigned(row[0]), row[1].value_or(std::string()), toUnsigned(row[2]), 0});
    }
    for (auto const& row : latestRows(reader, "spell_template", {"entry", "name"}, "build"))
    {
      lists.spells.push_back({toUnsigned(row[0]), row[1].value_or(std::string()), 0, 0});
    }
    lists.area_triggers = readAreaTriggers(reader);
    lists.quests = readQuestList(reader);
    lists.own_quests = owned(reader, "quest");
    lists.own_items = owned(reader, "item");
    return lists;
  }

  QuestList loadQuestList(Layouts& layouts)
  {
    Database db;
    Reader reader(db, layouts);
    return readQuestList(reader);
  }

  QuestEditorData loadQuest(Layouts& layouts, std::uint32_t entry, QuestList const& list)
  {
    Database db;
    Reader reader(db, layouts);
    QuestEditorData out;
    out.db = Q::detectQuestDatabase(reader.columns());
    auto const& schema = out.db.quest;
    require(schema.valid(), "The local world database has no quest_template table the quest editor understands.");
    out.next_entry = nextQuestId(reader, out.db);

    // Typical rewards come from the game's own quests.
    if (!schema.xp_col.empty() && !schema.level_col.empty())
    {
      for (auto const& row : reader.rows("SELECT " + quoteIdentifier(schema.level_col) + ", " + quoteIdentifier(schema.xp_col)
                                         + " FROM `quest_template` WHERE " + quoteIdentifier(schema.xp_col) + " > 0 AND "
                                         + quoteIdentifier(schema.entry_col) + " NOT IN (SELECT entry FROM creator_content WHERE kind='quest')"))
      {
        out.xp_by_level[toUnsigned(row[0])].push_back(toUnsigned(row[1]));
      }
    }
    if (!entry)
    {
      return out;
    }

    auto const rows = namedRows(reader, "quest_template", "WHERE " + equals(schema.entry_col, literal(entry)) + newest(reader, "quest_template"));
    require(!rows.empty(), "There is no quest " + std::to_string(entry) + " in the local database.");
    auto& content = out.content;
    content.fields = Q::questFieldsFromRow(schema, rows.front());

    auto const& links = out.db.links;
    content.links.starters = linkedGivers(reader, links.npc_starters, Q::Giver::Kind::Npc, entry);
    auto objects = linkedGivers(reader, links.object_starters, Q::Giver::Kind::Object, entry);
    content.links.starters.insert(content.links.starters.end(), objects.begin(), objects.end());
    content.links.enders = linkedGivers(reader, links.npc_enders, Q::Giver::Kind::Npc, entry);
    objects = linkedGivers(reader, links.object_enders, Q::Giver::Kind::Object, entry);
    content.links.enders.insert(content.links.enders.end(), objects.begin(), objects.end());
    if (!links.item_start_quest_col.empty())
    {
      content.links.start_item = reader.number("SELECT " + quoteIdentifier(links.item_entry_col) + " FROM `item_template` WHERE "
                                               + equals(links.item_start_quest_col, literal(entry)) + " LIMIT 1");
    }
    if (links.area_triggers.valid())
    {
      content.links.area_trigger = reader.number("SELECT " + quoteIdentifier(links.area_triggers.owner_col) + " FROM "
                                                 + quoteIdentifier(links.area_triggers.table) + " WHERE "
                                                 + equals(links.area_triggers.quest_col, literal(entry)) + " LIMIT 1");
    }

    Q::ChainModel chain(list.quests);
    content.prerequisites = chain.prerequisitesOf(entry);
    content.exclusive_with = chain.exclusiveWith(entry);
    content.offers_next = content.fields.next_in_chain.value_or(0);

    for (auto const& collect : content.fields.collect.value_or(std::vector<Q::ItemCount>{}))
    {
      if (auto drop = findDrop(reader, out.db.loot, collect.id))
      {
        content.drops.push_back(*drop);
      }
      auto const condition = collectItemsCondition(schema, collect.id);
      if (!condition.empty() && reader.number("SELECT COUNT(*) FROM `quest_template` WHERE " + quoteIdentifier(schema.entry_col)
                                              + " <> " + literal(entry) + " AND (" + condition + ")") > 0)
      {
        out.stored.shared_items.insert(collect.id);
      }
    }

    auto const accept = loadScript(reader, out.db.scripts, Q::ScriptWhen::Accepted, content.fields.start_script.value_or(0));
    auto const complete = loadScript(reader, out.db.scripts, Q::ScriptWhen::Completed, content.fields.complete_script.value_or(0));
    content.on_accept = accept.actions;
    content.on_complete = complete.actions;
    content.scripts_complete = accept.complete && complete.complete;
    out.stored.accept_text_ids = accept.custom_text_ids;
    out.stored.complete_text_ids = complete.custom_text_ids;
    return out;
  }

  Q::SavePlan planQuestSave(Layouts& layouts, QuestEditorData const& data, QuestList const& list, QuestSaveRequest const& request)
  {
    Database db;
    Reader reader(db, layouts);
    Q::QuestContent content;
    Q::SaveInputs inputs;
    return plan(reader, data, list, request, content, inputs);
  }

  Q::SavePlan saveQuest(Layouts& layouts, QuestEditorData const& data, QuestList const& list, QuestSaveRequest const& request)
  {
    Database db;
    Reader reader(db, layouts);
    bool const editing = request.mode == Q::SaveMode::Edit;
    if (editing)
    {
      require(db.owned("quest", request.entry), "Only quests made in Noggit can be changed. Copy a game quest to edit it.");
    }
    else
    {
      require(reader.number("SELECT COUNT(*) FROM `quest_template` WHERE " + equals(data.db.quest.entry_col, literal(request.entry))) == 0,
              "Another quest was saved with this ID meanwhile. Close the editor and try again.");
    }
    Q::QuestContent content;
    Q::SaveInputs inputs;
    auto const result = plan(reader, data, list, request, content, inputs);
    refuse(result);

    auto const start_items = nonZero({content.links.start_item, editing ? data.content.links.start_item : 0u});
    Q::QuestContent const& before = editing ? data.content : Q::QuestContent{};
    trackQuestPlan(db, request.entry, result, content, start_items);
    snapshotQuestRows(db, data.db, request.entry, result, content, before, data.stored, inputs, start_items);
    db.snapshot("creator_content", "entry", request.entry);
    run(db, result.statements, result.require_first_row, "The quest to copy no longer exists in the local database.");
    db.mark("quest", request.entry);
    db.commit();
    return result;
  }

  Q::SavePlan planQuestDeletion(Layouts& layouts, QuestEditorData const& data, QuestList const& list, std::uint32_t quest,
                                Q::NameLookup const& names)
  {
    Database db;
    Reader reader(db, layouts);
    auto content = data.content;
    auto inputs = saveInputs(reader, data, list, content);
    inputs.names = names;
    return Q::planQuestDelete(data.db, quest, content, data.stored, inputs);
  }

  Q::SavePlan deleteQuest(Layouts& layouts, QuestEditorData const& data, QuestList const& list, std::uint32_t quest,
                          Q::NameLookup const& names)
  {
    Database db;
    Reader reader(db, layouts);
    require(db.owned("quest", quest), "Only quests made in Noggit can be deleted.");
    auto content = data.content;
    auto inputs = saveInputs(reader, data, list, content);
    inputs.names = names;
    auto const result = Q::planQuestDelete(data.db, quest, content, data.stored, inputs);
    refuse(result);

    // Items that start the quest stop doing so.
    std::vector<std::uint32_t> start_items;
    if (auto const& links = data.db.links; !links.item_start_quest_col.empty())
    {
      for (auto const& row : reader.rows("SELECT " + quoteIdentifier(links.item_entry_col) + " FROM `item_template` WHERE "
                                         + equals(links.item_start_quest_col, literal(quest))))
      {
        start_items.push_back(toUnsigned(row[0]));
      }
    }
    trackQuestPlan(db, quest, result, Q::QuestContent{}, start_items);
    Q::QuestContent nothing;
    snapshotQuestRows(db, data.db, quest, result, nothing, content, data.stored, inputs, start_items);
    db.snapshot("creator_content", "entry", quest);
    run(db, result.statements, false, {});
    db.exec("DELETE FROM creator_content WHERE kind='quest' AND entry=" + QString::number(quest));
    db.commit();
    return result;
  }

  void saveChain(Layouts& layouts, std::map<std::uint32_t, Q::ChainQuest> const& changes)
  {
    if (changes.empty())
    {
      return;
    }
    Database db;
    Reader reader(db, layouts);
    auto const schema = Q::detectQuestSchema(reader.columnsOf("quest_template"));
    std::vector<std::string> statements;
    std::vector<std::uint32_t> quests;
    for (auto const& [entry, quest] : changes)
    {
      require(db.owned("quest", entry), "Only quests made in Noggit can be linked this way.");
      Q::QuestFields fields;
      fields.prev_quest = quest.prev_quest;
      fields.next_quest = quest.next_quest;
      fields.exclusive_group = quest.exclusive_group;
      fields.next_in_chain = quest.next_in_chain;
      if (auto update = Q::buildQuestUpdate(schema, entry, fields); !update.empty())
      {
        statements.push_back(std::move(update));
        quests.push_back(entry);
      }
    }
    require(!statements.empty(), "This database keeps quest chains in a table the editor cannot change.");
    for (auto const quest : quests)
    {
      db.track(EntityType::Quest, quest);
    }
    snapshot(db, "quest_template", inList(schema.entry_col, quests));
    run(db, statements, false, {});
    db.commit();
  }

  ItemEditorData loadItem(Layouts& layouts, std::uint32_t entry)
  {
    Database db;
    Reader reader(db, layouts);
    ItemEditorData out;
    out.schema = I::detectItemSchema(reader.columnsOf("item_template"));
    require(out.schema.valid(), "The local world database has no item_template table the item editor understands.");
    out.next_entry = nextId(reader, {{"item_template", out.schema.entry_col}}, FIRST_ID, ITEM_ID_LIMIT);
    if (!entry)
    {
      return out;
    }
    auto const rows = reader.rows("SELECT " + columnList(out.schema.columns) + " FROM `item_template` WHERE "
                                  + equals(out.schema.entry_col, literal(entry)) + newest(reader, "item_template"));
    require(!rows.empty(), "There is no item " + std::to_string(entry) + " in the local database.");
    out.values = I::itemFieldsFromRow(out.schema, rows.front());

    // Where it is used.
    auto const quest = Q::detectQuestSchema(reader.columnsOf("quest_template"));
    std::string uses;
    auto const use = [&](std::string const& column)
    {
      if (!column.empty())
      {
        uses += (uses.empty() ? "" : " OR ") + equals(column, literal(entry));
      }
    };
    for (auto const& column : quest.collect_cols) use(column);
    for (auto const& column : quest.reward_cols) use(column);
    for (auto const& column : quest.choice_cols) use(column);
    use(quest.source_item_col);
    if (!uses.empty())
    {
      for (auto const& row : reader.rows("SELECT DISTINCT " + quoteIdentifier(quest.entry_col) + " FROM `quest_template` WHERE " + uses))
      {
        out.uses.quests.push_back(toUnsigned(row[0]));
      }
    }
    for (auto const* table : {"creature_loot_template", "gameobject_loot_template"})
    {
      if (hasColumn(reader.columnsOf(table), "item"))
      {
        out.uses.loot_rows += reader.number("SELECT COUNT(*) FROM " + quoteIdentifier(table) + " WHERE `item` = " + literal(entry));
      }
    }
    if (hasColumn(reader.columnsOf("npc_vendor"), "item"))
    {
      out.uses.vendor_rows = reader.number("SELECT COUNT(*) FROM `npc_vendor` WHERE `item` = " + literal(entry));
    }
    return out;
  }

  std::uint32_t createItem(Layouts& layouts, ItemEditorData const& data, I::ItemFields const& fields)
  {
    Database db;
    Reader reader(db, layouts);
    auto const entry = nextId(reader, {{"item_template", data.schema.entry_col}}, FIRST_ID, ITEM_ID_LIMIT);
    db.track(EntityType::Item, entry);
    snapshot(db, "item_template", equals(data.schema.entry_col, literal(entry)));
    db.snapshot("creator_content", "entry", entry);
    run(db, {I::buildItemInsert(data.schema, entry, fields)}, true, "The item could not be created.");
    db.mark("item", entry);
    db.commit();
    return entry;
  }

  void updateItem(Layouts& layouts, ItemEditorData const& data, std::uint32_t entry, I::ItemFields const& fields)
  {
    auto const update = I::buildItemUpdate(data.schema, entry, fields);
    if (update.empty())
    {
      return;
    }
    Database db;
    require(db.owned("item", entry), "Only items made in Noggit can be changed.");
    db.track(EntityType::Item, entry);
    snapshot(db, "item_template", equals(data.schema.entry_col, literal(entry)));
    run(db, {update}, false, {});
    db.commit();
  }

  void deleteItem(Layouts& layouts, ItemEditorData const& data, std::uint32_t entry)
  {
    Database db;
    Reader reader(db, layouts);
    require(db.owned("item", entry), "Only items made in Noggit can be deleted.");
    auto const plan = I::planItemDelete(data.schema, entry, data.uses);
    std::string problems;
    for (auto const& problem : plan.problems)
    {
      problems += (problems.empty() ? "" : "\n") + problem;
    }
    require(plan.problems.empty(), problems);
    db.track(EntityType::Item, entry);
    for (auto const* table : {"creature_loot_template", "gameobject_loot_template", "npc_vendor"})
    {
      if (hasColumn(reader.columnsOf(table), "item"))
      {
        snapshot(db, table, equals("item", literal(entry)));
      }
    }
    snapshot(db, "item_template", equals(data.schema.entry_col, literal(entry)));
    db.snapshot("creator_content", "entry", entry);
    run(db, plan.statements, false, {});
    db.exec("DELETE FROM creator_content WHERE kind='item' AND entry=" + QString::number(entry));
    db.commit();
  }
}
