// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/ui/quest/QuestPage.hpp>

class QLabel;
class QMenu;
class QPushButton;

namespace Noggit::Ui::Content
{
  class RowList;
}

namespace Noggit::Ui::Quest
{
  // What players must do: kill creatures, use objects, collect items (and where they come from), explore
  // a place, reach a reputation -- as a list of objective cards.
  class QuestObjectivesPage : public QuestPage
  {
  public:
    QuestObjectivesPage(QuestEditorSetup const& setup, QWidget* parent = nullptr);

    QString title() const override { return "Objectives"; }
    QString subtitle() const override { return "What players must do"; }
    void load(Noggit::Quest::QuestContent const& content) override;
    void store(Noggit::Quest::QuestContent& content) const override;
    QStringList problems() const override;

  private:
    void updateMenu();
    void updateSummary();
    int countOf(int kind) const;

    Content::RowList* _objectives = nullptr;
    QPushButton* _add = nullptr;
    QMenu* _menu = nullptr;
    QLabel* _summary = nullptr;
  };
}
