#include "ServiceEditors.hpp"
#include "CreatorPreviews.hpp"
#include "EditorWidgets.hpp"
#include "ItemBrowser.hpp"
#include "TestSessionService.hpp"
#include <noggit/ui/FontAwesome.hpp>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QSignalBlocker>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>
#include <random>
namespace Noggit::Creator {
namespace {
using Icon = Ui::FontAwesome::Icons;
enum Column { ItemColumn, ChanceColumn, AlwaysColumn, MinColumn, MaxColumn, GroupColumn, PerKillColumn, NotesColumn, RemoveColumn, Columns };
QString percent(double fraction) {
  auto p = fraction * 100;
  return p >= 10 || p == 0 ? QString("%1%").arg(p, 0, 'f', p == int(p) ? 0 : 1) : QString("%1%").arg(p, 0, 'g', 2);
}
QPushButton* button(QString const& text, Icon icon, QWidget* parent) {
  auto b = new QPushButton(Ui::FontAwesomeIcon(icon), text, parent); return b;
}
QWidget* centered(QWidget* w) {
  auto box = new QWidget; auto l = new QHBoxLayout(box); l->setContentsMargins(0, 0, 0, 0); l->addWidget(w, 0, Qt::AlignCenter); return box;
}
// Drop simulator: designer feedback, not the server's exact numbers (it also applies loot rates).
void showSimulation(QWidget* parent, LootTable const& table, QHash<Id, QVector<LootRow>> const& references, QHash<Id, ItemInfo> const& items) {
  QDialog dialog(parent); dialog.setWindowTitle("Simulate Drops — " + table.ownerName); dialog.resize(620, 560);
  auto layout = new QVBoxLayout(&dialog);
  auto row = new QHBoxLayout; layout->addLayout(row);
  row->addWidget(new QLabel(table.owner.kind == LootOwner::Kind::Npc ? "Simulate" : "Open"));
  auto kills = new QSpinBox; kills->setRange(100, 1000000); kills->setSingleStep(1000); kills->setValue(10000); kills->setGroupSeparatorShown(true);
  row->addWidget(kills); row->addWidget(new QLabel(table.owner.kind == LootOwner::Kind::Npc ? "kills" : "times"));
  auto run = new QPushButton(Ui::FontAwesomeIcon(Icon::dice), "Run"); row->addWidget(run); row->addStretch();
  auto summary = new QLabel; summary->setTextFormat(Qt::RichText); summary->setWordWrap(true); layout->addWidget(summary);
  auto results = new QTableWidget(0, 4); results->setHorizontalHeaderLabels({"Item", "Expected", "Observed", "Avg. quantity"});
  results->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch); results->verticalHeader()->hide();
  results->setEditTriggers(QAbstractItemView::NoEditTriggers); layout->addWidget(results, 1);
  auto note = new QLabel("Quest drops are counted as if the quest were active. The server's loot rate settings are not applied.");
  note->setWordWrap(true); note->setStyleSheet("color: gray;"); layout->addWidget(note);
  auto known = items;
  auto simulate = [&] {
    auto s = LootService::simulate(table, references, kills->value(), std::random_device{}());
    QVector<Id> missing; for (auto const& i : s.items) if (!known.contains(i.item)) missing << i.item;
    if (!missing.isEmpty()) try { auto more = ItemService::get(missing); for (auto it = more.begin(); it != more.end(); ++it) known[it.key()] = *it; } catch (...) {}
    summary->setText(QString("<b>Simulated %L1 %2</b><br>Average money: %3<br>%4 with nothing at all")
                       .arg(s.kills).arg(table.owner.kind == LootOwner::Kind::Npc ? "kills" : "openings").arg(moneyHtml(qint64(std::llround(s.averageMoney))))
                       .arg(percent(s.kills ? double(s.empty) / s.kills : 0)));
    results->setRowCount(s.items.size());
    for (int r = 0; r < s.items.size(); ++r) {
      auto const& i = s.items[r];
      auto cell = new QTableWidgetItem(known.contains(i.item) ? known[i.item].name : QString("Item %1").arg(i.item));
      if (known.contains(i.item)) { cell->setIcon(itemIcon(known[i.item])); if (auto c = itemColor(known[i.item].quality); c.isValid()) cell->setForeground(c); }
      results->setItem(r, 0, cell);
      results->setItem(r, 1, new QTableWidgetItem(i.expected < 0 ? QString("via shared table") : percent(i.expected)));
      results->setItem(r, 2, new QTableWidgetItem(percent(s.kills ? double(i.kills) / s.kills : 0) + " observed"));
      results->setItem(r, 3, new QTableWidgetItem(i.kills ? QString::number(double(i.count) / i.kills, 'f', 1) : QString("—")));
    }
  };
  QObject::connect(run, &QPushButton::clicked, &dialog, simulate);
  simulate();
  dialog.exec();
}
class LootEditor final : public QDialog {
public:
  LootEditor(QWidget* parent, World* world, LootOwner owner) : QDialog(parent), _owner(owner) {
    bool npc = owner.kind == LootOwner::Kind::Npc;
    setWindowTitle("Loot"); resize(1440, 800);
    auto layout = new QVBoxLayout(this);
    _title = editorTitle({}, this); layout->addWidget(_title);
    _notice = noticeBanner({}, this); layout->addWidget(_notice);
    auto split = new QSplitter(this); layout->addWidget(split, 1);

    auto left = new QWidget; auto leftLayout = new QVBoxLayout(left); leftLayout->setContentsMargins(0, 0, 0, 0);
    if (npc) { auto preview = new NpcPreview(world, left); preview->setFixedHeight(260); preview->showNpc(owner.entry); leftLayout->addWidget(preview); }
    else { _objectPreview = new ObjectPreview(left); _objectPreview->setFixedHeight(220); leftLayout->addWidget(_objectPreview); }
    _details = new ItemDetails(left); leftLayout->addWidget(_details, 1);
    split->addWidget(left);

    auto center = new QWidget; auto centerLayout = new QVBoxLayout(center); centerLayout->setContentsMargins(0, 0, 0, 0);
    auto money = new QHBoxLayout; centerLayout->addLayout(money);
    money->addWidget(new QLabel(npc ? "Money dropped:" : "Money inside:", center));
    _moneyMin = new MoneyEdit(center); _moneyMax = new MoneyEdit(center);
    money->addWidget(_moneyMin); money->addWidget(new QLabel("to", center)); money->addWidget(_moneyMax); money->addStretch();
    _table = new ItemDropTable(0, Columns, center);
    _table->setHorizontalHeaderLabels({"Item", "Chance", "Always", "Min", "Max", "Group", "Per kill", "Notes", ""});
    _table->horizontalHeaderItem(GroupColumn)->setToolTip("Items in the same group never drop together: exactly one of them drops (if the chances allow).\n0% items in a group share what the others leave.");
    _table->horizontalHeaderItem(PerKillColumn)->setToolTip("How often it drops per kill, groups considered.");
    _table->horizontalHeader()->setSectionResizeMode(ItemColumn, QHeaderView::Stretch);
    _table->horizontalHeader()->setSectionResizeMode(NotesColumn, QHeaderView::Stretch);
    for (int c : {ChanceColumn, AlwaysColumn, MinColumn, MaxColumn, GroupColumn, PerKillColumn, RemoveColumn}) _table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    _table->verticalHeader()->hide(); _table->setSelectionBehavior(QAbstractItemView::SelectRows); _table->setSelectionMode(QAbstractItemView::SingleSelection);
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers); _table->setAcceptDrops(true); _table->setDragDropMode(QAbstractItemView::DropOnly);
    _table->setIconSize(QSize(28, 28)); _table->verticalHeader()->setDefaultSectionSize(36);
    _table->onItemsDropped = [this](QVector<Id> const& items) { addItems(items); };
    centerLayout->addWidget(_table, 1);
    _listProblems = new QLabel(center); _listProblems->setStyleSheet("color: #e04040;"); _listProblems->setWordWrap(true); centerLayout->addWidget(_listProblems);
    _summary = new QLabel(center); _summary->setStyleSheet("color: gray;"); centerLayout->addWidget(_summary);
    auto actions = new QHBoxLayout; centerLayout->addLayout(actions);
    _add = button("Add Item", Icon::plus, center); _copyFrom = button("Copy Loot…", Icon::paste, center);
    _copyTo = button("Use on Another…", Icon::copy, center); _clear = button("Clear", Icon::eraser, center); _reset = button("Reset", Icon::undo, center);
    for (auto b : {_add, _copyFrom, _copyTo, _clear, _reset}) actions->addWidget(b);
    actions->addStretch();
    _add->setToolTip("Pick an item to add. You can also drag items from the browser on the right.");
    _copyFrom->setToolTip("Copy the loot of another NPC or chest into this one.");
    _copyTo->setToolTip("Give one of your own NPCs or chests this exact loot.");
    split->addWidget(center);

