#include "ContentEditors.hpp"
#include "EditorWidgets.hpp"
#include "SpellService.hpp"
#include "TalentService.hpp"
#include "TestSessionService.hpp"
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/content/ClientData.hpp>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
namespace Noggit::Creator {
namespace {
using Icon = Ui::FontAwesome::Icons;
// The talent frame's own geometry (63 px between talents), scaled to fit each view.
constexpr double cell = 63, iconSize = 40, gridLeft = 34, gridTop = 58;
constexpr double baseWidth = gridLeft + cell * TalentRules::columns + 16, baseHeight = gridTop + cell * TalentRules::tiers + 12;
QPointF centre(int row, int column) { return {gridLeft + column * cell + cell / 2, gridTop + row * cell + cell / 2}; }
QRectF iconRect(int row, int column) { auto c = centre(row, column); return {c.x() - iconSize / 2, c.y() - iconSize / 2, iconSize, iconSize}; }
QPointF handlePoint(int row, int column) { auto c = centre(row, column); return {c.x(), c.y() + iconSize / 2 + 7}; }
QColor const gold(255, 209, 0), green(40, 255, 40), grey(140, 140, 140), red(235, 60, 50), amber(255, 160, 40), blue(90, 170, 255);
QString const goldText = "#ffd100", redText = "#ff3a2f", greenText = "#40ff40", greyText = "#9d9d9d";
QPushButton* button(QString const& text, Icon icon, QWidget* parent = nullptr) { return new QPushButton(Ui::FontAwesomeIcon(icon), text, parent); }
QLabel* hint(QString const& text) { auto l = new QLabel(text); l->setWordWrap(true); l->setStyleSheet("color: palette(mid); font-style: italic;"); return l; }
QString plural(int n, QString const& word) { return QString("%1 %2%3").arg(n).arg(word).arg(n == 1 ? "" : "s"); }
QString place(Talent const& t) { return QString("row %1, column %2").arg(t.row + 1).arg(t.column + 1); }
bool databaseReady() { return qApp->property("creatorDatabaseReady").toBool(); }

// What the editor shows of a rank spell: loaded from the local database once, forgotten after edits.
struct SpellView { Id id = 0, icon = 0; bool exists = false, own = false, scripted = false; QString name, rank, text; int level = 0; };
class SpellCache {
public:
  SpellView get(Id id) {
    if (auto it = _cache.find(id); it != _cache.end()) return *it;
    SpellView v; v.id = id; v.name = QString("Spell %1").arg(id);
    if (!id || !databaseReady()) return v; // not cached: loads once the database is up
    try {
      auto d = SpellService::load(id);
      v.exists = true; v.own = d.editable; v.name = d.name; v.rank = d.rank; v.icon = d.icon; v.level = d.level; v.text = spellTooltipText(d);
      for (auto const& e : d.effects) if (e.type && SpellCatalog::needsScripting(e.type, e.aura)) v.scripted = true;
    } catch (std::exception const&) { v.text = QString("<i>Spell %1 is not in the local database.</i>").arg(id); }
    _cache.insert(id, v);
    return v;
  }
  void forget(Id id) { _cache.remove(id); }
  void clear() { _cache.clear(); }
private:
  QHash<Id, SpellView> _cache;
};
QPixmap iconPixmap(Id icon, bool desaturated) {
  static QHash<qint64, QPixmap> cache;
  qint64 key = qint64(icon) * 2 + (desaturated ? 1 : 0);
  if (auto it = cache.find(key); it != cache.end()) return *it;
  QPixmap pixmap = icon ? Ui::Content::ClientData::spellIcon(icon).pixmap(64, 64) : QPixmap();
  if (pixmap.isNull()) {
    pixmap = QPixmap(64, 64); pixmap.fill(QColor(35, 35, 40));
    QPainter p(&pixmap); QFont f; f.setPixelSize(40); f.setBold(true); p.setFont(f); p.setPen(QColor(190, 190, 190));
    p.drawText(pixmap.rect(), Qt::AlignCenter, "?");
  }
  if (desaturated) pixmap = QPixmap::fromImage(pixmap.toImage().convertToFormat(QImage::Format_Grayscale8));
  return cache[key] = pixmap;
}
// The tree's painting from the client (Interface\TalentFrame\<name>-TopLeft and its three neighbours).
QPixmap treeBackground(QString const& name) {
  static QHash<QString, QPixmap> cache;
  if (auto it = cache.find(name); it != cache.end()) return *it;
  QPixmap out;
  if (!name.isEmpty()) {
    struct Part { char const* suffix; int x, y, w, h; };
    static Part const parts[] = {{"TopLeft", 0, 0, 256, 256}, {"TopRight", 256, 0, 64, 256}, {"BottomLeft", 0, 256, 256, 128}, {"BottomRight", 256, 256, 64, 128}};
    QPixmap canvas(320, 384); canvas.fill(Qt::black); bool any = false;
    {
      QPainter p(&canvas);
      for (auto const& part : parts) {
        try {
          auto piece = Ui::Content::ClientData::texture("Interface\\TalentFrame\\" + name + "-" + part.suffix + ".blp", part.w, part.h);
          if (!piece.isNull()) { p.drawPixmap(part.x, part.y, piece); any = true; }
        } catch (...) {}
      }
    }
    if (any) out = canvas;
  }
  return cache[name] = out;
}
// A WoW-style tooltip that follows the cursor.
class TalentTip final : public QFrame {
public:
  TalentTip() : QFrame(nullptr, Qt::ToolTip | Qt::FramelessWindowHint) {
    setObjectName("TalentTip");
    setStyleSheet("#TalentTip { background: rgba(8, 8, 24, 238); border: 2px solid #8a8aa0; border-radius: 5px; } QLabel { color: white; background: transparent; }");
    auto layout = new QVBoxLayout(this); layout->setContentsMargins(10, 8, 10, 8);
    _text = new QLabel; _text->setTextFormat(Qt::RichText); _text->setWordWrap(true); _text->setFixedWidth(300); layout->addWidget(_text);
  }
  void show(QString const& html, QPoint at) {
    _text->setText(html); adjustSize();
    auto screen = QGuiApplication::screenAt(at); auto area = screen ? screen->availableGeometry() : QRect(0, 0, 4000, 4000);
    QPoint pos = at + QPoint(18, 18);
    if (pos.x() + width() > area.right()) pos.setX(at.x() - width() - 12);
    if (pos.y() + height() > area.bottom()) pos.setY(area.bottom() - height());
    move(pos); QFrame::show(); raise();
  }
private:
  QLabel* _text;
};
class TalentEditor;
// One talent tree painted as the game's talent frame; every part of it is a handle for editing or testing.
class TreeView final : public QWidget {
public:
  TreeView(TalentEditor& editor, int index, QWidget* parent) : QWidget(parent), _e(editor), _index(index) {
    setMouseTracking(true); setFocusPolicy(Qt::ClickFocus); setMinimumSize(int(baseWidth * 0.62), int(baseHeight * 0.62));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }
  QSize sizeHint() const override { return QSize(int(baseWidth), int(baseHeight)); }
protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void leaveEvent(QEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
private:
  struct Hit { Id talent = 0; int row = -1, column = -1; bool handle = false, header = false; Id arrow = 0; };
  QTransform transform() const {
    double scale = std::min(width() / baseWidth, height() / baseHeight);
    QTransform t; t.translate((width() - baseWidth * scale) / 2, (height() - baseHeight * scale) / 2); t.scale(scale, scale); return t;
  }
  QPointF toBase(QPointF p) const { return transform().inverted().map(p); }
  Hit hit(QPointF base) const;
  void hover(QPointF base, QPoint global);
  TalentEditor& _e;
  int _index;
  Id _press = 0, _hover = 0; QPoint _pressAt; QPointF _cursor;
  bool _dragging = false, _linking = false;
  int _hoverRow = -1, _hoverColumn = -1;
};
class TalentEditor final : public QDialog {
public:
  explicit TalentEditor(QWidget* parent) : QDialog(parent, Qt::Window) {
    setWindowTitle("Talent Editor"); resize(1560, 940);
    _store = TalentStore::instance();
    auto layout = new QVBoxLayout(this);
    layout->addWidget(toolbar());
    layout->addWidget(levelBar());
    _banner = noticeBanner({}, this); layout->addWidget(_banner);
    auto split = new QSplitter(Qt::Horizontal); layout->addWidget(split, 1);
    _canvas = new QWidget; _canvasLayout = new QHBoxLayout(_canvas); _canvasLayout->setContentsMargins(0, 0, 0, 0); _canvasLayout->setSpacing(6);
    _canvas->setStyleSheet("background: #0d0d10;");
    split->addWidget(_canvas);
    auto scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setMinimumWidth(400); scroll->setWidget(sidePanel());
    split->addWidget(scroll); split->setStretchFactor(0, 3); split->setStretchFactor(1, 1); split->setSizes({1100, 440});
    _status = new QLabel; _status->setWordWrap(true); layout->addWidget(_status);
    new QShortcut(QKeySequence::Undo, this, [this] { undo(); });
    new QShortcut(QKeySequence::Redo, this, [this] { redo(); });
    new QShortcut(QKeySequence("Ctrl+Shift+Z"), this, [this] { redo(); });
    reload();
  }
  ~TalentEditor() override { delete _tip; }

  // What the tree views read.
  TalentTree const& tree(int index) const { return *_visible[index]; }
  int treeCount() const { return int(_visible.size()); }
  TalentTree const* originalOf(TalentTree const& t) const { for (auto const& o : _originals) if (t.tab && o.tab == t.tab) return &o; return nullptr; }
  bool testing() const { return _test; }
  bool comparing() const { return _compare && originalOf(*_visible[0]); }
  QHash<Id, int> const& build() const { return _build; }
  int available() const { return TalentRules::pointsAt(_level) - spent(); }
  int spent() const { int sum = 0; for (auto* t : _visible) sum += t->points(_build); return sum; }
  SpellView spell(Id id) { return _spells.get(id); }
  QString name(Talent const& t) { if (t.ranks.isEmpty() || !t.ranks[0]) return "New talent"; auto v = spell(t.ranks[0]); return v.exists ? v.name : v.name + " (missing)"; }
  Id icon(Talent const& t) { return t.ranks.isEmpty() ? 0 : spell(t.ranks[0]).icon; }
  TalentReach reachOf(Id id) const { return _reach.value(id); }
  QVector<TalentProblem> problemsOf(Id id) const { return _problems.value(id); }
  QVector<TalentProblem> treeProblems(int index) const { return _treeProblems.value(index); }
  Id selected() const { return _selected; }
  int selectedTree() const { return _selectedTree; }
  bool focused() const { return _focus >= 0; }
  // Talents a hover over `id` lights up: it and everything it needs.
  QSet<Id> pathTo(int index, Id id) const {
    QSet<Id> out; auto const& t = tree(index);
    for (auto const* at = t.find(id); at && !out.contains(at->id); at = t.find(at->prerequisite)) out.insert(at->id);
    return out;
  }
  QString tooltip(int index, Talent const& t, int rank, bool test);

