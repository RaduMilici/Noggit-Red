// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "TestSupport.hpp"
#include "TortoiseSchema.hpp"
#include "VmangosSchema.hpp"

#include <noggit/quest/QuestCatalog.hpp>
#include <noggit/quest/QuestLinksSql.hpp>
#include <noggit/quest/QuestLootSql.hpp>
#include <noggit/quest/QuestScripts.hpp>
#include <noggit/quest/QuestSavePlan.hpp>
#include <noggit/quest/QuestTemplateSql.hpp>

#include <QtTest/QtTest>

using namespace Noggit::Quest;

namespace
{
  // Parses "UPDATE ... SET `a` = 1, `b` = 'x' WHERE ..." into column -> literal (literals without ", ").
  std::map<std::string, std::optional<std::string>> assignedValues(std::string const& update)
  {
    std::map<std::string, std::optional<std::string>> out;
    auto const set = update.find(" SET ") + 5;
    auto const where = update.rfind(" WHERE ");
    std::string const body = update.substr(set, where - set);
    std::size_t position = 0;
    while (position < body.size())
    {
      auto const name_start = body.find('`', position) + 1;
      auto const name_end = body.find('`', name_start);
      auto const value_start = name_end + 4; // "` = "
      std::size_t value_end;
      if (body[value_start] == '\'')
      {
        value_end = value_start + 1;
        while (!(body[value_end] == '\'' && body[value_end - 1] != '\\'))
        {
          ++value_end;
        }
        ++value_end;
        out[body.substr(name_start, name_end - name_start)] = body.substr(value_start + 1, value_end - value_start - 2);
      }
      else
      {
        value_end = body.find(", ", value_start);
        value_end = value_end == std::string::npos ? body.size() : value_end;
        out[body.substr(name_start, name_end - name_start)] = body.substr(value_start, value_end - value_start);
      }
      position = value_end + 2;
    }
    return out;
  }
}

class QuestTest : public QObject
{
  Q_OBJECT

private slots:
  void detectsVmangos()
  {
    auto const db = detectQuestDatabase(vmangosColumns);
    auto const& q = db.quest;
    QVERIFY(q.valid());
    QCOMPARE(q.type_col, std::string("Type"));
    QCOMPARE(q.time_limit_col, std::string("LimitTime"));
    QCOMPARE(q.skill_col, std::string("RequiredSkill"));
    QCOMPARE(q.next_quest_col, std::string("NextQuestId"));
    QCOMPARE(q.source_item_col, std::string("SrcItemId"));
    QCOMPARE(q.target_spell_cols[3], std::string("ReqSpellCast4"));
    QCOMPARE(q.rep_reward_faction_cols[4], std::string("RewRepFaction5"));
    QVERIFY(db.links.object_starters.valid() && db.links.area_triggers.valid());
    QCOMPARE(db.links.item_start_quest_col, std::string("start_quest"));
    QVERIFY(db.loot.creatureDrops() && db.loot.objectDrops());
    QVERIFY(db.scripts.valid());
  }

  void detectsTortoise()
  {
    auto const db = detectQuestDatabase(tortoiseColumns);
    auto const& q = db.quest;
    QVERIFY(q.valid());
    QVERIFY(q.patch_col.empty());
    QCOMPARE(q.xp_col, std::string("RewXP"));
    QCOMPARE(q.target_spell_cols[3], std::string("ReqSpellCast4"));
    QCOMPARE(q.rep_reward_faction_cols[4], std::string("RewRepFaction5"));
    QVERIFY(db.links.npc_starters.valid() && db.links.object_enders.valid() && db.links.area_triggers.valid());
    QCOMPARE(db.links.item_start_quest_col, std::string("start_quest"));
    QVERIFY(db.loot.creatureDrops() && db.loot.objectDrops());
    QVERIFY(db.loot.creature.quest_required_col.empty()); // quest drops: negative ChanceOrQuestChance
    QVERIFY(db.scripts.valid());
    QVERIFY(db.npc.valid());

    QuestFields fields;
    fields.title = "Into the Den";
    auto const insert = buildQuestInsert(q, 90010, fields, 0);
    QVERIFY(contains(insert, "INSERT INTO `quest_template` (`entry`, "));
    QVERIFY(!contains(insert, "`patch`"));
  }

