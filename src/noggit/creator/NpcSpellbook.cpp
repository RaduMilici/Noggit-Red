#include "NpcSpellbook.hpp"
#include "ContentEditors.hpp"
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/content/ClientData.hpp>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
namespace Noggit::Creator {
namespace {
using Icon = Ui::FontAwesome::Icons;
QPushButton* button(QString const& text, Icon icon, QWidget* parent) { return new QPushButton(Ui::FontAwesomeIcon(icon), text, parent); }
QString targetName(int target) {
  switch (target) {
    case CombatSpell::Self: return "Itself";
    case CombatSpell::Victim: return "Its current target";
    case CombatSpell::SecondThreat: return "Second on its threat list";
    case CombatSpell::LastThreat: return "Last on its threat list";
    case CombatSpell::RandomEnemy: return "A random enemy";
    case CombatSpell::RandomNotTank: return "A random enemy, not its target";
    default: return QString("Target type %1").arg(target);
  }
}
QString range(int a, int b) { return a == b ? QString::number(a) : QString("%1–%2").arg(a).arg(b); }
QString summary(CombatSpell const& s) {
  return QString("%1% · every %2 s · %3").arg(s.chance).arg(range(s.repeatMin, s.repeatMax)).arg(targetName(s.target));
}
// A spell's usual target: helpful spells on itself, the rest on its target.
int defaultTarget(SpellDesign const* d) {
  if (!d) return CombatSpell::Victim;
  for (auto const& e : d->effects) {
    if (!e.type) continue;
    // Heal (10), energize (30), or a positive self aura target (1: caster).
    if (e.type == 10 || e.type == 30 || e.targetA == 1) return CombatSpell::Self;
    return CombatSpell::Victim;
  }
  return CombatSpell::Victim;
}
}
NpcSpellbook::NpcSpellbook(QVector<CombatSpell> spells, QWidget* parent) : QWidget(parent), _spells(std::move(spells)) {
  if (_spells.size() > Npc::maxCombatSpells) _spells.resize(Npc::maxCombatSpells);
  auto layout = new QHBoxLayout(this);

  // The spellbook: two pages of four slots, like the game's.
  auto bookColumn = new QVBoxLayout; layout->addLayout(bookColumn, 3);
  auto title = new QLabel("<b>SPELLBOOK</b> — what it casts in combat, in this order of priority");
  bookColumn->addWidget(title);
  auto book = new QWidget(this); book->setObjectName("NpcSpellbookPages");
  book->setStyleSheet("#NpcSpellbookPages { background: #2b2418; border: 2px solid #6b5a36; border-radius: 6px; }"
                      "#NpcSpellbookPages QToolButton { background: #3a3122; color: #f2e6c8; border: 1px solid #5c4d2e; border-radius: 4px; padding: 4px; text-align: left; }"
                      "#NpcSpellbookPages QToolButton:checked { border: 2px solid #ffd100; background: #4a3e28; }"
                      "#NpcSpellbookPages QToolButton:hover { background: #4a3e28; }");
  auto grid = new QGridLayout(book); grid->setSpacing(8); grid->setContentsMargins(10, 10, 10, 10);
  for (int i = 0; i < Npc::maxCombatSpells; ++i) {
    auto slot = new QToolButton(book);
    slot->setCheckable(true); slot->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    slot->setIconSize(QSize(36, 36)); slot->setMinimumSize(220, 52); slot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    grid->addWidget(slot, i % 4, i / 4); // left page first, then the right one
    _slots[i] = slot;
    connect(slot, &QToolButton::clicked, this, [this, i] {
      if (i < _spells.size()) { select(i); return; }
      // An empty slot: choose a spell for it.
      if (auto spell = pickSpell(this, "Add a combat spell")) assign(int(_spells.size()), *spell);
      else refreshSlots();
    });
  }
  bookColumn->addWidget(book);
  auto addRow = new QHBoxLayout; bookColumn->addLayout(addRow);
  auto add = button("Add Spell…", Icon::plus, this);
  add->setToolTip("Choose any spell in your local world: the game's or one you made.");
  auto create = button("Create Custom Spell…", Icon::magic, this);
  create->setToolTip("Make a new spell in the Spell Editor (clone one or start from an effect template) and add it.");
  addRow->addWidget(add); addRow->addWidget(create); addRow->addStretch();
  _status = new QLabel(this); _status->setWordWrap(true); _status->setStyleSheet("color: gray;");
  bookColumn->addWidget(_status);
  auto help = new QLabel("The server's default AI casts these in combat when their timer is ready, rolling the chance each time. "
                         "Spells you create reach your test client with Test Locally.", this);
  help->setWordWrap(true); help->setStyleSheet("color: gray; font-style: italic;");
  bookColumn->addWidget(help);
  bookColumn->addStretch();
  connect(add, &QPushButton::clicked, this, [this] {
    if (_spells.size() >= Npc::maxCombatSpells) return;
    if (auto spell = pickSpell(this, "Add a combat spell")) assign(int(_spells.size()), *spell);
  });
  connect(create, &QPushButton::clicked, this, [this] {
    if (_spells.size() >= Npc::maxCombatSpells) return;
    if (auto spell = createSpell(this)) { _info.remove(*spell); assign(int(_spells.size()), *spell); }
  });

  // The selected slot.
  _details = new QGroupBox("Selected spell", this); layout->addWidget(_details, 2);
  auto details = new QVBoxLayout(_details);
  _tooltip = new QLabel(_details); _tooltip->setWordWrap(true); _tooltip->setTextFormat(Qt::RichText); _tooltip->setAlignment(Qt::AlignTop);
  _tooltip->setStyleSheet("background: rgba(10,14,30,0.85); color: #f0f0f0; border: 1px solid #555; border-radius: 4px; padding: 8px;");
  _tooltip->setMinimumHeight(110);
  details->addWidget(_tooltip);
  auto actions = new QHBoxLayout; details->addLayout(actions);
  _change = button("Change…", Icon::exchangealt, _details); _change->setToolTip("Put another spell in this slot, keeping its timing.");
  _open = button("Spell Editor…", Icon::edit, _details); _open->setToolTip("Open the spell. Game spells open read-only, with Clone: the clone takes this slot.");
  _remove = button("Remove", Icon::trash, _details);
  actions->addWidget(_change); actions->addWidget(_open); actions->addWidget(_remove);
  auto order = new QHBoxLayout; details->addLayout(order);
  _earlier = button("Higher priority", Icon::arrowup, _details); _later = button("Lower priority", Icon::arrowdown, _details);
  order->addWidget(_earlier); order->addWidget(_later);

  auto form = new QFormLayout; details->addLayout(form);
  _target = new QComboBox(_details);
  for (int t : {int(CombatSpell::Victim), int(CombatSpell::Self), int(CombatSpell::RandomEnemy), int(CombatSpell::RandomNotTank), int(CombatSpell::SecondThreat), int(CombatSpell::LastThreat)})
    _target->addItem(targetName(t), t);
  form->addRow("Cast on", _target);
  _chance = new QSpinBox(_details); _chance->setRange(1, 100); _chance->setSuffix(" %"); _chance->setToolTip("Rolled each time the timer is ready.");
  form->addRow("Chance", _chance);
  auto pair = [&](QSpinBox*& a, QSpinBox*& b, QString const& label, QString const& tip) {
    a = new QSpinBox(_details); b = new QSpinBox(_details);
    for (auto* s : {a, b}) { s->setRange(0, 3600); s->setSuffix(" s"); s->setToolTip(tip); }
    auto row = new QHBoxLayout; row->addWidget(a); row->addWidget(new QLabel("to", _details)); row->addWidget(b);
    form->addRow(label, row);
  };
  pair(_firstMin, _firstMax, "First cast after", "Seconds after combat starts before it is first cast (a random time in this range).");
  pair(_repeatMin, _repeatMax, "Then every", "Seconds between casts (a random time in this range).");
  _interrupt = new QCheckBox("Interrupt its current cast for this", _details);
  _auraMissing = new QCheckBox("Only if the target doesn't already have its effect", _details);
  _auraMissing->setToolTip("For buffs, debuffs and damage over time: don't recast while it is still active.");
  _melee = new QCheckBox("Only in melee range", _details);
  _notMelee = new QCheckBox("Only out of melee range", _details);
  _ranged = new QCheckBox("Main ranged spell: keeps its distance and casts this", _details);
  for (auto* c : {_interrupt, _auraMissing, _melee, _notMelee, _ranged}) details->addWidget(c);
  details->addStretch();

  connect(_change, &QPushButton::clicked, this, [this] {
    if (_selected < 0) return;
    if (auto spell = pickSpell(this, "Change combat spell", _spells[_selected].spell)) assign(_selected, *spell);
  });
  connect(_open, &QPushButton::clicked, this, [this] {
    if (_selected < 0) return;
    auto const current = _spells[_selected].spell;
    if (auto saved = editSpell(this, current)) { _info.remove(*saved); _info.remove(current); assign(_selected, *saved); }
  });
  connect(_remove, &QPushButton::clicked, this, [this] {
    if (_selected < 0) return;
    _spells.removeAt(_selected);
    refreshSlots(); select(std::min(_selected, int(_spells.size()) - 1));
  });
  auto move = [this](int delta) {
    int const to = _selected + delta;
    if (_selected < 0 || to < 0 || to >= _spells.size()) return;
    std::swap(_spells[_selected], _spells[to]);
    refreshSlots(); select(to);
  };
  connect(_earlier, &QPushButton::clicked, this, [move] { move(-1); });
  connect(_later, &QPushButton::clicked, this, [move] { move(1); });
  connect(_target, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { edited(_target); });
  for (auto* s : {_chance, _firstMin, _firstMax, _repeatMin, _repeatMax})
    connect(s, qOverload<int>(&QSpinBox::valueChanged), this, [this, s] { edited(s); });
  for (auto* c : {_interrupt, _auraMissing, _melee, _notMelee, _ranged})
    connect(c, &QCheckBox::toggled, this, [this, c] { edited(c); });

  refreshSlots();
  select(_spells.isEmpty() ? -1 : 0);
}
SpellDesign const* NpcSpellbook::info(Id spell) {
  auto it = _info.find(spell);
  if (it == _info.end()) {
    std::optional<SpellDesign> loaded;
    try { loaded = SpellService::load(spell); } catch (std::exception const&) {}
    it = _info.insert(spell, loaded);
  }
  return it->has_value() ? &**it : nullptr;
}
void NpcSpellbook::refreshSlots() {
  for (int i = 0; i < Npc::maxCombatSpells; ++i) {
    auto* slot = _slots[i];
    QSignalBlocker block(slot);
    if (i < _spells.size()) {
      auto const& s = _spells[i];
      auto const* d = info(s.spell);
      slot->setIcon(Ui::Content::ClientData::spellIcon(d ? std::uint32_t(d->icon) : 1u));
      QString name = d ? d->name + (d->rank.isEmpty() ? QString() : " (" + d->rank + ")") : QString("Missing spell %1").arg(s.spell);
      slot->setText(name + "\n" + summary(s));
      slot->setToolTip(d ? spellTooltipText(*d) : QString("Spell %1 is not in your local world.").arg(s.spell));
      slot->setEnabled(true);
    } else {
      slot->setIcon(Ui::FontAwesomeIcon(Icon::plus));
      slot->setText(i == _spells.size() ? "Empty slot\nclick to add a spell" : "Empty slot");
      slot->setToolTip("Choose a spell for this slot.");
      slot->setChecked(false);
      slot->setEnabled(i == _spells.size()); // fill slots in order
    }
  }
  _status->setText(QString("%1 of %2 slots used.").arg(_spells.size()).arg(Npc::maxCombatSpells));
}
void NpcSpellbook::select(int slot) {
  _selected = slot >= 0 && slot < _spells.size() ? slot : -1;
  for (int i = 0; i < Npc::maxCombatSpells; ++i) { QSignalBlocker block(_slots[i]); _slots[i]->setChecked(i == _selected); }
  showSelected();
}
void NpcSpellbook::showSelected() {
  bool const chosen = _selected >= 0;
  _details->setEnabled(chosen);
  if (!chosen) { _tooltip->setText("<i>Choose a spell in the spellbook, or add one.</i>"); return; }
  auto const& s = _spells[_selected];
  auto const* d = info(s.spell);
  _tooltip->setText(d ? spellTooltipText(*d) : QString("<b>Spell %1</b><br>Not in your local world: choose another.").arg(s.spell));
  _updating = true;
  int t = _target->findData(s.target);
  if (t < 0) { _target->addItem(targetName(s.target), s.target); t = _target->count() - 1; } // kept as loaded
  _target->setCurrentIndex(t);
  _chance->setValue(s.chance);
  _firstMin->setValue(s.firstMin); _firstMax->setValue(s.firstMax);
  _repeatMin->setValue(s.repeatMin); _repeatMax->setValue(s.repeatMax);
  _interrupt->setChecked(s.flags & CombatSpell::InterruptCast);
  _auraMissing->setChecked(s.flags & CombatSpell::AuraNotPresent);
  _melee->setChecked(s.flags & CombatSpell::OnlyInMelee);
  _notMelee->setChecked(s.flags & CombatSpell::NotInMelee);
  _ranged->setChecked(s.flags & CombatSpell::Ranged);
  _updating = false;
  _earlier->setEnabled(_selected > 0); _later->setEnabled(_selected + 1 < _spells.size());
}
void NpcSpellbook::assign(int slot, Id spell) {
  if (slot < 0 || slot > _spells.size() || slot >= Npc::maxCombatSpells) return;
  if (slot == _spells.size()) {
    CombatSpell s; s.spell = spell; s.target = defaultTarget(info(spell));
    if (s.target == CombatSpell::Self) s.flags |= CombatSpell::AuraNotPresent; // don't keep recasting a buff
    _spells.push_back(s);
  } else {
    _spells[slot].spell = spell;
  }
  refreshSlots(); select(slot);
}
void NpcSpellbook::edited(QObject* source) {
  if (_updating || _selected < 0) return;
  auto& s = _spells[_selected];
  s.target = _target->currentData().toInt();
  s.chance = _chance->value();
  // Keep each range ordered as the server expects.
  if (source == _firstMin && _firstMax->value() < _firstMin->value()) { QSignalBlocker b(_firstMax); _firstMax->setValue(_firstMin->value()); }
  if (source == _firstMax && _firstMin->value() > _firstMax->value()) { QSignalBlocker b(_firstMin); _firstMin->setValue(_firstMax->value()); }
  if (source == _repeatMin && _repeatMax->value() < _repeatMin->value()) { QSignalBlocker b(_repeatMax); _repeatMax->setValue(_repeatMin->value()); }
  if (source == _repeatMax && _repeatMin->value() > _repeatMax->value()) { QSignalBlocker b(_repeatMin); _repeatMin->setValue(_repeatMax->value()); }
  s.firstMin = _firstMin->value(); s.firstMax = _firstMax->value();
  s.repeatMin = _repeatMin->value(); s.repeatMax = _repeatMax->value();
  int flags = s.flags & ~(CombatSpell::InterruptCast | CombatSpell::AuraNotPresent | CombatSpell::OnlyInMelee | CombatSpell::NotInMelee | CombatSpell::Ranged);
  if (_interrupt->isChecked()) flags |= CombatSpell::InterruptCast;
  if (_auraMissing->isChecked()) flags |= CombatSpell::AuraNotPresent;
  if (_melee->isChecked()) flags |= CombatSpell::OnlyInMelee;
  if (_notMelee->isChecked()) flags |= CombatSpell::NotInMelee;
  if (_ranged->isChecked()) flags |= CombatSpell::Ranged;
  s.flags = flags;
  auto* slot = _slots[_selected];
  auto const* d = info(s.spell);
  QString name = d ? d->name + (d->rank.isEmpty() ? QString() : " (" + d->rank + ")") : QString("Missing spell %1").arg(s.spell);
  slot->setText(name + "\n" + summary(s));
}
}
