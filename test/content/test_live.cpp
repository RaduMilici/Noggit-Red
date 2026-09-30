// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// End-to-end: the plans the editors produce, run against a scratch copy of real world-database tables
// (see LiveDb.hpp). Proves the generated SQL is accepted by the real schema and does what the editor says.

#include "LiveDb.hpp"
#include "TestSupport.hpp"

#include <mysql/content_db.h>
#include <noggit/item/ItemTemplateSql.hpp>
#include <noggit/npc/NpcSavePlan.hpp>
#include <noggit/quest/QuestSavePlan.hpp>

#include <QtTest/QtTest>

#include <sstream>

using namespace Noggit;

class LiveTest : public QObject
{
  Q_OBJECT

private:
  LiveDb _db;

  Npc::ColumnsOf columnsOf()
  {
    return [this](std::string const& table) { return _db.columnsOf(table); };
  }

  std::uint32_t count(std::string const& table, std::string const& where)
  {
    return static_cast<std::uint32_t>(std::stoul(_db.scalar("SELECT COUNT(*) FROM `" + table + "` WHERE " + where)));
  }

private slots:
  void initTestCase()
  {
    auto const error = _db.open();
    QVERIFY2(error.empty(), error.c_str());
    if (!_db.connected())
    {
      return;
    }
    // Catherine Leland (5494, a vendor, + the NPCs sharing her dialogue), Stormwind Guard (1423), Marshal McBride (197),
    // Young Wolf (299);
    // the Northshire quests 7 -> 15 -> 21; the Sunken Chest (32, loot 1683) and a goober (1721).
    QVERIFY(_db.copyTable("creature_template", "entry IN (197, 299, 1423, 5494) OR gossip_menu_id = 685"));
    QVERIFY(_db.copyTable("npc_vendor", "entry = 5494"));
    QVERIFY(_db.copyTable("npc_trainer"));
    QVERIFY(_db.copyTable("quest_template", "entry IN (7, 15, 21)"));
    for (auto const* table : {"creature_questrelation", "creature_involvedrelation", "gameobject_questrelation",
                              "gameobject_involvedrelation"})
    {
      QVERIFY(_db.copyTable(table, "quest IN (7, 15, 21)"));
    }
    QVERIFY(_db.copyTable("areatrigger_involvedrelation", "quest = 6421"));
    QVERIFY(_db.copyTable("item_template", "entry IN (750, 1309, 1468)"));
    QVERIFY(_db.copyTable("creature_loot_template", "entry = 299"));
    QVERIFY(_db.copyTable("gameobject_loot_template", "entry = 1683"));
    QVERIFY(_db.copyTable("gameobject_template", "entry IN (32, 1721)"));
    for (auto const* table : {"quest_start_scripts", "quest_end_scripts", "quest_greeting", "creature_addon",
                              "creature_movement", "game_event_creature", "pool_creature", "creature_linking"})
    {
      QVERIFY(_db.copyTable(table));
    }
    // The vendor's dialogue: menu 685, shared by 22 game NPCs.
    std::string const src = "`" + _db.source() + "`";
    QVERIFY(_db.copyTable("gossip_menu", "entry = 685"));
    QVERIFY(_db.copyTable("npc_text", "ID IN (SELECT text_id FROM " + src + ".gossip_menu WHERE entry = 685)"));
    QVERIFY(_db.copyTable("broadcast_text", "entry IN (SELECT BroadcastTextID0 FROM " + src + ".npc_text WHERE ID IN "
                                            "(SELECT text_id FROM " + src + ".gossip_menu WHERE entry = 685))"));
    QVERIFY(_db.copyTable("creature", "id = 299 LIMIT 3"));
    for (auto const* table : {"areatrigger_template", "areatrigger_teleport", "areatrigger_tavern", "areatrigger_scripts",
                              "areatrigger_bg_entrance"})
    {
      QVERIFY(_db.copyTable(table, "1"));
    }
  }

  void cleanupTestCase()
  {
    _db.close();
  }

