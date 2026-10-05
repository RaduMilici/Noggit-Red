#pragma once
#include "Services.hpp"
#include <QHash>
namespace Noggit::Creator {
struct LootOwner {
  enum class Kind { Npc, Object };
  Kind kind = Kind::Npc;
  Id entry = 0;
  bool operator==(LootOwner const&) const = default;
};
struct LootRow {
  Id item = 0;
  double chance = 100; // percent. 0 inside a group shares what the group's other items leave; negative: quest drop
  int group = 0;       // 0: independent; 1..127: at most one item; references select this group in their table
  int minCount = 1, maxCount = 1;
  Id reference = 0;    // a shared loot table, rolled maxCount times, instead of an item
  Id condition = 0;
  bool quest() const { return chance < 0; }
  bool operator==(LootRow const&) const = default;
};
struct LootTable {
  LootOwner owner;
  QString ownerName;
  Id display = 0;          // the NPC's or object's look, for the preview
  bool editable = false;   // Creator content; original loot is shown read-only
  bool lootable = true;    // objects: only chests and fishing holes hold loot
  QString notice;          // why it is read-only, or what saving will do
  qint64 moneyMin = 0, moneyMax = 0;
  QVector<LootRow> rows;
};
struct LootSimulation {
  struct Item { Id item = 0; int kills = 0; qint64 count = 0; double expected = -1; }; // kills: kills it dropped in; expected < 0: unknown
  int kills = 0, empty = 0;  // empty: kills with no item and no money
  double averageMoney = 0;
  QVector<Item> items;       // in table order, then items from shared tables
};
// Loot of NPCs and of chest-like GameObjects, in the local world. Only Creator content can be changed;
// an NPC or chest still using a shared table (copied from what it was cloned from) gets its own on save.
class LootService {
public:
  // NPCs or chests by name: with loot (to copy from), or your own (to copy to).
  static QVector<Choice> owners(QString const& text, LootOwner::Kind, bool own = false);
  static LootTable load(LootOwner const&);
  static void save(LootTable const&);
  static QVector<RowProblem> validate(LootTable const&);

  // Database-free parts.
  static QVector<RowProblem> check(LootTable const&, QSet<Id> const& existingItems, QSet<Id> const& existingReferences);
  // The chance per kill of row `row` dropping, groups considered (quest drops: while the quest is active).
  static double expectedChance(QVector<LootRow> const& rows, int row);
  static LootSimulation simulate(LootTable const&, QHash<Id, QVector<LootRow>> const& references, int kills, quint32 seed);
  // The shared tables a table refers to, and theirs in turn (read-only).
  static QHash<Id, QVector<LootRow>> references(LootTable const&);
};
}
