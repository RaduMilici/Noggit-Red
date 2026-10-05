#include "ServiceEditors.hpp"
#include "VendorService.hpp"
#include "CreatorPreviews.hpp"
#include "EditorWidgets.hpp"
#include "ItemBrowser.hpp"
#include "TestSessionService.hpp"
#include <noggit/ui/FontAwesome.hpp>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
namespace Noggit::Creator {
namespace {
using Icon = Ui::FontAwesome::Icons;
enum Column { ItemColumn, PriceColumn, StockColumn, RestockColumn, NotesColumn, UpColumn, DownColumn, RemoveColumn, Columns };
QPushButton* button(QString const& text, Icon icon, QWidget* parent) { return new QPushButton(Ui::FontAwesomeIcon(icon), text, parent); }
// One merchant slot: icon with stack size, name in its quality colour, price below.
class MerchantSlot final : public QFrame {
public:
  MerchantSlot(QWidget* parent) : QFrame(parent) {
    setObjectName("MerchantSlot"); setFixedSize(176, 50); setCursor(Qt::PointingHandCursor);
    auto layout = new QHBoxLayout(this); layout->setContentsMargins(4, 4, 4, 4); layout->setSpacing(6);
    _icon = new QLabel(this); _icon->setFixedSize(40, 40); _icon->setObjectName("MerchantIcon"); layout->addWidget(_icon);
    auto text = new QVBoxLayout; text->setSpacing(0); layout->addLayout(text, 1);
    _name = new QLabel(this); _name->setWordWrap(true); _name->setTextFormat(Qt::RichText); text->addWidget(_name);
    _price = new QLabel(this); _price->setTextFormat(Qt::RichText); text->addWidget(_price);
  }
  void set(std::optional<ItemInfo> const& item, VendorItem const& entry, bool shared) {
    if (!item) { _icon->clear(); _name->setText(entry.item ? QString("<span style='color:#e04040'>Unknown item %1</span>").arg(entry.item) : QString()); _price->clear(); setToolTip({}); return; }
    auto pixmap = itemIcon(*item).pixmap(36, 36);
    if (item->buyCount > 1 || entry.stock > 0) {
      QPainter p(&pixmap); p.setPen(Qt::white); auto font = p.font(); font.setBold(true); font.setPointSize(8); p.setFont(font);
      if (item->buyCount > 1) p.drawText(pixmap.rect().adjusted(0, 0, -2, -1), Qt::AlignRight | Qt::AlignBottom, QString::number(item->buyCount));
      if (entry.stock > 0) p.drawText(pixmap.rect().adjusted(2, 1, 0, 0), Qt::AlignLeft | Qt::AlignTop, QString("(%1)").arg(entry.stock));
    }
    _icon->setPixmap(pixmap);
    auto color = itemColor(item->quality);
    _name->setText(QString("<span style='color:%1;font-size:8pt'>%2</span>").arg(color.isValid() ? color.name() : "#ffffff", item->name.toHtmlEscaped()));
    _price->setText("<span style='font-size:8pt'>" + moneyHtml(item->buyPrice) + "</span>");
    setToolTip(itemTooltip(*item) + (shared ? "<br><i>From the shared vendor list.</i>" : QString()));
  }
  std::function<void()> onClicked;
protected:
  void mousePressEvent(QMouseEvent*) override { if (onClicked) onClicked(); }
private:
  QLabel *_icon, *_name, *_price;
};
// The game's merchant window: two columns of five goods per page.
class MerchantPreview final : public QFrame {
public:
  MerchantPreview(QWidget* parent) : QFrame(parent) {
    setObjectName("Merchant");
    setStyleSheet("#Merchant { background: qlineargradient(y1:0, y2:1, stop:0 #2a1c0f, stop:1 #17100a); border: 2px solid #8a6d2f; border-radius: 6px; }"
                  "#MerchantSlot { background: rgba(0,0,0,0.35); border: 1px solid #4a3a1c; border-radius: 3px; }"
                  "#MerchantIcon { border: 1px solid #6b5a33; } QLabel { color: #f0e6c8; background: transparent; }");
    auto layout = new QVBoxLayout(this); layout->setContentsMargins(10, 8, 10, 8);
    _title = new QLabel(this); _title->setAlignment(Qt::AlignCenter); _title->setStyleSheet("color: #ffd100; font-weight: bold; font-size: 11pt;");
    layout->addWidget(_title);
    auto grid = new QGridLayout; grid->setSpacing(6); layout->addLayout(grid);
    for (int i = 0; i < 10; ++i) {
      auto slot = new MerchantSlot(this); _slots.push_back(slot); grid->addWidget(slot, i / 2, i % 2);
      slot->onClicked = [this, i] { auto index = _page * 10 + i; if (onSelected && index < _goods.size() && _goods[index].row >= 0) onSelected(_goods[index].row); };
    }
    auto pages = new QHBoxLayout; layout->addLayout(pages);
    _prev = new QToolButton(this); _prev->setText("◀"); _next = new QToolButton(this); _next->setText("▶");
    _pageLabel = new QLabel(this); _pageLabel->setAlignment(Qt::AlignCenter);
    pages->addWidget(_prev); pages->addWidget(_pageLabel, 1); pages->addWidget(_next);
    connect(_prev, &QToolButton::clicked, this, [this] { if (_page > 0) { --_page; draw(); } });
    connect(_next, &QToolButton::clicked, this, [this] { if ((_page + 1) * 10 < _goods.size()) { ++_page; draw(); } });
  }
  struct Good { std::optional<ItemInfo> item; VendorItem entry; int row = -1; }; // row: editor row; -1 shared
  void setGoods(QString const& name, QVector<Good> goods) {
    _title->setText(name); _goods = std::move(goods);
    _page = std::min(_page, std::max(0, (int(_goods.size()) - 1) / 10)); draw();
  }
  std::function<void(int)> onSelected;
private:
  void draw() {
    for (int i = 0; i < 10; ++i) {
      auto index = _page * 10 + i;
      _slots[i]->setVisible(index < _goods.size());
      if (index < _goods.size()) _slots[i]->set(_goods[index].item, _goods[index].entry, _goods[index].row < 0);
    }
    int pages = std::max(1, (int(_goods.size()) + 9) / 10);
    _pageLabel->setText(QString("Page %1 of %2").arg(_page + 1).arg(pages));
    _prev->setEnabled(_page > 0); _next->setEnabled(_page + 1 < pages);
  }
  QLabel *_title, *_pageLabel;
  QToolButton *_prev, *_next;
  QVector<MerchantSlot*> _slots;
  QVector<Good> _goods;
  int _page = 0;
};
class VendorEditor final : public QDialog {
public:
  VendorEditor(QWidget* parent, World* world, Id entry) : QDialog(parent), _entry(entry) {
    setWindowTitle("Vendor"); resize(1480, 820);
    auto layout = new QVBoxLayout(this);
    _title = editorTitle({}, this); layout->addWidget(_title);
    _notice = noticeBanner({}, this); layout->addWidget(_notice);
    auto split = new QSplitter(this); layout->addWidget(split, 1);

    auto left = new QWidget; auto leftLayout = new QVBoxLayout(left); leftLayout->setContentsMargins(0, 0, 0, 0);
    auto preview = new NpcPreview(world, left); preview->setFixedHeight(250); preview->showNpc(entry); leftLayout->addWidget(preview);
    _details = new ItemDetails(left); leftLayout->addWidget(_details, 1);
    split->addWidget(left);

    auto center = new QWidget; auto centerLayout = new QVBoxLayout(center); centerLayout->setContentsMargins(0, 0, 0, 0);
    auto options = new QHBoxLayout; centerLayout->addLayout(options);
    _sells = new QCheckBox("Sells items", center); _sells->setToolTip("Gives this NPC the vendor role, so players can open the shop.");
    options->addWidget(_sells);
    _makeEditable = button("Make Shared List Editable", Icon::edit, center);
    _makeEditable->setToolTip("Copies the shared list's goods into this NPC's own list, so you can change them.");
    options->addWidget(_makeEditable); options->addStretch();
    options->addWidget(new QLabel("Sort by", center));
    _sort = new QComboBox(center); _sort->addItems({"—", "Name", "Price", "Type", "Required level", "Quality"}); options->addWidget(_sort);

    auto shopRow = new QHBoxLayout; centerLayout->addLayout(shopRow);
    _shop = new MerchantPreview(center); shopRow->addWidget(_shop, 0, Qt::AlignTop);
    _table = new ItemDropTable(0, Columns, center);
    _table->setHorizontalHeaderLabels({"Item", "Price", "Stock", "Restock", "Notes", "", "", ""});
    _table->horizontalHeader()->setSectionResizeMode(ItemColumn, QHeaderView::Stretch);
    _table->horizontalHeader()->setSectionResizeMode(NotesColumn, QHeaderView::Stretch);
    for (int c : {PriceColumn, StockColumn, RestockColumn, UpColumn, DownColumn, RemoveColumn}) _table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    _table->verticalHeader()->hide(); _table->setSelectionBehavior(QAbstractItemView::SelectRows); _table->setSelectionMode(QAbstractItemView::SingleSelection);
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers); _table->setAcceptDrops(true); _table->setDragDropMode(QAbstractItemView::DropOnly);
    _table->setIconSize(QSize(26, 26)); _table->verticalHeader()->setDefaultSectionSize(34);
    _table->onItemsDropped = [this](QVector<Id> const& items) { addItems(items); };
    shopRow->addWidget(_table, 1);
    _listProblems = new QLabel(center); _listProblems->setWordWrap(true); centerLayout->addWidget(_listProblems);
    auto actions = new QHBoxLayout; centerLayout->addLayout(actions);
    _add = button("Add Item", Icon::plus, center); _copyFrom = button("Copy Inventory…", Icon::paste, center);
    _copyTo = button("Use on Another…", Icon::copy, center); _clear = button("Clear", Icon::eraser, center); _reset = button("Reset", Icon::undo, center);
    for (auto b : {_add, _copyFrom, _copyTo, _clear, _reset}) actions->addWidget(b);
    actions->addStretch();
    split->addWidget(center);