  void npcCreateEditDelete()
  {
    REQUIRE_LIVE_DB(_db);
    auto const db = Npc::detectNpcDatabase(columnsOf());
    QVERIFY(db.dialogue.greetingSupported());

    Npc::NpcContent content;
    content.fields.name = "Mira Stonehand";
    content.fields.subname = "Odd Goods";
    content.copy_related = {"npc_vendor"};
    content.dialogue = {"Welcome, $N. Browse all you like.", "Work is waiting, $N."};
    // The copy starts on the vendor's shared menu (685): the greeting must go to a new menu of its own.
    Npc::DialogueStored shared;
    shared.menu = 685;
    shared.menu_shared = true;
    auto const plan = Npc::planNpcSave(db, Npc::NpcSaveMode::Create, 90001, 5494, content, shared, {60500, 900000, 900000});
    QVERIFY(plan.problems.empty());
    QVERIFY(_db.execAll(plan.statements));
    QCOMPARE(_db.scalar("SELECT CONCAT_WS('|', patch, name, subname, gossip_menu_id) FROM creature_template WHERE entry = 90001"),
             std::string("0|Mira Stonehand|Odd Goods|60500"));
    QCOMPARE(count("npc_vendor", "entry = 90001"), count("npc_vendor", "entry = 5494"));
    QCOMPARE(_db.scalar("SELECT b.male_text FROM gossip_menu g JOIN npc_text t ON t.ID = g.text_id "
                        "JOIN broadcast_text b ON b.entry = t.BroadcastTextID0 WHERE g.entry = 60500"),
             std::string("Welcome, $N. Browse all you like."));
    QCOMPARE(count("gossip_menu", "entry = 685"), 1u); // the game NPCs' menu is untouched
    QCOMPARE(_db.scalar("SELECT content_default FROM quest_greeting WHERE entry = 90001"), std::string("Work is waiting, $N."));

    // Spawn it twice, then delete it with everything that hangs off it.
    QVERIFY(_db.exec("INSERT INTO creature (guid, id, map) VALUES (990001, 90001, 0), (990002, 90001, 0)"));
    QVERIFY(_db.exec("INSERT INTO creature_addon (guid) VALUES (990001)"));
    Npc::DialogueStored own;
    own.menu = 60500;
    own.npc_text = 900000;
    own.broadcast = 900000;
    auto const removal = Npc::planNpcDelete(db, 90001, own, 2, 0);
    QVERIFY(_db.execAll(removal.statements));
    QCOMPARE(count("creature_template", "entry = 90001"), 0u);
    QCOMPARE(count("creature", "id = 90001"), 0u);
    QCOMPARE(count("creature_addon", "guid = 990001"), 0u);
    QCOMPARE(count("npc_vendor", "entry = 90001"), 0u);
    QCOMPARE(count("gossip_menu", "entry = 60500"), 0u);
    QCOMPARE(count("broadcast_text", "entry = 900000"), 0u);
    QCOMPARE(count("quest_greeting", "entry = 90001"), 0u);
    QCOMPARE(count("creature", "id = 299"), 3u); // other spawns stay
  }

