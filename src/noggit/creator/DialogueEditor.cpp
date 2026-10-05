#include "ServiceEditors.hpp"
#include "GossipService.hpp"
#include "CreatorPreviews.hpp"
#include "EditorWidgets.hpp"
#include "TestSessionService.hpp"
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/content/ContentSession.hpp>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QPointer>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
namespace Noggit::Creator {
namespace {
using Icon = Ui::FontAwesome::Icons;
using Action = DialogueResponse::Action;
using Kind = DialogueCondition::Kind;
using Effect = DialogueEffect::Kind;
using Ui::Content::EntryPicker;
// Tree items: the node, and the response within it (-1: the node itself, QuestList: the greeting's quests).
constexpr int NodeRole = Qt::UserRole, ResponseRole = Qt::UserRole + 1, NestedRole = Qt::UserRole + 2;
constexpr int QuestList = -2;
constexpr int NewNode = -2;
QPushButton* button(QString const& text, Icon icon, QWidget* parent) { return new QPushButton(Ui::FontAwesomeIcon(icon), text, parent); }
QLabel* hint(QString const& text, QWidget* parent) {
  auto label = new QLabel(text, parent); label->setWordWrap(true); label->setStyleSheet("color: palette(mid); font-style: italic;"); return label;
}
QLabel* heading(QString const& text, QWidget* parent) {
  auto label = new QLabel(text, parent); label->setStyleSheet("font-weight: bold; letter-spacing: 1px; color: palette(mid);"); return label;
}
QString elide(QString text, int length) { text = text.simplified(); return text.size() > length ? text.left(length - 1) + "…" : text; }
// The text as the game shows it, with placeholders for the player.
QString spoken(QString text) {
  text.replace(QRegularExpression("\\$[Gg]([^:;]*):([^;]*);"), "\\1");
  for (auto const& [token, shown] : std::initializer_list<std::pair<char const*, char const*>>{
         {"$B", "\n"}, {"$b", "\n"}, {"$N", "<name>"}, {"$n", "<name>"}, {"$C", "<class>"}, {"$c", "<class>"}, {"$R", "<race>"}, {"$r", "<race>"}})
    text.replace(token, shown);
  return text;
}
QString nodeName(int node) { return node == 0 ? "Greeting" : QString("Node %1").arg(node + 1); }
QString nodeTitle(Dialogue const& d, int node) {
  auto const& text = d.nodes[node].text;
  return nodeName(node) + (text.trimmed().isEmpty() ? QString() : " · “" + elide(text, 48) + "”");
}
Icon actionIcon(Action action) {
  switch (action) {
    case Action::Continue: return Icon::reply;
    case Action::Close: return Icon::signoutalt;
    case Action::Vendor: return Icon::store;
    case Action::Trainer: return Icon::graduationcap;
  }
  return Icon::reply;
}
QVector<QPair<Kind, QString>> const conditionKinds{
  {Kind::OnQuest, "Player is on quest"}, {Kind::NotOnQuest, "Player is not on quest"}, {Kind::QuestReady, "Player finished the objectives of"},
  {Kind::CompletedQuest, "Player has completed quest"}, {Kind::NotCompletedQuest, "Player has not completed quest"},
  {Kind::HasItem, "Player carries item"}, {Kind::LacksItem, "Player does not carry item"},
  {Kind::MinLevel, "Player is at least level"}, {Kind::MaxLevel, "Player is at most level"}};
// What the editor cannot do without new server scripting, listed so designers know it is not missing by accident.
QStringList const needsScripting{"Start an escort or make the NPC follow", "Run custom logic (C++ or AI scripts)", "Play a cinematic sequence"};
bool hasErrors(QVector<DialogueProblem> const& problems) {
  return std::any_of(problems.begin(), problems.end(), [](DialogueProblem const& p) { return p.error; });
}
// The game's gossip window: what the NPC says, the player's responses, and the greeting's quests. Clicking a
// response plays it, so designers can walk every branch before testing in game.
class GossipWindow final : public QFrame {
public:
  explicit GossipWindow(QWidget* parent) : QFrame(parent) {
    setObjectName("Gossip");
    setStyleSheet("#Gossip { background: qlineargradient(y1:0, y2:1, stop:0 #f3e8cc, stop:1 #d6c196); border: 2px solid #8a6d2f; border-radius: 6px; }"
                  "QLabel { color: #2b1d0e; background: transparent; }"
                  "#GossipTitle { color: #ffd100; background: #3b2a14; border-radius: 3px; padding: 3px; font-weight: bold; }"
                  "QPushButton#GossipOption { text-align: left; border: none; padding: 3px 4px; color: #1d1205; background: transparent; }"
                  "QPushButton#GossipOption:hover { background: rgba(255,255,255,0.5); }"
                  "QPushButton#GossipSelected { text-align: left; border: none; padding: 3px 4px; color: #1d1205; background: rgba(255,209,0,0.45); }"
                  "#GossipNote { color: #6b5536; font-size: 8pt; font-style: italic; }"
                  "#GossipQuest { color: #1d1205; padding: 3px 4px; }"
                  "QToolButton { color: #2b1d0e; }");
    auto layout = new QVBoxLayout(this); layout->setContentsMargins(10, 8, 10, 8);
    _title = new QLabel(this); _title->setObjectName("GossipTitle"); _title->setAlignment(Qt::AlignCenter); layout->addWidget(_title);
    _text = new QLabel(this); _text->setWordWrap(true); _text->setTextFormat(Qt::PlainText); _text->setMinimumHeight(70);
    _text->setAlignment(Qt::AlignTop | Qt::AlignLeft); layout->addWidget(_text);
    _options = new QVBoxLayout; _options->setSpacing(1); layout->addLayout(_options);
    layout->addStretch();
    _event = new QLabel(this); _event->setObjectName("GossipNote"); _event->setWordWrap(true); layout->addWidget(_event);
    auto nav = new QHBoxLayout; layout->addLayout(nav);
    _back = new QToolButton(this); _back->setText("◀ Back"); _back->setAutoRaise(true);
    auto restart = new QToolButton(this); restart->setText("Start over"); restart->setAutoRaise(true);
    nav->addWidget(_back); nav->addStretch(); nav->addWidget(restart);
    connect(_back, &QToolButton::clicked, this, [this] {
      if (_history.isEmpty()) return;
      _node = _history.takeLast(); _ended = false; _event->clear(); draw(); if (onShown) onShown(_node);
    });
    connect(restart, &QToolButton::clicked, this, [this] { _history.clear(); _node = 0; _ended = false; _event->clear(); draw(); if (onShown) onShown(0); });
  }
  std::function<QString(DialogueCondition const&)> describe;
  std::function<QString(DialogueEffect const&)> effectText;
  std::function<void(int)> onShown; // the preview moved to a node
  void setDialogue(Dialogue const* d) { _d = d; if (!_d || _node >= _d->nodes.size()) { _node = 0; _history.clear(); } draw(); }
  void showNode(int node, int highlight) {
    if (node != _node) { _ended = false; _event->clear(); }
    _node = std::max(0, node); _highlight = highlight; draw();
  }
private:
  void choose(int j) {
    auto const& r = _d->nodes[_node].responses[j];
    QStringList happened;
    for (auto const& e : r.effects) if (effectText) happened << effectText(e);
    switch (r.action) {
      case Action::Continue:
        if (r.target >= 0 && r.target < _d->nodes.size()) { _history << _node; _node = r.target; _highlight = -1; }
        break;
      case Action::Close: _ended = true; happened << "The conversation ends."; break;
      case Action::Vendor: _ended = true; happened << (_d->vendor ? "The shop window opens." : "Players do not see this response: the NPC is not a vendor."); break;
      case Action::Trainer: _ended = true; happened << (_d->trainer ? "The trainer window opens." : "Players do not see this response: the NPC is not a trainer."); break;
    }
    _event->setText(happened.join('\n'));
    // Deferred: this runs inside the clicked button, which draw() replaces.
    QTimer::singleShot(0, this, [this, action = r.action] { draw(); if (action == Action::Continue && onShown) onShown(_node); });
  }
  void note(QString const& text) {
    auto label = new QLabel(text, this); label->setObjectName("GossipNote"); label->setWordWrap(true); label->setIndent(22); _options->addWidget(label);
  }
  void draw() {
    while (auto item = _options->takeAt(0)) { if (auto w = item->widget()) { w->hide(); w->deleteLater(); } delete item; }
    _back->setEnabled(!_history.isEmpty());
    _title->setText(_d ? _d->name : QString());
    if (!_d || _d->nodes.isEmpty()) { _text->setText("This NPC has no dialogue yet: players see the game's default greeting."); return; }
    if (_ended) { _text->setText("(The window is closed. Start over to talk again.)"); return; }
    auto const& node = _d->nodes[std::min<int>(_node, _d->nodes.size() - 1)];
    _text->setText(node.text.trimmed().isEmpty() ? "(What the NPC says goes here.)" : spoken(node.text));
    for (int j = 0; j < node.responses.size(); ++j) {
      auto const& r = node.responses[j];
      auto option = new QPushButton(Ui::FontAwesomeIcon(actionIcon(r.action)), r.text.trimmed().isEmpty() ? "(the player's response)" : elide(r.text, 90), this);
      option->setObjectName(j == _highlight ? "GossipSelected" : "GossipOption");
      option->setCursor(Qt::PointingHandCursor); option->setFlat(true);
      connect(option, &QPushButton::clicked, this, [this, j] { choose(j); });
      _options->addWidget(option);
      if (!r.conditions.isEmpty() && describe) {
        QStringList rules; for (auto const& c : r.conditions) rules << describe(c);
        note("Only if: " + rules.join("; "));
      }
    }
    if (_node == 0 && _d->listQuests) {
      for (auto const& q : _d->quests) {
        auto quest = new QLabel(this); quest->setObjectName("GossipQuest");
        quest->setText("<span style='color:#c99a00;font-weight:bold'>!</span>&nbsp;&nbsp;" + q.name.toHtmlEscaped());
        _options->addWidget(quest);
      }
      if (_d->quests.isEmpty()) note("(Quests this NPC gives are listed here. It gives none yet.)");
      else note("Players see only the quests they can take now.");
    }
    if (_node == 0 && node.responses.isEmpty() && !_d->listQuests)
      note("Without responses, the game shows its usual options here: the NPC's quests, “I want to browse your goods.”, “Train me.”.");
  }
  Dialogue const* _d = nullptr;
  int _node = 0, _highlight = -1;
  bool _ended = false;
  QVector<int> _history;
  QLabel *_title, *_text, *_event;
  QVBoxLayout* _options;
  QToolButton* _back;
};
class DialogueEditor final : public QDialog {
public:
  DialogueEditor(QWidget* parent, World* world, Id entry, Ui::Content::ContentSession& session) : QDialog(parent), _entry(entry), _session(session) {
    setWindowTitle("Dialogue"); resize(1440, 840);
    auto layout = new QVBoxLayout(this);
    _title = editorTitle({}, this); layout->addWidget(_title);
    _notice = noticeBanner({}, this); layout->addWidget(_notice);
    _dropped = noticeBanner({}, this); layout->addWidget(_dropped);
    auto split = new QSplitter(this); layout->addWidget(split, 1);

    auto left = new QWidget; auto leftLayout = new QVBoxLayout(left); leftLayout->setContentsMargins(0, 0, 0, 0);
    auto preview = new NpcPreview(world, left); preview->setFixedHeight(220); preview->showNpc(entry); leftLayout->addWidget(preview);
    leftLayout->addWidget(heading("IN GAME — CLICK A RESPONSE TO PLAY IT", left));
    _window = new GossipWindow(left); leftLayout->addWidget(_window, 1);
    split->addWidget(left);

    auto center = new QWidget; auto centerLayout = new QVBoxLayout(center); centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->addWidget(heading("CONVERSATION", center));
    _tree = new QTreeWidget(center); _tree->setHeaderHidden(true); _tree->setIndentation(22); _tree->setIconSize(QSize(14, 14));
    _tree->setStyleSheet("QTreeWidget::item { padding: 3px 0; }");
    centerLayout->addWidget(_tree, 1);
    _listProblems = new QLabel(center); _listProblems->setWordWrap(true); centerLayout->addWidget(_listProblems);
    split->addWidget(center);

    _inspector = new QScrollArea; _inspector->setWidgetResizable(true); _inspector->setFrameShape(QFrame::NoFrame);
    split->addWidget(_inspector);
    split->setSizes({330, 560, 520});

    auto bottom = new QHBoxLayout; layout->addLayout(bottom);
    _copyFrom = button("Copy from NPC…", Icon::paste, this); _copyTo = button("Copy to My NPC…", Icon::copy, this);
    _clear = button("Clear", Icon::eraser, this); _reset = button("Reset", Icon::undo, this);
    auto test = button("Test Dialogue…", Icon::play, this);
    for (auto b : {_copyFrom, _copyTo, _clear, _reset, test}) bottom->addWidget(b);
    bottom->addStretch();
    auto close = new QPushButton("Close", this); _save = new QPushButton("Save", this); _save->setDefault(true);
    bottom->addWidget(close); bottom->addWidget(_save);

    _factsTimer.setSingleShot(true); _factsTimer.setInterval(350);
    connect(&_factsTimer, &QTimer::timeout, this, [this] { refreshFacts(); recheck(); });
    _window->describe = [this](DialogueCondition const& c) { return describe(c); };
    _window->effectText = [this](DialogueEffect const& e) { return effectText(e); };
    _window->onShown = [this](int node) { selectItem(node, -1); };
    connect(_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
      if (_building || !item) return;
      select(item->data(0, NodeRole).toInt(), item->data(0, ResponseRole).toInt());
    });
    connect(_copyFrom, &QPushButton::clicked, this, [this] { copyFrom(); });
    connect(_copyTo, &QPushButton::clicked, this, [this] { copyTo(); });
    connect(_clear, &QPushButton::clicked, this, [this] {
      if (QMessageBox::question(this, "Clear", "Remove the whole dialogue? Players then see the game's default greeting. Nothing is saved until you click Save.") != QMessageBox::Yes) return;
      _d.nodes.clear(); _d.listQuests = false; _node = -1; _response = -1; changed(true); buildInspector();
    });
    connect(_reset, &QPushButton::clicked, this, [this] {
      if (_dirty && QMessageBox::question(this, "Reset", "Discard your changes and reload the saved dialogue?") != QMessageBox::Yes) return;
      load();
    });
    connect(test, &QPushButton::clicked, this, [this] { testDialogue(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(_save, &QPushButton::clicked, this, [this] { save(); });
    load();
  }
  bool saved = false;
  void reject() override { if (confirmClose(this, _dirty, [this] { return save(); })) QDialog::reject(); }
private:
  // --- state ---
  void load() {
    try { _d = GossipService::load(_entry); } catch (std::exception const& e) { QMessageBox::warning(this, "Dialogue", e.what()); return; }
    _title->setText((_d.name + " — Dialogue").toUpper());
    _notice->setText(_d.notice); _notice->setVisible(!_d.notice.isEmpty());
    if (_d.dropped.isEmpty()) _dropped->hide();
    else {
      _dropped->setText("The editor cannot show everything this dialogue does"
                        + QString(!_d.editable ? " (left out when you copy it):" : _d.shared ? " (left out of this NPC's copy):" : ":")
                        + "\n• " + _d.dropped.join("\n• "));
      _dropped->show();
    }
    for (auto w : std::initializer_list<QWidget*>{_copyFrom, _clear, _reset, _save}) w->setVisible(_d.editable);
    _copyTo->setVisible(!_d.editable);
    refreshFacts();
    _dirty = false;
    if (_node >= _d.nodes.size()) { _node = _d.nodes.isEmpty() ? -1 : 0; _response = -1; }
    if (_node < 0 && !_d.nodes.isEmpty()) _node = 0;
    if (_node >= 0 && _response >= _d.nodes[_node].responses.size()) _response = -1;
    recheck(); rebuildTree(); buildInspector();
  }
  void refreshFacts() { try { _facts = GossipService::facts(_d); } catch (std::exception const&) { /* keeps the last facts */ } }
  // After an edit. `structure`: nodes or responses were added, removed, moved or relinked.
  void changed(bool structure, bool ids = false) {
    _dirty = true;
    if (ids) _factsTimer.start();
    recheck();
    if (structure) rebuildTree(); else updateTree();
  }
  void recheck() {
    _problems = GossipService::check(_d, _facts);
    auto global = QStringList{};
    int errors = 0, warnings = 0;
    for (auto const& p : _problems) {
      (p.error ? errors : warnings)++;
      if (p.node < 0) global << (p.error ? "✗ " : "⚠ ") + p.text;
    }
    if (errors + warnings) global << QString("%1 to fix, %2 to look at (marked in the tree).").arg(errors).arg(warnings);
    _listProblems->setText(global.join('\n')); _listProblems->setVisible(!global.isEmpty());
    _listProblems->setStyleSheet(errors ? "color: #e04040;" : "color: #d08a00;");
    _save->setEnabled(_d.editable && !hasErrors(_problems));
    showPageProblems();
    _window->setDialogue(&_d);
    _window->showNode(_node, _response >= 0 ? _response : -1);
  }
  QVector<DialogueProblem> problemsOf(int node, int response) const {
    QVector<DialogueProblem> result;
    for (auto const& p : _problems) if (p.node == node && p.response == response) result << p;
    return result;
  }
  // --- names ---
  QString name(QString const& kind, Id id) const {
    if (!id) return "(choose one)";
    auto& lookups = _session.lookups();
    auto label = kind == "quest" ? lookups.questName(id) : kind == "spell" ? lookups.spellName(id) : lookups.itemName(id);
    return label.isEmpty() ? QString("#%1").arg(id) : label;
  }
  QString describe(DialogueCondition const& c) const { return GossipService::describe(c, [this](QString const& kind, Id id) { return name(kind, id); }); }
  QString effectText(DialogueEffect const& e) const {
    switch (e.kind) {
      case Effect::CastSpell: return _d.name + " casts " + name("spell", e.id) + " on you.";
      case Effect::Teleport: return QString("You are teleported (map %1: %2, %3, %4).").arg(e.position.map).arg(e.position.x, 0, 'f', 1).arg(e.position.y, 0, 'f', 1).arg(e.position.z, 0, 'f', 1);
      case Effect::CompleteQuest: return "You get credit for " + name("quest", e.id) + " (if you are on it).";
    }
    return {};
  }
  QString responseLine(int node, int response, bool nested) const {
    auto const& r = _d.nodes[node].responses[response];
    QString target;
    switch (r.action) {
      case Action::Continue:
        target = r.target < 0 || r.target >= _d.nodes.size() ? "choose a node" : nested ? nodeName(r.target) : "back to " + nodeName(r.target);
        break;
      case Action::Close: target = "End conversation"; break;
      case Action::Vendor: target = "Open vendor"; break;
      case Action::Trainer: target = "Open trainer"; break;
    }
    QStringList extras;
    for (auto const& e : r.effects) extras << (e.kind == Effect::CastSpell ? "cast " + name("spell", e.id) : e.kind == Effect::Teleport ? QString("teleport") : "complete " + name("quest", e.id));
    if (!r.conditions.isEmpty()) extras << QString("if %1 %2").arg(r.conditions.size()).arg(r.conditions.size() == 1 ? "condition" : "conditions");
    return "“" + (r.text.trimmed().isEmpty() ? QString("…") : elide(r.text, 50)) + "”  →  " + target + (extras.isEmpty() ? QString() : "   · " + extras.join(" · "));
  }
  // --- tree ---
  void decorate(QTreeWidgetItem* item) {
    int node = item->data(0, NodeRole).toInt(), response = item->data(0, ResponseRole).toInt();
    if (node < 0 || node >= _d.nodes.size()) return;
    QVector<DialogueProblem> problems;
    if (response == QuestList) {
      QStringList names; for (auto const& q : _d.quests) names << q.name;
      item->setText(0, "Lists quests: " + (names.isEmpty() ? QString("(this NPC gives none yet)") : names.join(", ")));
      item->setIcon(0, Ui::FontAwesomeIcon(Icon::exclamation));
    } else if (response < 0) {
      item->setText(0, nodeTitle(_d, node));
      item->setIcon(0, Ui::FontAwesomeIcon(Icon::comment));
      auto font = item->font(0); font.setBold(true); item->setFont(0, font);
      problems = problemsOf(node, -1);
    } else if (response < _d.nodes[node].responses.size()) {
      item->setText(0, responseLine(node, response, item->data(0, NestedRole).toBool()));
      item->setIcon(0, Ui::FontAwesomeIcon(actionIcon(_d.nodes[node].responses[response].action)));
      problems = problemsOf(node, response);
    }
    QStringList tips; bool error = false;
    for (auto const& p : problems) { tips << (p.error ? "✗ " : "⚠ ") + p.text; error = error || p.error; }
    item->setToolTip(0, tips.join('\n'));
    item->setForeground(0, problems.isEmpty() ? palette().text() : QBrush(QColor(error ? "#e04040" : "#d08a00")));
  }
  void updateTree() {
    for (QTreeWidgetItemIterator it(_tree); *it; ++it) decorate(*it);
  }
  // Each node appears once, under the first response that leads to it (as the conversation unfolds);
  // other responses to it say "back to". Nodes nothing leads to are listed last, marked.
  void rebuildTree() {
    _building = true;
    _tree->clear();
    QVector<bool> placed(_d.nodes.size());
    auto item = [&](QTreeWidgetItem* parent, int node, int response, bool nested = false) {
      auto i = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(_tree);
      i->setData(0, NodeRole, node); i->setData(0, ResponseRole, response); i->setData(0, NestedRole, nested);
      decorate(i); return i;
    };
    std::function<void(QTreeWidgetItem*, int)> add = [&](QTreeWidgetItem* parent, int node) {
      placed[node] = true;
      auto n = item(parent, node, -1);
      for (int j = 0; j < _d.nodes[node].responses.size(); ++j) {
        auto const& r = _d.nodes[node].responses[j];
        bool nested = r.action == Action::Continue && r.target >= 0 && r.target < _d.nodes.size() && !placed[r.target];
        auto child = item(n, node, j, nested);
        if (nested) add(child, r.target);
      }
      if (node == 0 && _d.listQuests) item(n, 0, QuestList);
    };
    if (!_d.nodes.isEmpty()) add(nullptr, 0);
    for (int i = 0; i < _d.nodes.size(); ++i) if (!placed[i]) add(nullptr, i);
    _tree->expandAll();
    QTreeWidgetItem* current = nullptr;
    for (QTreeWidgetItemIterator it(_tree); *it && !current; ++it)
      if ((*it)->data(0, NodeRole).toInt() == _node && (*it)->data(0, ResponseRole).toInt() == _response) current = *it;
    if (current) _tree->setCurrentItem(current);
    _building = false;
  }
  void selectItem(int node, int response) {
    for (QTreeWidgetItemIterator it(_tree); *it; ++it)
      if ((*it)->data(0, NodeRole).toInt() == node && (*it)->data(0, ResponseRole).toInt() == response) { _tree->setCurrentItem(*it); _tree->scrollToItem(*it); return; }
  }
  void select(int node, int response) {
    _node = node; _response = response;
    buildInspector();
    _window->showNode(node, response >= 0 ? response : -1);
  }
  // Selects after the tree is rebuilt; deferred so the widget that asked can finish its signal first.
  void reselect(int node, int response) {
    QTimer::singleShot(0, this, [this, node, response] { _node = node; _response = response; rebuildTree(); buildInspector(); _window->showNode(node, response >= 0 ? response : -1); });
  }
  int addNode() { _d.nodes.push_back({}); return _d.nodes.size() - 1; }
  void deleteNode(int index) {
    for (auto& node : _d.nodes) for (auto& r : node.responses) {
      if (r.action != Action::Continue) continue;
      if (r.target == index) { r.action = Action::Close; r.target = -1; }
      else if (r.target > index) --r.target;
    }
    _d.nodes.removeAt(index);
    changed(true, true); reselect(0, -1);
  }
  // --- inspector ---
  void buildInspector() {
    _pageProblems = nullptr;
    auto panel = new QWidget; auto v = new QVBoxLayout(panel);
    if (_d.nodes.isEmpty()) emptyPage(v);
    else if (_node >= 0 && _node < _d.nodes.size()) {
      if (_response == QuestList) questPage(v);
      else if (_response >= 0 && _response < _d.nodes[_node].responses.size()) responsePage(v, _node, _response);
      else nodePage(v, _node);
    }
    v->addStretch();
    if (!_d.editable) for (auto* w : panel->findChildren<QWidget*>()) if (!qobject_cast<QLabel*>(w) && !qobject_cast<QGroupBox*>(w)) w->setEnabled(false);
    _inspector->setWidget(panel); // deletes the previous page
  }
  // The selected node's or response's problems, kept current as the designer types.
  void problemsFor(QVBoxLayout* v, int, int) {
    _pageProblems = new QLabel; _pageProblems->setWordWrap(true); _pageProblems->setTextFormat(Qt::RichText); v->addWidget(_pageProblems);
    showPageProblems();
  }
  void showPageProblems() {
    if (!_pageProblems) return;
    QStringList lines;
    for (auto const& p : problemsOf(_node, _response))
      lines << QString("<span style='color:%1'>%2 %3</span>").arg(p.error ? "#e04040" : "#d08a00", p.error ? "✗" : "⚠", p.text.toHtmlEscaped());
    _pageProblems->setText(lines.join("<br>")); _pageProblems->setVisible(!lines.isEmpty());
  }
  void emptyPage(QVBoxLayout* v) {
    v->addWidget(heading("NO DIALOGUE YET", nullptr));
    v->addWidget(hint("Players who talk to " + _d.name + " see the game's default greeting. Start a conversation tree, or copy another NPC's.", nullptr));
    if (!_d.editable) return;
    auto start = button("Start a Dialogue", Icon::comments, nullptr), copy = button("Copy from Another NPC…", Icon::paste, nullptr);
    v->addWidget(start); v->addWidget(copy);
    connect(start, &QPushButton::clicked, this, [this] {
      _d.nodes = {DialogueNode{}}; _d.listQuests = !_d.quests.isEmpty();
      changed(true); reselect(0, -1);
    });
    connect(copy, &QPushButton::clicked, this, [this] { QTimer::singleShot(0, this, [this] { copyFrom(); }); });
  }
  void questsHint(QVBoxLayout* v) {
    QStringList names; for (auto const& q : _d.quests) names << "“" + q.name + "”";
    v->addWidget(hint(names.isEmpty() ? "It gives no quests yet: make it a giver on a quest's Givers page (Quests in the NPC card)."
                                      : "Players see the quests they can take here, by title: " + names.join(", ") + ".", nullptr));
    v->addWidget(hint("The game lists an NPC's quests in its greeting only. Offering a quest from a later node needs scripting support.", nullptr));
  }
  void questPage(QVBoxLayout* v) {
    v->addWidget(heading("QUESTS IN THE GREETING", nullptr));
    questsHint(v);
    auto off = button("Stop Listing Quests", Icon::times, nullptr); v->addWidget(off);
    connect(off, &QPushButton::clicked, this, [this] { _d.listQuests = false; changed(true); reselect(0, -1); });
  }
  void nodePage(QVBoxLayout* v, int i) {
    v->addWidget(heading(i == 0 ? "GREETING — WHAT PLAYERS SEE FIRST" : nodeName(i).toUpper(), nullptr));
    problemsFor(v, i, -1);
    v->addWidget(new QLabel("<b>The NPC says</b>"));
    auto text = new QPlainTextEdit(_d.nodes[i].text); text->setTabChangesFocus(true); text->setMinimumHeight(130);
    text->setPlaceholderText(i == 0 ? "Something has disturbed the dead, $N." : "What the NPC answers.");
    v->addWidget(text);
    connect(text, &QPlainTextEdit::textChanged, this, [this, i, text] { _d.nodes[i].text = text->toPlainText(); changed(false); });
    v->addWidget(hint("$N the player's name · $C class · $R race · $B line break", nullptr));
    if (i == 0) {
      auto quests = new QCheckBox("List this NPC's quests here"); quests->setChecked(_d.listQuests); v->addWidget(quests);
      connect(quests, &QCheckBox::toggled, this, [this](bool on) { _d.listQuests = on; changed(true); });
      questsHint(v);
    }
    auto row = new QHBoxLayout; v->addLayout(row);
    auto add = button("Add Response", Icon::plus, nullptr); row->addWidget(add);
    connect(add, &QPushButton::clicked, this, [this, i] {
      _d.nodes[i].responses.push_back({}); changed(true); reselect(i, _d.nodes[i].responses.size() - 1);
    });
    if (i > 0) {
      auto remove = button("Delete Node", Icon::trash, nullptr); row->addWidget(remove);
      connect(remove, &QPushButton::clicked, this, [this, i] {
        if (QMessageBox::question(this, "Delete Node", "Delete " + nodeName(i) + " and its responses? Responses that lead here end the conversation instead.") == QMessageBox::Yes)
          QTimer::singleShot(0, this, [this, i] { deleteNode(i); });
      });
    }
    row->addStretch();
  }
  void responsePage(QVBoxLayout* v, int i, int j) {
    auto& r = _d.nodes[i].responses[j];
    v->addWidget(heading("PLAYER RESPONSE — IN " + nodeName(i).toUpper(), nullptr));
    problemsFor(v, i, j);
    v->addWidget(new QLabel("<b>The player says</b>"));
    auto text = new QLineEdit(r.text); text->setMaxLength(GossipService::maxResponseText); text->setPlaceholderText("What happened?");
    v->addWidget(text);
    connect(text, &QLineEdit::textEdited, this, [this, i, j](QString const& value) { _d.nodes[i].responses[j].text = value; changed(false); });

    v->addWidget(new QLabel("<b>Then</b>"));
    auto then = new QComboBox; auto model = new QStandardItemModel(then);
    auto entry = [&](QString const& label, int code, Icon icon, bool enabled = true) {
      auto item = new QStandardItem(Ui::FontAwesomeIcon(icon), label); item->setData(code); item->setEnabled(enabled); model->appendRow(item);
    };
    entry("Continue the conversation", int(Action::Continue), Icon::reply);
    entry("End the conversation", int(Action::Close), Icon::signoutalt);
    entry("Open the shop (vendor)", int(Action::Vendor), Icon::store);
    entry("Open training (trainer)", int(Action::Trainer), Icon::graduationcap);
    entry("Offer a quest here — needs scripting support", -1, Icon::lock, false);
    for (auto const& what : needsScripting) entry(what + " — needs scripting support", -1, Icon::lock, false);
    then->setModel(model);
    then->setCurrentIndex(int(r.action));
    v->addWidget(then);
    connect(then, qOverload<int>(&QComboBox::activated), this, [this, i, j, then](int index) {
      int code = then->itemData(index, Qt::UserRole + 1).toInt();
      if (code < 0) return;
      auto& r = _d.nodes[i].responses[j];
      r.action = Action(code);
      // A new branch gets its own node right away, so the designer can write what comes next.
      if (r.action == Action::Continue && (r.target < 0 || r.target >= _d.nodes.size())) r.target = addNode();
      changed(true); reselect(i, j);
    });
    if (r.action == Action::Continue) {
      auto row = new QHBoxLayout; v->addLayout(row);
      row->addWidget(new QLabel("Leads to"));
      auto target = new QComboBox;
      for (int k = 0; k < _d.nodes.size(); ++k) target->addItem(Ui::FontAwesomeIcon(Icon::comment), nodeTitle(_d, k), k);
      target->addItem(Ui::FontAwesomeIcon(Icon::plus), "New node", NewNode);
      target->setCurrentIndex(std::max(0, target->findData(r.target)));
      row->addWidget(target, 1);
      connect(target, qOverload<int>(&QComboBox::activated), this, [this, i, j, target](int index) {
        int chosen = target->itemData(index).toInt();
        _d.nodes[i].responses[j].target = chosen == NewNode ? addNode() : chosen;
        changed(true); reselect(i, j);
      });
      v->addWidget(hint("Responses may also lead back to an earlier node, such as the greeting.", nullptr));
    } else if (r.action == Action::Vendor || r.action == Action::Trainer) {
      bool has = r.action == Action::Vendor ? _d.vendor : _d.trainer;
      v->addWidget(hint(QString(has ? "Opens the %1 window, as set up in %2." : "Players only see this once the NPC is a %1: set it up in %2 (NPC card).")
                          .arg(r.action == Action::Vendor ? "shop" : "trainer", r.action == Action::Vendor ? "Vendor" : "Trainer"), nullptr));
    }
    effectsGroup(v, i, j);
    conditionsGroup(v, i, j);
    auto row = new QHBoxLayout; v->addLayout(row);
    auto up = button("Move Up", Icon::arrowup, nullptr), down = button("Move Down", Icon::arrowdown, nullptr), remove = button("Delete Response", Icon::trash, nullptr);
    up->setEnabled(j > 0); down->setEnabled(j + 1 < _d.nodes[i].responses.size());
    for (auto b : {up, down, remove}) row->addWidget(b);
    row->addStretch();
    connect(up, &QPushButton::clicked, this, [this, i, j] { _d.nodes[i].responses.swapItemsAt(j, j - 1); changed(true); reselect(i, j - 1); });
    connect(down, &QPushButton::clicked, this, [this, i, j] { _d.nodes[i].responses.swapItemsAt(j, j + 1); changed(true); reselect(i, j + 1); });
    connect(remove, &QPushButton::clicked, this, [this, i, j] { _d.nodes[i].responses.removeAt(j); changed(true, true); reselect(i, -1); });
  }
  void effectsGroup(QVBoxLayout* v, int i, int j) {
    auto& r = _d.nodes[i].responses[j];
    bool talking = r.action == Action::Continue || r.action == Action::Close;
    if (!talking && r.effects.isEmpty()) return;
    auto group = new QGroupBox("When chosen"); auto g = new QVBoxLayout(group); v->addWidget(group);
    for (int k = 0; k < r.effects.size(); ++k) g->addLayout(effectRow(i, j, k));
    if (r.effects.isEmpty()) g->addWidget(hint("Nothing else happens. Effects run as the response is chosen.", group));
    auto add = new QToolButton(group); add->setText("Add Effect"); add->setIcon(Ui::FontAwesomeIcon(Icon::plus));
    add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); add->setPopupMode(QToolButton::InstantPopup);
    add->setEnabled(talking && r.effects.size() < GossipService::maxEffects);
    auto menu = new QMenu(add);
    auto effect = [&](QString const& label, Effect kind, Icon icon) {
      menu->addAction(Ui::FontAwesomeIcon(icon), label, this, [this, i, j, kind] {
        DialogueEffect e; e.kind = kind;
        if (kind == Effect::Teleport && _session.cursor_position) if (auto p = _session.cursor_position()) e.position = {p->map, p->x, p->y, p->z, p->orientation};
        _d.nodes[i].responses[j].effects.push_back(e); changed(true, true); reselect(i, j);
      });
    };
    effect("Cast a spell on the player", Effect::CastSpell, Icon::magic);
    effect("Teleport the player", Effect::Teleport, Icon::mapmarkeralt);
    effect("Complete a quest (talk or event quests)", Effect::CompleteQuest, Icon::check);
    menu->addSeparator();
    for (auto const& what : needsScripting) menu->addAction(Ui::FontAwesomeIcon(Icon::lock), what + " — needs scripting support")->setEnabled(false);
    add->setMenu(menu);
    g->addWidget(add, 0, Qt::AlignLeft);
  }
  QLayout* effectRow(int i, int j, int k) {
    auto& e = _d.nodes[i].responses[j].effects[k];
    auto column = new QVBoxLayout; auto row = new QHBoxLayout; column->addLayout(row);
    auto ref = [this, i, j, k]() -> DialogueEffect& { return _d.nodes[i].responses[j].effects[k]; };
    auto picker = [&](Ui::Content::LookupModel* model) {
      auto p = new EntryPicker(model); p->setEntry(e.id);
      connect(p, &QComboBox::currentTextChanged, this, [this, p, ref] { ref().id = p->entry(); changed(false, true); });
      return p;
    };
    if (e.kind == Effect::CastSpell) {
      row->addWidget(new QLabel("Cast")); row->addWidget(picker(_session.lookups().spells.get()), 1);
      column->addWidget(hint(_d.name + " casts it on the player, e.g. a blessing.", nullptr));
    } else if (e.kind == Effect::CompleteQuest) {
      row->addWidget(new QLabel("Complete")); row->addWidget(picker(_session.lookups().quests.get()), 1);
      column->addWidget(hint("Gives credit for the quest's talk/event objective. Your own quests get that objective when you save.", nullptr));
    } else {
      row->addWidget(new QLabel("Teleport to"));
      auto map = new QSpinBox; map->setRange(0, 9999); map->setPrefix("map "); map->setValue(e.position.map); row->addWidget(map);
      QVector<QDoubleSpinBox*> axes;
      for (auto const& [label, value] : std::initializer_list<std::pair<char const*, float>>{
             {"x", e.position.x}, {"y", e.position.y}, {"z", e.position.z}, {"facing", e.position.orientation}}) {
        auto spin = new QDoubleSpinBox; spin->setRange(-20000, 20000); spin->setDecimals(2); spin->setPrefix(QString(label) + " "); spin->setValue(value);
        row->addWidget(spin); axes << spin;
      }
      auto apply = [this, ref, map, axes] {
        ref().position = {unsigned(map->value()), float(axes[0]->value()), float(axes[1]->value()), float(axes[2]->value()), float(axes[3]->value())};
        changed(false);
      };
      connect(map, qOverload<int>(&QSpinBox::valueChanged), this, apply);
      for (auto spin : axes) connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, apply);
      auto here = button("Use my cursor position", Icon::crosshairs, nullptr); here->setEnabled(bool(_session.cursor_position));
      here->setToolTip("Point at the spot in the 3D view, then click.");
      column->addWidget(here, 0, Qt::AlignLeft);
      connect(here, &QPushButton::clicked, this, [this, map, axes] {
        auto p = _session.cursor_position ? _session.cursor_position() : std::nullopt;
        if (!p) return;
        map->setValue(p->map); axes[0]->setValue(p->x); axes[1]->setValue(p->y); axes[2]->setValue(p->z); axes[3]->setValue(p->orientation);
      });
    }
    auto remove = new QToolButton; remove->setIcon(Ui::FontAwesomeIcon(Icon::trash)); remove->setAutoRaise(true); remove->setToolTip("Remove this effect");
    row->addWidget(remove);
    connect(remove, &QToolButton::clicked, this, [this, i, j, k] { _d.nodes[i].responses[j].effects.removeAt(k); changed(true, true); reselect(i, j); });
    return column;
  }
  void conditionsGroup(QVBoxLayout* v, int i, int j) {
    auto& r = _d.nodes[i].responses[j];
    auto group = new QGroupBox("Only shown when"); auto g = new QVBoxLayout(group); v->addWidget(group);
    g->addWidget(hint(r.conditions.isEmpty() ? "Everyone sees this response." : "All of these must be true. Players who do not meet them never see this response.", group));
    for (int k = 0; k < r.conditions.size(); ++k) g->addLayout(conditionRow(i, j, k));
    auto add = button("Add Condition", Icon::filter, group); add->setEnabled(r.conditions.size() < GossipService::maxConditions);
    g->addWidget(add, 0, Qt::AlignLeft);
    connect(add, &QPushButton::clicked, this, [this, i, j] { _d.nodes[i].responses[j].conditions.push_back({}); changed(true, true); reselect(i, j); });
  }
  QLayout* conditionRow(int i, int j, int k) {
    auto& c = _d.nodes[i].responses[j].conditions[k];
    auto ref = [this, i, j, k]() -> DialogueCondition& { return _d.nodes[i].responses[j].conditions[k]; };
    auto row = new QHBoxLayout;
    auto kind = new QComboBox;
    for (auto const& choice : conditionKinds) kind->addItem(choice.second, int(choice.first));
    kind->setCurrentIndex(std::max(0, kind->findData(int(c.kind))));
    row->addWidget(kind);
    connect(kind, qOverload<int>(&QComboBox::activated), this, [this, i, j, k, kind, ref](int index) {
      auto& c = ref(); auto before = c;
      c.kind = Kind(kind->itemData(index).toInt());
      bool level = c.kind == Kind::MinLevel || c.kind == Kind::MaxLevel, wasLevel = before.kind == Kind::MinLevel || before.kind == Kind::MaxLevel;
      if (c.aboutQuest() != before.aboutQuest() || c.aboutItem() != before.aboutItem() || level != wasLevel) { c.value = level ? 10 : 0; c.count = 1; }
      changed(true, true); reselect(i, j);
    });
    if (c.aboutQuest() || c.aboutItem()) {
      auto picker = new EntryPicker(c.aboutQuest() ? _session.lookups().quests.get() : _session.lookups().items.get()); picker->setEntry(c.value);
      connect(picker, &QComboBox::currentTextChanged, this, [this, picker, ref] { ref().value = picker->entry(); changed(false, true); });
      row->addWidget(Ui::Content::browsable(picker), 1);
      if (c.aboutItem()) {
        auto count = new QSpinBox; count->setRange(1, 255); count->setPrefix("× "); count->setValue(c.count); row->addWidget(count);
        connect(count, qOverload<int>(&QSpinBox::valueChanged), this, [this, ref](int value) { ref().count = value; changed(false); });
      }
    } else {
      auto level = new QSpinBox; level->setRange(1, 60); level->setValue(std::max<int>(1, c.value)); row->addWidget(level); row->addStretch();
      connect(level, qOverload<int>(&QSpinBox::valueChanged), this, [this, ref](int value) { ref().value = value; changed(false); });
    }
    auto remove = new QToolButton; remove->setIcon(Ui::FontAwesomeIcon(Icon::trash)); remove->setAutoRaise(true); remove->setToolTip("Remove this condition");
    row->addWidget(remove);
    connect(remove, &QToolButton::clicked, this, [this, i, j, k] { _d.nodes[i].responses[j].conditions.removeAt(k); changed(true, true); reselect(i, j); });
    return row;
  }
  // --- actions ---
  bool save() {
    try {
      refreshFacts();
      if (hasErrors(GossipService::check(_d, _facts))) { recheck(); updateTree(); QMessageBox::warning(this, "Dialogue", "Fix the problems marked ✗ first."); return false; }
      GossipService::save(_d);
    } catch (std::exception const& e) { QMessageBox::warning(this, "Dialogue", QString::fromUtf8(e.what())); return false; }
    saved = true;
    load(); // the saved rows' IDs, and no longer shared
    return true;
  }
  void copyFrom() {
    auto source = chooseOwner(this, "Copy dialogue from", [](QString const& text) { return GossipService::owners(text); });
    if (!source || source->id == _entry) return;
    try {
      auto from = GossipService::load(source->id);
      if (from.nodes.isEmpty()) { QMessageBox::information(this, "Copy Dialogue", source->name + " has no dialogue."); return; }
      if (!_d.nodes.isEmpty() && QMessageBox::question(this, "Copy Dialogue", QString("Replace this dialogue with %1's (%2 nodes)? Nothing is saved until you click Save.").arg(source->name).arg(from.nodes.size())) != QMessageBox::Yes) return;
      _d = GossipService::copied(from, _d);
      if (!_d.dropped.isEmpty())
        QMessageBox::information(this, "Copy Dialogue", "Copied. Some of what " + source->name + "'s dialogue does cannot be shown here and was left out:\n\n• " + _d.dropped.join("\n• "));
      _d.dropped.clear(); _dropped->hide();
      changed(true, true); reselect(0, -1);
    } catch (std::exception const& e) { QMessageBox::warning(this, "Copy Dialogue", e.what()); }
  }
  void copyTo() {
    auto target = chooseOwner(this, "Give this dialogue to one of your NPCs", [](QString const& text) { return CreatureService::search(text, true); });
    if (!target || target->id == _entry) return;
    try {
      auto other = GossipService::load(target->id);
      if (!other.nodes.isEmpty() && QMessageBox::question(this, "Copy to My NPC", "Replace what " + target->name + " says with this dialogue?") != QMessageBox::Yes) return;
      auto copy = GossipService::copied(_d, other);
      if (hasErrors(GossipService::validate(copy))) {
        QMessageBox::warning(this, "Copy to My NPC", "The copy needs changes before it can be saved: open " + target->name + "'s dialogue, copy from this NPC there and fix what is marked."); return;
      }
      GossipService::save(copy); saved = true;
      QMessageBox::information(this, "Copy to My NPC", target->name + " now has this dialogue" + QString(_d.dropped.isEmpty() ? "" : " (without the parts listed at the top)")
                               + ". It is listed in Local Changes.");
    } catch (std::exception const& e) { QMessageBox::warning(this, "Copy to My NPC", e.what()); }
  }
  void testDialogue() {
    auto tests = TestSessionService::instance();
    if (!tests) return;
    if (_dirty && (QMessageBox::question(this, "Test", "Save the dialogue before testing?") != QMessageBox::Yes || !save())) return;
    QDialog options(this); options.setWindowTitle("Test Dialogue");
    auto layout = new QVBoxLayout(&options);
    auto text = new QLabel("Your local test character is placed beside " + _d.name + ". Talk to it to try each branch."); text->setWordWrap(true); layout->addWidget(text);
    QStringList gated;
    int levelMin = 0;
    for (int i = 0; i < _d.nodes.size(); ++i) for (auto const& r : _d.nodes[i].responses) if (!r.conditions.isEmpty()) {
      QStringList rules; for (auto const& c : r.conditions) { rules << describe(c); if (c.kind == Kind::MinLevel) levelMin = std::max<int>(levelMin, c.value); }
      gated << "“" + elide(r.text, 40) + "” (" + nodeName(i) + "): " + rules.join("; ");
    }
    if (!gated.isEmpty()) {
      auto list = new QLabel("Only some players see these responses:\n• " + gated.join("\n• ")); list->setWordWrap(true); layout->addWidget(list);
    }
    auto level = new QCheckBox("Set my test character's level to"); auto value = new QSpinBox; value->setRange(1, 60); value->setValue(std::max(levelMin, 1));
    level->setChecked(levelMin > 1);
    auto row = new QHBoxLayout; row->addWidget(level); row->addWidget(value); row->addStretch(); layout->addLayout(row);
    auto note = new QLabel("Only changes your character on the local test server. Quests and items are as your character has them."); note->setWordWrap(true);
    note->setStyleSheet("color: gray;"); layout->addWidget(note);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); buttons->addButton("Test", QDialogButtonBox::AcceptRole); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &options, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &options, &QDialog::reject);
    if (options.exec() != QDialog::Accepted) return;
    TestOptions test; if (level->isChecked()) test.level = value->value();
    tests->testEntity(this, false, _entry, test);
  }
  Id _entry;
  Ui::Content::ContentSession& _session;
  Dialogue _d;
  DialogueFacts _facts;
  QVector<DialogueProblem> _problems;
  int _node = -1, _response = -1;
  bool _dirty = false, _building = false;
  QTimer _factsTimer;
  QPointer<QLabel> _pageProblems;
  QLabel *_title, *_notice, *_dropped, *_listProblems;
  GossipWindow* _window;
  QTreeWidget* _tree;
  QScrollArea* _inspector;
  QPushButton *_copyFrom, *_copyTo, *_clear, *_reset, *_save;
};
}
bool editDialogue(QWidget* parent, World* world, Id npc, Ui::Content::ContentSession& session) {
  DialogueEditor editor(parent, world, npc, session);
  editor.exec();
  return editor.saved;
}
}
