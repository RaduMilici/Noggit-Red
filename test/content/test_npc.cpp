// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "TestSupport.hpp"
#include "TortoiseSchema.hpp"
#include "VmangosSchema.hpp"

#include <noggit/npc/NpcDialogueSql.hpp>
#include <noggit/npc/NpcSavePlan.hpp>
#include <noggit/npc/NpcTemplateSql.hpp>

#include <QtTest/QtTest>

using namespace Noggit::Npc;

class NpcTest : public QObject
{
  Q_OBJECT

private slots:
  void detectsVmangosSchema()
  {
    auto const schema = detectNpcSchema(vmangosColumns);
    QVERIFY(schema.valid());
    QCOMPARE(schema.patch_col, std::string("patch"));
    QCOMPARE(schema.level_min_col, std::string("level_min"));
    QCOMPARE(schema.npc_flags_col, std::string("npc_flags"));
    QCOMPARE(schema.display_col, std::string("display_id1"));
    QCOMPARE(schema.other_display_cols.size(), std::size_t(3));
    QCOMPARE(schema.scale_col, std::string("display_scale1"));
    QCOMPARE(schema.gossip_menu_col, std::string("gossip_menu_id"));
    QCOMPARE(schema.faction_cols, std::vector<std::string>{"faction"});
    QCOMPARE(schema.related.size(), std::size_t(2)); // npc_vendor, npc_trainer
  }

  void detectsOtherCores()
  {
    auto const cmangos = detectTemplateSchema({"Entry", "Name", "SubName", "MinLevel", "MaxLevel", "DisplayId1",
                                               "DisplayId2", "FactionAlliance", "FactionHorde", "NpcFlags",
                                               "Scale", "Rank", "ScriptName", "GossipMenuId"});
    QCOMPARE(cmangos.faction_cols, (std::vector<std::string>{"FactionAlliance", "FactionHorde"}));
    QCOMPARE(cmangos.scale_col, std::string("Scale"));
    QCOMPARE(cmangos.gossip_menu_col, std::string("GossipMenuId"));
    auto const azeroth = detectTemplateSchema({"entry", "name", "minlevel", "npcflag", "scale", "ScriptName"});
    QVERIFY(azeroth.display_col.empty()); // models in creature_template_model
    QVERIFY(!detectTemplateSchema({"id", "title"}).valid());
  }

  void cloneCopiesNewestPatch()
  {
    auto const schema = detectNpcSchema(vmangosColumns);
    CloneRequest request;
    request.source_entry = 5494;
    request.new_entry = 90001;
    request.fields.name = "Mira's Wares";
    request.copy_related = {"npc_vendor"};
    auto const statements = buildCloneStatements(schema, request);
    QCOMPARE(statements.size(), std::size_t(2)); // template + vendor items (trainer not requested)
    QVERIFY(contains(statements[0], "SELECT 90001, 0, 'Mira\\'s Wares', src.`subname`"));
    QVERIFY(contains(statements[0], "WHERE src.`entry` = 5494 ORDER BY src.`patch` DESC LIMIT 1"));
    QVERIFY(contains(statements[1], "INSERT INTO `npc_vendor`"));
    QVERIFY(contains(statements[1], "SELECT 90001, src.`slot`"));
  }

  void modelChangeClearsVariants()
  {
    NpcFields fields;
    fields.display_id = 1234;
    fields.scale = 1.5f;
    QCOMPARE(buildUpdateStatement(detectNpcSchema(vmangosColumns), 90001, fields),
             std::string("UPDATE `creature_template` SET `display_id1` = 1234, `display_id2` = 0, `display_id3` = 0, "
                         "`display_id4` = 0, `display_scale1` = 1.5, `display_probability1` = 100, "
                         "`display_probability2` = 0, `display_probability3` = 0, `display_probability4` = 0, "
                         "`display_total_probability` = 100 WHERE `entry` = 90001"));
    QVERIFY(buildUpdateStatement(detectNpcSchema(vmangosColumns), 90001, {}).empty());
  }

