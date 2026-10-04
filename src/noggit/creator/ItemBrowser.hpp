#pragma once
#include "ItemService.hpp"
#include <QStandardItemModel>
#include <QWidget>
#include <functional>
#include <optional>
class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QSpinBox;
class QTabBar;
class QTimer;
class QToolButton;
namespace Noggit::Creator {
class ItemPreview;
// Items dragged out of the browser: their entries, one per line.
inline constexpr char itemMimeType[] = "application/x-noggit-creator-items";
QVector<Id> itemsFromMime(class QMimeData const*);

QIcon itemIcon(ItemInfo const&);
QColor itemColor(int quality); // invalid for common items (use the text colour)
// The item's tooltip as the game shows it, as rich text.
QString itemTooltip(ItemInfo const&);

// Icon, name, tooltip and 3D model of one item.
class ItemDetails final : public QWidget {
public:
  explicit ItemDetails(QWidget* parent = nullptr, bool model = true);
  void setItem(std::optional<ItemInfo> const& item);
private:
  QLabel *_icon, *_name, *_text, *_modelNote;
  ItemPreview* _model = nullptr;
};

// The searchable item browser every Creator editor uses to pick items: icon grid or list, filters,
// your own and recently used items. Items can be dragged out of it.
class ItemBrowser final : public QWidget {
public:
  // compact: no details card (for a side panel next to an editor that shows its own).
  ItemBrowser(ItemFilter const& preset = {}, bool compact = false, QWidget* parent = nullptr);
  std::optional<ItemInfo> current() const;
  std::function<void(Id)> onActivated;                          // double-click / Enter
  std::function<void(std::optional<ItemInfo> const&)> onCurrent; // selection
  void select(Id entry);
private:
  void search();
  ItemFilter filter() const;
  QLineEdit* _text;
  QTabBar* _scope;
  QComboBox *_quality, *_class, *_subclass, *_slot;
  QSpinBox *_minLevel, *_maxLevel;
  QToolButton *_grid, *_list;
  QListView* _view;
  QStandardItemModel* _model;
  QLabel* _status;
  ItemDetails* _details = nullptr;
  QTimer* _timer;
  QVector<ItemInfo> _items;
  Id _wanted = 0;
};
// A modal Item Browser; the chosen item is remembered in Recent.
std::optional<Id> pickItem(QWidget* parent, QString const& title, ItemFilter const& preset = {}, Id current = 0);
}
