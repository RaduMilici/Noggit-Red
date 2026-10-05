// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/quest/QuestWorkflow.hpp>

#include <noggit/creator/TestSessionService.hpp>
#include <noggit/ui/content/ContentSession.hpp>
#include <noggit/ui/content/ContentStyle.hpp>
#include <noggit/ui/item/ItemWorkflow.hpp>
#include <noggit/ui/quest/QuestEditorDialog.hpp>

#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>

#include <stdexcept>

namespace Noggit::Ui::Quest
{
  namespace Q = Noggit::Quest;
  using Content::confirmChange;
  using Content::showProblems;
  using Content::toQStringList;

  namespace
  {
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

    void warn(QWidget* parent, QString const& title, std::exception const& e)
    {
      QMessageBox::warning(parent, title, QString::fromUtf8(e.what()));
    }

    // The quest list, the quest pickers and the quest givers' roles after a write.
    void refresh(Content::ContentSession& session, std::vector<std::uint32_t> const& new_givers)
    {
      session.reloadQuests();
      auto& creatures = *session.lookups().creatures;
      for (auto const npc : new_givers)
      {
        if (int const row = creatures.rowOf(npc); row > 0)
        {
          creatures.item(row)->setData(creatures.kindOf(npc) | Q::QUEST_GIVER_FLAG, Content::LookupModel::KindRole);
        }
      }
    }
  }

  std::uint32_t editQuest(Content::ContentSession& session, QWidget* parent, EditorMode mode, std::uint32_t source,
                          std::uint32_t follows, std::uint32_t default_npc)
  {
    Creator::QuestEditorData data;
    try
    {
      QApplication::setOverrideCursor(Qt::WaitCursor);
      data = Creator::loadQuest(session.layouts(), mode == EditorMode::Create ? 0 : source, session.quests());
      QApplication::restoreOverrideCursor();
    }
    catch (std::exception const& e)
    {
      QApplication::restoreOverrideCursor();
      warn(parent, "Quest", e);
      return 0;
    }
    std::uint32_t const entry = mode == EditorMode::Edit ? source : data.next_entry;

    auto const request_for = [&](Q::QuestContent content)
    {
      Creator::QuestSaveRequest request;
      request.mode = saveMode(mode);
      request.entry = entry;
      request.source = source;
      request.content = std::move(content);
      request.names = session.names();
      return request;
    };

    QuestEditorSetup setup;
    setup.mode = mode;
    setup.entry = entry;
    setup.data = &data;
    setup.lookups = &session.lookups();
    setup.default_npc = default_npc;
    setup.follows = follows;
    setup.cursor_position = session.cursor_position;
    setup.owns_item = [&session](std::uint32_t item) { return session.ownsItem(item); };
    setup.can_test = Creator::TestSessionService::instance() != nullptr;
    // Set before the dialog is built: its pages decide from these which buttons to show.
    QWidget* item_parent = parent;
    setup.create_item = [&] { return Item::createItem(session, item_parent); };
    setup.validate = [&](Q::QuestContent const& content)
    {
      try
      {
        return toQStringList(Creator::planQuestSave(session.layouts(), data, session.quests(), request_for(content)).problems);
      }
      catch (std::exception const& e)
      {
        return QStringList{QString::fromUtf8(e.what())};
      }
    };
    QuestEditorDialog dialog(setup, parent);
    item_parent = &dialog;
    if (dialog.exec() != QDialog::Accepted)
    {
      return 0;
    }

    auto const request = request_for(dialog.content());
    QString const title = QString::fromStdString(request.content.fields.title.value_or(std::string()));
    try
    {
      auto const plan = Creator::planQuestSave(session.layouts(), data, session.quests(), request);
      if (!plan.problems.empty())
      {
        showProblems(parent, "Quest", toQStringList(plan.problems));
        return 0;
      }
      QString const question = mode == EditorMode::Edit ? QString("Save the changes to \"%1\"?").arg(title)
                                                        : QString("Create the quest \"%1\"?").arg(title);
      if (!confirmChange(parent, mode == EditorMode::Edit ? "Save quest" : "Create quest", question, toQStringList(plan.summary)))
      {
        return 0;
      }
      auto const written = Creator::saveQuest(session.layouts(), data, session.quests(), request);
      session.setOwnQuest(entry, true);
      refresh(session, written.new_quest_givers);
    }
    catch (std::exception const& e)
    {
      warn(parent, "Quest", e);
      return 0;
    }
    session.lookups().addQuest(entry, title, request.content.fields.level.value_or(1));

    if (auto* tests = Creator::TestSessionService::instance(); tests && dialog.testRequested())
    {
      tests->testQuest(parent, entry);
    }
    else
    {
      QMessageBox::information(parent, mode == EditorMode::Edit ? "Quest saved" : "Quest created",
        QString("%1 \"%2\" (ID %3) in your local world.\n\nThe local server loads quests when it starts: use Test Local, "
                "or Save and test in the quest editor, to try it in game.")
          .arg(mode == EditorMode::Edit ? "Saved" : "Created").arg(title).arg(entry));
    }
    return entry;
  }

  bool deleteQuest(Content::ContentSession& session, QWidget* parent, std::uint32_t quest)
  {
    try
    {
      auto const data = Creator::loadQuest(session.layouts(), quest, session.quests());
      auto const names = session.names();
      auto const plan = Creator::planQuestDeletion(session.layouts(), data, session.quests(), quest, names);
      if (!plan.problems.empty())
      {
        showProblems(parent, "Delete quest", toQStringList(plan.problems));
        return false;
      }
      QString const title = QString::fromStdString(data.content.fields.title.value_or(std::string()));
      if (!confirmChange(parent, "Delete quest", QString("Delete the quest \"%1\" from your local world?").arg(title),
                         toQStringList(plan.summary)))
      {
        return false;
      }
      Creator::deleteQuest(session.layouts(), data, session.quests(), quest, names);
      session.setOwnQuest(quest, false);
      refresh(session, {});
      return true;
    }
    catch (std::exception const& e)
    {
      warn(parent, "Delete quest", e);
      return false;
    }
  }

  bool saveChainChanges(Content::ContentSession& session, QWidget* parent,
                        std::map<std::uint32_t, Q::ChainQuest> const& changes)
  {
    if (changes.empty())
    {
      return true;
    }
    QStringList names;
    for (auto const& [entry, quest] : changes)
    {
      names << QString("\"%1\"").arg(QString::fromStdString(quest.title));
    }
    if (!confirmChange(parent, "Save quest chain", QString("Save the chain links of %1?").arg(names.join(", ")), {}))
    {
      return false;
    }
    try
    {
      Creator::saveChain(session.layouts(), changes);
      refresh(session, {});
      return true;
    }
    catch (std::exception const& e)
    {
      warn(parent, "Quest chain", e);
      return false;
    }
  }
}
