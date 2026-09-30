// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/quest/QuestChain.hpp>

#include <algorithm>
#include <deque>

namespace Noggit::Quest
{
  namespace
  {
    ChainResult problem(ChainProblem what, std::uint32_t quest)
    {
      return ChainResult{what, quest};
    }
  }

  ChainModel::ChainModel(std::vector<ChainQuest> quests)
  {
    for (auto& quest : quests)
    {
      auto const entry = quest.entry;
      _quests[entry] = std::move(quest);
    }
    _original = _quests;
  }

  ChainQuest const* ChainModel::find(std::uint32_t entry) const
  {
    auto const found = _quests.find(entry);
    return found == _quests.end() ? nullptr : &found->second;
  }

  ChainQuest* ChainModel::findMutable(std::uint32_t entry)
  {
    auto const found = _quests.find(entry);
    return found == _quests.end() ? nullptr : &found->second;
  }

  std::vector<std::uint32_t> ChainModel::unlockedBy(std::uint32_t quest) const
  {
    std::vector<std::uint32_t> out;
    for (auto const& [id, other] : _quests)
    {
      if (other.next_quest > 0 && static_cast<std::uint32_t>(other.next_quest) == quest)
      {
        out.push_back(id);
      }
    }
    return out;
  }

  std::vector<std::uint32_t> ChainModel::groupMembers(std::int32_t group) const
  {
    std::vector<std::uint32_t> out;
    if (!group)
    {
      return out;
    }
    for (auto const& [id, other] : _quests)
    {
      if (other.exclusive_group == group)
      {
        out.push_back(id);
      }
    }
    return out;
  }

  Prerequisites ChainModel::prerequisitesOf(std::uint32_t quest) const
  {
    Prerequisites result;
    auto const* target = find(quest);
    if (!target)
    {
      return result;
    }
    if (target->prev_quest > 0)
    {
      result.quests.push_back(static_cast<std::uint32_t>(target->prev_quest));
    }
    auto const unlockers = unlockedBy(quest);
    result.quests.insert(result.quests.end(), unlockers.begin(), unlockers.end());
    std::sort(result.quests.begin(), result.quests.end());
    result.quests.erase(std::unique(result.quests.begin(), result.quests.end()), result.quests.end());

    // All-of: the NextQuestId prerequisites share one negative group.
    bool const all_of = unlockers.size() >= 2 && std::all_of(unlockers.begin(), unlockers.end(), [&](std::uint32_t id)
    {
      auto const group = find(id)->exclusive_group;
      return group < 0 && group == find(unlockers.front())->exclusive_group;
    });
    result.mode = all_of ? Prerequisites::Mode::All : Prerequisites::Mode::Any;
    return result;
  }

  bool ChainModel::dependsOn(std::uint32_t quest, std::uint32_t other) const
  {
    std::set<std::uint32_t> seen{quest};
    std::deque<std::uint32_t> queue{quest};
    while (!queue.empty())
    {
      auto const current = queue.front();
      queue.pop_front();
      for (auto const prerequisite : prerequisitesOf(current).quests)
      {
        if (prerequisite == other)
        {
          return true;
        }
        if (seen.insert(prerequisite).second)
        {
          queue.push_back(prerequisite);
        }
      }
    }
    return false;
  }

  ChainResult ChainModel::setPrerequisites(std::uint32_t quest, Prerequisites const& requested)
  {
    auto* target = findMutable(quest);
    if (!target)
    {
      return problem(ChainProblem::UnknownQuest, quest);
    }
    std::vector<std::uint32_t> wanted = requested.quests;
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());

    auto const current = prerequisitesOf(quest);
    if (current.quests == wanted && (wanted.size() < 2 || current.mode == requested.mode))
    {
      return {};
    }
    if (!target->editable)
    {
      return problem(ChainProblem::NotEditable, quest);
    }

    // Check everything first: a refused change leaves the model untouched.
    for (auto const id : wanted)
    {
      auto const* prerequisite = find(id);
      if (!prerequisite)
      {
        return problem(ChainProblem::UnknownQuest, id);
      }
      if (id == quest)
      {
        return problem(ChainProblem::SameQuest, id);
      }
      if (dependsOn(id, quest))
      {
        return problem(ChainProblem::WouldLoop, id);
      }
      if (wanted.size() >= 2)
      {
        if (!prerequisite->editable)
        {
          return problem(ChainProblem::NotEditable, id);
        }
        if (prerequisite->next_quest != 0 && prerequisite->next_quest != static_cast<std::int32_t>(quest))
        {
          return problem(ChainProblem::UnlocksAnotherQuest, id);
        }
        if (requested.mode == Prerequisites::Mode::All && prerequisite->exclusive_group > 0)
        {
          return problem(ChainProblem::InAnotherGroup, id);
        }
      }
    }
    for (auto const id : unlockedBy(quest))
    {
      if (!find(id)->editable && !std::binary_search(wanted.begin(), wanted.end(), id))
      {
        return problem(ChainProblem::NotEditable, id);
      }
    }

