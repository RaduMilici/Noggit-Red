#pragma once
#include "Services.hpp"
#include <QHash>
namespace Noggit::Creator {
struct TrainerSpell {
  Id spell = 0;          // the teaching spell trainers list (it teaches SpellInfo::learned)
  qint64 cost = 0;       // copper
  int level = 0;         // required character level; 0: none
  int skill = 0, skillValue = 0; // required profession skill, kept as loaded
  bool operator==(TrainerSpell const&) const = default;
};
// A spell as its trainer tooltip shows it.
struct SpellInfo {
  Id teach = 0, learned = 0, previous = 0; // previous: the learned spell of the rank before (spell chain)
  QString name, rank, description, previousName;
  int icon = 0, level = 0, school = 0, manaCost = 0, powerType = 0;
  qint64 suggestedCost = 0; int suggestedLevel = 0; // what original trainers charge and require
  QString title() const { return rank.isEmpty() ? name : name + " " + rank; }
};
struct Trainer {
  Id entry = 0, display = 0;
  QString name, notice;
  bool editable = false;
  bool teaches = false;   // has the trainer role: players can open the trainer window
  int type = 2;           // 0: one class only (playerClass), 2: anyone; 1 and 3 (riding, pets) are kept as loaded
  int playerClass = 0;
  Id sharedList = 0;      // a list several original trainers use; shown, not changed
  QVector<TrainerSpell> spells, shared;
};
struct LocalCharacter { Id guid = 0; QString name; int level = 1, playerClass = 0, race = 0; };
// What an NPC teaches, in the local world. Existing spells only; only Creator NPCs can be changed.
class TrainerService {
public:
  static QVector<Choice> owners(QString const& text); // NPCs that teach something
  static Trainer load(Id entry);
  static void save(Trainer const&);
  static QVector<RowProblem> validate(Trainer const&);
  // Database-free: `spells` holds every listed spell that trainers can teach.
  static QVector<RowProblem> check(Trainer const&, QHash<Id, SpellInfo> const& spells);
  // Spells a trainer can teach, by the name of what they teach.
  static QVector<SpellInfo> search(QString const& text, int limit = 300);
  static QHash<Id, SpellInfo> spells(QVector<Id> const& teaching);
  // Local test characters, to preview the trainer window as one of them.
  static QVector<LocalCharacter> characters();
  static QSet<Id> knownSpells(Id character); // learned spell IDs
  static QString className(int playerClass);
  static QVector<QPair<int, QString>> classes();
};
}
