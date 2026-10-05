// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/ui/quest/QuestPage.hpp>

#include <QtWidgets/QDialog>

#include <vector>

class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;

namespace Noggit::Ui::Quest
{
  class QuestRewardsPage;
  class QuestStoryPage;

  // The quest editor: a sidebar of pages (Story, Who can take it, Objectives, Rewards, Givers, Events),
  // a header naming the quest, and the problems still to fix before it can be saved. Collects the
  // choices only; QuestWorkflow plans and writes them.
  class QuestEditorDialog : public QDialog
  {
  public:
    QuestEditorDialog(QuestEditorSetup const& setup, QWidget* parent = nullptr);

    // The quest as edited (the stored content with every page's values applied).
    Noggit::Quest::QuestContent content() const;

    // Shows a page by index (0 = Story), e.g. to open straight on the events.
    void showPage(int index);

    void accept() override;

    // True when closed with "Save and test".
    bool testRequested() const { return _test_requested; }

  private:
    void refresh();

    QuestEditorSetup const& _setup;
    std::vector<QuestPage*> _pages;
    QuestStoryPage* _story = nullptr;
    QuestRewardsPage* _rewards = nullptr;
    QLabel* _header = nullptr;
    QLabel* _subheader = nullptr;
    QListWidget* _sidebar = nullptr;
    QStackedWidget* _stack = nullptr;
    QLabel* _problem = nullptr;
    QPushButton* _ok = nullptr;
    QPushButton* _test = nullptr;
    bool _test_requested = false;
  };
}
