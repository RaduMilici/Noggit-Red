// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/ui/quest/QuestPage.hpp>

class QLabel;

namespace Noggit::Ui::Content
{
  class RowList;
}

namespace Noggit::Ui::Quest
{
  // Scripted events: what happens when the quest is accepted and when it is handed in, as a timeline of
  // simple actions (the NPC talks, emotes, casts, gives an item, spawns a creature, attacks, completes the
  // quest for "listen to the story" quests).
  class QuestEventsPage : public QuestPage
  {
  public:
    QuestEventsPage(QuestEditorSetup const& setup, QWidget* parent = nullptr);

    QString title() const override { return "Events"; }
    QString subtitle() const override { return "What happens on accepting / handing in"; }
    void load(Noggit::Quest::QuestContent const& content) override;
    void store(Noggit::Quest::QuestContent& content) const override;
    QStringList problems() const override;

  private:
    Content::RowList* makeTimeline(QString const& title, QString const& description, bool on_accept);
    void addAction(Content::RowList* list, Noggit::Quest::ScriptAction const& action);
    std::vector<Noggit::Quest::ScriptAction> actionsOf(Content::RowList const* list) const;

    Content::RowList* _on_accept = nullptr;
    Content::RowList* _on_complete = nullptr;
    QLabel* _incomplete = nullptr;
  };
}
