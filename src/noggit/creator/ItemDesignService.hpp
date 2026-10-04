#pragma once
#include "ItemService.hpp"
#include <array>
namespace Noggit::Creator {
// A spell an item carries.
struct ItemSpell {
  enum Trigger { Use = 0, Equip = 1, ChanceOnHit = 2 };
  Id spell = 0;
  int trigger = Use;
  int charges = 0;          // 0: unlimited; negative: the item is used up after that many uses
  double perMinute = 0;     // chance on hit: procs per minute (0: the spell's own chance)
  int cooldown = -1, category = 0, categoryCooldown = -1; // ms; -1: the spell's own
  bool operator==(ItemSpell const&) const = default;
};
struct ItemDesign {
  Id entry = 0, source = 0;
  Fields row; // every item_template column: what the editor does not show is kept as copied
  QString name, description;
  int quality = 1, itemLevel = 1, requiredLevel = 0;
  int itemClass = 0, subclass = 0, inventoryType = 0, bonding = 0;
  int stackable = 1, maxCount = 0, buyCount = 1;
  qint64 buyPrice = 0, sellPrice = 0;
  Id display = 0;
  int armor = 0, block = 0, delay = 0, durability = 0;
  double damageMin = 0, damageMax = 0;
  int damageType = 0; // school of the weapon's damage
  QVector<QPair<int, int>> stats;   // (stat type, value), up to 10
  std::array<int, 6> resistances{}; // holy, fire, nature, frost, shadow, arcane
  int classes = -1, races = -1;     // masks; -1: everyone
  std::array<ItemSpell, 5> spells{};
  // Read-only facts.
  bool editable = false;
  QString notice;
  bool operator==(ItemDesign const& o) const;
};
struct ItemProblem { QString field; QString text; bool error = true; }; // field: the editor page it belongs to
struct ItemFacts { QSet<Id> spells, displays; };
// Items in the local world. Only Creator items can be changed; game items are cloned.
class ItemDesignService {
public:
  enum class Template { Weapon, Armor, Consumable, QuestItem, CraftingMaterial, Key, Trinket };
  struct TemplateInfo { Template id; QString label, hint; };
  static QVector<TemplateInfo> const& templates();
  static ItemDesign load(Id entry);
  static ItemDesign blank(Template); // with a look the game's items of that kind use
  static ItemDesign clone(Id source);
  static Id save(ItemDesign const&);
  static void remove(Id entry);       // Creator items nothing uses
  static QStringList uses(Id entry);
  static QVector<ItemProblem> validate(ItemDesign const&);
  static ItemFacts facts(ItemDesign const&);
  // "Use: Restores 500 health." as the tooltip shows it.
  static QString effectText(ItemSpell const&);

  // Database-free parts.
  static QVector<ItemProblem> check(ItemDesign const&, ItemFacts const&);
  static ItemDesign fromRow(Fields const& row);
  static Fields toRow(ItemDesign const&);
  static void apply(Template, ItemDesign&); // the kind's class, slot, binding, stacking and typical values
  static ItemInfo info(ItemDesign const&, QStringList const& effects); // for the tooltip preview
  static bool weapon(ItemDesign const& d) { return d.itemClass == 2; }
  static bool equippable(ItemDesign const& d) { return d.inventoryType > 0; }
  static QVector<QPair<int, QString>> slotsFor(int itemClass, int subclass); // the slots that make sense
  static QVector<QPair<int, QString>> classMask();  // (bit, class name), Vanilla classes
  static QVector<QPair<int, QString>> raceMask();
};
}
