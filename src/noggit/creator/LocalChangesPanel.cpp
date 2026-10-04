#include "LocalChangesPanel.hpp"
#include "ChangeExport.hpp"
#include "ProductionDialogs.hpp"
#include "ProductionProfile.hpp"
#include "TestSessionService.hpp"
#include "ClientPatch.hpp"
#include <noggit/runtime/ClientManager.hpp>
#include <noggit/runtime/RuntimeManager.hpp>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <algorithm>
#include <stdexcept>
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
    QString("Remove %1 %2 from Local Changes?\n\nThis only clears the list. The NPCs, GameObjects, patrols and quests stay in your local world exactly as they are.")
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
  auto note = new QLabel(QString("Exports %1 %2. NPCs, GameObjects, items and quests made in Noggit that these changes rely on are included automatically.")
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
  auto panel = new QDialog(window, Qt::Tool); panel->setWindowTitle("Local Changes"); panel->resize(420, 520);
  auto layout = new QVBoxLayout(panel);
  layout->addWidget(new QLabel("LOCAL CHANGES", panel));
  auto warning = new QLabel(panel); warning->setWordWrap(true); warning->setTextFormat(Qt::PlainText); layout->addWidget(warning);
  auto list = new QListWidget(panel); list->setSelectionMode(QAbstractItemView::ExtendedSelection); layout->addWidget(list, 1);
  auto empty = new QLabel("No local changes yet. NPCs, GameObjects, patrols and quests you save appear here.", panel);
  empty->setWordWrap(true); layout->addWidget(empty);
  auto testLocal = new QPushButton("Test Locally", panel);
  testLocal->setToolTip("Restart the local server with these changes and play them in WoW");
  auto sync = new QPushButton("Sync to Production", panel);
  sync->setToolTip("Send these changes to the production server: backed up first, put back if anything fails");
  sync->setStyleSheet("font-weight: bold;");
  auto production = new QToolButton(panel); production->setText("Server…"); production->setToolTip("Production server settings");
  auto syncRow = new QHBoxLayout; syncRow->addWidget(sync, 1); syncRow->addWidget(production);
  auto target = new QLabel(panel); target->setStyleSheet("color: gray;"); target->setTextFormat(Qt::PlainText);
  auto clear = new QPushButton("Clear Selected", panel), exportButton = new QPushButton("Export Changes", panel);
  auto fileRow = new QHBoxLayout; fileRow->addWidget(clear); fileRow->addWidget(exportButton);
  layout->addWidget(testLocal); layout->addLayout(syncRow); layout->addWidget(target); layout->addLayout(fileRow);
  // Client data: tracked apart from the database changes above, used by the local test client only.
  auto clientHeading = new QLabel("CLIENT DATA — LOCAL TEST CLIENT", panel); clientHeading->setStyleSheet("margin-top: 8px;");
  auto clientList = new QListWidget(panel); clientList->setSelectionMode(QAbstractItemView::NoSelection); clientList->setMaximumHeight(110);
  auto clientStatus = new QLabel(panel); clientStatus->setWordWrap(true); clientStatus->setStyleSheet("color: gray;");
  auto updateClient = new QPushButton("Update Test Client", panel), restoreClient = new QPushButton("Restore Original", panel);
  auto exportClient = new QPushButton("Export Client Patch…", panel);
  updateClient->setToolTip("Put your spells into the local test client now (Test Locally does this too)");
  restoreClient->setToolTip("Put the client's own files back (Play Production does this too)");
  exportClient->setToolTip("Save the client patch with your spells, e.g. to hand to production players");
  auto clientRow = new QHBoxLayout; clientRow->addWidget(updateClient); clientRow->addWidget(restoreClient); clientRow->addWidget(exportClient);
  layout->addWidget(clientHeading); layout->addWidget(clientList); layout->addWidget(clientStatus); layout->addLayout(clientRow);
  auto client = ClientPatchService::instance();
  auto refreshClient = [=] {
    clientList->clear();
    if (!client) return;
    if (!qApp->property("creatorDatabaseReady").toBool()) { clientStatus->setText("Start the local server to see client data changes."); return; }
    auto status = client->status();
    for (auto const& c : status.pending) new QListWidgetItem(c.summary() + (c.kind == ClientDataChange::Kind::Remove ? "  (removed)" : "  (not in the test client yet)"), clientList);
    for (auto const& label : status.installedSpells) if (std::none_of(status.pending.begin(), status.pending.end(), [&](auto const& c) { return c.label == label; }))
      new QListWidgetItem("✓ Spell.dbc: " + label, clientList);
    clientList->setVisible(clientList->count() > 0);
    QStringList lines;
    if (!status.problem.isEmpty()) lines << status.problem;
    else if (clientList->count() == 0) lines << "No client data changes. Spells you make are added to the test client's Spell.dbc.";
    if (status.installed) lines << "Creator's test patch is in " + QDir::toNativeSeparators(status.patch) + ".";
    if (!status.backup.isEmpty()) lines << "The client's own " + QFileInfo(status.backup).fileName() + " is backed up in " + QDir::toNativeSeparators(QFileInfo(status.backup).absolutePath())
                                          + " and kept inside Creator's patch; it is put back before playing on production.";
    lines << "Sync to Production does not send client data.";
    clientStatus->setText(lines.join('\n'));
    updateClient->setEnabled(status.problem.isEmpty() && !status.pending.isEmpty());
    restoreClient->setEnabled(status.installed);
    exportClient->setEnabled(status.problem.isEmpty() && !(status.installedSpells.isEmpty() && status.pending.isEmpty()));
  };
  auto clientAction = [=](auto action) {
    try { action(); } catch (std::exception const& e) { QMessageBox::warning(panel, "Client data", e.what()); }
    refreshClient();
  };
  QObject::connect(updateClient, &QPushButton::clicked, panel, [=] { clientAction([=] { client->install(); }); });
  QObject::connect(restoreClient, &QPushButton::clicked, panel, [=] { clientAction([=] { client->restoreOriginal(); }); });
  QObject::connect(exportClient, &QPushButton::clicked, panel, [=] {
    auto path = QFileDialog::getSaveFileName(panel, "Export client patch", QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath("patch-Z.mpq"),
                                             "Client patch (*.mpq)", nullptr, QFileDialog::DontUseNativeDialog);
    if (!path.isEmpty()) clientAction([=] { client->exportPatch(path); QMessageBox::information(panel, "Client data", "Saved " + QDir::toNativeSeparators(path)
      + ".\n\nPlayers put it in their client's Data folder as patch-Z.mpq. It also contains the files of your own patch-Z, if you have one."); });
  });
  if (client) QObject::connect(client, &ClientPatchService::changed, panel, refreshClient);
  // Test Locally installs the test client data; Play Production takes it out again.
  Runtime::ClientManager::beforeLaunch = [](Runtime::ClientManager::Profile profile) {
    auto* service = ClientPatchService::instance();
    if (!service) return;
    if (profile == Runtime::ClientManager::Profile::PlayProduction) { service->restoreOriginal(); return; }
    auto status = service->status();
    if (!status.problem.isEmpty()) throw std::runtime_error(status.problem.toStdString());
    if (!status.pending.isEmpty()) service->install();
  };
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
    exportButton->setEnabled(!changes.isEmpty()); sync->setEnabled(!changes.isEmpty()); clear->setEnabled(false);
    auto profile = ProductionProfile::load();
    target->setText(profile.configured() ? "Production: " + profile.describe() : "Production server not set up yet");
    button->setText(QString("Local changes: %1").arg(changes.size()));
    button->setToolTip("Show what you changed locally: test it, sync it to production or export it");
  };
  QObject::connect(list, &QListWidget::itemSelectionChanged, panel, [=] { clear->setEnabled(!list->selectedItems().isEmpty()); });
  QObject::connect(clear, &QPushButton::clicked, panel, [=] {
    QStringList selected; for (auto item : list->selectedItems()) selected << item->data(Qt::UserRole).toString();
    if (!selected.isEmpty() && confirmClear(panel, selected.size())) clearEntries(panel, selected);
  });
  QObject::connect(exportButton, &QPushButton::clicked, panel, [panel] { exportChanges(panel); });
  QObject::connect(sync, &QPushButton::clicked, panel, [=] { syncToProduction(panel); refresh(); });
  QObject::connect(production, &QToolButton::clicked, panel, [=] { if (editProductionProfile(panel)) refresh(); });
  QObject::connect(testLocal, &QPushButton::clicked, panel, [window] {
    if (auto session = TestSessionService::instance()) session->testLocal(window);
  });
  QObject::connect(button, &QToolButton::clicked, panel, [panel] { panel->show(); panel->raise(); panel->activateWindow(); });
  QObject::connect(tracker, &ChangeTracker::changed, panel, refresh);
  QObject::connect(tracker, &ChangeTracker::changed, panel, refreshClient);
  QObject::connect(button, &QToolButton::clicked, panel, refreshClient);
  refresh();
  window->statusBar()->addPermanentWidget(button);
}
}
