// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/quest/QuestEventsPage.hpp>

#include <noggit/quest/QuestCatalog.hpp>
#include <noggit/ui/content/ContentStyle.hpp>

#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

namespace Noggit::Ui::Quest
{
  using namespace Noggit::Ui::Content;
  namespace Q = Noggit::Quest;
  using Kind = Q::ScriptAction::Kind;

  namespace
  {
    struct KindInfo
    {
      Kind kind;
      char const* menu;
      char const* badge;
      QColor color;
    };

    std::vector<KindInfo> const& kinds()
    {
      static std::vector<KindInfo> const list = {
        {Kind::Say, "The NPC says something", "SAY", BadgeColor::event},
        {Kind::Yell, "The NPC yells something", "YELL", BadgeColor::event},
        {Kind::Emote, "The NPC does an emote (bow, wave, ...)", "EMOTE", BadgeColor::event},
        {Kind::CastSpell, "The NPC casts a spell on the player", "SPELL", BadgeColor::use},
        {Kind::GiveItem, "The player gets an item", "ITEM", BadgeColor::collect},
        {Kind::SummonCreature, "A creature appears", "SPAWN", BadgeColor::explore},
        {Kind::AttackPlayer, "The NPC attacks the player", "ATTACK", BadgeColor::kill},
        {Kind::CompleteQuest, "The quest is completed (\"listen to the story\")", "COMPLETE", BadgeColor::reputation},
      };
      return list;
    }

    KindInfo const& infoOf(Kind kind)
    {
      for (auto const& info : kinds())
      {
        if (info.kind == kind)
        {
          return info;
        }
      }
      return kinds().front();
    }

    class ActionRow : public QWidget
    {
    public:
      ActionRow(Q::ScriptAction const& action, QuestEditorSetup const& setup, QWidget* parent)
        : QWidget(parent), _action(action)
      {
        auto& lookups = *setup.lookups;
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        _wait = new QSpinBox(this);
        _wait->setRange(0, 3600);
        _wait->setPrefix("after ");
        _wait->setSuffix(" s");
        _wait->setValue(static_cast<int>(action.wait));
        _wait->setToolTip("Seconds after the previous event.");
        layout->addWidget(_wait, 0, Qt::AlignTop);
        auto const& info = infoOf(action.kind);
        layout->addWidget(badge(info.badge, info.color, this), 0, Qt::AlignTop);

        switch (action.kind)
        {
          case Kind::Say:
          case Kind::Yell:
            _text = new QLineEdit(QString::fromStdString(action.text), this);
            _text->setPlaceholderText(action.kind == Kind::Say ? "What the NPC says ($N = the player's name)"
                                                               : "What the NPC yells");
            layout->addWidget(_text, 1);
            break;
          case Kind::Emote:
            _choice = new QComboBox(this);
            for (auto const& emote : Q::emotes())
            {
              _choice->addItem(emote.label, emote.id);
            }
            if (_choice->findData(action.id) < 0 && action.id)
            {
              _choice->addItem(QString("Emote #%1").arg(action.id), action.id);
            }
            _choice->setCurrentIndex(std::max(0, _choice->findData(action.id)));
            layout->addWidget(_choice, 1);
            break;
          case Kind::CastSpell:
            _picker = new EntryPicker(lookups.spells.get(), this);
            _picker->setEntry(action.id);
            layout->addWidget(_picker, 1);
            break;
          case Kind::GiveItem:
            _picker = new EntryPicker(lookups.items.get(), this);
            _picker->setEntry(action.id);
            _count = new QSpinBox(this);
            _count->setRange(1, 255);
            _count->setPrefix("x ");
            _count->setValue(std::max<int>(action.count, 1));
            layout->addWidget(_picker, 1);
            layout->addWidget(_count);
            break;
          case Kind::SummonCreature:
          {
            auto* column = new QVBoxLayout();
            auto* line = new QHBoxLayout();
            _picker = new EntryPicker(lookups.creatures.get(), this);
            _picker->setEntry(action.id);
            _count = new QSpinBox(this);
            _count->setRange(5, 3600);
            _count->setPrefix("for ");
            _count->setSuffix(" s");
            _count->setValue(action.count ? static_cast<int>(action.count) : 120);
            line->addWidget(_picker, 1);
            line->addWidget(_count);
            column->addLayout(line);
            auto* place = new QHBoxLayout();
            _position = hintLabel({}, this);
            auto* here = new QPushButton("Use my cursor position", this);
            here->setEnabled(static_cast<bool>(setup.cursor_position));
            here->setToolTip("Point at the spot in the 3D view, then click.");
            place->addWidget(_position, 1);
            place->addWidget(here);
            column->addLayout(place);
            layout->addLayout(column, 1);
            auto const cursor = setup.cursor_position;
            connect(here, &QPushButton::clicked, this, [this, cursor]
            {
              if (auto const position = cursor())
              {
                _action.x = position->x;
                _action.y = position->y;
                _action.z = position->z;
                _action.o = position->orientation;
                describePosition();
              }
            });
            describePosition();
            break;
          }
          case Kind::AttackPlayer:
            layout->addWidget(hintLabel("The NPC turns hostile and attacks the player (it is friendly again once it "
                                        "respawns).", this), 1);
            break;
          case Kind::CompleteQuest:
            layout->addWidget(hintLabel("Completes the quest's objectives -- for quests where players just listen or "
                                        "watch. Players then hand it in.", this), 1);
            break;
        }
      }