  // What the tree views do.
  void select(int index, Id id) { _selectedTree = index; _selected = id; refreshPanel(); for (auto* v : _views) v->update(); }
  void focusTree(int index) { _focus = index; layoutViews(); }
  void showTip(QString const& html, QPoint at) { if (!_tip) _tip = new TalentTip; _tip->show(html, at); }
  void hideTip() { if (_tip) _tip->hide(); }
  void status(QString const& text, bool error = false) { _status->setText(text); _status->setStyleSheet(error ? "color: #ff6b5e; font-weight: bold;" : "color: palette(mid);"); }
  void moveTalent(int index, Id id, int row, int column) {
    auto const& t = tree(index); auto const* talent = t.find(id); if (!talent) return;
    if (talent->row == row && talent->column == column) return;
    auto const* other = t.at(row, column);
    QString what = other ? "Swapped " + name(*talent) + " with " + name(*other) : "Moved " + name(*talent) + " to " + QString("row %1, column %2").arg(row + 1).arg(column + 1);
    edit(index, [&](TalentTree& tree) { TalentLogic::move(tree, id, row, column); }, what);
  }
  void link(int index, Id from, Id to) {
    auto const& t = tree(index);
    if (auto why = TalentLogic::cannotRequire(t, to, from); !why.isEmpty()) { status("Cannot link: " + why, true); return; }
    auto const* p = t.find(from);
    edit(index, [&](TalentTree& tree) { auto* target = tree.find(to); target->prerequisite = from; target->prerequisiteRank = std::max(1, p->maxRank()); },
         name(*t.find(to)) + " now requires " + plural(std::max(1, p->maxRank()), "point") + " in " + name(*p));
    select(index, to);
  }
  void learn(int index, Id id) {
    auto why = TalentLogic::cannotLearn(tree(index), _build, id, available());
    if (!why.isEmpty()) { status(name(*tree(index).find(id)) + ": " + why, true); return; }
    ++_build[id]; _order << id; refreshBuild();
  }
  void unlearn(int index, Id id) {
    auto why = TalentLogic::cannotUnlearn(tree(index), _build, id);
    if (!why.isEmpty()) { status("Cannot remove the point: " + why, true); return; }
    if (--_build[id] <= 0) _build.remove(id);
    for (int i = int(_order.size()) - 1; i >= 0; --i) if (_order[i] == id) { _order.remove(i); break; }
    refreshBuild();
  }
  void openTalent(int index, Id id) { select(index, id); _panel->setCurrentIndex(1); _pName->setFocus(); _pName->selectAll(); }
  void deleteTalent(int index, Id id) {
    auto const* t = tree(index).find(id); if (!t) return;
    auto label = name(*t);
    edit(index, [&](TalentTree& tree) {
      tree.talents.erase(std::remove_if(tree.talents.begin(), tree.talents.end(), [&](auto const& x) { return x.id == id; }), tree.talents.end());
      for (auto& other : tree.talents) if (other.prerequisite == id) { other.prerequisite = 0; other.prerequisiteRank = 1; }
    }, "Deleted " + label + " (Undo brings it back)");
    if (_selected == id) select(index, 0);
  }
  void talentMenu(int index, Id id, QPoint at);
  void cellMenu(int index, int row, int column, QPoint at);
  void arrowMenu(int index, Id id, QPoint at);
  void treeMenu(int index, QPoint at);
private:
  struct Step { Id tab = 0; QString key; TalentTree before, after; };
  TalentStore* _store = nullptr;
  QVector<TalentTree> _originals, _trees, _templates;
  QVector<TalentTree*> _visible;
  QVector<TreeView*> _views;
  QString _scope;                 // "c:<class mask>" or "t:<template>"
  int _focus = -1, _selectedTree = 0;
  Id _selected = 0;
  bool _test = false, _compare = false;
  int _level = TalentRules::maxLevel;
  QHash<Id, int> _build; QVector<Id> _order;
  QHash<QString, QPair<QHash<Id, int>, QVector<Id>>> _builds; // per class or template
  QHash<Id, TalentReach> _reach;
  QHash<Id, QVector<TalentProblem>> _problems;
  QHash<int, QVector<TalentProblem>> _treeProblems;
  QVector<Step> _undo, _redo;
  SpellCache _spells;
  TalentTip* _tip = nullptr;
  // Toolbar, level bar.
  QComboBox* _class; QWidget* _treeButtons; QHBoxLayout* _treeButtonsLayout; QButtonGroup* _treeGroup = nullptr;
  QPushButton *_editMode, *_testMode, *_compareButton, *_undoButton, *_redoButton, *_testGame;
  QSlider* _levelSlider; QSpinBox* _levelSpin; QLabel *_points, *_perTree, *_legal; QPushButton* _resetPoints;
  QLabel *_banner, *_status;
  QWidget* _canvas; QHBoxLayout* _canvasLayout;
  // Side panel.
  QStackedWidget* _panel;
  QLabel *_tTitle, *_tSummary; QListWidget *_tProblems, *_tChanges; QPushButton *_tClone, *_tReset, *_tApply, *_tDeleteTemplate, *_tExport;
  QToolButton* _pIcon; QLineEdit* _pName; QLabel *_pPlace, *_pProblems, *_pReach, *_pPoints, *_pPreview, *_pRankNote;
  QSpinBox *_pRanks, *_pRequiresRank; QComboBox* _pRequires; QSlider* _pPreviewRank;
  QWidget* _pRankList; QVBoxLayout* _pRankLayout; QPushButton *_pNextRank, *_pEditable, *_pDelete;
  QString _rankSignature;

