// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// The look the content editors share: a sidebar of sections, cards, hints, "add" buttons, removable
// rows, coloured badges. Colours come from the widget palette, so the editors follow Noggit's theme.

#include <QtWidgets/QFrame>
#include <QtWidgets/QWidget>

#include <QtCore/QStringList>

#include <functional>
#include <string>
#include <vector>

class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace Noggit::Ui::Content
{
  // Asks before a write: the question, then each consequence as a bullet. True when confirmed.
  bool confirmChange(QWidget* parent, QString const& title, QString const& question, QStringList const& consequences);
  // A refused save's reasons.
  void showProblems(QWidget* parent, QString const& title, QStringList const& problems);
  QStringList toQStringList(std::vector<std::string> const& lines);

  // Style sheet for an editor dialog (call once on the dialog).
  void applyEditorStyle(QWidget* dialog);

  // A titled card grouping related fields; put the fields in body().
  class Card : public QFrame
  {
  public:
    Card(QString const& title, QString const& description = {}, QWidget* parent = nullptr);
    QVBoxLayout* body() const { return _body; }
    void setTitle(QString const& title);

  private:
    QLabel* _title = nullptr;
    QVBoxLayout* _body = nullptr;
  };

  QLabel* hintLabel(QString const& text, QWidget* parent = nullptr);
  QLabel* problemLabel(QWidget* parent = nullptr);
  QPushButton* addButton(QString const& text, QWidget* parent = nullptr);
  QToolButton* removeButton(QWidget* parent = nullptr);

  // A small coloured tag ("KILL", "COLLECT", ...).
  QLabel* badge(QString const& text, QColor const& color, QWidget* parent = nullptr);

  namespace BadgeColor
  {
    QColor const kill(196, 64, 64);
    QColor const use(52, 120, 200);
    QColor const collect(56, 150, 80);
    QColor const explore(30, 150, 150);
    QColor const reputation(140, 80, 190);
    QColor const event(200, 140, 40);
  }

  // A vertical list of rows the user adds and removes. Rows are the page's own widgets; the list wraps
  // each with a remove button.
  class RowList : public QWidget
  {
  public:
    explicit RowList(QWidget* parent = nullptr);

    void addRow(QWidget* row);
    std::vector<QWidget*> rows() const;
    int count() const { return static_cast<int>(_rows.size()); }
    void clear();

    std::function<void()> on_changed;

  private:
    QVBoxLayout* _layout = nullptr;
    std::vector<std::pair<QWidget*, QWidget*>> _rows; // (wrapper, row)
  };
}