  void fieldsRoundTrip()
  {
    auto const schema = detectQuestSchema(vmangosColumns("quest_template"));
    QuestFields fields;
    fields.title = "Wolves";
    fields.type = 1;
    fields.suggested_players = 3;
    fields.time_limit = 900;
    fields.skill = 185;
    fields.skill_value = 50;
    fields.min_rep = Reputation{72, 3000};
    fields.rep_objective = Reputation{47, 9000};
    fields.source_item = ItemCount{750, 2};
    fields.reward_spell = 133;
    fields.targets = std::vector<Target>{{Target::Kind::Creature, 299, 8, "", 0},
                                         {Target::Kind::Object, 1721, 1, "Chain unlocked", 3366}};
    fields.collect = std::vector<ItemCount>{{750, 5}};
    fields.rep_rewards = std::vector<Reputation>{{72, 250}, {47, 100}};

    auto const update = buildQuestUpdate(schema, 90010, fields);
    QVERIFY(contains(update, "`ReqCreatureOrGOId2` = -1721"));
    QVERIFY(contains(update, "`ReqSpellCast2` = 3366"));
    QVERIFY(contains(update, "`RewRepFaction3` = 0"));
    auto const back = questFieldsFromRow(schema, assignedValues(update));
    QCOMPARE(*back.title, std::string("Wolves"));
    QCOMPARE(*back.time_limit, std::uint32_t(900));
    QCOMPARE(back.min_rep->value, 3000);
    QCOMPARE(back.rep_objective->faction, std::uint32_t(47));
    QCOMPARE(back.source_item->count, std::uint32_t(2));
    QCOMPARE(back.targets->size(), std::size_t(2));
    QVERIFY((*back.targets)[1].kind == Target::Kind::Object);
    QCOMPARE((*back.targets)[1].id, std::uint32_t(1721));
    QCOMPARE((*back.targets)[1].text, std::string("Chain unlocked"));
    QCOMPARE(back.rep_rewards->size(), std::size_t(2));
    QCOMPARE((*back.rep_rewards)[1].value, 100);
  }

  void copyClearsChainAndScripts()
  {
    auto const schema = detectQuestSchema(vmangosColumns("quest_template"));
    QuestFields fields;
    fields.title = "Again";
    auto const sql = buildQuestInsert(schema, 90011, fields, 15);
    for (auto const* column : {"PrevQuestId", "NextQuestId", "ExclusiveGroup", "NextQuestInChain", "StartScript"})
    {
      QVERIFY2(!contains(sql, std::string("src.`") + column + "`"), column);
    }
    QVERIFY(contains(sql, "ORDER BY src.`patch` DESC LIMIT 1"));
    fields.prev_quest = 7; // unless the editor sets it
    QVERIFY(contains(buildQuestInsert(schema, 90011, fields, 15), ", 7, "));
  }

  void links()
  {
    auto const schema = detectLinkSchema(vmangosColumns);
    QuestLinks links;
    links.starters = {{Giver::Kind::Npc, 197}, {Giver::Kind::Object, 180000}};
    links.enders = {{Giver::Kind::Npc, 197}};
    links.start_item = 90100;
    links.area_trigger = 2946;
    auto const statements = buildLinkStatements(schema, 90010, links, 90050);
    QVERIFY(anyContains(statements, "INSERT INTO `creature_questrelation` (`id`, `quest`) VALUES (197, 90010)"));
    QVERIFY(anyContains(statements, "INSERT INTO `gameobject_questrelation` (`id`, `quest`) VALUES (180000, 90010)"));
    QVERIFY(anyContains(statements, "DELETE FROM `gameobject_involvedrelation` WHERE `quest` = 90010"));
    QVERIFY(anyContains(statements, "INSERT INTO `areatrigger_involvedrelation` (`id`, `quest`) VALUES (2946, 90010)"));
    QVERIFY(anyContains(statements, "UPDATE `item_template` SET `start_quest` = 0 WHERE `entry` = 90050 AND `start_quest` = 90010"));
    QVERIFY(anyContains(statements, "UPDATE `item_template` SET `start_quest` = 90010 WHERE `entry` = 90100"));
    QCOMPARE(npcEntries(links.starters), std::vector<std::uint32_t>{197});
  }

  void drops()
  {
    auto const db = detectQuestDatabase(vmangosColumns);
    auto const creature = buildDropStatements(db.loot, {QuestDrop::Source::Creature, 299, 750, 45.0f, 299});
    QCOMPARE(creature, (std::vector<std::string>{
      "DELETE FROM `creature_loot_template` WHERE `entry` = 299 AND `item` = 750 AND `ChanceOrQuestChance` < 0",
      "INSERT IGNORE INTO `creature_loot_template` (`entry`, `item`, `ChanceOrQuestChance`, `groupid`, `mincountOrRef`, "
      "`maxcount`) VALUES (299, 750, -45, 0, 1, 1)"}));
    auto const chest = buildDropStatements(db.loot, {QuestDrop::Source::Object, 90200, 750, 100.0f, 0});
    QCOMPARE(chest.size(), std::size_t(3));
    QCOMPARE(chest[0], std::string("UPDATE `gameobject_template` SET `data1` = 90200 WHERE `entry` = 90200 AND `type` = 3 AND `data1` = 0"));
    QVERIFY(contains(chest[2], "INSERT IGNORE INTO `gameobject_loot_template`"));

    auto const ac = detectLootTable("creature_loot_template", {"Entry", "Item", "Reference", "Chance", "QuestRequired", "GroupId", "MinCount", "MaxCount"});
    QCOMPARE(questOnlyCondition(ac), std::string("`QuestRequired` = 1"));
  }