  void rolesHaveDistinctFlags()
  {
    for (bool vanilla : {true, false})
    {
      std::uint32_t seen = 0;
      for (auto const& role : npcRoles(vanilla))
      {
        QVERIFY2((seen & role.flag) == 0, role.label);
        seen |= role.flag;
      }
    }
  }

  // --- dialogue ---

  void dialogueNewMenuForSharedOne()
  {
    auto const schema = detectDialogueSchema(vmangosColumns);
    QVERIFY(schema.greetingSupported() && schema.questGreetingSupported());
    auto const npc = detectNpcSchema(vmangosColumns);
    DialogueStored stored; // a clone still using the game NPC's menu
    stored.menu = 4108;
    stored.npc_text = 5000;
    stored.menu_shared = true;
    stored.dialogue.greeting = "Old text";
    auto const plan = planDialogue(schema, npc, 90001, {"Welcome, $N!", ""}, stored, {60403, 900000, 900000});
    QVERIFY(plan.problems.empty());
    QVERIFY(!anyContains(plan.statements, "DELETE")); // the shared menu is left alone
    QVERIFY(anyContains(plan.statements, "INSERT INTO `broadcast_text` (`entry`, `male_text`, `female_text`) "
                                         "VALUES (900000, 'Welcome, $N!', 'Welcome, $N!')"));
    QVERIFY(anyContains(plan.statements, "INSERT INTO `gossip_menu` (`entry`, `text_id`) VALUES (60403, 900000)"));
    QVERIFY(anyContains(plan.statements, "SET `gossip_menu_id` = 60403 WHERE `entry` = 90001"));
  }

  void dialogueRewritesOwnMenuInPlace()
  {
    auto const schema = detectDialogueSchema(vmangosColumns);
    DialogueStored stored;
    stored.menu = 60403;
    stored.npc_text = 900000;
    stored.broadcast = 900001;
    stored.dialogue.greeting = "Old";
    auto const plan = planDialogue(schema, detectNpcSchema(vmangosColumns), 90001, {"New", ""}, stored, {60500, 900010, 900010});
    QVERIFY(anyContains(plan.statements, "DELETE FROM `gossip_menu` WHERE `entry` = 60403"));
    QVERIFY(anyContains(plan.statements, "VALUES (900001, 'New', 'New')"));
    QVERIFY(anyContains(plan.statements, "VALUES (60403, 900000)"));
    QVERIFY(!anyContains(plan.statements, "gossip_menu_id")); // same menu
  }

  void dialogueRemovedAndQuestGreeting()
  {
    auto const schema = detectDialogueSchema(vmangosColumns);
    DialogueStored stored;
    stored.menu = 60403;
    stored.npc_text = 900000;
    stored.dialogue = {"Hi", "Old greeting"};
    auto const plan = planDialogue(schema, detectNpcSchema(vmangosColumns), 90001, {"", "Adventurer!"}, stored, {1, 2, 3});
    QVERIFY(anyContains(plan.statements, "SET `gossip_menu_id` = 0"));
    QVERIFY(anyContains(plan.statements, "DELETE FROM `quest_greeting` WHERE `entry` = 90001 AND `type` = 0"));
    QVERIFY(anyContains(plan.statements, "VALUES (90001, 0, 'Adventurer!')"));
  }

  void menuIdLimit()
  {
    auto const plan = planDialogue(detectDialogueSchema(vmangosColumns), detectNpcSchema(vmangosColumns), 90001,
                                   {"Hi", ""}, {}, {MAX_MENU_ID + 1, 900000, 900000});
    QCOMPARE(plan.problems.size(), std::size_t(1));
  }

  // --- save / delete plans ---

  void createPlanCleansUpItsMenu()
  {
    auto const db = detectNpcDatabase(vmangosColumns);
    NpcContent content;
    content.fields.name = "Mira";
    content.dialogue.greeting = "Hello";
    auto const plan = planNpcSave(db, NpcSaveMode::Create, 90002, 5494, content, {}, {60410, 900020, 900020});
    QVERIFY(plan.require_first_row);
    QVERIFY(contains(plan.statements.front(), "INSERT INTO `creature_template`"));
    QVERIFY(anyContains(plan.cleanup, "DELETE FROM `creature_template` WHERE `entry` = 90002"));
    QVERIFY(anyContains(plan.cleanup, "DELETE FROM `gossip_menu` WHERE `entry` = 60410"));
  }