  QWidget* toolbar();
  QWidget* levelBar();
  QWidget* sidePanel();
  QWidget* treePage();
  QWidget* talentPage();
  void reload();
  void showScope(QString const& scope);
  void layoutViews();
  void analyse();
  void refresh() { analyse(); refreshBuild(); }
  void refreshBuild();
  void refreshPanel();
  void refreshTreePage();
  void refreshTalentPage();
  void rebuildRanks(TalentTree const&, Talent const&);
  TalentTree* findTree(Id tab, QString const& key) {
    for (auto& t : _trees) if (tab && t.tab == tab) return &t;
    for (auto& t : _templates) if (!tab && t.key == key) return &t;
    return nullptr;
  }
  void persist(TalentTree const& tree) {
    try { _store->save(tree); }
    catch (std::exception const& e) { status(QString("Not saved: %1").arg(e.what()), true); }
  }
  bool edit(int index, std::function<void(TalentTree&)> const& change, QString const& what) {
    auto& tree = *_visible[index];
    auto before = tree; change(tree);
    if (tree == before) return false;
    _undo.push_back({tree.tab, tree.key, before, tree}); _redo.clear();
    persist(tree); refresh(); status(what);
    return true;
  }
  void undo() { step(_undo, _redo, true); }
  void redo() { step(_redo, _undo, false); }
  void step(QVector<Step>& from, QVector<Step>& to, bool back) {
    if (from.isEmpty()) return;
    auto s = from.takeLast();
    auto* tree = findTree(s.tab, s.key);
    if (!tree) { status("That tree is gone; nothing to undo.", true); return; }
    *tree = back ? s.before : s.after;
    to.push_back(s); persist(*tree);
    if (_selected && !tree->find(_selected)) _selected = 0;
    refresh(); status(back ? "Undone." : "Redone.");
  }
  // Rank spells as the designer's own: the game's spells stay as they are; copies replace them in this talent.
  QVector<Id> ownRanks(Talent const& t, bool ask, QString const& why);
  Id copyRank(Id source, int rank, Id previous);
  Id newTalentSpell();
  Talent newTalent(int row, int column, QVector<Id> ranks) {
    Talent t; t.id = nextId(); t.row = row; t.column = column; t.ranks = std::move(ranks); t.flags = t.ranks.size() == 1 ? 1 : 0;
    return t;
  }
  Id nextId() const {
    Id highest = 0;
    for (auto const* list : {&_originals, &_trees, &_templates}) for (auto const& tree : *list) for (auto const& t : tree.talents) highest = std::max(highest, t.id);
    return highest + 1;
  }
  void createTalent(int index, int row, int column, QString const& how, Talent const* source = nullptr);
  void changeIcon(int index, Id id);
  void renameTalent(int index, Id id, QString const& text);
  void setRanks(int index, Id id, int count);
  void openRank(int index, Id id, int rank);
  void cloneRank(int index, Id id, int rank);
  void replaceRank(int index, Id id, int rank);
  void cloneTree(int index);
  void applyTemplate(int index, TalentTree const& source);
  void testInGame();
};

// ---------------------------------------------------------------- tree view
TreeView::Hit TreeView::hit(QPointF p) const {
  Hit h;
  auto const& tree = _e.tree(_index);
  if (p.y() < gridTop - 8) { h.header = true; return h; }
  int column = int(std::floor((p.x() - gridLeft) / cell)), row = int(std::floor((p.y() - gridTop) / cell));
  if (column >= 0 && column < TalentRules::columns && row >= 0 && row < TalentRules::tiers) { h.row = row; h.column = column; }
  for (auto const& t : tree.talents) {
    if (iconRect(t.row, t.column).adjusted(-3, -3, 3, 3).contains(p)) { h.talent = t.id; return h; }
    if (!_e.testing() && t.id == _hover && QLineF(p, handlePoint(t.row, t.column)).length() < 7) { h.talent = t.id; h.handle = true; return h; }
  }
  // Arrows: within a few pixels of one of their segments.
  for (auto const& l : TalentLogic::links(tree)) {
    for (int i = 0; i + 1 < l.path.size(); ++i) {
      auto a = centre(l.path[i].y(), l.path[i].x()), b = centre(l.path[i + 1].y(), l.path[i + 1].x());
      QPointF d = b - a; double len2 = d.x() * d.x() + d.y() * d.y(); if (len2 <= 0) continue;
      double u = std::clamp(((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / len2, 0.0, 1.0);
      if (QLineF(p, a + d * u).length() < 6) { h.arrow = l.to; return h; }
    }
  }
  return h;
}
void TreeView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::SmoothPixmapTransform);
  p.fillRect(rect(), QColor(13, 13, 16));
  p.setTransform(transform());
  auto const& tree = _e.tree(_index);
  bool test = _e.testing();
  auto const& build = _e.build();
  QRectF frame(0, 0, baseWidth, baseHeight);
  // Background painting, darkened so icons stand out.
  auto background = treeBackground(tree.background);
  if (!background.isNull()) p.drawPixmap(frame, background, QRectF(0, 0, 296, 384)); else {
    QLinearGradient g(0, 0, 0, baseHeight); g.setColorAt(0, QColor(40, 34, 28)); g.setColorAt(1, QColor(14, 12, 10)); p.fillRect(frame, g);
  }
  p.fillRect(frame, QColor(0, 0, 0, test ? 90 : 120));
  p.setPen(QPen(QColor(110, 96, 70), 2)); p.drawRect(frame.adjusted(1, 1, -1, -1));
  // Header: name, points or talent count, problems.
  QFont font = p.font(); font.setPixelSize(17); font.setBold(true); p.setFont(font);
  p.setPen(gold);
  QString title = tree.tab ? tree.name.toUpper() : "TEMPLATE: " + tree.name.toUpper();
  p.drawText(QRectF(10, 6, baseWidth - 20, 24), Qt::AlignLeft | Qt::AlignVCenter, title);
  font.setPixelSize(12); font.setBold(false); p.setFont(font);
  auto problems = _e.treeProblems(_index);
  int errors = 0, warnings = 0;
  for (auto const& pr : problems) (pr.error ? errors : warnings)++;
  for (auto const& t : tree.talents) for (auto const& pr : _e.problemsOf(t.id)) (pr.error ? errors : warnings)++;
  QString sub = test ? QString("Points spent: %1").arg(tree.points(build)) : QString("%1 / %2 talents").arg(tree.talents.size()).arg(TalentRules::talentsPerTree);
  p.setPen(QColor(230, 230, 230)); p.drawText(QRectF(10, 30, baseWidth - 20, 18), Qt::AlignLeft | Qt::AlignVCenter, sub);
  if (errors || warnings) {
    p.setPen(errors ? red : amber);
    p.drawText(QRectF(10, 30, baseWidth - 20, 18), Qt::AlignRight | Qt::AlignVCenter, errors ? QString("⚠ %1").arg(plural(errors, "problem")) : QString("⚠ %1").arg(plural(warnings, "warning")));
  }
  if (!_e.focused() && _e.treeCount() > 1) { p.setPen(grey); p.drawText(QRectF(10, 6, baseWidth - 20, 24), Qt::AlignRight | Qt::AlignVCenter, "double-click to zoom"); }
  // Rows: the points each one needs, and the empty slots (edit mode).
  font.setPixelSize(10); p.setFont(font);
  for (int r = 0; r < TalentRules::tiers; ++r) {
    bool open = !test || tree.points(build) >= TalentRules::requiredPoints(r);
    p.setPen(open ? QColor(200, 190, 160, 170) : QColor(120, 120, 120, 140));
    p.drawText(QRectF(2, gridTop + r * cell, gridLeft - 6, cell), Qt::AlignRight | Qt::AlignVCenter, QString::number(TalentRules::requiredPoints(r)));
    if (test) continue;
    for (int c = 0; c < TalentRules::columns; ++c) {
      if (tree.at(r, c)) continue;
      bool hovered = r == _hoverRow && c == _hoverColumn && !_hover;
      p.setPen(QPen(QColor(255, 255, 255, hovered ? 140 : 38), 1, Qt::DashLine)); p.setBrush(hovered ? QColor(255, 255, 255, 18) : Qt::NoBrush);
      p.drawRoundedRect(iconRect(r, c), 4, 4);
      if (hovered) { p.setPen(QColor(255, 255, 255, 150)); font.setPixelSize(20); p.setFont(font); p.drawText(iconRect(r, c), Qt::AlignCenter, "+"); font.setPixelSize(10); p.setFont(font); }
    }
  }
  // Comparison: where talents were.
  auto const* original = _e.comparing() ? _e.originalOf(tree) : nullptr;
  if (original) {
    for (auto const& o : original->talents) {
      auto const* now = tree.find(o.id);
      if (now && now->row == o.row && now->column == o.column) continue;
      auto r = iconRect(o.row, o.column);
      p.setOpacity(0.35); p.drawPixmap(r, iconPixmap(_e.icon(o), true), QRectF(0, 0, 64, 64)); p.setOpacity(1);
      p.setPen(QPen(now ? blue : red, 2, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(r.adjusted(-2, -2, 2, 2), 4, 4);
      if (now) { p.setPen(QPen(blue, 1.2, Qt::DotLine)); p.drawLine(centre(o.row, o.column), centre(now->row, now->column)); }
      else { p.setPen(red); font.setPixelSize(9); font.setBold(true); p.setFont(font); p.drawText(r.adjusted(-10, 0, 10, 14), Qt::AlignHCenter | Qt::AlignBottom, "REMOVED"); font.setBold(false); }
    }
    // The original arrows that are gone or different.
    for (auto const& l : TalentLogic::links(*original)) {
      auto const* now = tree.find(l.to);
      if (now && now->prerequisite == l.from) continue;
      QPainterPath path; bool first = true;
      for (auto const& c : l.path) { auto pt = centre(c.y(), c.x()); if (first) path.moveTo(pt); else path.lineTo(pt); first = false; }
      p.setPen(QPen(QColor(235, 60, 50, 150), 2, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawPath(path);
    }
  }
  // Prerequisite arrows: under the icons, gold when met, grey when not, red when the game cannot draw them.
  QSet<Id> lit = _hover ? _e.pathTo(_index, _hover) : QSet<Id>();
  auto arrowHead = [&](QPointF tip, QPointF direction, QColor color) {
    QPointF n(-direction.y(), direction.x());
    QPolygonF head; head << tip << tip - direction * 9 + n * 6 << tip - direction * 9 - n * 6;
    p.setPen(Qt::NoPen); p.setBrush(color); p.drawPolygon(head);
  };
  for (auto const& l : TalentLogic::links(tree)) {
    auto const* to = tree.find(l.to); auto const* from = tree.find(l.from);
    bool met = !test || (build.value(l.from) >= to->prerequisiteRank && tree.points(build) >= TalentRules::requiredPoints(to->row));
    QColor color = !l.blocked.isEmpty() ? red : met ? gold : grey;
    bool highlighted = lit.contains(l.to) && lit.contains(l.from);
    if (_e.selected() == l.to && _e.selectedTree() == _index && !test) highlighted = true;
    QVector<QPointF> points; for (auto const& c : l.path) points << centre(c.y(), c.x());
    // End at the talent's edge, with the arrow pointing into it.
    QPointF last = points.back(), before = points[points.size() - 2];
    QPointF dir = last - before; double len = std::hypot(dir.x(), dir.y()); if (len > 0) dir /= len;
    points.back() = last - dir * (iconSize / 2 + 3);
    QPainterPath path(points[0]); for (int i = 1; i < points.size(); ++i) path.lineTo(points[i]);
    if (highlighted) { p.setPen(QPen(QColor(255, 240, 120, 110), 9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(Qt::NoBrush); p.drawPath(path); }
    p.setPen(QPen(QColor(0, 0, 0, 200), 6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(Qt::NoBrush); p.drawPath(path);
    p.setPen(QPen(highlighted ? QColor(255, 245, 150) : color, 3.2, l.blocked.isEmpty() ? Qt::SolidLine : Qt::DashLine, Qt::RoundCap, Qt::RoundJoin)); p.drawPath(path);
    arrowHead(points.back() + dir * 2, dir, highlighted ? QColor(255, 245, 150) : color);
    Q_UNUSED(from);
  }
  // Talents.
  for (auto const& t : tree.talents) {
    auto r = iconRect(t.row, t.column);
    int rank = build.value(t.id), max = t.maxRank();
    bool canLearn = test && TalentLogic::cannotLearn(tree, build, t.id, _e.available()).isEmpty();
    bool locked = test && rank == 0 && !canLearn;
    bool maxed = test && rank >= max && max > 0;
    bool dragged = _dragging && _press == t.id;
    auto problems = _e.problemsOf(t.id);
    bool error = false, warning = false; for (auto const& pr : problems) (pr.error ? error : warning) = true;
    auto reach = _e.reachOf(t.id);
    if (!lit.isEmpty() && !lit.contains(t.id)) p.setOpacity(0.45);
    if (dragged) p.setOpacity(0.3);
    // Slot frame: the game's coloured border (green: can take points, gold: maxed, grey: locked).
    QColor border = test ? (maxed ? gold : canLearn || rank > 0 ? green : QColor(90, 90, 90)) : QColor(170, 150, 100);
    if (!test && error) border = red; else if (!test && warning) border = amber;
    if (lit.contains(t.id)) { p.setPen(Qt::NoPen); p.setBrush(QColor(255, 230, 90, 70)); p.drawRoundedRect(r.adjusted(-7, -7, 7, 7), 8, 8); }
    if (_e.selected() == t.id && _e.selectedTree() == _index) { p.setPen(QPen(QColor(120, 220, 255), 3)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(r.adjusted(-6, -6, 6, 6), 7, 7); }
    p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, 220)); p.drawRoundedRect(r.adjusted(-4, -4, 4, 4), 5, 5);
    p.drawPixmap(r, iconPixmap(_e.icon(t), locked), QRectF(0, 0, 64, 64));
    if (locked) { p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, 90)); p.drawRect(r); }
    p.setPen(QPen(border, 2.4)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(r.adjusted(-2.5, -2.5, 2.5, 2.5), 4, 4);
    if (!test && !reach.reachable && !t.ranks.isEmpty()) {
      p.setPen(QPen(QColor(235, 60, 50, 200), 2)); p.drawLine(r.topLeft() + QPointF(4, 4), r.bottomRight() - QPointF(4, 4)); p.drawLine(r.topRight() + QPointF(-4, 4), r.bottomLeft() + QPointF(4, -4));
    }
    // Rank badge: "2/5".
    QString badge = QString("%1/%2").arg(rank).arg(max);
    font.setPixelSize(11); font.setBold(true); p.setFont(font);
    QRectF b(r.right() - 16, r.bottom() - 7, 30, 15);
    p.setPen(QPen(QColor(150, 150, 150), 1)); p.setBrush(QColor(0, 0, 0, 230)); p.drawRoundedRect(b, 4, 4);
    p.setPen(test ? (maxed ? gold : canLearn || rank > 0 ? green : grey) : QColor(235, 235, 235)); p.drawText(b, Qt::AlignCenter, badge);
    // Edit mode: the earliest level, and comparison badges.
    font.setPixelSize(9); font.setBold(true); p.setFont(font);
    if (!test && !t.ranks.isEmpty()) {
      p.setPen(reach.reachable ? QColor(200, 200, 200) : red);
      p.drawText(QRectF(r.left() - 10, r.top() - 15, iconSize + 20, 12), Qt::AlignHCenter | Qt::AlignBottom, reach.reachable ? QString("Lv %1").arg(reach.level) : QString("unreachable"));
    }
    if (original) {
      auto const* o = original->find(t.id);
      QString tag; QColor color;
      if (!o) { tag = "NEW"; color = green; }
      else if (o->row != t.row || o->column != t.column) { tag = "MOVED"; color = blue; }
      if (o && (o->ranks != t.ranks || o->prerequisite != t.prerequisite || (t.prerequisite && o->prerequisiteRank != t.prerequisiteRank))) { tag = tag.isEmpty() ? "CHANGED" : tag + "+"; if (!color.isValid()) color = amber; }
      if (!tag.isEmpty()) {
        QRectF pill(r.left() - 6, r.top() - 6, 8 + tag.size() * 6.2, 13);
        p.setPen(Qt::NoPen); p.setBrush(color); p.drawRoundedRect(pill, 4, 4); p.setPen(Qt::black); p.drawText(pill, Qt::AlignCenter, tag);
      }
    }
    if (error || warning) {
      QRectF dot(r.left() - 7, r.bottom() - 8, 14, 14);
      p.setPen(Qt::NoPen); p.setBrush(error ? red : amber); p.drawEllipse(dot); p.setPen(Qt::black); p.drawText(dot, Qt::AlignCenter, "!");
    }
    p.setOpacity(1);
    // The link handle under a hovered talent.
    if (!test && t.id == _hover && !_dragging) {
      auto h = handlePoint(t.row, t.column);
      p.setPen(QPen(gold, 1.5)); p.setBrush(QColor(30, 30, 30)); p.drawEllipse(h, 6, 6);
      p.drawLine(h + QPointF(0, -3), h + QPointF(0, 3));
    }
  }
  // Dragging a talent: the cell it goes to.
  if (_dragging && _press) {
    auto const* t = tree.find(_press);
    int row = int(std::floor((_cursor.y() - gridTop) / cell)), column = int(std::floor((_cursor.x() - gridLeft) / cell));
    bool inside = row >= 0 && row < TalentRules::tiers && column >= 0 && column < TalentRules::columns;
    if (inside) {
      auto const* there = tree.at(row, column);
      p.setPen(QPen(there && there->id != _press ? blue : green, 2.5, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(iconRect(row, column).adjusted(-5, -5, 5, 5), 6, 6);
      font.setPixelSize(10); p.setFont(font); p.setPen(Qt::white);
      p.drawText(iconRect(row, column).adjusted(-20, 44, 20, 58), Qt::AlignCenter, there && there->id != _press ? "swap" : QString("needs %1 pts").arg(TalentRules::requiredPoints(row)));
    }
    if (t) { p.setOpacity(0.85); p.drawPixmap(QRectF(_cursor.x() - iconSize / 2, _cursor.y() - iconSize / 2, iconSize, iconSize), iconPixmap(_e.icon(*t), false), QRectF(0, 0, 64, 64)); p.setOpacity(1); }
  }
  // Linking: a line to the cursor, green over a talent that can require it.
  if (_linking && _press) {
    auto const* from = tree.find(_press);
    auto h = hit(_cursor);
    QString why = h.talent && h.talent != _press ? TalentLogic::cannotRequire(tree, h.talent, _press) : QString("Drop on the talent that should require it");
    bool ok = h.talent && h.talent != _press && why.isEmpty();
    if (from) {
      p.setPen(QPen(ok ? green : h.talent ? red : gold, 2.5, Qt::DashLine)); p.drawLine(centre(from->row, from->column), _cursor);
      if (h.talent) { auto const* to = tree.find(h.talent); p.setPen(QPen(ok ? green : red, 3)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(iconRect(to->row, to->column).adjusted(-6, -6, 6, 6), 7, 7); }
      font.setPixelSize(10); p.setFont(font); p.setPen(ok ? green : QColor(255, 200, 190));
      p.drawText(QRectF(_cursor.x() - 90, _cursor.y() + 14, 180, 30), Qt::AlignHCenter | Qt::TextWordWrap, ok ? "requires " + _e.name(*from) : why);
    }
  }
}
void TreeView::hover(QPointF base, QPoint global) {
  auto h = hit(base);
  Id was = _hover; int row = _hoverRow, column = _hoverColumn;
  _hover = h.talent; _hoverRow = h.row; _hoverColumn = h.column;
  if (_hover != was || row != _hoverRow || column != _hoverColumn) update();
  setCursor(h.talent ? (h.handle ? Qt::CrossCursor : _e.testing() ? Qt::PointingHandCursor : Qt::OpenHandCursor) : h.arrow && !_e.testing() ? Qt::PointingHandCursor : Qt::ArrowCursor);
  auto const& tree = _e.tree(_index);
  if (h.talent && !_dragging && !_linking) {
    auto const* t = tree.find(h.talent);
    _e.showTip(_e.tooltip(_index, *t, _e.testing() ? _e.build().value(t->id) : 0, _e.testing()), global);
  } else if (h.arrow && !_e.testing() && !_dragging && !_linking) {
    auto const* t = tree.find(h.arrow); auto const* pr = t ? tree.find(t->prerequisite) : nullptr;
    if (t && pr) {
      auto l = TalentLogic::link(tree, *t);
      _e.showTip("<b>" + _e.name(*t).toHtmlEscaped() + "</b> requires " + plural(t->prerequisiteRank, "point") + " in <b>" + _e.name(*pr).toHtmlEscaped() + "</b>"
                 + (l && !l->blocked.isEmpty() ? "<br><span style='color:" + redText + "'>" + l->blocked.toHtmlEscaped() + "</span>" : QString())
                 + "<br><span style='color:" + greyText + "'>Click to change or remove it.</span>", global);
    }
  } else _e.hideTip();
}
void TreeView::mousePressEvent(QMouseEvent* event) {
  auto base = toBase(event->pos()); auto h = hit(base);
  _e.hideTip();
  if (event->button() == Qt::RightButton) {
    if (_e.testing()) { if (h.talent) _e.unlearn(_index, h.talent); return; }
    if (h.talent) _e.talentMenu(_index, h.talent, event->globalPos());
    else if (h.arrow) _e.arrowMenu(_index, h.arrow, event->globalPos());
    else if (h.header) _e.treeMenu(_index, event->globalPos());
    else if (h.row >= 0) _e.cellMenu(_index, h.row, h.column, event->globalPos());
    return;
  }
  if (event->button() != Qt::LeftButton) return;
  _press = h.talent; _pressAt = event->pos(); _cursor = base;
  if (h.talent && h.handle) { _linking = true; update(); }
  if (!h.talent && h.arrow && !_e.testing()) { _e.arrowMenu(_index, h.arrow, event->globalPos()); return; }
  if (!h.talent && !_e.testing()) _e.select(_index, 0);
}
void TreeView::mouseMoveEvent(QMouseEvent* event) {
  _cursor = toBase(event->pos());
  if (_press && !_e.testing() && !_dragging && !_linking && (event->buttons() & Qt::LeftButton) && (event->pos() - _pressAt).manhattanLength() > 6) {
    if (event->modifiers() & Qt::ShiftModifier) _linking = true; else _dragging = true;
    _e.hideTip(); setCursor(_linking ? Qt::CrossCursor : Qt::ClosedHandCursor);
  }
  if (_dragging || _linking) { update(); return; }
  hover(_cursor, event->globalPos());
}
void TreeView::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) return;
  auto base = toBase(event->pos()); auto h = hit(base);
  Id pressed = _press; bool dragging = _dragging, linking = _linking;
  _press = 0; _dragging = _linking = false; update();
  if (!pressed) return;
  if (dragging) {
    int row = int(std::floor((base.y() - gridTop) / cell)), column = int(std::floor((base.x() - gridLeft) / cell));
    if (row >= 0 && row < TalentRules::tiers && column >= 0 && column < TalentRules::columns) _e.moveTalent(_index, pressed, row, column);
    else _e.status("Dropped outside the talent window: nothing moved.");
    return;
  }
  if (linking) { if (h.talent && h.talent != pressed) _e.link(_index, pressed, h.talent); else if (h.talent != pressed) _e.status("Drop the link on the talent that should require it."); return; }
  if (h.talent != pressed) return;
  if (_e.testing()) _e.learn(_index, pressed); else _e.select(_index, pressed);
  hover(base, event->globalPos());
}
void TreeView::mouseDoubleClickEvent(QMouseEvent* event) {
  auto h = hit(toBase(event->pos()));
  if (h.talent && !_e.testing()) _e.openTalent(_index, h.talent);
  else if (h.header && _e.treeCount() > 1) _e.focusTree(_e.focused() ? -1 : _index);
}
void TreeView::leaveEvent(QEvent*) { _hover = 0; _hoverRow = _hoverColumn = -1; _e.hideTip(); update(); }
void TreeView::keyPressEvent(QKeyEvent* event) {
  if (_e.testing() || _e.selectedTree() != _index || !_e.selected()) return QWidget::keyPressEvent(event);
  auto const* t = _e.tree(_index).find(_e.selected());
  if (!t) return;
  switch (event->key()) {
    case Qt::Key_Delete: case Qt::Key_Backspace: _e.deleteTalent(_index, t->id); return;
    case Qt::Key_Up: _e.moveTalent(_index, t->id, t->row - 1, t->column); return;
    case Qt::Key_Down: _e.moveTalent(_index, t->id, t->row + 1, t->column); return;
    case Qt::Key_Left: _e.moveTalent(_index, t->id, t->row, t->column - 1); return;
    case Qt::Key_Right: _e.moveTalent(_index, t->id, t->row, t->column + 1); return;
    default: QWidget::keyPressEvent(event);
  }
}

// ---------------------------------------------------------------- editor: layout
QWidget* TalentEditor::toolbar() {
  auto bar = new QWidget; auto row = new QHBoxLayout(bar); row->setContentsMargins(0, 0, 0, 0);
  _class = new QComboBox; _class->setMinimumWidth(180); row->addWidget(_class);
  _treeButtons = new QWidget; _treeButtonsLayout = new QHBoxLayout(_treeButtons); _treeButtonsLayout->setContentsMargins(0, 0, 0, 0); _treeButtonsLayout->setSpacing(2);
  row->addWidget(_treeButtons);
  row->addSpacing(16);
  _editMode = new QPushButton("EDIT TREE"); _testMode = new QPushButton("TEST BUILD");
  for (auto b : {_editMode, _testMode}) { b->setCheckable(true); b->setMinimumHeight(30); b->setStyleSheet("QPushButton { font-weight: bold; padding: 4px 16px; } QPushButton:checked { background: #6b4f12; color: #ffd100; }"); row->addWidget(b); }
  auto modes = new QButtonGroup(bar); modes->addButton(_editMode); modes->addButton(_testMode); _editMode->setChecked(true);
  _editMode->setToolTip("Drag talents, link prerequisites, right-click empty slots to create talents.");
  _testMode->setToolTip("Spend points like a player: click to learn, right-click to give a point back.");
  _compareButton = button("Compare with Original", Icon::exchangealt); _compareButton->setCheckable(true); row->addWidget(_compareButton);
  _compareButton->setToolTip("Show what changed from the game's own tree: added, removed, moved and changed talents and arrows.");
  row->addStretch();
  _undoButton = button("Undo", Icon::undo); _redoButton = button("Redo", Icon::redo); row->addWidget(_undoButton); row->addWidget(_redoButton);
  _testGame = button("Test In Game…", Icon::play); row->addWidget(_testGame);
  _testGame->setToolTip("Put the edited trees into the local server and test client, and log in with this level (and build).");
  connect(_class, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { showScope(_class->currentData().toString()); });
  connect(_editMode, &QPushButton::toggled, this, [this](bool on) { _test = !on; hideTip(); refresh(); status(_test ? "Test Build: click a talent to spend a point, right-click to take it back." : "Edit Tree: drag to move, drag the handle under a talent (or Shift-drag) to link, right-click for more."); });
  connect(_compareButton, &QPushButton::toggled, this, [this](bool on) { _compare = on; refresh(); });
  connect(_undoButton, &QPushButton::clicked, this, [this] { undo(); });
  connect(_redoButton, &QPushButton::clicked, this, [this] { redo(); });
  connect(_testGame, &QPushButton::clicked, this, [this] { testInGame(); });
  return bar;
}
QWidget* TalentEditor::levelBar() {
  auto box = new QFrame; box->setObjectName("LevelBar");
  box->setStyleSheet("#LevelBar { background: #1b1710; border: 1px solid #5a4a26; border-radius: 6px; } #LevelBar QLabel { color: #f0e6c8; }");
  auto row = new QHBoxLayout(box);
  auto title = new QLabel("CHARACTER LEVEL"); title->setStyleSheet("color: #ffd100; font-weight: bold; font-size: 15px;"); row->addWidget(title);
  row->addWidget(new QLabel(QString::number(TalentRules::firstLevel)));
  _levelSlider = new QSlider(Qt::Horizontal); _levelSlider->setRange(TalentRules::firstLevel, TalentRules::maxLevel); _levelSlider->setMinimumWidth(260); row->addWidget(_levelSlider, 2);
  row->addWidget(new QLabel(QString::number(TalentRules::maxLevel)));
  _levelSpin = new QSpinBox; _levelSpin->setRange(1, TalentRules::maxLevel); _levelSpin->setStyleSheet("font-size: 15px; font-weight: bold;"); row->addWidget(_levelSpin);
  row->addSpacing(14);
  _points = new QLabel; _points->setTextFormat(Qt::RichText); row->addWidget(_points, 2);
  _perTree = new QLabel; _perTree->setTextFormat(Qt::RichText); row->addWidget(_perTree, 2);
  _legal = new QLabel; _legal->setTextFormat(Qt::RichText); row->addWidget(_legal, 1);
  _resetPoints = button("Reset Points", Icon::undoalt); row->addWidget(_resetPoints);
  _levelSlider->setValue(_level); _levelSpin->setValue(_level);
  auto setLevel = [this](int level) {
    _level = level;
    { QSignalBlocker a(_levelSlider), b(_levelSpin); _levelSlider->setValue(std::max(level, TalentRules::firstLevel)); _levelSpin->setValue(level); }
    // The point budget is enforced: lowering the level gives back the points learned last.
    int removed = 0;
    while (spent() > TalentRules::pointsAt(_level) && !_order.isEmpty()) { auto id = _order.takeLast(); if (--_build[id] <= 0) _build.remove(id); ++removed; }
    if (removed) status(QString("Level %1 gives %2: took back the last %3.").arg(_level).arg(plural(TalentRules::pointsAt(_level), "talent point")).arg(plural(removed, "point")));
    refreshBuild();
  };
  connect(_levelSlider, &QSlider::valueChanged, this, setLevel);
  connect(_levelSpin, qOverload<int>(&QSpinBox::valueChanged), this, setLevel);
  connect(_resetPoints, &QPushButton::clicked, this, [this] { _build.clear(); _order.clear(); refreshBuild(); status("All points given back."); });
  return box;
}
QWidget* TalentEditor::sidePanel() {
  _panel = new QStackedWidget;
  _panel->addWidget(treePage()); _panel->addWidget(talentPage());
  return _panel;
}
QWidget* TalentEditor::treePage() {
  auto page = new QWidget; auto layout = new QVBoxLayout(page);
  _tTitle = editorTitle({}, page); layout->addWidget(_tTitle);
  _tSummary = new QLabel; _tSummary->setWordWrap(true); _tSummary->setTextFormat(Qt::RichText); layout->addWidget(_tSummary);
  layout->addWidget(new QLabel("<b>Problems</b>"));
  _tProblems = new QListWidget; _tProblems->setMaximumHeight(170); _tProblems->setWordWrap(true); layout->addWidget(_tProblems);
  layout->addWidget(new QLabel("<b>Changes from the original</b>"));
  _tChanges = new QListWidget; _tChanges->setMaximumHeight(220); _tChanges->setWordWrap(true); layout->addWidget(_tChanges);
  auto grid = new QGridLayout; layout->addLayout(grid);
  _tClone = button("Clone Tree as Template…", Icon::clone); _tApply = button("Replace With Template", Icon::paintbrush);
  _tReset = button("Reset From Original…", Icon::history); _tDeleteTemplate = button("Delete Template…", Icon::trash);
  _tExport = button("Export Server Table…", Icon::filedownload);
  grid->addWidget(_tClone, 0, 0); grid->addWidget(_tApply, 0, 1); grid->addWidget(_tReset, 1, 0); grid->addWidget(_tDeleteTemplate, 1, 1); grid->addWidget(_tExport, 2, 0, 1, 2);
  _tClone->setToolTip("Copy this tree into a template: an editable tree of its own to experiment on, never sent to the game.");
  _tApply->setToolTip("Put a template's talents into this tree (Undo takes it back).");
  _tReset->setToolTip("Put the game's own tree back. Spells you made for it stay in your library.");
  _tExport->setToolTip("Save the server's Talent.dbc with your trees, for a production server administrator.");
  layout->addWidget(hint("Click a talent to edit it. Right-click an empty slot to create one. Drag talents to move them; drag the handle under a talent to the talent "
                         "that should require it. Double-click a tree's title to zoom in or out."));
  layout->addStretch();
  auto current = [this]() -> int { return std::clamp(_selectedTree, 0, treeCount() - 1); };
  connect(_tProblems, &QListWidget::itemClicked, this, [=, this](QListWidgetItem* item) { if (auto id = item->data(Qt::UserRole).toUInt()) select(current(), id); });
  connect(_tChanges, &QListWidget::itemClicked, this, [=, this](QListWidgetItem* item) { if (auto id = item->data(Qt::UserRole).toUInt(); tree(current()).find(id)) select(current(), id); });
  connect(_tClone, &QPushButton::clicked, this, [=, this] { cloneTree(current()); });
  connect(_tApply, &QPushButton::clicked, this, [=, this] {
    QMenu menu(this);
    if (_templates.isEmpty()) menu.addAction("No templates yet: Clone Tree as Template first")->setEnabled(false);
    for (auto const& t : _templates) menu.addAction(t.name, this, [=, this] { applyTemplate(current(), t); });
    menu.exec(QCursor::pos());
  });
  connect(_tReset, &QPushButton::clicked, this, [=, this] {
    auto& tree = *_visible[current()];
    auto const* original = originalOf(tree);
    if (!original || tree == *original) return;
    if (QMessageBox::question(this, "Reset From Original", "Put the game's own " + tree.name + " tree back?\n\nUndo can bring your version back while this editor is open. "
                              "Spells you made for it stay in your spell library.") != QMessageBox::Yes) return;
    edit(current(), [&](TalentTree& t) { t.talents = original->talents; }, tree.name + " is the game's own tree again.");
  });
  connect(_tDeleteTemplate, &QPushButton::clicked, this, [=, this] {
    auto const& tree = *_visible[current()];
    if (tree.tab || QMessageBox::question(this, "Delete Template", "Delete the template \"" + tree.name + "\"?") != QMessageBox::Yes) return;
    try { _store->removeTemplate(tree.key); } catch (std::exception const& e) { status(e.what(), true); return; }
    reload();
  });
  connect(_tExport, &QPushButton::clicked, this, [this] {
    auto path = QFileDialog::getSaveFileName(this, "Export Talent.dbc", QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath("Talent.dbc"),
                                             "Client table (*.dbc)", nullptr, QFileDialog::DontUseNativeDialog);
    if (path.isEmpty()) return;
    try {
      auto bytes = _store->exportTable();
      QSaveFile file(path);
      if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) throw std::runtime_error("Cannot write the file.");
      QMessageBox::information(this, "Export Server Table", "Saved " + QDir::toNativeSeparators(path) + ".\n\nThe production server reads it from its dbc folder (restart it after "
                               "replacing Talent.dbc). Players need the same table: Local changes → Export Client Patch… includes it.");
    } catch (std::exception const& e) { QMessageBox::warning(this, "Export Server Table", e.what()); }
  });
  return page;
}
QWidget* TalentEditor::talentPage() {
  auto page = new QWidget; auto layout = new QVBoxLayout(page);
  auto back = new QPushButton("‹ Tree overview"); back->setFlat(true); back->setStyleSheet("text-align: left; color: palette(link);"); layout->addWidget(back);
  auto head = new QHBoxLayout; layout->addLayout(head);
  _pIcon = new QToolButton; _pIcon->setIconSize(QSize(56, 56)); _pIcon->setAutoRaise(true); _pIcon->setToolTip("Change the icon"); head->addWidget(_pIcon);
  auto names = new QVBoxLayout; head->addLayout(names, 1);
  _pName = new QLineEdit; _pName->setStyleSheet("font-size: 16px; font-weight: bold;"); _pName->setToolTip("The talent's name: every rank spell is renamed"); names->addWidget(_pName);
  _pPlace = new QLabel; _pPlace->setStyleSheet("color: palette(mid);"); names->addWidget(_pPlace);
  _pProblems = new QLabel; _pProblems->setWordWrap(true); _pProblems->setTextFormat(Qt::RichText); layout->addWidget(_pProblems);
  auto reachBox = new QGroupBox("Reaching it"); auto reachLayout = new QVBoxLayout(reachBox);
  _pReach = new QLabel; _pReach->setWordWrap(true); _pReach->setTextFormat(Qt::RichText); reachLayout->addWidget(_pReach); layout->addWidget(reachBox);
  auto ranksBox = new QGroupBox("Ranks"); auto ranksLayout = new QVBoxLayout(ranksBox); layout->addWidget(ranksBox);
  auto count = new QHBoxLayout; ranksLayout->addLayout(count);
  count->addWidget(new QLabel("Ranks:")); _pRanks = new QSpinBox; _pRanks->setRange(1, TalentRules::maxRanks); count->addWidget(_pRanks); count->addStretch();
  _pRankNote = hint("More ranks copy the last rank with its values raised; fewer drop the last ones."); ranksLayout->addWidget(_pRankNote);
  _pRankList = new QWidget; _pRankLayout = new QVBoxLayout(_pRankList); _pRankLayout->setContentsMargins(0, 0, 0, 0); ranksLayout->addWidget(_pRankList);
  _pNextRank = button("Create Next Rank", Icon::plus); ranksLayout->addWidget(_pNextRank);
  auto requiresBox = new QGroupBox("Requires"); auto requiresLayout = new QFormLayout(requiresBox); layout->addWidget(requiresBox);
  _pRequires = new QComboBox; requiresLayout->addRow("Talent:", _pRequires);
  _pRequiresRank = new QSpinBox; _pRequiresRank->setSuffix(" points in it"); requiresLayout->addRow("Needs:", _pRequiresRank);
  _pPoints = new QLabel; _pPoints->setWordWrap(true); requiresLayout->addRow(_pPoints);
  auto previewBox = new QGroupBox("In game"); auto previewLayout = new QVBoxLayout(previewBox); layout->addWidget(previewBox);
  auto rankRow = new QHBoxLayout; previewLayout->addLayout(rankRow);
  rankRow->addWidget(new QLabel("Show with")); _pPreviewRank = new QSlider(Qt::Horizontal); rankRow->addWidget(_pPreviewRank, 1);
  auto card = new QFrame; card->setObjectName("Tip");
  card->setStyleSheet("#Tip { background: #08081a; border: 2px solid #8a8aa0; border-radius: 5px; } QLabel { color: white; background: transparent; }");
  auto cardLayout = new QVBoxLayout(card); _pPreview = new QLabel; _pPreview->setWordWrap(true); _pPreview->setTextFormat(Qt::RichText); cardLayout->addWidget(_pPreview);
  previewLayout->addWidget(card);
  auto actions = new QHBoxLayout; layout->addLayout(actions);
  _pEditable = button("Make Ranks Editable", Icon::clone); _pDelete = button("Delete Talent", Icon::trash);
  _pEditable->setToolTip("Copy the game's rank spells into your own, so their effects, values and icon can change. The game's spells stay as they are.");
  actions->addWidget(_pEditable); actions->addWidget(_pDelete);
  layout->addStretch();
  auto index = [this] { return std::clamp(_selectedTree, 0, treeCount() - 1); };
  connect(back, &QPushButton::clicked, this, [=, this] { select(index(), 0); });
  connect(_pIcon, &QToolButton::clicked, this, [=, this] { changeIcon(index(), _selected); });
  connect(_pName, &QLineEdit::editingFinished, this, [=, this] { renameTalent(index(), _selected, _pName->text()); });
  connect(_pRanks, qOverload<int>(&QSpinBox::valueChanged), this, [=, this](int n) { setRanks(index(), _selected, n); });
  connect(_pNextRank, &QPushButton::clicked, this, [=, this] { if (auto const* t = tree(index()).find(_selected)) setRanks(index(), _selected, t->maxRank() + 1); });
  connect(_pRequires, qOverload<int>(&QComboBox::activated), this, [=, this] {
    Id prerequisite = _pRequires->currentData().toUInt();
    if (auto const* t = tree(index()).find(_selected); !t || t->prerequisite == prerequisite) return;
    if (!prerequisite) { edit(index(), [&](TalentTree& t) { auto* x = t.find(_selected); x->prerequisite = 0; x->prerequisiteRank = 1; }, "Requirement removed."); return; }
    link(index(), prerequisite, _selected);
  });
  connect(_pRequiresRank, qOverload<int>(&QSpinBox::valueChanged), this, [=, this](int n) {
    edit(index(), [&](TalentTree& t) { if (auto* x = t.find(_selected); x && x->prerequisite) x->prerequisiteRank = n; }, QString("Now requires %1.").arg(plural(n, "point")));
  });
  connect(_pPreviewRank, &QSlider::valueChanged, this, [=, this] { refreshTalentPage(); });
  connect(_pEditable, &QPushButton::clicked, this, [=, this] {
    auto const* t = tree(index()).find(_selected); if (!t) return;
    auto ranks = ownRanks(*t, false, {});
    if (!ranks.isEmpty() && ranks != t->ranks) edit(index(), [&](TalentTree& tree) { tree.find(_selected)->ranks = ranks; }, "The ranks are your own spells now.");
  });
  connect(_pDelete, &QPushButton::clicked, this, [=, this] { deleteTalent(index(), _selected); });
  return page;
}

// ---------------------------------------------------------------- editor: data and refresh
void TalentEditor::reload() {
  _banner->setText({}); _banner->hide();
  if (!_store) { _banner->setText("Talent editing needs the managed Creator runtime (start Noggit from Creator/Noggit)."); _banner->show(); setEnabled(false); return; }
  try {
    _originals = _store->originals(); _trees = _store->current(); _templates = _store->templates();
  } catch (std::exception const& e) {
    _banner->setText(QString("The local server's talent tables cannot be read: %1").arg(e.what())); _banner->show();
    _originals.clear(); _trees.clear(); _templates.clear();
  }
  if (!databaseReady()) { _banner->setText((_banner->text().isEmpty() ? QString() : _banner->text() + "\n") + "The local database is not running: talent names, icons and descriptions come from it. "
                                           "Start the local server, then reopen this editor. Layout editing still works."); _banner->show(); }
  auto scope = _scope;
  { QSignalBlocker block(_class);
    _class->clear();
    for (auto const& c : talentClasses()) {
      bool any = std::any_of(_trees.begin(), _trees.end(), [&](auto const& t) { return t.classMask == c.mask; });
      if (any) _class->addItem(c.name, "c:" + QString::number(c.mask));
    }
    if (!_templates.isEmpty()) _class->insertSeparator(_class->count());
    for (auto const& t : _templates) _class->addItem("Template: " + t.name, "t:" + t.key);
    int at = _class->findData(scope); _class->setCurrentIndex(at >= 0 ? at : 0);
  }
  showScope(_class->currentData().toString());
}
void TalentEditor::showScope(QString const& scope) {
  if (!_scope.isEmpty()) _builds[_scope] = {_build, _order};
  _scope = scope;
  _visible.clear();
  if (scope.startsWith("c:")) { quint32 mask = scope.mid(2).toUInt(); for (auto& t : _trees) if (t.classMask == mask) _visible << &t; }
  else for (auto& t : _templates) if ("t:" + t.key == scope) _visible << &t;
  auto saved = _builds.value(scope); _build = saved.first; _order = saved.second;
  _selected = 0; _selectedTree = 0; _focus = -1;
  hideTip();
  // One view per tree, with a button to zoom into each.
  for (auto* v : _views) { v->hide(); _canvasLayout->removeWidget(v); v->deleteLater(); }
  _views.clear();
  delete _treeGroup; _treeGroup = new QButtonGroup(this);
  while (auto item = _treeButtonsLayout->takeAt(0)) { delete item->widget(); delete item; }
  if (_visible.isEmpty()) { refresh(); return; }
  auto all = new QPushButton("All Trees"); all->setCheckable(true); all->setChecked(true); _treeButtonsLayout->addWidget(all); _treeGroup->addButton(all, -1);
  connect(all, &QPushButton::clicked, this, [this] { focusTree(-1); });
  for (int i = 0; i < _visible.size(); ++i) {
    _views << new TreeView(*this, i, _canvas); _canvasLayout->addWidget(_views.back(), 1);
    auto b = new QPushButton(_visible[i]->name); b->setCheckable(true); _treeButtonsLayout->addWidget(b); _treeGroup->addButton(b, i);
    connect(b, &QPushButton::clicked, this, [this, i] { focusTree(i); });
  }
  _treeButtons->setVisible(_visible.size() > 1);
  _compareButton->setEnabled(_visible[0]->tab != 0);
  _testGame->setEnabled(_visible[0]->tab != 0);
  layoutViews();
  refresh();
}
void TalentEditor::layoutViews() {
  for (int i = 0; i < _views.size(); ++i) _views[i]->setVisible(_focus < 0 || _focus == i);
  if (_treeGroup) if (auto* b = _treeGroup->button(_focus)) { QSignalBlocker block(b); b->setChecked(true); }
  if (_focus >= 0) _selectedTree = _focus;
  for (auto* v : _views) v->update();
  refreshPanel();
}
void TalentEditor::analyse() {
  _reach.clear(); _problems.clear(); _treeProblems.clear();
  for (int i = 0; i < _visible.size(); ++i) {
    auto const& tree = *_visible[i];
    QSet<Id> spells;
    if (databaseReady()) for (auto const& t : tree.talents) for (auto id : t.ranks) if (id && spell(id).exists) spells.insert(id);
    // Unknown spells are only reported when the database could be asked (0 is never a spell: it keeps the set non-empty).
    if (databaseReady()) spells.insert(0);
    auto owners = tree.tab ? TalentLogic::spellOwners(_trees, tree.tab) : QHash<Id, QString>();
    for (auto const& p : TalentLogic::check(tree, owners, databaseReady() ? spells : QSet<Id>())) {
      if (p.talent) _problems[p.talent] << p; else _treeProblems[i] << p;
    }
    for (auto const& t : tree.talents) {
      _reach[t.id] = TalentLogic::reach(tree, t.id);
      for (auto id : t.ranks) if (id && spell(id).exists && spell(id).scripted && spell(id).own)
        _problems[t.id] << TalentProblem{t.id, QString("%1 has effects that need server scripting; the game's own spell has them, your copy may not.").arg(spell(id).name), false};
    }
  }
}
void TalentEditor::refreshBuild() {
  QHash<Id, int> repaired;
  for (auto* t : _visible) { auto mine = TalentLogic::repair(*t, _build); for (auto it = mine.begin(); it != mine.end(); ++it) repaired.insert(it.key(), it.value()); }
  if (repaired != _build) {
    _build = repaired;
    QHash<Id, int> seen; QVector<Id> order;
    for (auto id : _order) if (seen[id] < _build.value(id)) { ++seen[id]; order << id; }
    _order = order;
  }
  int budget = TalentRules::pointsAt(_level), used = spent(), left = budget - used;
  _points->setText(QString("Talent Points Available: <b>%1</b> &nbsp; Spent: <b>%2</b> &nbsp; Remaining: <b style='color:%3'>%4</b>").arg(budget).arg(used).arg(left < 0 ? redText : left == 0 ? goldText : greenText).arg(left));
  QStringList trees; for (auto* t : _visible) trees << QString("%1: <b>%2</b>").arg(t->name.toHtmlEscaped()).arg(t->points(_build));
  _perTree->setText(trees.join(" &nbsp; "));
  _legal->setText(left < 0 ? "<b style='color:" + redText + "'>✗ " + QString("Over budget by %1").arg(plural(-left, "point")) + "</b>"
                           : "<b style='color:" + greenText + "'>✓ Legal at level " + QString::number(_level) + "</b>");
  _undoButton->setEnabled(!_undo.isEmpty()); _redoButton->setEnabled(!_redo.isEmpty());
  for (auto* v : _views) v->update();
  refreshPanel();
}
void TalentEditor::refreshPanel() {
  if (_visible.isEmpty()) { _panel->setCurrentIndex(0); _tTitle->setText("NO TALENT TREES"); _tSummary->setText({}); return; }
  _selectedTree = std::clamp(_selectedTree, 0, treeCount() - 1);
  if (_selected && !tree(_selectedTree).find(_selected)) _selected = 0;
  if (_selected) { _panel->setCurrentIndex(1); refreshTalentPage(); } else { _panel->setCurrentIndex(0); refreshTreePage(); }
}
void TalentEditor::refreshTreePage() {
  auto const& t = tree(_selectedTree);
  auto const* original = originalOf(t);
  _tTitle->setText((t.tab ? className(t.classMask) + " — " + t.name : "Template — " + t.name).toUpper());
  int reachable = 0, latest = 0;
  for (auto const& talent : t.talents) { auto r = _reach.value(talent.id); if (r.reachable) { ++reachable; latest = std::max(latest, r.level); } }
  QString state = !t.tab ? "A template: edit and test it freely; it never goes into the game. Put it into a class tree with Replace With Template."
                : original && *original == t ? "The game's own tree." : "Edited — Test In Game puts it into the local server and test client.";
  _tSummary->setText(QString("%1<br><br>%2 of %3 slots used · %4 can be learned by level %5%6")
                     .arg(state.toHtmlEscaped()).arg(t.talents.size()).arg(TalentRules::talentsPerTree).arg(plural(reachable, "talent")).arg(TalentRules::maxLevel)
                     .arg(latest ? QString(" · the last one at level %1").arg(latest) : QString()));
  _tProblems->clear();
  for (auto const& p : treeProblems(_selectedTree)) { auto item = new QListWidgetItem((p.error ? "✗ " : "⚠ ") + p.text, _tProblems); item->setForeground(p.error ? red : amber); }
  for (auto const& talent : t.talents) for (auto const& p : problemsOf(talent.id)) {
    auto item = new QListWidgetItem((p.error ? "✗ " : "⚠ ") + name(talent) + ": " + p.text, _tProblems);
    item->setForeground(p.error ? red : amber); item->setData(Qt::UserRole, talent.id);
  }
  if (_tProblems->count() == 0) new QListWidgetItem("✓ No problems: the game can show and use this tree.", _tProblems);
  _tChanges->clear();
  if (original) {
    auto label = [this](Talent const& x) { return name(x); };
    for (auto const& c : TalentLogic::diff(*original, t, label)) {
      static QHash<int, QString> const marks{{int(TalentChange::Kind::Added), "+ "}, {int(TalentChange::Kind::Removed), "− "}, {int(TalentChange::Kind::Moved), "↔ "},
                                             {int(TalentChange::Kind::Ranks), "# "}, {int(TalentChange::Kind::Spells), "✎ "}, {int(TalentChange::Kind::Prerequisite), "→ "}};
      auto item = new QListWidgetItem(marks.value(int(c.kind)) + c.text, _tChanges); item->setData(Qt::UserRole, c.talent);
    }
    if (_tChanges->count() == 0) new QListWidgetItem("No changes.", _tChanges);
  } else new QListWidgetItem("Templates have no original.", _tChanges);
  _tReset->setEnabled(original && !(*original == t));
  _tDeleteTemplate->setVisible(!t.tab); _tApply->setVisible(t.tab != 0); _tReset->setVisible(t.tab != 0);
  _tExport->setEnabled(_store && !_store->modifiedTabs().isEmpty());
}
void TalentEditor::rebuildRanks(TalentTree const& tree, Talent const& t) {
  QString signature = QString::number(t.id); for (auto id : t.ranks) signature += "," + QString::number(id);
  signature += _test ? "t" : "e";
  if (signature == _rankSignature) return;
  _rankSignature = signature;
  while (auto item = _pRankLayout->takeAt(0)) { delete item->widget(); delete item; }
  int index = int(_visible.indexOf(const_cast<TalentTree*>(&tree)));
  for (int i = 0; i < t.maxRank(); ++i) {
    auto row = new QWidget; auto layout = new QHBoxLayout(row); layout->setContentsMargins(0, 2, 0, 2);
    auto v = spell(t.ranks[i]);
    auto icon = new QLabel; icon->setPixmap(iconPixmap(v.icon, false).scaled(28, 28, Qt::KeepAspectRatio, Qt::SmoothTransformation)); layout->addWidget(icon);
    auto text = new QLabel(QString("<b>Rank %1</b> → %2%3%4").arg(i + 1).arg(t.ranks[i] ? (v.name + (v.rank.isEmpty() ? QString() : " " + v.rank)).toHtmlEscaped() : QString("<i>no spell</i>"))
                           .arg(v.own ? QString(" <span style='color:%1'>(yours)</span>").arg(greenText) : QString()).arg(t.ranks[i] && !v.exists && databaseReady() ? QString(" <span style='color:%1'>missing</span>").arg(redText) : QString()));
    text->setTextFormat(Qt::RichText); text->setWordWrap(true); layout->addWidget(text, 1);
    auto open = new QToolButton; open->setIcon(Ui::FontAwesomeIcon(Icon::edit)); open->setToolTip("Open this rank's spell in the Spell Editor"); open->setEnabled(t.ranks[i] != 0);
    auto clone = new QToolButton; clone->setIcon(Ui::FontAwesomeIcon(Icon::clone)); clone->setToolTip(i ? "Replace with a copy of the previous rank, values raised" : "Replace with your own copy of this spell");
    auto replace = new QToolButton; replace->setIcon(Ui::FontAwesomeIcon(Icon::exchangealt)); replace->setToolTip("Use another spell for this rank");
    for (auto b : {open, clone, replace}) layout->addWidget(b);
    connect(open, &QToolButton::clicked, this, [=, this] { openRank(index, t.id, i); });
    connect(clone, &QToolButton::clicked, this, [=, this] { cloneRank(index, t.id, i); });
    connect(replace, &QToolButton::clicked, this, [=, this] { replaceRank(index, t.id, i); });
    _pRankLayout->addWidget(row);
  }
}
void TalentEditor::refreshTalentPage() {
  auto const& tr = tree(_selectedTree);
  auto const* t = tr.find(_selected);
  if (!t) return;
  bool editing = !_test;
  for (auto* w : std::initializer_list<QWidget*>{_pName, _pRanks, _pRequires, _pRequiresRank, _pNextRank, _pEditable, _pDelete, _pRankList}) w->setEnabled(editing);
  _pIcon->setEnabled(editing);
  _pIcon->setIcon(QIcon(iconPixmap(icon(*t), false)));
  bool own = !t->ranks.isEmpty() && std::all_of(t->ranks.begin(), t->ranks.end(), [&](Id id) { return id && spell(id).own; });
  { QSignalBlocker block(_pName); if (!_pName->hasFocus()) _pName->setText(name(*t)); }
  _pName->setReadOnly(!own);
  _pName->setToolTip(own ? "The talent's name: every rank spell is renamed" : "The ranks are game spells: Make Ranks Editable to rename it");
  _pPlace->setText(QString("%1 · %2 · drag it in the tree to move it").arg(tr.name, place(*t)));
  QStringList problems;
  for (auto const& p : problemsOf(t->id)) problems << QString("<span style='color:%1'>%2 %3</span>").arg(p.error ? redText : "#ffa028", p.error ? "✗" : "⚠", p.text.toHtmlEscaped());
  _pProblems->setText(problems.join("<br>")); _pProblems->setVisible(!problems.isEmpty());
  auto r = _reach.value(t->id);
  QStringList path; for (auto id : r.path) if (auto const* p = tr.find(id)) path << name(*p).toHtmlEscaped();
  path << "<b>" + name(*t).toHtmlEscaped() + "</b>";
  _pReach->setText(r.reachable ? QString("Earliest possible level: <b style='font-size:15px'>%1</b> (%2)<br>Requires %3 in %4%5")
                                   .arg(r.level).arg(plural(r.points, "talent point")).arg(plural(r.treePoints, "point")).arg(tr.name.toHtmlEscaped())
                                   .arg(r.path.isEmpty() ? QString() : "<br>Path: " + path.join(" → "))
                               : QString("<b style='color:%1'>Cannot be learned.</b> %2").arg(redText, r.why.toHtmlEscaped()));
  { QSignalBlocker block(_pRanks); _pRanks->setValue(std::max(1, t->maxRank())); }
  _pNextRank->setEnabled(editing && t->maxRank() < TalentRules::maxRanks);
  rebuildRanks(tr, *t);
  { QSignalBlocker a(_pRequires), b(_pRequiresRank);
    _pRequires->clear(); _pRequires->addItem("Nothing", 0u);
    for (auto const& other : tr.talents)
      if (other.id != t->id && TalentLogic::cannotRequire(tr, t->id, other.id).isEmpty())
        _pRequires->addItem(QIcon(iconPixmap(icon(other), false)), name(other) + " (" + place(other) + ")", other.id);
    if (t->prerequisite && _pRequires->findData(t->prerequisite) < 0) if (auto const* p = tr.find(t->prerequisite)) _pRequires->addItem(name(*p) + " — not allowed here", p->id);
    _pRequires->setCurrentIndex(std::max(0, _pRequires->findData(t->prerequisite)));
    auto const* p = tr.find(t->prerequisite);
    _pRequiresRank->setEnabled(editing && p); _pRequiresRank->setRange(1, p ? std::max(1, p->maxRank()) : 1); _pRequiresRank->setValue(t->prerequisiteRank);
  }
  _pPoints->setText(QString("Points required in %1: <b>%2</b> — set by its row (%3 per row). Move it up or down to change this.")
                    .arg(tr.name.toHtmlEscaped()).arg(TalentRules::requiredPoints(t->row)).arg(TalentRules::pointsPerTier));
  _pPoints->setTextFormat(Qt::RichText);
  { QSignalBlocker block(_pPreviewRank);
    int shown = _pPreviewRank->value(); _pPreviewRank->setRange(0, std::max(1, t->maxRank()));
    if (_test) shown = _build.value(t->id);
    _pPreviewRank->setValue(std::clamp(shown, 0, t->maxRank())); }
  _pPreview->setText(tooltip(_selectedTree, *t, _pPreviewRank->value(), _test));
  _pEditable->setVisible(!own && !t->ranks.isEmpty());
}
QString TalentEditor::tooltip(int index, Talent const& t, int rank, bool test) {
  auto const& tr = tree(index);
  int max = t.maxRank();
  QString html = "<div style='font-size:15px'><b>" + name(t).toHtmlEscaped() + "</b></div>";
  html += QString("<div>Rank %1/%2</div>").arg(rank).arg(max);
  auto requirement = [&](bool met, QString const& text) { return QString("<div style='color:%1'>%2</div>").arg(met || !test ? (test ? "white" : greyText) : redText, text.toHtmlEscaped()); };
  if (int needed = TalentRules::requiredPoints(t.row); needed && (rank == 0 || !test))
    html += requirement(tr.points(_build) >= needed, QString("Requires %1 points in %2 Talents").arg(needed).arg(tr.name));
  if (auto const* p = tr.find(t.prerequisite); p && (rank == 0 || !test))
    html += requirement(_build.value(p->id) >= t.prerequisiteRank, QString("Requires %1 in %2").arg(plural(t.prerequisiteRank, "point"), name(*p)));
  auto text = [&](int r) { if (r < 1 || r > max || !t.ranks[r - 1]) return QString("<i>No spell for this rank.</i>"); return spell(t.ranks[r - 1]).text; };
  if (max) html += QString("<div style='color:%1; margin-top:4px'>%2</div>").arg(goldText, text(std::max(1, rank)));
  if (rank > 0 && rank < max) html += QString("<div style='margin-top:8px'>Next rank:</div><div style='color:%1'>%2</div>").arg(goldText, text(rank + 1));
  if (test) {
    auto why = TalentLogic::cannotLearn(tr, _build, t.id, available());
    if (why.isEmpty()) html += QString("<div style='color:%1; margin-top:6px'>Click to learn</div>").arg(greenText);
    else if (rank < max) html += QString("<div style='color:%1; margin-top:6px'>%2</div>").arg(greyText, why.toHtmlEscaped());
    if (rank > 0) html += QString("<div style='color:%1'>Right-click to give a point back</div>").arg(greyText);
  } else {
    auto r = _reach.value(t.id);
    html += r.reachable ? QString("<div style='color:%1; margin-top:6px'>Earliest at level %2 (%3)</div>").arg(greyText).arg(r.level).arg(plural(r.points, "point"))
                        : QString("<div style='color:%1; margin-top:6px'>Cannot be learned: %2</div>").arg(redText, r.why.toHtmlEscaped());
    for (auto const& p : problemsOf(t.id)) html += QString("<div style='color:%1'>%2 %3</div>").arg(p.error ? redText : "#ffa028", p.error ? "✗" : "⚠", p.text.toHtmlEscaped());
  }
  return html;
}

// ---------------------------------------------------------------- editor: actions
void TalentEditor::talentMenu(int index, Id id, QPoint at) {
  auto const& tr = tree(index); auto const* t = tr.find(id); if (!t) return;
  select(index, id);
  QMenu menu(this);
  menu.addSection(name(*t));
  menu.addAction(Ui::FontAwesomeIcon(Icon::edit), "Edit Talent", this, [=, this] { openTalent(index, id); });
  menu.addAction(Ui::FontAwesomeIcon(Icon::image), "Change Icon…", this, [=, this] { changeIcon(index, id); });
  auto ranks = menu.addMenu("Ranks");
  for (int n = 1; n <= TalentRules::maxRanks; ++n) { auto a = ranks->addAction(QString::number(n), this, [=, this] { setRanks(index, id, n); }); a->setCheckable(true); a->setChecked(n == t->maxRank()); }
  auto needs = menu.addMenu(Ui::FontAwesomeIcon(Icon::link), "Requires");
  for (auto const& other : tr.talents) {
    if (other.id == id || !TalentLogic::cannotRequire(tr, id, other.id).isEmpty()) continue;
    auto a = needs->addAction(QIcon(iconPixmap(icon(other), false)), name(other), this, [=, this] { link(index, other.id, id); });
    a->setCheckable(true); a->setChecked(t->prerequisite == other.id);
  }
  if (needs->isEmpty()) needs->addAction("No talent above it can be required")->setEnabled(false);
  if (t->prerequisite) menu.addAction(Ui::FontAwesomeIcon(Icon::unlink), "Remove Requirement", this, [=, this] {
    edit(index, [&](TalentTree& tree) { auto* x = tree.find(id); x->prerequisite = 0; x->prerequisiteRank = 1; }, "Requirement removed.");
  });
  menu.addAction(Ui::FontAwesomeIcon(Icon::exchangealt), "Replace Talent With Spell…", this, [=, this] {
    auto spell = pickSpell(this, "Replace the talent with which spell?"); if (!spell) return;
    edit(index, [&](TalentTree& tree) { auto* x = tree.find(id); x->ranks = {*spell}; x->flags = 1; }, "Replaced; add ranks in the side panel.");
  });
  menu.addAction(Ui::FontAwesomeIcon(Icon::clone), "Duplicate", this, [=, this] {
    auto cell = TalentLogic::freeCell(tr, t->row);
    if (cell.x() < 0) { status("This tree has no free slot.", true); return; }
    createTalent(index, cell.y(), cell.x(), "clone", t);
  });
  menu.addSeparator();
  menu.addAction(Ui::FontAwesomeIcon(Icon::trash), "Delete Talent", this, [=, this] { deleteTalent(index, id); });
  menu.exec(at);
}
void TalentEditor::cellMenu(int index, int row, int column, QPoint at) {
  auto const& tr = tree(index);
  QMenu menu(this);
  menu.addSection(QString("Create Talent — row %1, column %2 (needs %3 points)").arg(row + 1).arg(column + 1).arg(TalentRules::requiredPoints(row)));
  bool full = tr.talents.size() >= TalentRules::talentsPerTree;
  if (full) menu.addAction(QString("This tree is full (%1 talents, the game's limit)").arg(TalentRules::talentsPerTree))->setEnabled(false);
  auto spell = menu.addAction(Ui::FontAwesomeIcon(Icon::magic), "From Existing Spell…", this, [=, this] { createTalent(index, row, column, "spell"); });
  auto clones = menu.addMenu(Ui::FontAwesomeIcon(Icon::clone), "Clone Existing Talent");
  // Talents of this class first, then any other class (names load when the menu opens).
  for (int i = 0; i < treeCount(); ++i) {
    auto sub = clones->addMenu(tree(i).name);
    connect(sub, &QMenu::aboutToShow, this, [=, this] {
      if (!sub->isEmpty()) return;
      for (auto const& t : tree(i).talents) sub->addAction(QIcon(iconPixmap(icon(t), false)), name(t), this, [=, this] { createTalent(index, row, column, "clone", tree(i).find(t.id)); });
    });
  }
  auto others = clones->addMenu("Other Classes");
  for (auto const& c : talentClasses()) {
    auto classMenu = others->addMenu(c.name);
    for (int k = 0; k < _trees.size(); ++k) {
      if (_trees[k].classMask != c.mask) continue;
      auto sub = classMenu->addMenu(_trees[k].name);
      connect(sub, &QMenu::aboutToShow, this, [=, this] {
        if (!sub->isEmpty()) return;
        for (auto const& t : _trees[k].talents) { auto copy = t; sub->addAction(QIcon(iconPixmap(icon(t), false)), name(t), this, [=, this] { createTalent(index, row, column, "clone", &copy); }); }
      });
    }
  }
  auto fresh = menu.addAction(Ui::FontAwesomeIcon(Icon::plus), "Create New Talent", this, [=, this] { createTalent(index, row, column, "new"); });
  for (auto a : {spell, fresh}) a->setEnabled(!full);
  clones->setEnabled(!full);
  menu.exec(at);
}
void TalentEditor::arrowMenu(int index, Id id, QPoint at) {
  auto const& tr = tree(index); auto const* t = tr.find(id); auto const* p = t ? tr.find(t->prerequisite) : nullptr;
  if (!p) return;
  select(index, id);
  QMenu menu(this);
  menu.addSection(name(*t) + " requires " + name(*p));
  auto change = menu.addMenu(Ui::FontAwesomeIcon(Icon::link), "Change Required Talent");
  for (auto const& other : tr.talents) {
    if (other.id == id || !TalentLogic::cannotRequire(tr, id, other.id).isEmpty()) continue;
    auto a = change->addAction(QIcon(iconPixmap(icon(other), false)), name(other), this, [=, this] { link(index, other.id, id); });
    a->setCheckable(true); a->setChecked(other.id == p->id);
  }
  auto rank = menu.addMenu("Required Points");
  for (int n = 1; n <= p->maxRank(); ++n) {
    auto a = rank->addAction(QString("%1 of %2").arg(n).arg(p->maxRank()), this, [=, this] {
      edit(index, [&](TalentTree& tree) { tree.find(id)->prerequisiteRank = n; }, QString("%1 now requires %2 in %3.").arg(name(*t), plural(n, "point"), name(*p)));
    });
    a->setCheckable(true); a->setChecked(n == t->prerequisiteRank);
  }
  menu.addAction(Ui::FontAwesomeIcon(Icon::unlink), "Remove Link", this, [=, this] {
    edit(index, [&](TalentTree& tree) { auto* x = tree.find(id); x->prerequisite = 0; x->prerequisiteRank = 1; }, "Link removed.");
  });
  menu.exec(at);
}
void TalentEditor::treeMenu(int index, QPoint at) {
  QMenu menu(this);
  menu.addSection(tree(index).name);
  if (treeCount() > 1) menu.addAction(Ui::FontAwesomeIcon(focused() ? Icon::compress : Icon::expand), focused() ? "Show All Trees" : "Zoom In", this, [=, this] { focusTree(focused() ? -1 : index); });
  menu.addAction(Ui::FontAwesomeIcon(Icon::clone), "Clone Tree as Template…", this, [=, this] { cloneTree(index); });
  menu.exec(at);
}
Id TalentEditor::copyRank(Id source, int rank, Id previous) {
  // A copy of `previous` raised to the next rank (values scaled like a talent's: 1, 2, 3…), or a plain copy of `source`.
  SpellDesign d;
  if (previous) {
    auto base = SpellService::load(previous);
    int at = std::max(1, rank);
    d = SpellService::nextRank(previous, std::max(1, base.level), double(at + 1) / at, base.cost, base.cooldown);
  } else d = SpellService::clone(source);
  d.teachable = false; // talents teach their ranks; trainers do not
  d.previous = 0; // talent ranks are chained by Talent.dbc, not spell_chain (the server ignores talent spells there)
  d.rank = QString("Rank %1").arg(rank + 1);
  auto id = SpellService::save(d);
  _spells.forget(id);
  return id;
}
QVector<Id> TalentEditor::ownRanks(Talent const& t, bool ask, QString const& why) {
  auto ranks = t.ranks;
  bool needed = std::any_of(ranks.begin(), ranks.end(), [&](Id id) { return id && !spell(id).own; });
  if (!needed) return ranks;
  if (!databaseReady()) { status("Start the local server first: rank spells are copied in the local database.", true); return {}; }
  if (ask && QMessageBox::question(this, "Make Ranks Editable", why + "\n\nThis talent's ranks are game spells. Creator copies them into your own spells (the game's stay as they are) "
                                   "and this talent uses the copies. Continue?") != QMessageBox::Yes) return {};
  try {
    for (int i = 0; i < ranks.size(); ++i) if (ranks[i] && !spell(ranks[i]).own) ranks[i] = copyRank(ranks[i], i, 0);
  } catch (std::exception const& e) { status(QString("Cannot copy the rank spells: %1").arg(e.what()), true); return {}; }
  return ranks;
}
Id TalentEditor::newTalentSpell() {
  // A passive talent spell as the game's are (Deflection, rank 1: passive, permanent, on yourself), renamed.
  SpellDesign d;
  try { d = SpellService::clone(16462); }
  catch (std::exception const&) {
    d = SpellService::blank();
    d.row["attributes"] = "464"; d.duration = 21; d.effects[0].type = 6; d.effects[0].aura = 47; d.effects[0].targetA = 1; d.effects[0].minValue = d.effects[0].maxValue = 1;
  }
  d.name = "New Talent"; d.rank = "Rank 1"; d.previous = 0;
  d.description = "Increases your chance to parry an attack by $s1%.";
  auto id = SpellService::save(d);
  _spells.forget(id);
  return id;
}
void TalentEditor::createTalent(int index, int row, int column, QString const& how, Talent const* source) {
  auto const& tr = tree(index);
  if (tr.talents.size() >= TalentRules::talentsPerTree) { status(QString("This tree is full: the game shows at most %1 talents.").arg(TalentRules::talentsPerTree), true); return; }
  if (tr.at(row, column)) { status("That slot is taken.", true); return; }
  QVector<Id> ranks;
  QString what;
  try {
    if (how == "spell") {
      auto picked = pickSpell(this, "Choose the spell for the talent's first rank");
      if (!picked) return;
      ranks = {*picked}; what = "Created a talent from " + spell(*picked).name;
    } else if (how == "clone") {
      if (!source) return;
      if (!databaseReady()) { status("Start the local server first: cloning copies the rank spells in the local database.", true); return; }
      // A clone gets its own copies of the rank spells: a spell can belong to one talent only.
      for (int i = 0; i < source->maxRank(); ++i) ranks << (source->ranks[i] ? copyRank(source->ranks[i], i, 0) : 0);
      what = "Cloned " + name(*source) + " (with your own copies of its rank spells)";
    } else {
      if (!databaseReady()) { status("Start the local server first: the new talent's spell goes in the local database.", true); return; }
      ranks = {newTalentSpell()}; what = "Created a new talent: name it, pick its icon, and open its spell to set what it does";
    }
  } catch (std::exception const& e) { status(QString("Cannot create the talent: %1").arg(e.what()), true); return; }
  auto talent = newTalent(row, column, ranks);
  if (source && how == "clone") talent.flags = source->flags;
  edit(index, [&](TalentTree& tree) { tree.talents << talent; }, what + ".");
  openTalent(index, talent.id);
}
void TalentEditor::changeIcon(int index, Id id) {
  auto const* t = tree(index).find(id); if (!t || t->ranks.isEmpty()) return;
  auto picked = pickSpellIcon(this, icon(*t));
  if (!picked) return;
  auto ranks = ownRanks(*t, true, "The icon belongs to the talent's rank spells.");
  if (ranks.isEmpty()) return;
  try {
    for (auto spellId : ranks) { if (!spellId) continue; auto d = SpellService::load(spellId); d.icon = *picked; SpellService::save(d); _spells.forget(spellId); }
  } catch (std::exception const& e) { status(QString("Cannot change the icon: %1").arg(e.what()), true); return; }
  if (ranks != t->ranks) edit(index, [&](TalentTree& tree) { tree.find(id)->ranks = ranks; }, "Icon changed (the ranks are your own spells now).");
  else { _rankSignature.clear(); refresh(); status("Icon changed."); }
}
void TalentEditor::renameTalent(int index, Id id, QString const& text) {
  auto const* t = tree(index).find(id); auto name = text.trimmed();
  if (!t || name.isEmpty() || name == this->name(*t)) return;
  try {
    for (auto spellId : t->ranks) {
      if (!spellId || !spell(spellId).own) continue;
      auto d = SpellService::load(spellId); d.name = name; SpellService::save(d); _spells.forget(spellId);
    }
  } catch (std::exception const& e) { status(QString("Cannot rename: %1").arg(e.what()), true); return; }
  _rankSignature.clear(); refresh(); status("Renamed to " + name + ".");
}
void TalentEditor::setRanks(int index, Id id, int count) {
  auto const* t = tree(index).find(id); if (!t) return;
  count = std::clamp(count, 1, TalentRules::maxRanks);
  if (count == t->maxRank()) return;
  auto ranks = t->ranks;
  if (count < ranks.size()) ranks.resize(count);
  else {
    if (!databaseReady()) { status("Start the local server first: new ranks are copies of the last rank's spell.", true); refreshPanel(); return; }
    try { while (ranks.size() < count) { Id last = ranks.isEmpty() ? 0 : ranks.back(); ranks << (last ? copyRank(last, int(ranks.size()), last) : 0); } }
    catch (std::exception const& e) { status(QString("Cannot create the next rank: %1").arg(e.what()), true); refreshPanel(); return; }
  }
  edit(index, [&](TalentTree& tree) {
    auto* x = tree.find(id); x->ranks = ranks; x->flags = ranks.size() == 1 ? 1 : 0;
    for (auto& other : tree.talents) if (other.prerequisite == id) other.prerequisiteRank = std::min(other.prerequisiteRank, int(ranks.size()));
  }, QString("%1 now has %2.").arg(name(*t), plural(count, "rank")));
}
void TalentEditor::openRank(int index, Id id, int rank) {
  auto const* t = tree(index).find(id); if (!t || rank >= t->maxRank() || !t->ranks[rank]) return;
  Id was = t->ranks[rank];
  auto saved = editSpell(this, was);
  _spells.forget(was); _rankSignature.clear();
  if (saved && *saved != was) {
    // Cloned in the Spell Editor: the copy becomes this rank.
    _spells.forget(*saved);
    edit(index, [&](TalentTree& tree) { tree.find(id)->ranks[rank] = *saved; }, QString("Rank %1 now uses your copy.").arg(rank + 1));
  } else refresh();
}
void TalentEditor::cloneRank(int index, Id id, int rank) {
  auto const* t = tree(index).find(id); if (!t) return;
  if (!databaseReady()) { status("Start the local server first.", true); return; }
  Id copy = 0;
  try { copy = rank > 0 && t->ranks[rank - 1] ? copyRank(t->ranks[rank - 1], rank, t->ranks[rank - 1]) : t->ranks[rank] ? copyRank(t->ranks[rank], rank, 0) : 0; }
  catch (std::exception const& e) { status(QString("Cannot copy the spell: %1").arg(e.what()), true); return; }
  if (!copy) return;
  edit(index, [&](TalentTree& tree) { tree.find(id)->ranks[rank] = copy; },
       rank > 0 ? QString("Rank %1 is now a copy of rank %2 with its values raised.").arg(rank + 1).arg(rank) : QString("Rank 1 is now your own copy."));
}
void TalentEditor::replaceRank(int index, Id id, int rank) {
  auto const* t = tree(index).find(id); if (!t) return;
  auto picked = pickSpell(this, QString("Spell for rank %1").arg(rank + 1), t->ranks[rank]);
  if (!picked) return;
  edit(index, [&](TalentTree& tree) { tree.find(id)->ranks[rank] = *picked; }, QString("Rank %1 now teaches %2.").arg(rank + 1).arg(spell(*picked).name));
}
void TalentEditor::cloneTree(int index) {
  auto const& source = tree(index);
  bool ok = false;
  auto name = QInputDialog::getText(this, "Clone Tree as Template", "Name of the new template:", QLineEdit::Normal, className(source.classMask) + " " + source.name + " experiment", &ok).trimmed();
  if (!ok || name.isEmpty()) return;
  for (auto const& t : _templates) if (t.key == name) { status("A template with that name exists already.", true); return; }
  auto copy = source; copy.tab = 0; copy.key = name; copy.name = name;
  try { _store->save(copy); } catch (std::exception const& e) { status(e.what(), true); return; }
  _templates = _store->templates();
  reload();
  if (int at = _class->findData("t:" + name); at >= 0) _class->setCurrentIndex(at);
  status("Template \"" + name + "\" created: edit and test it freely. Replace With Template puts it into a class tree.");
}
void TalentEditor::applyTemplate(int index, TalentTree const& source) {
  auto const& target = tree(index);
  if (QMessageBox::question(this, "Replace With Template", "Replace the talents of " + target.name + " with the template \"" + source.name + "\"?\n\n"
                            "Rank spells shared with another tree are reported as problems: use Make Ranks Editable on those talents to give them their own copies.") != QMessageBox::Yes) return;
  // New talent IDs, with the prerequisites following them.
  QHash<Id, Id> ids; Id next = nextId();
  for (auto const& t : source.talents) ids.insert(t.id, next++);
  QVector<Talent> talents;
  for (auto t : source.talents) { t.id = ids.value(t.id); t.prerequisite = ids.value(t.prerequisite); talents << t; }
  edit(index, [&](TalentTree& tree) { tree.talents = talents; }, target.name + " now has the template's talents.");
}
void TalentEditor::testInGame() {
  auto* tests = TestSessionService::instance();
  if (!tests || _visible.isEmpty() || !_visible[0]->tab) return;
  auto problems = _store->blockingProblems();
  if (!problems.isEmpty()) { QMessageBox::warning(this, "Test In Game", "Fix these first; the game cannot show or use the trees as they are:\n\n" + problems.mid(0, 10).join('\n')); return; }
  QDialog options(this); options.setWindowTitle("Test Talents In Game");
  auto layout = new QVBoxLayout(&options);
  auto text = new QLabel("The local server and test client get your edited talent trees, then WoW starts where your test character logged out."); text->setWordWrap(true); layout->addWidget(text);
  auto level = new QCheckBox(QString("Set my test character's level to %1 (the level shown here)").arg(_level)); level->setChecked(true); layout->addWidget(level);
  auto reset = new QCheckBox("Reset its " + className(_visible[0]->classMask) + " talents (points come back)"); reset->setChecked(true); layout->addWidget(reset);
  auto apply = new QCheckBox(QString("Then give it this build (%1)").arg(plural(spent(), "point"))); apply->setChecked(spent() > 0); apply->setEnabled(spent() > 0); layout->addWidget(apply);
  connect(reset, &QCheckBox::toggled, apply, [=, this](bool on) { apply->setEnabled(on && spent() > 0); if (!on) apply->setChecked(false); });
  auto note = new QLabel("The test character must be a " + className(_visible[0]->classMask) + ". Only your local character changes. The trees go to production only by hand "
                         "(Export Server Table, and Export Client Patch for players).");
  note->setWordWrap(true); note->setStyleSheet("color: gray;"); layout->addWidget(note);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); buttons->addButton("Test", QDialogButtonBox::AcceptRole); layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &options, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &options, &QDialog::reject);
  if (options.exec() != QDialog::Accepted) return;
  TestOptions test;
  if (level->isChecked()) test.level = _level;
  if (reset->isChecked()) {
    QSet<Id> all;
    for (auto const* list : {&_originals, &_trees}) for (auto const& tree : *list) if (tree.classMask == _visible[0]->classMask) for (auto const& t : tree.talents) for (auto id : t.ranks) if (id) all.insert(id);
    test.unlearn = QVector<Id>(all.begin(), all.end());
  }
  // The highest rank of each talent: the server counts a talent's points from the rank its spell teaches.
  if (apply->isChecked()) for (auto* tree : _visible) for (auto const& t : tree->talents) if (int r = _build.value(t.id); r > 0 && t.ranks[r - 1]) test.spells << t.ranks[r - 1];
  tests->testCharacter(this, test);
}
}
void openTalentEditor(QWidget* parent) {
  TalentEditor editor(parent);
  editor.exec();
}
}