  void questWithEverything()
  {
    REQUIRE_LIVE_DB(_db);
    auto const db = Quest::detectQuestDatabase(columnsOf());
    Quest::QuestContent content;
    auto& f = content.fields;
    f.title = "Into the Den";
    f.details = "The wolves have a den nearby, $N.";
    f.objectives = "Search the chest, free the prisoner and thin out the wolves.";
    f.level = 5;
    f.min_level = 3;
    f.type = 1;
    f.suggested_players = 2;
    f.time_limit = 1800;
    f.classes = 1 | 8;
    f.skill = 185;
    f.skill_value = 10;
    f.min_rep = Quest::Reputation{72, 3000};
    f.source_item = Quest::ItemCount{1468, 1};
    f.xp = 450;
    f.money = 125;
    f.reward_spell = 133;
    f.targets = std::vector<Quest::Target>{{Quest::Target::Kind::Creature, 299, 6, "", 0},
                                           {Quest::Target::Kind::Object, 1721, 1, "Prisoner freed", 3366}};
    f.collect = std::vector<Quest::ItemCount>{{750, 4}, {1309, 1}};
    f.rep_rewards = std::vector<Quest::Reputation>{{72, 250}};
    content.links.starters = {{Quest::Giver::Kind::Npc, 197}, {Quest::Giver::Kind::Object, 32}};
    content.links.enders = {{Quest::Giver::Kind::Npc, 1423}};
    content.links.start_item = 1468;
    content.links.area_trigger = 171; // no quest uses it (2946 completes "Boulderslide Ravine")
    content.drops = {{Quest::QuestDrop::Source::Creature, 299, 750, 40.0f, 299},
                     {Quest::QuestDrop::Source::Object, 32, 1309, 100.0f, 1683}};
    content.prerequisites = {{7}, Quest::Prerequisites::Mode::Any};
    Quest::ScriptAction say, complete;
    say.text = "Stay sharp, $N.";
    complete.kind = Quest::ScriptAction::Kind::CompleteQuest;
    complete.wait = 2;
    content.on_complete = {say, complete};

    Quest::SaveInputs inputs;
    inputs.chain = {{7, "Kobold Camp Cleanup", 2, 0, 0, 0, 0, false}};
    inputs.npc_flags = {{197, 3}, {1423, 1}};
    inputs.trigger_quests = {{2946, 6421}};
    auto const plan = Quest::planQuestSave(db, Quest::SaveMode::Create, 90010, 0, content, {}, {}, inputs);
    QVERIFY2(plan.problems.empty(), plan.problems.empty() ? "" : plan.problems.front().c_str());
    QVERIFY(_db.execAll(plan.statements));

    QCOMPARE(_db.scalar("SELECT CONCAT_WS('|', patch, Title, Type, SuggestedPlayers, LimitTime, RequiredClasses, "
                        "RequiredSkill, RequiredSkillValue, RequiredMinRepFaction, RequiredMinRepValue, PrevQuestId, "
                        "SrcItemId, ReqCreatureOrGOId2, ReqSpellCast2, RewRepFaction1, RewRepValue1, SpecialFlags, "
                        "StartScript, CompleteScript) FROM quest_template WHERE entry = 90010"),
             std::string("0|Into the Den|1|2|1800|9|185|10|72|3000|7|1468|-1721|3366|72|250|2|0|90010"));
    QCOMPARE(count("gameobject_questrelation", "id = 32 AND quest = 90010"), 1u);
    QCOMPARE(count("creature_involvedrelation", "id = 1423 AND quest = 90010"), 1u);
    QCOMPARE(count("areatrigger_involvedrelation", "id = 171 AND quest = 90010"), 1u);
    QCOMPARE(count("areatrigger_involvedrelation", "id = 2946 AND quest = 6421"), 1u); // untouched
    QCOMPARE(_db.scalar("SELECT start_quest FROM item_template WHERE entry = 1468"), std::string("90010"));
    QCOMPARE(_db.scalar("SELECT ChanceOrQuestChance FROM creature_loot_template WHERE entry = 299 AND item = 750"), std::string("-40"));
    QCOMPARE(_db.scalar("SELECT ChanceOrQuestChance FROM gameobject_loot_template WHERE entry = 1683 AND item = 1309"), std::string("-100"));
    QCOMPARE(_db.scalar("SELECT npc_flags & 2 FROM creature_template WHERE entry = 1423 ORDER BY patch DESC LIMIT 1"), std::string("2"));

    // The script reads back as it was written.
    std::vector<Quest::ScriptRow> rows;
    auto const columns = _db.columnsOf("quest_end_scripts");
    for (auto const& row : _db.rows("SELECT * FROM quest_end_scripts WHERE id = 90010"))
    {
      Quest::ScriptRow named;
      for (std::size_t i = 0; i < columns.size(); ++i)
      {
        named[columns[i]] = row[i];
      }
      rows.push_back(named);
    }
    std::map<std::uint32_t, Quest::SpokenText> texts;
    for (auto const id : Quest::spokenTextIds(rows))
    {
      texts[id] = {_db.scalar("SELECT male_text FROM broadcast_text WHERE entry = " + std::to_string(id)),
                   static_cast<std::uint32_t>(std::stoul(_db.scalar("SELECT chat_type FROM broadcast_text WHERE entry = " + std::to_string(id))))};
    }
    auto const parsed = Quest::parseScript(rows, texts);
    QVERIFY(parsed.complete);
    QCOMPARE(parsed.actions.size(), std::size_t(2));
    QVERIFY(parsed.actions[0] == say);
    QCOMPARE(parsed.actions[1].id, std::uint32_t(90010)); // completes this quest
  }

