// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/quest/QuestObjectivesPage.hpp>

#include <noggit/quest/QuestCatalog.hpp>
#include <noggit/ui/content/ContentStyle.hpp>

#include <QtGui/QStandardItemModel>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace Noggit::Ui::Quest
{
  using namespace Noggit::Ui::Content;
  namespace Q = Noggit::Quest;

  namespace
  {
    enum Kind
    {
      KILL,
      USE,
      COLLECT,
      EXPLORE,
      REPUTATION,
    };

    // One objective card. The page reads them back through the typed subclasses.
    class ObjectiveRow : public QWidget
    {
    public:
      ObjectiveRow(Kind kind, QString const& label, QColor const& color, QWidget* parent)
        : QWidget(parent), kind(kind)
      {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(badge(label, color, this), 0, Qt::AlignTop);
        fields = new QGridLayout();
        fields->setHorizontalSpacing(8);
        fields->setVerticalSpacing(4);
        layout->addLayout(fields, 1);
      }
      Kind const kind;
      QGridLayout* fields = nullptr;
    };

    QSpinBox* countBox(QWidget* parent, int value)
    {
      auto* spin = new QSpinBox(parent);
      spin->setRange(1, 255);
      spin->setPrefix("x ");
      spin->setValue(std::max(value, 1));
      return spin;
    }

    class TargetRow : public ObjectiveRow
    {
    public:
      TargetRow(Q::Target const& target, ContentLookups& lookups, QWidget* parent)
        : ObjectiveRow(target.kind == Q::Target::Kind::Creature ? KILL : USE,
                       target.kind == Q::Target::Kind::Creature ? "KILL" : "USE",
                       target.kind == Q::Target::Kind::Creature ? BadgeColor::kill : BadgeColor::use, parent)
        , object(target.kind == Q::Target::Kind::Object)
      {
        picker = new EntryPicker(object ? lookups.usable_objects.get() : lookups.creatures.get(), this);
        picker->setEntry(target.id);
        count = countBox(this, static_cast<int>(target.count));
        text = new QLineEdit(QString::fromStdString(target.text), this);
        text->setPlaceholderText(object ? "Quest log text (optional), e.g. \"Brazier lit\""
                                        : "Quest log text (optional) -- default: \"<name> slain\"");
        fields->addWidget(picker, 0, 0);
        fields->addWidget(count, 0, 1);
        fields->addWidget(text, 1, 0, 1, 2);
        if (!object)
        {
          // Rarely needed: tucked behind a link unless the objective already uses it.
          auto* reveal = new QPushButton("Count it only when a spell is cast on it...", this);
          reveal->setFlat(true);
          reveal->setCursor(Qt::PointingHandCursor);
          reveal->setStyleSheet("text-align: left; padding: 0;");
          spell = new EntryPicker(lookups.spells.get(), this);
          spell->setEntry(target.spell);
          spell->setToolTip("Only counts when players cast this spell on the creature (e.g. \"use the net on 8 "
                            "birds\"). Leave at (none) to count kills.");
          fields->addWidget(reveal, 2, 0, 1, 2);
          fields->addWidget(spell, 3, 0, 1, 2);
          spell->setVisible(target.spell != 0);
          reveal->setVisible(target.spell == 0);
          connect(reveal, &QPushButton::clicked, this, [this, reveal]
          {
            reveal->hide();
            spell->show();
            spell->setFocus();
          });
        }
        else
        {
          picker->setToolTip("Objects players click (levers, altars, braziers, ...). Using one counts once per object.");
        }
        fields->setColumnStretch(0, 1);
      }

      Q::Target value() const
      {
        Q::Target target;
        target.kind = object ? Q::Target::Kind::Object : Q::Target::Kind::Creature;
        target.id = picker->entry();
        target.count = static_cast<std::uint32_t>(count->value());
        target.text = text->text().trimmed().toStdString();
        target.spell = spell ? spell->entry() : 0;
        return target;
      }

      bool const object;
      EntryPicker* picker = nullptr;
      QSpinBox* count = nullptr;
      QLineEdit* text = nullptr;
      EntryPicker* spell = nullptr;
    };

    class CollectRow : public ObjectiveRow
    {
    public:
      CollectRow(Q::ItemCount const& item, std::optional<Q::QuestDrop> const& drop, QuestEditorSetup const& setup,
                 QWidget* parent)
        : ObjectiveRow(COLLECT, "COLLECT", BadgeColor::collect, parent)
      {
        auto& lookups = *setup.lookups;
        auto const& loot = setup.data->db.loot;
        picker = new EntryPicker(lookups.items.get(), this);
        picker->setEntry(item.id);
        count = countBox(this, static_cast<int>(item.count));
        fields->addWidget(picker, 0, 0);
        fields->addWidget(count, 0, 1);
        if (setup.create_item)
        {
          auto* create = new QPushButton("New item...", this);
          create->setToolTip("Make a new item for this quest (e.g. \"Wolf Pelt\").");
          fields->addWidget(create, 0, 2);
          auto const create_item = setup.create_item;
          connect(create, &QPushButton::clicked, this, [this, create_item]
          {
            if (auto const id = create_item())
            {
              picker->setEntry(id);
            }
          });
        }

        source = new QComboBox(this);
        source->addItem("Players find it some other way", -1);
        if (loot.creatureDrops())
        {
          source->addItem("Dropped by", static_cast<int>(Q::QuestDrop::Source::Creature));
        }
        if (loot.objectDrops())
        {
          source->addItem("Found in", static_cast<int>(Q::QuestDrop::Source::Object));
        }
        creature = new EntryPicker(lookups.creatures.get(), this);
        chest = new EntryPicker(lookups.chests.get(), this);
        chest->setToolTip("Chests, crates and other objects players loot.");
        chance = new QSpinBox(this);
        chance->setRange(1, 100);
        chance->setSuffix(" %");
        chance->setValue(50);
        chance->setToolTip("How often it drops, for players on the quest only. Game quests mostly use 35-80 %; "
                           "chests usually 100 %.");
        auto* source_row = new QHBoxLayout();
        source_row->addWidget(source);
        source_row->addWidget(creature, 1);
        source_row->addWidget(chest, 1);
        source_row->addWidget(chance);
        fields->addLayout(source_row, 1, 0, 1, 3);
        fields->setColumnStretch(0, 1);

        if (drop)
        {
          bool const from_creature = drop->source == Q::QuestDrop::Source::Creature;
          source->setCurrentIndex(std::max(0, source->findData(static_cast<int>(drop->source))));
          (from_creature ? creature : chest)->setEntry(drop->source_entry);
          chance->setValue(std::clamp(static_cast<int>(std::lround(drop->chance)), 1, 100));
        }
        connect(source, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateSource(); });
        updateSource();
      }

      void updateSource()
      {
        int const kind = source->currentData().toInt();
        creature->setVisible(kind == static_cast<int>(Q::QuestDrop::Source::Creature));
        chest->setVisible(kind == static_cast<int>(Q::QuestDrop::Source::Object));
        chance->setVisible(kind >= 0);
      }

      Q::ItemCount item() const
      {
        return {picker->entry(), static_cast<std::uint32_t>(count->value())};
      }

      std::optional<Q::QuestDrop> drop() const
      {
        int const kind = source->currentData().toInt();
        if (kind < 0 || !picker->entry())
        {
          return std::nullopt;
        }
        auto const from = static_cast<Q::QuestDrop::Source>(kind);
        auto const entry = (from == Q::QuestDrop::Source::Creature ? creature : chest)->entry();
        if (!entry)
        {
          return std::nullopt;
        }
        return Q::QuestDrop{from, entry, picker->entry(), static_cast<float>(chance->value()), 0};
      }

      EntryPicker* picker = nullptr;
      QSpinBox* count = nullptr;
      QComboBox* source = nullptr;
      EntryPicker* creature = nullptr;
      EntryPicker* chest = nullptr;
      QSpinBox* chance = nullptr;
    };

    class ExploreRow : public ObjectiveRow
    {
    public:
      ExploreRow(std::uint32_t trigger, QuestEditorSetup const& setup, QWidget* parent)
        : ObjectiveRow(EXPLORE, "EXPLORE", BadgeColor::explore, parent)
        , _setup(setup)
      {
        places = new QComboBox(this);
        places->setMinimumContentsLength(30);
        places->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        auto* nearest = new QPushButton("Nearest to my cursor", this);
        nearest->setEnabled(static_cast<bool>(setup.cursor_position));
        nearest->setToolTip("Point at the spot in the 3D view, then click: picks the closest free place.");
        info = hintLabel({}, this);
        fields->addWidget(places, 0, 0);
        fields->addWidget(nearest, 0, 1);
        fields->addWidget(info, 1, 0, 1, 2);
        fields->addWidget(hintLabel("Places are the area triggers the game client knows; new ones need a client "
                                    "patch. Places already used by another quest or for teleports and inns are "
                                    "greyed out.", this), 2, 0, 1, 2);
        fields->setColumnStretch(0, 1);
        fill(nullptr);
        select(trigger);
        connect(places, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { describe(); });
        connect(nearest, &QPushButton::clicked, this, [this]
        {
          if (auto const position = _setup.cursor_position ? _setup.cursor_position() : std::nullopt)
          {
            auto const current = value();
            fill(&*position);
            // The first enabled entry is the nearest free place.
            for (int i = 0; i < places->count(); ++i)
            {
              if (static_cast<QStandardItemModel*>(places->model())->item(i)->isEnabled())
              {
                places->setCurrentIndex(i);
                return;
              }
            }
            select(current);
          }
        });
        describe();
      }

      std::uint32_t value() const
      {
        return places->currentData().toUInt();
      }

    private:
      // Lists the places, nearest to `position` first when given (same map only).
      void fill(WorldPosition const* position)
      {
        places->blockSignals(true);
        places->clear();
        std::vector<AreaTriggerEntry const*> triggers;
        for (auto const& trigger : _setup.lookups->area_triggers)
        {
          if (!position || trigger.map == position->map)
          {
            triggers.push_back(&trigger);
          }
        }
        auto const distance = [&](AreaTriggerEntry const* t)
        {
          return position ? std::hypot(t->x - position->x, t->y - position->y, t->z - position->z) : 0.0f;
        };
        std::stable_sort(triggers.begin(), triggers.end(), [&](auto const* a, auto const* b)
        {
          return position ? distance(a) < distance(b) : a->name < b->name;
        });
        places->addItem("Choose a place...", 0u);
        auto* model = static_cast<QStandardItemModel*>(places->model());
        for (auto const* trigger : triggers)
        {
          QString label = QString("%1 (#%2)").arg(QString::fromStdString(trigger->name)).arg(trigger->id);
          if (position)
          {
            label = QString("%1 m -- %2").arg(static_cast<int>(distance(trigger))).arg(label);
          }
          places->addItem(label, trigger->id);
          bool const taken = (trigger->quest && trigger->quest != _setup.entry) || trigger->other_use;
          model->item(places->count() - 1)->setEnabled(!taken);
        }
        places->blockSignals(false);
      }

      void select(std::uint32_t trigger)
      {
        int const index = places->findData(trigger);
        places->setCurrentIndex(index >= 0 ? index : 0);
        describe();
      }

      void describe()
      {
        auto const id = value();
        auto const& triggers = _setup.lookups->area_triggers;
        auto const found = std::find_if(triggers.begin(), triggers.end(), [id](auto const& t) { return t.id == id; });
        info->setText(found == triggers.end() ? QString("Players complete the quest by reaching the place.")
                      : QString("Map %1, radius %2 yards. Players complete the quest by reaching it.")
                          .arg(found->map).arg(found->radius, 0, 'f', 0));
      }

      QuestEditorSetup const& _setup;
      QComboBox* places = nullptr;
      QLabel* info = nullptr;
    };

    class ReputationRow : public ObjectiveRow
    {
    public:
      ReputationRow(Q::Reputation const& rep, ContentLookups& lookups, QWidget* parent)
        : ObjectiveRow(REPUTATION, "REPUTATION", BadgeColor::reputation, parent)
      {
        rank = new QComboBox(this);
        for (auto const& r : Q::reputationRanks())
        {
          rank->addItem(r.label, r.value);
        }
        rank->setCurrentIndex(static_cast<int>(Q::reputationRankOf(rep.faction ? rep.value : 9000)));
        faction = new EntryPicker(lookups.reputation_factions.get(), this);
        faction->setEntry(rep.faction);
        fields->addWidget(new QLabel("Reach", this), 0, 0);
        fields->addWidget(rank, 0, 1);
        fields->addWidget(new QLabel("with", this), 0, 2);
        fields->addWidget(faction, 0, 3);
        fields->setColumnStretch(3, 1);
      }

      Q::Reputation value() const
      {
        return {faction->entry(), rank->currentData().toInt()};
      }

      QComboBox* rank = nullptr;
      EntryPicker* faction = nullptr;
    };
  }

  QuestObjectivesPage::QuestObjectivesPage(QuestEditorSetup const& setup, QWidget* parent)
    : QuestPage(setup, parent)
  {
    auto* card = new Card("Objectives", "Leave the list empty for a quest that only asks players to talk to someone.", this);
    _objectives = new RowList(card);
    card->body()->addWidget(_objectives);
    _add = addButton("+ Add an objective", card);
    _menu = new QMenu(_add);
    _add->setMenu(_menu);
    card->body()->addWidget(_add);
    _summary = hintLabel({}, card);
    card->body()->addWidget(_summary);
    cards()->addWidget(card);
    cards()->addStretch();

    auto const add = [this](QString const& text, QString const& tip, auto&& make)
    {
      auto* action = _menu->addAction(text);
      action->setToolTip(tip);
      connect(action, &QAction::triggered, this, [this, make] { _objectives->addRow(make()); });
      return action;
    };
    add("Kill creatures", "e.g. \"Kill 10 Young Wolves\"", [this] { return new TargetRow({}, lookups(), _objectives); });
    add("Use an object", "e.g. \"Light the brazier\", \"Search the altar\"", [this]
    {
      Q::Target target;
      target.kind = Q::Target::Kind::Object;
      return new TargetRow(target, lookups(), _objectives);
    });
    add("Collect items", "e.g. \"Bring 8 Wolf Pelts\" -- and say where they come from", [this]
    {
      return new CollectRow({}, std::nullopt, _setup, _objectives);
    });
    add("Explore a place", "e.g. \"Find the hidden cave\"", [this] { return new ExploreRow(0, _setup, _objectives); });
    add("Reach a reputation", "e.g. \"Become Honored with Stormwind\"", [this]
    {
      return new ReputationRow({}, lookups(), _objectives);
    });
    connect(_menu, &QMenu::aboutToShow, this, [this] { updateMenu(); });
    _objectives->on_changed = [this] { updateSummary(); changed(); };
    updateSummary();
  }

  int QuestObjectivesPage::countOf(int kind) const
  {
    int count = 0;
    for (auto* row : _objectives->rows())
    {
      count += static_cast<ObjectiveRow*>(row)->kind == kind;
    }
    return count;
  }

  void QuestObjectivesPage::updateMenu()
  {
    auto const& s = schema();
    int const targets = countOf(KILL) + countOf(USE);
    auto const actions = _menu->actions();
    actions[0]->setEnabled(targets < Q::slotCount(s.target_cols));
    actions[1]->setEnabled(targets < Q::slotCount(s.target_cols));
    actions[2]->setEnabled(countOf(COLLECT) < Q::slotCount(s.collect_cols));
    actions[3]->setEnabled(countOf(EXPLORE) == 0 && _setup.data->db.links.area_triggers.valid());
    actions[4]->setEnabled(countOf(REPUTATION) == 0 && !s.rep_objective_faction_col.empty());
  }

  void QuestObjectivesPage::updateSummary()
  {
    _summary->setText(_objectives->count() ? QString("Players need to do all of these.")
                                           : QString("No objectives: players only have to go and talk to the NPC the "
                                                     "quest is handed in to."));
  }

  void QuestObjectivesPage::load(Q::QuestContent const& content)
  {
    _objectives->clear();
    auto const& f = content.fields;
    for (auto const& target : f.targets.value_or(std::vector<Q::Target>{}))
    {
      _objectives->addRow(new TargetRow(target, lookups(), _objectives));
    }
    for (auto const& item : f.collect.value_or(std::vector<Q::ItemCount>{}))
    {
      auto const drop = std::find_if(content.drops.begin(), content.drops.end(), [&](auto const& d) { return d.item == item.id; });
      _objectives->addRow(new CollectRow(item, drop == content.drops.end() ? std::nullopt : std::optional<Q::QuestDrop>(*drop),
                                         _setup, _objectives));
    }
    if (content.links.area_trigger)
    {
      _objectives->addRow(new ExploreRow(content.links.area_trigger, _setup, _objectives));
    }
    if (f.rep_objective && f.rep_objective->faction)
    {
      _objectives->addRow(new ReputationRow(*f.rep_objective, lookups(), _objectives));
    }
    updateSummary();
  }

  void QuestObjectivesPage::store(Q::QuestContent& content) const
  {
    std::vector<Q::Target> targets;
    std::vector<Q::ItemCount> collect;
    content.drops.clear();
    content.links.area_trigger = 0;
    std::optional<Q::Reputation> rep;
    for (auto* widget : _objectives->rows())
    {
      auto* row = static_cast<ObjectiveRow*>(widget);
      switch (row->kind)
      {
        case KILL:
        case USE:
          if (auto const target = static_cast<TargetRow*>(row)->value(); target.id)
          {
            targets.push_back(target);
          }
          break;
        case COLLECT:
        {
          auto* collect_row = static_cast<CollectRow*>(row);
          if (auto const item = collect_row->item(); item.id)
          {
            collect.push_back(item);
            if (auto const drop = collect_row->drop())
            {
              content.drops.push_back(*drop);
            }
          }
          break;
        }
        case EXPLORE:
          content.links.area_trigger = static_cast<ExploreRow*>(row)->value();
          break;
        case REPUTATION:
          rep = static_cast<ReputationRow*>(row)->value();
          break;
      }
    }
    auto const& s = schema();
    auto& f = content.fields;
    if (s.target_cols[0].size()) f.targets = targets;
    if (s.collect_cols[0].size()) f.collect = collect;
    if (!s.rep_objective_faction_col.empty()) f.rep_objective = rep.value_or(Q::Reputation{});
  }

  QStringList QuestObjectivesPage::problems() const
  {
    QStringList problems;
    for (auto* widget : _objectives->rows())
    {
      auto* row = static_cast<ObjectiveRow*>(widget);
      if (row->kind == EXPLORE && !static_cast<ExploreRow*>(row)->value())
      {
        problems << "Choose the place to explore (Objectives).";
      }
      if (row->kind == REPUTATION && !static_cast<ReputationRow*>(row)->value().faction)
      {
        problems << "Choose the faction of the reputation objective (Objectives).";
      }
    }
    return problems;
  }
}
