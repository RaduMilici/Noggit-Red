// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/ui/quest/QuestPage.hpp>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

namespace Noggit::Ui::Content
{
  class EntryPicker;
}

namespace Noggit::Ui::Quest
{
  // Title, levels, zone, kind of quest (elite, dungeon, timed, repeatable) and its texts.
  class QuestStoryPage : public QuestPage
  {
  public:
    QuestStoryPage(QuestEditorSetup const& setup, QWidget* parent = nullptr);

    QString title() const override { return "Story"; }
    QString subtitle() const override { return "Title, level and what the NPC says"; }
    void load(Noggit::Quest::QuestContent const& content) override;
    void store(Noggit::Quest::QuestContent& content) const override;
    QStringList problems() const override;

    QString questTitle() const;
    int questLevel() const;

  private:
    void updateVisibility();

    QLineEdit* _title = nullptr;
    QSpinBox* _level = nullptr;
    QSpinBox* _min_level = nullptr;
    Content::EntryPicker* _zone = nullptr;
    QComboBox* _type = nullptr;
    QSpinBox* _group_size = nullptr;
    QWidget* _group_row = nullptr;
    QCheckBox* _timed = nullptr;
    QSpinBox* _minutes = nullptr;
    QCheckBox* _repeatable = nullptr;
    QCheckBox* _sharable = nullptr;
    QPlainTextEdit* _details = nullptr;
    QPlainTextEdit* _objectives = nullptr;
    QPlainTextEdit* _request_items = nullptr;
    QPlainTextEdit* _offer_reward = nullptr;
    std::uint32_t _other_quest_flags = 0; // QuestFlags bits the page has no control for, kept as they are
  };
}