  void deletePlan()
  {
    auto const db = detectNpcDatabase(vmangosColumns);
    QCOMPARE(db.spawns.entry_col, std::string("id"));
    QCOMPARE(db.spawns.per_spawn.size(), std::size_t(5));
    QVERIFY(!planNpcDelete(db, 1423, {}, 0, 0).problems.empty()); // game NPC
    auto const plan = planNpcDelete(db, 90002, {}, 3, 1);
    QVERIFY(plan.problems.empty());
    QCOMPARE(plan.statements.front(),
             std::string("DELETE FROM `creature_addon` WHERE `guid` IN (SELECT `guid` FROM `creature` WHERE `id` = 90002)"));
    QVERIFY(anyContains(plan.statements, "DELETE FROM `creature` WHERE `id` = 90002"));
    QVERIFY(anyContains(plan.statements, "DELETE FROM `creature_questrelation` WHERE `id` = 90002"));
    QCOMPARE(plan.statements.back(), std::string("DELETE FROM `npc_trainer` WHERE `entry` = 90002"));
    QCOMPARE(plan.summary.size(), std::size_t(2));
  }

  // --- tortoise-wow (Turtle WoW) ---

  void detectsTortoiseSchema()
  {
    auto const schema = detectNpcSchema(tortoiseColumns);
    QVERIFY(schema.valid());
    QVERIFY(schema.patch_col.empty());
    QCOMPARE(schema.display_col, std::string("display_id1"));
    QCOMPARE(schema.other_display_cols.size(), std::size_t(3));
    QVERIFY(schema.display_probability_col.empty());
    QCOMPARE(schema.scale_col, std::string("scale"));
    QCOMPARE(schema.gossip_menu_col, std::string("gossip_menu_id"));
    QCOMPARE(schema.script_col, std::string("script_name"));
    QCOMPARE(schema.related.size(), std::size_t(2)); // npc_vendor, npc_trainer
  }

  void tortoiseCloneAndModelChange()
  {
    auto const schema = detectNpcSchema(tortoiseColumns);
    CloneRequest request;
    request.source_entry = 5494;
    request.new_entry = 90001;
    request.fields.name = "Mira";
    auto const statements = buildCloneStatements(schema, request);
    QCOMPARE(statements.size(), std::size_t(1));
    QVERIFY(contains(statements[0], "SELECT 90001, src.`display_id1`"));
    QVERIFY(endsWith(statements[0], "WHERE src.`entry` = 5494")); // one row per entry: no patch versions

    NpcFields fields;
    fields.display_id = 1234;
    fields.scale = 1.5f;
    QCOMPARE(buildUpdateStatement(schema, 90001, fields),
             std::string("UPDATE `creature_template` SET `display_id1` = 1234, `display_id2` = 0, `display_id3` = 0, "
                         "`display_id4` = 0, `scale` = 1.5 WHERE `entry` = 90001"));
  }

  void tortoiseDeletePlan()
  {
    auto const db = detectNpcDatabase(tortoiseColumns);
    QCOMPARE(db.spawns.entry_col, std::string("id"));
    QCOMPARE(db.spawns.per_spawn.size(), std::size_t(9));
    auto const plan = planNpcDelete(db, 90002, {}, 1, 0);
    QVERIFY(anyContains(plan.statements,
                        "DELETE FROM `npc_gossip` WHERE `npc_guid` IN (SELECT `guid` FROM `creature` WHERE `id` = 90002)"));
    QVERIFY(anyContains(plan.statements, "DELETE FROM `creature` WHERE `id` = 90002"));
  }
};

int runNpcTests(int argc, char** argv)
{
  NpcTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "test_npc.moc"
