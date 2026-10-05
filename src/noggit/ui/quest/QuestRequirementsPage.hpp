// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/ui/quest/QuestPage.hpp>

#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QRadioButton;
class QSpinBox;

namespace Noggit::Ui::Content
{
  class EntryPicker;
  class RowList;
}

namespace Noggit::Ui::Quest
{
  // Who can take the quest: races, classes, a profession, a reputation, earlier quests, and quests it
  // excludes (either/or).
  class QuestRequirementsPage : public QuestPage
  {
  public:
    QuestRequirementsPage(QuestEditorSetup const& setup, QWidget* parent = nullptr);

    QString title() const override { return "Who can take it"; }
    QString subtitle() const override { return "Races, classes, skills, earlier quests"; }
    void load(Noggit::Quest::QuestContent const& content) override;
    void store(Noggit::Quest::QuestContent& content) const override;
    QStringList problems() const override;

  private:
    struct ReputationRow
    {
      QCheckBox* enabled = nullptr;
      QComboBox* rank = nullptr;
      Content::EntryPicker* faction = nullptr;
    };

    void addQuestRow(Content::RowList* list, std::uint32_t quest);
    std::vector<std::uint32_t> questsOf(Content::RowList const* list) const;
    void updateModeVisibility();

    QComboBox* _races = nullptr;
    std::vector<std::pair<std::uint32_t, QCheckBox*>> _classes; // class mask bit -> checkbox
    std::uint32_t _unknown_class_bits = 0;
    QComboBox* _skill = nullptr;
    QSpinBox* _skill_value = nullptr;
    ReputationRow _min_rep, _max_rep;
    Content::RowList* _prerequisites = nullptr;
    QWidget* _mode_row = nullptr;
    QRadioButton* _needs_all = nullptr;
    QRadioButton* _needs_any = nullptr;
    Content::RowList* _exclusive = nullptr;
  };
}