    auto right = new QWidget; auto rightLayout = new QVBoxLayout(right); rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(new QLabel("<b>Items</b> — drag into the shop, or double-click", right));
    auto browser = new ItemBrowser({}, true, right); rightLayout->addWidget(browser, 1);
    browser->onActivated = [this](Id item) { addItems({item}); };
    browser->onCurrent = [this](std::optional<ItemInfo> const& item) { _details->setItem(item); };
    split->addWidget(right);
    split->setSizes({280, 820, 360});

    auto bottom = new QHBoxLayout; layout->addLayout(bottom);
    auto test = button("Test Vendor…", Icon::play, this); bottom->addWidget(test); bottom->addStretch();
    auto close = new QPushButton("Close", this); _save = new QPushButton("Save", this); _save->setDefault(true);
    bottom->addWidget(close); bottom->addWidget(_save);

    _shop->onSelected = [this](int row) { _table->selectRow(row); };
    connect(_table, &QTableWidget::itemSelectionChanged, this, [this] {
      auto row = _table->currentRow();
      if (row >= 0 && row < _vendor.items.size() && _items.contains(_vendor.items[row].item)) _details->setItem(_items[_vendor.items[row].item]);
    });
    connect(_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
      if (column != ItemColumn || !_vendor.editable) return;
      if (auto item = pickItem(this, "Change item", {}, _vendor.items[row].item)) { _vendor.items[row].item = *item; fetchItems(); _dirty = true; rebuild(); }
    });
    connect(_sells, &QCheckBox::toggled, this, [this](bool on) { if (!_building) { _vendor.sells = on; changed(); } });
    connect(_makeEditable, &QPushButton::clicked, this, [this] {
      _vendor.items = _vendor.shared + _vendor.items; _vendor.shared.clear(); _vendor.sharedList = 0; _dirty = true; rebuild();
    });
    connect(_sort, qOverload<int>(&QComboBox::activated), this, [this](int mode) { sortBy(mode); _sort->setCurrentIndex(0); });
    connect(_add, &QPushButton::clicked, this, [this] { if (auto item = pickItem(this, "Add to the shop")) addItems({*item}); });
    connect(_copyFrom, &QPushButton::clicked, this, [this] { copyFrom(); });
    connect(_copyTo, &QPushButton::clicked, this, [this] { copyTo(); });
    connect(_clear, &QPushButton::clicked, this, [this] {
      if (QMessageBox::question(this, "Clear", "Remove every item this NPC sells? Nothing is saved until you click Save.") != QMessageBox::Yes) return;
      _vendor.items.clear(); _dirty = true; rebuild();
    });
    connect(_reset, &QPushButton::clicked, this, [this] {
      if (_dirty && QMessageBox::question(this, "Reset", "Discard your changes and reload the saved shop?") != QMessageBox::Yes) return;
      load();
    });
    connect(test, &QPushButton::clicked, this, [this] { testVendor(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(_save, &QPushButton::clicked, this, [this] { if (save()) accept(); });
    load();
  }
  bool saved = false;
  void reject() override { if (confirmClose(this, _dirty, [this] { return save(); })) QDialog::reject(); }
private:
  void load() {
    try { _vendor = VendorService::load(_entry); } catch (std::exception const& e) { QMessageBox::warning(this, "Vendor", e.what()); return; }
    _title->setText((_vendor.name + " — Vendor").toUpper());
    _notice->setText(_vendor.notice); _notice->setVisible(!_vendor.notice.isEmpty());
    for (auto w : std::initializer_list<QWidget*>{_add, _copyFrom, _clear, _reset, _save, _sort}) w->setVisible(_vendor.editable);
    _sells->setEnabled(_vendor.editable);
    _copyTo->setText(_vendor.editable ? "Use on Another…" : "Copy to My NPC…");
    fetchItems(); _dirty = false; rebuild();
  }
  void fetchItems() {
    QVector<Id> missing;
    for (auto const* list : {&_vendor.items, &_vendor.shared}) for (auto const& i : *list) if (i.item && !_items.contains(i.item)) missing << i.item;
    if (missing.isEmpty()) return;
    try { auto found = ItemService::get(missing); for (auto it = found.begin(); it != found.end(); ++it) _items[it.key()] = *it; } catch (...) {}
  }
  std::optional<ItemInfo> info(Id item) const { return _items.contains(item) ? std::optional<ItemInfo>(_items[item]) : std::nullopt; }
  void rebuild() {
    _building = true;
    _sells->setChecked(_vendor.sells);
    _makeEditable->setVisible(_vendor.editable && _vendor.sharedList);
    _table->setRowCount(_vendor.items.size());
    for (int i = 0; i < _vendor.items.size(); ++i) buildRow(i);
    _building = false;
    refresh();
  }
  void buildRow(int i) {
    auto const v = _vendor.items[i];
    bool locked = !_vendor.editable;
    auto cell = new QTableWidgetItem;
    if (auto item = info(v.item)) {
      cell->setText(item->name + (item->buyCount > 1 ? QString(" ×%1").arg(item->buyCount) : QString())); cell->setIcon(itemIcon(*item));
      cell->setToolTip(itemTooltip(*item)); if (auto c = itemColor(item->quality); c.isValid()) cell->setForeground(c);
    } else cell->setText(QString("Unknown item %1").arg(v.item));
    cell->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    _table->setItem(i, ItemColumn, cell);
    auto price = new QLabel(info(v.item) ? moneyHtml(info(v.item)->buyPrice) : QString()); price->setTextFormat(Qt::RichText);
    price->setToolTip("The item's buy price, set on the item itself.");
    _table->setCellWidget(i, PriceColumn, price);
    auto stock = new QSpinBox; stock->setRange(0, 255); stock->setSpecialValueText("Unlimited"); stock->setValue(v.stock); stock->setEnabled(!locked);
    stock->setToolTip("How many the vendor has. Limited stock sells out and comes back after the restock time.");
    auto restock = new QSpinBox; restock->setRange(0, 7 * 24 * 60); restock->setSuffix(" min"); restock->setSpecialValueText("—");
    restock->setValue((v.restockSeconds + 59) / 60); restock->setEnabled(!locked && v.stock > 0);
    connect(stock, qOverload<int>(&QSpinBox::valueChanged), this, [this, i, restock](int value) {
      if (_building) return;
      _vendor.items[i].stock = value; restock->setEnabled(value > 0);
      if (value > 0 && _vendor.items[i].restockSeconds == 0) restock->setValue(15); // a sensible default: 15 minutes
      changed();
    });
    connect(restock, qOverload<int>(&QSpinBox::valueChanged), this, [this, i](int minutes) { if (!_building) { _vendor.items[i].restockSeconds = minutes * 60; changed(); } });
    _table->setCellWidget(i, StockColumn, stock); _table->setCellWidget(i, RestockColumn, restock);
    auto tool = [&](Icon icon, QString const& tip, int column, std::function<void()> action) {
      auto b = new QToolButton; b->setIcon(Ui::FontAwesomeIcon(icon)); b->setAutoRaise(true); b->setToolTip(tip); b->setEnabled(!locked);
      connect(b, &QToolButton::clicked, this, action); _table->setCellWidget(i, column, b);
    };
    tool(Icon::arrowup, "Move up", UpColumn, [this, i] { move(i, i - 1); });
    tool(Icon::arrowdown, "Move down", DownColumn, [this, i] { move(i, i + 1); });
    tool(Icon::trash, "Stop selling this", RemoveColumn, [this, i] { _vendor.items.removeAt(i); _dirty = true; rebuild(); });
  }
  void move(int from, int to) {
    if (to < 0 || to >= _vendor.items.size()) return;
    _vendor.items.move(from, to); _dirty = true; rebuild(); _table->selectRow(to);
  }
  void sortBy(int mode) {
    if (!mode) return;
    auto key = [&](VendorItem const& v) -> QVariant {
      auto i = info(v.item); if (!i) return QVariant();
      switch (mode) {
        case 1: return i->name.toLower(); case 2: return i->buyPrice; case 3: return i->itemClass * 1000 + i->subclass;
        case 4: return i->requiredLevel; default: return -i->quality;
      }
    };
    std::stable_sort(_vendor.items.begin(), _vendor.items.end(), [&](VendorItem const& a, VendorItem const& b) {
      auto x = key(a), y = key(b);
      return x.type() == QVariant::String ? x.toString() < y.toString() : x.toLongLong() < y.toLongLong();
    });
    _dirty = true; rebuild();
  }
  void changed() { _dirty = true; refresh(); }
  void refresh() {
    QHash<Id, qint64> prices; for (auto it = _items.begin(); it != _items.end(); ++it) prices[it.key()] = it->buyPrice;
    auto problems = VendorService::check(_vendor, prices);
    for (int i = 0; i < _vendor.items.size(); ++i) showRowProblems(_table, i, NotesColumn, problems);
    auto list = listProblems(problems);
    _listProblems->setText(list); _listProblems->setVisible(!list.isEmpty());
    _listProblems->setStyleSheet(hasErrors(problems) ? "color: #e04040;" : "color: #d08a00;");
    QVector<MerchantPreview::Good> goods;
    for (int i = 0; i < _vendor.items.size(); ++i) goods.push_back({info(_vendor.items[i].item), _vendor.items[i], i});
    for (auto const& s : _vendor.shared) goods.push_back({info(s.item), s, -1});
    _shop->setGoods(_vendor.name, goods);
    _save->setEnabled(_vendor.editable && !hasErrors(problems));
  }
  void addItems(QVector<Id> const& items) {
    if (!_vendor.editable) return;
    for (auto id : items) { _vendor.items.push_back({id}); try { ItemService::used(id); } catch (...) {} }
    fetchItems(); _dirty = true; rebuild(); _table->selectRow(_vendor.items.size() - 1);
  }
  void copyFrom() {
    auto source = chooseOwner(this, "Copy inventory from", [](QString const& text) { return VendorService::owners(text); });
    if (!source) return;
    try {
      auto from = VendorService::load(source->id);
      auto goods = from.shared + from.items;
      if (goods.isEmpty()) { QMessageBox::information(this, "Copy Inventory", source->name + " sells nothing."); return; }
      QMessageBox ask(QMessageBox::Question, "Copy Inventory", QString("Copy %1 items from %2.").arg(goods.size()).arg(source->name), QMessageBox::Cancel, this);
      auto replace = ask.addButton("Replace current goods", QMessageBox::AcceptRole), add = ask.addButton("Add to them", QMessageBox::AcceptRole);
      ask.exec();
      if (ask.clickedButton() != replace && ask.clickedButton() != add) return;
      if (ask.clickedButton() == replace) _vendor.items.clear();
      QSet<Id> have; for (auto const& v : _vendor.items) have.insert(v.item);
      for (auto const& g : goods) if (!have.contains(g.item)) { _vendor.items.push_back(g); have.insert(g.item); }
      _vendor.sells = true;
      fetchItems(); _dirty = true; rebuild();
    } catch (std::exception const& e) { QMessageBox::warning(this, "Copy Inventory", e.what()); }
  }
  void copyTo() {
    auto target = chooseOwner(this, "Give these goods to one of your NPCs", [](QString const& text) { return CreatureService::search(text, true); });
    if (!target || target->id == _entry) return;
    if (QMessageBox::question(this, "Use on Another", "Replace what " + target->name + " sells with these goods?") != QMessageBox::Yes) return;
    try {
      auto other = VendorService::load(target->id);
      other.items = _vendor.shared + _vendor.items; other.sells = true; other.sharedList = 0; other.shared.clear();
      VendorService::save(other); saved = true;
      QMessageBox::information(this, "Use on Another", target->name + " now sells these goods. It is listed in Local Changes.");
    } catch (std::exception const& e) { QMessageBox::warning(this, "Use on Another", e.what()); }
  }
  bool save() {
    try {
      if (hasErrors(VendorService::validate(_vendor))) { refresh(); QMessageBox::warning(this, "Vendor", "Fix the problems marked ✗ first."); return false; }
      VendorService::save(_vendor);
    } catch (std::exception const& e) { QMessageBox::warning(this, "Vendor", QString::fromUtf8(e.what())); return false; }
    _dirty = false; saved = true; return true;
  }
  void testVendor() {
    auto tests = TestSessionService::instance();
    if (!tests) return;
    if (_dirty && (QMessageBox::question(this, "Test", "Save the shop before testing?") != QMessageBox::Yes || !save())) return;
    QDialog options(this); options.setWindowTitle("Test Vendor");
    auto layout = new QVBoxLayout(&options);
    auto text = new QLabel("Your local test character is placed beside " + _vendor.name + "."); text->setWordWrap(true); layout->addWidget(text);
    auto money = new QCheckBox("Give my test character money to shop with:"); money->setChecked(true);
    auto amount = new MoneyEdit; amount->setValue(100 * 10000);
    auto row = new QHBoxLayout; row->addWidget(money); row->addWidget(amount); row->addStretch(); layout->addLayout(row);
    auto note = new QLabel("Only changes your character on the local test server."); note->setStyleSheet("color: gray;"); layout->addWidget(note);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); buttons->addButton("Test", QDialogButtonBox::AcceptRole); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &options, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &options, &QDialog::reject);
    if (options.exec() != QDialog::Accepted) return;
    TestOptions test; if (money->isChecked()) test.money = amount->value();
    tests->testEntity(this, false, _entry, test);
  }
  Id _entry;
  Vendor _vendor;
  QHash<Id, ItemInfo> _items;
  bool _dirty = false, _building = false;
  QLabel *_title, *_notice, *_listProblems;
  ItemDetails* _details;
  QCheckBox* _sells;
  QPushButton *_makeEditable, *_add, *_copyFrom, *_copyTo, *_clear, *_reset, *_save;
  QComboBox* _sort;
  MerchantPreview* _shop;
  ItemDropTable* _table;
};
}
bool editVendor(QWidget* parent, World* world, Id npc) {
  VendorEditor editor(parent, world, npc);
  editor.exec();
  return editor.saved;
}
}
