#include "ItemDesignService.hpp"
#include "Database.hpp"
#include <cmath>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
QString n(qint64 value) { return QString::number(value); }
void require(bool condition, QString const& message) { if (!condition) throw std::runtime_error(message.toStdString()); }
QStringList const resistanceColumns{"holy_res", "fire_res", "nature_res", "frost_res", "shadow_res", "arcane_res"};
using Template = ItemDesignService::Template;
// Inventory types (InventoryType in the client).
enum Slot { NonEquip = 0, Head = 1, Neck = 2, Shoulders = 3, Shirt = 4, Chest = 5, Waist = 6, Legs = 7, Feet = 8, Wrists = 9, Hands = 10,
  Finger = 11, Trinket = 12, OneHand = 13, Shield = 14, Ranged = 15, Back = 16, TwoHand = 17, Bag = 18, Tabard = 19, Robe = 20, MainHand = 21,
  OffHand = 22, Holdable = 23, Ammo = 24, Thrown = 25, RangedRight = 26, Relic = 28 };
}
QVector<ItemDesignService::TemplateInfo> const& ItemDesignService::templates() {
  static QVector<TemplateInfo> const list{
    {Template::Weapon, "Weapon", "A one-handed sword to start from: damage, speed and stats."},
    {Template::Armor, "Armor", "A leather chest piece: armor, stats and resistances."},
    {Template::Consumable, "Consumable", "A potion, food or scroll that casts a spell when used, then is gone."},
    {Template::QuestItem, "Quest Item", "Something a quest asks for. Bound to the player, cannot be sold."},
    {Template::CraftingMaterial, "Crafting Material", "Trade goods that stack, sold to and bought from vendors."},
    {Template::Key, "Key", "Opens a lock. Bound to the player, one per character."},
    {Template::Trinket, "Trinket", "An equipped trinket with an on-use or passive spell."}};
  return list;
}
bool ItemDesign::operator==(ItemDesign const& o) const { return ItemDesignService::toRow(*this) == ItemDesignService::toRow(o); }
ItemDesign ItemDesignService::fromRow(Fields const& row) {
  ItemDesign d; d.row = row;
  auto i = [&](QString const& c) { return row.value(c).toInt(); };
  d.entry = row.value("entry").toUInt();
  d.name = row.value("name").toString(); d.description = row.value("description").toString();
  d.quality = i("quality"); d.itemLevel = i("item_level"); d.requiredLevel = i("required_level");
  d.itemClass = i("class"); d.subclass = i("subclass"); d.inventoryType = i("inventory_type"); d.bonding = i("bonding");
  d.stackable = i("stackable"); d.maxCount = i("max_count"); d.buyCount = std::max(1, i("buy_count"));
  d.buyPrice = row.value("buy_price").toLongLong(); d.sellPrice = row.value("sell_price").toLongLong();
  d.display = row.value("display_id").toUInt();
  d.armor = i("armor"); d.block = i("block"); d.delay = i("delay"); d.durability = i("max_durability");
  d.damageMin = row.value("dmg_min1").toDouble(); d.damageMax = row.value("dmg_max1").toDouble(); d.damageType = i("dmg_type1");
  for (int k = 1; k <= 10; ++k) if (auto value = i("stat_value" + n(k))) d.stats.push_back({i("stat_type" + n(k)), value});
  for (int k = 0; k < 6; ++k) d.resistances[k] = i(resistanceColumns[k]);
  d.classes = i("allowable_class"); d.races = i("allowable_race");
  for (int k = 0; k < 5; ++k) {
    auto c = [&](QString const& name) { return name + "_" + n(k + 1); };
    auto& s = d.spells[k];
    s.spell = row.value(c("spellid")).toUInt(); s.trigger = i(c("spelltrigger")); s.charges = i(c("spellcharges"));
    s.perMinute = row.value(c("spellppmrate")).toDouble(); s.cooldown = i(c("spellcooldown"));
    s.category = i(c("spellcategory")); s.categoryCooldown = i(c("spellcategorycooldown"));
  }
  return d;
}
Fields ItemDesignService::toRow(ItemDesign const& d) {
  Fields row = d.row;
  auto set = [&](QString const& column, QVariant const& value) { row[column] = value.toString(); };
  if (d.entry) set("entry", d.entry);
  set("name", d.name.trimmed()); set("description", d.description.trimmed());
  set("quality", d.quality); set("item_level", d.itemLevel); set("required_level", d.requiredLevel);
  set("class", d.itemClass); set("subclass", d.subclass); set("inventory_type", d.inventoryType); set("bonding", d.bonding);
  set("stackable", d.stackable); set("max_count", d.maxCount); set("buy_count", d.buyCount);
  set("buy_price", d.buyPrice); set("sell_price", d.sellPrice); set("display_id", d.display);
  set("armor", d.armor); set("block", d.block); set("delay", d.delay); set("max_durability", d.durability);
  set("dmg_min1", d.damageMin); set("dmg_max1", d.damageMax); set("dmg_type1", d.damageType);
  for (int k = 0; k < 10; ++k) {
    set("stat_type" + n(k + 1), k < d.stats.size() ? d.stats[k].first : 0);
    set("stat_value" + n(k + 1), k < d.stats.size() ? d.stats[k].second : 0);
  }
  for (int k = 0; k < 6; ++k) set(resistanceColumns[k], d.resistances[k]);
  set("allowable_class", d.classes); set("allowable_race", d.races);
  for (int k = 0; k < 5; ++k) {
    auto c = [&](QString const& name) { return name + "_" + n(k + 1); };
    auto const& s = d.spells[k];
    set(c("spellid"), s.spell); set(c("spelltrigger"), s.spell ? s.trigger : 0); set(c("spellcharges"), s.spell ? s.charges : 0);
    set(c("spellppmrate"), s.spell ? s.perMinute : 0); set(c("spellcooldown"), s.spell ? s.cooldown : -1);
    set(c("spellcategory"), s.spell ? s.category : 0); set(c("spellcategorycooldown"), s.spell ? s.categoryCooldown : -1);
  }
  return row;
}
void ItemDesignService::apply(Template kind, ItemDesign& d) {
  d.stats.clear(); d.resistances = {}; d.spells = {}; d.armor = d.block = 0; d.damageMin = d.damageMax = 0; d.delay = 0; d.durability = 0;
  d.maxCount = 0; d.stackable = 1; d.classes = d.races = -1; d.damageType = 0;
  auto material = [&](int m, int sheath = 0) { d.row["material"] = n(m); d.row["sheath"] = n(sheath); };
  switch (kind) {
    case Template::Weapon:
      d.itemClass = 2; d.subclass = 7; d.inventoryType = OneHand; d.bonding = 2; d.quality = 2; d.itemLevel = 20; d.requiredLevel = 15;
      d.delay = 2600; d.damageMin = 18; d.damageMax = 34; d.durability = 75; d.stats = {{4, 3}, {7, 2}}; d.buyPrice = 18000; d.sellPrice = 3600;
      material(1, 3); break;
    case Template::Armor:
      d.itemClass = 4; d.subclass = 2; d.inventoryType = Chest; d.bonding = 2; d.quality = 2; d.itemLevel = 20; d.requiredLevel = 15;
      d.armor = 90; d.durability = 85; d.stats = {{3, 4}, {7, 3}}; d.buyPrice = 12000; d.sellPrice = 2400; material(8); break;
    case Template::Consumable:
      d.itemClass = 0; d.subclass = 0; d.inventoryType = NonEquip; d.bonding = 0; d.quality = 1; d.itemLevel = 15; d.requiredLevel = 10;
      d.stackable = 5; d.buyPrice = 500; d.sellPrice = 100; d.spells[0] = {0, ItemSpell::Use, -1, 0, -1, 0, -1}; material(-1); break;
    case Template::QuestItem:
      d.itemClass = 12; d.subclass = 0; d.inventoryType = NonEquip; d.bonding = 4; d.quality = 1; d.itemLevel = 1; d.requiredLevel = 0;
      d.stackable = 20; d.buyPrice = d.sellPrice = 0; material(-1); break;
    case Template::CraftingMaterial:
      d.itemClass = 7; d.subclass = 0; d.inventoryType = NonEquip; d.bonding = 0; d.quality = 1; d.itemLevel = 10; d.requiredLevel = 0;
      d.stackable = 20; d.buyPrice = 400; d.sellPrice = 100; material(-1); break;
    case Template::Key:
      d.itemClass = 13; d.subclass = 0; d.inventoryType = NonEquip; d.bonding = 1; d.quality = 1; d.itemLevel = 1; d.requiredLevel = 0;
      d.maxCount = 1; d.buyPrice = d.sellPrice = 0; material(-1); break;
    case Template::Trinket:
      d.itemClass = 4; d.subclass = 0; d.inventoryType = Trinket; d.bonding = 1; d.quality = 3; d.itemLevel = 40; d.requiredLevel = 35;
      d.buyPrice = 40000; d.sellPrice = 10000; d.spells[0] = {0, ItemSpell::Use, 0, 0, -1, 0, -1}; material(4); break;
  }
}
QVector<QPair<int, QString>> ItemDesignService::slotsFor(int itemClass, int subclass) {
  QVector<int> wearable;
  if (itemClass == 2) {
    if (QSet<int>{0, 4, 7, 13, 15}.contains(subclass)) wearable = {OneHand, MainHand, OffHand};
    else if (QSet<int>{1, 5, 6, 8, 10, 17, 20}.contains(subclass)) wearable = {TwoHand};
    else if (subclass == 2) wearable = {Ranged};
    else if (subclass == 3 || subclass == 18 || subclass == 19) wearable = {RangedRight};
    else if (subclass == 16) wearable = {Thrown};
    else wearable = {OneHand, MainHand, OffHand, TwoHand};
  } else if (itemClass == 4) {
    if (subclass == 0) wearable = {Neck, Finger, Trinket, Holdable, Shirt, Tabard, Head, Back};
    else if (subclass == 6) wearable = {Shield};
    else if (subclass >= 7) wearable = {Relic};
    else wearable = {Head, Shoulders, Chest, Robe, Waist, Legs, Feet, Wrists, Hands, Back};
  } else if (itemClass == 1 || itemClass == 11) wearable = {Bag};
  else if (itemClass == 6) wearable = {Ammo};
  else wearable = {NonEquip};
  QVector<QPair<int, QString>> out;
  for (auto slot : wearable) out.push_back({slot, slot == NonEquip ? QString("Not worn") : ItemService::slotName(slot)});
  return out;
}
QVector<QPair<int, QString>> ItemDesignService::classMask() {
  return {{1, "Warrior"}, {2, "Paladin"}, {4, "Hunter"}, {8, "Rogue"}, {16, "Priest"}, {64, "Shaman"}, {128, "Mage"}, {256, "Warlock"}, {1024, "Druid"}};
}
QVector<QPair<int, QString>> ItemDesignService::raceMask() {
  // Vanilla's races, and the two Turtle WoW adds (Goblin, High Elf).
  QStringList names{"Human", "Orc", "Dwarf", "Night Elf", "Undead", "Tauren", "Gnome", "Troll", "Goblin", "High Elf"};
  QVector<QPair<int, QString>> out;
  for (int race = 1; race <= names.size(); ++race) out.push_back({1 << (race - 1), names[race - 1]});
  return out;
}
QVector<ItemProblem> ItemDesignService::check(ItemDesign const& d, ItemFacts const& f) {
  QVector<ItemProblem> p;
  auto add = [&](QString const& field, QString const& text, bool error = true) { p.push_back({field, text, error}); };
  if (d.name.trimmed().isEmpty()) add("Basics", "Name the item.");
  else if (d.name.size() > 100) add("Basics", "Keep the name under 100 characters.");
  if (d.quality < 0 || d.quality > 6) add("Basics", "Choose a quality.");
  if (d.itemLevel < 0 || d.itemLevel > 255) add("Basics", "The item level is 0 to 255.");
  if (d.requiredLevel < 0 || d.requiredLevel > 60) add("Basics", "The required level is 0 (none) to 60.");
  if (!d.display) add("Basics", "Choose a look: without one the item shows a question mark.", false);
  else if (!f.displays.isEmpty() && !f.displays.contains(d.display)) add("Basics", "The client has no such look. Choose the look of another item.");
  bool slotOk = false;
  for (auto const& s : slotsFor(d.itemClass, d.subclass)) slotOk = slotOk || s.first == d.inventoryType;
  if (!slotOk) add("Basics", d.inventoryType ? "This kind of item cannot be worn in that slot." : "Choose where it is worn.");
  if (d.stackable < 1 || d.stackable > 1000) add("Basics", "A stack is 1 to 1000 items.");
  if (equippable(d) && d.stackable > 1) add("Basics", "Worn items do not stack.");
  if (d.maxCount < 0) add("Basics", "The carry limit cannot be negative.");
  if (d.buyPrice < 0 || d.sellPrice < 0) add("Basics", "Prices cannot be negative.");
  else if (d.sellPrice > d.buyPrice && d.buyPrice > 0) add("Basics", "It sells to vendors for more than it costs: players can make money buying and selling it.", false);
  if (d.bonding == 4 && d.sellPrice > 0) add("Basics", "Quest items cannot be sold, so the sell price is never used.", false);
  if (weapon(d)) {
    if (d.delay < 500 || d.delay > 10000) add("Combat", "A weapon's speed is 0.5 to 10 seconds.");
    if (d.damageMin <= 0 || d.damageMax < d.damageMin) add("Combat", "Set the weapon's damage (the maximum at least the minimum).");
  } else if (d.damageMax > 0 && d.itemClass != 6) add("Combat", "Only weapons and ammunition deal damage.", false);
  if (d.armor < 0 || d.block < 0) add("Combat", "Armor and block cannot be negative.");
  if (equippable(d) && (weapon(d) || (d.itemClass == 4 && d.subclass >= 1 && d.subclass <= 6)) && d.durability <= 0)
    add("Combat", "Weapons and armor usually have durability, or they never need repair.", false);
  if (d.stats.size() > 10) add("Stats", "An item has at most 10 stats.");
  QSet<int> seen;
  for (auto const& [type, value] : d.stats) {
    if (!value) add("Stats", "A stat without a value does nothing: remove it.", false);
    if (seen.contains(type)) add("Stats", ItemService::statName(type) + " is listed twice.", false);
    seen.insert(type);
  }
  if (!equippable(d) && (!d.stats.isEmpty() || d.armor)) add("Stats", "Stats and armor only work on items that are worn.", false);
  for (auto r : d.resistances) if (r < -255 || r > 255) add("Stats", "Resistances are -255 to 255.");
  if (d.classes == 0 || d.races == 0) add("Requirements", "No class or no race can use it. Allow at least one, or everyone.");
  int used = 0;
  for (int k = 0; k < 5; ++k) {
    auto const& s = d.spells[k];
    if (!s.spell) continue;
    ++used;
    if (!f.spells.contains(s.spell)) add("Spells", QString("Spell %1 does not exist.").arg(s.spell));
    if (s.trigger == ItemSpell::ChanceOnHit && !weapon(d)) add("Spells", "Chance on hit only works on weapons.", false);
    if (s.trigger == ItemSpell::Equip && !equippable(d)) add("Spells", "Equip effects only work on items that are worn.");
    if (s.trigger == ItemSpell::ChanceOnHit && (s.perMinute < 0 || s.perMinute > 60)) add("Spells", "Procs per minute is 0 to 60.");
  }
  if ((d.itemClass == 0) && !used) add("Spells", "A consumable does nothing until it casts a spell when used.", false);
  return p;
}
ItemInfo ItemDesignService::info(ItemDesign const& d, QStringList const& effects) {
  ItemInfo i;
  i.entry = d.entry; i.display = d.display; i.name = d.name.isEmpty() ? QString("New item") : d.name; i.description = d.description;
  i.quality = d.quality; i.itemClass = d.itemClass; i.subclass = d.subclass; i.inventoryType = d.inventoryType;
  i.itemLevel = d.itemLevel; i.requiredLevel = d.requiredLevel; i.buyPrice = d.buyPrice; i.sellPrice = d.sellPrice;
  i.buyCount = d.buyCount; i.stackable = d.stackable; i.maxCount = d.maxCount; i.bonding = d.bonding; i.armor = d.armor; i.block = d.block;
  i.delay = d.delay; i.durability = d.durability; i.damageMin = d.damageMin; i.damageMax = d.damageMax; i.damageType = d.damageType;
  i.stats = d.stats;
  for (int k = 0; k < 6; ++k) if (d.resistances[k]) i.resistances.push_back({k + 1, d.resistances[k]});
  i.effects = effects; i.own = true;
  return i;
}
namespace {
Fields defaults(Database& db) {
  Fields row;
  for (auto const& c : db.query("SHOW COLUMNS FROM item_template")) row[c["Field"].toString()] = c["Default"].isNull() ? QString() : c["Default"].toString();
  row.remove("script_name"); // NULL by default: left to the table
  return row;
}
}
ItemDesign ItemDesignService::load(Id entry) {
  Database db;
  auto rows = db.query("SELECT * FROM item_template WHERE entry=" + n(entry));
  require(!rows.isEmpty(), QString("Item %1 does not exist.").arg(entry));
  auto d = fromRow(rows[0]);
  d.editable = db.owned("item", entry);
  if (!d.editable) d.notice = "This is a game item: it is shown read-only. Clone it to make your own version.";
  return d;
}
ItemDesign ItemDesignService::blank(Template kind) {
  Database db;
  auto d = fromRow(defaults(db));
  d.entry = 0; d.editable = true;
  apply(kind, d);
  d.name = "New " + [&] { for (auto const& t : templates()) if (t.id == kind) return t.label.toLower(); return QString("item"); }();
  // The look most of the game's items of this kind have.
  auto look = db.query("SELECT display_id FROM item_template WHERE class=" + n(d.itemClass) + " AND subclass=" + n(d.subclass) + " AND inventory_type=" + n(d.inventoryType)
                       + " AND display_id>0 GROUP BY display_id ORDER BY COUNT(*) DESC LIMIT 1");
  if (!look.isEmpty()) d.display = look[0]["display_id"].toUInt();
  return d;
}
ItemDesign ItemDesignService::clone(Id source) {
  auto d = load(source);
  d.source = source; d.entry = 0; d.row["entry"] = "0"; d.editable = true; d.notice.clear();
  d.row["start_quest"] = "0"; // a copy does not start the original's quest
  d.row.remove("script_name");
  return d;
}
ItemFacts ItemDesignService::facts(ItemDesign const& d) {
  ItemFacts f;
  QStringList spells;
  for (auto const& s : d.spells) if (s.spell) spells << n(s.spell);
  Database db;
  if (!spells.isEmpty()) for (auto const& r : db.query("SELECT entry FROM spell_template WHERE entry IN (" + spells.join(',') + ")")) f.spells.insert(r["entry"].toUInt());
  return f;
}
QVector<ItemProblem> ItemDesignService::validate(ItemDesign const& d) { return check(d, facts(d)); }
QString ItemDesignService::effectText(ItemSpell const& s) {
  if (!s.spell) return {};
  Database db;
  auto rows = db.query("SELECT name,description,effectBasePoints1,effectBasePoints2,effectBasePoints3,effectDieSides1,effectDieSides2,effectDieSides3 FROM spell_template WHERE entry=" + n(s.spell));
  QString text = rows.isEmpty() ? QString("(unknown spell %1)").arg(s.spell) : describeSpell(rows[0]);
  if (text.trimmed().isEmpty() && !rows.isEmpty()) text = rows[0]["name"].toString();
  return QString(s.trigger == ItemSpell::Equip ? "Equip: " : s.trigger == ItemSpell::ChanceOnHit ? "Chance on hit: " : "Use: ") + text;
}
QStringList ItemDesignService::uses(Id entry) {
  Database db; QStringList out; auto id = n(entry);
  for (auto const& r : db.query("SELECT Title FROM quest_template WHERE " + id + " IN (SrcItemId,ReqItemId1,ReqItemId2,ReqItemId3,ReqItemId4,RewItemId1,RewItemId2,RewItemId3,RewItemId4,"
                                "RewChoiceItemId1,RewChoiceItemId2,RewChoiceItemId3,RewChoiceItemId4,RewChoiceItemId5,RewChoiceItemId6) LIMIT 5")) out << "Quest: " + r["Title"].toString();
  for (auto const& [table, owner] : std::initializer_list<std::pair<char const*, char const*>>{{"creature_loot_template", "Loot of"}, {"gameobject_loot_template", "Loot of object"}, {"npc_vendor", "Sold by"}})
    if (auto count = db.query("SELECT COUNT(*) AS c FROM " + QString(table) + " WHERE item=" + id)[0]["c"].toInt()) out << QString("%1 %2 owner(s)").arg(owner).arg(count);
  for (auto const& r : db.query("SELECT t.name FROM creature_equip_template e JOIN creature_template t ON t.equipment_id=e.entry WHERE " + id + " IN (e.equipentry1,e.equipentry2,e.equipentry3) LIMIT 5"))
    out << "Carried by NPC: " + r["name"].toString();
  for (auto const& r : db.query("SELECT name FROM spell_template WHERE (effect1=24 AND effectItemType1=" + id + ") OR (effect2=24 AND effectItemType2=" + id + ") OR (effect3=24 AND effectItemType3=" + id + ") LIMIT 5"))
    out << "Created by spell: " + r["name"].toString();
  return out;
}
Id ItemDesignService::save(ItemDesign const& d) {
  for (auto const& p : validate(d)) require(!p.error, p.text);
  Database db;
  Id entry = d.entry;
  if (entry) require(db.owned("item", entry), "Game items are read-only here. Clone it to make your own version.");
  else entry = db.allocate("item_template", "entry", 0xffffff);
  auto row = toRow(d); row["entry"] = n(entry);
  db.track(EntityType::Item, entry);
  db.snapshot("item_template", "entry", entry);
  db.exec("DELETE FROM item_template WHERE entry=" + n(entry));
  db.insert("item_template", row);
  db.snapshotWhere("creator_content", "kind='item' AND entry=" + n(entry));
  db.mark("item", entry);
  db.commit();
  return entry;
}
void ItemDesignService::remove(Id entry) {
  auto used = uses(entry);
  require(used.isEmpty(), "This item is still used:\n" + used.join('\n') + "\nRemove those uses first.");
  Database db;
  require(db.owned("item", entry), "Only items made in Noggit can be deleted.");
  db.track(EntityType::Item, entry);
  db.snapshot("item_template", "entry", entry);
  db.exec("DELETE FROM item_template WHERE entry=" + n(entry));
  db.snapshotWhere("creator_content", "kind='item' AND entry=" + n(entry));
  db.exec("DELETE FROM creator_content WHERE kind='item' AND entry=" + n(entry));
  db.commit();
}
}
