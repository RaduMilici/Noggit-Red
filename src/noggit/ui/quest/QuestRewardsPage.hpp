// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/ui/quest/QuestPage.hpp>

class QLabel;
class QPushButton;
class QSpinBox;

namespace Noggit::Ui::Content
{
  class EntryPicker;
  class RowList;
}

namespace Noggit::Ui::Quest
{
  // Experience, money, items (always given / pick one), reputation and spells.
  class QuestRewardsPage : public QuestPage
  {
  public:
    QuestRewardsPage(QuestEditorSetup const& setup, QWidget* parent = nullptr);

    QString title() const override { return "Rewards"; }
    QString subtitle() const override { return "Experience, money, items, reputation"; }
    void load(Noggit::Quest::QuestContent const& content) override;
    void store(Noggit::Quest::QuestContent& content) const override;

    // The quest level, for the typical experience (the dialog passes it on from the Story page).
    void setLevel(int level);

  private:
    void addItemRow(Content::RowList* list, std::uint32_t item, std::uint32_t count);
    void addReputationRow(std::uint32_t faction, std::int32_t value);
    void updateLimits();

    int _level = 1;
    QSpinBox* _xp = nullptr;
    QLabel* _xp_hint = nullptr;
    QSpinBox* _gold = nullptr;
    QSpinBox* _silver = nullptr;
    QSpinBox* _copper = nullptr;
    bool _costs_money = false; // a negative money value: the quest costs money, left as it is
    Content::RowList* _rewards = nullptr;
    Content::RowList* _choices = nullptr;
    Content::RowList* _reputation = nullptr;
    QPushButton* _add_reward = nullptr;
    QPushButton* _add_choice = nullptr;
    QPushButton* _add_reputation = nullptr;
    Content::EntryPicker* _spell = nullptr;
    Content::EntryPicker* _spell_cast = nullptr;
  };
}
