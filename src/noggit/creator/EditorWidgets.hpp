#pragma once
#include "Services.hpp"
#include <QTableWidget>
#include <QWidget>
#include <functional>
#include <optional>
class QLabel;
class QSpinBox;
namespace Noggit::Creator {
// Building blocks shared by the loot, vendor and trainer editors.

// Gold / silver / copper.
class MoneyEdit final : public QWidget {
public:
  explicit MoneyEdit(QWidget* parent = nullptr);
  qint64 value() const;
  void setValue(qint64 copper);
  void setReadOnly(bool);
  std::function<void()> onChanged;
private:
  QSpinBox *_gold, *_silver, *_copper;
  bool _setting = false;
};
// "2g 40s 5c" with coin colours, as rich text.
QString moneyHtml(qint64 copper);

// A table that accepts items dragged from the Item Browser.
class ItemDropTable final : public QTableWidget {
public:
  using QTableWidget::QTableWidget;
  std::function<void(QVector<Id> const&)> onItemsDropped;
protected:
  void dragEnterEvent(QDragEnterEvent*) override;
  void dragMoveEvent(QDragMoveEvent*) override;
  void dropEvent(QDropEvent*) override;
};
// A table cell showing the problems of `row` (errors red, warnings amber); clears it when there are none.
void showRowProblems(QTableWidget* table, int row, int column, QVector<RowProblem> const& problems);
// The problems that belong to the whole list, for a banner under the table.
QString listProblems(QVector<RowProblem> const& problems);
bool hasErrors(QVector<RowProblem> const& problems);

// "RESTLESS MILLER — LOOT" and a notice banner (hidden when empty).
QLabel* editorTitle(QString const& text, QWidget* parent);
QLabel* noticeBanner(QString const& text, QWidget* parent);

// Asks whether to save, discard or keep editing unsaved changes. True when the editor may close;
// `save` is called for Save and returns whether it worked.
bool confirmClose(QWidget* parent, bool dirty, std::function<bool()> const& save);
// Search-and-pick among NPCs or objects (for copying from or to).
std::optional<Choice> chooseOwner(QWidget* parent, QString const& title, std::function<QVector<Choice>(QString const&)> const& search);
}