      Q::ScriptAction value() const
      {
        Q::ScriptAction action = _action;
        action.wait = static_cast<std::uint32_t>(_wait->value());
        if (_text) action.text = _text->text().trimmed().toStdString();
        if (_choice) action.id = _choice->currentData().toUInt();
        if (_picker) action.id = _picker->entry();
        if (_count) action.count = static_cast<std::uint32_t>(_count->value());
        return action;
      }

      QString problem() const
      {
        auto const action = value();
        if ((action.kind == Kind::Say || action.kind == Kind::Yell) && action.text.empty())
        {
          return "An event has no text to say (Events).";
        }
        if ((action.kind == Kind::CastSpell || action.kind == Kind::GiveItem || action.kind == Kind::SummonCreature)
            && !action.id)
        {
          return "An event is missing its spell, item or creature (Events).";
        }
        if (action.kind == Kind::SummonCreature && action.x == 0.0f && action.y == 0.0f && action.z == 0.0f)
        {
          return "Choose where the creature appears (Events).";
        }
        return {};
      }

    private:
      void describePosition()
      {
        bool const set = _action.x != 0.0f || _action.y != 0.0f || _action.z != 0.0f;
        _position->setText(set ? QString("At %1, %2, %3").arg(_action.x, 0, 'f', 1).arg(_action.y, 0, 'f', 1).arg(_action.z, 0, 'f', 1)
                               : QString("No position yet."));
      }

      Q::ScriptAction _action;
      QSpinBox* _wait = nullptr;
      QLineEdit* _text = nullptr;
      QComboBox* _choice = nullptr;
      EntryPicker* _picker = nullptr;
      QSpinBox* _count = nullptr;
      QLabel* _position = nullptr;
    };
  }

  QuestEventsPage::QuestEventsPage(QuestEditorSetup const& setup, QWidget* parent)
    : QuestPage(setup, parent)
  {
    if (!_setup.data->db.scripts.valid())
    {
      auto* card = new Card("Events", {}, this);
      card->body()->addWidget(hintLabel("This world database scripts events differently from vmangos; the editor "
                                        "cannot write them here.", card));
      cards()->addWidget(card);
      cards()->addStretch();
      return;
    }
    _incomplete = hintLabel("This quest's stored events have steps the editor cannot show (movement, map events, "
                            "...). Changing the events here replaces all of them.", this);
    _incomplete->setStyleSheet("color: #e0a040;");
    _incomplete->hide();
    cards()->addWidget(_incomplete);
    _on_accept = makeTimeline("When the quest is accepted", "Played right after the player accepts.", true);
    _on_complete = makeTimeline("When the quest is handed in", "Played right after the player hands it in.", false);
    cards()->addStretch();
  }

  RowList* QuestEventsPage::makeTimeline(QString const& title, QString const& description, bool on_accept)
  {
    auto* card = new Card(title, description, this);
    auto* list = new RowList(card);
    card->body()->addWidget(list);
    auto* add = addButton("+ Add an event", card);
    auto* menu = new QMenu(add);
    for (auto const& info : kinds())
    {
      auto const kind = info.kind;
      if (kind == Kind::CompleteQuest && !on_accept)
      {
        continue; // completing a quest that was just handed in means nothing
      }
      connect(menu->addAction(info.menu), &QAction::triggered, this, [this, list, kind]
      {
        Q::ScriptAction action;
        action.kind = kind;
        action.wait = list->count() ? 2 : 0;
        addAction(list, action);
      });
    }
    add->setMenu(menu);
    card->body()->addWidget(add);
    list->on_changed = [this] { changed(); };
    cards()->addWidget(card);
    return list;
  }

  void QuestEventsPage::addAction(RowList* list, Q::ScriptAction const& action)
  {
    list->addRow(new ActionRow(action, _setup, list));
  }

  std::vector<Q::ScriptAction> QuestEventsPage::actionsOf(RowList const* list) const
  {
    std::vector<Q::ScriptAction> actions;
    for (auto* row : list->rows())
    {
      actions.push_back(static_cast<ActionRow*>(row)->value());
    }
    return actions;
  }

  void QuestEventsPage::load(Q::QuestContent const& content)
  {
    if (!_on_accept)
    {
      return;
    }
    _on_accept->clear();
    _on_complete->clear();
    for (auto const& action : content.on_accept)
    {
      addAction(_on_accept, action);
    }
    for (auto const& action : content.on_complete)
    {
      addAction(_on_complete, action);
    }
    _incomplete->setVisible(!content.scripts_complete);
  }

  void QuestEventsPage::store(Q::QuestContent& content) const
  {
    if (!_on_accept)
    {
      return;
    }
    content.on_accept = actionsOf(_on_accept);
    content.on_complete = actionsOf(_on_complete);
  }

  QStringList QuestEventsPage::problems() const
  {
    QStringList problems;
    for (auto const* list : {_on_accept, _on_complete})
    {
      if (!list)
      {
        continue;
      }
      for (auto* row : list->rows())
      {
        if (auto const problem = static_cast<ActionRow*>(row)->problem(); !problem.isEmpty() && !problems.contains(problem))
        {
          problems << problem;
        }
      }
    }
    return problems;
  }
}
