#include "LocalChangesPanel.hpp"
#include "ChangeExport.hpp"
#include <noggit/runtime/RuntimeManager.hpp>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
namespace Noggit::Creator {
namespace {
QString settingsPath() { return Runtime::RuntimeManager::instance()->root() + "/Workspace/runtime.ini"; }
QStringList ids(QVector<TrackedChange> const& changes) { QStringList result; for (auto const& c : changes) result << c.id; return result; }
bool confirmClear(QWidget* parent, int count) {
  return QMessageBox::question(parent, "Clear Local Changes",
    QString("Remove %1 %2 from Local Changes?\n\nThis only clears the list. The NPCs, placements and quests stay in your local world exactly as they are.")
      .arg(count).arg(count == 1 ? "entry" : "entries"),
    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}
void clearEntries(QWidget* parent, QStringList const& entries) {
  try { ChangeTracker::instance()->clear(entries); }
  catch (std::exception const& e) { QMessageBox::warning(parent, "Clear Local Changes", e.what()); }
}
void exportChanges(QWidget* parent) {
  auto tracker = ChangeTracker::instance();
  auto changes = tracker->changes();
  if (changes.isEmpty()) { QMessageBox::information(parent, "Export Changes", "There are no local changes to export."); return; }
  QSettings settings(settingsPath(), QSettings::IniFormat);
  QDialog dialog(parent); dialog.setWindowTitle("Export Changes");
  auto layout = new QVBoxLayout(&dialog); auto form = new QFormLayout; layout->addLayout(form);
  auto name = new QLineEdit; name->setPlaceholderText("Haunted Mill"); form->addRow("Package name", name);
  auto author = new QLineEdit(settings.value("export/author", qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME"))).toString());
  form->addRow("Author", author);
  auto folder = new QLineEdit(settings.value("export/folder", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString());
  folder->setReadOnly(true); auto browse = new QPushButton("Browse…"); auto folderRow = new QHBoxLayout; folderRow->addWidget(folder, 1); folderRow->addWidget(browse);
  form->addRow("Save in", folderRow);
  auto preview = new QLabel; preview->setWordWrap(true); preview->setTextFormat(Qt::PlainText); layout->addWidget(preview);
  auto note = new QLabel(QString("Exports %1 %2. NPCs, items and quests made in Noggit that these changes rely on are included automatically.")
    .arg(changes.size()).arg(changes.size() == 1 ? "change" : "changes"));
  note->setWordWrap(true); layout->addWidget(note);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); auto save = buttons->addButton("Export", QDialogButtonBox::AcceptRole);
  layout->addWidget(buttons);
  auto update = [&] {
    auto directory = ExportService::folderName(name->text());
    preview->setText(directory.isEmpty() ? QString() : "Creates " + QDir(folder->text()).filePath(directory));
    save->setEnabled(!directory.isEmpty());
  };
  QObject::connect(name, &QLineEdit::textChanged, &dialog, update);
  QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
    auto chosen = QFileDialog::getExistingDirectory(&dialog, "Save package in", folder->text(), QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
    if (!chosen.isEmpty()) { folder->setText(chosen); update(); }
  });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  ExportResult result;
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    try { result = ExportService::exportChanges(name->text(), author->text(), folder->text(), changes); dialog.accept(); }
    catch (std::exception const& e) { QMessageBox::warning(&dialog, "Export Changes", e.what()); }
  });
  update();
  if (dialog.exec() != QDialog::Accepted) return;
  settings.setValue("export/author", author->text().trimmed());
  settings.setValue("export/folder", folder->text());
  QMessageBox done(QMessageBox::Information, "Export Changes",
    QString("Exported %1 %2 to\n%3").arg(result.changes).arg(result.changes == 1 ? "change" : "changes").arg(QDir::toNativeSeparators(result.folder))
      + (result.dependencies ? QString("\n\n%1 NPC, item or quest %2 they rely on %3 included.").arg(result.dependencies).arg(result.dependencies == 1 ? "definition" : "definitions").arg(result.dependencies == 1 ? "was" : "were") : QString()),
    QMessageBox::Close, parent);
  auto open = done.addButton("Open Folder", QMessageBox::ActionRole);
  auto clear = done.addButton("Clear Exported Entries…", QMessageBox::ActionRole);
  done.exec();
  if (done.clickedButton() == open) QDesktopServices::openUrl(QUrl::fromLocalFile(result.folder));
  else if (done.clickedButton() == clear && confirmClear(parent, changes.size())) clearEntries(parent, ids(changes));
}
}
void addLocalChangesPanel(QMainWindow* window) {
  if (!qApp->property("creatorRuntimeManaged").toBool()) return;
  auto tracker = ChangeTracker::instance();
  if (!tracker || window->findChild<QToolButton*>("localChangesButton")) return;
  auto button = new QToolButton(window); button->setObjectName("localChangesButton"); button->setAutoRaise(true);
  auto panel = new QDialog(window, Qt::Tool); panel->setWindowTitle("Local Changes"); panel->resize(420, 460);
  auto layout = new QVBoxLayout(panel);
  layout->addWidget(new QLabel("LOCAL CHANGES", panel));
  auto warning = new QLabel(panel); warning->setWordWrap(true); warning->setTextFormat(Qt::PlainText); layout->addWidget(warning);
  auto list = new QListWidget(panel); list->setSelectionMode(QAbstractItemView::ExtendedSelection); layout->addWidget(list, 1);
  auto empty = new QLabel("No local changes yet. NPCs, placements and quests you save appear here.", panel);
  empty->setWordWrap(true); layout->addWidget(empty);
  auto clear = new QPushButton("Clear Selected", panel), exportButton = new QPushButton("Export Changes", panel);
  layout->addWidget(clear); layout->addWidget(exportButton);
  auto refresh = [=] {
    auto const& changes = tracker->changes();
    list->clear();
    for (auto const& c : changes) {
      auto item = new QListWidgetItem(c.summary(), list); item->setData(Qt::UserRole, c.id);
      item->setToolTip(QString("%1 #%2 · %3 · %4").arg(toString(c.type).toUpper()).arg(c.entity).arg(toString(c.action))
        .arg(c.timestamp.toLocalTime().toString("yyyy-MM-dd HH:mm")));
    }
    warning->setText(tracker->warning()); warning->setVisible(!tracker->warning().isEmpty());
    empty->setVisible(changes.isEmpty()); list->setVisible(!changes.isEmpty());
    exportButton->setEnabled(!changes.isEmpty()); clear->setEnabled(false);
    button->setText(QString("Local changes: %1").arg(changes.size()));
    button->setToolTip("Show what you changed locally and export it as a package");
  };
  QObject::connect(list, &QListWidget::itemSelectionChanged, panel, [=] { clear->setEnabled(!list->selectedItems().isEmpty()); });
  QObject::connect(clear, &QPushButton::clicked, panel, [=] {
    QStringList selected; for (auto item : list->selectedItems()) selected << item->data(Qt::UserRole).toString();
    if (!selected.isEmpty() && confirmClear(panel, selected.size())) clearEntries(panel, selected);
  });
  QObject::connect(exportButton, &QPushButton::clicked, panel, [panel] { exportChanges(panel); });
  QObject::connect(button, &QToolButton::clicked, panel, [panel] { panel->show(); panel->raise(); panel->activateWindow(); });
  QObject::connect(tracker, &ChangeTracker::changed, panel, refresh);
  refresh();
  window->statusBar()->addPermanentWidget(button);
}
}