  void scriptsRoundTrip()
  {
    auto const schema = detectScriptSchema(vmangosColumns);
    QVERIFY(schema.valid());
    std::vector<ScriptAction> actions(6);
    actions[0].kind = ScriptAction::Kind::Say;
    actions[0].text = "Listen well, $N.";
    actions[1].kind = ScriptAction::Kind::Emote;
    actions[1].wait = 3;
    actions[1].id = 1;
    actions[2].kind = ScriptAction::Kind::Yell;
    actions[2].wait = 2;
    actions[2].text = "To arms!";
    actions[3].kind = ScriptAction::Kind::SummonCreature;
    actions[3].wait = 1;
    actions[3].id = 299;
    actions[3].count = 60;
    actions[3].x = 1.5f;
    actions[4].kind = ScriptAction::Kind::AttackPlayer;
    actions[5].kind = ScriptAction::Kind::CompleteQuest;
    actions[5].wait = 4;
    actions[5].id = 90010; // always the quest the script belongs to

    std::uint32_t next_text = TEXT_ID_START;
    auto const statements = buildScriptStatements(schema, ScriptWhen::Accepted, 90010, 90010, actions, {900005}, next_text);
    QCOMPARE(next_text, TEXT_ID_START + 2);
    QVERIFY(anyContains(statements, "DELETE FROM `quest_start_scripts` WHERE `id` = 90010"));
    QVERIFY(anyContains(statements, "DELETE FROM `broadcast_text` WHERE `entry` IN (900005)"));
    QVERIFY(anyContains(statements, "VALUES (900001, 'To arms!', 'To arms!', 1)"));
    QVERIFY(anyContains(statements, "(`id`, `delay`, `command`, `datalong`, `comments`) VALUES (90010, 10, 7, 90010"));

    // Back from rows, as the database layer reads them.
    std::vector<ScriptRow> rows;
    std::map<std::uint32_t, SpokenText> texts;
    for (auto const& statement : statements)
    {
      if (!contains(statement, "INSERT INTO `quest_start_scripts`"))
      {
        if (contains(statement, "INSERT INTO `broadcast_text`"))
        {
          auto const values = statement.substr(statement.find("VALUES (") + 8);
          auto const id = static_cast<std::uint32_t>(std::stoul(values));
          auto const text_start = values.find('\'') + 1;
          texts[id] = {values.substr(text_start, values.find('\'', text_start) - text_start),
                       static_cast<std::uint32_t>(values[values.size() - 2] - '0')};
        }
        continue;
      }
      auto const columns_part = statement.substr(statement.find('(') + 1, statement.find(')') - statement.find('(') - 1);
      auto const values_part = statement.substr(statement.find("VALUES (") + 8);
      ScriptRow row;
      std::size_t c = 0, v = 0;
      while (c < columns_part.size())
      {
        auto const name_start = columns_part.find('`', c) + 1;
        auto const name_end = columns_part.find('`', name_start);
        auto value_end = values_part[v] == '\'' ? values_part.find('\'', v + 1) + 1 : values_part.find_first_of(",)", v);
        std::string value = values_part.substr(v, value_end - v);
        if (!value.empty() && value.front() == '\'')
        {
          value = value.substr(1, value.size() - 2);
        }
        row[columns_part.substr(name_start, name_end - name_start)] = value;
        c = name_end + 1;
        v = value_end + 2;
      }
      rows.push_back(row);
    }
    auto const parsed = parseScript(rows, texts);
    QVERIFY(parsed.complete);
    QCOMPARE(parsed.actions.size(), actions.size());
    for (std::size_t i = 0; i < actions.size(); ++i)
    {
      QVERIFY2(parsed.actions[i] == actions[i], std::to_string(i).c_str());
    }
    QCOMPARE(parsed.custom_text_ids, (std::vector<std::uint32_t>{900000, 900001}));
    QVERIFY(completesQuest(parsed.actions));
  }

  void unknownScriptStepsAreReported()
  {
    auto const parsed = parseScript({{{"command", std::string("35")}, {"delay", std::string("0")}}}, {});
    QVERIFY(!parsed.complete);
    QVERIFY(parsed.actions.empty());
  }

  void catalog()
  {
    std::uint32_t all = 0;
    for (auto const& c : playerClasses(true))
    {
      all |= c.mask();
    }
    QCOMPARE(all, std::uint32_t(1 | 2 | 4 | 8 | 16 | 64 | 128 | 256 | 1024));
    QCOMPARE(playerClasses(false).size(), std::size_t(10));
    QCOMPARE(reputationRankOf(3000), std::size_t(4));   // Friendly
    QCOMPARE(reputationRankOf(2999), std::size_t(3));   // Neutral
    QCOMPARE(reputationRankOf(-50000), std::size_t(0)); // Hated
    QCOMPARE(suggestedXp({{5, {400, 452, 1000}}}, 5), std::uint32_t(450));
  }
};

int runQuestTests(int argc, char** argv)
{
  QuestTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "test_quest.moc"
