// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/quest/QuestRewardsPage.hpp>

#include <noggit/ui/content/ContentStyle.hpp>

#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace Noggit::Ui::Quest
{
  using namespace Noggit::Ui::Content;
  namespace Q = Noggit::Quest;

  namespace
  {
    class ItemRow : public QWidget
    {
    public:
      ItemRow(LookupModel* items, std::uint32_t item, std::uint32_t count, QWidget* parent)
        : QWidget(parent)
      {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        picker = new EntryPicker(items, this);
        picker->setEntry(item);
        amount = new QSpinBox(this);
        amount->setRange(1, 255);
        amount->setPrefix("x ");
        amount->setValue(std::max<int>(count, 1));
        layout->addWidget(picker, 1);
        layout->addWidget(amount);
      }
      EntryPicker* picker = nullptr;
      QSpinBox* amount = nullptr;
    };

    class ReputationRow : public QWidget
    {
    public:
      ReputationRow(LookupModel* factions, std::uint32_t faction, std::int32_t value, QWidget* parent)
        : QWidget(parent)
      {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        points = new QSpinBox(this);
        points->setRange(-42000, 42000);
        points->setSingleStep(25);
        points->setValue(value ? value : 250);
        points->setToolTip("Reputation points: 250 is a normal quest, 500+ a big one. Negative values lose reputation.");
        picker = new EntryPicker(factions, this);
        picker->setEntry(faction);
        layout->addWidget(points);
        layout->addWidget(new QLabel("with", this));
        layout->addWidget(picker, 1);
      }
      QSpinBox* points = nullptr;
      EntryPicker* picker = nullptr;
    };

    template<typename Row>
    std::vector<Row*> typedRows(RowList const* list)
    {
      std::vector<Row*> out;
      for (auto* row : list->rows())
      {
        out.push_back(static_cast<Row*>(row));
      }
      return out;
    }
  }

  QuestRewardsPage::QuestRewardsPage(QuestEditorSetup const& setup, QWidget* parent)
    : QuestPage(setup, parent)
  {
    auto const& s = schema();

    auto* basics = new Card("Experience and money", {}, this);
    auto* form = new QFormLayout();
    basics->body()->addLayout(form);
    auto* xp_row = new QHBoxLayout();
    _xp = new QSpinBox(basics);
    _xp->setRange(0, 1000000);
    _xp->setSingleStep(10);
    _xp->setSuffix(" XP");
    _xp->setEnabled(!s.xp_col.empty());
    _xp->setToolTip("For a player at the quest's level; higher-level players get less.");
    auto* typical = new QPushButton("Use typical", basics);
    typical->setEnabled(_xp->isEnabled());
    _xp_hint = hintLabel({}, basics);
    xp_row->addWidget(_xp);
    xp_row->addWidget(typical);
    xp_row->addWidget(_xp_hint, 1);
    form->addRow("Experience", xp_row);
    connect(typical, &QPushButton::clicked, this, [this]
    {
      if (auto const xp = Q::suggestedXp(_setup.data->xp_by_level, static_cast<std::uint32_t>(_level)))
      {
        _xp->setValue(static_cast<int>(xp));
      }
    });

    auto* money_row = new QHBoxLayout();
    _gold = new QSpinBox(basics);
    _gold->setRange(0, 9999);
    _gold->setSuffix(" g");
    _silver = new QSpinBox(basics);
    _silver->setRange(0, 99);
    _silver->setSuffix(" s");
    _copper = new QSpinBox(basics);
    _copper->setRange(0, 99);
    _copper->setSuffix(" c");
    for (auto* spin : {_gold, _silver, _copper})
    {
      spin->setEnabled(!s.money_col.empty());
      money_row->addWidget(spin);
    }
    money_row->addStretch();
    form->addRow("Money", money_row);
    cards()->addWidget(basics);

    auto* items = new Card("Items", {}, this);
    items->body()->addWidget(hintLabel("Always given", items));
    _rewards = new RowList(items);
    items->body()->addWidget(_rewards);
    _add_reward = addButton("+ Add an item", items);
    items->body()->addWidget(_add_reward);
    items->body()->addWidget(hintLabel("The player picks one of", items));
    _choices = new RowList(items);
    items->body()->addWidget(_choices);
    _add_choice = addButton("+ Add a choice", items);
    items->body()->addWidget(_add_choice);
    connect(_add_reward, &QPushButton::clicked, this, [this] { addItemRow(_rewards, 0, 1); });
    connect(_add_choice, &QPushButton::clicked, this, [this] { addItemRow(_choices, 0, 1); });
    _rewards->on_changed = [this] { updateLimits(); changed(); };
    _choices->on_changed = [this] { updateLimits(); changed(); };
    cards()->addWidget(items);

    auto* reputation = new Card("Reputation", "Standing players gain (or lose) with factions.", this);
    _reputation = new RowList(reputation);
    reputation->body()->addWidget(_reputation);
    _add_reputation = addButton("+ Add a faction", reputation);
    reputation->body()->addWidget(_add_reputation);
    connect(_add_reputation, &QPushButton::clicked, this, [this] { addReputationRow(0, 250); });
    _reputation->on_changed = [this] { updateLimits(); changed(); };
    cards()->addWidget(reputation);

    auto* spells = new Card("Spells", {}, this);
    auto* spell_form = new QFormLayout();
    spells->body()->addLayout(spell_form);
    _spell = new EntryPicker(lookups().spells.get(), spells);
    _spell->setEnabled(!s.reward_spell_col.empty());
    _spell->setToolTip("A spell the player learns (shown as the quest reward).");
    spell_form->addRow("Teaches", _spell);
    _spell_cast = new EntryPicker(lookups().spells.get(), spells);
    _spell_cast->setEnabled(!s.reward_spell_cast_col.empty());
    _spell_cast->setToolTip("Cast on the player when handing the quest in (a buff, a teleport, ...).");
    spell_form->addRow("Casts on the player", _spell_cast);
    cards()->addWidget(spells);
    cards()->addStretch();
    updateLimits();
  }

  void QuestRewardsPage::setLevel(int level)
  {
    _level = level;
    auto const xp = Q::suggestedXp(_setup.data->xp_by_level, static_cast<std::uint32_t>(level));
    _xp_hint->setText(xp ? QString("Typical for a level %1 quest: %2 XP").arg(level).arg(xp) : QString());
  }

  void QuestRewardsPage::addItemRow(RowList* list, std::uint32_t item, std::uint32_t count)
  {
    list->addRow(new ItemRow(lookups().items.get(), item, count, list));
  }

  void QuestRewardsPage::addReputationRow(std::uint32_t faction, std::int32_t value)
  {
    _reputation->addRow(new ReputationRow(lookups().reputation_factions.get(), faction, value, _reputation));
  }

  void QuestRewardsPage::updateLimits()
  {
    auto const& s = schema();
    _add_reward->setEnabled(_rewards->count() < Q::slotCount(s.reward_cols));
    _add_choice->setEnabled(_choices->count() < Q::slotCount(s.choice_cols));
    _add_reputation->setEnabled(_reputation->count() < Q::slotCount(s.rep_reward_faction_cols));
  }

  void QuestRewardsPage::load(Q::QuestContent const& content)
  {
    auto const& f = content.fields;
    _xp->setValue(static_cast<int>(f.xp.value_or(0)));
    std::int32_t const money = f.money.value_or(0);
    _costs_money = money < 0;
    std::int32_t const shown = std::max(money, 0);
    _gold->setValue(shown / 10000);
    _silver->setValue((shown / 100) % 100);
    _copper->setValue(shown % 100);
    for (auto* spin : {_gold, _silver, _copper})
    {
      spin->setEnabled(!schema().money_col.empty() && !_costs_money);
      spin->setToolTip(_costs_money ? "This quest costs money to hand in; that is kept as it is." : QString());
    }
    _rewards->clear();
    for (auto const& item : f.rewards.value_or(std::vector<Q::ItemCount>{}))
    {
      addItemRow(_rewards, item.id, item.count);
    }
    _choices->clear();
    for (auto const& item : f.choices.value_or(std::vector<Q::ItemCount>{}))
    {
      addItemRow(_choices, item.id, item.count);
    }
    _reputation->clear();
    for (auto const& rep : f.rep_rewards.value_or(std::vector<Q::Reputation>{}))
    {
      addReputationRow(rep.faction, rep.value);
    }
    _spell->setEntry(f.reward_spell.value_or(0));
    _spell_cast->setEntry(f.reward_spell_cast.value_or(0));
    updateLimits();
  }

  void QuestRewardsPage::store(Q::QuestContent& content) const
  {
    auto const& s = schema();
    auto& f = content.fields;
    if (_xp->isEnabled()) f.xp = static_cast<std::uint32_t>(_xp->value());
    if (!s.money_col.empty() && !_costs_money)
    {
      f.money = _gold->value() * 10000 + _silver->value() * 100 + _copper->value();
    }
    auto const items = [](RowList const* list)
    {
      std::vector<Q::ItemCount> out;
      for (auto* row : typedRows<ItemRow>(list))
      {
        if (auto const id = row->picker->entry())
        {
          out.push_back({id, static_cast<std::uint32_t>(row->amount->value())});
        }
      }
      return out;
    };
    if (!s.reward_cols[0].empty()) f.rewards = items(_rewards);
    if (!s.choice_cols[0].empty()) f.choices = items(_choices);
    if (!s.rep_reward_faction_cols[0].empty())
    {
      std::vector<Q::Reputation> reputation;
      for (auto* row : typedRows<ReputationRow>(_reputation))
      {
        if (auto const faction = row->picker->entry())
        {
          reputation.push_back({faction, row->points->value()});
        }
      }
      f.rep_rewards = reputation;
    }
    if (_spell->isEnabled()) f.reward_spell = _spell->entry();
    if (_spell_cast->isEnabled()) f.reward_spell_cast = _spell_cast->entry();
  }
}
