#include "VendorService.hpp"
#include "Database.hpp"
#include <QSet>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
QString n(qint64 value) { return QString::number(value); }
void require(bool condition, QString const& message) { if (!condition) throw std::runtime_error(message.toStdString()); }
QVector<VendorItem> itemsOf(QVector<Fields> const& rows) {
  QVector<VendorItem> items;
  for (auto const& r : rows) items.push_back({r["item"].toUInt(), r["maxcount"].toInt(), r["incrtime"].toInt(), r["condition_id"].toUInt()});
  return items;
}
}
QVector<Choice> VendorService::owners(QString const& text) {
  Database db; QVector<Choice> out;
  for (auto const& r : db.query("SELECT entry,name,subname FROM creature_template WHERE (entry IN (SELECT entry FROM npc_vendor) OR vendor_id>0) AND name LIKE "
                                + db.quote('%' + text + '%') + " ORDER BY name LIMIT 250"))
    out.push_back({r["entry"].toUInt(), r["name"].toString(), r["subname"].toString()});
  return out;
}
Vendor VendorService::load(Id entry) {
  Database db;
  auto rows = db.query("SELECT name,display_id1,npc_flags,vendor_id FROM creature_template WHERE entry=" + n(entry));
  require(!rows.isEmpty(), "This NPC no longer exists.");
  Vendor v; v.entry = entry; v.name = rows[0]["name"].toString(); v.display = rows[0]["display_id1"].toUInt();
  v.sells = rows[0]["npc_flags"].toUInt() & 4; v.sharedList = rows[0]["vendor_id"].toUInt();
  v.editable = db.owned("npc", entry);
  v.items = itemsOf(db.query("SELECT * FROM npc_vendor WHERE entry=" + n(entry) + " ORDER BY slot,item"));
  if (v.sharedList) v.shared = itemsOf(db.query("SELECT * FROM npc_vendor_template WHERE entry=" + n(v.sharedList) + " ORDER BY slot,item"));
  if (!v.editable) v.notice = "This is an original NPC: its shop is shown read-only. Copy its goods to one of your own NPCs to change them.";
  else if (v.sharedList) v.notice = "Part of this shop is a list shared with original vendors. Make it editable to change those goods too.";
  return v;
}
QVector<RowProblem> VendorService::validate(Vendor const& v) {
  QStringList ids; for (auto const& i : v.items) if (i.item) ids << n(i.item);
  QHash<Id, qint64> prices;
  if (!ids.isEmpty()) {
    Database db;
    for (auto const& r : db.query("SELECT entry,buy_price FROM item_template WHERE entry IN (" + ids.join(',') + ")")) prices[r["entry"].toUInt()] = r["buy_price"].toLongLong();
  }
  return check(v, prices);
}
QVector<RowProblem> VendorService::check(Vendor const& v, QHash<Id, qint64> const& prices) {
  QVector<RowProblem> problems;
  auto add = [&](int row, QString const& text, bool error = true) { problems.push_back({row, text, error}); };
  if (v.items.size() + v.shared.size() > maxItems) add(-1, QString("A shop holds at most %1 items; this one has %2.").arg(maxItems).arg(v.items.size() + v.shared.size()));
  if (!v.sells && !v.items.isEmpty()) add(-1, "Players cannot open this shop until the NPC is a vendor. Turn on \"Sells items\".", false);
  QSet<Id> seen, shared; for (auto const& i : v.shared) shared.insert(i.item);
  for (int r = 0; r < v.items.size(); ++r) {
    auto const& i = v.items[r];
    if (!i.item) { add(r, "Choose an item."); continue; }
    if (!prices.contains(i.item)) add(r, "This item does not exist in the world database.");
    else if (prices[i.item] == 0) add(r, "This item's price is 0: players get it for free.", false);
    if (seen.contains(i.item)) add(r, "This item is already in the shop.");
    else if (shared.contains(i.item)) add(r, "The shared list already sells this item, so it would be listed twice.", false);
    seen.insert(i.item);
    if (i.stock < 0 || i.stock > 255) add(r, "Stock is 1 to 255, or unlimited.");
    if (i.stock > 0 && i.restockSeconds <= 0) add(r, "Limited stock needs a restock time, or the item never comes back.");
    if (i.stock == 0 && i.restockSeconds > 0) add(r, "Restock time only matters for limited stock.", false);
    if (i.restockSeconds < 0) add(r, "The restock time cannot be negative.");
  }
  return problems;
}
void VendorService::save(Vendor const& v) {
  for (auto const& p : validate(v)) require(!p.error, p.text);
  Database db;
  require(db.owned("npc", v.entry), "Original NPCs keep their shop. Copy its goods to one of your own NPCs to change them.");
  auto id = n(v.entry);
  db.track(EntityType::Vendor, v.entry);
  db.snapshotWhere("npc_vendor", "entry=" + id);
  db.exec("DELETE FROM npc_vendor WHERE entry=" + id);
  for (int slot = 0; slot < v.items.size(); ++slot) {
    auto const& i = v.items[slot];
    db.insert("npc_vendor", {{"entry", v.entry}, {"slot", slot}, {"item", i.item}, {"maxcount", i.stock}, {"incrtime", i.restockSeconds},
                             {"itemflags", 0}, {"condition_id", i.condition}});
  }
  db.snapshot("creature_template", "entry", v.entry);
  db.exec("UPDATE creature_template SET vendor_id=" + n(v.sharedList) + ",npc_flags=(npc_flags&~4)|" + n(v.sells ? 4 : 0) + " WHERE entry=" + id);
  db.commit();
}
}
