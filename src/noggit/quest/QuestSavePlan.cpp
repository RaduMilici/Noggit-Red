// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/quest/QuestSavePlan.hpp>

#include <algorithm>

namespace Noggit::Quest
{
  namespace
  {
    std::string named(std::function<std::string(std::uint32_t)> const& lookup, std::uint32_t id)
    {
      std::string name = lookup ? lookup(id) : std::string();
      return "\"" + (name.empty() ? "#" + std::to_string(id) : name) + "\"";
    }

    void append(std::vector<std::string>& to, std::vector<std::string> const& more)
    {
      for (auto const& statement : more)
      {
        if (!statement.empty())
        {
          to.push_back(statement);
        }
      }
    }

    QuestFields chainFields(ChainQuest const& quest)
    {
      QuestFields fields;
      fields.prev_quest = quest.prev_quest;
      fields.next_quest = quest.next_quest;
      fields.exclusive_group = quest.exclusive_group;
      fields.next_in_chain = quest.next_in_chain;
      return fields;
    }

    ChainQuest const* findIn(std::vector<ChainQuest> const& quests, std::uint32_t entry)
    {
      auto const found = std::find_if(quests.begin(), quests.end(), [entry](ChainQuest const& q) { return q.entry == entry; });
      return found == quests.end() ? nullptr : &*found;
    }

    bool sameDrop(QuestDrop const& a, QuestDrop const& b)
    {
      return a.item == b.item && a.source == b.source && a.source_entry == b.source_entry;
    }

    std::string sourceName(QuestDrop const& drop, NameLookup const& names)
    {
      return drop.source == QuestDrop::Source::Creature ? named(names.npc, drop.source_entry)
                                                        : named(names.object, drop.source_entry);
    }

    // Writes the other quests whose chain columns the model changed; for a create, cleanup restores them.
    void writeOtherChainChanges(QuestDatabase const& db, ChainModel const& model, std::uint32_t entry,
                                SaveInputs const& inputs, SavePlan& plan, bool with_cleanup)
    {
      for (auto const& [id, quest] : model.changes())
      {
        if (id == entry)
        {
          continue;
        }
        plan.statements.push_back(buildQuestUpdate(db.quest, id, chainFields(quest)));
        plan.quests_changed.insert(id);
        plan.summary.push_back("The chain links of " + named(inputs.names.quest, id) + " change too.");
        if (with_cleanup)
        {
          if (auto const* original = findIn(inputs.chain, id))
          {
            plan.cleanup.push_back(buildQuestUpdate(db.quest, id, chainFields(*original)));
          }
        }
      }
    }
  }

  QuestDatabase detectQuestDatabase(Noggit::Npc::ColumnsOf const& columns_of)
  {
    QuestDatabase db;
    db.quest = detectQuestSchema(columns_of("quest_template"));
    db.links = detectLinkSchema(columns_of);
    db.loot = detectLootSchema(columns_of("creature_loot_template"), columns_of("gameobject_loot_template"),
                               columns_of("creature_template"), columns_of("gameobject_template"));
    db.scripts = detectScriptSchema(columns_of);
    db.npc = Noggit::Npc::detectNpcSchema(columns_of);
    return db;
  }

  std::string describe(ChainResult const& result, NameLookup const& names)
  {
    auto const quest = named(names.quest, result.quest);
    switch (result.problem)
    {
      case ChainProblem::None: return {};
      case ChainProblem::UnknownQuest: return "Quest " + quest + " does not exist.";
      case ChainProblem::SameQuest: return "A quest cannot depend on itself.";
      case ChainProblem::NotEditable:
        return quest + " is a quest from the game, so it cannot be changed. Only your own quests can be linked this way.";
      case ChainProblem::UnlocksAnotherQuest:
        return quest + " already unlocks another quest. When several quests are needed, each can unlock only one quest.";
      case ChainProblem::InAnotherGroup:
        return quest + " is already part of an either/or choice or a \"needs all of\" requirement.";
      case ChainProblem::WouldLoop: return quest + " already depends on this quest: linking them would make a circle.";
    }
    return {};
  }

  std::vector<std::string> questRowsDelete(QuestDatabase const& db, std::uint32_t entry, QuestStored const& stored)
  {
    std::vector<std::string> statements{buildQuestRowDelete(db.quest, entry)};
    append(statements, buildLinkDeletes(db.links, entry));
    append(statements, buildScriptDeletes(db.scripts, ScriptWhen::Accepted, entry, stored.accept_text_ids));
    append(statements, buildScriptDeletes(db.scripts, ScriptWhen::Completed, entry, stored.complete_text_ids));
    return statements;
  }

