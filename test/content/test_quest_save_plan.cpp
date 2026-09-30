// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "TestSupport.hpp"
#include "TortoiseSchema.hpp"
#include "VmangosSchema.hpp"

#include <noggit/quest/QuestSavePlan.hpp>

#include <QtTest/QtTest>

using namespace Noggit::Quest;

namespace
{
  ChainQuest chainQuest(std::uint32_t entry, bool editable = true)
  {
    ChainQuest q;
    q.entry = entry;
    q.editable = editable;
    return q;
  }

  SaveInputs inputs()
  {
    SaveInputs in;
    in.chain = {chainQuest(7, false), chainQuest(90000), chainQuest(90001)};
    in.npc_flags = {{197, 3}, {90005, 1}};
    in.names.quest = [](std::uint32_t id) { return "Quest " + std::to_string(id); };
    return in;
  }

  QuestContent fullQuest()
  {
    QuestContent content;
    content.fields.title = "Into the Den";
    content.fields.special_flags = SPECIAL_REPEATABLE;
    content.links.starters = {{Giver::Kind::Npc, 197}, {Giver::Kind::Object, 180000}};
    content.links.enders = {{Giver::Kind::Npc, 90005}};
    content.links.area_trigger = 2946;
    content.drops = {{QuestDrop::Source::Creature, 299, 750, 40.0f, 299}};
    content.prerequisites = {{90000}, Prerequisites::Mode::Any};
    content.exclusive_with = {90001};
    ScriptAction say;
    say.text = "Be careful out there.";
    content.on_accept = {say};
    return content;
  }
}

class QuestSavePlanTest : public QObject
{
  Q_OBJECT

private slots:
  void createWithEverything()
  {
    auto const db = detectQuestDatabase(vmangosColumns);
    auto const plan = planQuestSave(db, SaveMode::Create, 90010, 0, fullQuest(), {}, {}, inputs());
    QVERIFY(plan.problems.empty());
    QVERIFY(plan.require_first_row);
    auto const& insert = plan.statements.front();
    QVERIFY(contains(insert, "INSERT INTO `quest_template`"));
    // Repeatable (1) + exploration (2), started by script 90010, needs 90000, either/or with 90001.
    QVERIFY(contains(insert, "`SpecialFlags`"));
    auto const values = insert.substr(insert.find("VALUES"));
    QVERIFY(contains(values, "'Into the Den'"));
    QVERIFY(anyContains(plan.statements, "INSERT INTO `gameobject_questrelation` (`id`, `quest`) VALUES (180000, 90010)"));
    QVERIFY(anyContains(plan.statements, "INSERT INTO `areatrigger_involvedrelation`"));
    QVERIFY(anyContains(plan.statements, "INSERT IGNORE INTO `creature_loot_template`"));
    QVERIFY(anyContains(plan.statements, "INSERT INTO `quest_start_scripts`"));
    // 90000 now offers the new quest and is in the either/or group with 90001: both change.
    QVERIFY(anyContains(plan.statements, "`NextQuestInChain` = 90010 WHERE `entry` = 90000"));
    QVERIFY(anyContains(plan.statements, "`ExclusiveGroup` = 90001, `NextQuestInChain` = 0 WHERE `entry` = 90001"));
    QCOMPARE(plan.quests_changed, (std::set<std::uint32_t>{90000, 90001, 90010}));
    // Only 90005 lacks the quest-giver bit (197 has 3).
    QCOMPARE(plan.new_quest_givers, std::vector<std::uint32_t>{90005});
    QVERIFY(contains(plan.statements.back(), "`npc_flags` | 2 WHERE `entry` IN (90005)"));
    // Cleanup removes the new rows and restores the other quests.
    QVERIFY(anyContains(plan.cleanup, "DELETE FROM `quest_template` WHERE `entry` = 90010"));
    QVERIFY(anyContains(plan.cleanup, "DELETE FROM `broadcast_text` WHERE `entry` IN (900000)"));
    QVERIFY(anyContains(plan.cleanup, "SET `PrevQuestId` = 0, `NextQuestId` = 0, `ExclusiveGroup` = 0, `NextQuestInChain` = 0 WHERE `entry` = 90000"));
  }

  void createOnTortoise()
  {
    auto const db = detectQuestDatabase(tortoiseColumns);
    auto const plan = planQuestSave(db, SaveMode::Create, 90010, 0, fullQuest(), {}, {}, inputs());
    QVERIFY(plan.problems.empty());
    QVERIFY(contains(plan.statements.front(), "INSERT INTO `quest_template`"));
    QVERIFY(anyContains(plan.statements, "INSERT INTO `gameobject_questrelation` (`id`, `quest`) VALUES (180000, 90010)"));
    QVERIFY(anyContains(plan.statements, "INSERT INTO `areatrigger_involvedrelation`"));
    QVERIFY(anyContains(plan.statements, "INSERT IGNORE INTO `creature_loot_template`"));
    QVERIFY(anyContains(plan.statements, "INSERT INTO `quest_start_scripts`"));
    QVERIFY(contains(plan.statements.back(), "`npc_flags` | 2 WHERE `entry` IN (90005)"));
    for (auto const& statement : plan.statements)
    {
      QVERIFY2(!contains(statement, "patch"), statement.c_str()); // no patch versioning on tortoise-wow
    }
  }

