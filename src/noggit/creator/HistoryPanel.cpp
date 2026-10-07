#include "HistoryPanel.hpp"
#include "History.hpp"
#include "Timeline.hpp"
#include <noggit/ui/FontAwesome.hpp>
#include <QColor>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
namespace Noggit::Creator {
namespace {
using Icon = Ui::FontAwesome::Icons;
constexpr int entryRole = Qt::UserRole; // cursor to jump to when the row is clicked
Icon iconFor(Timeline::Entry const& e) {
  auto const& l = e.label;
  if (l.startsWith("Removed") || l.startsWith("Deleted")) return Icon::trashalt;
  if (l.startsWith("Brought back")) return Icon::undo;
  if (e.kind == Timeline::Entry::Kind::Map) return l.contains("terrain") || l.contains("ground") || l.contains("water") ? Icon::map : Icon::tree;
  if (l.contains("Quest")) return Icon::flag;
  if (l.contains("NPC") || l.startsWith("Placed") || l.startsWith("Moved")) return Icon::user;
  return Icon::save;
}
}
HistoryPanel::HistoryPanel(Timeline* timeline, RemovedDrawer::Actions removed, QWidget* parent) : QWidget(parent), _timeline(timeline) {
  auto layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
  _tabs = new QTabWidget(this); layout->addWidget(_tabs);

  auto history = new QWidget(_tabs); auto h = new QVBoxLayout(history); h->setContentsMargins(6, 6, 6, 6); h->setSpacing(4);
  auto hint = new QLabel("Every change is saved as you go. Click any step to go back to it; nothing is lost until you do something new.", history);
  hint->setWordWrap(true); hint->setStyleSheet("color: palette(mid);");
  h->addWidget(hint);
  auto buttons = new QHBoxLayout;
  _undo = new QPushButton(Ui::FontAwesomeIcon(Icon::undo), "Undo", history); _undo->setToolTip("Ctrl+Z");
  _redo = new QPushButton(Ui::FontAwesomeIcon(Icon::redo), "Redo", history); _redo->setToolTip("Ctrl+Y");
  auto save = new QPushButton(Ui::FontAwesomeIcon(Icon::bookmark), "Save point…", history);
  save->setToolTip("Name this moment (\"before the ambush idea\") so you can come back to it.");
  _savepoints = new QToolButton(history); _savepoints->setText("Go to ▾"); _savepoints->setPopupMode(QToolButton::InstantPopup);
  _savepoints->setMenu(new QMenu(_savepoints));
  buttons->addWidget(_undo); buttons->addWidget(_redo); buttons->addStretch(1); buttons->addWidget(save); buttons->addWidget(_savepoints);
  h->addLayout(buttons);
  _list = new QListWidget(history); h->addWidget(_list, 1);
  _status = new QLabel(history); _status->setStyleSheet("color: palette(mid);"); h->addWidget(_status);
  _tabs->addTab(history, Ui::FontAwesomeIcon(Icon::history), "History");

  _removed = new RemovedDrawer(std::move(removed), _tabs);
  _tabs->addTab(_removed, Ui::FontAwesomeIcon(Icon::trashalt), "Removed");

  connect(_undo, &QPushButton::clicked, this, [this] { _timeline->undo(); });
  connect(_redo, &QPushButton::clicked, this, [this] { _timeline->redo(); });
  connect(save, &QPushButton::clicked, this, &HistoryPanel::addSavepoint);
  connect(_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
    auto target = item->data(entryRole);
    if (target.isValid() && target.toInt() != _timeline->cursor()) _timeline->jump(target.toInt());
  });
  connect(_timeline, &Timeline::changed, this, &HistoryPanel::refresh);
  // Keep "3 min ago" honest.
  auto clock = new QTimer(this); clock->setInterval(30000); clock->start();
  connect(clock, &QTimer::timeout, this, &HistoryPanel::refresh);
  refresh();
}
void HistoryPanel::showRemoved() { _tabs->setCurrentWidget(_removed); }
void HistoryPanel::showHistory() { _tabs->setCurrentIndex(0); }
void HistoryPanel::addSavepoint() {
  if (!_timeline->cursor()) return;
  bool ok = false;
  auto name = QInputDialog::getText(this, "Save point", "Name this moment:", QLineEdit::Normal, QString(), &ok);
  if (ok && !name.trimmed().isEmpty()) _timeline->addSavepoint(name);
}
void HistoryPanel::refresh() {
  auto const& entries = _timeline->entries();
  int cursor = _timeline->cursor();
  _list->clear();
  // Newest first; the row's click target is the state right after that step.
  for (int i = entries.size() - 1; i >= 0; --i) {
    auto const& e = entries[i];
    auto savepoint = _timeline->savepoint(i);
    auto text = e.label + "    ·    " + relativeTime(e.time);
    if (!savepoint.isEmpty()) text = "◆ " + savepoint + "    —    " + text;
    auto item = new QListWidgetItem(Ui::FontAwesomeIcon(iconFor(e)), text, _list);
    item->setData(entryRole, i + 1);
    bool undone = i >= cursor;
    if (undone) { auto f = item->font(); f.setItalic(true); item->setFont(f); item->setForeground(QColor(140, 140, 140)); item->setToolTip("Undone. Click to redo up to here."); }
    else if (i == cursor - 1) { auto f = item->font(); f.setBold(true); item->setFont(f); item->setToolTip("You are here."); }
    if (!savepoint.isEmpty()) { auto f = item->font(); f.setBold(true); item->setFont(f); }
  }
  auto start = new QListWidgetItem(Ui::FontAwesomeIcon(Icon::flag), "Start of history", _list);
  start->setData(entryRole, 0);
  if (!cursor) { auto f = start->font(); f.setBold(true); start->setFont(f); }
  _undo->setEnabled(_timeline->canUndo()); _redo->setEnabled(_timeline->canRedo());
  auto menu = _savepoints->menu(); menu->clear();
  for (int i = entries.size() - 1; i >= 0; --i) {
    auto name = _timeline->savepoint(i);
    if (name.isEmpty()) continue;
    menu->addAction(Ui::FontAwesomeIcon(Icon::bookmark), name + "  ·  " + relativeTime(entries[i].time), this, [this, i] { _timeline->jump(i + 1); });
  }
  _savepoints->setEnabled(!menu->isEmpty());
  auto undone = entries.size() - cursor;
  _status->setText(undone ? QString("%1 undone %2 — do something new and they are set aside.").arg(undone).arg(undone == 1 ? "step" : "steps")
                          : QString("%1 %2").arg(entries.size()).arg(entries.size() == 1 ? "step" : "steps"));
  if (auto store = HistoryStore::instance(); store && !store->warning().isEmpty()) _status->setText(store->warning());
}
}
