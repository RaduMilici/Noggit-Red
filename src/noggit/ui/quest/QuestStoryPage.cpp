// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/quest/QuestStoryPage.hpp>

#include <noggit/quest/QuestCatalog.hpp>
#include <noggit/ui/content/ContentStyle.hpp>

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QSpinBox>

#include <algorithm>

namespace Noggit::Ui::Quest
{
  using namespace Noggit::Ui::Content;
  namespace Q = Noggit::Quest;

  namespace
  {
    QPlainTextEdit* textBox(QString const& placeholder, int lines, QWidget* parent)
    {
      auto* edit = new QPlainTextEdit(parent);
      edit->setPlaceholderText(placeholder);
      edit->setTabChangesFocus(true);
      edit->setMinimumHeight(edit->fontMetrics().lineSpacing() * lines + 14);
      return edit;
    }

    QWidget* row(QWidget* parent, std::initializer_list<QWidget*> widgets)
    {
      auto* container = new QWidget(parent);
      auto* layout = new QHBoxLayout(container);
      layout->setContentsMargins(0, 0, 0, 0);
      for (auto* widget : widgets)
      {
        layout->addWidget(widget);
      }
      layout->addStretch();
      return container;
    }
  }

  QuestStoryPage::QuestStoryPage(QuestEditorSetup const& setup, QWidget* parent)
    : QuestPage(setup, parent)
  {
    auto const& s = schema();
    int const max_level = lookups().vanilla ? 63 : 83;

    auto* quest = new Card("Quest", {}, this);
    auto* form = new QFormLayout();
    quest->body()->addLayout(form);

    _title = new QLineEdit(quest);
    _title->setPlaceholderText("e.g. Wolves at the Door");
    form->addRow("Title", _title);

    _level = new QSpinBox(quest);
    _level->setRange(1, max_level);
    _level->setToolTip("How hard the quest is: its colour in the quest log and the experience it gives.");
    _level->setEnabled(!s.level_col.empty());
    _min_level = new QSpinBox(quest);
    _min_level->setRange(1, max_level);
    _min_level->setToolTip("Players below this level do not see the quest.");
    _min_level->setEnabled(!s.min_level_col.empty());
    form->addRow("Level", row(quest, {_level, new QLabel("available from level", quest), _min_level}));

    _zone = new EntryPicker(lookups().zones.get(), quest);
    _zone->setToolTip("The heading the quest is listed under in the quest log.");
    _zone->setEnabled(!s.zone_col.empty());
    form->addRow("Zone", _zone);

    _type = new QComboBox(quest);
    for (auto const& type : Q::questTypes())
    {
      _type->addItem(type.label, type.id);
    }
    _type->setEnabled(!s.type_col.empty());
    _group_size = new QSpinBox(quest);
    _group_size->setRange(2, 40);
    _group_size->setSuffix(" players");
    _group_size->setEnabled(!s.suggested_players_col.empty());
    _group_row = row(quest, {new QLabel("suggested group:", quest), _group_size});
    form->addRow("Kind", row(quest, {_type, _group_row}));

    _timed = new QCheckBox("Must be finished within", quest);
    _minutes = new QSpinBox(quest);
    _minutes->setRange(1, 600);
    _minutes->setSuffix(" minutes");
    _timed->setEnabled(!s.time_limit_col.empty());
    form->addRow("Time limit", row(quest, {_timed, _minutes}));

    _repeatable = new QCheckBox("Can be done again after finishing it", quest);
    _repeatable->setEnabled(!s.special_flags_col.empty());
    _sharable = new QCheckBox("Can be shared with the group", quest);
    _sharable->setEnabled(!s.quest_flags_col.empty());
    form->addRow("", _repeatable);
    form->addRow("", _sharable);
    cards()->addWidget(quest);

    auto* texts = new Card("What the NPC says",
                           "In these texts $N is the player's name, $C their class, $R their race and $B a new line.", this);
    auto* text_form = new QFormLayout();
    texts->body()->addLayout(text_form);
    _details = textBox("The story, told when the quest is offered.", 5, texts);
    _details->setEnabled(!s.details_col.empty());
    text_form->addRow("Story", _details);
    _objectives = textBox("One or two lines for the quest log, e.g. \"Kill 8 Young Wolves and return to Marshal McBride.\"", 2, texts);
    _objectives->setEnabled(!s.objectives_col.empty());
    text_form->addRow("Summary", _objectives);
    _request_items = textBox("Said when the player comes back before finishing.", 2, texts);
    _request_items->setEnabled(!s.request_items_col.empty());
    text_form->addRow("Not done yet", _request_items);
    _offer_reward = textBox("Said when the player hands the quest in.", 3, texts);
    _offer_reward->setEnabled(!s.offer_reward_col.empty());
    text_form->addRow("Completed", _offer_reward);
    cards()->addWidget(texts);
    cards()->addStretch();

    connect(_title, &QLineEdit::textChanged, this, [this] { changed(); });
    connect(_level, qOverload<int>(&QSpinBox::valueChanged), this, [this](int level)
    {
      if (_min_level->value() > level)
      {
        _min_level->setValue(level);
      }
      changed();
    });
    connect(_type, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateVisibility(); });
    connect(_timed, &QCheckBox::toggled, this, [this] { updateVisibility(); });
    updateVisibility();
  }

  void QuestStoryPage::updateVisibility()
  {
    _group_row->setVisible(_type->currentData().toUInt() != 0);
    _minutes->setEnabled(_timed->isEnabled() && _timed->isChecked());
  }

  void QuestStoryPage::load(Q::QuestContent const& content)
  {
    auto const& f = content.fields;
    int const max_level = _level->maximum();
    _title->setText(QString::fromStdString(f.title.value_or(std::string())));
    _level->setValue(std::clamp<int>(f.level.value_or(1), 1, max_level));
    _min_level->setValue(std::clamp<int>(f.min_level.value_or(1), 1, max_level));
    auto const zone = f.zone.value_or(0);
    _zone->setEntry(zone > 0 ? static_cast<std::uint32_t>(zone) : 0);
    int const type = _type->findData(f.type.value_or(0));
    if (type < 0)
    {
      _type->addItem(QString("Type %1 (kept)").arg(f.type.value_or(0)), f.type.value_or(0));
    }
    _type->setCurrentIndex(type >= 0 ? type : _type->count() - 1);
    _group_size->setValue(std::max<int>(f.suggested_players.value_or(0), 2));
    _timed->setChecked(f.time_limit.value_or(0) > 0);
    _minutes->setValue(f.time_limit.value_or(0) ? std::max<int>((*f.time_limit + 59) / 60, 1) : 30);
    _repeatable->setChecked(f.special_flags.value_or(0) & Q::SPECIAL_REPEATABLE);
    // New quests can be shared, like nearly every game quest.
    std::uint32_t const flags = f.quest_flags.value_or(_setup.mode == EditorMode::Create ? Q::FLAG_SHARABLE : 0);
    _sharable->setChecked(flags & Q::FLAG_SHARABLE);
    _other_quest_flags = flags & ~Q::FLAG_SHARABLE;
    _details->setPlainText(QString::fromStdString(f.details.value_or(std::string())));
    _objectives->setPlainText(QString::fromStdString(f.objectives.value_or(std::string())));
    _request_items->setPlainText(QString::fromStdString(f.request_items.value_or(std::string())));
    _offer_reward->setPlainText(QString::fromStdString(f.offer_reward.value_or(std::string())));
    updateVisibility();
  }

  void QuestStoryPage::store(Q::QuestContent& content) const
  {
    auto& f = content.fields;
    auto const text = [](QPlainTextEdit const* edit) -> std::optional<std::string>
    {
      return edit->isEnabled() ? std::optional<std::string>(edit->toPlainText().trimmed().toStdString()) : std::nullopt;
    };
    f.title = questTitle().toStdString();
    if (_level->isEnabled()) f.level = static_cast<std::uint32_t>(_level->value());
    if (_min_level->isEnabled()) f.min_level = static_cast<std::uint32_t>(_min_level->value());
    if (_zone->isEnabled())
    {
      // A negative zone (class / profession heading) the picker cannot show stays as it is.
      if (!(f.zone.value_or(0) < 0 && _zone->entry() == 0))
      {
        f.zone = static_cast<std::int32_t>(_zone->entry());
      }
    }
    if (_type->isEnabled()) f.type = _type->currentData().toUInt();
    if (_group_size->isEnabled()) f.suggested_players = _type->currentData().toUInt() ? static_cast<std::uint32_t>(_group_size->value()) : 0u;
    if (_timed->isEnabled()) f.time_limit = _timed->isChecked() ? static_cast<std::uint32_t>(_minutes->value() * 60) : 0u;
    if (_repeatable->isEnabled())
    {
      // Only the repeatable bit is the page's; the save plan manages the exploration / event bit.
      f.special_flags = (f.special_flags.value_or(0) & ~Q::SPECIAL_REPEATABLE)
                      | (_repeatable->isChecked() ? Q::SPECIAL_REPEATABLE : 0u);
    }
    if (_sharable->isEnabled()) f.quest_flags = _other_quest_flags | (_sharable->isChecked() ? Q::FLAG_SHARABLE : 0u);
    f.details = text(_details);
    f.objectives = text(_objectives);
    f.request_items = text(_request_items);
    f.offer_reward = text(_offer_reward);
  }

  QStringList QuestStoryPage::problems() const
  {
    return questTitle().isEmpty() ? QStringList{"The quest needs a title (Story)."} : QStringList{};
  }

  QString QuestStoryPage::questTitle() const
  {
    return _title->text().trimmed();
  }

  int QuestStoryPage::questLevel() const
  {
    return _level->value();
  }
}
