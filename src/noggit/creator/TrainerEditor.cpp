#include "ServiceEditors.hpp"
#include "TrainerService.hpp"
#include "CreatorPreviews.hpp"
#include "EditorWidgets.hpp"
#include "ItemService.hpp"
#include "TestSessionService.hpp"
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/content/ClientData.hpp>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
namespace Noggit::Creator {
namespace {
using Icon = Ui::FontAwesome::Icons;
enum Column { SpellColumn, LevelColumn, CostColumn, NotesColumn, RemoveColumn, Columns };
QPushButton* button(QString const& text, Icon icon, QWidget* parent) { return new QPushButton(Ui::FontAwesomeIcon(icon), text, parent); }
QIcon spellIcon(SpellInfo const& s) { return Ui::Content::ClientData::spellIcon(std::uint32_t(s.icon)); }
// The spell's trainer tooltip.
QString spellHtml(SpellInfo const& s, TrainerSpell const* row = nullptr, int previewLevel = 0, bool hasPrevious = true) {
  QString html = "<b style='font-size:11pt'>" + s.name.toHtmlEscaped() + "</b>" + (s.rank.isEmpty() ? QString() : " <span style='color:gray'>" + s.rank.toHtmlEscaped() + "</span>");
  if (row) {
    bool tooLow = previewLevel && row->level > previewLevel;
    if (row->level) html += QString("<br><span style='color:%1'>Required level: %2</span>").arg(tooLow ? "#e04040" : "#e0e0e0").arg(row->level);
    html += "<br>Cost: " + moneyHtml(row->cost);
  } else {
    html += QString("<br>Usable from level %1").arg(std::max(1, s.level));
    if (s.suggestedCost) html += "<br>Original trainers charge " + moneyHtml(s.suggestedCost);
  }
  if (!s.previousName.isEmpty()) html += QString("<br><span style='color:%1'>Requires: %2</span>").arg(hasPrevious ? "#e0e0e0" : "#e04040", s.previousName.toHtmlEscaped());
  if (!s.description.isEmpty()) html += "<br><br><span style='color:#ffd100'>" + s.description.toHtmlEscaped() + "</span>";
  return html;
}
std::optional<SpellInfo> pickSpell(QWidget* parent) {
  QDialog dialog(parent); dialog.setWindowTitle("Add a spell to teach"); dialog.resize(860, 560);
  auto layout = new QVBoxLayout(&dialog);
  auto input = new QLineEdit; input->setPlaceholderText("Search spells by name, e.g. Frostbolt…"); input->setClearButtonEnabled(true); layout->addWidget(input);
  auto row = new QHBoxLayout; layout->addLayout(row, 1);
  auto list = new QListWidget; list->setIconSize(QSize(28, 28)); row->addWidget(list, 3);
  auto detail = new QLabel; detail->setWordWrap(true); detail->setTextFormat(Qt::RichText); detail->setAlignment(Qt::AlignTop);
  detail->setStyleSheet("background: rgba(10,14,30,0.85); color: #f0f0f0; border: 1px solid #555; border-radius: 4px; padding: 8px;");
  detail->setMinimumWidth(300); row->addWidget(detail, 2);
  auto status = new QLabel; status->setStyleSheet("color: gray;"); layout->addWidget(status);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); buttons->addButton("Add", QDialogButtonBox::AcceptRole); layout->addWidget(buttons);
  QVector<SpellInfo> results; QTimer timer; timer.setSingleShot(true); timer.setInterval(300);
  auto refresh = [&] {
    if (input->text().trimmed().size() < 2) { results.clear(); list->clear(); status->setText("Type at least two letters."); return; }
    try {
      results = TrainerService::search(input->text()); list->clear();
      for (auto const& s : results) list->addItem(new QListWidgetItem(spellIcon(s), s.title() + QString("   —   level %1").arg(std::max(1, s.level))));
      status->setText(results.isEmpty() ? "No learnable spell matches. Only spells trainers can teach are listed." : QString("%1 spells").arg(results.size()));
    } catch (std::exception const& e) { results.clear(); list->clear(); status->setText(QString::fromUtf8(e.what())); }
  };
  QObject::connect(input, &QLineEdit::textChanged, &timer, [&] { timer.start(); });
  QObject::connect(&timer, &QTimer::timeout, &dialog, refresh);
  QObject::connect(list, &QListWidget::currentRowChanged, &dialog, [&](int r) { detail->setText(r >= 0 && r < results.size() ? spellHtml(results[r]) : QString()); });
  QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] { if (list->currentRow() >= 0) dialog.accept(); });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  refresh();
  if (dialog.exec() != QDialog::Accepted || list->currentRow() < 0 || list->currentRow() >= results.size()) return std::nullopt;
  return results[list->currentRow()];
}
// The game's trainer window, as a chosen local character (or any character of a level) would see it.
class TrainerWindow final : public QFrame {
public:
  TrainerWindow(QWidget* parent) : QFrame(parent) {
    setObjectName("TrainerWindow");
    setStyleSheet("#TrainerWindow { background: qlineargradient(y1:0, y2:1, stop:0 #2a1c0f, stop:1 #17100a); border: 2px solid #8a6d2f; border-radius: 6px; }"
                  "#TrainerWindow QLabel, #TrainerWindow QCheckBox { color: #f0e6c8; background: transparent; }"
                  "#TrainerWindow QListWidget { background: rgba(0,0,0,0.35); border: 1px solid #4a3a1c; color: #f0e6c8; }");
    setMinimumWidth(360);
    auto layout = new QVBoxLayout(this); layout->setContentsMargins(10, 8, 10, 8);
    _title = new QLabel(this); _title->setAlignment(Qt::AlignCenter); _title->setStyleSheet("color: #ffd100; font-weight: bold; font-size: 11pt;");
    layout->addWidget(_title);
    auto as = new QHBoxLayout; layout->addLayout(as);
    as->addWidget(new QLabel("Preview as", this));
    _character = new QComboBox(this); as->addWidget(_character, 1);
    _level = new QSpinBox(this); _level->setRange(1, 60); _level->setPrefix("Level "); as->addWidget(_level);
    auto filters = new QHBoxLayout; layout->addLayout(filters);
    _available = new QCheckBox("Available", this); _high = new QCheckBox("Too High Level", this); _known = new QCheckBox("Already Known", this);
    for (auto box : {_available, _high, _known}) { box->setChecked(box != _known); filters->addWidget(box); }
    _list = new QListWidget(this); _list->setIconSize(QSize(26, 26)); layout->addWidget(_list, 1);
    _detail = new QLabel(this); _detail->setWordWrap(true); _detail->setTextFormat(Qt::RichText); _detail->setMinimumHeight(140); _detail->setAlignment(Qt::AlignTop);
    layout->addWidget(_detail);
    _character->addItem("Any character", 0);
    try { _characters = TrainerService::characters(); } catch (...) {}
    for (auto const& c : _characters) _character->addItem(QString("%1 — level %2 %3").arg(c.name).arg(c.level).arg(TrainerService::className(c.playerClass)), c.guid);
    auto redraw = [this] { draw(); };
    connect(_character, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
      auto guid = _character->currentData().toUInt(); _knownSpells.clear(); _level->setEnabled(!guid);
      for (auto const& c : _characters) if (c.guid == guid) { _level->setValue(c.level); _playerClass = c.playerClass; }
      if (guid) { try { _knownSpells = TrainerService::knownSpells(guid); } catch (...) {} } else _playerClass = 0;
      draw();
    });
    connect(_level, qOverload<int>(&QSpinBox::valueChanged), this, redraw);
    for (auto box : {_available, _high, _known}) connect(box, &QCheckBox::toggled, this, redraw);
    connect(_list, &QListWidget::currentRowChanged, this, [this](int r) {
      auto row = r >= 0 ? _list->item(r)->data(Qt::UserRole).toInt() : -1;
      if (row >= 0 && row < _rows.size() && _info.contains(_rows[row].spell)) {
        auto const& s = _info[_rows[row].spell];
        _detail->setText(spellHtml(s, &_rows[row], _level->value(), !s.previous || _knownSpells.contains(s.previous) || taughtHere(s.previous)));
        if (onSelected && row < _ownCount) onSelected(row);
      } else _detail->clear();
    });
  }
  void setTrainer(QString const& name, int type, int playerClass, QVector<TrainerSpell> rows, int ownCount, QHash<Id, SpellInfo> const& info) {
    _title->setText(name + (type == 0 && playerClass ? " — " + TrainerService::className(playerClass) + " Trainer" : QString(" — Trainer")));
    _trainerType = type; _trainerClass = playerClass; _rows = std::move(rows); _ownCount = ownCount; _info = info; draw();
  }
  std::function<void(int)> onSelected; // own rows only
  int previewLevel() const { return _level->value(); }
