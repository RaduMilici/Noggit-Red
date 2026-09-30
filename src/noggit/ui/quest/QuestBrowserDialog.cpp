// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifdef USE_MYSQL_UID_STORAGE

#include <noggit/ui/quest/QuestBrowserDialog.hpp>

#include <noggit/content/SqlText.hpp>
#include <noggit/ui/content/ContentSession.hpp>
#include <noggit/ui/content/ContentStyle.hpp>
#include <noggit/ui/item/ItemWorkflow.hpp>
#include <noggit/ui/quest/QuestChainDialog.hpp>
#include <noggit/ui/quest/QuestWorkflow.hpp>

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <map>
#include <set>

namespace Noggit::Ui::Quest
{
  using Noggit::Content::isCustom;
  namespace Q = Noggit::Quest;

  QuestBrowserDialog::QuestBrowserDialog(Content::ContentSession& session, std::uint32_t focus_npc, QWidget* parent)
    : QDialog(parent)
    , _session(session)
    , _focus_npc(focus_npc)
  {
    setWindowTitle("Quests");
    resize(980, 600);
    Content::applyEditorStyle(this);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 12);
    auto* header = new QLabel("Quests", this);
    header->setObjectName("ContentHeader");
    root->addWidget(header);

    auto* filters = new QHBoxLayout();
    _search = new QLineEdit(this);
    _search->setPlaceholderText("Search by title, ID or NPC...");
    _search->setClearButtonEnabled(true);
    filters->addWidget(_search, 1);
    _only_focus = new QCheckBox(this);
    _only_focus->setVisible(focus_npc != 0);
    _only_focus->setChecked(focus_npc != 0);
    _only_focus->setText(QString("Only quests of %1").arg(session.lookups().creatureName(focus_npc)));
    filters->addWidget(_only_focus);
    root->addLayout(filters);

    _list = new QTreeWidget(this);
    _list->setHeaderLabels({"ID", "Title", "Level", "Given by", "Handed in to"});
    _list->setRootIsDecorated(false);
    _list->setAlternatingRowColors(true);
    _list->setSortingEnabled(true);
    _list->sortByColumn(0, Qt::AscendingOrder);
    _list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    root->addWidget(_list, 1);
    _status = Content::hintLabel({}, this);
    root->addWidget(_status);

    auto* buttons = new QHBoxLayout();
    auto* create = new QPushButton(focus_npc ? QString("New quest for %1...").arg(session.lookups().creatureName(focus_npc))
                                             : QString("New quest..."), this);
    _copy = new QPushButton("Copy...", this);
    _copy->setToolTip("A new quest starting as a copy of the selected one.");
    _edit = new QPushButton("Edit...", this);
    _delete = new QPushButton("Delete...", this);
    auto* chain = new QPushButton("Chain...", this);
    chain->setToolTip("The selected quest's chain as a diagram: link quests by drawing arrows. With nothing selected: "
                      "every chain your quests are in.");
    auto* items = new QPushButton("Your items...", this);
    items->setToolTip("Items made for your quests (\"Wolf Pelt\"): create, change or delete them.");
    auto* close = new QPushButton("Close", this);
    for (auto* button : {create, _copy, _edit, _delete, chain})
    {
      buttons->addWidget(button);
    }
    buttons->addStretch();
    buttons->addWidget(items);
    buttons->addWidget(close);
    root->addLayout(buttons);

