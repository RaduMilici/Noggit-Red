// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/quest/QuestRequirementsPage.hpp>

#include <noggit/quest/QuestCatalog.hpp>
#include <noggit/ui/content/ContentStyle.hpp>

#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

namespace Noggit::Ui::Quest
{
  using namespace Noggit::Ui::Content;
  namespace Q = Noggit::Quest;

  namespace
  {
    // A picker row in a RowList.
    class QuestRow : public QWidget
    {
    public:
      QuestRow(LookupModel* model, std::uint32_t quest, QWidget* parent)
        : QWidget(parent)
      {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        picker = new EntryPicker(model, this);
        picker->setEntry(quest);
        layout->addWidget(picker, 1);
      }
      EntryPicker* picker = nullptr;
    };

    QComboBox* rankCombo(QWidget* parent)
    {
      auto* combo = new QComboBox(parent);
      for (auto const& rank : Q::reputationRanks())
      {
        combo->addItem(rank.label, rank.value);
      }
      return combo;
    }
  }

  QuestRequirementsPage::QuestRequirementsPage(QuestEditorSetup const& setup, QWidget* parent)
    : QuestPage(setup, parent)
  {
    auto const& s = schema();
    bool const vanilla = lookups().vanilla;

    // --- players ---
    auto* players = new Card("Players", {}, this);
    auto* form = new QFormLayout();
    players->body()->addLayout(form);
    _races = new QComboBox(players);
    _races->addItem("Everyone", 0u);
    _races->addItem("Alliance only", Q::allianceRaces(vanilla));
    _races->addItem("Horde only", Q::hordeRaces(vanilla));
    _races->setEnabled(!s.races_col.empty());
    form->addRow("Faction", _races);

    auto* classes = new QWidget(players);
    auto* grid = new QGridLayout(classes);
    grid->setContentsMargins(0, 0, 0, 0);
    int index = 0;
    for (auto const& player_class : Q::playerClasses(vanilla))
    {
      auto* check = new QCheckBox(player_class.name, classes);
      check->setEnabled(!s.classes_col.empty());
      grid->addWidget(check, index / 5, index % 5);
      _classes.emplace_back(player_class.mask(), check);
      connect(check, &QCheckBox::toggled, this, [this] { changed(); });
      ++index;
    }
    form->addRow("Classes", classes);
    players->body()->addWidget(hintLabel("Untick the classes that should not get this quest.", players));
    cards()->addWidget(players);

    // --- profession ---
    auto* skill = new Card("Profession", "For fishing, cooking and other profession quests.", this);
    auto* skill_row = new QHBoxLayout();
    _skill = new QComboBox(skill);
    _skill->addItem("No profession needed", 0u);
    for (auto const& profession : Q::professions(vanilla))
    {
      _skill->addItem(profession.label, profession.id);
    }
    _skill_value = new QSpinBox(skill);
    _skill_value->setRange(1, vanilla ? 300 : 450);
    _skill_value->setPrefix("at least ");
    skill_row->addWidget(_skill, 1);
    skill_row->addWidget(_skill_value);
    skill->body()->addLayout(skill_row);
    _skill->setEnabled(!s.skill_col.empty());
    connect(_skill, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]
    {
      _skill_value->setEnabled(_skill->isEnabled() && _skill->currentData().toUInt() != 0);
    });
    cards()->addWidget(skill);

    // --- reputation ---
    auto* reputation = new Card("Reputation", "Only offered to players standing well (or badly) enough with a faction.", this);
    auto const rep_row = [&](QString const& text, ReputationRow& row, bool available)
    {
      auto* line = new QHBoxLayout();
      row.enabled = new QCheckBox(text, reputation);
      row.rank = rankCombo(reputation);
      row.faction = new EntryPicker(lookups().reputation_factions.get(), reputation);
      line->addWidget(row.enabled);
      line->addWidget(row.rank);
      line->addWidget(new QLabel("with", reputation));
      line->addWidget(row.faction, 1);
      reputation->body()->addLayout(line);
      row.enabled->setEnabled(available);
      auto const update = [row] { row.rank->setEnabled(row.enabled->isChecked()); row.faction->setEnabled(row.enabled->isChecked()); };
      connect(row.enabled, &QCheckBox::toggled, this, update);
      update();
    };
    rep_row("At least", _min_rep, !s.min_rep_faction_col.empty());
    rep_row("At most", _max_rep, !s.max_rep_faction_col.empty());
    cards()->addWidget(reputation);

    // --- earlier quests ---
    bool const chains = !s.prev_quest_col.empty();
    auto* earlier = new Card("Earlier quests", "The quest only appears once these are done.", this);
    _prerequisites = new RowList(earlier);
    earlier->body()->addWidget(_prerequisites);
    _mode_row = new QWidget(earlier);
    auto* mode_layout = new QHBoxLayout(_mode_row);
    mode_layout->setContentsMargins(0, 0, 0, 0);
    _needs_all = new QRadioButton("All of them", _mode_row);
    _needs_any = new QRadioButton("Any one of them", _mode_row);
    auto* mode_group = new QButtonGroup(_mode_row);
    mode_group->addButton(_needs_all);
    mode_group->addButton(_needs_any);
    _needs_all->setChecked(true);
    mode_layout->addWidget(new QLabel("Players must finish:", _mode_row));
    mode_layout->addWidget(_needs_all);
    mode_layout->addWidget(_needs_any);
    mode_layout->addStretch();
    earlier->body()->addWidget(_mode_row);
    auto* add_earlier = addButton("+ Add an earlier quest", earlier);
    add_earlier->setEnabled(chains);
    earlier->body()->addWidget(add_earlier);
    earlier->body()->addWidget(hintLabel("One earlier quest can be any quest. Several must all be your own quests.", earlier));
    connect(add_earlier, &QPushButton::clicked, this, [this] { addQuestRow(_prerequisites, 0); });
    _prerequisites->on_changed = [this] { updateModeVisibility(); changed(); };
    cards()->addWidget(earlier);