  void allOfChainAndDelete()
  {
    REQUIRE_LIVE_DB(_db);
    auto const db = Quest::detectQuestDatabase(columnsOf());
    // Two custom quests (90011, 90012), then 90013 needing both.
    Quest::SaveInputs inputs;
    inputs.chain = {{7, "", 2, 0, 0, 0, 0, false}, {90010, "", 5, 7, 0, 0, 0, true}};
    for (std::uint32_t entry : {90011u, 90012u})
    {
      Quest::QuestContent content;
      content.fields.title = "Part " + std::to_string(entry);
      auto const plan = Quest::planQuestSave(db, Quest::SaveMode::Create, entry, 0, content, {}, {}, inputs);
      QVERIFY(_db.execAll(plan.statements));
      inputs.chain.push_back({entry, "", 0, 0, 0, 0, 0, true});
    }
    Quest::QuestContent finale;
    finale.fields.title = "Finale";
    finale.prerequisites = {{90011, 90012}, Quest::Prerequisites::Mode::All};
    auto const plan = Quest::planQuestSave(db, Quest::SaveMode::Create, 90013, 0, finale, {}, {}, inputs);
    QVERIFY(plan.problems.empty());
    QVERIFY(_db.execAll(plan.statements));
    QCOMPARE(_db.scalar("SELECT GROUP_CONCAT(CONCAT(entry, ':', NextQuestId, ':', ExclusiveGroup) ORDER BY entry) "
                        "FROM quest_template WHERE entry IN (90011, 90012)"),
             std::string("90011:90013:-90011,90012:90013:-90011"));

    // Deleting 90012 unlinks it: 90013 then needs 90011 alone (stored as its PrevQuestId).
    inputs.chain.push_back({90013, "", 0, 0, 0, 0, 0, true});
    inputs.chain[2].next_quest = 90013;
    inputs.chain[2].exclusive_group = -90011;
    inputs.chain[3].next_quest = 90013;
    inputs.chain[3].exclusive_group = -90011;
    auto const removal = Quest::planQuestDelete(db, 90012, {}, {}, inputs);
    QVERIFY(removal.problems.empty());
    QVERIFY(_db.execAll(removal.statements));
    QCOMPARE(count("quest_template", "entry = 90012"), 0u);
    QCOMPARE(_db.scalar("SELECT CONCAT_WS('|', PrevQuestId, NextQuestId, ExclusiveGroup) FROM quest_template WHERE entry = 90013"),
             std::string("90011|0|0"));
    QCOMPARE(_db.scalar("SELECT CONCAT_WS('|', NextQuestId, ExclusiveGroup) FROM quest_template WHERE entry = 90011"),
             std::string("0|0"));
  }

  void itemCreateAndDelete()
  {
    REQUIRE_LIVE_DB(_db);
    auto const schema = Item::detectItemSchema(_db.columnsOf("item_template"));
    Item::ItemFields fields;
    fields.name = "Wolf Pelt";
    fields.quality = 1;
    fields.item_class = 12;
    fields.bonding = Item::BONDING_QUEST_ITEM;
    fields.stackable = 10;
    QVERIFY(_db.exec(Item::buildItemInsert(schema, 90100, fields)));
    QVERIFY(_db.exec(Item::buildItemInsert(schema, 90101, fields, 750)));
    QCOMPARE(_db.scalar("SELECT CONCAT_WS('|', name, class, bonding, stackable, display_id) FROM item_template WHERE entry = 90101"),
             std::string("Wolf Pelt|12|4|10|1116")); // display kept from the copied item
    QVERIFY(_db.exec("INSERT INTO creature_loot_template (entry, item, ChanceOrQuestChance) VALUES (299, 90100, -30)"));
    auto const plan = Item::planItemDelete(schema, 90100, {{}, 1, 0});
    QVERIFY(_db.execAll(plan.statements));
    QCOMPARE(count("item_template", "entry = 90100"), 0u);
    QCOMPARE(count("creature_loot_template", "item = 90100"), 0u);
  }
  // --- the database layer (src/mysql/content_db.cpp) on the same scratch schema ---

