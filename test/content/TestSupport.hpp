// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <string>
#include <vector>

inline bool contains(std::string const& haystack, std::string const& needle)
{
  return haystack.find(needle) != std::string::npos;
}

inline bool endsWith(std::string const& text, std::string const& suffix)
{
  return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool anyContains(std::vector<std::string> const& statements, std::string const& needle)
{
  for (auto const& statement : statements)
  {
    if (contains(statement, needle))
    {
      return true;
    }
  }
  return false;
}

// Each test file defines one of these; main.cpp runs them all.
int runSqlTextTests(int argc, char** argv);
int runNpcTests(int argc, char** argv);
int runQuestTests(int argc, char** argv);
int runQuestChainTests(int argc, char** argv);
int runQuestSavePlanTests(int argc, char** argv);
int runItemTests(int argc, char** argv);
int runLiveTests(int argc, char** argv);