    auto const run = [this](EditorMode mode)
    {
      std::uint32_t const source = mode == EditorMode::Create ? 0 : selectedQuest();
      if (mode != EditorMode::Create && !source)
      {
        return;
      }
      if (auto const saved = editQuest(_session, this, mode, source, 0, _focus_npc))
      {
        rebuildList();
        select(saved);
      }
    };
    connect(create, &QPushButton::clicked, this, [run] { run(EditorMode::Create); });
    connect(_copy, &QPushButton::clicked, this, [run] { run(EditorMode::Copy); });
    connect(_edit, &QPushButton::clicked, this, [run] { run(EditorMode::Edit); });
    connect(_list, &QTreeWidget::itemDoubleClicked, this, [this, run]
    {
      run(isCustom(selectedQuest()) ? EditorMode::Edit : EditorMode::Copy);
    });
    connect(_delete, &QPushButton::clicked, this, [this]
    {
      if (auto const quest = selectedQuest(); quest && deleteQuest(_session, this, quest))
      {
        rebuildList();
      }
    });
    connect(chain, &QPushButton::clicked, this, [this] { openChain(); });
    connect(items, &QPushButton::clicked, this, [this] { Item::manageItems(_session, this); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(_search, &QLineEdit::textChanged, this, [this] { rebuildList(); });
    connect(_only_focus, &QCheckBox::toggled, this, [this] { rebuildList(); });
    connect(_list, &QTreeWidget::itemSelectionChanged, this, [this] { updateButtons(); });
    rebuildList();
  }

  void QuestBrowserDialog::rebuildList()
  {
    auto const& lookups = _session.lookups();
    auto const& list = _session.quests();
    std::map<std::uint32_t, QStringList> givers, takers;
    std::set<std::uint32_t> focus_quests;
    for (auto const& link : list.links)
    {
      QString const name = link.giver.kind == Q::Giver::Kind::Npc ? lookups.creatureName(link.giver.entry)
                                                                  : lookups.objectName(link.giver.entry);
      (link.starts ? givers : takers)[link.quest] << name.section(" (#", 0, 0);
      if (link.giver.kind == Q::Giver::Kind::Npc && link.giver.entry == _focus_npc)
      {
        focus_quests.insert(link.quest);
      }
    }

    QString const needle = _search->text().trimmed();
    std::uint32_t const selected = selectedQuest();
    _list->setSortingEnabled(false);
    _list->clear();
    std::size_t shown = 0;
    for (auto const& quest : list.quests)
    {
      if (_only_focus->isChecked() && !focus_quests.count(quest.entry))
      {
        continue;
      }
      QString const title = QString::fromStdString(quest.title);
      QString const given = givers[quest.entry].join(", ");
      QString const taken = takers[quest.entry].join(", ");
      if (!needle.isEmpty() && !title.contains(needle, Qt::CaseInsensitive) && !QString::number(quest.entry).contains(needle)
          && !given.contains(needle, Qt::CaseInsensitive) && !taken.contains(needle, Qt::CaseInsensitive))
      {
        continue;
      }
      auto* item = new QTreeWidgetItem(_list);
      item->setData(0, Qt::DisplayRole, quest.entry);
      item->setText(1, title);
      item->setData(2, Qt::DisplayRole, quest.level);
      item->setText(3, given);
      item->setText(4, taken);
      if (isCustom(quest.entry))
      {
        QFont font = item->font(1);
        font.setBold(true);
        item->setFont(1, font);
        item->setToolTip(1, "Your quest: can be edited and deleted.");
      }
      ++shown;
    }
    _list->setSortingEnabled(true);
    _list->resizeColumnToContents(0);
    _list->resizeColumnToContents(2);
    _status->setText(QString("%1 of %2 quests shown. Your own quests are in bold; double-click one to edit it, or a "
                             "game quest to make an editable copy.").arg(shown).arg(list.quests.size()));
    select(selected);
    updateButtons();
  }

  void QuestBrowserDialog::select(std::uint32_t quest)
  {
    if (!quest)
    {
      return;
    }
    auto found = _list->findItems(QString::number(quest), Qt::MatchFixedString, 0);
    if (found.isEmpty() && (_only_focus->isChecked() || !_search->text().isEmpty()))
    {
      // Filtered out: show everything.
      _only_focus->blockSignals(true);
      _only_focus->setChecked(false);
      _only_focus->blockSignals(false);
      _search->blockSignals(true);
      _search->clear();
      _search->blockSignals(false);
      rebuildList();
      found = _list->findItems(QString::number(quest), Qt::MatchFixedString, 0);
    }
    if (!found.isEmpty())
    {
      _list->setCurrentItem(found.front());
      _list->scrollToItem(found.front());
    }
  }

  std::uint32_t QuestBrowserDialog::selectedQuest() const
  {
    auto const* item = _list->currentItem();
    return item && item->isSelected() ? item->data(0, Qt::DisplayRole).toUInt() : 0;
  }

  void QuestBrowserDialog::updateButtons()
  {
    auto const quest = selectedQuest();
    bool const custom = isCustom(quest);
    _copy->setEnabled(quest != 0);
    _edit->setEnabled(custom);
    _delete->setEnabled(custom);
    QString const locked = QString("Only quests made here (ID %1 and up) can be changed, so the game's own quests stay "
                                   "intact. Use Copy to make an editable version.").arg(Noggit::Content::customIds().entry_start);
    _edit->setToolTip(custom || !quest ? QString("Change the selected quest.") : locked);
    _delete->setToolTip(custom || !quest ? QString("Delete the selected quest.") : locked);
  }

  void QuestBrowserDialog::openChain()
  {
    QuestChainDialog chain(_session, selectedQuest(), this);
    chain.exec();
    rebuildList();
  }
}

#endif
