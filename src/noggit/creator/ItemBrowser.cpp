#include "ItemBrowser.hpp"
#include "ContentEditors.hpp"
#include "CreatorPreviews.hpp"
#include <noggit/ui/content/ClientData.hpp>
#include <noggit/ui/content/ContentLookups.hpp>
#include <QButtonGroup>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMimeData>
#include <QPushButton>
#include <QSpinBox>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <noggit/ui/FontAwesome.hpp>
namespace Noggit::Creator {
namespace {
constexpr int EntryRole = Qt::UserRole, DisplayRole = Qt::UserRole + 1;
QIcon cachedIcon(Id display) {
  static QHash<Id, QIcon> icons;
  if (!display) return {};
  auto it = icons.find(display);
  if (it == icons.end()) it = icons.insert(display, Ui::Content::ClientData::itemIcon(display));
  return *it;
}
// Icons load as rows are drawn, and rows drag out as item entries.
class ItemListModel final : public QStandardItemModel {
public:
  using QStandardItemModel::QStandardItemModel;
  QVariant data(QModelIndex const& index, int role) const override {
    if (role == Qt::DecorationRole) return cachedIcon(QStandardItemModel::data(index, DisplayRole).toUInt());
    return QStandardItemModel::data(index, role);
  }
  QStringList mimeTypes() const override { return {itemMimeType}; }
  QMimeData* mimeData(QModelIndexList const& indexes) const override {
    QStringList entries; for (auto const& i : indexes) entries << QString::number(i.data(EntryRole).toUInt());
    auto mime = new QMimeData; mime->setData(itemMimeType, entries.join('\n').toUtf8()); mime->setText(entries.join('\n'));
    return mime;
  }
};
QString esc(QString const& text) { return text.toHtmlEscaped(); }
QString typeLine(ItemInfo const& i) {
  QStringList parts;
  if (auto slot = ItemService::slotName(i.inventoryType); i.inventoryType && !slot.isEmpty()) parts << slot;
  if (auto sub = ItemService::subclassName(i.itemClass, i.subclass); !sub.isEmpty()) parts << sub;
  else parts << ItemService::className(i.itemClass);
  return parts.join(" · ");
}
}
QVector<Id> itemsFromMime(QMimeData const* mime) {
  QVector<Id> out;
  if (!mime || !mime->hasFormat(itemMimeType)) return out;
  for (auto const& line : QString::fromUtf8(mime->data(itemMimeType)).split('\n')) if (auto id = line.toUInt()) out.push_back(id);
  return out;
}
QIcon itemIcon(ItemInfo const& item) { return cachedIcon(item.display); }
QColor itemColor(int quality) { return Ui::Content::qualityColor(std::uint32_t(quality)); }
QString itemTooltip(ItemInfo const& i) {
  auto color = itemColor(i.quality);
  QString html = "<table cellspacing='0' cellpadding='0' style='min-width:220px'>";
  auto line = [&](QString const& left, QString const& right = {}, QString const& style = {}) {
    html += "<tr><td" + (style.isEmpty() ? QString() : " style='" + style + "'") + ">" + left + "</td><td align='right'>" + right + "</td></tr>";
  };
  line("<b>" + (color.isValid() ? "<span style='color:" + color.name() + "'>" + esc(i.name) + "</span>" : esc(i.name)) + "</b>");
  if (auto bond = ItemService::bondingText(i.bonding); !bond.isEmpty()) line(bond);
  if (i.inventoryType) line(esc(ItemService::slotName(i.inventoryType)), esc(ItemService::subclassName(i.itemClass, i.subclass)));
  if (i.damageMax > 0) {
    auto school = i.damageType ? " " + ItemService::schoolName(i.damageType) : QString();
    line(QString("%1 - %2%3 Damage").arg(int(i.damageMin)).arg(int(i.damageMax)).arg(school), i.delay ? QString("Speed %1").arg(i.delay / 1000.0, 0, 'f', 2) : QString());
    if (i.delay) line(QString("(%1 damage per second)").arg((i.damageMin + i.damageMax) / 2 / (i.delay / 1000.0), 0, 'f', 1));
  }
  if (i.armor) line(QString("%1 Armor").arg(i.armor));
  if (i.block) line(QString("%1 Block").arg(i.block));
  for (auto const& [stat, value] : i.stats) line(QString("%1%2 %3").arg(value > 0 ? "+" : "").arg(value).arg(ItemService::statName(stat)));
  for (auto const& [school, value] : i.resistances) line(QString("%1%2 %3 Resistance").arg(value > 0 ? "+" : "").arg(value).arg(ItemService::schoolName(school)));
  if (i.bagSlots) line(QString("%1 Slot %2").arg(i.bagSlots).arg(i.itemClass == 11 ? "Quiver" : "Bag"));
  if (i.durability) line(QString("Durability %1 / %1").arg(i.durability));
  if (i.requiredLevel > 1) line(QString("Requires Level %1").arg(i.requiredLevel));
  for (auto const& effect : i.effects) line(esc(effect), {}, "color:#1eff00");
  if (!i.description.isEmpty()) line("\"" + esc(i.description) + "\"", {}, "color:#ffd100");
  if (i.itemLevel) line(QString("Item Level %1").arg(i.itemLevel), {}, "color:gray");
  line(i.sellPrice > 0 ? "Sell Price: " + formatMoney(i.sellPrice) : "No sell price", {}, "color:gray");
  return html + "</table>";
}
ItemDetails::ItemDetails(QWidget* parent, bool model) : QWidget(parent) {
  auto layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
  auto head = new QHBoxLayout; layout->addLayout(head);
  _icon = new QLabel(this); _icon->setFixedSize(56, 56); head->addWidget(_icon, 0, Qt::AlignTop);
  _name = new QLabel(this); _name->setWordWrap(true); _name->setTextFormat(Qt::RichText); head->addWidget(_name, 1);
  _text = new QLabel(this); _text->setWordWrap(true); _text->setTextFormat(Qt::RichText); _text->setAlignment(Qt::AlignTop);
  _text->setTextInteractionFlags(Qt::TextSelectableByMouse);
  _text->setStyleSheet("background: rgba(10,14,30,0.85); color: #f0f0f0; border: 1px solid #555; border-radius: 4px; padding: 6px;");
  layout->addWidget(_text);
  if (model) { _model = new ItemPreview(this); _model->setMinimumHeight(170); layout->addWidget(_model, 1); }
  _modelNote = new QLabel(this); _modelNote->setWordWrap(true); _modelNote->setStyleSheet("color: gray; font-style: italic;");
  layout->addWidget(_modelNote);
  layout->addStretch();
  setItem(std::nullopt);
}
void ItemDetails::setItem(std::optional<ItemInfo> const& item) {
  if (!item) {
    _icon->clear(); _name->setText("<i>No item selected</i>"); _text->hide(); _modelNote->hide();
    if (_model) _model->hide();
    return;
  }
  _icon->setPixmap(itemIcon(*item).pixmap(56, 56));
  auto color = itemColor(item->quality);
  _name->setText(QString("<span style='font-size:12pt;font-weight:bold;%1'>%2</span><br><span style='color:gray'>%3 · %4</span>")
                   .arg(color.isValid() ? "color:" + color.name() + ";" : QString(), item->name.toHtmlEscaped(),
                        ItemService::qualityName(item->quality), typeLine(*item).toHtmlEscaped()));
  _text->setText(itemTooltip(*item)); _text->show();
  if (_model) {
    _model->showItem(item->display, item->inventoryType);
    _model->setVisible(_model->hasModel());
    _modelNote->setText(_model->hasModel() ? QString() : "No separate 3D model: the game draws this item on the character or in bags.");
    _modelNote->setVisible(!_model->hasModel());
  }
}
ItemBrowser::ItemBrowser(ItemFilter const& preset, bool compact, QWidget* parent) : QWidget(parent) {
  auto outer = new QHBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0);
  auto left = new QVBoxLayout; outer->addLayout(left, 3);
  auto top = new QHBoxLayout; left->addLayout(top);
  _text = new QLineEdit(preset.text, this); _text->setPlaceholderText("Search items by name…"); _text->setClearButtonEnabled(true);
  top->addWidget(_text, 1);
  auto toggle = [&](Ui::FontAwesome::Icons icon, QString const& tip) {
    auto b = new QToolButton(this); b->setIcon(Ui::FontAwesomeIcon(icon)); b->setToolTip(tip); b->setCheckable(true); b->setAutoRaise(true);
    top->addWidget(b); return b;
  };
  _grid = toggle(Ui::FontAwesome::Icons::th, "Icon grid"); _list = toggle(Ui::FontAwesome::Icons::list, "List");
  // New items can be made right where one is needed (loot, shops, quest rewards, NPC weapons).
  auto action = [&](QString const& text, Ui::FontAwesome::Icons icon, QString const& tip) {
    auto b = new QToolButton(this); b->setText(text); b->setIcon(Ui::FontAwesomeIcon(icon)); b->setToolTip(tip);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); top->addWidget(b); return b;
  };
  auto create = action("New", Ui::FontAwesome::Icons::plus, "Make a new item: clone an existing one, or start from a template");
  auto clone = action("Clone", Ui::FontAwesome::Icons::clone, "Make your own copy of the selected item");
  auto open = action("Open", Ui::FontAwesome::Icons::edit, "Open the selected item in the Item Editor (game items open read-only)");
  auto made = [this](std::optional<Id> id) { if (!id) return; search(); select(*id); };
  connect(create, &QToolButton::clicked, this, [this, made] { made(createItem(this)); });
  connect(clone, &QToolButton::clicked, this, [this, made] { made(cloneItem(this, current() ? current()->entry : 0)); });
  connect(open, &QToolButton::clicked, this, [this, made] { if (auto c = current()) made(editItem(this, c->entry)); });
  auto views = new QButtonGroup(this); views->addButton(_grid); views->addButton(_list); views->setExclusive(true);
  _scope = new QTabBar(this); _scope->addTab("All items"); _scope->addTab("My items"); _scope->addTab("Recent");
  _scope->setCurrentIndex(int(preset.scope)); _scope->setExpanding(false); left->addWidget(_scope);
  auto filters = new QHBoxLayout; left->addLayout(filters);
  _quality = new QComboBox(this); _quality->addItem("Any quality", -1);
  for (int q = 0; q <= 5; ++q) { _quality->addItem(ItemService::qualityName(q), q); if (auto c = itemColor(q); c.isValid()) _quality->setItemData(_quality->count() - 1, c, Qt::ForegroundRole); }
  _class = new QComboBox(this); _class->addItem("Any class", -1); for (auto const& [id, name] : ItemService::classes()) _class->addItem(name, id);
  _subclass = new QComboBox(this);
  _slot = new QComboBox(this); _slot->addItem("Any slot", -1); for (auto const& [id, name] : ItemService::equipSlots()) _slot->addItem(name, id);
  _minLevel = new QSpinBox(this); _maxLevel = new QSpinBox(this);
  for (auto s : {_minLevel, _maxLevel}) { s->setRange(0, 60); s->setSpecialValueText("any"); s->setToolTip("Required level"); }
  for (auto w : std::initializer_list<QWidget*>{_quality, _class, _subclass, _slot}) filters->addWidget(w, 1);
  filters->addWidget(new QLabel("Level", this)); filters->addWidget(_minLevel); filters->addWidget(new QLabel("–", this)); filters->addWidget(_maxLevel);
  _view = new QListView(this);
  _model = new ItemListModel(this); _view->setModel(_model);
  _view->setDragEnabled(true); _view->setDragDropMode(QAbstractItemView::DragOnly); _view->setSelectionMode(QAbstractItemView::ExtendedSelection);
  _view->setUniformItemSizes(true); _view->setTextElideMode(Qt::ElideRight);
  left->addWidget(_view, 1);
  _status = new QLabel(this); _status->setStyleSheet("color: gray;"); left->addWidget(_status);
  if (!compact) { _details = new ItemDetails(this); _details->setMinimumWidth(280); outer->addWidget(_details, 2); }

  auto setSubclasses = [this] {
    _subclass->clear(); _subclass->addItem("Any type", -1);
    for (auto const& [id, name] : ItemService::subclasses(_class->currentData().toInt())) _subclass->addItem(name, id);
    _subclass->setEnabled(_subclass->count() > 1);
  };
  auto pick = [](QComboBox* box, int value) { if (auto i = box->findData(value); i >= 0) box->setCurrentIndex(i); };
  pick(_quality, preset.quality); pick(_class, preset.itemClass); setSubclasses(); pick(_subclass, preset.subclass); pick(_slot, preset.inventoryType);
  _minLevel->setValue(preset.minLevel); _maxLevel->setValue(preset.maxLevel);
  auto setMode = [this](bool grid) {
    _view->setViewMode(grid ? QListView::IconMode : QListView::ListMode);
    _view->setIconSize(grid ? QSize(40, 40) : QSize(22, 22));
    _view->setGridSize(grid ? QSize(92, 86) : QSize());
    _view->setWordWrap(grid); _view->setResizeMode(QListView::Adjust); _view->setMovement(QListView::Static);
    _view->setSpacing(grid ? 2 : 0);
  };
  (compact ? _list : _grid)->setChecked(true); setMode(!compact);
  connect(_grid, &QToolButton::toggled, this, [setMode](bool on) { if (on) setMode(true); });
  connect(_list, &QToolButton::toggled, this, [setMode](bool on) { if (on) setMode(false); });

  _timer = new QTimer(this); _timer->setSingleShot(true); _timer->setInterval(250);
  connect(_timer, &QTimer::timeout, this, [this] { search(); });
  auto later = [this] { _timer->start(); };
  connect(_text, &QLineEdit::textChanged, this, later);
  connect(_scope, &QTabBar::currentChanged, this, later);
  connect(_class, qOverload<int>(&QComboBox::currentIndexChanged), this, [setSubclasses, later] { setSubclasses(); later(); });
  for (auto box : {_quality, _subclass, _slot}) connect(box, qOverload<int>(&QComboBox::currentIndexChanged), this, later);
  for (auto spin : {_minLevel, _maxLevel}) connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, later);
  connect(_view->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
    auto item = current();
    if (_details) _details->setItem(item);
    if (onCurrent) onCurrent(item);
  });
  connect(_view, &QListView::activated, this, [this](QModelIndex const& index) { if (onActivated) onActivated(index.data(EntryRole).toUInt()); });
  search();
}
ItemFilter ItemBrowser::filter() const {
  ItemFilter f;
  f.text = _text->text(); f.scope = ItemFilter::Scope(_scope->currentIndex());
  f.quality = _quality->currentData().toInt(); f.itemClass = _class->currentData().toInt(); f.subclass = _subclass->currentData().toInt();
  f.inventoryType = _slot->currentData().toInt(); f.minLevel = _minLevel->value(); f.maxLevel = _maxLevel->value();
  return f;
}
void ItemBrowser::search() {
  auto f = filter();
  try { _items = ItemService::search(f); }
  catch (std::exception const& e) { _items.clear(); _model->clear(); _status->setText(QString::fromUtf8(e.what())); return; }
  _model->clear();
  for (auto const& i : _items) {
    auto row = new QStandardItem(_list->isChecked() ? QString("%1   —   %2%3").arg(i.name, typeLine(i), i.requiredLevel > 1 ? QString(" · Level %1").arg(i.requiredLevel) : QString()) : i.name);
    row->setData(i.entry, EntryRole); row->setData(i.display, DisplayRole);
    row->setToolTip(itemTooltip(i)); row->setEditable(false);
    row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
    if (auto c = itemColor(i.quality); c.isValid()) row->setForeground(c);
    if (i.own) { auto font = row->font(); font.setBold(true); row->setFont(font); }
    _model->appendRow(row);
  }
  _status->setText(_items.isEmpty() ? (f.scope == ItemFilter::Scope::Recent ? "No recently used items yet." : "No items match.")
                   : _items.size() >= f.limit ? QString("Showing the first %1 matches. Refine the search to see more.").arg(f.limit)
                   : QString("%1 items · drag them into an editor, or double-click").arg(_items.size()));
  if (_wanted) select(_wanted);
}
std::optional<ItemInfo> ItemBrowser::current() const {
  auto index = _view->currentIndex();
  if (!index.isValid() || index.row() >= _items.size()) return std::nullopt;
  return _items[index.row()];
}
void ItemBrowser::select(Id entry) {
  _wanted = entry;
  for (int r = 0; r < _model->rowCount(); ++r)
    if (_model->item(r)->data(EntryRole).toUInt() == entry) { _view->setCurrentIndex(_model->index(r, 0)); _view->scrollTo(_model->index(r, 0)); _wanted = 0; return; }
}
std::optional<Id> pickItem(QWidget* parent, QString const& title, ItemFilter const& preset, Id current) {
  QDialog dialog(parent); dialog.setWindowTitle(title); dialog.resize(1040, 660);
  auto layout = new QVBoxLayout(&dialog);
  auto browser = new ItemBrowser(preset, false, &dialog); layout->addWidget(browser, 1);
  if (current) browser->select(current);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); auto choose = buttons->addButton("Choose", QDialogButtonBox::AcceptRole);
  choose->setDefault(true); layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] { if (browser->current()) dialog.accept(); });
  browser->onActivated = [&](Id) { dialog.accept(); };
  if (dialog.exec() != QDialog::Accepted || !browser->current()) return std::nullopt;
  auto id = browser->current()->entry;
  try { ItemService::used(id); } catch (...) {}
  return id;
}
}
