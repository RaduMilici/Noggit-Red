// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// Unit tests for the content editors' SQL (src/noggit/content, npc, quest, item). The pure tests always
// run; the live ones need NOGGIT_NPC_TEST_DB (see LiveDb.hpp).

#include "TestSupport.hpp"

#include <QtCore/QCoreApplication>

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  int failures = 0;
  for (auto const run : {runSqlTextTests, runNpcTests, runQuestTests, runQuestChainTests, runQuestSavePlanTests,
                         runItemTests, runLiveTests})
  {
    failures += run(argc, argv);
  }
  return failures ? 1 : 0;
}