private:
  bool taughtHere(Id learned) const { for (auto const& r : _rows) if (_info.contains(r.spell) && _info[r.spell].learned == learned) return true; return false; }
  void draw() {
    _list->clear(); _detail->clear();
    if (_trainerType == 0 && _trainerClass && _playerClass && _playerClass != _trainerClass) {
      _list->addItem(QString("Only %1s can train here.").arg(TrainerService::className(_trainerClass))); return;
    }
    // The game lists by status, then level.
    struct Entry { int row; int status; };
    QVector<Entry> entries;
    for (int i = 0; i < _rows.size(); ++i) {
      if (!_info.contains(_rows[i].spell)) continue;
      auto const& s = _info[_rows[i].spell];
      bool known = _knownSpells.contains(s.learned);
      bool missing = s.previous && !_knownSpells.contains(s.previous);
      int status = known ? 2 : (_rows[i].level > _level->value() || (missing && _character->currentData().toUInt())) ? 1 : 0;
      if ((status == 0 && _available->isChecked()) || (status == 1 && _high->isChecked()) || (status == 2 && _known->isChecked())) entries.push_back({i, status});
    }
    std::stable_sort(entries.begin(), entries.end(), [this](Entry const& a, Entry const& b) {
      return a.status != b.status ? a.status < b.status : _rows[a.row].level < _rows[b.row].level;
    });
    static char const* headers[] = {"Available", "Too High Level", "Already Known"};
    static char const* colors[] = {"#40d040", "#e04040", "#909090"};
    int last = -1;
    for (auto const& e : entries) {
      if (e.status != last) {
        auto header = new QListWidgetItem(headers[e.status]); auto font = header->font(); font.setBold(true); header->setFont(font);
        header->setForeground(QColor("#ffd100")); header->setFlags(Qt::NoItemFlags); header->setData(Qt::UserRole, -1); _list->addItem(header); last = e.status;
      }
      auto const& s = _info[_rows[e.row].spell];
      auto item = new QListWidgetItem(spellIcon(s), "   " + s.title() + (e.row >= _ownCount ? "  (shared list)" : QString()));
      item->setForeground(QColor(colors[e.status])); item->setData(Qt::UserRole, e.row); _list->addItem(item);
    }
    if (entries.isEmpty()) _list->addItem("Nothing to show with these filters.");
  }
  QLabel *_title, *_detail;
  QComboBox* _character;
  QSpinBox* _level;
  QCheckBox *_available, *_high, *_known;
  QListWidget* _list;
  QVector<LocalCharacter> _characters;
  QSet<Id> _knownSpells;
  QVector<TrainerSpell> _rows;
  QHash<Id, SpellInfo> _info;
  int _ownCount = 0, _trainerType = 2, _trainerClass = 0, _playerClass = 0;
};
class TrainerEditor final : public QDialog {
public:
  TrainerEditor(QWidget* parent, World* world, Id entry) : QDialog(parent), _entry(entry) {
    setWindowTitle("Trainer"); resize(1400, 800);
    auto layout = new QVBoxLayout(this);
    _title = editorTitle({}, this); layout->addWidget(_title);
    _notice = noticeBanner({}, this); layout->addWidget(_notice);
    auto split = new QSplitter(this); layout->addWidget(split, 1);
    auto left = new QWidget; auto leftLayout = new QVBoxLayout(left); leftLayout->setContentsMargins(0, 0, 0, 0);
    auto preview = new NpcPreview(world, left); preview->setFixedHeight(280); preview->showNpc(entry); leftLayout->addWidget(preview);
    _window = new TrainerWindow(left); leftLayout->addWidget(_window, 1);
    split->addWidget(left);

    auto center = new QWidget; auto centerLayout = new QVBoxLayout(center); centerLayout->setContentsMargins(0, 0, 0, 0);
    auto options = new QHBoxLayout; centerLayout->addLayout(options);
    _teaches = new QCheckBox("Trains players", center); _teaches->setToolTip("Gives this NPC the trainer role, so players can open the trainer window.");
    options->addWidget(_teaches);
    options->addWidget(new QLabel("Who can train:", center));
    _who = new QComboBox(center); _who->addItem("Everyone", -1);
    for (auto const& [id, name] : TrainerService::classes()) _who->addItem("Only " + name + "s", id);
    options->addWidget(_who);
    _makeEditable = button("Make Shared List Editable", Icon::edit, center); options->addWidget(_makeEditable);
    options->addStretch();
    _table = new QTableWidget(0, Columns, center);
    _table->setHorizontalHeaderLabels({"Spell", "Required level", "Cost", "Notes", ""});
    _table->horizontalHeader()->setSectionResizeMode(SpellColumn, QHeaderView::Stretch);
    _table->horizontalHeader()->setSectionResizeMode(NotesColumn, QHeaderView::Stretch);
    for (int c : {LevelColumn, CostColumn, RemoveColumn}) _table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    _table->verticalHeader()->hide(); _table->setSelectionBehavior(QAbstractItemView::SelectRows); _table->setSelectionMode(QAbstractItemView::SingleSelection);
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers); _table->setIconSize(QSize(26, 26)); _table->verticalHeader()->setDefaultSectionSize(34);
    centerLayout->addWidget(_table, 1);
    _listProblems = new QLabel(center); _listProblems->setWordWrap(true); centerLayout->addWidget(_listProblems);
    auto actions = new QHBoxLayout; centerLayout->addLayout(actions);
    _add = button("Add Spell", Icon::plus, center); _copyFrom = button("Copy Trainer Setup…", Icon::paste, center);
    _copyTo = button("Use on Another…", Icon::copy, center); _sort = button("Sort by Level", Icon::sortamountup, center);
    _reset = button("Reset", Icon::undo, center);
    for (auto b : {_add, _copyFrom, _copyTo, _sort, _reset}) actions->addWidget(b);
    actions->addStretch();
    split->addWidget(center);
    split->setSizes({420, 900});

