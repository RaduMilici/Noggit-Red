// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/npc/NpcEditorDialog.hpp>

#include <noggit/ui/content/ContentStyle.hpp>

#include <QtCore/QTimer>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace Noggit::Ui::Npc
{
  using namespace Noggit::Ui::Content;
  namespace N = Noggit::Npc;

  NpcEditorDialog::NpcEditorDialog(NpcEditorSetup const& setup, QWidget* parent)
    : QDialog(parent)
    , _setup(setup)
  {
    auto const& data = *setup.data;
    auto const& schema = data.db.npc;
    auto const& values = data.values;
    bool const vanilla = setup.lookups->vanilla;
    QString const source_name = QString::fromStdString(values.name.value_or(std::string()));

    setWindowTitle(setup.editing ? "Edit NPC" : "New NPC");
    resize(1060, 780);
    applyEditorStyle(this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 12);
    _header = new QLabel(this);
    _header->setObjectName("ContentHeader");
    root->addWidget(_header);
    auto* subheader = new QLabel(setup.editing
      ? QString("Your NPC  ·  ID %1").arg(setup.entry)
      : QString("New NPC  ·  ID %1  ·  a copy of %2 (#%3): it fights, drops loot and casts like the original")
          .arg(setup.entry).arg(source_name).arg(setup.source), this);
    subheader->setObjectName("ContentSubheader");
    root->addWidget(subheader);

    auto* body = new QHBoxLayout();
    body->setSpacing(14);
    root->addLayout(body, 1);

    auto* form_host = new QWidget(this);
    auto* cards = new QVBoxLayout(form_host);
    cards->setContentsMargins(0, 0, 8, 0);
    cards->setSpacing(12);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(form_host);
    body->addWidget(scroll, 3);

    // --- who ---
    auto* who = new Card("Who", {}, form_host);
    auto* who_form = new QFormLayout();
    who->body()->addLayout(who_form);
    _name = new QLineEdit(QString::fromStdString(values.name.value_or(std::string())), who);
    _name->setMaxLength(100);
    who_form->addRow("Name", _name);
    _subname = new QLineEdit(QString::fromStdString(values.subname.value_or(std::string())), who);
    _subname->setPlaceholderText("optional, e.g. Blacksmith -- shown as <Blacksmith>");
    _subname->setEnabled(!schema.subname_col.empty());
    who_form->addRow("Title", _subname);
    int const max_level = vanilla ? 63 : 83;
    auto* levels = new QHBoxLayout();
    _level_min = new QSpinBox(who);
    _level_max = new QSpinBox(who);
    for (auto* spin : {_level_min, _level_max})
    {
      spin->setRange(1, max_level);
      spin->setEnabled(!schema.level_min_col.empty());
    }
    _level_min->setValue(std::clamp<int>(values.level_min.value_or(1), 1, max_level));
    _level_max->setValue(std::clamp<int>(values.level_max.value_or(1), 1, max_level));
    levels->addWidget(_level_min);
    levels->addWidget(new QLabel("to", who));
    levels->addWidget(_level_max);
    _rank = new QComboBox(who);
    for (auto const& option : N::rankOptions())
    {
      _rank->addItem(option.label, option.rank);
    }
    _rank->setCurrentIndex(std::max(0, _rank->findData(values.rank.value_or(0))));
    _rank->setEnabled(!schema.rank_col.empty());
    _rank->setToolTip("Elite and boss NPCs get the dragon portrait frame.");
    levels->addSpacing(12);
    levels->addWidget(new QLabel("Rank", who));
    levels->addWidget(_rank);
    levels->addStretch();
    who_form->addRow("Level", levels);
    connect(_level_min, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) { if (_level_max->value() < v) _level_max->setValue(v); });
    connect(_level_max, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) { if (_level_min->value() > v) _level_min->setValue(v); });
    cards->addWidget(who);

    // --- looks ---
    auto* looks = new Card("Looks", {}, form_host);
    auto* looks_form = new QFormLayout();
    looks->body()->addLayout(looks_form);
    auto* model_row = new QHBoxLayout();
    _display = new QSpinBox(looks);
    _display->setRange(0, 16777215);
    _display->setValue(static_cast<int>(values.display_id.value_or(0)));
    _display->setEnabled(!schema.display_col.empty());
    _display->setToolTip(schema.display_col.empty()
      ? QString("This database keeps models in a separate table; the copy keeps the original model.")
      : QString("The creature model (display ID); the preview follows. Find a creature that looks right in the NPC "
                "list -- its model ID is shown there."));
    _display_info = hintLabel({}, looks);
    model_row->addWidget(_display);
    model_row->addWidget(_display_info, 1);
    looks_form->addRow("Model", model_row);
    _scale = new QDoubleSpinBox(looks);
    _scale->setRange(0.0, 10.0);
    _scale->setSingleStep(0.1);
    _scale->setDecimals(2);
    _scale->setSpecialValueText("Normal");
    _scale->setValue(values.scale.value_or(0.0f));
    _scale->setEnabled(!schema.scale_col.empty());
    looks_form->addRow("Size", _scale);
    connect(_display, qOverload<int>(&QSpinBox::valueChanged), this, [this] { lookChanged(); });
    connect(_scale, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { lookChanged(); });
    cards->addWidget(looks);

    // --- behaviour ---
    auto* behaviour = new Card("Behaviour", {}, form_host);
    auto* behaviour_form = new QFormLayout();
    behaviour->body()->addLayout(behaviour_form);
    _faction = new EntryPicker(setup.lookups->faction_templates.get(), behaviour);
    _faction->setEntry(values.faction.value_or(0));
    _faction->setEnabled(!schema.faction_cols.empty());
    _faction->setToolTip("Who the NPC belongs to: decides whether players can talk to it or must fight it. "
                         "Type to search, e.g. \"Stormwind\" or \"hostile\".");
    behaviour_form->addRow("Faction", _faction);
    behaviour->body()->addWidget(hintLabel("What can players do with this NPC?", behaviour));
    auto* grid = new QGridLayout();
    std::uint32_t mapped = 0;
    int index = 0;
    for (auto const& role : N::npcRoles(vanilla))
    {
      auto* check = new QCheckBox(role.label, behaviour);
      check->setToolTip(role.tooltip);
      check->setChecked(values.npc_flags.value_or(0) & role.flag);
      check->setEnabled(!schema.npc_flags_col.empty());
      grid->addWidget(check, index / 3, index % 3);
      _roles[role.flag] = check;
      mapped |= role.flag;
      ++index;
    }
    _unmapped_npc_flags = values.npc_flags.value_or(0) & ~mapped;
    behaviour->body()->addLayout(grid);
    cards->addWidget(behaviour);

    // --- dialogue ---
    auto const& dialogue_schema = data.db.dialogue;
    auto* dialogue = new Card("Dialogue", "In these texts $N is the player's name, $C their class and $B a new line.", form_host);
    auto* dialogue_form = new QFormLayout();
    dialogue->body()->addLayout(dialogue_form);
    _greeting = new QPlainTextEdit(QString::fromStdString(data.dialogue.dialogue.greeting), dialogue);
    _greeting->setPlaceholderText("What the NPC says when a player clicks it (optional).");
    _greeting->setTabChangesFocus(true);
    _greeting->setMaximumHeight(_greeting->fontMetrics().lineSpacing() * 4 + 14);
    _greeting->setEnabled(dialogue_schema.greetingSupported() && !schema.gossip_menu_col.empty());
    dialogue_form->addRow("Greeting", _greeting);
    _quest_greeting = new QPlainTextEdit(QString::fromStdString(data.dialogue.dialogue.quest_greeting), dialogue);
    _quest_greeting->setPlaceholderText("Shown above the NPC's list of quests (optional).");
    _quest_greeting->setTabChangesFocus(true);
    _quest_greeting->setMaximumHeight(_quest_greeting->fontMetrics().lineSpacing() * 3 + 14);
    _quest_greeting->setEnabled(dialogue_schema.questGreetingSupported());
    dialogue_form->addRow("Before quests", _quest_greeting);
    if (data.dialogue.menu_shared && !data.dialogue.dialogue.greeting.empty())
    {
      dialogue->body()->addWidget(hintLabel("Other NPCs say the same greeting. Changing it gives this NPC its own; "
                                            "the others keep theirs.", dialogue));
    }
    connect(_greeting, &QPlainTextEdit::textChanged, this, [this]
    {
      // A greeting needs the chat window: tick "Can talk" for the user.
      if (!_greeting->toPlainText().trimmed().isEmpty() && _roles.count(0x1) && !_roles.at(0x1)->isChecked())
      {
        _roles.at(0x1)->setChecked(true);
      }
    });
    cards->addWidget(dialogue);

    // --- copy options ---
    if (!setup.editing)
    {
      Card* copy = nullptr;
      auto const ensure = [&]
      {
        if (!copy)
        {
          copy = new Card("Also copy", {}, form_host);
          cards->addWidget(copy);
        }
        return copy;
      };
      for (auto const& related : schema.related)
      {
        auto const rows = data.related_rows.count(related.table) ? data.related_rows.at(related.table) : 0;
        if (!related.optional || !rows)
        {
          continue;
        }
        auto* check = new QCheckBox(QString("The %1 %2 of %3").arg(rows).arg(QString::fromStdString(related.label))
                                                              .arg(source_name), ensure());
        check->setChecked(true);
        copy->body()->addWidget(check);
        _copy_related[related.table] = check;
      }
      if (!data.script_name.empty() && !schema.script_col.empty())
      {
        _keep_script = new QCheckBox("Keep the original's special scripted behaviour (advanced)", ensure());
        _keep_script->setToolTip(QString("The original runs the server script \"%1\" (boss fights, events, ...). Copies "
                                         "normally should not: the script may expect the original NPC.")
                                   .arg(QString::fromStdString(data.script_name)));
        copy->body()->addWidget(_keep_script);
      }
    }
    cards->addStretch();

    if (setup.preview)
    {
      auto* preview = new Card("Preview", "Drag to turn it; scroll to zoom.", this);
      setup.preview->setParent(preview);
      setup.preview->setMinimumSize(300, 360);
      preview->body()->addWidget(setup.preview, 1);
      body->addWidget(preview, 2);
    }

    auto* footer = new QHBoxLayout();
    _problem = problemLabel(this);
    footer->addWidget(_problem, 1);
    auto* buttons = new QDialogButtonBox(this);
    _ok = buttons->addButton(setup.editing ? "Save changes" : "Create NPC", QDialogButtonBox::AcceptRole);
    _ok->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    footer->addWidget(buttons);
    root->addLayout(footer);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(_name, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(_faction, &QComboBox::currentTextChanged, this, [this] { refresh(); });

    if (!setup.editing)
    {
      _name->selectAll();
    }
    _name->setFocus();
    _display_info->setText(_setup.model_name ? _setup.model_name(static_cast<std::uint32_t>(_display->value())) : QString());
    refresh();
  }

  void NpcEditorDialog::showEvent(QShowEvent* event)
  {
    QDialog::showEvent(event);
    if (!_shown_once)
    {
      // The preview gets its first look once the dialog is on screen: before that its GL widget has no
      // context to load a model into.
      _shown_once = true;
      QTimer::singleShot(0, this, [this] { lookChanged(); });
    }
  }

  void NpcEditorDialog::lookChanged()
  {
    auto const display = static_cast<std::uint32_t>(_display->value());
    QString const name = _setup.model_name ? _setup.model_name(display) : QString();
    _display_info->setText(name.isEmpty() ? QString("unknown model") : name);
    if (_setup.show_look)
    {
      _setup.show_look(display, static_cast<float>(_scale->value()));
    }
    refresh();
  }

  void NpcEditorDialog::refresh()
  {
    QString const name = _name->text().trimmed();
    _header->setText(name.isEmpty() ? QString("Unnamed NPC") : name);
    QString problem;
    auto const display = static_cast<std::uint32_t>(_display->value());
    if (name.isEmpty())
    {
      problem = "The NPC needs a name.";
    }
    else if (_faction->isEnabled() && !_faction->entry())
    {
      problem = "Pick a faction from the list.";
    }
    else if (_display->isEnabled() && display != _setup.data->values.display_id.value_or(0) && _setup.model_name
             && _setup.model_name(display).isEmpty())
    {
      problem = "That model ID does not exist in this client.";
    }
    _problem->setText(problem);
    _problem->setVisible(!problem.isEmpty());
    _ok->setEnabled(problem.isEmpty());
  }

  std::uint32_t NpcEditorDialog::selectedNpcFlags() const
  {
    std::uint32_t flags = _unmapped_npc_flags;
    for (auto const& [flag, check] : _roles)
    {
      flags |= check->isChecked() ? flag : 0u;
    }
    return flags;
  }

  N::NpcContent NpcEditorDialog::content() const
  {
    auto const& values = _setup.data->values;
    N::NpcContent content;
    auto& fields = content.fields;
    fields.name = _name->text().trimmed().toStdString();
    if (_subname->isEnabled()) fields.subname = _subname->text().trimmed().toStdString();
    if (_level_min->isEnabled())
    {
      fields.level_min = static_cast<std::uint32_t>(_level_min->value());
      fields.level_max = static_cast<std::uint32_t>(_level_max->value());
    }
    if (_rank->isEnabled()) fields.rank = _rank->currentData().toUInt();
    if (_faction->isEnabled()) fields.faction = _faction->entry();
    if (!_roles.empty() && _roles.begin()->second->isEnabled()) fields.npc_flags = selectedNpcFlags();
    // Model and size only when changed: writing the model resets the source's random model variants.
    auto const display = static_cast<std::uint32_t>(_display->value());
    if (_display->isEnabled() && display != values.display_id.value_or(0)) fields.display_id = display;
    if (_scale->isEnabled() && std::abs(_scale->value() - values.scale.value_or(0.0f)) > 0.001)
    {
      fields.scale = static_cast<float>(_scale->value());
    }
    fields.clear_script = !_setup.editing && !_setup.data->script_name.empty() && !(_keep_script && _keep_script->isChecked());
    for (auto const& [table, check] : _copy_related)
    {
      if (check->isChecked())
      {
        content.copy_related.insert(table);
      }
    }
    content.dialogue.greeting = _greeting->isEnabled() ? _greeting->toPlainText().trimmed().toStdString()
                                                       : _setup.data->dialogue.dialogue.greeting;
    content.dialogue.quest_greeting = _quest_greeting->isEnabled() ? _quest_greeting->toPlainText().trimmed().toStdString()
                                                                   : _setup.data->dialogue.dialogue.quest_greeting;
    return content;
  }
}
