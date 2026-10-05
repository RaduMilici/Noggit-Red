#include "ContentEditors.hpp"
#include "CreatorPreviews.hpp"
#include "EditorWidgets.hpp"
#include "ItemBrowser.hpp"
#include "ItemDesignService.hpp"
#include "SpellService.hpp"
#include "TestSessionService.hpp"
#include <noggit/ui/FontAwesome.hpp>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
namespace Noggit::Creator {
namespace {
using Icon = Ui::FontAwesome::Icons;
QPushButton* button(QString const& text, Icon icon, QWidget* parent = nullptr) { return new QPushButton(Ui::FontAwesomeIcon(icon), text, parent); }
QLabel* hint(QString const& text) { auto l = new QLabel(text); l->setWordWrap(true); l->setStyleSheet("color: palette(mid); font-style: italic;"); return l; }
void choose(QComboBox* box, int value) { if (auto i = box->findData(value); i >= 0) box->setCurrentIndex(i); }
QVector<int> const statTypes{4, 3, 7, 5, 6, 1, 0}; // Strength, Agility, Stamina, Intellect, Spirit, Health, Mana
class ItemEditor final : public QDialog {
public:
  ItemEditor(QWidget* parent, ItemDesign design) : QDialog(parent), _d(std::move(design)) {
    setWindowTitle("Item Editor"); resize(1180, 760);
    auto layout = new QVBoxLayout(this);
    _title = editorTitle({}, this); layout->addWidget(_title);
    _notice = noticeBanner({}, this); layout->addWidget(_notice);
    auto body = new QHBoxLayout; layout->addLayout(body, 1);

    auto left = new QVBoxLayout; body->addLayout(left, 2);
    _model = new ItemPreview(this); _model->setFixedHeight(230); left->addWidget(_model);
    _modelNote = hint("No separate 3D model: the game draws this item on the character or in bags."); left->addWidget(_modelNote);
    auto card = new QFrame(this); card->setObjectName("Tooltip");
    card->setStyleSheet("#Tooltip { background: #0b0b1e; border: 1px solid #5a5a7a; border-radius: 4px; } QLabel { color: #ffffff; background: transparent; }");
    auto cardLayout = new QHBoxLayout(card); _icon = new QLabel(card); _icon->setFixedSize(44, 44); cardLayout->addWidget(_icon, 0, Qt::AlignTop);
    _tooltip = new QLabel(card); _tooltip->setTextFormat(Qt::RichText); _tooltip->setWordWrap(true); _tooltip->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    cardLayout->addWidget(_tooltip, 1); left->addWidget(card, 1);

    _tabs = new QTabWidget(this); body->addWidget(_tabs, 3);
    _tabs->addTab(basics(), "Basics"); _tabs->addTab(combat(), "Combat"); _tabs->addTab(statsPage(), "Stats");
    _tabs->addTab(requirements(), "Who can use it"); _tabs->addTab(spellsPage(), "Spells");

    _problems = new QLabel(this); _problems->setWordWrap(true); layout->addWidget(_problems);
    auto bottom = new QHBoxLayout; layout->addLayout(bottom);
    auto clone = button("Clone Existing Item…", Icon::clone, this), test = button("Test Item…", Icon::play, this);
    _delete = button("Delete Item…", Icon::trash, this);
    _copy = button("Clone This Item", Icon::copy, this); _copy->setStyleSheet("font-weight: bold;");
    for (auto b : {clone, test, _delete, _copy}) bottom->addWidget(b);
    bottom->addStretch();
    auto close = new QPushButton("Close", this); _save = new QPushButton("Save", this); _save->setDefault(true);
    bottom->addWidget(close); bottom->addWidget(_save);
    connect(clone, &QPushButton::clicked, this, [this] { cloneOther(); });
    connect(_copy, &QPushButton::clicked, this, [this] { cloneThis(); });
    connect(test, &QPushButton::clicked, this, [this] { testThis(); });
    connect(_delete, &QPushButton::clicked, this, [this] { deleteThis(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(_save, &QPushButton::clicked, this, [this] { save(); });
    _effectsTimer.setSingleShot(true); _effectsTimer.setInterval(300);
    connect(&_effectsTimer, &QTimer::timeout, this, [this] { refreshEffects(); });
    show(_d);
  }
  std::optional<Id> saved;
  void reject() override { if (confirmClose(this, _dirty, [this] { return save(); })) QDialog::reject(); }
private:
  // --- pages ---
  QWidget* scroll(QWidget* page) { auto area = new QScrollArea; area->setWidgetResizable(true); area->setFrameShape(QFrame::NoFrame); area->setWidget(page); return area; }
  QWidget* basics() {
    auto page = new QWidget; auto form = new QFormLayout(page);
    _name = new QLineEdit; _name->setMaxLength(100); form->addRow("Name", _name);
    _description = new QPlainTextEdit; _description->setMaximumHeight(60); _description->setPlaceholderText("Flavor text shown in yellow under the item.");
    form->addRow("Description", _description);
    _quality = new QComboBox;
    for (int q = 0; q <= 6; ++q) { _quality->addItem(ItemService::qualityName(q), q); if (auto c = itemColor(q); c.isValid()) _quality->setItemData(q, c, Qt::ForegroundRole); }
    form->addRow("Quality", _quality);
    auto levels = new QHBoxLayout;
    _itemLevel = new QSpinBox; _itemLevel->setRange(0, 255); _requiredLevel = new QSpinBox; _requiredLevel->setRange(0, 60); _requiredLevel->setSpecialValueText("none");
    levels->addWidget(new QLabel("Item level")); levels->addWidget(_itemLevel); levels->addSpacing(16); levels->addWidget(new QLabel("Requires level")); levels->addWidget(_requiredLevel); levels->addStretch();
    form->addRow("Levels", levels);
    _class = new QComboBox; for (auto const& c : ItemService::classes()) _class->addItem(c.second, c.first);
    _subclass = new QComboBox; _slot = new QComboBox;
    auto kind = new QHBoxLayout; kind->addWidget(_class, 1); kind->addWidget(_subclass, 1); form->addRow("Kind", kind);
    form->addRow("Worn", _slot);
    _bonding = new QComboBox;
    for (int b = 0; b <= 4; ++b) _bonding->addItem(b ? ItemService::bondingText(b) : QString("Never binds (can be traded)"), b);
    form->addRow("Binding", _bonding);
    auto stacking = new QHBoxLayout;
    _stack = new QSpinBox; _stack->setRange(1, 1000); _maxCount = new QSpinBox; _maxCount->setRange(0, 1000); _maxCount->setSpecialValueText("no limit");
    stacking->addWidget(new QLabel("Stacks up to")); stacking->addWidget(_stack); stacking->addSpacing(16); stacking->addWidget(new QLabel("A player can carry")); stacking->addWidget(_maxCount); stacking->addStretch();
    form->addRow("Stacking", stacking);
    _buy = new MoneyEdit; _sell = new MoneyEdit; _buyCount = new QSpinBox; _buyCount->setRange(1, 200); _buyCount->setPrefix("in stacks of ");
    auto suggest = new QToolButton; suggest->setText("¼ of buy price"); suggest->setToolTip("What the game's own items usually sell for");
    auto buyRow = new QHBoxLayout; buyRow->addWidget(_buy); buyRow->addWidget(_buyCount); buyRow->addStretch(); form->addRow("Vendors sell it for", buyRow);
    auto sellRow = new QHBoxLayout; sellRow->addWidget(_sell); sellRow->addWidget(suggest); sellRow->addStretch(); form->addRow("Vendors buy it for", sellRow);
    auto look = new QHBoxLayout;
    _lookIcon = new QLabel; _lookIcon->setFixedSize(36, 36); _lookName = new QLabel;
    auto pickLook = button("Use the Look of Another Item…", Icon::paintbrush);
    look->addWidget(_lookIcon); look->addWidget(_lookName, 1); look->addWidget(pickLook); form->addRow("Look", look);
    form->addRow(hint("Items use a look (icon and model) the game already has; custom models are not supported."));
    connect(_class, qOverload<int>(&QComboBox::activated), this, [this] { _d.itemClass = _class->currentData().toInt(); _d.subclass = 0; fillSubclasses(); fillSlots(); edited(); });
    connect(_subclass, qOverload<int>(&QComboBox::activated), this, [this] { _d.subclass = _subclass->currentData().toInt(); fillSlots(); edited(); });
    connect(suggest, &QToolButton::clicked, this, [this] { _sell->setValue(_buy->value() / 4); edited(); });
    connect(pickLook, &QPushButton::clicked, this, [this] {
      if (auto other = pickItem(this, "Use the look of", {}, 0)) if (auto info = ItemService::get(*other)) {
        _d.display = info->display; _lookFrom = info->name;
        if (!equippedLook(*info)) _lookFrom += " (its icon; it is not worn)";
        edited();
      }
    });
    return scroll(page);
  }
  static bool equippedLook(ItemInfo const& info) { return info.inventoryType > 0; }
  QWidget* combat() {
    auto page = new QWidget; auto form = new QFormLayout(page);
    _armor = new QSpinBox; _armor->setRange(0, 20000); form->addRow("Armor", _armor);
    _block = new QSpinBox; _block->setRange(0, 5000); form->addRow("Block (shields)", _block);
    _weaponBox = new QGroupBox("Weapon"); auto weapon = new QFormLayout(_weaponBox);
    _damageMin = new QDoubleSpinBox; _damageMax = new QDoubleSpinBox;
    for (auto s : {_damageMin, _damageMax}) { s->setRange(0, 10000); s->setDecimals(0); }
    auto damage = new QHBoxLayout; damage->addWidget(_damageMin); damage->addWidget(new QLabel("–")); damage->addWidget(_damageMax);
    _damageType = new QComboBox; for (int s = 0; s <= 6; ++s) _damageType->addItem(ItemService::schoolName(s), s);
    damage->addWidget(_damageType); damage->addStretch(); weapon->addRow("Damage", damage);
    _speed = new QDoubleSpinBox; _speed->setRange(0.5, 10); _speed->setDecimals(2); _speed->setSuffix(" sec"); weapon->addRow("Speed", _speed);
    _dps = new QLabel; weapon->addRow("Damage per second", _dps);
    form->addRow(_weaponBox);
    _durability = new QSpinBox; _durability->setRange(0, 1000); _durability->setSpecialValueText("none (never breaks)"); form->addRow("Durability", _durability);
    return scroll(page);
  }
  QWidget* statsPage() {
    auto page = new QWidget; auto v = new QVBoxLayout(page);
    v->addWidget(new QLabel("<b>Stats</b> (up to 10)"));
    _statRows = new QVBoxLayout; v->addLayout(_statRows);
    auto add = button("Add Stat", Icon::plus); v->addWidget(add, 0, Qt::AlignLeft);
    connect(add, &QPushButton::clicked, this, [this] { if (_d.stats.size() < 10) { _d.stats.push_back({7, 1}); buildStats(); edited(); } });
    auto box = new QGroupBox("Resistances"); auto grid = new QGridLayout(box);
    QStringList names{"Holy", "Fire", "Nature", "Frost", "Shadow", "Arcane"};
    for (int k = 0; k < 6; ++k) {
      _resist[k] = new QSpinBox; _resist[k]->setRange(-255, 255);
      grid->addWidget(new QLabel(names[k]), k / 3, (k % 3) * 2); grid->addWidget(_resist[k], k / 3, (k % 3) * 2 + 1);
      connect(_resist[k], qOverload<int>(&QSpinBox::valueChanged), this, [this, k](int value) { if (!_building) { _d.resistances[k] = value; edited(); } });
    }
    v->addWidget(box); v->addStretch();
    return scroll(page);
  }
  QWidget* requirements() {
    auto page = new QWidget; auto v = new QVBoxLayout(page);
    auto group = [&](QString const& title, QVector<QPair<int, QString>> const& choices, QCheckBox*& everyone, QVector<QCheckBox*>& boxes, int ItemDesign::*mask) {
      auto box = new QGroupBox(title); auto grid = new QGridLayout(box);
      everyone = new QCheckBox("Everyone"); grid->addWidget(everyone, 0, 0, 1, 3);
      for (int i = 0; i < choices.size(); ++i) {
        auto check = new QCheckBox(choices[i].second); check->setProperty("bit", choices[i].first); boxes << check; grid->addWidget(check, 1 + i / 3, i % 3);
        connect(check, &QCheckBox::toggled, this, [this, &boxes, mask] {
          if (_building) return;
          int value = 0; for (auto b : boxes) if (b->isChecked()) value |= b->property("bit").toInt();
          _d.*mask = value; edited();
        });
      }
      connect(everyone, &QCheckBox::toggled, this, [this, &boxes, mask](bool on) {
        if (_building) return;
        _d.*mask = on ? -1 : 0; for (auto b : boxes) b->setEnabled(!on);
        if (!on) { int value = 0; for (auto b : boxes) if (b->isChecked()) value |= b->property("bit").toInt(); _d.*mask = value; }
        edited();
      });
      v->addWidget(box);
    };
    group("Classes", ItemDesignService::classMask(), _allClasses, _classBoxes, &ItemDesign::classes);
    group("Races", ItemDesignService::raceMask(), _allRaces, _raceBoxes, &ItemDesign::races);
    v->addStretch();
    return scroll(page);
  }
  QWidget* spellsPage() {
    auto page = new QWidget; auto v = new QVBoxLayout(page);
    v->addWidget(hint("Spells the item casts: when used (potions, trinkets), while worn (passive bonuses), or a chance when the weapon hits."));
    _spellRows = new QVBoxLayout; v->addLayout(_spellRows);
    auto add = button("Add Spell", Icon::plus); v->addWidget(add, 0, Qt::AlignLeft);
    connect(add, &QPushButton::clicked, this, [this] {
      for (auto& s : _d.spells) if (!s.spell) { if (auto spell = pickSpell(this, "Spell the item casts")) { s = {*spell, ItemSpell::Use, 0, 0, -1, 0, -1}; buildSpells(); edited(true); } return; }
      QMessageBox::information(this, "Spells", "An item holds at most five spells.");
    });
    v->addStretch();
    return scroll(page);
  }
  void buildStats() {
    while (auto item = _statRows->takeAt(0)) { if (auto w = item->widget()) w->deleteLater(); delete item; }
    for (int k = 0; k < _d.stats.size(); ++k) {
      auto row = new QWidget; auto h = new QHBoxLayout(row); h->setContentsMargins(0, 0, 0, 0);
      auto type = new QComboBox; for (auto t : statTypes) type->addItem(ItemService::statName(t), t);
      choose(type, _d.stats[k].first);
      auto value = new QSpinBox; value->setRange(-1000, 1000); value->setPrefix("+"); value->setValue(_d.stats[k].second);
      auto remove = new QToolButton; remove->setIcon(Ui::FontAwesomeIcon(Icon::trash)); remove->setAutoRaise(true);
      h->addWidget(type, 1); h->addWidget(value); h->addWidget(remove);
      _statRows->addWidget(row);
      connect(type, qOverload<int>(&QComboBox::activated), this, [this, k, type] { _d.stats[k].first = type->currentData().toInt(); edited(); });
      connect(value, qOverload<int>(&QSpinBox::valueChanged), this, [this, k](int v) { _d.stats[k].second = v; edited(); });
      connect(remove, &QToolButton::clicked, this, [this, k] { _d.stats.removeAt(k); QTimer::singleShot(0, this, [this] { buildStats(); }); edited(); });
    }
  }
  void buildSpells() {
    while (auto item = _spellRows->takeAt(0)) { if (auto w = item->widget()) w->deleteLater(); delete item; }
    for (int k = 0; k < 5; ++k) {
      auto& s = _d.spells[k];
      if (!s.spell) continue;
      auto box = new QGroupBox; auto grid = new QGridLayout(box);
      auto trigger = new QComboBox; trigger->addItem("When used", ItemSpell::Use); trigger->addItem("While worn", ItemSpell::Equip); trigger->addItem("Chance on hit", ItemSpell::ChanceOnHit);
      choose(trigger, s.trigger);
      auto name = new QLabel(spellName(s.spell)); name->setStyleSheet("font-weight: bold;");
      auto change = new QToolButton; change->setText("Change…"); auto open = new QToolButton; open->setText("Open"); open->setToolTip("Open the spell in the Spell Editor");
      auto remove = new QToolButton; remove->setIcon(Ui::FontAwesomeIcon(Icon::trash)); remove->setAutoRaise(true);
      grid->addWidget(trigger, 0, 0); grid->addWidget(name, 0, 1); grid->addWidget(change, 0, 2); grid->addWidget(open, 0, 3); grid->addWidget(remove, 0, 4);
      auto charges = new QSpinBox; charges->setRange(0, 100); charges->setSpecialValueText("unlimited"); charges->setValue(std::abs(s.charges));
      auto consumed = new QCheckBox("then the item is used up"); consumed->setChecked(s.charges < 0);
      auto cooldown = new QDoubleSpinBox; cooldown->setRange(-1, 86400); cooldown->setDecimals(1); cooldown->setSuffix(" sec"); cooldown->setSpecialValueText("the spell's own");
      cooldown->setValue(s.cooldown < 0 ? -1 : s.cooldown / 1000.0);
      auto ppm = new QDoubleSpinBox; ppm->setRange(0, 60); ppm->setDecimals(1); ppm->setSpecialValueText("the spell's own chance"); ppm->setSuffix(" per minute"); ppm->setValue(s.perMinute);
      grid->addWidget(new QLabel("Uses"), 1, 0); grid->addWidget(charges, 1, 1); grid->addWidget(consumed, 1, 2, 1, 3);
      grid->addWidget(new QLabel("Cooldown"), 2, 0); grid->addWidget(cooldown, 2, 1);
      grid->addWidget(new QLabel("Procs"), 3, 0); grid->addWidget(ppm, 3, 1);
      bool use = s.trigger == ItemSpell::Use, proc = s.trigger == ItemSpell::ChanceOnHit;
      for (auto w : std::initializer_list<QWidget*>{charges, consumed, cooldown}) w->setEnabled(use);
      ppm->setEnabled(proc);
      _spellRows->addWidget(box);
      auto apply = [this, k, charges, consumed, cooldown, ppm] {
        auto& s = _d.spells[k];
        s.charges = charges->value() * (consumed->isChecked() ? -1 : 1);
        s.cooldown = cooldown->value() < 0 ? -1 : int(cooldown->value() * 1000); s.perMinute = ppm->value();
        edited();
      };
      connect(charges, qOverload<int>(&QSpinBox::valueChanged), this, apply); connect(consumed, &QCheckBox::toggled, this, apply);
      connect(cooldown, qOverload<double>(&QDoubleSpinBox::valueChanged), this, apply); connect(ppm, qOverload<double>(&QDoubleSpinBox::valueChanged), this, apply);
      connect(trigger, qOverload<int>(&QComboBox::activated), this, [this, k, trigger] { _d.spells[k].trigger = trigger->currentData().toInt(); later(); edited(true); });
      connect(change, &QToolButton::clicked, this, [this, k] { if (auto spell = pickSpell(this, "Spell the item casts", _d.spells[k].spell)) { _d.spells[k].spell = *spell; later(); edited(true); } });
      connect(open, &QToolButton::clicked, this, [this, k] { if (editSpell(this, _d.spells[k].spell)) { _spellNames.clear(); later(); edited(true); } });
      connect(remove, &QToolButton::clicked, this, [this, k] { _d.spells[k] = {}; later(); edited(true); });
    }
  }
  void later() { QTimer::singleShot(0, this, [this] { buildSpells(); }); }
  QString spellName(Id spell) {
    if (!_spellNames.contains(spell)) {
      auto found = SpellService::search(QString::number(spell), false, 5);
      QString name = QString("Spell %1").arg(spell);
      for (auto const& c : found) if (c.id == spell) name = c.name;
      _spellNames[spell] = name;
    }
    return _spellNames[spell];
  }
  void fillSubclasses() {
    _subclass->clear();
    for (auto const& s : ItemService::subclasses(_d.itemClass)) _subclass->addItem(s.second, s.first);
    if (_subclass->count() == 0) _subclass->addItem("—", 0);
    choose(_subclass, _d.subclass);
    _d.subclass = _subclass->currentData().toInt();
  }
  void fillSlots() {
    _slot->clear();
    for (auto const& s : ItemDesignService::slotsFor(_d.itemClass, _d.subclass)) _slot->addItem(s.second, s.first);
    if (_slot->findData(_d.inventoryType) < 0) _d.inventoryType = _slot->itemData(0).toInt();
    choose(_slot, _d.inventoryType);
  }
  // --- state ---
  void show(ItemDesign const& d) {
    _d = d; _building = true;
    _title->setText(((d.name.isEmpty() ? QString("New item") : d.name) + " — Item").toUpper());
    _notice->setText(d.notice); _notice->setVisible(!d.notice.isEmpty());
    _name->setText(d.name); _description->setPlainText(d.description); choose(_quality, d.quality);
    _itemLevel->setValue(d.itemLevel); _requiredLevel->setValue(d.requiredLevel);
    choose(_class, d.itemClass); fillSubclasses(); fillSlots(); choose(_bonding, d.bonding);
    _stack->setValue(d.stackable); _maxCount->setValue(d.maxCount); _buy->setValue(d.buyPrice); _sell->setValue(d.sellPrice); _buyCount->setValue(d.buyCount);
    _armor->setValue(d.armor); _block->setValue(d.block); _damageMin->setValue(d.damageMin); _damageMax->setValue(d.damageMax);
    choose(_damageType, d.damageType); _speed->setValue(std::max(0.5, d.delay / 1000.0)); _durability->setValue(d.durability);
    for (int k = 0; k < 6; ++k) _resist[k]->setValue(d.resistances[k]);
    auto masks = [](int value, QCheckBox* everyone, QVector<QCheckBox*> const& boxes) {
      everyone->setChecked(value == -1);
      for (auto b : boxes) { b->setChecked(value != -1 && (value & b->property("bit").toInt())); b->setEnabled(value != -1); }
    };
    masks(d.classes, _allClasses, _classBoxes); masks(d.races, _allRaces, _raceBoxes);
    _lookFrom.clear();
    buildStats(); buildSpells();
    for (auto w : std::initializer_list<QWidget*>{_tabs}) w->setEnabled(d.editable);
    _save->setVisible(d.editable); _delete->setVisible(d.editable && d.entry); _copy->setVisible(!d.editable);
    _building = false; _dirty = !d.entry && d.editable;
    connectFields();
    _effectsTimer.start(0);
    refresh();
  }
  void connectFields() {
    if (_connected) return;
    _connected = true;
    auto text = [this] { if (!_building) { _d.name = _name->text(); _d.description = _description->toPlainText(); edited(); } };
    connect(_name, &QLineEdit::textEdited, this, text); connect(_description, &QPlainTextEdit::textChanged, this, text);
    auto combo = [this](QComboBox* box, int ItemDesign::*field) { connect(box, qOverload<int>(&QComboBox::activated), this, [this, box, field] { _d.*field = box->currentData().toInt(); edited(); }); };
    combo(_quality, &ItemDesign::quality); combo(_slot, &ItemDesign::inventoryType); combo(_bonding, &ItemDesign::bonding); combo(_damageType, &ItemDesign::damageType);
    auto spin = [this](QSpinBox* box, int ItemDesign::*field) { connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [this, field](int v) { if (!_building) { _d.*field = v; edited(); } }); };
    spin(_itemLevel, &ItemDesign::itemLevel); spin(_requiredLevel, &ItemDesign::requiredLevel); spin(_stack, &ItemDesign::stackable); spin(_maxCount, &ItemDesign::maxCount);
    spin(_buyCount, &ItemDesign::buyCount); spin(_armor, &ItemDesign::armor); spin(_block, &ItemDesign::block); spin(_durability, &ItemDesign::durability);
    auto real = [this] { if (!_building) { _d.damageMin = _damageMin->value(); _d.damageMax = _damageMax->value(); _d.delay = int(std::lround(_speed->value() * 1000)); edited(); } };
    for (auto s : {_damageMin, _damageMax, _speed}) connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this, real);
    _buy->onChanged = [this] { if (!_building) { _d.buyPrice = _buy->value(); edited(); } };
    _sell->onChanged = [this] { if (!_building) { _d.sellPrice = _sell->value(); edited(); } };
  }
  void edited(bool spells = false) {
    if (_building) return;
    _dirty = true;
    if (spells) _effectsTimer.start();
    refresh();
  }
  void refreshEffects() {
    _effects.clear();
    for (auto const& s : _d.spells) if (s.spell) try { _effects << ItemDesignService::effectText(s); } catch (...) {}
    try { _facts = ItemDesignService::facts(_d); } catch (...) {}
    refresh();
  }
  void refresh() {
    _title->setText(((_d.name.trimmed().isEmpty() ? QString("New item") : _d.name) + " — Item").toUpper());
    auto info = ItemDesignService::info(_d, _effects);
    _tooltip->setText(itemTooltip(info));
    _icon->setPixmap(itemIcon(info).pixmap(40, 40));
    _lookIcon->setPixmap(itemIcon(info).pixmap(32, 32));
    _lookName->setText(_lookFrom.isEmpty() ? (_d.display ? QString("Look %1").arg(_d.display) : QString("No look yet")) : "Looks like " + _lookFrom);
    _model->showItem(_d.display, _d.inventoryType);
    QTimer::singleShot(100, this, [this] { _modelNote->setVisible(!_model->hasModel()); });
    bool weapon = ItemDesignService::weapon(_d);
    _weaponBox->setVisible(weapon);
    _dps->setText(weapon && _d.delay > 0 ? QString::number((_d.damageMin + _d.damageMax) / 2 / (_d.delay / 1000.0), 'f', 1) : QString("—"));
    _block->setEnabled(_d.itemClass == 4 && _d.subclass == 6);
    auto problems = ItemDesignService::check(_d, _facts);
    QStringList lines; bool errors = false;
    for (auto const& p : problems) { lines << QString("%1 %2: %3").arg(p.error ? "✗" : "⚠", p.field, p.text); errors = errors || p.error; }
    _problems->setText(lines.join('\n')); _problems->setVisible(!lines.isEmpty());
    _problems->setStyleSheet(errors ? "color: #e04040;" : "color: #d08a00;");
    _save->setEnabled(_d.editable && !errors);
  }
  bool save() {
    try {
      _facts = ItemDesignService::facts(_d);
      auto problems = ItemDesignService::check(_d, _facts);
      if (std::any_of(problems.begin(), problems.end(), [](auto const& p) { return p.error; })) { refresh(); QMessageBox::warning(this, "Item", "Fix the problems marked ✗ first."); return false; }
      auto entry = ItemDesignService::save(_d);
      saved = entry;
      try { ItemService::used(entry); } catch (...) {}
      auto look = _lookFrom;
      show(ItemDesignService::load(entry)); _lookFrom = look; refresh();
      return true;
    } catch (std::exception const& e) { QMessageBox::warning(this, "Item", QString::fromUtf8(e.what())); return false; }
  }
  void cloneThis() {
    if (!_d.entry) return;
    try { show(ItemDesignService::clone(_d.entry)); _dirty = true; } catch (std::exception const& e) { QMessageBox::warning(this, "Item", e.what()); }
  }
  void cloneOther() {
    if (_dirty && QMessageBox::question(this, "Clone Existing Item", "Discard your changes to this item and start from a copy of another one?") != QMessageBox::Yes) return;
    if (auto source = pickItem(this, "Clone which item?")) try { show(ItemDesignService::clone(*source)); _dirty = true; } catch (std::exception const& e) { QMessageBox::warning(this, "Item", e.what()); }
  }
  void deleteThis() {
    if (QMessageBox::question(this, "Delete Item", "Delete " + _d.name + " from your local world? It is listed in Local Changes.") != QMessageBox::Yes) return;
    try { ItemDesignService::remove(_d.entry); saved = _d.entry; _dirty = false; accept(); } catch (std::exception const& e) { QMessageBox::warning(this, "Delete Item", e.what()); }
  }
  void testThis() {
    if (_d.editable && (_dirty || !_d.entry) && (QMessageBox::question(this, "Test Item", "Save the item before testing?") != QMessageBox::Yes || !save())) return;
    if (_d.entry) testItem(this, _d.entry);
  }
  ItemDesign _d;
  ItemFacts _facts;
  QStringList _effects;
  QHash<Id, QString> _spellNames;
  QString _lookFrom;
  bool _dirty = false, _building = false, _connected = false;
  QTimer _effectsTimer;
  QLabel *_title, *_notice, *_icon, *_tooltip, *_problems, *_modelNote, *_lookIcon = nullptr, *_lookName = nullptr, *_dps = nullptr;
  ItemPreview* _model;
  QTabWidget* _tabs;
  QPushButton *_save, *_delete, *_copy;
  QLineEdit* _name = nullptr;
  QPlainTextEdit* _description = nullptr;
  QComboBox *_quality = nullptr, *_class = nullptr, *_subclass = nullptr, *_slot = nullptr, *_bonding = nullptr, *_damageType = nullptr;
  QSpinBox *_itemLevel = nullptr, *_requiredLevel = nullptr, *_stack = nullptr, *_maxCount = nullptr, *_buyCount = nullptr, *_armor = nullptr, *_block = nullptr, *_durability = nullptr;
  QSpinBox* _resist[6]{};
  QDoubleSpinBox *_damageMin = nullptr, *_damageMax = nullptr, *_speed = nullptr;
  MoneyEdit *_buy = nullptr, *_sell = nullptr;
  QGroupBox* _weaponBox = nullptr;
  QVBoxLayout *_statRows = nullptr, *_spellRows = nullptr;
  QCheckBox *_allClasses = nullptr, *_allRaces = nullptr;
  QVector<QCheckBox*> _classBoxes, _raceBoxes;
};
std::optional<Id> run(QWidget* parent, ItemDesign design) {
  ItemEditor editor(parent, std::move(design));
  editor.exec();
  return editor.saved;
}
}
std::optional<Id> createItem(QWidget* parent) {
  QDialog dialog(parent); dialog.setWindowTitle("New Item");
  auto layout = new QVBoxLayout(&dialog);
  layout->addWidget(new QLabel("<b>How do you want to start?</b>"));
  std::optional<ItemDesignService::Template> chosen; bool clone = false;
  auto primary = button("Clone Existing Item", Icon::clone); primary->setStyleSheet("font-weight: bold; padding: 10px;");
  primary->setToolTip("Start from any item in the game: its look, stats and effects. The original stays unchanged.");
  layout->addWidget(primary);
  QObject::connect(primary, &QPushButton::clicked, &dialog, [&] { clone = true; dialog.accept(); });
  layout->addWidget(new QLabel("…or from a template:"));
  auto grid = new QGridLayout; layout->addLayout(grid);
  int i = 0;
  for (auto const& t : ItemDesignService::templates()) {
    auto b = new QPushButton(t.label); b->setToolTip(t.hint); b->setMinimumHeight(40);
    grid->addWidget(b, i / 4, i % 4); ++i;
    QObject::connect(b, &QPushButton::clicked, &dialog, [&, id = t.id] { chosen = id; dialog.accept(); });
  }
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted) return std::nullopt;
  if (clone) return cloneItem(parent);
  try { return run(parent, ItemDesignService::blank(*chosen)); } catch (std::exception const& e) { QMessageBox::warning(parent, "New Item", e.what()); return std::nullopt; }
}
std::optional<Id> cloneItem(QWidget* parent, Id source) {
  if (!source) { auto picked = pickItem(parent, "Clone which item?"); if (!picked) return std::nullopt; source = *picked; }
  try { return run(parent, ItemDesignService::clone(source)); } catch (std::exception const& e) { QMessageBox::warning(parent, "Clone Item", e.what()); return std::nullopt; }
}
std::optional<Id> editItem(QWidget* parent, Id item) {
  try { return run(parent, ItemDesignService::load(item)); } catch (std::exception const& e) { QMessageBox::warning(parent, "Item", e.what()); return std::nullopt; }
}
void testItem(QWidget* parent, Id item) {
  auto tests = TestSessionService::instance();
  if (!tests) return;
  std::optional<ItemInfo> info;
  try { info = ItemService::get(item); } catch (...) {}
  if (!info) return;
  QDialog options(parent); options.setWindowTitle("Test Item");
  auto layout = new QVBoxLayout(&options);
  auto text = new QLabel("Your local test character gets " + info->name + " in the backpack, then WoW starts where the character last logged out.");
  text->setWordWrap(true); layout->addWidget(text);
  auto row = new QHBoxLayout; auto count = new QSpinBox; count->setRange(1, std::max(1, std::min(200, info->stackable * 4))); count->setValue(1);
  row->addWidget(new QLabel("How many")); row->addWidget(count); row->addStretch(); layout->addLayout(row);
  auto level = new QCheckBox(QString("Set my test character's level to %1 (the item's required level)").arg(std::max(1, info->requiredLevel)));
  level->setChecked(info->requiredLevel > 1); level->setEnabled(info->requiredLevel > 1); layout->addWidget(level);
  auto note = new QLabel("Only changes your character on the local test server. The backpack needs free slots."); note->setWordWrap(true); note->setStyleSheet("color: gray;");
  layout->addWidget(note);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); buttons->addButton("Test", QDialogButtonBox::AcceptRole); layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &options, &QDialog::accept); QObject::connect(buttons, &QDialogButtonBox::rejected, &options, &QDialog::reject);
  if (options.exec() != QDialog::Accepted) return;
  TestOptions test; test.items = {{item, count->value()}};
  if (level->isChecked()) test.level = info->requiredLevel;
  tests->testCharacter(parent, test);
}
void openItemLibrary(QWidget* parent) {
  QDialog dialog(parent); dialog.setWindowTitle("Items"); dialog.resize(1100, 700);
  auto layout = new QVBoxLayout(&dialog);
  layout->addWidget(new QLabel("<b>ITEMS</b> — every item in your local world; yours are in bold. Double-click to open one."));
  auto browser = new ItemBrowser({}, false, &dialog); layout->addWidget(browser, 1);
  auto row = new QHBoxLayout; layout->addLayout(row);
  // New, Clone and Open are on the browser itself.
  auto test = button("Test Item…", Icon::play);
  row->addWidget(test); row->addStretch();
  auto close = new QPushButton("Close"); row->addWidget(close);
  auto after = [browser](std::optional<Id> id) { browser->refresh(); if (id) browser->select(*id); };
  QObject::connect(test, &QPushButton::clicked, &dialog, [&] { if (auto c = browser->current()) testItem(&dialog, c->entry); });
  browser->onActivated = [&](Id id) { after(editItem(&dialog, id)); };
  QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
  dialog.exec();
}
}
