// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifdef USE_MYSQL_UID_STORAGE

#include <noggit/ui/quest/QuestWorkflow.hpp>

#include <noggit/ui/content/ContentSession.hpp>
#include <noggit/ui/content/SqlApply.hpp>
#include <noggit/ui/item/ItemWorkflow.hpp>
#include <noggit/ui/quest/QuestEditorDialog.hpp>

#include <QtWidgets/QMessageBox>

namespace Noggit::Ui::Quest
{
  namespace Q = Noggit::Quest;

  namespace
  {
    QString exportPath(std::uint32_t quest)
    {
      return QString("quests/quest_%1.sql").arg(quest);
    }

    // Refreshes the re-runnable copies of the changed quests and of the custom NPCs that became quest givers.
    void recordChanges(Content::ContentSession& session, Q::QuestDatabase const& db, std::set<std::uint32_t> const& quests,
                       std::vector<std::uint32_t> const& new_givers)
    {
      for (auto const quest : quests)
      {
        writeSqlExport(exportPath(quest), mysql::content::questSnapshot(session.db(), db, quest), {});
      }
      auto const npc_db = Noggit::Npc::detectNpcDatabase(session.db().columns());
      for (auto const npc : new_givers)
      {
        if (Noggit::Content::isCustom(npc))
        {
          writeSqlExport(QString("npcs/npc_%1.sql").arg(npc), mysql::content::npcSnapshot(session.db(), npc_db, npc), {});
        }
      }
      session.reloadQuests();
    }

    Q::SaveMode saveMode(EditorMode mode)
    {
      switch (mode)
      {
        case EditorMode::Create: return Q::SaveMode::Create;
        case EditorMode::Copy: return Q::SaveMode::Copy;
        case EditorMode::Edit: return Q::SaveMode::Edit;
      }
      return Q::SaveMode::Create;
    }
  }

  std::uint32_t editQuest(Content::ContentSession& session, QWidget* parent, EditorMode mode, std::uint32_t source,
                          std::uint32_t follows, std::uint32_t default_npc)
  {
    mysql::content::QuestEditorData data;
    std::string error;
    if (!mysql::content::loadQuest(session.db(), mode == EditorMode::Create ? 0 : source, session.quests(), data, &error))
    {
      QMessageBox::critical(parent, "Quest", QString::fromStdString(error));
      return 0;
    }
    std::uint32_t const entry = mode == EditorMode::Edit ? source : data.next_entry;

    // Plans the save for the content given; the editor asks before closing, the save runs the same plan.
    auto const plan_for = [&](Q::QuestContent content)
    {
      auto inputs = mysql::content::saveInputs(session.db(), data, session.quests(), content);
      inputs.names = session.names();
      return Q::planQuestSave(data.db, saveMode(mode), entry, mode == EditorMode::Copy ? source : 0, content,
                              data.content, data.stored, inputs);
    };

    QuestEditorSetup setup;
    setup.mode = mode;
    setup.entry = entry;
    setup.data = &data;
    setup.lookups = &session.lookups();
    setup.default_npc = default_npc;
    setup.follows = follows;
    setup.cursor_position = session.cursor_position;
    // Set before the dialog is built: its pages decide from these which buttons to show.
    QWidget* item_parent = parent;
    setup.create_item = [&] { return Item::createItem(session, item_parent); };
    setup.validate = [&](Q::QuestContent const& content) { return toQStringList(plan_for(content).problems); };
    QuestEditorDialog dialog(setup, parent);
    item_parent = &dialog;
    if (dialog.exec() != QDialog::Accepted)
    {
      return 0;
    }

    auto const content = dialog.content();
    auto const plan = plan_for(content);
    if (!plan.problems.empty())
    {
      showProblems(parent, "Quest", toQStringList(plan.problems));
      return 0;
    }
    QString const title = QString::fromStdString(content.fields.title.value_or(std::string()));
    QString const question = mode == EditorMode::Edit ? QString("Save the changes to \"%1\" (ID %2)?").arg(title).arg(entry)
                                                      : QString("Create the quest \"%1\" (ID %2)?").arg(title).arg(entry);
    if (!applyPlan(parent, session.db(), mode == EditorMode::Edit ? "Save quest" : "Create quest", question,
                   toQStringList(plan.summary), plan.statements, plan.cleanup, plan.require_first_row))
    {
      return 0;
    }
    recordChanges(session, data.db, plan.quests_changed, plan.new_quest_givers);
    session.lookups().addQuest(entry, title, content.fields.level.value_or(1));
    for (auto const npc : plan.new_quest_givers)
    {
      auto& creatures = *session.lookups().creatures;
      int const row = creatures.rowOf(npc);
      if (row > 0)
      {
        creatures.item(row)->setData(creatures.kindOf(npc) | Q::QUEST_GIVER_FLAG, Content::LookupModel::KindRole);
      }
    }
    QMessageBox::information(parent, mode == EditorMode::Edit ? "Quest saved" : "Quest created",
      QString("%1 \"%2\" (ID %3).\n\nRestart the world server (mangosd) so it loads the %4. A re-runnable copy is in "
              "the project's sql_exports/quests/ folder.")
        .arg(mode == EditorMode::Edit ? "Saved" : "Created").arg(title).arg(entry)
        .arg(mode == EditorMode::Edit ? "changes" : "new quest"));
    return entry;
  }

