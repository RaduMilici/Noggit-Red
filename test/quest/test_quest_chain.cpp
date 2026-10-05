// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "TestSupport.hpp"

#include <noggit/quest/QuestChain.hpp>

#include <QtTest/QtTest>

using namespace Noggit::Quest;
using Mode = Prerequisites::Mode;

namespace
{
  ChainQuest quest(std::uint32_t entry, std::int32_t prev = 0, std::uint32_t offers = 0, bool editable = true)
  {
    ChainQuest q;
    q.entry = entry;
    q.title = "Q" + std::to_string(entry);
    q.prev_quest = prev;
    q.next_in_chain = offers;
    q.editable = editable;
    return q;
  }

  // Northshire as in vmangos (7 -> 15 -> 21, 15 offers 21), four custom quests, an unrelated game quest.
  std::vector<ChainQuest> world()
  {
    return {quest(7, 0, 0, false), quest(15, 7, 21, false), quest(21, 15, 0, false),
            quest(90000), quest(90001), quest(90002), quest(90003), quest(33, 0, 0, false)};
  }
}

class QuestChainTest : public QObject
{
  Q_OBJECT

private slots:
  void readsGameChains()
  {
    ChainModel model(world());
    QCOMPARE(model.chainOf(21), (std::set<std::uint32_t>{7, 15, 21}));
    QCOMPARE(model.prerequisitesOf(21).quests, std::vector<std::uint32_t>{15});
    auto const links = model.links({7, 15, 21});
    QCOMPARE(links.size(), std::size_t(2));
    QVERIFY(links[1].from == 15 && links[1].needs_done && links[1].offers);
    QCOMPARE(model.layers({7, 15, 21}).at(21), 2);
  }

  void readsAllOfGroups()
  {
    // Stealing the Flames: 9330..9332 each NextQuestId 9365, ExclusiveGroup -9330.
    std::vector<ChainQuest> quests;
    for (std::uint32_t id : {9330u, 9331u, 9332u})
    {
      auto q = quest(id, 0, 0, false);
      q.next_quest = 9365;
      q.exclusive_group = -9330;
      quests.push_back(q);
    }
    quests.push_back(quest(9365, 0, 0, false));
    ChainModel model(quests);
    auto const prerequisites = model.prerequisitesOf(9365);
    QCOMPARE(prerequisites.quests.size(), std::size_t(3));
    QVERIFY(prerequisites.mode == Mode::All);
  }

  void singlePrerequisiteMayBeAGameQuest()
  {
    ChainModel model(world());
    QVERIFY(model.setPrerequisites(90000, {{21}, Mode::Any}));
    QCOMPARE(model.find(90000)->prev_quest, 21);
    QVERIFY(!model.setPrerequisites(21, {{90000}, Mode::Any})); // game quests never change
    QCOMPARE(model.changes().size(), std::size_t(1));
  }

  void allOfEncoding()
  {
    ChainModel model(world());
    QVERIFY(model.setPrerequisites(90003, {{90001, 90000}, Mode::All}));
    for (std::uint32_t id : {90000u, 90001u})
    {
      QCOMPARE(model.find(id)->next_quest, 90003);
      QCOMPARE(model.find(id)->exclusive_group, -90000);
    }
    QCOMPARE(model.find(90003)->prev_quest, 0);
    auto const read = model.prerequisitesOf(90003);
    QCOMPARE(read.quests, (std::vector<std::uint32_t>{90000, 90001}));
    QVERIFY(read.mode == Mode::All);

    // Switching to "any one" keeps the links and drops the group.
    QVERIFY(model.setPrerequisites(90003, {{90000, 90001}, Mode::Any}));
    QCOMPARE(model.find(90000)->exclusive_group, 0);
    QVERIFY(model.prerequisitesOf(90003).mode == Mode::Any);

    // Back to one prerequisite: stored on the quest, the other unlinked.
    QVERIFY(model.removePrerequisite(90003, 90001));
    QCOMPARE(model.find(90003)->prev_quest, 90000);
    QCOMPARE(model.find(90000)->next_quest, 0);
    QCOMPARE(model.find(90001)->next_quest, 0);
  }

  void severalNeedCustomQuests()
  {
    ChainModel model(world());
    auto const result = model.setPrerequisites(90003, {{90000, 21}, Mode::All});
    QVERIFY(result.problem == ChainProblem::NotEditable);
    QCOMPARE(result.quest, std::uint32_t(21));
    QVERIFY(!model.hasChanges()); // refused changes leave nothing behind
  }

  void oneUnlockPerQuest()
  {
    ChainModel model(world());
    QVERIFY(model.setPrerequisites(90002, {{90000, 90001}, Mode::All}));
    auto const result = model.setPrerequisites(90003, {{90000, 90001}, Mode::Any});
    QVERIFY(result.problem == ChainProblem::UnlocksAnotherQuest);
  }

  void loopsRefused()
  {
    ChainModel model(world());
    QVERIFY(model.setPrerequisites(90001, {{90000}, Mode::Any}));
    QVERIFY(model.setPrerequisites(90002, {{90001}, Mode::Any}));
    QVERIFY(model.setPrerequisites(90000, {{90002}, Mode::Any}).problem == ChainProblem::WouldLoop);
    QVERIFY(model.setPrerequisites(90000, {{90000}, Mode::Any}).problem == ChainProblem::SameQuest);
  }

  void droppedPrerequisiteStopsOffering()
  {
    ChainModel model(world());
    QVERIFY(model.setPrerequisites(90002, {{90000}, Mode::Any}));
    QVERIFY(model.setOffers(90000, 90002));
    QVERIFY(model.setPrerequisites(90002, {{90001}, Mode::Any}));
    QCOMPARE(model.find(90000)->next_in_chain, std::uint32_t(0));
  }

  void eitherOr()
  {
    ChainModel model(world());
    QVERIFY(model.makeExclusive({90001, 90000}));
    QCOMPARE(model.find(90000)->exclusive_group, 90000);
    QCOMPARE(model.exclusiveWith(90001), std::vector<std::uint32_t>{90000});
    QVERIFY(model.chainOf(90000).count(90001));
    // A quest in an all-of requirement cannot be either/or as well.
    QVERIFY(model.setPrerequisites(90003, {{90002, 90001}, Mode::All}).problem == ChainProblem::InAnotherGroup);
    QVERIFY(model.leaveExclusiveGroup(90001));
    QVERIFY(model.exclusiveWith(90000).empty());
    QVERIFY(model.makeExclusive({7, 90000}).problem == ChainProblem::NotEditable);
  }

  void eitherOrGroupNamesDoNotCapture()
  {
    ChainModel model(world());
    QVERIFY(model.makeExclusive({90000, 90001}));        // group 90000
    QVERIFY(model.makeExclusive({90000, 90002}));        // must not pull 90001 back in
    QCOMPARE(model.exclusiveWith(90002), std::vector<std::uint32_t>{90000});
    QVERIFY(model.find(90001)->exclusive_group != model.find(90000)->exclusive_group);
  }

  void changesTrackFields()
  {
    ChainModel model(world());
    QVERIFY(!model.hasChanges());
    QVERIFY(model.setOffers(90000, 90001));
    QVERIFY(model.setOffers(90000, 0));
    QVERIFY(!model.hasChanges());
    QVERIFY(!model.setOffers(15, 0)); // game quest
  }
};

int runQuestChainTests(int argc, char** argv)
{
  QuestChainTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "test_quest_chain.moc"
