#pragma once
#include "RemovedDrawer.hpp"
#include <QWidget>
class QLabel;
class QListWidget;
class QPushButton;
class QTabWidget;
class QToolButton;
namespace Noggit::Creator {
class Timeline;
// History dock: every change in order (click one to go back to it), save points, and the Removed drawer.
class HistoryPanel final : public QWidget {
  Q_OBJECT
public:
  HistoryPanel(Timeline* timeline, RemovedDrawer::Actions removed, QWidget* parent = nullptr);
  void showRemoved();
  void showHistory();
  void addSavepoint();
private:
  Timeline* _timeline;
  QTabWidget* _tabs;
  QListWidget* _list;
  QLabel* _status;
  QPushButton *_undo, *_redo;
  QToolButton* _savepoints;
  RemovedDrawer* _removed;
  void refresh();
};
}
