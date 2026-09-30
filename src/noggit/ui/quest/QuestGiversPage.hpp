// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/ui/quest/QuestPage.hpp>

class QSpinBox;

namespace Noggit::Ui::Content
{
  class EntryPicker;
  class RowList;
}

namespace Noggit::Ui::Quest
{
  // Who gives the quest and who takes it back (NPCs or objects such as a wanted poster), the item that
  // starts it, the item handed out on accepting, and the quest offered right after.
  class QuestGiversPage : public QuestPage
  {
  public:
    QuestGiversPage(QuestEditorSetup const& setup, QWidget* parent = nullptr);

    QString title() const override { return "Givers"; }
    QString subtitle() const override { return "Who gives it, who takes it back"; }
    void load(Noggit::Quest::QuestContent const& content) override;
    void store(Noggit::Quest::QuestContent& content) const override;
    QStringList problems() const override;

  private:
    void addGiver(Content::RowList* list, Noggit::Quest::Giver const& giver);
    std::vector<Noggit::Quest::Giver> giversOf(Content::RowList const* list) const;

    Content::RowList* _starters = nullptr;
    Content::RowList* _enders = nullptr;
    Content::EntryPicker* _start_item = nullptr;
    Content::EntryPicker* _source_item = nullptr;
    QSpinBox* _source_count = nullptr;
    Content::EntryPicker* _next = nullptr;
  };
}
