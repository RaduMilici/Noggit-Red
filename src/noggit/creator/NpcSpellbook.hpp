#pragma once
#include "Services.hpp"
#include "SpellService.hpp"
#include <QHash>
#include <QWidget>
#include <array>
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QToolButton;
namespace Noggit::Creator {
// The NPC dialog's Spellbook page: the spells an NPC casts in combat, as a two-column spellbook of
// Npc::maxCombatSpells slots. Each slot picks any spell (or a new custom one from the Spell Editor) and
// sets when the server's default AI casts it: target, chance, first-cast delay and repeat timer.
class NpcSpellbook final : public QWidget {
public:
  explicit NpcSpellbook(QVector<CombatSpell> spells, QWidget* parent = nullptr);
  QVector<CombatSpell> const& spells() const { return _spells; }
private:
  SpellDesign const* info(Id spell); // cached; null when the spell cannot be loaded
  void refreshSlots();
  void select(int slot);
  void showSelected();
  void assign(int slot, Id spell); // slot == spells().size(): append
  void edited(QObject* source); // source: the control that changed
  QVector<CombatSpell> _spells;
  QHash<Id, std::optional<SpellDesign>> _info;
  int _selected = -1;
  bool _updating = false;
  std::array<QToolButton*, Npc::maxCombatSpells> _slots{};
  QGroupBox* _details = nullptr;
  QLabel *_tooltip = nullptr, *_status = nullptr;
  QPushButton *_change = nullptr, *_open = nullptr, *_remove = nullptr, *_earlier = nullptr, *_later = nullptr;
  QComboBox* _target = nullptr;
  QSpinBox *_chance = nullptr, *_firstMin = nullptr, *_firstMax = nullptr, *_repeatMin = nullptr, *_repeatMax = nullptr;
  QCheckBox *_interrupt = nullptr, *_auraMissing = nullptr, *_melee = nullptr, *_notMelee = nullptr, *_ranged = nullptr;
};
}
