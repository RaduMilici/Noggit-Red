// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/quest/QuestPage.hpp>

#include <QtWidgets/QVBoxLayout>

namespace Noggit::Ui::Quest
{
  QuestPage::QuestPage(QuestEditorSetup const& setup, QWidget* parent)
    : QWidget(parent)
    , _setup(setup)
  {
    _cards = new QVBoxLayout(this);
    _cards->setContentsMargins(4, 4, 12, 12);
    _cards->setSpacing(12);
  }

  void QuestPage::changed() const
  {
    if (on_changed)
    {
      on_changed();
    }
  }
}