    // A prerequisite that is dropped stops offering the quest (it would offer a quest players cannot take).
    for (auto const id : current.quests)
    {
      auto* old = findMutable(id);
      if (old && old->editable && old->next_in_chain == quest && !std::binary_search(wanted.begin(), wanted.end(), id))
      {
        old->next_in_chain = 0;
      }
    }

    // Detach the current prerequisites.
    if (target->prev_quest > 0)
    {
      target->prev_quest = 0;
    }
    for (auto const id : unlockedBy(quest))
    {
      auto* old = findMutable(id);
      old->next_quest = 0;
      if (old->exclusive_group < 0)
      {
        old->exclusive_group = 0;
      }
    }

    if (wanted.size() == 1)
    {
      target->prev_quest = static_cast<std::int32_t>(wanted.front());
    }
    else if (wanted.size() >= 2)
    {
      // vmangos names an all-of group after one of its members (negated).
      std::int32_t const group = -static_cast<std::int32_t>(wanted.front());
      for (auto const id : wanted)
      {
        auto* prerequisite = findMutable(id);
        prerequisite->next_quest = static_cast<std::int32_t>(quest);
        if (requested.mode == Prerequisites::Mode::All)
        {
          prerequisite->exclusive_group = group;
        }
      }
    }
    return {};
  }

  ChainResult ChainModel::addPrerequisite(std::uint32_t quest, std::uint32_t prerequisite,
                                          Prerequisites::Mode mode_if_several)
  {
    auto requested = prerequisitesOf(quest);
    if (std::find(requested.quests.begin(), requested.quests.end(), prerequisite) != requested.quests.end())
    {
      return {};
    }
    if (requested.quests.size() < 2)
    {
      requested.mode = mode_if_several;
    }
    requested.quests.push_back(prerequisite);
    return setPrerequisites(quest, requested);
  }

  ChainResult ChainModel::removePrerequisite(std::uint32_t quest, std::uint32_t prerequisite)
  {
    auto requested = prerequisitesOf(quest);
    requested.quests.erase(std::remove(requested.quests.begin(), requested.quests.end(), prerequisite),
                           requested.quests.end());
    return setPrerequisites(quest, requested);
  }

  std::vector<std::uint32_t> ChainModel::exclusiveWith(std::uint32_t quest) const
  {
    std::vector<std::uint32_t> out;
    auto const* target = find(quest);
    if (!target || target->exclusive_group <= 0)
    {
      return out;
    }
    for (auto const id : groupMembers(target->exclusive_group))
    {
      if (id != quest)
      {
        out.push_back(id);
      }
    }
    return out;
  }

  ChainResult ChainModel::makeExclusive(std::vector<std::uint32_t> const& quests)
  {
    std::vector<std::uint32_t> members = quests;
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    if (members.size() < 2)
    {
      return problem(ChainProblem::SameQuest, members.empty() ? 0 : members.front());
    }
    for (auto const id : members)
    {
      auto const* quest = find(id);
      if (!quest)
      {
        return problem(ChainProblem::UnknownQuest, id);
      }
      if (!quest->editable)
      {
        return problem(ChainProblem::NotEditable, id);
      }
      if (quest->exclusive_group < 0)
      {
        return problem(ChainProblem::InAnotherGroup, id);
      }
    }
    // Named after a member, like vmangos' own groups -- one whose name no quest outside the set uses, so
    // no bystander joins.
    std::int32_t group = 0;
    for (auto const id : members)
    {
      auto const users = groupMembers(static_cast<std::int32_t>(id));
      if (std::all_of(users.begin(), users.end(), [&](std::uint32_t user)
          { return std::binary_search(members.begin(), members.end(), user); }))
      {
        group = static_cast<std::int32_t>(id);
        break;
      }
    }
    if (!group)
    {
      for (auto const& [id, quest] : _quests)
      {
        group = std::max(group, quest.exclusive_group);
      }
      ++group;
    }
    for (auto const id : members)
    {
      findMutable(id)->exclusive_group = group;
    }
    return {};
  }

  ChainResult ChainModel::leaveExclusiveGroup(std::uint32_t quest)
  {
    auto* target = findMutable(quest);
    if (!target)
    {
      return problem(ChainProblem::UnknownQuest, quest);
    }
    if (target->exclusive_group <= 0)
    {
      return {};
    }
    if (!target->editable)
    {
      return problem(ChainProblem::NotEditable, quest);
    }
    target->exclusive_group = 0;
    return {};
  }

  ChainResult ChainModel::setOffers(std::uint32_t from, std::uint32_t to)
  {
    auto* source = findMutable(from);
    if (!source || (to && !find(to)))
    {
      return problem(ChainProblem::UnknownQuest, source ? to : from);
    }
    if (from == to)
    {
      return problem(ChainProblem::SameQuest, from);
    }
    if (source->next_in_chain == to)
    {
      return {};
    }
    if (!source->editable)
    {
      return problem(ChainProblem::NotEditable, from);
    }
    source->next_in_chain = to;
    return {};
  }

  std::set<std::uint32_t> ChainModel::chainOf(std::uint32_t entry) const
  {
    std::map<std::uint32_t, std::vector<std::uint32_t>> neighbours;
    auto const add = [&](std::uint32_t a, std::uint32_t b)
    {
      if (a && b && a != b && _quests.count(a) && _quests.count(b))
      {
        neighbours[a].push_back(b);
        neighbours[b].push_back(a);
      }
    };
    std::map<std::int32_t, std::uint32_t> group_anchor;
    for (auto const& [id, quest] : _quests)
    {
      if (quest.prev_quest > 0)
      {
        add(static_cast<std::uint32_t>(quest.prev_quest), id);
      }
      if (quest.next_quest > 0)
      {
        add(id, static_cast<std::uint32_t>(quest.next_quest));
      }
      add(id, quest.next_in_chain);
      if (quest.exclusive_group)
      {
        auto const [anchor, inserted] = group_anchor.emplace(quest.exclusive_group, id);
        if (!inserted)
        {
          add(anchor->second, id);
        }
      }
    }

    std::set<std::uint32_t> chain;
    if (!_quests.count(entry))
    {
      return chain;
    }
    std::deque<std::uint32_t> queue{entry};
    chain.insert(entry);
    while (!queue.empty())
    {
      auto const current = queue.front();
      queue.pop_front();
      for (auto const next : neighbours[current])
      {
        if (chain.insert(next).second)
        {
          queue.push_back(next);
        }
      }
    }
    return chain;
  }

  std::vector<ChainLink> ChainModel::links(std::set<std::uint32_t> const& shown) const
  {
    std::map<std::pair<std::uint32_t, std::uint32_t>, ChainLink> found;
    auto const link = [&](std::uint32_t from, std::uint32_t to) -> ChainLink&
    {
      auto& entry = found[{from, to}];
      entry.from = from;
      entry.to = to;
      return entry;
    };
    for (auto const id : shown)
    {
      if (!find(id))
      {
        continue;
      }
      for (auto const prerequisite : prerequisitesOf(id).quests)
      {
        if (shown.count(prerequisite))
        {
          link(prerequisite, id).needs_done = true;
        }
      }
      auto const next = find(id)->next_in_chain;
      if (next && next != id && shown.count(next))
      {
        link(id, next).offers = true;
      }
    }
    std::vector<ChainLink> out;
    for (auto const& [key, value] : found)
    {
      out.push_back(value);
    }
    return out;
  }

  std::map<std::uint32_t, int> ChainModel::layers(std::set<std::uint32_t> const& shown) const
  {
    std::map<std::uint32_t, int> layer;
    for (auto const id : shown)
    {
      layer[id] = 0;
    }
    auto const all_links = links(shown);
    // Longest-path layering by relaxation; the bound keeps a (database-made) cycle from looping forever.
    for (std::size_t pass = 0; pass < shown.size(); ++pass)
    {
      bool moved = false;
      for (auto const& link : all_links)
      {
        if (layer[link.to] < layer[link.from] + 1)
        {
          layer[link.to] = layer[link.from] + 1;
          moved = true;
        }
      }
      if (!moved)
      {
        break;
      }
    }
    return layer;
  }

  std::map<std::uint32_t, ChainQuest> ChainModel::changes() const
  {
    std::map<std::uint32_t, ChainQuest> out;
    for (auto const& [id, quest] : _quests)
    {
      auto const& original = _original.at(id);
      if (quest.prev_quest != original.prev_quest || quest.next_quest != original.next_quest
          || quest.exclusive_group != original.exclusive_group || quest.next_in_chain != original.next_in_chain)
      {
        out[id] = quest;
      }
    }
    return out;
  }
}
