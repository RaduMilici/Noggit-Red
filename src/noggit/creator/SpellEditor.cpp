#include "ContentEditors.hpp"
#include "ClientPatch.hpp"
#include "EditorWidgets.hpp"
#include "ItemBrowser.hpp"
#include "SpellService.hpp"
#include "TestSessionService.hpp"
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/content/ClientData.hpp>
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
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
namespace Noggit::Creator {
namespace {
using Icon = Ui::FontAwesome::Icons;
using namespace SpellCatalog;
QPushButton* button(QString const& text, Icon icon, QWidget* parent = nullptr) { return new QPushButton(Ui::FontAwesomeIcon(icon), text, parent); }
QLabel* hint(QString const& text) { auto l = new QLabel(text); l->setWordWrap(true); l->setStyleSheet("color: palette(mid); font-style: italic;"); return l; }
void choose(QComboBox* box, int value) { if (auto i = box->findData(value); i >= 0) box->setCurrentIndex(i); }
QIcon spellIcon(Id icon) { return Ui::Content::ClientData::spellIcon(icon); }
// The client's readable lists (cast times, durations, ranges, radii); empty without a configured client.
QVector<ClientChoice> clientList(QString const& file, QVector<ClientChoice> (*make)(Wdbc const&)) {
  static QHash<QString, QVector<ClientChoice>> cache;
  if (auto it = cache.find(file); it != cache.end()) return *it;
  QVector<ClientChoice> list;
  if (auto* client = ClientPatchService::instance()) if (auto table = client->table(file)) list = make(*table);
  if (!list.isEmpty()) cache.insert(file, list);
  return list;
}
QVector<ClientChoice> castTimes() { return clientList("SpellCastTimes.dbc", &ClientLists::castTimes); }
QVector<ClientChoice> durations() { return clientList("SpellDuration.dbc", &ClientLists::durations); }
QVector<ClientChoice> ranges() { return clientList("SpellRange.dbc", &ClientLists::ranges); }
QVector<ClientChoice> radii() { return clientList("SpellRadius.dbc", &ClientLists::radii); }
QString labelOf(QVector<ClientChoice> const& list, int id, QString const& none = {}) {
  if (!id && !none.isEmpty()) return none;
  for (auto const& c : list) if (int(c.id) == id) return c.label;
  return QString("Table row %1").arg(id);
}
double valueOf(QVector<ClientChoice> const& list, int id) { for (auto const& c : list) if (int(c.id) == id) return c.value; return 0; }
void fill(QComboBox* box, QVector<ClientChoice> const& list, int current, QString const& none = {}) {
  box->clear();
  if (!none.isEmpty()) box->addItem(none, 0);
  for (auto const& c : list) box->addItem(c.label, int(c.id));
  if (box->findData(current) < 0) box->addItem(labelOf(list, current, none), current);
  choose(box, current);
}
// The spell's tooltip text with the client's tokens filled in, as players see it.
QString filled(SpellDesign const& d) {
  QString text = d.description;
  double duration = valueOf(durations(), d.duration);
  auto amount = [&](int j) {
    auto const& e = d.effects[std::clamp(j, 0, 2)];
    return e.minValue == e.maxValue ? QString::number(std::abs(e.minValue)) : QString("%1 to %2").arg(std::abs(e.minValue)).arg(std::abs(e.maxValue));
  };
  QRegularExpression token("\\$(\\d*)([sSmMoOtTaAdD])(\\d?)");
  QString out; int last = 0;
  for (auto it = token.globalMatch(text); it.hasNext();) {
    auto m = it.next(); out += text.mid(last, m.capturedStart() - last); last = m.capturedEnd();
    if (!m.captured(1).isEmpty()) { out += m.captured(0); continue; } // another spell's value: shown as written
    int j = m.captured(3).isEmpty() ? 0 : m.captured(3).toInt() - 1;
    auto const& e = d.effects[std::clamp(j, 0, 2)];
    switch (m.captured(2).toLower().at(0).toLatin1()) {
      case 's': case 'm': out += amount(j); break;
      case 'o': out += QString::number(e.period > 0 && duration > 0 ? std::abs(e.maxValue) * int(duration / e.period) : std::abs(e.maxValue)); break;
      case 't': out += QString::number(e.period / 1000.0); break;
      case 'a': out += QString::number(valueOf(radii(), e.radius)); break;
      case 'd': out += duration > 0 ? ClientLists::seconds(duration) : QString("until cancelled"); break;
      default: out += m.captured(0);
    }
  }
  return (out + text.mid(last)).toHtmlEscaped().replace("\n", "<br>");
}
QString costText(SpellDesign const& d) {
  QString power; for (auto const& p : powers()) if (p.id == d.powerType) power = p.label;
  if (d.costPercent) return QString("%1% of base %2").arg(d.costPercent).arg(power.toLower());
  if (!d.cost) return {};
  return QString("%1 %2").arg(d.powerType == 1 ? d.cost / 10 : d.cost).arg(power.section(' ', 0, 0));
}
// A searchable spell list with icons; yours first.
class SpellBrowser final : public QWidget {
public:
  explicit SpellBrowser(QWidget* parent = nullptr) : QWidget(parent) {
    auto layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
    auto top = new QHBoxLayout; layout->addLayout(top);
    _text = new QLineEdit; _text->setPlaceholderText("Search spells by name…"); _text->setClearButtonEnabled(true); top->addWidget(_text, 1);
    _own = new QCheckBox("Only my spells"); top->addWidget(_own);
    auto create = new QToolButton; create->setText("New"); create->setIcon(Ui::FontAwesomeIcon(Icon::plus)); create->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto clone = new QToolButton; clone->setText("Clone"); clone->setIcon(Ui::FontAwesomeIcon(Icon::clone)); clone->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto open = new QToolButton; open->setText("Open"); open->setIcon(Ui::FontAwesomeIcon(Icon::edit)); open->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    for (auto b : {create, clone, open}) top->addWidget(b);
    _list = new QListWidget; _list->setIconSize(QSize(28, 28)); layout->addWidget(_list, 1);
    _status = new QLabel; _status->setStyleSheet("color: gray;"); layout->addWidget(_status);
    _timer.setSingleShot(true); _timer.setInterval(250);
    connect(&_timer, &QTimer::timeout, this, [this] { search(); });
    connect(_text, &QLineEdit::textChanged, this, [this] { _timer.start(); });
    connect(_own, &QCheckBox::toggled, this, [this] { search(); });
    auto made = [this](std::optional<Id> id) { if (!id) return; search(); select(*id); };
    connect(create, &QToolButton::clicked, this, [this, made] { made(createSpell(this)); });
    connect(clone, &QToolButton::clicked, this, [this, made] { made(cloneSpell(this, current())); });
    connect(open, &QToolButton::clicked, this, [this, made] { if (auto id = current()) made(editSpell(this, id)); });
    connect(_list, &QListWidget::itemActivated, this, [this] { if (onActivated && current()) onActivated(current()); });
    search();
  }
  Id current() const { auto item = _list->currentItem(); return item ? item->data(Qt::UserRole).toUInt() : 0; }
  void select(Id id) {
    for (int i = 0; i < _list->count(); ++i) if (_list->item(i)->data(Qt::UserRole).toUInt() == id) { _list->setCurrentRow(i); _list->scrollToItem(_list->item(i)); return; }
    _wanted = id;
  }
  void search() {
    _list->clear();
    QVector<SpellListEntry> spells;
    try { spells = SpellService::browse(_text->text(), _own->isChecked()); } catch (std::exception const& e) { _status->setText(QString::fromUtf8(e.what())); return; }
    for (auto const& s : spells) {
      auto item = new QListWidgetItem(spellIcon(s.icon), s.name + (s.rank.isEmpty() ? QString() : "  (" + s.rank + ")")
                                      + QString("   —   %1%2").arg(schoolName(s.school), s.level ? QString(" · level %1").arg(s.level) : QString()), _list);
      item->setData(Qt::UserRole, s.entry);
      if (s.own) { auto font = item->font(); font.setBold(true); item->setFont(font); }
    }
    _status->setText(spells.isEmpty() ? "No spells match." : spells.size() >= 400 ? "Showing the first 400 matches. Refine the search to see more." : QString("%1 spells").arg(spells.size()));
    if (_wanted) { auto wanted = _wanted; _wanted = 0; select(wanted); }
  }
  std::function<void(Id)> onActivated;
private:
  QLineEdit* _text; QCheckBox* _own; QListWidget* _list; QLabel* _status; QTimer _timer; Id _wanted = 0;
};
// Spell icons from the client's SpellIcon.dbc, searchable by file name ("Fire", "Holy").
std::optional<Id> pickIcon(QWidget* parent, Id current) {
  std::optional<Wdbc> table;
  if (auto* client = ClientPatchService::instance()) table = client->table("SpellIcon.dbc");
  if (!table) { QMessageBox::information(parent, "Icon", "Choose the WoW client in Client Profiles to browse spell icons."); return std::nullopt; }
  QDialog dialog(parent); dialog.setWindowTitle("Choose Icon"); dialog.resize(760, 560);
  auto layout = new QVBoxLayout(&dialog);
  auto search = new QLineEdit; search->setPlaceholderText("Filter by name, e.g. Fire, Holy, Shadow, Nature…"); layout->addWidget(search);
  auto list = new QListWidget; list->setViewMode(QListView::IconMode); list->setIconSize(QSize(40, 40)); list->setGridSize(QSize(56, 56));
  list->setResizeMode(QListView::Adjust); list->setMovement(QListView::Static); layout->addWidget(list, 1);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(buttons);
  auto show = [&] {
    list->clear();
    auto filter = search->text().trimmed();
    int shown = 0;
    for (int r = 0; r < table->rows() && shown < 600; ++r) {
      auto name = table->text(r, 1).section('\\', -1);
      if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive)) continue;
      auto item = new QListWidgetItem(spellIcon(table->cell(r, 0)), {}, list); item->setToolTip(name); item->setData(Qt::UserRole, table->cell(r, 0));
      if (table->cell(r, 0) == current) list->setCurrentItem(item);
      ++shown;
    }
  };
  QTimer timer; timer.setSingleShot(true); timer.setInterval(250);
  QObject::connect(&timer, &QTimer::timeout, &dialog, show);
  QObject::connect(search, &QLineEdit::textChanged, &dialog, [&] { timer.start(); });
  QObject::connect(list, &QListWidget::itemActivated, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  show();
  if (dialog.exec() != QDialog::Accepted || !list->currentItem()) return std::nullopt;
  return list->currentItem()->data(Qt::UserRole).toUInt();
}
class SpellEditor final : public QDialog {
public:
  SpellEditor(QWidget* parent, SpellDesign design) : QDialog(parent) {
    setWindowTitle("Spell Editor"); resize(1240, 800);
    auto layout = new QVBoxLayout(this);
    _title = editorTitle({}, this); layout->addWidget(_title);
    _notice = noticeBanner({}, this); layout->addWidget(_notice);
    auto body = new QHBoxLayout; layout->addLayout(body, 1);
    auto left = new QVBoxLayout; body->addLayout(left, 2);
    auto card = new QFrame; card->setObjectName("Tooltip");
    card->setStyleSheet("#Tooltip { background: #0b0b1e; border: 1px solid #5a5a7a; border-radius: 4px; } QLabel { color: #ffffff; background: transparent; }");
    auto cardLayout = new QHBoxLayout(card); _icon = new QToolButton; _icon->setIconSize(QSize(48, 48)); _icon->setAutoRaise(true); _icon->setToolTip("Choose the icon");
    cardLayout->addWidget(_icon, 0, Qt::AlignTop);
    _tooltip = new QLabel; _tooltip->setTextFormat(Qt::RichText); _tooltip->setWordWrap(true); _tooltip->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    cardLayout->addWidget(_tooltip, 1); left->addWidget(card);
    left->addWidget(new QLabel("<b>Rank chain</b>"));
    _chain = new QListWidget; _chain->setIconSize(QSize(20, 20)); left->addWidget(_chain, 1);
    left->addWidget(hint("Ranks of the same spell, lowest first. Double-click one to open it."));
    _tabs = new QTabWidget; body->addWidget(_tabs, 3);
    _tabs->addTab(basicsPage(), "Basics"); _tabs->addTab(castingPage(), "Casting");
    _effectsPage = new QWidget; _effectsLayout = new QVBoxLayout(_effectsPage);
    auto effectsScroll = new QScrollArea; effectsScroll->setWidgetResizable(true); effectsScroll->setFrameShape(QFrame::NoFrame); effectsScroll->setWidget(_effectsPage);
    _tabs->addTab(effectsScroll, "Effects"); _tabs->addTab(ranksPage(), "Ranks & trainers");
    _problems = new QLabel; _problems->setWordWrap(true); layout->addWidget(_problems);
    auto bottom = new QHBoxLayout; layout->addLayout(bottom);
    auto clone = button("Clone Existing Spell…", Icon::clone), next = button("Create Next Rank…", Icon::arrowup), test = button("Test Spell…", Icon::play);
    _delete = button("Delete Spell…", Icon::trash); _copy = button("Clone This Spell", Icon::copy); _copy->setStyleSheet("font-weight: bold;");
    for (auto b : {clone, next, test, _delete, _copy}) bottom->addWidget(b);
    bottom->addStretch();
    auto close = new QPushButton("Close"); _save = new QPushButton("Save"); _save->setDefault(true);
    bottom->addWidget(close); bottom->addWidget(_save);
    connect(clone, &QPushButton::clicked, this, [this] { cloneOther(); });
    connect(_copy, &QPushButton::clicked, this, [this] { if (_d.entry) replaceWith(SpellService::clone(_d.entry)); });
    connect(next, &QPushButton::clicked, this, [this] { nextRank(); });
    connect(test, &QPushButton::clicked, this, [this] { testThis(); });
    connect(_delete, &QPushButton::clicked, this, [this] { deleteThis(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(_save, &QPushButton::clicked, this, [this] { save(); });
    connect(_icon, &QToolButton::clicked, this, [this] { if (_d.editable) if (auto icon = pickIcon(this, _d.icon)) { _d.icon = *icon; edited(); } });
    connect(_chain, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { openRank(item->data(Qt::UserRole).toUInt()); });
    _factsTimer.setSingleShot(true); _factsTimer.setInterval(300);
    connect(&_factsTimer, &QTimer::timeout, this, [this] { try { _facts = SpellService::facts(_d); } catch (...) {} refresh(); });
    show(std::move(design));
  }
  std::optional<Id> saved;
  void reject() override { if (confirmClose(this, _dirty, [this] { return save(); })) QDialog::reject(); }
private:
  QWidget* page(QLayout* content) { auto w = new QWidget; w->setLayout(content); auto area = new QScrollArea; area->setWidgetResizable(true); area->setFrameShape(QFrame::NoFrame); area->setWidget(w); return area; }
  QWidget* basicsPage() {
    auto form = new QFormLayout;
    _name = new QLineEdit; _name->setMaxLength(100); _rank = new QLineEdit; _rank->setPlaceholderText("Rank 1 (optional)");
    auto names = new QHBoxLayout; names->addWidget(_name, 2); names->addWidget(_rank, 1); form->addRow("Name", names);
    _school = new QComboBox; for (auto const& s : schools()) _school->addItem(s.label, s.id); form->addRow("School", _school);
    _description = new QPlainTextEdit; _description->setMaximumHeight(90);
    auto write = new QToolButton; write->setText("Write it for me"); write->setToolTip("A description from the effects, with the game's $s1 / $d / $o1 values");
    auto descriptionRow = new QVBoxLayout; descriptionRow->addWidget(_description); descriptionRow->addWidget(write, 0, Qt::AlignLeft);
    form->addRow("Description", descriptionRow);
    form->addRow(hint("$s1 is effect 1's amount, $o1 its total over time, $d the duration, $t1 its tick time, $a1 its radius."));
    _aura = new QPlainTextEdit; _aura->setMaximumHeight(60); _aura->setPlaceholderText("Text on the buff or debuff, e.g. \"Increases Stamina by $s1.\"");
    form->addRow("Buff tooltip", _aura);
    auto visual = button("Use the Cast Effects of…", Icon::magic); _visualFrom = new QLabel;
    auto visualRow = new QHBoxLayout; visualRow->addWidget(_visualFrom, 1); visualRow->addWidget(visual); form->addRow("Looks like", visualRow);
    form->addRow(hint("Cast animations and effects come from an existing spell; custom visuals are not supported."));
    connect(_name, &QLineEdit::textEdited, this, [this](QString const& v) { _d.name = v; edited(); });
    connect(_rank, &QLineEdit::textEdited, this, [this](QString const& v) { _d.rank = v; edited(); });
    connect(_description, &QPlainTextEdit::textChanged, this, [this] { if (!_building) { _d.description = _description->toPlainText(); edited(); } });
    connect(_aura, &QPlainTextEdit::textChanged, this, [this] { if (!_building) { _d.tooltip = _aura->toPlainText(); edited(); } });
    connect(_school, qOverload<int>(&QComboBox::activated), this, [this] { _d.school = _school->currentData().toInt(); edited(); });
    connect(write, &QToolButton::clicked, this, [this] { auto text = SpellService::suggestedDescription(_d); if (!text.isEmpty()) _description->setPlainText(text); });
    connect(visual, &QPushButton::clicked, this, [this] {
      if (auto spell = pickSpell(this, "Use the cast effects of")) try {
        auto source = SpellService::load(*spell); _d.visual = source.visual; _visualFrom->setText(source.name + (source.rank.isEmpty() ? QString() : " (" + source.rank + ")")); edited();
      } catch (std::exception const& e) { QMessageBox::warning(this, "Spell", e.what()); }
    });
    return page(form);
  }
  QWidget* castingPage() {
    auto form = new QFormLayout;
    _castTime = new QComboBox; form->addRow("Cast time", _castTime);
    _cooldown = new QDoubleSpinBox; _cooldown->setRange(0, 86400); _cooldown->setDecimals(1); _cooldown->setSuffix(" sec"); _cooldown->setSpecialValueText("none");
    _gcd = new QCheckBox("Triggers the global cooldown");
    auto cd = new QHBoxLayout; cd->addWidget(_cooldown); cd->addWidget(_gcd); cd->addStretch(); form->addRow("Cooldown", cd);
    _range = new QComboBox; form->addRow("Range", _range);
    _power = new QComboBox; for (auto const& p : powers()) _power->addItem(p.label, p.id);
    _cost = new QSpinBox; _cost->setRange(0, 100000); _percent = new QSpinBox; _percent->setRange(0, 100); _percent->setSuffix(" % of base mana"); _percent->setSpecialValueText("no percentage");
    auto cost = new QHBoxLayout; cost->addWidget(_cost); cost->addWidget(_power); cost->addWidget(_percent); cost->addStretch(); form->addRow("Cost", cost);
    form->addRow(hint("Rage is stored in tenths: 150 is 15 rage."));
    _target = new QComboBox; for (auto const& t : targets()) _target->addItem(t.label, t.id); _target->addItem("(set per effect)", -1);
    form->addRow("Target type", _target);
    _duration = new QComboBox; form->addRow("Duration", _duration);
    _stacks = new QSpinBox; _stacks->setRange(0, 255); _stacks->setSpecialValueText("does not stack"); form->addRow("Stacks up to", _stacks);
    _mechanic = new QComboBox; for (auto const& m : mechanics()) _mechanic->addItem(m.label, m.id); form->addRow("Crowd control kind", _mechanic);
    form->addRow(hint("Decides immunities and diminishing returns, e.g. Stun for a stun."));
    _proc = new QSpinBox; _proc->setRange(0, 101); _proc->setSuffix(" %"); _proc->setSpecialValueText("—"); form->addRow("Proc chance", _proc);
    _level = new QSpinBox; _level->setRange(0, 60); _level->setSpecialValueText("none"); _maxLevel = new QSpinBox; _maxLevel->setRange(0, 60); _maxLevel->setSpecialValueText("none");
    auto levels = new QHBoxLayout; levels->addWidget(_level); levels->addWidget(new QLabel("scales up to level")); levels->addWidget(_maxLevel); levels->addStretch();
    form->addRow("Level", levels);
    auto combo = [this](QComboBox* box, int SpellDesign::*field) { connect(box, qOverload<int>(&QComboBox::activated), this, [this, box, field] { _d.*field = box->currentData().toInt(); edited(); }); };
    combo(_castTime, &SpellDesign::castTime); combo(_range, &SpellDesign::range); combo(_power, &SpellDesign::powerType); combo(_duration, &SpellDesign::duration); combo(_mechanic, &SpellDesign::mechanic);
    auto spin = [this](QSpinBox* box, int SpellDesign::*field) { connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [this, field](int v) { if (!_building) { _d.*field = v; edited(); } }); };
    spin(_cost, &SpellDesign::cost); spin(_percent, &SpellDesign::costPercent); spin(_stacks, &SpellDesign::stacks); spin(_proc, &SpellDesign::procChance);
    spin(_level, &SpellDesign::level); spin(_maxLevel, &SpellDesign::maxLevel);
    connect(_cooldown, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) { if (!_building) { _d.cooldown = int(std::lround(v * 1000)); edited(); } });
    connect(_gcd, &QCheckBox::toggled, this, [this](bool on) { if (!_building) { _d.globalCooldown = on ? 1500 : 0; edited(); } });
    connect(_target, qOverload<int>(&QComboBox::activated), this, [this] {
      int t = _target->currentData().toInt(); if (t < 0) return;
      auto const& preset = targets()[t];
      for (auto& e : _d.effects) if (e.type && e.type != 28 && e.type != 41) { e.targetA = preset.a; e.targetB = preset.b; }
      edited(true);
    });
    return page(form);
  }
  QWidget* ranksPage() {
    auto form = new QFormLayout;
    _previous = new QLabel; auto pick = new QToolButton; pick->setText("Choose…"); auto clear = new QToolButton; clear->setText("None");
    auto previous = new QHBoxLayout; previous->addWidget(_previous, 1); previous->addWidget(pick); previous->addWidget(clear);
    form->addRow("Required previous rank", previous);
    form->addRow(hint("Players need the previous rank before a trainer teaches this one."));
    _teachable = new QCheckBox("Trainers can teach it");
    form->addRow(_teachable);
    form->addRow(hint("Adds the spell to the Trainer Editor's spell list (it makes the teaching spell trainers use). Set the spell's level: trainers list spells by level."));
    connect(pick, &QToolButton::clicked, this, [this] { if (auto spell = pickSpell(this, "Previous rank", _d.previous)) { _d.previous = *spell; edited(true); } });
    connect(clear, &QToolButton::clicked, this, [this] { _d.previous = 0; edited(true); });
    connect(_teachable, &QCheckBox::toggled, this, [this](bool on) { if (!_building) { _d.teachable = on; edited(); } });
    return page(form);
  }
  // --- effects ---
  void buildEffects() {
    while (auto item = _effectsLayout->takeAt(0)) { if (auto w = item->widget()) { w->hide(); w->deleteLater(); } delete item; }
    for (int j = 0; j < 3; ++j) _effectsLayout->addWidget(effectBox(j));
    _effectsLayout->addStretch();
    _effectsPage->setEnabled(_d.editable);
  }
  void rebuildEffects() { QTimer::singleShot(0, this, [this] { buildEffects(); refresh(); }); }
  QGroupBox* effectBox(int j) {
    auto& e = _d.effects[j];
    auto box = new QGroupBox(QString("Effect %1").arg(j + 1)); auto form = new QFormLayout(box);
    auto header = new QHBoxLayout;
    auto templateButton = new QToolButton; templateButton->setText("Fill from template"); templateButton->setPopupMode(QToolButton::InstantPopup);
    auto menu = new QMenu(templateButton);
    for (auto const& t : templates()) menu->addAction(t.label, this, [this, j, id = t.id] { apply(id, _d, j); edited(true); rebuildEffects(); fillCasting(); })->setToolTip(t.hint);
    templateButton->setMenu(menu);
    auto clear = new QToolButton; clear->setText("Clear"); clear->setEnabled(e.type != 0);
    connect(clear, &QToolButton::clicked, this, [this, j] { _d.effects[j] = {}; edited(true); rebuildEffects(); });
    header->addWidget(templateButton); header->addWidget(clear); header->addStretch();
    form->addRow(header);
    auto type = new QComboBox;
    for (auto const& x : effects()) { type->addItem(x.label, x.id); type->setItemData(type->count() - 1, x.hint, Qt::ToolTipRole); }
    if (type->findData(e.type) < 0) type->addItem(effectName(e.type) + " (kept as copied)", e.type);
    choose(type, e.type);
    form->addRow("Does", type);
    connect(type, qOverload<int>(&QComboBox::activated), this, [this, j, type] {
      auto& e = _d.effects[j]; e.type = type->currentData().toInt();
      if (e.type && !e.targetA) { e.targetA = 6; }
      edited(true); rebuildEffects();
    });
    if (!e.type) { form->addRow(hint("Unused. Pick what it does, or fill it from a template.")); return box; }
    if (needsScripting(e.type, e.aura)) { auto warn = new QLabel("⚠ Does nothing without server scripting support (kept as copied)."); warn->setStyleSheet("color: #d08a00;"); form->addRow(warn); }
    auto in = inputs(e);
    if (in.aura) {
      auto aura = new QComboBox;
      for (auto const& a : auras()) { aura->addItem(a.label, a.id); aura->setItemData(aura->count() - 1, a.hint, Qt::ToolTipRole); }
      if (aura->findData(e.aura) < 0) aura->addItem(e.aura ? auraName(e.aura) + " (kept as copied)" : QString("(choose)"), e.aura);
      choose(aura, e.aura);
      form->addRow("Aura", aura);
      connect(aura, qOverload<int>(&QComboBox::activated), this, [this, j, aura] { _d.effects[j].aura = aura->currentData().toInt(); edited(true); rebuildEffects(); });
    }
    auto target = new QComboBox;
    for (auto const& t : targets()) target->addItem(t.label, t.id);
    int preset = targetOf(e.targetA, e.targetB);
    if (preset < 0) target->addItem(targetName(e.targetA, e.targetB) + " (kept as copied)", -1);
    choose(target, preset);
    form->addRow("Affects", target);
    connect(target, qOverload<int>(&QComboBox::activated), this, [this, j, target] {
      int t = target->currentData().toInt(); if (t < 0) return;
      _d.effects[j].targetA = targets()[t].a; _d.effects[j].targetB = targets()[t].b; edited(true); rebuildEffects();
    });
    if (in.radius) {
      auto radius = new QComboBox; fill(radius, radii(), e.radius, "(choose)"); form->addRow("Radius", radius);
      connect(radius, qOverload<int>(&QComboBox::activated), this, [this, j, radius] { _d.effects[j].radius = radius->currentData().toInt(); edited(); });
    }
    if (in.value) {
      auto row = new QHBoxLayout;
      auto min = new QSpinBox, max = new QSpinBox;
      for (auto s : {min, max}) s->setRange(-1000000, 1000000);
      min->setValue(e.minValue); max->setValue(e.maxValue);
      row->addWidget(min); row->addWidget(new QLabel("to")); row->addWidget(max); row->addStretch();
      form->addRow(in.valueLabel, row);
      connect(min, qOverload<int>(&QSpinBox::valueChanged), this, [this, j, max](int v) { _d.effects[j].minValue = v; if (max->value() < v) max->setValue(v); edited(); });
      connect(max, qOverload<int>(&QSpinBox::valueChanged), this, [this, j](int v) { _d.effects[j].maxValue = v; edited(); });
    }
    if (in.period) {
      auto period = new QDoubleSpinBox; period->setRange(0, 600); period->setDecimals(1); period->setSuffix(" sec"); period->setValue(e.period / 1000.0);
      form->addRow("Ticks every", period);
      connect(period, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, j](double v) { _d.effects[j].period = int(std::lround(v * 1000)); edited(); });
    }
    if (in.trigger) {
      auto row = new QHBoxLayout; auto label = new QLabel(spellLabel(e.trigger)); auto pick = new QToolButton; pick->setText("Choose…");
      auto open = new QToolButton; open->setText("Open"); open->setEnabled(e.trigger != 0);
      row->addWidget(label, 1); row->addWidget(pick); row->addWidget(open);
      form->addRow(e.type == 36 ? "Teaches" : "Spell", row);
      connect(pick, &QToolButton::clicked, this, [this, j] { if (auto spell = pickSpell(this, "Spell", _d.effects[j].trigger)) { _d.effects[j].trigger = *spell; edited(true); rebuildEffects(); } });
      connect(open, &QToolButton::clicked, this, [this, j] { editSpell(this, _d.effects[j].trigger); _names.clear(); rebuildEffects(); });
    }
    switch (in.misc) {
      case Misc::Creature: {
        auto row = new QHBoxLayout; auto label = new QLabel(creatureLabel(Id(e.misc))); auto pick = new QToolButton; pick->setText("Choose…");
        row->addWidget(label, 1); row->addWidget(pick); form->addRow(e.aura == 56 ? "Turns into" : "Creature", row);
        connect(pick, &QToolButton::clicked, this, [this, j] {
          if (auto c = chooseOwner(this, "Choose the creature", [](QString const& text) { return CreatureService::search(text); })) { _d.effects[j].misc = int(c->id); _names["creature" + QString::number(c->id)] = c->name; edited(true); rebuildEffects(); }
        });
        break;
      }
      case Misc::Item: {
        auto row = new QHBoxLayout; auto label = new QLabel(itemLabel(Id(e.misc))); auto pick = new QToolButton; pick->setText("Choose…");
        row->addWidget(label, 1); row->addWidget(pick); form->addRow("Item", row);
        connect(pick, &QToolButton::clicked, this, [this, j] { if (auto item = pickItem(this, "Item the spell creates", {}, Id(_d.effects[j].misc))) { _d.effects[j].misc = int(*item); edited(true); rebuildEffects(); } });
        break;
      }
      case Misc::Quest: {
        auto row = new QHBoxLayout; auto label = new QLabel(e.misc ? QString("Quest %1").arg(e.misc) : QString("(choose)")); auto pick = new QToolButton; pick->setText("Choose…");
        row->addWidget(label, 1); row->addWidget(pick); form->addRow("Quest", row);
        connect(pick, &QToolButton::clicked, this, [this, j, label] { if (auto q = chooseOwner(this, "Choose the quest", [](QString const& text) { return SpellService::quests(text); })) { _d.effects[j].misc = int(q->id); label->setText(q->name); edited(); } });
        break;
      }
      case Misc::Stat: case Misc::Power: case Misc::Dispel: {
        auto combo = new QComboBox;
        if (in.misc == Misc::Stat) for (auto const& s : stats()) combo->addItem(s.label, s.id);
        else if (in.misc == Misc::Power) for (auto const& p : powers()) { if (p.id >= 0) combo->addItem(p.label, p.id); }
        else for (auto const& [id, name] : std::initializer_list<std::pair<int, char const*>>{{1, "Magic"}, {2, "Curse"}, {3, "Disease"}, {4, "Poison"}}) combo->addItem(name, id);
        if (combo->findData(e.misc) < 0) combo->addItem(QString("Kept as copied (%1)").arg(e.misc), e.misc);
        choose(combo, e.misc);
        form->addRow(in.misc == Misc::Stat ? "Stat" : in.misc == Misc::Power ? "Power" : "Removes", combo);
        connect(combo, qOverload<int>(&QComboBox::activated), this, [this, j, combo] { _d.effects[j].misc = combo->currentData().toInt(); edited(); });
        break;
      }
      case Misc::SchoolMask: {
        auto row = new QHBoxLayout;
        for (auto const& s : schools()) {
          auto check = new QCheckBox(s.id == 0 ? QString("Physical / armor") : s.label); check->setChecked(e.misc & (1 << s.id)); row->addWidget(check);
          connect(check, &QCheckBox::toggled, this, [this, j, bit = 1 << s.id](bool on) { auto& m = _d.effects[j].misc; m = on ? (m | bit) : (m & ~bit); edited(); });
        }
        row->addStretch(); form->addRow("Schools", row);
        break;
      }
      case Misc::None: break;
    }
    auto said = new QLabel; said->setWordWrap(true); said->setStyleSheet("color: palette(mid);"); said->setObjectName(QString("said%1").arg(j));
    form->addRow(said);
    return box;
  }
  QString spellLabel(Id spell) {
    if (!spell) return "(choose)";
    auto key = "spell" + QString::number(spell);
    if (!_names.contains(key)) { QString name = QString("Spell %1").arg(spell); try { for (auto const& c : SpellService::search(QString::number(spell), false, 5)) if (c.id == spell) name = c.name; } catch (...) {} _names[key] = name; }
    return _names[key];
  }
  QString creatureLabel(Id creature) {
    if (!creature) return "(choose)";
    auto key = "creature" + QString::number(creature);
    if (!_names.contains(key)) { QString name = QString("Creature %1").arg(creature); try { name = CreatureService::load(creature).name; } catch (...) {} _names[key] = name; }
    return _names[key];
  }
  QString itemLabel(Id item) {
    if (!item) return "(choose)";
    auto key = "item" + QString::number(item);
    if (!_names.contains(key)) { QString name = QString("Item %1").arg(item); try { if (auto i = ItemService::get(item)) name = i->name; } catch (...) {} _names[key] = name; }
    return _names[key];
  }
  // --- state ---
  void fillCasting() {
    _building = true;
    fill(_castTime, castTimes(), _d.castTime); fill(_range, ranges(), _d.range); fill(_duration, durations(), _d.duration, "None (instant effects)");
    choose(_power, _d.powerType); _cost->setValue(_d.cost); _percent->setValue(_d.costPercent); _cooldown->setValue(_d.cooldown / 1000.0);
    _gcd->setChecked(_d.globalCooldown > 0); _stacks->setValue(_d.stacks); choose(_mechanic, _d.mechanic); _proc->setValue(_d.procChance);
    _level->setValue(_d.level); _maxLevel->setValue(_d.maxLevel);
    _building = false;
  }
  void show(SpellDesign d) {
    _d = std::move(d); _building = true;
    _notice->setText(_d.notice); _notice->setVisible(!_d.notice.isEmpty());
    _name->setText(_d.name); _rank->setText(_d.rank); choose(_school, _d.school);
    _description->setPlainText(_d.description); _aura->setPlainText(_d.tooltip);
    _visualFrom->setText(_d.visual ? (_d.source ? "As copied" : "Set") : QString("No cast effects"));
    _teachable->setChecked(_d.teachable);
    _building = false;
    fillCasting(); buildEffects();
    for (int t = 0; t < _tabs->count(); ++t) if (t != 2) _tabs->widget(t)->setEnabled(_d.editable);
    _save->setVisible(_d.editable); _delete->setVisible(_d.editable && _d.entry); _copy->setVisible(!_d.editable);
    _dirty = !_d.entry && _d.editable;
    loadChain();
    _factsTimer.start(0);
    refresh();
  }
  void loadChain() {
    _chain->clear();
    QVector<SpellRank> ranks;
    try { if (_d.entry) ranks = SpellService::chain(_d.entry); else if (_d.previous) ranks = SpellService::chain(_d.previous); } catch (...) {}
    for (auto const& r : ranks) {
      auto item = new QListWidgetItem(QString("%1 %2 · level %3%4").arg(r.name, r.rankText.isEmpty() ? QString("(rank %1)").arg(r.rank) : r.rankText).arg(r.level).arg(r.own ? " · yours" : ""), _chain);
      item->setData(Qt::UserRole, r.entry);
      if (r.entry == _d.entry) { auto font = item->font(); font.setBold(true); item->setFont(font); }
    }
    if (!_d.entry) { auto item = new QListWidgetItem(QString("%1 %2 · level %3 · not saved yet").arg(_d.name, _d.rank).arg(_d.level), _chain); auto font = item->font(); font.setItalic(true); item->setFont(font); }
  }
  void edited(bool facts = false) {
    if (_building) return;
    _dirty = true;
    if (facts) _factsTimer.start();
    refresh();
  }
  void refresh() {
    _title->setText(((_d.name.trimmed().isEmpty() ? QString("New spell") : _d.name) + (_d.rank.isEmpty() ? QString() : " " + _d.rank) + " — Spell").toUpper());
    _icon->setIcon(spellIcon(_d.icon));
    QStringList lines;
    lines << "<table width='100%'><tr><td><b style='font-size:11pt'>" + _d.name.toHtmlEscaped() + "</b></td><td align='right' style='color:#9d9d9d'>" + _d.rank.toHtmlEscaped() + "</td></tr>";
    auto cost = costText(_d), range = _d.range == 1 ? QString() : labelOf(ranges(), _d.range).section('(', 1).remove(')') + " range";
    lines << "<tr><td>" + cost + "</td><td align='right'>" + range + "</td></tr>";
    lines << "<tr><td>" + labelOf(castTimes(), _d.castTime) + "</td><td align='right'>" + (_d.cooldown ? ClientLists::seconds(_d.cooldown) + " cooldown" : QString()) + "</td></tr></table>";
    if (!_d.description.isEmpty()) lines << "<span style='color:#ffd100'>" + filled(_d) + "</span>";
    _tooltip->setText(lines.join(""));
    _previous->setText(_d.previous ? spellLabel(_d.previous) : QString("None: this is the first (or only) rank"));
    for (int j = 0; j < 3; ++j)
      if (auto said = _effectsPage->findChild<QLabel*>(QString("said%1").arg(j)))
        said->setText(SpellService::describe(_d, j, [this](QString const& kind, Id id) { return kind == "spell" ? spellLabel(id) : kind == "creature" ? creatureLabel(id) : itemLabel(id); }));
    // The target type reads "per effect" when the effects target differently.
    int common = -2;
    for (auto const& e : _d.effects) if (e.type && e.type != 28 && e.type != 41) { int t = targetOf(e.targetA, e.targetB); common = common == -2 || common == t ? t : -1; }
    _building = true; choose(_target, common < 0 ? -1 : common); _building = false;
    auto problems = SpellService::check(_d, _facts);
    QStringList text; bool errors = false;
    for (auto const& p : problems) { text << QString("%1 %2%3").arg(p.error ? "✗" : "⚠", p.effect >= 0 ? QString("Effect %1: ").arg(p.effect + 1) : QString(), p.text); errors = errors || p.error; }
    _problems->setText(text.join('\n')); _problems->setVisible(!text.isEmpty()); _problems->setStyleSheet(errors ? "color: #e04040;" : "color: #d08a00;");
    _save->setEnabled(_d.editable && !errors);
  }
  bool save() {
    try {
      _facts = SpellService::facts(_d);
      auto problems = SpellService::check(_d, _facts);
      if (std::any_of(problems.begin(), problems.end(), [](auto const& p) { return p.error; })) { refresh(); QMessageBox::warning(this, "Spell", "Fix the problems marked ✗ first."); return false; }
      auto entry = SpellService::save(_d);
      saved = entry; _dirty = false;
      show(SpellService::load(entry));
      return true;
    } catch (std::exception const& e) { QMessageBox::warning(this, "Spell", QString::fromUtf8(e.what())); return false; }
  }
  bool leave() { return !_dirty || QMessageBox::question(this, "Spell", "Discard your changes to this spell?") == QMessageBox::Yes; }
  void replaceWith(SpellDesign d) { if (leave()) { show(std::move(d)); _dirty = true; } }
  void cloneOther() { if (auto source = pickSpell(this, "Clone which spell?")) try { replaceWith(SpellService::clone(*source)); } catch (std::exception const& e) { QMessageBox::warning(this, "Spell", e.what()); } }
  void openRank(Id spell) { if (spell && spell != _d.entry && leave()) try { show(SpellService::load(spell)); } catch (std::exception const& e) { QMessageBox::warning(this, "Spell", e.what()); } }
  void nextRank() {
    if (!_d.entry) { QMessageBox::information(this, "Create Next Rank", "Save this spell first: the next rank requires it."); return; }
    if (_dirty && (QMessageBox::question(this, "Create Next Rank", "Save this spell first?") != QMessageBox::Yes || !save())) return;
    QDialog dialog(this); dialog.setWindowTitle("Create Next Rank");
    auto form = new QFormLayout(&dialog);
    form->addRow(new QLabel("A copy of <b>" + (_d.name + " " + _d.rank).toHtmlEscaped() + "</b> as its next rank. Requires this rank."));
    auto level = new QSpinBox; level->setRange(1, 60); level->setValue(std::min(60, std::max(1, _d.level) + 6)); form->addRow("Level", level);
    auto scale = new QSpinBox; scale->setRange(10, 1000); scale->setSuffix(" %"); scale->setValue(130); form->addRow("Damage / healing", scale);
    auto cost = new QSpinBox; cost->setRange(0, 100000); cost->setValue(int(std::lround(_d.cost * 1.25))); form->addRow("Cost", cost);
    auto cooldown = new QDoubleSpinBox; cooldown->setRange(0, 86400); cooldown->setSuffix(" sec"); cooldown->setValue(_d.cooldown / 1000.0); form->addRow("Cooldown", cooldown);
    form->addRow(hint("Amounts are multiplied by the percentage; everything else is copied. You can still change it all before saving."));
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); buttons->addButton("Create", QDialogButtonBox::AcceptRole); form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    try { show(SpellService::nextRank(_d.entry, level->value(), scale->value() / 100.0, cost->value(), int(cooldown->value() * 1000))); _dirty = true; }
    catch (std::exception const& e) { QMessageBox::warning(this, "Create Next Rank", e.what()); }
  }
  void deleteThis() {
    if (QMessageBox::question(this, "Delete Spell", "Delete " + _d.name + " " + _d.rank + " from your local world? It is listed in Local Changes.") != QMessageBox::Yes) return;
    try { SpellService::remove(_d.entry); saved = _d.entry; _dirty = false; accept(); } catch (std::exception const& e) { QMessageBox::warning(this, "Delete Spell", e.what()); }
  }
  void testThis() {
    if (_d.editable && (_dirty || !_d.entry) && (QMessageBox::question(this, "Test Spell", "Save the spell before testing?") != QMessageBox::Yes || !save())) return;
    if (_d.entry) testSpell(this, _d.entry);
  }
  SpellDesign _d;
  SpellFacts _facts;
  QHash<QString, QString> _names;
  bool _dirty = false, _building = false;
  QTimer _factsTimer;
  QLabel *_title, *_notice, *_tooltip, *_problems, *_visualFrom = nullptr, *_previous = nullptr;
  QToolButton* _icon;
  QListWidget* _chain;
  QTabWidget* _tabs;
  QWidget* _effectsPage;
  QVBoxLayout* _effectsLayout;
  QPushButton *_save, *_delete, *_copy;
  QLineEdit *_name = nullptr, *_rank = nullptr;
  QPlainTextEdit *_description = nullptr, *_aura = nullptr;
  QComboBox *_school = nullptr, *_castTime = nullptr, *_range = nullptr, *_power = nullptr, *_target = nullptr, *_duration = nullptr, *_mechanic = nullptr;
  QSpinBox *_cost = nullptr, *_percent = nullptr, *_stacks = nullptr, *_proc = nullptr, *_level = nullptr, *_maxLevel = nullptr;
  QDoubleSpinBox* _cooldown = nullptr;
  QCheckBox *_gcd = nullptr, *_teachable = nullptr;
};
std::optional<Id> run(QWidget* parent, SpellDesign design) {
  SpellEditor editor(parent, std::move(design));
  editor.exec();
  return editor.saved;
}
}
std::optional<Id> createSpell(QWidget* parent) {
  QDialog dialog(parent); dialog.setWindowTitle("New Spell");
  auto layout = new QVBoxLayout(&dialog);
  layout->addWidget(new QLabel("<b>How do you want to start?</b>"));
  bool clone = false; std::optional<Template> chosen; bool empty = false;
  auto primary = button("Clone Existing Spell", Icon::clone); primary->setStyleSheet("font-weight: bold; padding: 10px;");
  primary->setToolTip("Start from any spell in the game: its look, timing and effects. The original stays unchanged.");
  layout->addWidget(primary);
  QObject::connect(primary, &QPushButton::clicked, &dialog, [&] { clone = true; dialog.accept(); });
  layout->addWidget(new QLabel("…or Create Spell from an effect template:"));
  auto grid = new QGridLayout; layout->addLayout(grid);
  int i = 0;
  for (auto const& t : templates()) {
    auto b = new QPushButton(t.label); b->setToolTip(t.hint); b->setMinimumHeight(36); grid->addWidget(b, i / 5, i % 5); ++i;
    QObject::connect(b, &QPushButton::clicked, &dialog, [&, id = t.id] { chosen = id; dialog.accept(); });
  }
  auto blank = new QPushButton("Empty spell"); grid->addWidget(blank, i / 5, i % 5);
  QObject::connect(blank, &QPushButton::clicked, &dialog, [&] { empty = true; dialog.accept(); });
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted) return std::nullopt;
  if (clone) return cloneSpell(parent);
  try {
    auto d = SpellService::blank();
    if (chosen) {
      apply(*chosen, d, 0);
      for (auto const& t : templates()) if (t.id == *chosen) d.name = "New " + t.label.toLower();
      d.description = SpellService::suggestedDescription(d);
    }
    return run(parent, d);
  } catch (std::exception const& e) { QMessageBox::warning(parent, "New Spell", e.what()); return std::nullopt; }
}
std::optional<Id> cloneSpell(QWidget* parent, Id source) {
  if (!source) { auto picked = pickSpell(parent, "Clone which spell?"); if (!picked) return std::nullopt; source = *picked; }
  try { return run(parent, SpellService::clone(source)); } catch (std::exception const& e) { QMessageBox::warning(parent, "Clone Spell", e.what()); return std::nullopt; }
}
std::optional<Id> editSpell(QWidget* parent, Id spell) {
  if (!spell) return std::nullopt;
  try { return run(parent, SpellService::load(spell)); } catch (std::exception const& e) { QMessageBox::warning(parent, "Spell", e.what()); return std::nullopt; }
}
std::optional<Id> pickSpell(QWidget* parent, QString const& title, Id current) {
  QDialog dialog(parent); dialog.setWindowTitle(title); dialog.resize(760, 620);
  auto layout = new QVBoxLayout(&dialog);
  auto browser = new SpellBrowser(&dialog); layout->addWidget(browser, 1);
  if (current) browser->select(current);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); auto ok = buttons->addButton("Choose", QDialogButtonBox::AcceptRole); ok->setDefault(true);
  layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] { if (browser->current()) dialog.accept(); });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  browser->onActivated = [&](Id) { dialog.accept(); };
  if (dialog.exec() != QDialog::Accepted || !browser->current()) return std::nullopt;
  return browser->current();
}
void openSpellLibrary(QWidget* parent) {
  QDialog dialog(parent); dialog.setWindowTitle("Spells"); dialog.resize(820, 680);
  auto layout = new QVBoxLayout(&dialog);
  layout->addWidget(new QLabel("<b>SPELLS</b> — every spell in your local world; yours are in bold. Double-click to open one."));
  auto browser = new SpellBrowser(&dialog); layout->addWidget(browser, 1);
  auto row = new QHBoxLayout; layout->addLayout(row);
  auto test = button("Test Spell…", Icon::play); row->addWidget(test); row->addStretch();
  auto close = new QPushButton("Close"); row->addWidget(close);
  browser->onActivated = [&](Id id) { if (editSpell(&dialog, id)) browser->search(); };
  QObject::connect(test, &QPushButton::clicked, &dialog, [&] { if (browser->current()) testSpell(&dialog, browser->current()); });
  QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
  dialog.exec();
}
void testSpell(QWidget* parent, Id spell) {
  auto tests = TestSessionService::instance();
  if (!tests) return;
  SpellDesign d;
  try { d = SpellService::load(spell); } catch (std::exception const& e) { QMessageBox::warning(parent, "Test Spell", e.what()); return; }
  // Earlier ranks too, as a player who reached this rank would have them.
  QVector<Id> earlier;
  try {
    auto ranks = SpellService::chain(spell);
    for (auto const& r : ranks) { if (r.entry == spell) break; earlier << r.entry; }
  } catch (...) {}
  auto spells = earlier; spells << spell;
  QDialog options(parent); options.setWindowTitle("Test Spell");
  auto layout = new QVBoxLayout(&options);
  auto text = new QLabel("Your local test character learns <b>" + (d.name + " " + d.rank).toHtmlEscaped() + "</b>"
                         + (earlier.isEmpty() ? QString() : QString(" and its %1 earlier rank%2").arg(earlier.size()).arg(earlier.size() == 1 ? "" : "s"))
                         + ", then WoW starts where the character last logged out. Find it in the spellbook (General tab).");
  text->setWordWrap(true); layout->addWidget(text);
  auto level = new QCheckBox(QString("Set my test character's level to %1 (the spell's level)").arg(std::max(1, d.level)));
  level->setChecked(d.level > 1); level->setEnabled(d.level > 1); layout->addWidget(level);
  auto note = new QLabel("The client needs your spells in its Spell.dbc: Creator updates the local test client before launching (see Local changes → Client data). "
                         "Only your local character changes.");
  note->setWordWrap(true); note->setStyleSheet("color: gray;"); layout->addWidget(note);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); buttons->addButton("Test", QDialogButtonBox::AcceptRole); layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &options, &QDialog::accept); QObject::connect(buttons, &QDialogButtonBox::rejected, &options, &QDialog::reject);
  if (options.exec() != QDialog::Accepted) return;
  TestOptions test; test.spells = spells;
  if (level->isChecked()) test.level = d.level;
  tests->testCharacter(parent, test);
}
}
