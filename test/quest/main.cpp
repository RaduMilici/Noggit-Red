// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// Unit tests for the quest and item editors' SQL (src/noggit/content, quest, item). Pure: no database.

#include "TestSupport.hpp"

#include <QtCore/QCoreApplication>

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  int failures = 0;
  for (auto const run : {runSqlTextTests, runQuestTests, runQuestChainTests, runQuestSavePlanTests, runItemTests})
  {
    failures += run(argc, argv);
  }
  return failures ? 1 : 0;
}
