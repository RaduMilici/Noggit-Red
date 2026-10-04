#pragma once
#include "Services.hpp"
#include <QHash>
#include <array>
#include <functional>
namespace Noggit::Creator {
// One of a spell's three effect slots, in designer terms. Values are what the spell does per hit or tick.
struct SpellEffect {
  int type = 0;               // what it does (SPELL_EFFECT_*), 0: unused slot
  int aura = 0;               // for "apply an aura" effects (SPELL_AURA_*)
  int targetA = 0, targetB = 0;
  int radius = 0;             // SpellRadius.dbc row, for area targets
  int period = 0;             // ms between ticks of periodic auras
  int mechanic = 0;
  int misc = 0;               // the creature, stat, power type or schools the effect needs
  int chain = 0;              // extra targets a chained effect jumps to
  Id trigger = 0;             // the spell it casts or teaches
  int minValue = 0, maxValue = 0;
  int baseDice = 1;           // kept as loaded (Vanilla uses 1)
  double perLevel = 0;        // value gained per caster level
  bool operator==(SpellEffect const&) const = default;
};
struct SpellDesign {
  Id entry = 0;     // 0: not saved yet
  Id source = 0;    // the spell it was copied from
  Fields row;       // every spell_template column: what the editor does not show is kept as copied
  QString name, rank, description, tooltip; // tooltip: the buff/debuff text
  Id icon = 1, visual = 0;
  int school = 0, castTime = 1, duration = 0, range = 1; // castTime, duration, range: rows of the client's tables
  int cooldown = 0, categoryCooldown = 0, globalCooldown = 1500; // ms
  int powerType = 0, cost = 0, costPercent = 0;
  int level = 1, maxLevel = 0, mechanic = 0, stacks = 0, procChance = 101;
  std::array<SpellEffect, 3> effects{};
  Id previous = 0;          // the rank before (spell_chain), 0: first or only rank
  bool teachable = false;   // trainers can teach it (a companion "teaching" spell exists)
  // Read-only facts.
  bool editable = false;
  QString notice;
  Id teach = 0;             // the companion teaching spell
  bool operator==(SpellDesign const& o) const;
};
struct SpellListEntry { Id entry = 0, icon = 0; QString name, rank; int school = 0, level = 0; bool own = false; };
struct SpellRank { Id entry = 0; int rank = 0, level = 0; QString name, rankText; bool own = false; };
struct SpellProblem { int effect = -1; QString text; bool error = true; }; // effect -1: the spell itself
// What the database-free checks need to know about the rest of the world.
struct SpellFacts {
  QSet<Id> spells, creatures, items;
  QSet<int> castTimes, durations, ranges, radii; // rows the client tables have (empty: unknown, not checked)
  QHash<Id, Id> previousOf;                       // spell -> its previous rank (to find cycles)
};
// Spells in the local world (spell_template; their client rows are generated from it, see ClientPatchService).
// Only Creator spells can be changed; game spells can be cloned or get a next rank.
class SpellService {
public:
  static constexpr Id idLimit = 65535; // The 1.12 spellbook protocol transmits spell IDs as uint16
  static QVector<Choice> search(QString const& text, bool own = false, int limit = 400); // detail: rank and school
  static QVector<SpellListEntry> browse(QString const& text, bool own = false, int limit = 400);
  static QVector<Choice> quests(QString const& text); // for "complete a quest" effects
  static SpellDesign load(Id entry);
  static SpellDesign blank(); // a new spell: an instant, self-cast, harmless spell to fill in
  static SpellDesign clone(Id source);
  // A copy of `entry` as its next rank: level and values raised, cost and cooldown kept, `previous` = entry.
  static SpellDesign nextRank(Id entry, int level, double valueScale, int cost, int cooldown);
  static QVector<SpellRank> chain(Id entry); // every rank of its chain, in order
  static Id save(SpellDesign const&);
  static void remove(Id entry);              // Creator spells nothing else uses
  static QStringList uses(Id entry);         // what uses it: items, trainers, quests, other spells
  static QVector<SpellProblem> validate(SpellDesign const&);
  static SpellFacts facts(SpellDesign const&);

  // Database-free parts.
  static QVector<SpellProblem> check(SpellDesign const&, SpellFacts const&);
  static SpellDesign fromRow(Fields const& row);
  static Fields toRow(SpellDesign const&); // the full spell_template row
  // "Deals 50 to 60 Fire damage."; names from `name(kind, id)` (kind: "spell", "creature", "item").
  static QString describe(SpellDesign const&, int effect, std::function<QString(QString const&, Id)> const& name);
  static QString suggestedDescription(SpellDesign const&); // with the client's $s1 / $d / $o1 tokens
};
namespace SpellCatalog {
struct Named { int id = 0; QString label, hint; };
QVector<Named> const& schools();      // 0 Physical .. 6 Arcane
QVector<Named> const& powers();       // Mana, Rage, Focus, Energy, Happiness, Health
QVector<Named> const& mechanics();
QVector<Named> const& stats();        // -1 all, 0 Strength ..
QVector<Named> const& effects();      // the effects the editor offers, in plain words
QVector<Named> const& auras();        // likewise
QString effectName(int effect);       // any effect, offered or not
QString auraName(int aura);
QString schoolName(int school);
bool needsScripting(int effect, int aura); // dummy and script effects do nothing without server scripts
struct Target { int id = 0; QString label; int a = 0, b = 0; bool area = false; };
QVector<Target> const& targets();
int targetOf(int a, int b);           // the preset index for an effect's targets, -1 if none matches
QString targetName(int a, int b);
// What an effect asks for, besides its target.
enum class Misc { None, Creature, Stat, Power, SchoolMask, Item, Quest, Dispel };
struct Inputs { bool value = false, aura = false, period = false, trigger = false, radius = false; Misc misc = Misc::None; QString valueLabel = "Amount"; };
Inputs inputs(SpellEffect const&);
enum class Template { DirectDamage, Heal, DoT, HoT, Buff, Debuff, Stun, Root, Silence, Fear, Summon, TriggerSpell, LearnSpell, Energize };
struct EffectTemplate { Template id; QString label, hint; };
QVector<EffectTemplate> const& templates();
// Fills effect `slot` (and, for the first effect, the spell's school-free defaults such as range, duration
// and look) the way the game's own spells of that kind are set up.
void apply(Template, SpellDesign&, int slot);
}
}