  SavePlan planQuestSave(QuestDatabase const& db,
                         SaveMode mode,
                         std::uint32_t entry,
                         std::uint32_t source,
                         QuestContent const& content,
                         QuestContent const& before,
                         QuestStored const& stored,
                         SaveInputs const& inputs)
  {
    SavePlan plan;
    plan.quest = entry;
    auto const& names = inputs.names;
    bool const editing = mode == SaveMode::Edit;

    // --- prerequisites, either/or, follow-up: through the chain model, which may change other quests ---
    std::vector<ChainQuest> chain = inputs.chain;
    if (!findIn(chain, entry))
    {
      // A new quest (or one the chain list does not have yet): it starts without links.
      ChainQuest created;
      created.entry = entry;
      created.title = content.fields.title.value_or(std::string());
      created.editable = Noggit::Content::isCustom(entry);
      chain.push_back(created);
    }
    ChainModel model(chain);
    auto const check = [&](ChainResult const& result)
    {
      if (!result)
      {
        plan.problems.push_back(describe(result, names));
      }
    };
    check(model.setPrerequisites(entry, content.prerequisites));
    if (content.exclusive_with.empty())
    {
      check(model.leaveExclusiveGroup(entry));
    }
    else
    {
      auto members = content.exclusive_with;
      members.push_back(entry);
      check(model.makeExclusive(members));
    }
    check(model.setOffers(entry, content.offers_next));
    // A new quest that follows one of the user's quests that offers nothing yet: offered right after it.
    if (!editing && content.prerequisites.quests.size() == 1)
    {
      auto const previous = content.prerequisites.quests.front();
      if (auto const* quest = model.find(previous); quest && quest->editable && !quest->next_in_chain)
      {
        model.setOffers(previous, entry);
        plan.summary.push_back("It is offered right after " + named(names.quest, previous) + " is handed in.");
      }
    }

    if (content.links.area_trigger)
    {
      auto const owner = inputs.trigger_quests.find(content.links.area_trigger);
      if (owner != inputs.trigger_quests.end() && owner->second && owner->second != entry)
      {
        plan.problems.push_back("That place already completes the quest " + named(names.quest, owner->second)
                                + ". Pick another spot.");
      }
    }
    if (!plan.problems.empty())
    {
      return plan;
    }

    // --- the quest row ---
    QuestFields fields = content.fields;
    if (auto const* quest = model.find(entry))
    {
      auto const links = chainFields(*quest);
      fields.prev_quest = links.prev_quest;
      fields.next_quest = links.next_quest;
      fields.exclusive_group = links.exclusive_group;
      fields.next_in_chain = links.next_in_chain;
    }
    bool const write_scripts = db.scripts.valid() && (!editing || content.on_accept != before.on_accept || content.on_complete != before.on_complete);
    std::vector<ScriptAction> const& on_accept = write_scripts ? content.on_accept : before.on_accept;
    std::vector<ScriptAction> const& on_complete = write_scripts ? content.on_complete : before.on_complete;
    if (!db.quest.special_flags_col.empty())
    {
      bool const event = content.links.area_trigger || completesQuest(on_accept) || completesQuest(on_complete);
      std::uint32_t const flags = content.fields.special_flags.value_or(before.fields.special_flags.value_or(0));
      fields.special_flags = (flags & ~SPECIAL_EXPLORATION_OR_EVENT) | (event ? SPECIAL_EXPLORATION_OR_EVENT : 0u);
    }
    if (write_scripts)
    {
      fields.start_script = on_accept.empty() ? 0u : entry;
      fields.complete_script = on_complete.empty() ? 0u : entry;
    }

    if (editing)
    {
      if (auto update = buildQuestUpdate(db.quest, entry, fields); !update.empty())
      {
        plan.statements.push_back(std::move(update));
      }
    }
    else
    {
      plan.statements.push_back(buildQuestInsert(db.quest, entry, fields, mode == SaveMode::Copy ? source : 0));
      plan.require_first_row = true;
    }

    // --- who gives / takes it, the start item, the exploration spot ---
    append(plan.statements, buildLinkStatements(db.links, entry, content.links, editing ? before.links.start_item : 0));
    if (content.links.start_item)
    {
      plan.summary.push_back("The item " + named(names.item, content.links.start_item) + " starts this quest when looted.");
    }

    // --- quest item drops ---
    for (auto const& drop : content.drops)
    {
      append(plan.statements, buildDropStatements(db.loot, drop));
      plan.summary.push_back(sourceName(drop, names) + " drops " + named(names.item, drop.item) + " ("
                             + std::to_string(static_cast<int>(drop.chance + 0.5f)) + " %) for players on the quest."
                             + " Every creature or object sharing its loot does too.");
    }
    if (editing)
    {
      for (auto const& old : before.drops)
      {
        bool const kept = std::any_of(content.drops.begin(), content.drops.end(),
                                      [&](QuestDrop const& drop) { return sameDrop(drop, old); });
        if (!kept && !stored.shared_items.count(old.item))
        {
          plan.statements.push_back(buildDropRemoval(db.loot, old.source, old.loot_id, old.item));
          plan.summary.push_back(sourceName(old, names) + " no longer drops " + named(names.item, old.item) + ".");
        }
      }
    }

    // --- scripted events ---
    std::uint32_t next_text_id = inputs.next_text_id;
    std::uint32_t const first_text_id = next_text_id;
    if (write_scripts)
    {
      append(plan.statements, buildScriptStatements(db.scripts, ScriptWhen::Accepted, entry, entry, on_accept,
                                                    editing ? stored.accept_text_ids : std::vector<std::uint32_t>{},
                                                    next_text_id));
      append(plan.statements, buildScriptStatements(db.scripts, ScriptWhen::Completed, entry, entry, on_complete,
                                                    editing ? stored.complete_text_ids : std::vector<std::uint32_t>{},
                                                    next_text_id));
      if (!content.scripts_complete)
      {
        plan.summary.push_back("The quest's scripted events had steps this editor cannot show; they are replaced by "
                               "the events listed in the editor.");
      }
    }

    // --- other quests' chain links ---
    writeOtherChainChanges(db, model, entry, inputs, plan, !editing);

    // --- quest-giver role, last: a failure before it leaves no NPC changed ---
    std::vector<Giver> linked = content.links.starters;
    linked.insert(linked.end(), content.links.enders.begin(), content.links.enders.end());
    for (auto const npc : npcEntries(linked))
    {
      auto const flags = inputs.npc_flags.find(npc);
      if ((flags == inputs.npc_flags.end() || !(flags->second & QUEST_GIVER_FLAG))
          && std::find(plan.new_quest_givers.begin(), plan.new_quest_givers.end(), npc) == plan.new_quest_givers.end())
      {
        plan.new_quest_givers.push_back(npc);
      }
    }
    if (!plan.new_quest_givers.empty())
    {
      plan.statements.push_back(buildQuestGiverFlagStatement(db.npc, plan.new_quest_givers));
      std::string list;
      for (auto const npc : plan.new_quest_givers)
      {
        list += (list.empty() ? "" : ", ") + named(names.npc, npc);
      }
      plan.summary.push_back("These NPCs become quest givers: " + list + ".");
    }

    // --- cleanup for a create / copy that fails part-way (MyISAM cannot roll back) ---
    if (!editing)
    {
      std::vector<std::uint32_t> new_texts;
      for (std::uint32_t id = first_text_id; id < next_text_id; ++id)
      {
        new_texts.push_back(id);
      }
      QuestStored created;
      created.accept_text_ids = new_texts;
      std::vector<std::string> cleanup = questRowsDelete(db, entry, created);
      cleanup.insert(cleanup.end(), plan.cleanup.begin(), plan.cleanup.end());
      plan.cleanup = std::move(cleanup);
    }
    plan.quests_changed.insert(entry);
    return plan;
  }

