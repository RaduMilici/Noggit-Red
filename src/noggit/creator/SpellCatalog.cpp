#include "SpellService.hpp"
#include <cmath>
namespace Noggit::Creator {
namespace {
// Fallback names for every effect, aura and target the server knows (generated from its enums).
char const* const effectNames[] = {
  "None", "Instakill", "School damage", "Dummy", "Portal teleport", "Teleport units",
  "Apply aura", "Environmental damage", "Power drain", "Health leech", "Heal", "Bind",
  "Portal", "Ritual base", "Ritual specialize", "Ritual activate portal", "Quest complete", "Weapon damage noschool",
  "Resurrect", "Add extra attacks", "Dodge", "Evade", "Parry", "Block",
  "Create item", "Weapon", "Defense", "Persistent area aura", "Summon", "Leap",
  "Energize", "Weapon percent damage", "Trigger missile", "Open lock", "Summon change item", "Apply area aura party",
  "Learn spell", "Spell defense", "Dispel", "Language", "Dual wield", "Summon wild",
  "Summon guardian", "Teleport units face caster", "Skill step", "Add honor", "Spawn", "Trade skill",
  "Stealth", "Detect", "Trans door", "Force critical hit", "Guarantee hit", "Enchant item",
  "Enchant item temporary", "Tamecreature", "Summon pet", "Learn pet spell", "Weapon damage", "Open lock item",
  "Proficiency", "Send event", "Power burn", "Threat", "Trigger spell", "Health funnel",
  "Power funnel", "Heal max health", "Interrupt cast", "Distract", "Pull", "Pickpocket",
  "Add farsight", "Summon possessed", "Summon totem", "Heal mechanical", "Summon object wild", "Script effect",
  "Attack", "Sanctuary", "Add combo points", "Create house", "Bind sight", "Duel",
  "Stuck", "Summon player", "Activate object", "Summon totem slot1", "Summon totem slot2", "Summon totem slot3",
  "Summon totem slot4", "Threat all", "Enchant held item", "Summon phantasm", "Self resurrect", "Skinning",
  "Charge", "Summon critter", "Knock back", "Disenchant", "Inebriate", "Feed pet",
  "Dismiss pet", "Reputation", "Summon object slot1", "Summon object slot2", "Summon object slot3", "Summon object slot4",
  "Dispel mechanic", "Summon dead pet", "Destroy all totems", "Durability damage", "Summon demon", "Resurrect new",
  "Attack me", "Durability damage pct", "Skin player corpse", "Spirit heal", "Skill", "Apply area aura pet",
  "Teleport graveyard", "Normalized weapon dmg", "Reputation quest reward", "Send taxi", "Player pull", "Modify threat percent",
  "Steal beneficial buff", "Prospecting", "Apply area aura friend", "Apply area aura enemy", "Despawn object", "Nostalrius",
  "Apply area aura raid", "Apply area aura owner", "Apply aura pet",
};
char const* const auraNames[] = {
  "None", "Bind sight", "Change possess", "Periodic damage", "Dummy", "Change confuse",
  "Change charm", "Change fear", "Periodic heal", "Change attackspeed", "Change threat", "Change taunt",
  "Change stun", "Change damage done", "Change damage taken", "Damage shield", "Change stealth", "Change stealth detect",
  "Change invisibility", "Change invisibility detection", "Obs change health", "Obs change mana", "Change resistance", "Periodic trigger spell",
  "Periodic energize", "Change pacify", "Change root", "Change silence", "Reflect spells", "Change stat",
  "Change skill", "Change increase speed", "Change increase mounted speed", "Change decrease speed", "Change increase health", "Change increase energy",
  "Change shapeshift", "Effect immunity", "State immunity", "School immunity", "Damage immunity", "Dispel immunity",
  "Proc trigger spell", "Proc trigger damage", "Track creatures", "Track resources", "Change parry skill", "Change parry percent",
  "Change dodge skill", "Change dodge percent", "Change block skill", "Change block percent", "Change crit percent", "Periodic leech",
  "Change hit chance", "Change spell hit chance", "Transform", "Change spell crit chance", "Change increase swim speed", "Change damage done creature",
  "Change pacify silence", "Change scale", "Periodic health funnel", "Periodic mana funnel", "Periodic mana leech", "Change casting speed not stack",
  "Feign death", "Change disarm", "Change stalked", "School absorb", "Extra attacks", "Change spell crit chance school",
  "Change power cost school pct", "Change power cost school", "Reflect spells school", "Change language", "Far sight", "Mechanic immunity",
  "Mounted", "Change damage percent done", "Change percent stat", "Split damage pct", "Water breathing", "Change base resistance",
  "Change regen", "Change power regen", "Channel death item", "Change damage percent taken", "Change health regen percent", "Periodic damage percent",
  "Change resist chance", "Change detect range", "Prevents fleeing", "Change unattackable", "Interrupt regen", "Ghost",
  "Spell magnet", "Mana shield", "Change skill talent", "Change attack power", "Auras visible", "Change resistance pct",
  "Change melee attack power versus", "Change total threat", "Water walk", "Feather fall", "Hover", "Add flat modifier",
  "Add pct modifier", "Add target trigger", "Change power regen percent", "Add caster hit trigger", "Override class scripts", "Change ranged damage taken",
  "Change ranged damage taken pct", "Change healing", "Change regen during combat", "Change mechanic resistance", "Change healing pct", "Share pet tracking",
  "Untrackable", "Empathy", "Change offhand damage pct", "Change target resistance", "Change ranged attack power", "Change melee damage taken",
  "Change melee damage taken pct", "Ranged attack power attacker bonus", "Change possess pet", "Change speed always", "Change mounted speed always", "Change ranged attack power versus",
  "Change increase energy percent", "Change increase health percent", "Change mana regen interrupt", "Change healing done", "Change healing done percent", "Change total stat percentage",
  "Change melee haste", "Force reaction", "Change ranged haste", "Change ranged ammo haste", "Change base resistance pct", "Change resistance exclusive",
  "Safe fall", "Charisma", "Persuaded", "Mechanic immunity mask", "Retain combo points", "Resist pushback",
  "Change shield blockvalue pct", "Track stealthed", "Change detected range", "Split damage flat", "Change stealth level", "Change water breathing",
  "Change reputation gain", "Pet damage multi", "Change shield blockvalue", "No pvp credit", "Change aoe avoidance", "Change health regen in combat",
  "Power burn mana", "Change crit damage bonus", "164", "Melee attack power attacker bonus", "Change attack power pct", "Change ranged attack power pct",
  "Change damage done versus", "Change crit percent versus", "Detect amore", "Change speed not stack", "Change mounted speed not stack", "Allow champion spells",
  "Change spell damage of stat percent", "Change spell healing of stat percent", "Spirit of redemption", "Aoe charm", "Change debuff resistance", "Change attacker spell crit chance",
  "Change flat spell damage versus", "Change flat spell crit damage versus", "Change resistance of stat percent", "Change critical threat", "Change attacker melee hit chance", "Change attacker ranged hit chance",
  "Change attacker spell hit chance", "Change attacker melee crit chance", "Change attacker ranged crit chance", "Change rating", "Change faction reputation gain", "Use normal movement speed",
  "Aura spell", "Split damage group pct", "Change aoe damage percent taken", "Change honor gain", "Enable flying", "Change periodic damage percent taken",
  "Change crit damage bonus taken", "Change spell healing of armor percent", "Transfer totem threat", "Trigger aura on totems", "Change pet stat percent of owner", "Change pet armor percent of owner",
  "Change pet resistance percent of owner", "Change pet attack power percent of owner", "Change pet spell damage percent of owner", "Change attack and spell range", "Change reagent consumption chance", "Change mechanic duration",
  "Change self resurrection recovery", "Change pet spell crit percent of owner", "Change ignore target armor", "Change spell damage of intellect percent", "Change mana gain percent", "Change pet spell hit percent of owner",
  "Change pet melee hit percent of owner", "Change energy regen time", "Periodic trigger spell2", "219", "Change damage taken from caster pet", "Change attack power area",
  "Change attack power percent area", "Change item proc chance", "Change block damage percent", "Change gathering item chance", "Change rage from damage dealt",
};
char const* const targetNames[] = {
  "None", "Unit caster", "Unit enemy near caster", "Unit friend near caster", "Unit near caster", "Unit caster pet",
  "Unit enemy", "Enum units script aoe at src loc", "Enum units script aoe at dest loc", "Location caster home bind", "Location caster divine bind (not implemented)", "Player (not implemented)",
  "Player near caster (not implemented)", "Player enemy (not implemented)", "Player friend (not implemented)", "Enum units enemy aoe at src loc", "Enum units enemy aoe at dest loc", "Location database",
  "Location caster dest", "Unk 19", "Enum units party within caster range", "Unit friend", "Location caster src", "Gameobject",
  "Enum units enemy in cone 24", "Unit", "Locked", "Unit caster master", "Enum units enemy aoe at dynobj loc", "Enum units friend aoe at dynobj loc",
  "Enum units friend aoe at src loc", "Enum units friend aoe at dest loc", "Location unit minion position", "Enum units party aoe at src loc", "Enum units party aoe at dest loc", "Unit party",
  "Enum units enemy within caster range", "Unit friend and party", "Unit script near caster", "Location caster fishing spot", "Gameobject script near caster", "Location caster front right",
  "Location caster back right", "Location caster back left", "Location caster front left", "Unit friend chain heal", "Location script near caster", "Location caster front",
  "Location caster back", "Location caster left", "Location caster right", "Enum gameobjects script aoe at src loc", "Enum gameobjects script aoe at dest loc", "Location caster target position",
  "Enum units enemy in cone 54", "Location caster front leap", "Enum units raid within caster range", "Unit raid", "Unit raid near caster", "Enum units friend in cone",
  "Enum units script in cone 60", "Unit raid and class", "Player raid (not implemented)", "Location unit position",
};
template <std::size_t N> QString fromTable(char const* const (&table)[N], int id, char const* kind) {
  return id >= 0 && std::size_t(id) < N ? QString(table[id]) : QString("%1 %2").arg(kind).arg(id);
}
int number(Fields const& row, QString const& column) { return row.value(column).toInt(); }
enum Effects { Instakill = 1, Damage = 2, Dummy = 3, ApplyAura = 6, PowerDrain = 8, Leech = 9, Heal = 10, CompleteQuest = 16, WeaponNoSchool = 17,
  CreateItem = 24, PersistentArea = 27, Summon = 28, Energize = 30, WeaponPercent = 31, PartyAura = 35, Learn = 36, Dispel = 38, SummonWild = 41,
  WeaponDamage = 58, PowerBurn = 62, Threat = 63, Trigger = 64, Interrupt = 68, Script = 77, KnockBack = 98, Charge = 96 };
enum Auras { PeriodicDamage = 3, AuraDummy = 4, Confuse = 5, Fear = 7, PeriodicHeal = 8, Taunt = 11, Stun = 12, DamageDone = 13, DamageShield = 15,
  Stealth = 16, Resistance = 22, PeriodicTrigger = 23, PeriodicEnergize = 24, Pacify = 25, Root = 26, Silence = 27, Stat = 29, Speed = 31,
  Slow = 33, MaxHealth = 34, ProcTrigger = 42, Crit = 52, PeriodicLeech = 53, Hit = 54, Transform = 56, SpellCrit = 57, Scale = 61, Absorb = 69,
  DamagePercent = 79, Regen = 84, PowerRegen = 85, DamageTakenPercent = 87, AttackPower = 99, Healing = 115 };
bool periodic(int aura) { return aura == PeriodicDamage || aura == PeriodicHeal || aura == PeriodicTrigger || aura == PeriodicEnergize || aura == PeriodicLeech; }
}
bool SpellDesign::operator==(SpellDesign const& o) const {
  return SpellService::toRow(*this) == SpellService::toRow(o) && previous == o.previous && teachable == o.teachable;
}
namespace SpellCatalog {
QVector<Named> const& schools() {
  static QVector<Named> const list{{0, "Physical"}, {1, "Holy"}, {2, "Fire"}, {3, "Nature"}, {4, "Frost"}, {5, "Shadow"}, {6, "Arcane"}};
  return list;
}
QString schoolName(int school) { for (auto const& s : schools()) if (s.id == school) return s.label; return QString("School %1").arg(school); }
QVector<Named> const& powers() {
  static QVector<Named> const list{{0, "Mana"}, {1, "Rage"}, {2, "Focus"}, {3, "Energy"}, {4, "Happiness (pets)"}, {-2, "Health"}};
  return list;
}
QVector<Named> const& mechanics() {
  static QVector<Named> const list{{0, "None"}, {1, "Charm"}, {2, "Disorient"}, {3, "Disarm"}, {4, "Distract"}, {5, "Fear"}, {6, "Fumble"}, {7, "Root"},
    {8, "Pacify"}, {9, "Silence"}, {10, "Sleep"}, {11, "Snare"}, {12, "Stun"}, {13, "Freeze"}, {14, "Knockout"}, {15, "Bleed"}, {16, "Bandage"},
    {17, "Polymorph"}, {18, "Banish"}, {19, "Shield"}, {20, "Shackle"}, {21, "Mount"}, {22, "Persuade"}, {23, "Turn"}, {24, "Horror"},
    {25, "Invulnerability"}, {26, "Interrupt"}, {27, "Daze"}, {28, "Discovery"}, {29, "Immune shield"}, {30, "Sapped"}};
  return list;
}
QVector<Named> const& stats() {
  static QVector<Named> const list{{-1, "All stats"}, {0, "Strength"}, {1, "Agility"}, {2, "Stamina"}, {3, "Intellect"}, {4, "Spirit"}};
  return list;
}
QVector<Named> const& effects() {
  static QVector<Named> const list{
    {0, "(none)", "This slot does nothing."},
    {Damage, "Damage", "Deals damage of the spell's school."},
    {Heal, "Heal", "Restores health."},
    {ApplyAura, "Apply an aura", "A lasting effect: damage or healing over time, buffs, debuffs, crowd control."},
    {Trigger, "Cast another spell", "Casts an existing spell as well."},
    {Learn, "Teach a spell", "The target learns an existing spell."},
    {Energize, "Restore power", "Gives mana, rage, energy or focus."},
    {SummonWild, "Summon a creature", "Summons an NPC near the caster for the spell's duration."},
    {Summon, "Summon a companion", "Summons an NPC that follows the caster."},
    {PowerDrain, "Drain power", "Takes mana or another power from the target."},
    {Leech, "Drain life", "Damages the target and heals the caster."},
    {PowerBurn, "Burn power", "Destroys the target's mana."},
    {CreateItem, "Create an item", "Puts an item in the caster's bags."},
    {WeaponDamage, "Weapon damage +", "A weapon attack with extra damage."},
    {WeaponPercent, "Weapon damage %", "A weapon attack for a percentage of weapon damage."},
    {Instakill, "Kill instantly", "Kills the target."},
    {Dispel, "Dispel", "Removes magic, curses, diseases or poisons."},
    {Interrupt, "Interrupt", "Interrupts the target's spellcasting."},
    {Threat, "Threat", "Adds threat."},
    {KnockBack, "Knock back", "Knocks the target back."},
    {Charge, "Charge", "The caster rushes to the target."},
    {CompleteQuest, "Complete a quest", "Completes a quest for the target."}};
  return list;
}
QVector<Named> const& auras() {
  static QVector<Named> const list{
    {PeriodicDamage, "Damage over time", "Deals the amount every tick."}, {PeriodicHeal, "Healing over time", "Heals the amount every tick."},
    {PeriodicEnergize, "Power over time", "Restores power every tick."}, {PeriodicTrigger, "Cast a spell every tick", ""},
    {PeriodicLeech, "Drain life over time", ""}, {Stat, "Change a stat", "Positive raises, negative lowers it."},
    {Resistance, "Change armor or resistance", "Choose the schools: armor is Physical."}, {MaxHealth, "Change maximum health", ""},
    {AttackPower, "Change attack power", ""}, {DamageDone, "Change damage done", "Flat bonus to the chosen schools."},
    {DamagePercent, "Change damage done %", ""}, {DamageTakenPercent, "Change damage taken %", ""}, {Healing, "Change healing done", ""},
    {Crit, "Change critical strike chance", ""}, {SpellCrit, "Change spell critical chance", ""}, {Hit, "Change hit chance", ""},
    {Regen, "Change health regeneration", ""}, {PowerRegen, "Change power regeneration", ""}, {Speed, "Increase movement speed %", ""},
    {Slow, "Slow movement %", "Use a negative amount."}, {Stun, "Stun", ""}, {Root, "Root", ""}, {Silence, "Silence", ""}, {Fear, "Fear", ""},
    {Confuse, "Confuse", ""}, {Pacify, "Pacify", ""}, {Taunt, "Taunt", ""}, {DamageShield, "Damage shield", "Damages attackers."},
    {Absorb, "Absorb damage", "Choose the schools it absorbs."}, {ProcTrigger, "Cast a spell on hit", "Uses the spell's proc chance."},
    {Stealth, "Stealth", ""}, {Transform, "Transform into a creature", ""}, {Scale, "Change size %", ""}};
  return list;
}
QString effectName(int effect) {
  for (auto const& e : effects()) if (e.id == effect) return e.label;
  return fromTable(effectNames, effect, "Effect");
}
QString auraName(int aura) {
  for (auto const& a : auras()) if (a.id == aura) return a.label;
  return fromTable(auraNames, aura, "Aura");
}
bool needsScripting(int effect, int aura) { return effect == Dummy || effect == Script || (effect == ApplyAura && aura == AuraDummy); }
QVector<Target> const& targets() {
  static QVector<Target> const list{
    {0, "Self", 1, 0, false}, {1, "Enemy target", 6, 0, false}, {2, "Friendly target", 21, 0, false}, {3, "Any target", 25, 0, false},
    {4, "My pet", 5, 0, false}, {5, "Party member", 35, 0, false}, {6, "Enemies around me", 22, 15, true}, {7, "Allies around me", 22, 30, true},
    {8, "My party around me", 20, 0, true}, {9, "Enemies in front of me", 24, 0, true}, {10, "A spot in front of me", 18, 0, false}};
  return list;
}
int targetOf(int a, int b) {
  for (auto const& t : targets()) if (t.a == a && t.b == b) return t.id;
  return -1;
}
QString targetName(int a, int b) {
  if (auto t = targetOf(a, b); t >= 0) return targets()[t].label;
  return fromTable(targetNames, a, "Target") + (b ? " + " + fromTable(targetNames, b, "Target") : QString());
}
Inputs inputs(SpellEffect const& e) {
  Inputs in;
  switch (e.type) {
    case Damage: case Heal: case Leech: case PowerBurn: case Threat: in.value = true; break;
    case WeaponDamage: in.value = true; in.valueLabel = "Bonus damage"; break;
    case WeaponPercent: in.value = true; in.valueLabel = "Weapon damage %"; break;
    case PowerDrain: in.value = true; in.misc = Misc::Power; break;
    case Energize: in.value = true; in.misc = Misc::Power; break;
    case Trigger: case Learn: in.trigger = true; break;
    case Summon: case SummonWild: in.misc = Misc::Creature; in.value = true; in.valueLabel = "How many"; break;
    case CreateItem: in.misc = Misc::Item; in.value = true; in.valueLabel = "How many"; break;
    case CompleteQuest: in.misc = Misc::Quest; break;
    case Dispel: in.misc = Misc::Dispel; in.value = true; in.valueLabel = "Effects removed"; break;
    case KnockBack: in.value = true; in.valueLabel = "Distance"; break;
    case ApplyAura: case PersistentArea: case PartyAura:
      in.aura = true;
      switch (e.aura) {
        case PeriodicDamage: case PeriodicHeal: case PeriodicLeech: in.value = true; in.period = true; in.valueLabel = "Per tick"; break;
        case PeriodicEnergize: in.value = true; in.period = true; in.misc = Misc::Power; in.valueLabel = "Per tick"; break;
        case PeriodicTrigger: in.trigger = true; in.period = true; break;
        case ProcTrigger: in.trigger = true; break;
        case Stat: in.value = true; in.misc = Misc::Stat; break;
        case Resistance: case DamageDone: case DamagePercent: case DamageTakenPercent: case Absorb: in.value = true; in.misc = Misc::SchoolMask; break;
        case PowerRegen: in.value = true; in.misc = Misc::Power; break;
        case Transform: in.misc = Misc::Creature; break;
        case Stun: case Root: case Silence: case Fear: case Confuse: case Pacify: case Taunt: case Stealth: break;
        default: in.value = true; break;
      }
      break;
    default: break;
  }
  in.radius = e.targetA && targetOf(e.targetA, e.targetB) >= 0 && targets()[targetOf(e.targetA, e.targetB)].area;
  if (targetOf(e.targetA, e.targetB) < 0 && e.radius) in.radius = true;
  return in;
}
QVector<EffectTemplate> const& templates() {
  static QVector<EffectTemplate> const list{
    {Template::DirectDamage, "Direct Damage", "Hits an enemy for an amount of damage."}, {Template::Heal, "Heal", "Heals a friendly target."},
    {Template::DoT, "Damage over Time", "Damages an enemy every 3 seconds."}, {Template::HoT, "Healing over Time", "Heals a friend every 3 seconds."},
    {Template::Buff, "Buff", "Raises a friend's Stamina for a while."}, {Template::Debuff, "Debuff", "Lowers an enemy's armor for a while."},
    {Template::Stun, "Stun", "The enemy cannot act."}, {Template::Root, "Root", "The enemy cannot move."},
    {Template::Silence, "Silence", "The enemy cannot cast spells."}, {Template::Fear, "Fear", "The enemy runs in fear."},
    {Template::Summon, "Summon", "Summons a creature next to the caster."}, {Template::TriggerSpell, "Trigger Spell", "Casts another existing spell."},
    {Template::LearnSpell, "Learn Spell", "Teaches the target an existing spell."}, {Template::Energize, "Energize", "Restores mana or another power."}};
  return list;
}
void apply(Template kind, SpellDesign& d, int slot) {
  SpellEffect e;
  // Spell-level defaults, as most of the game's spells of this kind have them (the first effect decides).
  struct Look { int range, duration, visual, icon; };
  Look look{1, 0, 0, 1};
  auto aura = [&](int name, int a, int mechanic = 0) { e.type = ApplyAura; e.aura = name; e.targetA = a; e.mechanic = 0; d.mechanic = mechanic; };
  switch (kind) {
    case Template::DirectDamage: e.type = Damage; e.targetA = 6; e.minValue = 50; e.maxValue = 60; look = {4, 0, 0, 11}; break;
    case Template::Heal: e.type = Heal; e.targetA = 21; e.minValue = 100; e.maxValue = 120; look = {5, 0, 58, 104}; break;
    case Template::DoT: aura(PeriodicDamage, 6); e.period = 3000; e.minValue = e.maxValue = 10; look = {4, 8, 372, 68}; break;
    case Template::HoT: aura(PeriodicHeal, 21); e.period = 3000; e.minValue = e.maxValue = 15; look = {5, 29, 280, 64}; break;
    case Template::Buff: aura(Stat, 21); e.misc = 2; e.minValue = e.maxValue = 10; look = {5, 30, 0, 63}; break;
    case Template::Debuff: aura(Resistance, 6); e.misc = 1; e.minValue = e.maxValue = -200; look = {4, 21, 0, 47}; break;
    case Template::Stun: aura(Stun, 6, 12); look = {2, 39, 87, 44}; break;
    case Template::Root: aura(Root, 6, 7); look = {4, 1, 38, 20}; break;
    case Template::Silence: aura(Silence, 6, 9); look = {3, 1, 179, 232}; break;
    case Template::Fear: aura(Fear, 6, 5); look = {4, 31, 336, 98}; break;
    case Template::Summon: e.type = SummonWild; e.targetA = 18; e.minValue = e.maxValue = 1; look = {1, 21, 74, 1}; break;
    case Template::TriggerSpell: e.type = Trigger; e.targetA = 1; look = {1, 0, 255, 329}; break;
    case Template::LearnSpell: e.type = Learn; e.targetA = 1; look = {6, 0, 107, 198}; break;
    case Template::Energize: e.type = Energize; e.targetA = 1; e.misc = 0; e.minValue = e.maxValue = 100; look = {1, 0, 0, 1}; break;
  }
  d.effects[slot] = e;
  if (slot == 0) { d.range = look.range; d.duration = look.duration; if (look.visual) d.visual = look.visual; d.icon = look.icon; }
  else if (!d.duration && look.duration) d.duration = look.duration;
}
}
using namespace SpellCatalog;
SpellDesign SpellService::fromRow(Fields const& row) {
  SpellDesign d; d.row = row;
  d.entry = row.value("entry").toUInt();
  d.name = row.value("name").toString(); d.rank = row.value("nameSubtext").toString();
  d.description = row.value("description").toString(); d.tooltip = row.value("auraDescription").toString();
  d.icon = row.value("spellIconId").toUInt(); d.visual = row.value("spellVisual1").toUInt();
  d.school = number(row, "school"); d.castTime = number(row, "castingTimeIndex"); d.duration = number(row, "durationIndex"); d.range = number(row, "rangeIndex");
  d.cooldown = number(row, "recoveryTime"); d.categoryCooldown = number(row, "categoryRecoveryTime"); d.globalCooldown = number(row, "startRecoveryTime");
  d.powerType = number(row, "powerType"); d.cost = number(row, "manaCost"); d.costPercent = number(row, "manaCostPercentage");
  d.level = number(row, "spellLevel"); d.maxLevel = number(row, "maxLevel"); d.mechanic = number(row, "mechanic");
  d.stacks = number(row, "stackAmount"); d.procChance = number(row, "procChance");
  for (int j = 0; j < 3; ++j) {
    auto c = [&](QString const& name) { return name + QString::number(j + 1); };
    auto& e = d.effects[j];
    e.type = number(row, c("effect")); e.aura = number(row, c("effectApplyAuraName"));
    e.targetA = number(row, c("effectImplicitTargetA")); e.targetB = number(row, c("effectImplicitTargetB"));
    e.radius = number(row, c("effectRadiusIndex")); e.period = number(row, c("effectAmplitude")); e.mechanic = number(row, c("effectMechanic"));
    e.misc = e.type == CreateItem ? number(row, c("effectItemType")) : number(row, c("effectMiscValue"));
    e.chain = number(row, c("effectChainTarget")); e.trigger = row.value(c("effectTriggerSpell")).toUInt();
    e.perLevel = row.value(c("effectRealPointsPerLevel")).toDouble();
    // The server's roll: base points plus a die from baseDice to dieSides (0 or 1 sides: baseDice exactly).
    int base = number(row, c("effectBasePoints")), dice = number(row, c("effectBaseDice")), sides = number(row, c("effectDieSides"));
    e.baseDice = dice;
    e.minValue = base + (sides <= 1 ? dice : std::min(dice, sides));
    e.maxValue = base + (sides <= 1 ? dice : std::max(dice, sides));
  }
  return d;
}
Fields SpellService::toRow(SpellDesign const& d) {
  Fields row = d.row;
  auto set = [&](QString const& column, QVariant const& value) { row[column] = value.toString(); };
  if (d.entry) set("entry", d.entry);
  set("name", d.name.trimmed()); set("nameSubtext", d.rank.trimmed()); set("description", d.description); set("auraDescription", d.tooltip);
  set("spellIconId", d.icon); set("spellVisual1", d.visual);
  set("school", d.school); set("castingTimeIndex", d.castTime); set("durationIndex", d.duration); set("rangeIndex", d.range);
  set("recoveryTime", d.cooldown); set("categoryRecoveryTime", d.categoryCooldown); set("startRecoveryTime", d.globalCooldown);
  set("powerType", d.powerType); set("manaCost", d.cost); set("manaCostPercentage", d.costPercent);
  set("spellLevel", d.level); set("baseLevel", d.level); set("maxLevel", d.maxLevel); set("mechanic", d.mechanic);
  set("stackAmount", d.stacks); set("procChance", d.procChance);
  for (int j = 0; j < 3; ++j) {
    auto c = [&](QString const& name) { return name + QString::number(j + 1); };
    auto const& e = d.effects[j];
    set(c("effect"), e.type); set(c("effectApplyAuraName"), e.type ? e.aura : 0);
    set(c("effectImplicitTargetA"), e.targetA); set(c("effectImplicitTargetB"), e.targetB);
    set(c("effectRadiusIndex"), e.radius); set(c("effectAmplitude"), e.period); set(c("effectMechanic"), e.mechanic);
    set(c("effectMiscValue"), e.type == CreateItem ? 0 : e.misc); set(c("effectItemType"), e.type == CreateItem ? e.misc : 0);
    set(c("effectChainTarget"), e.chain); set(c("effectTriggerSpell"), e.trigger);
    set(c("effectRealPointsPerLevel"), e.perLevel);
    // Written back with a one-sided die: base + 1 .. base + sides.
    int base = e.minValue - 1;
    set(c("effectBaseDice"), 1); set(c("effectBasePoints"), base); set(c("effectDieSides"), std::max(1, e.maxValue - base));
  }
  return row;
}
QVector<SpellProblem> SpellService::check(SpellDesign const& d, SpellFacts const& f) {
  QVector<SpellProblem> p;
  auto add = [&](int effect, QString const& text, bool error = true) { p.push_back({effect, text, error}); };
  if (d.name.trimmed().isEmpty()) add(-1, "Name the spell.");
  else if (d.name.size() > 100) add(-1, "Keep the name under 100 characters.");
  if (d.level < 0 || d.level > 60) add(-1, "The level is 0 (none) to 60.");
  if (d.maxLevel && d.maxLevel < d.level) add(-1, "The maximum level is below the spell's level.");
  if (d.cost < 0 || d.costPercent < 0 || d.costPercent > 100) add(-1, "The cost cannot be negative; a percentage is 0 to 100.");
  if (d.cooldown < 0 || d.categoryCooldown < 0 || d.globalCooldown < 0) add(-1, "Cooldowns cannot be negative.");
  if (d.stacks < 0 || d.stacks > 255) add(-1, "Stacks are 0 to 255.");
  if (!f.castTimes.isEmpty() && !f.castTimes.contains(d.castTime)) add(-1, "The client has no such cast time. Choose one from the list.");
  if (!f.durations.isEmpty() && d.duration && !f.durations.contains(d.duration)) add(-1, "The client has no such duration. Choose one from the list.");
  if (!f.ranges.isEmpty() && !f.ranges.contains(d.range)) add(-1, "The client has no such range. Choose one from the list.");
  if (d.previous) {
    if (d.previous == d.entry && d.entry) add(-1, "A spell cannot be its own previous rank.");
    else if (!f.spells.contains(d.previous)) add(-1, QString("The previous rank (spell %1) does not exist.").arg(d.previous));
    else for (Id at = d.previous, steps = 0; at && steps < 64; at = f.previousOf.value(at), ++steps)
      if (at == d.entry && d.entry) { add(-1, "The previous rank leads back to this spell: ranks would go in a circle."); break; }
  }
  if (d.teachable && d.level <= 0) add(-1, "Trainers offer spells by level: set the spell's level.", false);
  bool any = false;
  for (int j = 0; j < 3; ++j) {
    auto const& e = d.effects[j];
    if (!e.type) continue;
    any = true;
    auto in = inputs(e);
    if (needsScripting(e.type, e.aura)) add(j, "This effect does nothing without server scripting; it is kept as copied.", false);
    if (in.aura && !e.aura) add(j, "Choose what the aura does.");
    if (in.aura && !d.duration) add(j, "Choose how long the aura lasts (Duration).");
    if (in.period && e.period <= 0) add(j, "Choose how often it ticks.");
    if (in.value && e.minValue > e.maxValue) add(j, "The minimum is above the maximum.");
    if (in.value && (e.type == Damage || e.type == Heal) && e.maxValue <= 0) add(j, "Set how much it does.");
    if (in.trigger) {
      if (!e.trigger) add(j, "Choose the spell.");
      else if (e.trigger == d.entry && d.entry) add(j, "A spell cannot cast or teach itself.");
      else if (!f.spells.contains(e.trigger)) add(j, QString("Spell %1 does not exist.").arg(e.trigger));
    }
    if (in.misc == Misc::Creature) {
      if (!e.misc) add(j, "Choose the creature.");
      else if (!f.creatures.contains(Id(e.misc))) add(j, QString("Creature %1 does not exist.").arg(e.misc));
    }
    if (in.misc == Misc::Item) {
      if (!e.misc) add(j, "Choose the item.");
      else if (!f.items.contains(Id(e.misc))) add(j, QString("Item %1 does not exist.").arg(e.misc));
    }
    if (in.misc == Misc::SchoolMask && !e.misc) add(j, "Choose the schools it affects.");
    if (in.radius && !e.radius) add(j, "Choose the area's radius.");
    if (e.radius && !f.radii.isEmpty() && !f.radii.contains(e.radius)) add(j, "The client has no such radius. Choose one from the list.");
    if (!e.targetA && e.type != Learn && e.type != Trigger) add(j, "Choose who it affects.", false);
  }
  if (!any) add(-1, "Give the spell at least one effect.");
  return p;
}
QString SpellService::describe(SpellDesign const& d, int j, std::function<QString(QString const&, Id)> const& name) {
  auto const& e = d.effects[j];
  if (!e.type) return {};
  auto amount = [&] { return e.minValue == e.maxValue ? QString::number(std::abs(e.minValue)) : QString("%1 to %2").arg(e.minValue).arg(e.maxValue); };
  auto school = schoolName(d.school);
  auto every = [&] { return QString(" every %1 sec").arg(e.period / 1000.0); };
  auto miscName = [&](QVector<Named> const& list) { for (auto const& n : list) if (n.id == e.misc) return n.label; return QString::number(e.misc); };
  auto target = " (" + targetName(e.targetA, e.targetB).toLower() + ")";
  switch (e.type) {
    case Damage: return "Deals " + amount() + " " + school + " damage" + target;
    case Heal: return "Heals " + amount() + target;
    case Energize: return "Restores " + amount() + " " + miscName(powers()) + target;
    case Trigger: return "Casts " + name("spell", e.trigger) + target;
    case Learn: return "Teaches " + name("spell", e.trigger);
    case Summon: case SummonWild: return "Summons " + name("creature", Id(e.misc));
    case CreateItem: return "Creates " + amount() + " × " + name("item", Id(e.misc));
    case ApplyAura:
      switch (e.aura) {
        case PeriodicDamage: return "Deals " + amount() + " " + school + " damage" + every() + target;
        case PeriodicHeal: return "Heals " + amount() + every() + target;
        case PeriodicEnergize: return "Restores " + amount() + " " + miscName(powers()) + every() + target;
        case PeriodicTrigger: return "Casts " + name("spell", e.trigger) + every() + target;
        case Stat: return QString(e.minValue < 0 ? "Lowers " : "Raises ") + miscName(stats()) + " by " + amount() + target;
        case Resistance: return QString(e.minValue < 0 ? "Lowers " : "Raises ") + (e.misc == 1 ? QString("armor") : QString("resistance")) + " by " + amount() + target;
        case Stun: return "Stuns" + target; case Root: return "Roots" + target; case Silence: return "Silences" + target; case Fear: return "Fears" + target;
        case ProcTrigger: return "On hit, may cast " + name("spell", e.trigger) + target;
        default: return auraName(e.aura) + (inputs(e).value ? " by " + amount() : QString()) + target;
      }
    default: return effectName(e.type) + (inputs(e).value ? ": " + amount() : QString()) + target;
  }
}
QString SpellService::suggestedDescription(SpellDesign const& d) {
  QStringList parts;
  auto school = schoolName(d.school);
  for (int j = 0; j < 3; ++j) {
    auto const& e = d.effects[j]; auto s = QString("$s%1").arg(j + 1), o = QString("$o%1").arg(j + 1);
    switch (e.type) {
      case Damage: parts << "Deals " + s + " " + school + " damage."; break;
      case Heal: parts << "Heals for " + s + "."; break;
      case Energize: parts << "Restores " + s + " " + (e.misc == 1 ? "rage" : e.misc == 3 ? "energy" : "mana") + "."; break;
      case SummonWild: case Summon: parts << "Summons a companion for $d."; break;
      case ApplyAura:
        switch (e.aura) {
          case PeriodicDamage: parts << "Deals " + o + " " + school + " damage over $d."; break;
          case PeriodicHeal: parts << "Heals " + o + " over $d."; break;
          case Stat: parts << QString(e.minValue < 0 ? "Lowers" : "Raises") + " " + (e.misc == -1 ? QString("all stats") : stats().value(e.misc + 1).label) + " by " + s + " for $d."; break;
          case Resistance: parts << QString(e.minValue < 0 ? "Lowers armor by " : "Raises armor by ") + s + " for $d."; break;
          case Stun: parts << "Stuns the target for $d."; break;
          case Root: parts << "Roots the target in place for $d."; break;
          case Silence: parts << "Silences the target for $d."; break;
          case Fear: parts << "Causes the target to flee in fear for $d."; break;
          default: break;
        }
        break;
      default: break;
    }
  }
  return parts.join(' ');
}
}
