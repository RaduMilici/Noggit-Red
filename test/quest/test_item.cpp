// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "TestSupport.hpp"
#include "VmangosSchema.hpp"

#include <noggit/item/ItemTemplateSql.hpp>

#include <QtTest/QtTest>

using namespace Noggit::Item;

class ItemTest : public QObject
{
  Q_OBJECT

private slots:
  void detectsVmangos()
  {
    auto const schema = detectItemSchema(vmangosColumns("item_template"));
    QVERIFY(schema.valid());
    QCOMPARE(schema.display_col, std::string("display_id"));
    QCOMPARE(schema.max_count_col, std::string("max_count"));
    QCOMPARE(schema.start_quest_col, std::string("start_quest"));
  }

  void newQuestItem()
  {
    auto const schema = detectItemSchema(vmangosColumns("item_template"));
    auto const& kind = itemKinds().front();
    ItemFields fields;
    fields.name = "Wolf Pelt";
    fields.description = "Thick and matted.";
    fields.display_id = 1116;
    fields.quality = 1;
    fields.item_class = kind.item_class;
    fields.subclass = kind.subclass;
    fields.bonding = kind.bonding;
    fields.stackable = 20;
    QCOMPARE(buildItemInsert(schema, 90100, fields),
             std::string("INSERT INTO `item_template` (`entry`, `patch`, `class`, `subclass`, `name`, `description`, "
                         "`display_id`, `quality`, `stackable`, `bonding`, `start_quest`) VALUES (90100, 0, 12, 0, "
                         "'Wolf Pelt', 'Thick and matted.', 1116, 1, 20, 4, 0)"));
    auto const copy = buildItemInsert(schema, 90101, fields, 750);
    QVERIFY(contains(copy, "src.`spellid_1`"));
    QVERIFY(contains(copy, "ORDER BY src.`patch` DESC LIMIT 1"));
  }

  void sellPriceSetsBuyPrice()
  {
    ItemFields fields;
    fields.sell_price = 25;
    QVERIFY(contains(buildItemUpdate(detectItemSchema(vmangosColumns("item_template")), 90100, fields),
                     "`buy_price` = 100, `sell_price` = 25"));
  }

  void deletePlan()
  {
    auto const schema = detectItemSchema(vmangosColumns("item_template"));
    QVERIFY(!planItemDelete(schema, 90100, {{90010}, 0, 0}).problems.empty()); // still used by a quest
    auto const plan = planItemDelete(schema, 90100, {{}, 2, 1});
    QVERIFY(plan.problems.empty());
    QCOMPARE(plan.statements.back(), std::string("DELETE FROM `item_template` WHERE `entry` = 90100"));
    QCOMPARE(plan.summary.size(), std::size_t(2));
  }

  void qualitiesInOrder()
  {
    for (std::size_t i = 0; i < qualities().size(); ++i)
    {
      QCOMPARE(qualities()[i].id, std::uint32_t(i));
    }
  }
};

int runItemTests(int argc, char** argv)
{
  ItemTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "test_item.moc"
