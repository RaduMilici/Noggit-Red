// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "TestSupport.hpp"
#include "TortoiseSchema.hpp"
#include "VmangosSchema.hpp"

#include <noggit/content/SqlText.hpp>

#include <QtTest/QtTest>

using namespace Noggit::Content;

class SqlTextTest : public QObject
{
  Q_OBJECT

private slots:
  void escapesAndQuotes()
  {
    QCOMPARE(escapeString(std::string("a'b\\c\n\0z", 8)), std::string("a\\'b\\\\c\\n\\0z"));
    QCOMPARE(quoteIdentifier("we`ird"), std::string("`we``ird`"));
    QCOMPARE(literal("it's"), std::string("'it\\'s'"));
    QCOMPARE(literal(std::int32_t(-5)), std::string("-5"));
    QCOMPARE(literal(1.25f), std::string("1.25"));
  }

  void statements()
  {
    QCOMPARE(insertStatement("t", {{"a", "1"}, {"b", "'x'"}}), std::string("INSERT INTO `t` (`a`, `b`) VALUES (1, 'x')"));
    QCOMPARE(insertStatement("t", {{"a", "1"}}, true), std::string("INSERT IGNORE INTO `t` (`a`) VALUES (1)"));
    QCOMPARE(updateStatement("t", {{"a", "1"}}, equals("id", "7")), std::string("UPDATE `t` SET `a` = 1 WHERE `id` = 7"));
    QVERIFY(updateStatement("t", {}, "x").empty());
    QCOMPARE(deleteStatement("t", allOf({equals("a", "1"), equals("b", "2")})),
             std::string("DELETE FROM `t` WHERE `a` = 1 AND `b` = 2"));
  }

  void copyRow()
  {
    QCOMPARE(copyRowStatement("t", {"id", "patch", "name", "hp"}, {{"id", "9"}, {"patch", "0"}},
                              sourceColumn("id") + " = 5", "patch"),
             std::string("INSERT INTO `t` (`id`, `patch`, `name`, `hp`) SELECT 9, 0, src.`name`, src.`hp` FROM `t` src "
                         "WHERE src.`id` = 5 ORDER BY src.`patch` DESC LIMIT 1"));
  }

  void tableOrderAndLists()
  {
    auto const ordered = inTableOrder({"a", "b", "c"}, {{"c", "3"}, {"a", "1"}, {"zz", "9"}});
    QCOMPARE(ordered.size(), std::size_t(2));
    QCOMPARE(ordered[0].first, std::string("a"));
    QCOMPARE(idList({5, 1, 5, 3}), std::string("1, 3, 5"));
    QCOMPARE(firstColumn({"x", "Name"}, {"name", "Name"}), std::string("Name"));
    QVERIFY(firstColumn({"x"}, {"name"}).empty());
  }

  void snapshotValues()
  {
    QCOMPARE(buildInsertValues("t", {"a", "b"}, {{std::string("1"), std::nullopt}, {std::string("x'"), std::string("")}}),
             std::string("INSERT INTO `t` (`a`, `b`) VALUES\n  ('1', NULL),\n  ('x\\'', '');"));
    QVERIFY(buildInsertValues("t", {"a"}, {}).empty());
  }
};

int runSqlTextTests(int argc, char** argv)
{
  SqlTextTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "test_sql_text.moc"
