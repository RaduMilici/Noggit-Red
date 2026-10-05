#include "EditorWidgets.hpp"
#include "ItemBrowser.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
namespace Noggit::Creator {
MoneyEdit::MoneyEdit(QWidget* parent) : QWidget(parent) {
  auto layout = new QHBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(2);
  auto spin = [&](int max, QString const& suffix, QString const& color) {
    auto s = new QSpinBox(this); s->setRange(0, max); s->setSuffix(suffix); s->setStyleSheet("QSpinBox { color: " + color + "; }");
    layout->addWidget(s);
    connect(s, qOverload<int>(&QSpinBox::valueChanged), this, [this] { if (!_setting && onChanged) onChanged(); });
    return s;
  };
  _gold = spin(214748, "g", "#e6b800"); _silver = spin(99, "s", "#b0b0b0"); _copper = spin(99, "c", "#c07040");
}
qint64 MoneyEdit::value() const { return qint64(_gold->value()) * 10000 + _silver->value() * 100 + _copper->value(); }
void MoneyEdit::setValue(qint64 c) {
  _setting = true;
  c = std::max<qint64>(0, c);
  _gold->setValue(int(c / 10000)); _silver->setValue(int(c / 100 % 100)); _copper->setValue(int(c % 100));
  _setting = false;
}
void MoneyEdit::setReadOnly(bool readOnly) { for (auto s : {_gold, _silver, _copper}) s->setReadOnly(readOnly); }
QString moneyHtml(qint64 copper) {
  if (copper <= 0) return "<span style='color:#c07040'>0c</span>";
  QStringList parts;
  if (auto g = copper / 10000) parts << QString("<span style='color:#e6b800'>%1g</span>").arg(g);
  if (auto s = copper / 100 % 100) parts << QString("<span style='color:#b0b0b0'>%1s</span>").arg(s);
  if (auto c = copper % 100) parts << QString("<span style='color:#c07040'>%1c</span>").arg(c);
  return parts.join(' ');
}
void ItemDropTable::dragEnterEvent(QDragEnterEvent* e) { if (e->mimeData()->hasFormat(itemMimeType) && isEnabled()) e->acceptProposedAction(); else QTableWidget::dragEnterEvent(e); }
void ItemDropTable::dragMoveEvent(QDragMoveEvent* e) { if (e->mimeData()->hasFormat(itemMimeType) && isEnabled()) e->acceptProposedAction(); else QTableWidget::dragMoveEvent(e); }
void ItemDropTable::dropEvent(QDropEvent* e) {
  auto items = itemsFromMime(e->mimeData());
  if (items.isEmpty()) { QTableWidget::dropEvent(e); return; }
  e->acceptProposedAction();
  if (onItemsDropped) onItemsDropped(items);
}
void showRowProblems(QTableWidget* table, int row, int column, QVector<RowProblem> const& problems) {
  QStringList errors, warnings;
  for (auto const& p : problems) if (p.row == row) (p.error ? errors : warnings) << p.text;
  auto cell = table->item(row, column);
  if (!cell) { cell = new QTableWidgetItem; cell->setFlags(Qt::ItemIsEnabled); table->setItem(row, column, cell); }
  auto all = errors + warnings;
  cell->setText(all.isEmpty() ? QString() : (errors.isEmpty() ? "⚠ " : "✗ ") + all.join(" "));
  cell->setToolTip(all.join('\n'));
  cell->setForeground(errors.isEmpty() ? QColor("#d08a00") : QColor("#e04040"));
}
QString listProblems(QVector<RowProblem> const& problems) {
  QStringList lines;
  for (auto const& p : problems) if (p.row < 0) lines << (p.error ? "✗ " : "⚠ ") + p.text;
  return lines.join('\n');
}
bool hasErrors(QVector<RowProblem> const& problems) {
  return std::any_of(problems.begin(), problems.end(), [](RowProblem const& p) { return p.error; });
}
QLabel* editorTitle(QString const& text, QWidget* parent) {
  auto label = new QLabel(text.toUpper(), parent);
  label->setStyleSheet("font-size: 15pt; font-weight: bold; letter-spacing: 1px;"); label->setTextFormat(Qt::PlainText);
  return label;
}
QLabel* noticeBanner(QString const& text, QWidget* parent) {
  auto label = new QLabel(text, parent); label->setWordWrap(true); label->setTextFormat(Qt::PlainText);
  label->setStyleSheet("background: rgba(214,160,40,0.18); border: 1px solid rgba(214,160,40,0.6); border-radius: 4px; padding: 6px;");
  label->setVisible(!text.isEmpty());
  return label;
}
bool confirmClose(QWidget* parent, bool dirty, std::function<bool()> const& save) {
  if (!dirty) return true;
  auto answer = QMessageBox::question(parent, "Unsaved changes", "Save your changes before closing?",
                                      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
  if (answer == QMessageBox::Cancel) return false;
  return answer == QMessageBox::Discard || save();
}
std::optional<Choice> chooseOwner(QWidget* parent, QString const& title, std::function<QVector<Choice>(QString const&)> const& search) {
  QDialog dialog(parent); dialog.setWindowTitle(title); dialog.resize(520, 520);
  auto layout = new QVBoxLayout(&dialog);
  auto input = new QLineEdit; input->setPlaceholderText("Search by name…"); input->setClearButtonEnabled(true); layout->addWidget(input);
  auto list = new QListWidget; layout->addWidget(list, 1);
  auto status = new QLabel; status->setStyleSheet("color: gray;"); layout->addWidget(status);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(buttons);
  QVector<Choice> results; QTimer timer; timer.setSingleShot(true); timer.setInterval(250);
  auto refresh = [&] {
    try {
      results = search(input->text()); list->clear();
      for (auto const& r : results) list->addItem(r.name + (r.detail.isEmpty() ? QString() : "   —   " + r.detail));
      status->setText(results.isEmpty() ? "Nothing matches." : results.size() >= 250 ? "Showing the first 250 matches." : QString());
    } catch (std::exception const& e) { results.clear(); list->clear(); status->setText(QString::fromUtf8(e.what())); }
  };
  QObject::connect(input, &QLineEdit::textChanged, &timer, [&] { timer.start(); });
  QObject::connect(&timer, &QTimer::timeout, &dialog, refresh);
  QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] { if (list->currentRow() >= 0) dialog.accept(); });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  refresh();
  if (dialog.exec() != QDialog::Accepted || list->currentRow() < 0 || list->currentRow() >= results.size()) return std::nullopt;
  return results[list->currentRow()];
}
}