  void contentDbLists()
  {
    REQUIRE_LIVE_DB(_db);
    std::string error;
    auto db = mysql::content::Database::open(&error);
    QVERIFY2(db, error.c_str());
    mysql::content::QuestList list;
    QVERIFY(mysql::content::questList(*db, list, &error));
    auto const has = [&](std::uint32_t entry)
    {
      return std::any_of(list.quests.begin(), list.quests.end(), [&](auto const& q) { return q.entry == entry; });
    };
    QVERIFY(has(7) && has(90010) && has(90013) && !has(90012));
    QVERIFY(std::any_of(list.links.begin(), list.links.end(), [](auto const& link)
    {
      return link.quest == 90010 && link.starts && link.giver.kind == Quest::Giver::Kind::Object && link.giver.entry == 32;
    }));
    auto const items = mysql::content::items(*db);
    auto const meat = std::find_if(items.begin(), items.end(), [](auto const& i) { return i.entry == 750; });
    QVERIFY(meat != items.end() && meat->name == "Tough Wolf Meat" && meat->display == 1116);
    auto const objects = mysql::content::objects(*db);
    QVERIFY(std::any_of(objects.begin(), objects.end(), [](auto const& o) { return o.entry == 32 && o.kind == 3; }));
    auto const triggers = mysql::content::areaTriggers(*db);
    QVERIFY(triggers.size() > 400);
    auto const trigger = [&](std::uint32_t id) { return *std::find_if(triggers.begin(), triggers.end(), [&](auto const& t) { return t.id == id; }); };
    QCOMPARE(trigger(2946).quest, std::uint32_t(6421));
    QCOMPARE(trigger(171).quest, std::uint32_t(90010));
    QVERIFY(std::any_of(triggers.begin(), triggers.end(), [](auto const& t) { return t.other_use; }));
  }

  void contentDbLoadsQuest()
  {
    REQUIRE_LIVE_DB(_db);
    auto db = mysql::content::Database::open();
    mysql::content::QuestList list;
    QVERIFY(mysql::content::questList(*db, list));
    mysql::content::QuestEditorData data;
    std::string error;
    QVERIFY2(mysql::content::loadQuest(*db, 90010, list, data, &error), error.c_str());
    auto const& c = data.content;
    QCOMPARE(*c.fields.title, std::string("Into the Den"));
    QCOMPARE(c.links.starters.size(), std::size_t(2));
    QCOMPARE(c.links.enders.size(), std::size_t(1));
    QCOMPARE(c.links.start_item, std::uint32_t(1468));
    QCOMPARE(c.links.area_trigger, std::uint32_t(171));
    QCOMPARE(c.prerequisites.quests, std::vector<std::uint32_t>{7});
    QCOMPARE(c.drops.size(), std::size_t(2));
    QVERIFY(c.drops[0].source == Quest::QuestDrop::Source::Creature && c.drops[0].source_entry == 299
            && c.drops[0].loot_id == 299 && c.drops[0].chance == 40.0f);
    QVERIFY(c.drops[1].source == Quest::QuestDrop::Source::Object && c.drops[1].source_entry == 32
            && c.drops[1].loot_id == 1683);
    QVERIFY(c.scripts_complete);
    QCOMPARE(c.on_complete.size(), std::size_t(2));
    QCOMPARE(c.on_complete[0].text, std::string("Stay sharp, $N."));
    QCOMPARE(data.stored.complete_text_ids.size(), std::size_t(1));
    QVERIFY(data.next_entry >= 90014);
    QVERIFY(!data.xp_by_level.empty());
  }