    auto bottom = new QHBoxLayout; layout->addLayout(bottom);
    auto test = button("Test Trainer…", Icon::play, this); bottom->addWidget(test); bottom->addStretch();
    auto close = new QPushButton("Close", this); _save = new QPushButton("Save", this); _save->setDefault(true);
    bottom->addWidget(close); bottom->addWidget(_save);

    _window->onSelected = [this](int row) { _table->selectRow(row); };
    connect(_teaches, &QCheckBox::toggled, this, [this](bool on) { if (!_building) { _trainer.teaches = on; changed(); } });
    connect(_who, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
      if (_building) return;
      auto c = _who->currentData().toInt();
      if (c < 0) { _trainer.type = 2; _trainer.playerClass = 0; } else { _trainer.type = 0; _trainer.playerClass = c; }
      changed();
    });
    connect(_makeEditable, &QPushButton::clicked, this, [this] {
      _trainer.spells = _trainer.shared + _trainer.spells; _trainer.shared.clear(); _trainer.sharedList = 0; _dirty = true; rebuild();
    });
    connect(_add, &QPushButton::clicked, this, [this] {
      auto spell = pickSpell(this);
      if (!spell) return;
      _info[spell->teach] = *spell;
      _trainer.spells.push_back({spell->teach, spell->suggestedCost, spell->suggestedLevel});
      _dirty = true; rebuild(); _table->selectRow(_trainer.spells.size() - 1);
    });
    connect(_sort, &QPushButton::clicked, this, [this] {
      std::stable_sort(_trainer.spells.begin(), _trainer.spells.end(), [](TrainerSpell const& a, TrainerSpell const& b) { return a.level < b.level; });
      _dirty = true; rebuild();
    });
    connect(_copyFrom, &QPushButton::clicked, this, [this] { copyFrom(); });
    connect(_copyTo, &QPushButton::clicked, this, [this] { copyTo(); });
    connect(_reset, &QPushButton::clicked, this, [this] {
      if (_dirty && QMessageBox::question(this, "Reset", "Discard your changes and reload what this trainer teaches?") != QMessageBox::Yes) return;
      load();
    });
    connect(test, &QPushButton::clicked, this, [this] { testTrainer(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(_save, &QPushButton::clicked, this, [this] { if (save()) accept(); });
    load();
  }
  bool saved = false;
  void reject() override { if (confirmClose(this, _dirty, [this] { return save(); })) QDialog::reject(); }
private:
  void load() {
    try { _trainer = TrainerService::load(_entry); } catch (std::exception const& e) { QMessageBox::warning(this, "Trainer", e.what()); return; }
    _title->setText((_trainer.name + " — Trainer").toUpper());
    _notice->setText(_trainer.notice); _notice->setVisible(!_trainer.notice.isEmpty());
    for (auto w : std::initializer_list<QWidget*>{_add, _copyFrom, _sort, _reset, _save}) w->setVisible(_trainer.editable);
    _teaches->setEnabled(_trainer.editable); _who->setEnabled(_trainer.editable && (_trainer.type == 0 || _trainer.type == 2));
    _copyTo->setText(_trainer.editable ? "Use on Another…" : "Copy to My NPC…");
    fetch(); _dirty = false; rebuild();
  }
  void fetch() {
    QVector<Id> missing;
    for (auto const* list : {&_trainer.spells, &_trainer.shared}) for (auto const& s : *list) if (!_info.contains(s.spell)) missing << s.spell;
    if (missing.isEmpty()) return;
    try { auto found = TrainerService::spells(missing); for (auto it = found.begin(); it != found.end(); ++it) _info[it.key()] = *it; } catch (...) {}
  }
  void rebuild() {
    _building = true;
    _teaches->setChecked(_trainer.teaches);
    if (auto i = _who->findData(_trainer.type == 0 ? _trainer.playerClass : -1); i >= 0) _who->setCurrentIndex(i);
    _makeEditable->setVisible(_trainer.editable && _trainer.sharedList);
    _table->setRowCount(_trainer.spells.size());
    for (int i = 0; i < _trainer.spells.size(); ++i) buildRow(i);
    _building = false;
    refresh();
  }
  void buildRow(int i) {
    auto const s = _trainer.spells[i];
    bool locked = !_trainer.editable;
    auto cell = new QTableWidgetItem;
    if (_info.contains(s.spell)) { auto const& info = _info[s.spell]; cell->setText(info.title()); cell->setIcon(spellIcon(info)); cell->setToolTip(spellHtml(info, &s)); }
    else cell->setText(QString("Spell %1").arg(s.spell));
    cell->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    _table->setItem(i, SpellColumn, cell);
    auto level = new QSpinBox; level->setRange(0, 60); level->setSpecialValueText("Any"); level->setValue(s.level); level->setEnabled(!locked);
    connect(level, qOverload<int>(&QSpinBox::valueChanged), this, [this, i](int v) { if (!_building) { _trainer.spells[i].level = v; changed(); } });
    _table->setCellWidget(i, LevelColumn, level);
    auto cost = new MoneyEdit; cost->setValue(s.cost); cost->setReadOnly(locked);
    cost->onChanged = [this, i, cost] { if (!_building) { _trainer.spells[i].cost = cost->value(); changed(); } };
    _table->setCellWidget(i, CostColumn, cost);
    auto remove = new QToolButton; remove->setIcon(Ui::FontAwesomeIcon(Icon::trash)); remove->setAutoRaise(true); remove->setToolTip("Stop teaching this");
    remove->setEnabled(!locked);
    connect(remove, &QToolButton::clicked, this, [this, i] { _trainer.spells.removeAt(i); _dirty = true; rebuild(); });
    _table->setCellWidget(i, RemoveColumn, remove);
  }
  void changed() { _dirty = true; refresh(); }
  void refresh() {
    auto problems = TrainerService::check(_trainer, _info);
    for (int i = 0; i < _trainer.spells.size(); ++i) showRowProblems(_table, i, NotesColumn, problems);
    auto list = listProblems(problems);
    _listProblems->setText(list); _listProblems->setVisible(!list.isEmpty());
    _listProblems->setStyleSheet(hasErrors(problems) ? "color: #e04040;" : "color: #d08a00;");
    _window->setTrainer(_trainer.name, _trainer.type, _trainer.playerClass, _trainer.spells + _trainer.shared, _trainer.spells.size(), _info);
    _save->setEnabled(_trainer.editable && !hasErrors(problems));
  }
  void copyFrom() {
    auto source = chooseOwner(this, "Copy trainer setup from", [](QString const& text) { return TrainerService::owners(text); });
    if (!source) return;
    try {
      auto from = TrainerService::load(source->id);
      auto spells = from.shared + from.spells;
      if (spells.isEmpty()) { QMessageBox::information(this, "Copy Trainer Setup", source->name + " teaches nothing."); return; }
      QMessageBox ask(QMessageBox::Question, "Copy Trainer Setup", QString("Copy %1 spells from %2.").arg(spells.size()).arg(source->name), QMessageBox::Cancel, this);
      auto replace = ask.addButton("Replace current spells", QMessageBox::AcceptRole), add = ask.addButton("Add to them", QMessageBox::AcceptRole);
      ask.exec();
      if (ask.clickedButton() != replace && ask.clickedButton() != add) return;
      if (ask.clickedButton() == replace) { _trainer.spells.clear(); _trainer.type = from.type == 0 ? 0 : 2; _trainer.playerClass = from.type == 0 ? from.playerClass : 0; }
      QSet<Id> have; for (auto const& s : _trainer.spells) have.insert(s.spell);
      for (auto const& s : spells) if (!have.contains(s.spell)) { _trainer.spells.push_back(s); have.insert(s.spell); }
      _trainer.teaches = true;
      fetch(); _dirty = true; rebuild();
    } catch (std::exception const& e) { QMessageBox::warning(this, "Copy Trainer Setup", e.what()); }
  }
  void copyTo() {
    auto target = chooseOwner(this, "Give this setup to one of your NPCs", [](QString const& text) { return CreatureService::search(text, true); });
    if (!target || target->id == _entry) return;
    if (QMessageBox::question(this, "Use on Another", "Replace what " + target->name + " teaches with these spells?") != QMessageBox::Yes) return;
    try {
      auto other = TrainerService::load(target->id);
      other.spells = _trainer.shared + _trainer.spells; other.shared.clear(); other.sharedList = 0; other.teaches = true;
      other.type = _trainer.type == 0 ? 0 : 2; other.playerClass = _trainer.type == 0 ? _trainer.playerClass : 0;
      TrainerService::save(other); saved = true;
      QMessageBox::information(this, "Use on Another", target->name + " now teaches these spells. It is listed in Local Changes.");
    } catch (std::exception const& e) { QMessageBox::warning(this, "Use on Another", e.what()); }
  }
  bool save() {
    try {
      if (hasErrors(TrainerService::validate(_trainer))) { refresh(); QMessageBox::warning(this, "Trainer", "Fix the problems marked ✗ first."); return false; }
      TrainerService::save(_trainer);
    } catch (std::exception const& e) { QMessageBox::warning(this, "Trainer", QString::fromUtf8(e.what())); return false; }
    _dirty = false; saved = true; return true;
  }
  void testTrainer() {
    auto tests = TestSessionService::instance();
    if (!tests) return;
    if (_dirty && (QMessageBox::question(this, "Test", "Save the trainer before testing?") != QMessageBox::Yes || !save())) return;
    int highest = 1; for (auto const& s : _trainer.spells + _trainer.shared) highest = std::max(highest, s.level);
    QDialog options(this); options.setWindowTitle("Test Trainer");
    auto layout = new QVBoxLayout(&options);
    auto text = new QLabel("Your local test character is placed beside " + _trainer.name + "."); text->setWordWrap(true); layout->addWidget(text);
    auto row = new QHBoxLayout; layout->addLayout(row);
    auto setLevel = new QCheckBox("Set my test character to level"); setLevel->setChecked(true);
    auto level = new QSpinBox; level->setRange(1, 60); level->setValue(highest);
    row->addWidget(setLevel); row->addWidget(level); row->addStretch();
    auto money = new QCheckBox("Give it enough money to train everything"); money->setChecked(true); layout->addWidget(money);
    QString classNote = _trainer.type == 0 && _trainer.playerClass ? "\nOnly " + TrainerService::className(_trainer.playerClass) + " characters can train here." : QString();
    auto note = new QLabel("Only changes your character on the local test server." + classNote); note->setStyleSheet("color: gray;"); layout->addWidget(note);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); buttons->addButton("Test", QDialogButtonBox::AcceptRole); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &options, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &options, &QDialog::reject);
    if (options.exec() != QDialog::Accepted) return;
    TestOptions test;
    if (setLevel->isChecked()) test.level = level->value();
    if (money->isChecked()) { qint64 total = 0; for (auto const& s : _trainer.spells + _trainer.shared) total += s.cost; test.money = std::max<qint64>(total, 100); }
    tests->testEntity(this, false, _entry, test);
  }
  Id _entry;
  Trainer _trainer;
  QHash<Id, SpellInfo> _info;
  bool _dirty = false, _building = false;
  QLabel *_title, *_notice, *_listProblems;
  TrainerWindow* _window;
  QCheckBox* _teaches;
  QComboBox* _who;
  QPushButton *_makeEditable, *_add, *_copyFrom, *_copyTo, *_sort, *_reset, *_save;
  QTableWidget* _table;
};
}
bool editTrainer(QWidget* parent, World* world, Id npc) {
  TrainerEditor editor(parent, world, npc);
  editor.exec();
  return editor.saved;
}
}