    // --- either / or ---
    auto* exclusive = new Card("Only one of these", "Doing this quest closes these other quests, and doing one of them closes "
                                                   "this one -- e.g. choosing a side. Your own quests only.", this);
    _exclusive = new RowList(exclusive);
    exclusive->body()->addWidget(_exclusive);
    auto* add_exclusive = addButton("+ Add a quest", exclusive);
    add_exclusive->setEnabled(!s.exclusive_group_col.empty());
    exclusive->body()->addWidget(add_exclusive);
    connect(add_exclusive, &QPushButton::clicked, this, [this] { addQuestRow(_exclusive, 0); });
    _exclusive->on_changed = [this] { changed(); };
    cards()->addWidget(exclusive);
    cards()->addStretch();
    updateModeVisibility();
  }

  void QuestRequirementsPage::addQuestRow(RowList* list, std::uint32_t quest)
  {
    list->addRow(new QuestRow(lookups().quests.get(), quest, list));
  }

  std::vector<std::uint32_t> QuestRequirementsPage::questsOf(RowList const* list) const
  {
    std::vector<std::uint32_t> quests;
    for (auto* row : list->rows())
    {
      if (auto const quest = static_cast<QuestRow*>(row)->picker->entry())
      {
        quests.push_back(quest);
      }
    }
    return quests;
  }

  void QuestRequirementsPage::updateModeVisibility()
  {
    _mode_row->setVisible(_prerequisites->count() >= 2);
  }

  void QuestRequirementsPage::load(Q::QuestContent const& content)
  {
    auto const& f = content.fields;
    int race = _races->findData(f.races.value_or(0));
    if (race < 0)
    {
      _races->addItem("Some races only (kept as they are)", f.races.value_or(0));
      race = _races->count() - 1;
    }
    _races->setCurrentIndex(race);

    std::uint32_t const classes = f.classes.value_or(0);
    std::uint32_t known = 0;
    for (auto const& [bit, check] : _classes)
    {
      check->setChecked(classes == 0 || (classes & bit));
      known |= bit;
    }
    _unknown_class_bits = classes & ~known;

    int const skill = _skill->findData(f.skill.value_or(0));
    _skill->setCurrentIndex(skill >= 0 ? skill : 0);
    _skill_value->setValue(std::max<int>(f.skill_value.value_or(1), 1));
    _skill_value->setEnabled(_skill->isEnabled() && _skill->currentData().toUInt() != 0);

    auto const set_rep = [](ReputationRow const& row, std::optional<Q::Reputation> const& rep)
    {
      bool const on = rep && rep->faction;
      row.enabled->setChecked(on);
      row.faction->setEntry(on ? rep->faction : 0);
      row.rank->setCurrentIndex(static_cast<int>(Q::reputationRankOf(on ? rep->value : 3000)));
    };
    set_rep(_min_rep, f.min_rep);
    set_rep(_max_rep, f.max_rep);

    _prerequisites->clear();
    for (auto const quest : content.prerequisites.quests)
    {
      addQuestRow(_prerequisites, quest);
    }
    if (content.prerequisites.quests.empty() && _setup.follows)
    {
      addQuestRow(_prerequisites, _setup.follows);
    }
    (content.prerequisites.mode == Q::Prerequisites::Mode::All ? _needs_all : _needs_any)->setChecked(true);
    _exclusive->clear();
    for (auto const quest : content.exclusive_with)
    {
      addQuestRow(_exclusive, quest);
    }
    updateModeVisibility();
  }

  void QuestRequirementsPage::store(Q::QuestContent& content) const
  {
    auto& f = content.fields;
    if (_races->isEnabled()) f.races = _races->currentData().toUInt();
    if (!_classes.empty() && _classes.front().second->isEnabled())
    {
      std::uint32_t mask = 0, all = 0;
      for (auto const& [bit, check] : _classes)
      {
        all |= bit;
        mask |= check->isChecked() ? bit : 0u;
      }
      // Every class ticked = no restriction.
      f.classes = mask == all && !_unknown_class_bits ? 0u : (mask | _unknown_class_bits);
    }
    if (_skill->isEnabled())
    {
      f.skill = _skill->currentData().toUInt();
      f.skill_value = *f.skill ? static_cast<std::uint32_t>(_skill_value->value()) : 0u;
    }
    auto const rep = [](ReputationRow const& row) -> std::optional<Q::Reputation>
    {
      if (!row.enabled->isEnabled())
      {
        return std::nullopt;
      }
      if (!row.enabled->isChecked() || !row.faction->entry())
      {
        return Q::Reputation{};
      }
      return Q::Reputation{row.faction->entry(), row.rank->currentData().toInt()};
    };
    f.min_rep = rep(_min_rep);
    f.max_rep = rep(_max_rep);
    content.prerequisites.quests = questsOf(_prerequisites);
    content.prerequisites.mode = _needs_all->isChecked() ? Q::Prerequisites::Mode::All : Q::Prerequisites::Mode::Any;
    content.exclusive_with = questsOf(_exclusive);
  }

  QStringList QuestRequirementsPage::problems() const
  {
    QStringList problems;
    if (!_classes.empty() && _classes.front().second->isEnabled()
        && std::none_of(_classes.begin(), _classes.end(), [](auto const& c) { return c.second->isChecked(); }))
    {
      problems << "Tick at least one class (Who can take it).";
    }
    return problems;
  }
}