  void specialFlagsFollowContent()
  {
    auto const db = detectQuestDatabase(vmangosColumns);
    auto content = fullQuest();
    auto const flags = [&](QuestContent const& c)
    {
      auto const plan = planQuestSave(db, SaveMode::Create, 90010, 0, c, {}, {}, inputs());
      auto const& insert = plan.statements.front();
      auto const columns = insert.substr(0, insert.find(" VALUES"));
      std::size_t const index = std::count(columns.begin(), columns.begin() + columns.find("`SpecialFlags`"), ',');
      auto values = insert.substr(insert.find("VALUES (") + 8);
      for (std::size_t i = 0; i < index; ++i)
      {
        values = values[0] == '\'' ? values.substr(values.find("', ") + 3) : values.substr(values.find(", ") + 2);
      }
      return std::stoi(values);
    };
    QCOMPARE(flags(content), 3);
    content.links.area_trigger = 0;
    QCOMPARE(flags(content), 1);
    ScriptAction complete;
    complete.kind = ScriptAction::Kind::CompleteQuest;
    content.on_complete = {complete};
    QCOMPARE(flags(content), 3);
  }

  void editRemovesDroppedDropsButNotShared()
  {
    auto const db = detectQuestDatabase(vmangosColumns);
    QuestContent before = fullQuest();
    before.drops.push_back({QuestDrop::Source::Object, 32, 1309, 100.0f, 1683});
    QuestContent after = before;
    after.drops.clear();
    QuestStored stored;
    stored.shared_items = {1309};
    auto const plan = planQuestSave(db, SaveMode::Edit, 90010, 0, after, before, stored, inputs());
    QVERIFY(anyContains(plan.statements, "DELETE FROM `creature_loot_template` WHERE `entry` = 299 AND `item` = 750"));
    QVERIFY(!anyContains(plan.statements, "`item` = 1309"));
    QVERIFY(plan.cleanup.empty());
    // Scripts unchanged: not rewritten.
    QVERIFY(!anyContains(plan.statements, "quest_start_scripts"));
  }

  void refusals()
  {
    auto const db = detectQuestDatabase(vmangosColumns);
    auto in = inputs();
    in.trigger_quests[2946] = 6421;
    auto const taken = planQuestSave(db, SaveMode::Create, 90010, 0, fullQuest(), {}, {}, in);
    QCOMPARE(taken.problems.size(), std::size_t(1));
    QVERIFY(taken.statements.empty());

    auto content = fullQuest();
    content.prerequisites = {{7, 90000}, Prerequisites::Mode::All}; // 7 is a game quest
    auto const chain = planQuestSave(db, SaveMode::Create, 90010, 0, content, {}, {}, inputs());
    QCOMPARE(chain.problems.size(), std::size_t(1));
    QVERIFY(contains(chain.problems.front(), "Quest 7"));
  }

  void deleteUnlinksOthers()
  {
    auto const db = detectQuestDatabase(vmangosColumns);
    auto in = inputs();
    in.chain[1].next_in_chain = 90010;   // 90000 offers it
    auto after = chainQuest(90001);
    after.prev_quest = 90010;            // 90001 needs it
    in.chain[2] = after;
    auto target = chainQuest(90010);
    target.prev_quest = 90000;
    in.chain.push_back(target);

    QuestStored stored;
    stored.accept_text_ids = {900004};
    auto const plan = planQuestDelete(db, 90010, fullQuest(), stored, in);
    QVERIFY(plan.problems.empty());
    QVERIFY(anyContains(plan.statements, "DELETE FROM `quest_template` WHERE `entry` = 90010"));
    QVERIFY(anyContains(plan.statements, "DELETE FROM `quest_end_scripts` WHERE `id` = 90010"));
    QVERIFY(anyContains(plan.statements, "DELETE FROM `broadcast_text` WHERE `entry` IN (900004)"));
    QVERIFY(anyContains(plan.statements, "UPDATE `item_template` SET `start_quest` = 0 WHERE `start_quest` = 90010"));
    QVERIFY(anyContains(plan.statements, "`NextQuestInChain` = 0 WHERE `entry` = 90000"));
    QVERIFY(anyContains(plan.statements, "`PrevQuestId` = 0, `NextQuestId` = 0, `ExclusiveGroup` = 0, `NextQuestInChain` = 0 WHERE `entry` = 90001"));
  }
};

int runQuestSavePlanTests(int argc, char** argv)
{
  QuestSavePlanTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "test_quest_save_plan.moc"
