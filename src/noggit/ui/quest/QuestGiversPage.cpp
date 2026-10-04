// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/quest/QuestGiversPage.hpp>

#include <noggit/ui/content/ContentStyle.hpp>

#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

namespace Noggit::Ui::Quest
{
  using namespace Noggit::Ui::Content;
  namespace Q = Noggit::Quest;

  namespace
  {
    class GiverRow : public QWidget
    {
    public:
      GiverRow(Q::Giver const& giver, ContentLookups& lookups, QWidget* parent)
        : QWidget(parent)
        , kind(giver.kind)
      {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        bool const npc = kind == Q::Giver::Kind::Npc;
        layout->addWidget(badge(npc ? "NPC" : "OBJECT", npc ? BadgeColor::event : BadgeColor::use, this), 0, Qt::AlignTop);
        picker = new EntryPicker(npc ? lookups.creatures.get() : lookups.objects.get(), this);
        picker->setEntry(giver.entry);
        layout->addWidget(picker, 1);
      }
      Q::Giver::Kind const kind;
      EntryPicker* picker = nullptr;
    };

    QWidget* addButtons(RowList* list, QString const& what, std::function<void(Q::Giver::Kind)> add,
                        bool objects_available, QWidget* parent)
    {
      auto* row = new QWidget(parent);
      auto* layout = new QHBoxLayout(row);
      layout->setContentsMargins(0, 0, 0, 0);
      auto* npc = addButton("+ An NPC", row);
      auto* object = addButton("+ An object", row);
      object->setToolTip("A wanted poster, a book, a grave... players click it to " + what + ".");
      object->setEnabled(objects_available);
      layout->addWidget(npc);
      layout->addWidget(object);
      layout->addStretch();
      QObject::connect(npc, &QPushButton::clicked, list, [add] { add(Q::Giver::Kind::Npc); });
      QObject::connect(object, &QPushButton::clicked, list, [add] { add(Q::Giver::Kind::Object); });
      return row;
    }
  }

  QuestGiversPage::QuestGiversPage(QuestEditorSetup const& setup, QWidget* parent)
    : QuestPage(setup, parent)
  {
    auto const& links = _setup.data->db.links;
    auto const& s = schema();

    auto* given = new Card("Given by", "They show a yellow ! to players who can take the quest. NPCs that are not "
                                       "quest givers yet become quest givers.", this);
    _starters = new RowList(given);
    given->body()->addWidget(_starters);
    given->body()->addWidget(addButtons(_starters, "get the quest",
                                        [this](Q::Giver::Kind kind) { addGiver(_starters, {kind, 0}); },
                                        links.object_starters.valid(), given));
    _starters->on_changed = [this] { changed(); };
    cards()->addWidget(given);

    auto* ended = new Card("Handed in to", "Players return here when they are done (a yellow ?). Often the same NPC.", this);
    _enders = new RowList(ended);
    ended->body()->addWidget(_enders);
    ended->body()->addWidget(addButtons(_enders, "hand the quest in",
                                        [this](Q::Giver::Kind kind) { addGiver(_enders, {kind, 0}); },
                                        links.object_enders.valid(), ended));
    _enders->on_changed = [this] { changed(); };
    cards()->addWidget(ended);

    auto* items = new Card("Items", {}, this);
    auto* form = new QFormLayout();
    items->body()->addLayout(form);
    auto* start_row = new QHBoxLayout();
    _start_item = new EntryPicker(lookups().items.get(), items);
    _start_item->setEnabled(!links.item_start_quest_col.empty());
    _start_item->setToolTip("Looting this item offers the quest (\"This Item Begins a Quest\"). Only your own items "
                            "can be used, so game items keep working as before.");
    start_row->addWidget(browsable(_start_item), 1);
    if (_setup.create_item)
    {
      auto* create = new QPushButton("New item...", items);
      create->setEnabled(_start_item->isEnabled());
      start_row->addWidget(create);
      connect(create, &QPushButton::clicked, this, [this]
      {
        if (auto const id = _setup.create_item())
        {
          _start_item->setEntry(id);
        }
      });
    }
    form->addRow("Started by an item", start_row);
    auto* source_row = new QHBoxLayout();
    _source_item = new EntryPicker(lookups().items.get(), items);
    _source_item->setEnabled(!s.source_item_col.empty());
    _source_item->setToolTip("Put in the player's bags when they accept (\"Here, take this bottle\").");
    _source_count = new QSpinBox(items);
    _source_count->setRange(1, 255);
    _source_count->setPrefix("x ");
    _source_count->setEnabled(!s.source_item_count_col.empty());
    source_row->addWidget(browsable(_source_item), 1);
    source_row->addWidget(_source_count);
    form->addRow("Given on accepting", source_row);
    cards()->addWidget(items);

    auto* next = new Card("Next quest", "Offered by the NPC right after this one is handed in, without closing the window.", this);
    _next = new EntryPicker(lookups().quests.get(), next);
    _next->setEnabled(!s.next_in_chain_col.empty());
    next->body()->addWidget(_next);
    cards()->addWidget(next);
    cards()->addStretch();
  }

