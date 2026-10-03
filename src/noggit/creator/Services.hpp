#pragma once
#include <QString>
#include <QVector>
#include <QMap>
#include <QVariant>
#include <QStringList>
#include <array>
#include <cstdint>

namespace Noggit::Creator {
using Id = std::uint32_t;
using Fields = QMap<QString, QVariant>;
struct Choice { Id id = 0; QString name, detail; };
struct Position { unsigned map = 0; float x = 0, y = 0, z = 0, orientation = 0; };
struct Npc {
  Id entry = 0, source = 0, display = 0, faction = 35;
  QString name;
  int level = 1, health = 100, mana = 0, armor = 0, rank = 0, unitClass = 1;
  int type = 7, respawn = 120, role = 0, movement = 0, attackMs = 2000;
  double damageMin = 1, damageMax = 2;
  std::array<Id, 3> equipment{};
  // Whitelisted copying. Gameplay associations require explicit opt-in; scripts are excluded.
  bool appearance = true, stats = true, allegiance = true, weapons = true, combat = true, motion = true;
  bool loot = false, vendor = false, trainer = false, gossip = false, quests = false;
};
struct SpawnEdit { Id guid = 0, entry = 0; Position position; int respawn = 120; bool remove = false, create = false; };
struct Objective { enum Type { Kill, Collect, Talk }; Type type = Kill; Id target = 0; int count = 1; QString label; };
struct Quest {
  Id entry = 0, giver = 0, ender = 0;
  QString title, description, completion;
  int requiredLevel = 1, level = 1, xp = 0, money = 0;
  QVector<Objective> objectives;
};
struct Outfit { QString name; Id display = 0; std::array<Id, 3> equipment{}; };
class CreatureService {
public:
  static QVector<Choice> search(QString const& text, bool ownedOnly = false);
  static Npc load(Id entry);
  static Id save(Npc const&, Position const&, Id* spawn = nullptr);
  static void remove(Id entry); // Creator NPCs only, with all their placements
  static bool owned(Id entry);
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
class QuestService {
public:
  static QVector<Choice> search(QString const& text);
  static Quest load(Id entry);
  static Id save(Quest const&);
  static void remove(Id entry); // Creator quests only
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
