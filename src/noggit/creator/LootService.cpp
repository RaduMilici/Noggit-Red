#include "LootService.hpp"
#include "Database.hpp"
#include <QMap>
#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
QString n(qint64 value) { return QString::number(value); }
void require(bool condition, QString const& message) { if (!condition) throw std::runtime_error(message.toStdString()); }
bool npc(LootOwner const& owner) { return owner.kind == LootOwner::Kind::Npc; }
QString lootTable(LootOwner const& owner) { return npc(owner) ? "creature_loot_template" : "gameobject_loot_template"; }
bool chestType(int type) { return type == 3 || type == 25; }
LootRow fromFields(Fields const& f) {
  LootRow r;
  r.item = f["item"].toUInt(); r.chance = f["ChanceOrQuestChance"].toDouble(); r.group = f["groupid"].toInt();
  auto count = f["mincountOrRef"].toInt();
  if (count < 0) r.reference = Id(-count); else r.minCount = count;
  r.maxCount = f["maxcount"].toInt(); r.condition = f["condition_id"].toUInt();
  return r;
}
QVector<LootRow> rowsOf(QVector<Fields> const& fields) { QVector<LootRow> rows; for (auto const& f : fields) rows.push_back(fromFields(f)); return rows; }
Fields toFields(LootRow const& r, Id entry) {
  return {{"entry", entry}, {"item", r.item}, {"ChanceOrQuestChance", r.chance}, {"groupid", r.group},
          {"mincountOrRef", r.reference ? -qint64(r.reference) : qint64(r.minCount)}, {"maxcount", r.maxCount}, {"condition_id", r.condition}};
}
}
QVector<Choice> LootService::owners(QString const& text, LootOwner::Kind kind, bool own) {
  Database db;
  QVector<Choice> out;
  auto like = db.quote('%' + text + '%');
  auto rows = kind == LootOwner::Kind::Npc
    ? db.query("SELECT entry,name,CONCAT('Level ',level_min,'–',level_max) AS detail FROM creature_template WHERE "
               + QString(own ? "entry IN (SELECT entry FROM creator_content WHERE kind='npc')" : "loot_id>0") + " AND name LIKE " + like + " ORDER BY name LIMIT 250")
    : db.query("SELECT entry,name,IF(type=3,'Chest','Fishing hole') AS detail FROM gameobject_template WHERE type IN (3,25) AND "
               + QString(own ? "entry IN (SELECT entry FROM creator_content WHERE kind='gameobject')" : "data1>0") + " AND name LIKE " + like + " ORDER BY name LIMIT 250");
  for (auto const& r : rows) out.push_back({r["entry"].toUInt(), r["name"].toString(), r["detail"].toString()});
  return out;
}
LootTable LootService::load(LootOwner const& owner) {
  Database db;
  LootTable t; t.owner = owner;
  Id loot = 0;
  if (npc(owner)) {
    auto rows = db.query("SELECT name,display_id1,loot_id,gold_min,gold_max FROM creature_template WHERE entry=" + n(owner.entry));
    require(!rows.isEmpty(), "This NPC no longer exists.");
    auto r = rows[0];
    t.ownerName = r["name"].toString(); t.display = r["display_id1"].toUInt(); loot = r["loot_id"].toUInt();
    t.moneyMin = r["gold_min"].toLongLong(); t.moneyMax = r["gold_max"].toLongLong();
    t.editable = db.owned("npc", owner.entry);
  } else {
    auto rows = db.query("SELECT name,displayId,type,data1,mingold,maxgold FROM gameobject_template WHERE entry=" + n(owner.entry));
    require(!rows.isEmpty(), "This object no longer exists.");
    auto r = rows[0];
    t.ownerName = r["name"].toString(); t.display = r["displayId"].toUInt(); t.lootable = chestType(r["type"].toInt());
    loot = t.lootable ? r["data1"].toUInt() : 0;
    t.moneyMin = r["mingold"].toLongLong(); t.moneyMax = r["maxgold"].toLongLong();
    t.editable = db.owned("gameobject", owner.entry);
  }
  if (loot) t.rows = rowsOf(db.query("SELECT * FROM " + lootTable(owner) + " WHERE entry=" + n(loot) + " ORDER BY groupid,item"));
  QString what = npc(owner) ? "NPC" : "object";
  if (!t.editable) t.notice = "This is an original " + what + ": its loot is shown read-only. Copy it to one of your own " + what + "s to change it.";
  else if (!t.lootable) t.notice = "Only chests and fishing holes hold loot. Change this object's type to Chest in its editor first.";
  else if (loot && loot != owner.entry) t.notice = "This " + what + " still uses the loot of the one it was copied from. Saving gives it its own copy; the original stays unchanged.";
  return t;
}
QVector<RowProblem> LootService::validate(LootTable const& t) {
  QStringList items, references;
  for (auto const& r : t.rows) (r.reference ? references : items) << n(r.reference ? r.reference : r.item);
  items.removeDuplicates(); references.removeDuplicates();
  QSet<Id> existingItems, existingReferences;
  Database db;
  if (!items.isEmpty()) for (auto const& r : db.query("SELECT entry FROM item_template WHERE entry IN (" + items.join(',') + ")")) existingItems.insert(r["entry"].toUInt());
  if (!references.isEmpty()) for (auto const& r : db.query("SELECT DISTINCT entry FROM reference_loot_template WHERE entry IN (" + references.join(',') + ")")) existingReferences.insert(r["entry"].toUInt());
  return check(t, existingItems, existingReferences);
}
QVector<RowProblem> LootService::check(LootTable const& t, QSet<Id> const& existingItems, QSet<Id> const& existingReferences) {
  QVector<RowProblem> problems;
  auto add = [&](int row, QString const& text, bool error = true) { problems.push_back({row, text, error}); };
  if (t.moneyMin < 0 || t.moneyMax < 0) add(-1, "Money cannot be negative.");
  else if (t.moneyMin > t.moneyMax) add(-1, "The minimum money is higher than the maximum.");
  QMap<int, double> explicitSum; QMap<int, int> equalCount; QMap<QString, int> seen;
  for (int i = 0; i < t.rows.size(); ++i) {
    auto const& r = t.rows[i];
    if (!r.reference && !r.item) add(i, "Choose an item.");
    else if (!r.reference && !existingItems.contains(r.item)) add(i, "This item does not exist in the world database.");
    if (r.reference && !existingReferences.contains(r.reference)) add(i, QString("Shared loot table %1 does not exist.").arg(r.reference));
    if (!r.quest() && (r.chance > 100 || !std::isfinite(r.chance))) add(i, "The chance must be between 0% and 100%.");
    if (r.quest() && r.chance < -100) add(i, "A quest drop's chance must be at most 100%.");
    if (r.reference && r.chance == 0) add(i, "A shared-table reference needs a nonzero chance, even when it selects a group.");
    if (r.chance == 0 && r.group == 0) add(i, "A 0% item outside a group never drops. Give it a chance, or put it in a group.");
    if (r.group < 0 || r.group > 127) add(i, "Groups are numbered 1 to 127.");
    if (!r.reference && r.minCount < 1) add(i, "The quantity must be at least 1.");
    if (r.maxCount < 1 || r.maxCount > 255) add(i, r.reference ? "A shared table is rolled 1 to 255 times." : "At most 255 can drop at once.");
    if (!r.reference && r.minCount > r.maxCount) add(i, "The minimum quantity is greater than the maximum.");
    // The database keeps one row per item (per group for NPCs).
    auto key = n(r.item) + (t.owner.kind == LootOwner::Kind::Npc ? "/" + n(r.group) : QString());
    if (seen.contains(key)) add(i, "This item is already in the loot" + QString(t.owner.kind == LootOwner::Kind::Npc && r.group ? " in this group." : "."));
    else seen[key] = i;
    if (!r.reference && r.group > 0 && !r.quest()) { if (r.chance > 0) explicitSum[r.group] += r.chance; else if (r.chance == 0) ++equalCount[r.group]; }
  }
  for (int i = 0; i < t.rows.size(); ++i) {
    auto const& r = t.rows[i];
    if (r.reference || r.group <= 0 || r.quest()) continue;
    auto sum = explicitSum.value(r.group);
    if (sum > 100.0001) add(i, QString("Group %1's chances add up to %2%; at most 100% can drop.").arg(r.group).arg(sum, 0, 'g', 4));
    else if (r.chance == 0 && sum >= 99.9999) add(i, QString("Group %1's other items already use 100%, so this one never drops.").arg(r.group));
  }
  return problems;
}
double LootService::expectedChance(QVector<LootRow> const& rows, int index) {
  auto const& r = rows.value(index);
  if (r.reference || r.group <= 0) return std::clamp(std::abs(r.chance), 0.0, 100.0) / 100;
  // Explicit chances are taken in order and cut off at 100%; equal-chance items share what is left.
  double before = 0, sum = 0; int equal = 0;
  for (int i = 0; i < rows.size(); ++i) {
    if (rows[i].reference || rows[i].group != r.group) continue;
    double chance = std::abs(rows[i].chance);
    if (chance > 0) { if (i < index) before += chance; sum += chance; } else ++equal;
  }
  if (r.chance != 0) return std::max(0.0, std::min(before + std::abs(r.chance), 100.0) - std::min(before, 100.0)) / 100;
  return equal ? std::max(0.0, 100 - sum) / 100 / equal : 0;
}
LootSimulation LootService::simulate(LootTable const& t, QHash<Id, QVector<LootRow>> const& references, int kills, quint32 seed) {
  LootSimulation s; s.kills = std::max(0, kills);
  std::mt19937 random(seed);
  std::uniform_real_distribution<double> percent(0, 100);
  auto between = [&](int a, int b) { return std::uniform_int_distribution<int>(std::min(a, b), std::max(a, b))(random); };
  QHash<Id, int> index;
  auto slot = [&](Id item) -> LootSimulation::Item& {
    if (!index.contains(item)) { index[item] = s.items.size(); s.items.push_back({item}); }
    return s.items[index[item]];
  };
  for (int i = 0; i < t.rows.size(); ++i) if (!t.rows[i].reference && t.rows[i].item) slot(t.rows[i].item).expected = expectedChance(t.rows, i);
  QHash<Id, qint64> dropped;
  std::function<void(QVector<LootRow> const&, int, int)> roll = [&](QVector<LootRow> const& rows, int depth, int selectedGroup) {
    auto give = [&](LootRow const& r) {
      if (r.reference) {
        if (depth < 8 && references.contains(r.reference)) for (int k = 0; k < std::max(1, r.maxCount); ++k) roll(references[r.reference], depth + 1, r.group);
        return;
      }
      if (r.item) dropped[r.item] += between(std::max(1, r.minCount), std::max(1, r.maxCount));
    };
    QMap<int, QVector<LootRow const*>> groups;
    for (auto const& r : rows) {
      // A reference's group selects a group in the referenced table; it is not
      // a member of the parent's group. A group-only roll skips plain rows and references.
      if (selectedGroup && (r.reference || r.group != selectedGroup)) continue;
      if (!r.reference && r.group > 0) { groups[r.group].push_back(&r); continue; }
      auto chance = r.quest() ? -r.chance : r.chance; // quest drops: as if the quest were active
      if (chance >= 100 || percent(random) < chance) give(r);
    }
    for (auto const& group : groups) {
      QVector<LootRow const*> equal; LootRow const* picked = nullptr;
      double left = percent(random);
      for (auto const* r : group) {
        double chance = std::abs(r->chance);
        if (chance == 0) { equal.push_back(r); continue; }
        if (!picked && (chance >= 100 || (left -= chance) < 0)) picked = r;
      }
      if (!picked && !equal.isEmpty()) picked = equal[between(0, equal.size() - 1)];
      if (picked) give(*picked);
    }
  };
  double money = 0;
  for (int k = 0; k < s.kills; ++k) {
    dropped.clear();
    roll(t.rows, 0, 0);
    qint64 coins = t.moneyMax > 0 ? std::uniform_int_distribution<qint64>(std::min(t.moneyMin, t.moneyMax), t.moneyMax)(random) : 0;
    money += coins;
    if (dropped.isEmpty() && !coins) ++s.empty;
    for (auto it = dropped.begin(); it != dropped.end(); ++it) { auto& item = slot(it.key()); ++item.kills; item.count += it.value(); }
  }
  s.averageMoney = s.kills ? money / s.kills : 0;
  return s;
}
QHash<Id, QVector<LootRow>> LootService::references(LootTable const& t) {
  QHash<Id, QVector<LootRow>> out;
  QVector<LootRow> pending = t.rows;
  Database db;
  for (int depth = 0; depth < 8 && !pending.isEmpty(); ++depth) {
    QStringList ids;
    for (auto const& r : pending) if (r.reference && !out.contains(r.reference)) ids << n(r.reference);
    ids.removeDuplicates();
    pending.clear();
    if (ids.isEmpty()) break;
    for (auto const& f : db.query("SELECT * FROM reference_loot_template WHERE entry IN (" + ids.join(',') + ")")) {
      auto row = fromFields(f);
      out[f["entry"].toUInt()].push_back(row); pending.push_back(row);
    }
    for (auto const& id : ids) if (!out.contains(id.toUInt())) out[id.toUInt()] = {};
  }
  return out;
}
void LootService::save(LootTable const& t) {
  auto const& owner = t.owner;
  for (auto const& p : validate(t)) require(!p.error, p.text);
  Database db;
  QString kind = npc(owner) ? "npc" : "gameobject", ownerTable = npc(owner) ? "creature_template" : "gameobject_template";
  require(db.owned(kind, owner.entry), npc(owner) ? "Original NPCs keep their loot. Copy it to one of your own NPCs to change it."
                                                  : "Original objects keep their loot. Copy it to one of your own objects to change it.");
  auto head = db.query("SELECT * FROM " + ownerTable + " WHERE entry=" + n(owner.entry));
  require(!head.isEmpty(), "This " + QString(npc(owner) ? "NPC" : "object") + " no longer exists.");
  if (!npc(owner)) require(chestType(head[0]["type"].toInt()), "Only chests and fishing holes hold loot. Change this object's type to Chest first.");
  Id current = npc(owner) ? head[0]["loot_id"].toUInt() : head[0]["data1"].toUInt();
  auto id = n(owner.entry), table = lootTable(owner);
  db.track(npc(owner) ? EntityType::Loot : EntityType::ObjectLoot, owner.entry);
  db.snapshotWhere(table, "entry=" + id);
  if (current && current != owner.entry) {
    // Becoming its own table: quest drops from the shared one keep working for this owner.
    db.exec("DELETE FROM " + table + " WHERE entry=" + id);
    for (auto row : db.query("SELECT * FROM " + table + " WHERE entry=" + n(current) + " AND ChanceOrQuestChance<0")) { row["entry"] = owner.entry; db.insert(table, row); }
  } else {
    db.exec("DELETE FROM " + table + " WHERE entry=" + id + " AND ChanceOrQuestChance>=0");
  }
  // Quest drops belong to their quests and are never written here.
  for (auto const& r : t.rows) if (!r.quest()) db.insert(table, toFields(r, owner.entry));
  db.snapshot(ownerTable, "entry", owner.entry);
  db.exec(npc(owner) ? "UPDATE creature_template SET loot_id=" + id + ",gold_min=" + n(t.moneyMin) + ",gold_max=" + n(t.moneyMax) + " WHERE entry=" + id
                     : "UPDATE gameobject_template SET data1=" + id + ",mingold=" + n(t.moneyMin) + ",maxgold=" + n(t.moneyMax) + " WHERE entry=" + id);
  db.commit();
}
}