  void QuestGiversPage::addGiver(RowList* list, Q::Giver const& giver)
  {
    list->addRow(new GiverRow(giver, lookups(), list));
  }

  std::vector<Q::Giver> QuestGiversPage::giversOf(RowList const* list) const
  {
    std::vector<Q::Giver> out;
    for (auto* widget : list->rows())
    {
      auto* row = static_cast<GiverRow*>(widget);
      if (auto const entry = row->picker->entry())
      {
        out.push_back({row->kind, entry});
      }
    }
    return out;
  }

  void QuestGiversPage::load(Q::QuestContent const& content)
  {
    _starters->clear();
    _enders->clear();
    auto starters = content.links.starters;
    auto enders = content.links.enders;
    // Copies do not inherit the original's givers (the original's NPC would offer both quests).
    if (_setup.mode == EditorMode::Copy)
    {
      starters.clear();
      enders.clear();
    }
    if (_setup.mode != EditorMode::Edit && starters.empty() && _setup.default_npc)
    {
      starters = {{Q::Giver::Kind::Npc, _setup.default_npc}};
      enders = starters;
    }
    for (auto const& giver : starters)
    {
      addGiver(_starters, giver);
    }
    for (auto const& giver : enders)
    {
      addGiver(_enders, giver);
    }
    _start_item->setEntry(_setup.mode == EditorMode::Copy ? 0 : content.links.start_item);
    auto const source = content.fields.source_item.value_or(Q::ItemCount{0, 1});
    _source_item->setEntry(source.id);
    _source_count->setValue(std::max<int>(source.count, 1));
    _next->setEntry(_setup.mode == EditorMode::Copy ? 0 : content.offers_next);
  }

  void QuestGiversPage::store(Q::QuestContent& content) const
  {
    content.links.starters = giversOf(_starters);
    content.links.enders = giversOf(_enders);
    if (_start_item->isEnabled()) content.links.start_item = _start_item->entry();
    if (_source_item->isEnabled())
    {
      content.fields.source_item = Q::ItemCount{_source_item->entry(), static_cast<std::uint32_t>(_source_count->value())};
    }
    if (_next->isEnabled()) content.offers_next = _next->entry();
  }

  QStringList QuestGiversPage::problems() const
  {
    QStringList problems;
    if (giversOf(_starters).empty() && !_start_item->entry())
    {
      problems << "Choose who gives the quest, or an item that starts it (Givers).";
    }
    if (giversOf(_enders).empty())
    {
      problems << "Choose who the quest is handed in to (Givers).";
    }
    if (auto const item = _start_item->entry(); item && !(_setup.owns_item && _setup.owns_item(item)))
    {
      problems << "Only your own items can start a quest (Givers) -- make one with \"New item...\".";
    }
    if (_next->entry() && _next->entry() == _setup.entry)
    {
      problems << "A quest cannot be offered after itself (Givers).";
    }
    return problems;
  }
}