  bool deleteQuest(Content::ContentSession& session, QWidget* parent, std::uint32_t quest)
  {
    mysql::content::QuestEditorData data;
    std::string error;
    if (!mysql::content::loadQuest(session.db(), quest, session.quests(), data, &error))
    {
      QMessageBox::critical(parent, "Delete quest", QString::fromStdString(error));
      return false;
    }
    auto content = data.content;
    auto inputs = mysql::content::saveInputs(session.db(), data, session.quests(), content);
    inputs.names = session.names();
    auto const plan = Q::planQuestDelete(data.db, quest, data.content, data.stored, inputs);
    if (!plan.problems.empty())
    {
      showProblems(parent, "Delete quest", toQStringList(plan.problems));
      return false;
    }
    QString const title = QString::fromStdString(content.fields.title.value_or(std::string()));
    if (!applyPlan(parent, session.db(), "Delete quest",
                   QString("Delete the quest \"%1\" (ID %2)? This cannot be undone.").arg(title).arg(quest),
                   toQStringList(plan.summary), plan.statements))
    {
      return false;
    }
    removeSqlExport(exportPath(quest));
    auto others = plan.quests_changed;
    others.erase(quest);
    recordChanges(session, data.db, others, {});
    return true;
  }

  bool saveChainChanges(Content::ContentSession& session, QWidget* parent,
                        std::map<std::uint32_t, Q::ChainQuest> const& changes)
  {
    if (changes.empty())
    {
      return true;
    }
    auto const db = Q::detectQuestDatabase(session.db().columns());
    std::vector<std::string> statements;
    QStringList names;
    std::set<std::uint32_t> quests;
    for (auto const& [entry, quest] : changes)
    {
      Q::QuestFields fields;
      fields.prev_quest = quest.prev_quest;
      fields.next_quest = quest.next_quest;
      fields.exclusive_group = quest.exclusive_group;
      fields.next_in_chain = quest.next_in_chain;
      if (auto update = Q::buildQuestUpdate(db.quest, entry, fields); !update.empty())
      {
        statements.push_back(std::move(update));
      }
      names << QString("\"%1\"").arg(QString::fromStdString(quest.title));
      quests.insert(entry);
    }
    if (statements.empty())
    {
      QMessageBox::information(parent, "Quest chain", "This database keeps quest chains in a table the editor cannot change.");
      return false;
    }
    if (!applyPlan(parent, session.db(), "Save quest chain", QString("Save the chain links of %1?").arg(names.join(", ")),
                   {}, statements))
    {
      return false;
    }
    recordChanges(session, db, quests, {});
    return true;
  }
}

#endif