    auto right = new QWidget; auto rightLayout = new QVBoxLayout(right); rightLayout->setContentsMargins(0, 0, 0, 0);
    auto hint = new QLabel("<b>Items</b> — drag into the loot, or double-click", right); rightLayout->addWidget(hint);
    _browser = new ItemBrowser({}, true, right); rightLayout->addWidget(_browser, 1);
    _browser->onActivated = [this](Id item) { addItems({item}); };
    _browser->onCurrent = [this](std::optional<ItemInfo> const& item) { _details->setItem(item); };
    split->addWidget(right);
    split->setSizes({300, 760, 380});

    auto bottom = new QHBoxLayout; layout->addLayout(bottom);
    auto simulate = button("Simulate Drops", Icon::dice, this), test = button(npc ? "Test Loot…" : "Test Chest…", Icon::play, this);
    bottom->addWidget(simulate); bottom->addWidget(test); bottom->addStretch();
    auto close = new QPushButton("Close", this); _save = new QPushButton("Save", this); _save->setDefault(true);
    bottom->addWidget(close); bottom->addWidget(_save);

    _moneyMin->onChanged = [this] { _loot.moneyMin = _moneyMin->value(); changed(); };
    _moneyMax->onChanged = [this] { _loot.moneyMax = _moneyMax->value(); changed(); };
    connect(_table, &QTableWidget::itemSelectionChanged, this, [this] {
      auto row = _table->currentRow();
      if (row >= 0 && row < _loot.rows.size() && _items.contains(_loot.rows[row].item)) _details->setItem(_items[_loot.rows[row].item]);
    });
    connect(_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) { if (column == ItemColumn) changeItem(row); });
    connect(_add, &QPushButton::clicked, this, [this] { if (auto item = pickItem(this, "Add to loot")) addItems({*item}); });
    connect(_copyFrom, &QPushButton::clicked, this, [this] { copyFrom(); });
    connect(_copyTo, &QPushButton::clicked, this, [this] { copyTo(); });
    connect(_clear, &QPushButton::clicked, this, [this] { clearLoot(); });
    connect(_reset, &QPushButton::clicked, this, [this] {
      if (_dirty && QMessageBox::question(this, "Reset", "Discard your changes and reload the saved loot?") != QMessageBox::Yes) return;
      load();
    });
    connect(simulate, &QPushButton::clicked, this, [this] { showSimulation(this, _loot, _references, _items); });
    connect(test, &QPushButton::clicked, this, [this] { testLoot(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(_save, &QPushButton::clicked, this, [this] { if (save()) accept(); });
    load();
  }
  bool saved = false;
  void reject() override { if (confirmClose(this, _dirty, [this] { return save(); })) QDialog::reject(); }
private:
  bool editable() const { return _loot.editable && _loot.lootable; }
  void load() {
    try { _loot = LootService::load(_owner); _references = LootService::references(_loot); }
    catch (std::exception const& e) { QMessageBox::warning(this, "Loot", QString::fromUtf8(e.what())); return; }
    _title->setText((_loot.ownerName + (_owner.kind == LootOwner::Kind::Npc ? " — Loot" : " — Contents")).toUpper());
    _notice->setText(_loot.notice); _notice->setVisible(!_loot.notice.isEmpty());
    if (_objectPreview) _objectPreview->display(_loot.display);
    fetchItems();
    _moneyMin->setValue(_loot.moneyMin); _moneyMax->setValue(_loot.moneyMax);
    for (auto w : std::initializer_list<QWidget*>{_add, _clear, _reset, _save}) w->setVisible(editable());
    _moneyMin->setReadOnly(!editable()); _moneyMax->setReadOnly(!editable());
    _copyFrom->setVisible(editable());
    _copyTo->setText(editable() ? "Use on Another…" : (_owner.kind == LootOwner::Kind::Npc ? "Copy to My NPC…" : "Copy to My Chest…"));
    _dirty = false; rebuild();
  }
  void fetchItems() {
    QVector<Id> missing;
    for (auto const& r : _loot.rows) if (r.item && !r.reference && !_items.contains(r.item)) missing << r.item;
    for (auto const& rows : _references) for (auto const& r : rows) if (r.item && !r.reference && !_items.contains(r.item)) missing << r.item;
    if (missing.isEmpty()) return;
    try { auto found = ItemService::get(missing); for (auto it = found.begin(); it != found.end(); ++it) _items[it.key()] = *it; } catch (...) {}
  }
  void rebuild() {
    _building = true;
    _table->setRowCount(_loot.rows.size());
    for (int i = 0; i < _loot.rows.size(); ++i) buildRow(i);
    _building = false;
    refreshChecks();
  }
  void buildRow(int i) {
    auto const r = _loot.rows[i];
    bool locked = !editable() || r.quest();
    auto cell = new QTableWidgetItem;
    if (r.reference) {
      cell->setText(QString("Shared loot table #%1 (%2 items)").arg(r.reference).arg(_references.value(r.reference).size()));
      cell->setIcon(Ui::FontAwesomeIcon(Icon::book)); cell->setToolTip("A loot table several original creatures share. Group 0 rolls the whole table; another group selects only that group in the shared table.");
    } else if (_items.contains(r.item)) {
      auto const& info = _items[r.item];
      cell->setText(info.name + (r.quest() ? "  (quest drop)" : QString())); cell->setIcon(itemIcon(info)); cell->setToolTip(itemTooltip(info));
      if (auto c = itemColor(info.quality); c.isValid()) cell->setForeground(c);
    } else cell->setText(r.item ? QString("Unknown item %1").arg(r.item) : QString("Choose an item…"));
    cell->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    _table->setItem(i, ItemColumn, cell);

    auto chance = new QDoubleSpinBox; chance->setRange(0, 100); chance->setDecimals(2); chance->setSuffix("%");
    chance->setValue(std::abs(r.chance)); chance->setEnabled(!locked && r.chance < 100);
    if (r.quest()) chance->setToolTip("Quest drops are set in the quest editor: they only drop while the quest is active.");
    connect(chance, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, i](double v) { if (!_building) { _loot.rows[i].chance = v; changed(); } });
    _table->setCellWidget(i, ChanceColumn, chance);

    auto always = new QCheckBox; always->setChecked(r.chance >= 100); always->setEnabled(!locked && (r.reference || r.group == 0));
    always->setToolTip("Guaranteed: drops every time.");
    connect(always, &QCheckBox::toggled, this, [this, i, chance](bool on) {
      if (_building) return;
      _loot.rows[i].chance = on ? 100 : (chance->value() >= 100 ? 50 : chance->value());
      chance->setEnabled(!on); { QSignalBlocker block(chance); chance->setValue(_loot.rows[i].chance); }
      changed();
    });
    _table->setCellWidget(i, AlwaysColumn, centered(always));

    auto min = new QSpinBox, max = new QSpinBox;
    min->setRange(1, 255); max->setRange(1, 255); min->setValue(std::max(1, r.minCount)); max->setValue(std::max(1, r.maxCount));
    if (r.reference) { min->setVisible(false); max->setPrefix("×"); max->setToolTip("How many times the shared table is rolled."); }
    min->setEnabled(!locked); max->setEnabled(!locked);
    connect(min, qOverload<int>(&QSpinBox::valueChanged), this, [this, i](int v) { if (!_building) { _loot.rows[i].minCount = v; changed(); } });
    connect(max, qOverload<int>(&QSpinBox::valueChanged), this, [this, i](int v) { if (!_building) { _loot.rows[i].maxCount = v; changed(); } });
    _table->setCellWidget(i, MinColumn, min); _table->setCellWidget(i, MaxColumn, max);

    auto group = new QSpinBox; group->setRange(0, 127); group->setSpecialValueText("—"); group->setValue(r.group); group->setEnabled(!locked);
    group->setToolTip(r.reference ? "0 / —: roll the whole shared table. 1–127: roll only that group in the shared table."
                                 : "0 / —: rolled on its own. 1–127: at most one item in the group drops.");
    connect(group, qOverload<int>(&QSpinBox::valueChanged), this, [this, i, always](int v) {
      if (_building) return;
      _loot.rows[i].group = v; always->setEnabled((v == 0 || _loot.rows[i].reference) && editable()); changed();
    });
    _table->setCellWidget(i, GroupColumn, group);

    auto remove = new QToolButton; remove->setIcon(Ui::FontAwesomeIcon(Icon::trash)); remove->setAutoRaise(true); remove->setToolTip("Remove from the loot");
    remove->setEnabled(!locked);
    connect(remove, &QToolButton::clicked, this, [this, i] { _loot.rows.removeAt(i); _dirty = true; rebuild(); });
    _table->setCellWidget(i, RemoveColumn, remove);
  }
  void changed() { _dirty = true; refreshChecks(); }
  void refreshChecks() {
    QSet<Id> existing; for (auto it = _items.begin(); it != _items.end(); ++it) existing.insert(it.key());
    QSet<Id> references; for (auto it = _references.begin(); it != _references.end(); ++it) if (!it->isEmpty()) references.insert(it.key());
    auto problems = LootService::check(_loot, existing, references);
    double expectedItems = 0;
    for (int i = 0; i < _loot.rows.size(); ++i) {
      auto chance = LootService::expectedChance(_loot.rows, i);
      auto const& r = _loot.rows[i];
      if (!r.quest() && !r.reference) expectedItems += chance;
      auto cell = new QTableWidgetItem((r.group > 0 && r.chance == 0 ? "≈ " : "") + percent(chance)); cell->setFlags(Qt::ItemIsEnabled);
      cell->setTextAlignment(Qt::AlignCenter);
      _table->setItem(i, PerKillColumn, cell);
      showRowProblems(_table, i, NotesColumn, problems);
    }
    _listProblems->setText(listProblems(problems)); _listProblems->setVisible(!_listProblems->text().isEmpty());
    _summary->setText(QString("About %1 items per %2%3").arg(expectedItems, 0, 'f', 1).arg(_owner.kind == LootOwner::Kind::Npc ? "kill" : "opening")
                        .arg(_loot.moneyMax > 0 ? QString(", plus %1 – %2").arg(formatMoney(_loot.moneyMin), formatMoney(_loot.moneyMax)) : QString()));
    _save->setEnabled(editable() && !hasErrors(problems));
    _save->setToolTip(hasErrors(problems) ? "Fix the problems marked ✗ first." : QString());
  }
  void addItems(QVector<Id> const& items) {
    if (!editable()) return;
    for (auto id : items) { LootRow r; r.item = id; r.chance = 25; _loot.rows.push_back(r); try { ItemService::used(id); } catch (...) {} }
    fetchItems(); _dirty = true; rebuild();
    _table->selectRow(_loot.rows.size() - 1);
  }
  void changeItem(int row) {
    if (!editable() || _loot.rows[row].quest() || _loot.rows[row].reference) return;
    if (auto item = pickItem(this, "Change item", {}, _loot.rows[row].item)) { _loot.rows[row].item = *item; fetchItems(); _dirty = true; rebuild(); _table->selectRow(row); }
  }
  QVector<LootRow> ownRows(QVector<LootRow> const& rows) const { QVector<LootRow> out; for (auto const& r : rows) if (!r.quest()) out.push_back(r); return out; }
  void copyFrom() {
    auto source = chooseOwner(this, "Copy loot from", [this](QString const& text) { return LootService::owners(text, _owner.kind); });
    if (!source) return;
    LootTable from;
    try { from = LootService::load({_owner.kind, source->id}); } catch (std::exception const& e) { QMessageBox::warning(this, "Copy Loot", e.what()); return; }
    auto rows = ownRows(from.rows);
    if (rows.isEmpty()) { QMessageBox::information(this, "Copy Loot", source->name + " drops nothing to copy."); return; }
    QMessageBox ask(QMessageBox::Question, "Copy Loot", QString("Copy %1 items from %2.").arg(rows.size()).arg(source->name), QMessageBox::Cancel, this);
    auto replace = ask.addButton("Replace current loot", QMessageBox::AcceptRole), add = ask.addButton("Add to it", QMessageBox::AcceptRole);
    ask.exec();
    if (ask.clickedButton() != replace && ask.clickedButton() != add) return;
    if (ask.clickedButton() == replace) {
      QVector<LootRow> kept; for (auto const& r : _loot.rows) if (r.quest()) kept.push_back(r);
      _loot.rows = kept; _loot.moneyMin = from.moneyMin; _loot.moneyMax = from.moneyMax;
      _moneyMin->setValue(_loot.moneyMin); _moneyMax->setValue(_loot.moneyMax);
    }
    _loot.rows += rows;
    try { auto refs = LootService::references(_loot); for (auto it = refs.begin(); it != refs.end(); ++it) _references[it.key()] = *it; } catch (...) {}
    fetchItems(); _dirty = true; rebuild();
  }
  void copyTo() {
    auto target = chooseOwner(this, _owner.kind == LootOwner::Kind::Npc ? "Give this loot to one of your NPCs" : "Give this loot to one of your chests",
                              [this](QString const& text) { return LootService::owners(text, _owner.kind, true); });
    if (!target || target->id == _owner.entry) return;
    if (QMessageBox::question(this, "Use on Another", "Replace the loot of " + target->name + " with this loot?\n\nQuest drops it already has are kept.")
        != QMessageBox::Yes) return;
    try {
      auto other = LootService::load({_owner.kind, target->id});
      QVector<LootRow> rows; for (auto const& r : other.rows) if (r.quest()) rows.push_back(r);
      other.rows = rows + ownRows(_loot.rows); other.moneyMin = _loot.moneyMin; other.moneyMax = _loot.moneyMax;
      LootService::save(other); saved = true;
      QMessageBox::information(this, "Use on Another", target->name + " now has this loot. It is listed in Local Changes.");
    } catch (std::exception const& e) { QMessageBox::warning(this, "Use on Another", e.what()); }
  }
  void clearLoot() {
    if (QMessageBox::question(this, "Clear", "Remove every item from this loot? Quest drops stay. Nothing is saved until you click Save.") != QMessageBox::Yes) return;
    QVector<LootRow> kept; for (auto const& r : _loot.rows) if (r.quest()) kept.push_back(r);
    _loot.rows = kept; _dirty = true; rebuild();
  }
  bool save() {
    try {
      auto problems = LootService::validate(_loot);
      if (hasErrors(problems)) { refreshChecks(); QMessageBox::warning(this, "Loot", "Fix the problems marked ✗ first."); return false; }
      LootService::save(_loot);
    } catch (std::exception const& e) { QMessageBox::warning(this, "Loot", QString::fromUtf8(e.what())); return false; }
    _dirty = false; saved = true;
    return true;
  }
  void testLoot() {
    auto tests = TestSessionService::instance();
    if (!tests) return;
    if (_dirty) {
      if (QMessageBox::question(this, "Test", "Save the loot before testing?") != QMessageBox::Yes || !save()) return;
    }
    QDialog options(this); options.setWindowTitle(_owner.kind == LootOwner::Kind::Npc ? "Test Loot" : "Test Chest");
    auto layout = new QVBoxLayout(&options);
    auto text = new QLabel("Your local test character is placed beside " + _loot.ownerName + ". The local server restarts, which also resets it.");
    text->setWordWrap(true); layout->addWidget(text);
    auto quick = new QCheckBox("Repeat quickly: let my local account use .respawn to reset it in game (no restart needed)");
    quick->setChecked(true); layout->addWidget(quick);
    auto note = new QLabel("Only changes the local test server's account."); note->setStyleSheet("color: gray;"); layout->addWidget(note);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); buttons->addButton("Test", QDialogButtonBox::AcceptRole); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &options, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &options, &QDialog::reject);
    if (options.exec() != QDialog::Accepted) return;
    TestOptions test; test.developer = quick->isChecked();
    tests->testEntity(this, _owner.kind == LootOwner::Kind::Object, _owner.entry, test);
  }
  LootOwner _owner;
  LootTable _loot;
  QHash<Id, QVector<LootRow>> _references;
  QHash<Id, ItemInfo> _items;
  bool _dirty = false, _building = false;
  QLabel *_title, *_notice, *_listProblems, *_summary;
  ObjectPreview* _objectPreview = nullptr;
  ItemDetails* _details;
  MoneyEdit *_moneyMin, *_moneyMax;
  ItemDropTable* _table;
  ItemBrowser* _browser;
  QPushButton *_add, *_copyFrom, *_copyTo, *_clear, *_reset, *_save;
};
}
bool editLoot(QWidget* parent, World* world, LootOwner const& owner) {
  LootEditor editor(parent, world, owner);
  editor.exec();
  return editor.saved;
}
}
