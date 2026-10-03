#pragma once
#include "Services.hpp"
#include <QHash>
#include <QPair>
namespace Noggit::Creator {
// An item as designers see it: what its tooltip shows, plus what the editors need to judge it.
struct ItemInfo {
  Id entry = 0, display = 0;
  QString name, description;
  int quality = 1, itemClass = 0, subclass = 0, inventoryType = 0, itemLevel = 0, requiredLevel = 0;
  qint64 buyPrice = 0, sellPrice = 0;
  int buyCount = 1, stackable = 1, maxCount = 0, bonding = 0, armor = 0, block = 0, delay = 0, durability = 0, bagSlots = 0;
  double damageMin = 0, damageMax = 0;
  int damageType = 0;
  QVector<QPair<int, int>> stats;        // (stat type, value)
  QVector<QPair<int, int>> resistances;  // (school 1-6, value)
  QStringList effects;                   // "Equip: ...", "Use: ..."
  bool own = false;                      // made in Noggit
};
struct ItemFilter {
  enum class Scope { All, Project, Recent };
  QString text;
  Scope scope = Scope::All;
  int quality = -1, itemClass = -1, subclass = -1, inventoryType = -1, minLevel = 0, maxLevel = 0; // -1 / 0: any
  int limit = 600;
};
// Items from the local world database. Read-only: items are made in the item editor.
class ItemService {
public:
  static QVector<ItemInfo> search(ItemFilter const&);
  static std::optional<ItemInfo> get(Id entry);
  static QHash<Id, ItemInfo> get(QVector<Id> const& entries);
  // Items the designer picked recently in any Creator editor, newest first.
  static QVector<Id> recent();
  static void used(Id entry);

  static QString qualityName(int quality);
  static QString className(int itemClass);
  static QString subclassName(int itemClass, int subclass);
  static QString slotName(int inventoryType);
  static QString statName(int stat);
  static QString schoolName(int school);
  static QString bondingText(int bonding);
  static QVector<QPair<int, QString>> classes();
  static QVector<QPair<int, QString>> subclasses(int itemClass);
  static QVector<QPair<int, QString>> equipSlots();
};
// "2g 40s 5c"; "0c" for nothing.
QString formatMoney(qint64 copper);
// A spell_template row's description with its effect amounts filled in (needs description,
// effectBasePoints1-3 and effectDieSides1-3).
QString describeSpell(Fields const& spell);
}