  SavePlan planQuestDelete(QuestDatabase const& db,
                           std::uint32_t entry,
                           QuestContent const& content,
                           QuestStored const& stored,
                           SaveInputs const& inputs)
  {
    SavePlan plan;
    plan.quest = entry;
    auto const& names = inputs.names;

    // Every chain link to the quest goes.
    ChainModel model(inputs.chain);
    for (auto const& quest : inputs.chain)
    {
      if (quest.entry == entry)
      {
        continue;
      }
      auto const prerequisites = model.prerequisitesOf(quest.entry).quests;
      if (std::find(prerequisites.begin(), prerequisites.end(), entry) != prerequisites.end())
      {
        if (auto result = model.removePrerequisite(quest.entry, entry); !result)
        {
          plan.problems.push_back(describe(result, names));
        }
      }
      if (quest.next_in_chain == entry)
      {
        model.setOffers(quest.entry, 0);
      }
    }
    model.setPrerequisites(entry, {});
    model.leaveExclusiveGroup(entry);
    if (!plan.problems.empty())
    {
      return plan;
    }

    append(plan.statements, questRowsDelete(db, entry, stored));
    for (auto const& drop : content.drops)
    {
      if (!stored.shared_items.count(drop.item))
      {
        plan.statements.push_back(buildDropRemoval(db.loot, drop.source, drop.loot_id, drop.item));
        plan.summary.push_back(sourceName(drop, names) + " no longer drops " + named(names.item, drop.item) + ".");
      }
    }
    writeOtherChainChanges(db, model, entry, inputs, plan, false);
    return plan;
  }
}
