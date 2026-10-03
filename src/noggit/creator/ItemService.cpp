#include "ItemService.hpp"
#include "Database.hpp"
#include <noggit/runtime/RuntimeManager.hpp>
#include <QRegularExpression>
#include <QSettings>
#include <algorithm>
namespace Noggit::Creator {
namespace {
QString n(qint64 value) { return QString::number(value); }
QString const columns =
  "entry,name,description,display_id,quality,class,subclass,inventory_type,item_level,required_level,buy_price,sell_price,"
  "buy_count,stackable,max_count,bonding,armor,block,delay,max_durability,container_slots,dmg_min1,dmg_max1,dmg_type1,"
  "stat_type1,stat_value1,stat_type2,stat_value2,stat_type3,stat_value3,stat_type4,stat_value4,stat_type5,stat_value5,"
  "stat_type6,stat_value6,stat_type7,stat_value7,stat_type8,stat_value8,stat_type9,stat_value9,stat_type10,stat_value10,"
  "holy_res,fire_res,nature_res,frost_res,shadow_res,arcane_res,"
  "spellid_1,spelltrigger_1,spellid_2,spelltrigger_2,spellid_3,spelltrigger_3,spellid_4,spelltrigger_4,spellid_5,spelltrigger_5";
QString settingsPath() { return Runtime::RuntimeManager::instance()->root() + "/Workspace/runtime.ini"; }
QVector<ItemInfo> read(Database& db, QVector<Fields> const& rows) {
  QSet<Id> own;
  for (auto const& row : db.query("SELECT entry FROM creator_content WHERE kind='item'")) own.insert(row["entry"].toUInt());
  QSet<QString> spells;
  for (auto const& row : rows) for (int i = 1; i <= 5; ++i) if (auto spell = row["spellid_" + n(i)].toUInt()) spells.insert(n(spell));
  QHash<Id, Fields> spellRows;
  if (!spells.isEmpty())
    for (auto const& row : db.query("SELECT entry,name,description,effectBasePoints1,effectBasePoints2,effectBasePoints3,effectDieSides1,"
                                    "effectDieSides2,effectDieSides3 FROM spell_template WHERE entry IN (" + QStringList(spells.values()).join(',') + ")"))
      spellRows[row["entry"].toUInt()] = row;
  QVector<ItemInfo> items;
  for (auto const& r : rows) {
    ItemInfo i;
    i.entry = r["entry"].toUInt(); i.name = r["name"].toString(); i.description = r["description"].toString(); i.display = r["display_id"].toUInt();
    i.quality = r["quality"].toInt(); i.itemClass = r["class"].toInt(); i.subclass = r["subclass"].toInt(); i.inventoryType = r["inventory_type"].toInt();
    i.itemLevel = r["item_level"].toInt(); i.requiredLevel = r["required_level"].toInt();
    i.buyPrice = r["buy_price"].toLongLong(); i.sellPrice = r["sell_price"].toLongLong(); i.buyCount = std::max(1, r["buy_count"].toInt());
    i.stackable = r["stackable"].toInt(); i.maxCount = r["max_count"].toInt(); i.bonding = r["bonding"].toInt();
    i.armor = r["armor"].toInt(); i.block = r["block"].toInt(); i.delay = r["delay"].toInt(); i.durability = r["max_durability"].toInt();
    i.bagSlots = r["container_slots"].toInt();
    i.damageMin = r["dmg_min1"].toDouble(); i.damageMax = r["dmg_max1"].toDouble(); i.damageType = r["dmg_type1"].toInt();
    for (int s = 1; s <= 10; ++s)
      if (auto value = r["stat_value" + n(s)].toInt()) i.stats.push_back({r["stat_type" + n(s)].toInt(), value});
    QStringList schools{"holy_res", "fire_res", "nature_res", "frost_res", "shadow_res", "arcane_res"};
    for (int s = 0; s < schools.size(); ++s) if (auto value = r[schools[s]].toInt()) i.resistances.push_back({s + 1, value});
    for (int s = 1; s <= 5; ++s) {
      auto spell = r["spellid_" + n(s)].toUInt();
      if (!spell || !spellRows.contains(spell)) continue;
      auto trigger = r["spelltrigger_" + n(s)].toInt();
      auto text = describeSpell(spellRows[spell]);
      if (text.isEmpty()) text = spellRows[spell]["name"].toString();
      i.effects << (trigger == 1 ? "Equip: " : trigger == 2 ? "Chance on hit: " : "Use: ") + text;
    }
    i.own = own.contains(i.entry);
    items.push_back(i);
  }
  return items;
}
}
QString describeSpell(Fields const& spell) {
  auto text = spell["description"].toString();
  // $s1..$s3 are the effect amounts; other tokens (durations, ranges) need client tables, so they are dropped.
  for (int e = 1; e <= 3; ++e) {
    auto base = spell["effectBasePoints" + n(e)].toInt() + 1, dice = spell["effectDieSides" + n(e)].toInt();
    QString amount = dice > 1 ? QString("%1 to %2").arg(base).arg(base + dice - 1) : n(std::abs(base));
    text.replace(QRegularExpression("\\$(?:\\d+)?s" + n(e)), amount);
  }
  text.remove(QRegularExpression("\\$[a-zA-Z]+\\d*"));
  return text.simplified();
}
QString formatMoney(qint64 copper) {
  if (copper <= 0) return "0c";
  QStringList parts;
  if (auto g = copper / 10000) parts << n(g) + "g";
  if (auto s = copper / 100 % 100) parts << n(s) + "s";
  if (auto c = copper % 100) parts << n(c) + "c";
  return parts.join(' ');
}
QVector<ItemInfo> ItemService::search(ItemFilter const& f) {
  Database db;
  QStringList where{"1=1"};
  auto text = f.text.trimmed();
  bool numeric = false; auto entry = text.toUInt(&numeric);
  if (numeric) where << "entry=" + n(entry);
  else if (!text.isEmpty()) where << "name LIKE " + db.quote('%' + text + '%');
  if (f.quality >= 0) where << "quality=" + n(f.quality);
  if (f.itemClass >= 0) where << "class=" + n(f.itemClass);
  if (f.subclass >= 0) where << "subclass=" + n(f.subclass);
  if (f.inventoryType >= 0) where << "inventory_type=" + n(f.inventoryType);
  if (f.minLevel > 0) where << "required_level>=" + n(f.minLevel);
  if (f.maxLevel > 0) where << "required_level<=" + n(f.maxLevel);
  QVector<Id> recentItems;
  if (f.scope == ItemFilter::Scope::Project) where << "entry IN (SELECT entry FROM creator_content WHERE kind='item')";
  if (f.scope == ItemFilter::Scope::Recent) {
    recentItems = recent();
    if (recentItems.isEmpty()) return {};
    QStringList ids; for (auto id : recentItems) ids << n(id);
    where << "entry IN (" + ids.join(',') + ")";
  }
  auto items = read(db, db.query("SELECT " + columns + " FROM item_template WHERE " + where.join(" AND ")
                                 + " ORDER BY name LIMIT " + n(std::clamp(f.limit, 1, 5000))));
  if (!recentItems.isEmpty())
    std::sort(items.begin(), items.end(), [&](ItemInfo const& a, ItemInfo const& b) { return recentItems.indexOf(a.entry) < recentItems.indexOf(b.entry); });
  return items;
}
std::optional<ItemInfo> ItemService::get(Id entry) {
  auto items = get(QVector<Id>{entry});
  if (!items.contains(entry)) return std::nullopt;
  return items[entry];
}
QHash<Id, ItemInfo> ItemService::get(QVector<Id> const& entries) {
  QHash<Id, ItemInfo> out;
  QStringList ids; for (auto id : entries) if (id) ids << n(id);
  ids.removeDuplicates();
  if (ids.isEmpty()) return out;
  Database db;
  for (auto const& item : read(db, db.query("SELECT " + columns + " FROM item_template WHERE entry IN (" + ids.join(',') + ")"))) out[item.entry] = item;
  return out;
}
QVector<Id> ItemService::recent() {
  QSettings settings(settingsPath(), QSettings::IniFormat);
  QVector<Id> out; for (auto const& value : settings.value("items/recent").toStringList()) if (auto id = value.toUInt()) out.push_back(id);
  return out;
}
void ItemService::used(Id entry) {
  if (!entry) return;
  QSettings settings(settingsPath(), QSettings::IniFormat);
  auto list = settings.value("items/recent").toStringList();
  list.removeAll(n(entry)); list.prepend(n(entry));
  while (list.size() > 40) list.removeLast();
  settings.setValue("items/recent", list);
}
QString ItemService::qualityName(int q) {
  static QStringList const names{"Poor", "Common", "Uncommon", "Rare", "Epic", "Legendary", "Artifact"};
  return names.value(q, "Quality " + n(q));
}
QVector<QPair<int, QString>> ItemService::classes() {
  return {{0, "Consumable"}, {1, "Container"}, {2, "Weapon"}, {4, "Armor"}, {5, "Reagent"}, {6, "Projectile"}, {7, "Trade Goods"},
          {9, "Recipe"}, {11, "Quiver"}, {12, "Quest"}, {13, "Key"}, {15, "Miscellaneous"}};
}
QString ItemService::className(int c) {
  for (auto const& [id, name] : classes()) if (id == c) return name;
  return "Class " + n(c);
}
QVector<QPair<int, QString>> ItemService::subclasses(int c) {
  if (c == 2) return {{0, "One-Handed Axe"}, {1, "Two-Handed Axe"}, {2, "Bow"}, {3, "Gun"}, {4, "One-Handed Mace"}, {5, "Two-Handed Mace"},
                      {6, "Polearm"}, {7, "One-Handed Sword"}, {8, "Two-Handed Sword"}, {10, "Staff"}, {13, "Fist Weapon"},
                      {14, "Miscellaneous"}, {15, "Dagger"}, {16, "Thrown"}, {18, "Crossbow"}, {19, "Wand"}, {20, "Fishing Pole"}};
  if (c == 4) return {{0, "Miscellaneous"}, {1, "Cloth"}, {2, "Leather"}, {3, "Mail"}, {4, "Plate"}, {6, "Shield"}};
  if (c == 0) return {{0, "Consumable"}};
  if (c == 6) return {{2, "Arrow"}, {3, "Bullet"}};
  if (c == 9) return {{0, "Book"}, {1, "Leatherworking"}, {2, "Tailoring"}, {3, "Engineering"}, {4, "Blacksmithing"}, {5, "Cooking"},
                      {6, "Alchemy"}, {7, "First Aid"}, {8, "Enchanting"}, {9, "Fishing"}};
  return {};
}
QString ItemService::subclassName(int c, int s) {
  for (auto const& [id, name] : subclasses(c)) if (id == s) return name;
  return {};
}
QVector<QPair<int, QString>> ItemService::equipSlots() {
  return {{0, "Not equippable"}, {1, "Head"}, {2, "Neck"}, {3, "Shoulder"}, {4, "Shirt"}, {5, "Chest"}, {6, "Waist"}, {7, "Legs"},
          {8, "Feet"}, {9, "Wrist"}, {10, "Hands"}, {11, "Finger"}, {12, "Trinket"}, {13, "One-Hand"}, {14, "Shield"}, {15, "Ranged"},
          {16, "Back"}, {17, "Two-Hand"}, {18, "Bag"}, {19, "Tabard"}, {20, "Chest (robe)"}, {21, "Main Hand"}, {22, "Off Hand"},
          {23, "Held In Off-hand"}, {24, "Ammo"}, {25, "Thrown"}, {26, "Ranged"}, {27, "Quiver"}, {28, "Relic"}};
}
QString ItemService::slotName(int t) {
  for (auto const& [id, name] : equipSlots()) if (id == t) return name;
  return {};
}
QString ItemService::statName(int s) {
  switch (s) {
    case 0: return "Mana"; case 1: return "Health"; case 3: return "Agility"; case 4: return "Strength";
    case 5: return "Intellect"; case 6: return "Spirit"; case 7: return "Stamina"; default: return "Stat " + n(s);
  }
}
QString ItemService::schoolName(int s) {
  static QStringList const names{"Physical", "Holy", "Fire", "Nature", "Frost", "Shadow", "Arcane"};
  return names.value(s);
}
QString ItemService::bondingText(int b) {
  switch (b) {
    case 1: return "Binds when picked up"; case 2: return "Binds when equipped"; case 3: return "Binds when used";
    case 4: return "Quest Item"; default: return {};
  }
}
}
