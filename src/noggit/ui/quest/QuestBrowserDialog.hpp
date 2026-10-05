// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// The quest list: all of the local world's quests, or the ones an NPC gives or takes, with New / Copy /
// Edit / Delete, Test and the chain diagram. The work itself is done by QuestWorkflow.

#include <QtWidgets/QDialog>

#include <cstdint>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace Noggit::Ui::Content
{
  class ContentSession;
}

namespace Noggit::Ui::Quest
{
  class QuestBrowserDialog : public QDialog
  {
  public:
    QuestBrowserDialog(Content::ContentSession& session, std::uint32_t focus_npc, QWidget* parent = nullptr);

  private:
    void rebuildList();
    void select(std::uint32_t quest);
    std::uint32_t selectedQuest() const;
    void updateButtons();
    void openChain();
    void closeIfTesting();

    Content::ContentSession& _session;
    std::uint32_t _focus_npc;

    QLineEdit* _search = nullptr;
    QCheckBox* _only_focus = nullptr;
    QTreeWidget* _list = nullptr;
    QLabel* _status = nullptr;
    QPushButton* _copy = nullptr;
    QPushButton* _edit = nullptr;
    QPushButton* _delete = nullptr;
    QPushButton* _test = nullptr;
  };
}