  void contentDbEditsQuest()
  {
    REQUIRE_LIVE_DB(_db);
    auto db = mysql::content::Database::open();
    mysql::content::QuestList list;
    QVERIFY(mysql::content::questList(*db, list));
    mysql::content::QuestEditorData data;
    QVERIFY(mysql::content::loadQuest(*db, 90010, list, data));

    auto edited = data.content;
    edited.fields.title = "Into the Den, Again";
    edited.drops.pop_back();                 // the chest no longer gives the item
    edited.on_complete.front().text = "Well done, $N.";
    auto const inputs = mysql::content::saveInputs(*db, data, list, edited);
    auto const plan = Quest::planQuestSave(data.db, Quest::SaveMode::Edit, 90010, 0, edited, data.content, data.stored, inputs);
    QVERIFY(plan.problems.empty());
    auto const result = db->execute(plan.statements, plan.cleanup, plan.require_first_row);
    QVERIFY2(result.ok, result.error.c_str());

    mysql::content::QuestEditorData reloaded;
    QVERIFY(mysql::content::loadQuest(*db, 90010, list, reloaded));
    QCOMPARE(*reloaded.content.fields.title, std::string("Into the Den, Again"));
    QCOMPARE(reloaded.content.drops.size(), std::size_t(1));
    QCOMPARE(count("gameobject_loot_template", "entry = 1683 AND item = 1309"), 0u);
    QCOMPARE(reloaded.content.on_complete.front().text, std::string("Well done, $N."));
    // The old spoken line was replaced, not left behind.
    QCOMPARE(count("broadcast_text", "entry >= 900000"), 1u);
  }

  void contentDbSnapshotReplays()
  {
    REQUIRE_LIVE_DB(_db);
    auto db = mysql::content::Database::open();
    auto const schema = Quest::detectQuestDatabase(db->columns());
    auto const snapshot = mysql::content::questSnapshot(*db, schema, 90010);
    QVERIFY(contains(snapshot, "INSERT INTO `quest_template`"));
    QVERIFY(contains(snapshot, "INSERT INTO `quest_end_scripts`"));

    std::vector<std::string> statements;
    std::stringstream lines(snapshot);
    std::string line, statement;
    while (std::getline(lines, line))
    {
      if (line.rfind("--", 0) == 0 || line.empty())
      {
        continue;
      }
      statement += (statement.empty() ? "" : "\n") + line;
      if (!line.empty() && line.back() == ';')
      {
        statements.push_back(statement.substr(0, statement.size() - 1));
        statement.clear();
      }
    }
    mysql::content::QuestList list;
    QVERIFY(mysql::content::questList(*db, list));
    mysql::content::QuestEditorData before;
    QVERIFY(mysql::content::loadQuest(*db, 90010, list, before));
    for (int pass = 0; pass < 2; ++pass) // repeatable
    {
      QVERIFY(_db.execAll(statements));
    }
    mysql::content::QuestEditorData after;
    QVERIFY(mysql::content::loadQuest(*db, 90010, list, after));
    QCOMPARE(*after.content.fields.title, *before.content.fields.title);
    QVERIFY(after.content.on_complete == before.content.on_complete);
    QCOMPARE(after.content.drops.size(), before.content.drops.size());
    QCOMPARE(after.content.links.starters.size(), before.content.links.starters.size());
    QCOMPARE(count("broadcast_text", "entry >= 900000"), 1u);
  }

  void contentDbLoadsNpcAndItem()
  {
    REQUIRE_LIVE_DB(_db);
    auto db = mysql::content::Database::open();
    mysql::content::NpcEditorData npc;
    std::string error;
    QVERIFY2(mysql::content::loadNpc(*db, 5494, npc, &error), error.c_str());
    QCOMPARE(*npc.values.name, std::string("Catherine Leland"));
    QCOMPARE(*npc.values.gossip_menu, std::uint32_t(685));
    QVERIFY(npc.dialogue.menu_shared);
    QVERIFY(!npc.dialogue.dialogue.greeting.empty());
    QVERIFY(npc.related_rows.at("npc_vendor") > 0);
    QVERIFY(npc.next_entry >= 90000);
    QVERIFY(npc.dialogue_ids.next_menu > 685);

    mysql::content::ItemEditorData item;
    QVERIFY(mysql::content::loadItem(*db, 750, item, &error));
    QCOMPARE(*item.values.name, std::string("Tough Wolf Meat"));
    QCOMPARE(item.uses.quests, std::vector<std::uint32_t>{90010});
  }
};

int runLiveTests(int argc, char** argv)
{
  LiveTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "test_live.moc"
