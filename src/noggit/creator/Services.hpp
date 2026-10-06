#pragma once
#include <QString>
#include <QVector>
#include <QMap>
#include <QVariant>
#include <QStringList>
#include <array>
#include <optional>
#include <QSet>
#include <cstdint>

namespace Noggit::Creator {
using Id = std::uint32_t;
using Fields = QMap<QString, QVariant>;
struct Choice { Id id = 0; QString name, detail; };
// A problem shown beside its row in a list editor (row -1: the whole list). Errors block saving; warnings do not.
struct RowProblem { int row = -1; QString text; bool error = true; };
struct Position { unsigned map = 0; float x = 0, y = 0, z = 0, orientation = 0; bool operator==(Position const&) const = default; };
// One spell of an NPC's combat spell list (vmangos `creature_spells`, referenced by creature_template.spell_list_id).
// The server's default AI casts these in combat. Times are seconds.
struct CombatSpell {
  // castTarget values (the server's script target types) Creator offers.
  enum Target { Self = 0, Victim = 1, SecondThreat = 2, LastThreat = 3, RandomEnemy = 4, RandomNotTank = 5 };
  // castFlags Creator offers.
  enum Flag { InterruptCast = 0x01, Ranged = 0x08, AuraNotPresent = 0x20, OnlyInMelee = 0x40, NotInMelee = 0x80 };
  Id spell = 0;
  int chance = 100, target = Victim, flags = 0;
  int firstMin = 0, firstMax = 5, repeatMin = 10, repeatMax = 15;
  bool operator==(CombatSpell const&) const = default;
};
struct Npc {
  static constexpr int maxCombatSpells = 8; // creature_spells has eight slots
  Id entry = 0, source = 0, display = 0, faction = 35;
  QString name;
  int level = 1, health = 100, mana = 0, armor = 0, rank = 0, unitClass = 1;
  int type = 7, respawn = 120, role = 0, movement = 0, attackMs = 2000;
  double damageMin = 1, damageMax = 2;
  std::array<Id, 3> equipment{};
  QVector<CombatSpell> spells; // saved as the NPC's own list (spell_list_id = entry); empty: none
  // Whitelisted copying. Gameplay associations require explicit opt-in; scripts are excluded.
  bool appearance = true, stats = true, allegiance = true, weapons = true, combat = true, motion = true;
  bool loot = false, vendor = false, trainer = false, gossip = false, quests = false;
};
// What an NPC preview draws: its look, size and visible weapons (item display IDs).
struct NpcLook { Id display = 0; double scale = 0; QString name; Id mainhand = 0, offhand = 0, ranged = 0; int offhandType = 0; };
struct SpawnEdit { Id guid = 0, entry = 0; Position position; int respawn = 120; bool remove = false, create = false; };
struct GameObject {
  Id entry = 0, source = 0, display = 0, faction = 0, quest = 0;
  QString name;
  int type = 3, state = 1, respawn = 120;
  double size = 1;
};
class GameObjectService {
public:
  static QVector<Choice> search(QString const& text);
  static GameObject load(Id entry, Id guid = 0);
  static Id save(GameObject const&, std::optional<Position> place = std::nullopt);
  static void placements(QVector<SpawnEdit> const&);
};
struct Waypoint { Position position; int waitMs = 0; bool run = false; QVector<Fields> scripts; bool operator==(Waypoint const&) const = default; };
struct Patrol { QVector<Waypoint> points; bool loop = true; bool operator==(Patrol const&) const = default; };
class PatrolService {
public:
  static Patrol load(Id guid);
  static void save(Id guid, Patrol const&);
};
struct Outfit { QString name; Id display = 0; std::array<Id, 3> equipment{}; };
class CreatureService {
public:
  static QVector<Choice> search(QString const& text, bool ownedOnly = false);
  static Npc load(Id entry);
  // A new NPC is also placed at `place` when given (its first spawn's guid in *spawn).
  static Id save(Npc const&, std::optional<Position> const& place = std::nullopt, Id* spawn = nullptr);
  static QSet<Id> ownedEntries(); // NPCs made in Noggit
  static void remove(Id entry); // Creator NPCs only, with all their placements
  static bool owned(Id entry);
  static NpcLook look(Id entry);
};
class SpawnService {
public:
  static QVector<Id> save(QVector<SpawnEdit> const& edits);
};
class EquipmentService {
public:
  static QVector<Choice> search(QString const& text, int slot); // -1: all items (quest objectives)
  static QString name(Id item);
  static QVector<Outfit> outfits();
  static void saveOutfit(Outfit const& outfit);
};
// Local login accounts in the bundled realmd database. Never used for remote servers.
class AccountService {
public:
  static QStringList list();
  static void create(QString const& username, QString const& password);
};
struct Appearance {
  Id display = 0; QString name, model; int race = 0, sex = 0;
  std::array<int, 5> features{};
};
class AppearanceService {
public:
  static QVector<Appearance> catalog();
  static QVector<Choice> factions();
  static QString raceName(int race);
};
}
