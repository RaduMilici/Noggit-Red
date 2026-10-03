#pragma once
#include "Services.hpp"
#include <QHash>
namespace Noggit::Creator {
struct VendorItem {
  Id item = 0;
  int stock = 0;          // 0: unlimited
  int restockSeconds = 0; // how long until a sold-out limited item is back
  Id condition = 0;
  bool operator==(VendorItem const&) const = default;
};
struct Vendor {
  Id entry = 0, display = 0;
  QString name, notice;
  bool editable = false;
  bool sells = false;          // has the vendor role: players can open the shop
  Id sharedList = 0;           // a list several original vendors use; shown, not changed
  QVector<VendorItem> items;   // this NPC's own goods, in shop order
  QVector<VendorItem> shared;
};
// What an NPC sells, in the local world. Only Creator NPCs can be changed.
class VendorService {
public:
  static constexpr int maxItems = 128; // what the game's shop window holds
  static QVector<Choice> owners(QString const& text); // NPCs that sell something
  static Vendor load(Id entry);
  static void save(Vendor const&);
  static QVector<RowProblem> validate(Vendor const&);
  // Database-free: `prices` holds each existing item's buy price.
  static QVector<RowProblem> check(Vendor const&, QHash<Id, qint64> const& prices);
};
}
