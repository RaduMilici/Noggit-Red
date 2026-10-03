#include "ProductionDialogs.hpp"
#include "ProductionSync.hpp"
#include <noggit/runtime/RuntimeManager.hpp>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <exception>
#include <optional>
namespace Noggit::Creator {
namespace {
using Step = ProductionSync::Step;
using State = ProductionSync::State;
using Rollback = ProductionSync::Outcome::Rollback;
QString workspace() { return Runtime::RuntimeManager::instance()->root() + "/Workspace"; }
// Runs blocking network work off the UI thread while a busy indicator shows; rethrows its exception.
template <class Work> auto inBackground(QWidget* parent, QString const& text, Work&& work) -> decltype(work()) {
  std::optional<decltype(work())> result; std::exception_ptr error;
  QProgressDialog busy(text, QString(), 0, 0, parent);
  busy.setWindowTitle("Production Server"); busy.setWindowModality(Qt::WindowModal); busy.setCancelButton(nullptr); busy.setMinimumDuration(0);
  QEventLoop loop;
  auto thread = QThread::create([&] { try { result.emplace(work()); } catch (...) { error = std::current_exception(); } });
  QObject::connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
  thread->start(); busy.show(); loop.exec(); thread->wait(); delete thread;
  if (error) std::rethrow_exception(error);
  return std::move(*result);
}
bool confirmHostKey(QWidget* parent, ProductionProfile const& profile, QVector<Ssh::HostKey> const& keys) {
  QStringList lines; for (auto const& key : keys) lines << key.type + "   " + key.fingerprint;
  QMessageBox box(QMessageBox::Warning, "Verify Server", "Noggit has not connected to " + profile.sshHost + ":" + QString::number(profile.sshPort)
                  + " before. Is this really your server?", QMessageBox::NoButton, parent);
  box.setInformativeText("Compare these fingerprints with the ones the server's administrator gives you (on the server: "
                         "ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub). Trust them only if they match.\n\n" + lines.join('\n'));
  box.setTextInteractionFlags(Qt::TextSelectableByMouse);
  auto trust = box.addButton("Trust and Connect", QMessageBox::AcceptRole);
  box.setDefaultButton(box.addButton(QMessageBox::Cancel));
  box.exec();
  return box.clickedButton() == trust;
}
QListWidget* changeList(QVector<TrackedChange> const& changes, QWidget* parent) {
  auto list = new QListWidget(parent); list->setSelectionMode(QAbstractItemView::NoSelection); list->setFocusPolicy(Qt::NoFocus);
  for (auto const& c : changes) list->addItem(c.summary());
  list->setMaximumHeight(std::min(220, 24 + 20 * int(changes.size())));
  return list;
}
bool confirmSync(QWidget* parent, ProductionProfile const& profile, ChangePackage const& package) {
  QDialog dialog(parent); dialog.setWindowTitle("Sync to Production");
  auto layout = new QVBoxLayout(&dialog);
  auto heading = new QLabel(QString("Sync %1 %2 to Production?").arg(package.tracked).arg(package.tracked == 1 ? "change" : "changes"));
  heading->setStyleSheet("font-size: 13pt; font-weight: bold;"); layout->addWidget(heading);
  layout->addWidget(changeList(package.changes.mid(0, package.tracked), &dialog));
  if (package.changes.size() > package.tracked) {
    auto also = new QLabel("Also sent, because these changes rely on them:"); layout->addWidget(also);
    layout->addWidget(changeList(package.changes.mid(package.tracked), &dialog));
  }
  auto note = new QLabel(
    "To: " + profile.describe() + " (as " + profile.sshUser + ")\n\n"
    "The production rows these changes touch are backed up first, and put back if anything fails.\n"
    + (profile.restartCommand.trimmed().isEmpty()
         ? QString("The world server is not restarted: the changes appear in game after its next restart.")
         : QString("The world server is restarted afterwards: players online are disconnected for a few minutes.")));
  note->setWordWrap(true); note->setTextFormat(Qt::PlainText); layout->addWidget(note);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
  auto sync = buttons->addButton("Sync", QDialogButtonBox::AcceptRole); sync->setAutoDefault(false);
  buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
  layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  return dialog.exec() == QDialog::Accepted;
}
// The sync's steps as they happen; cannot be closed while production is being changed.
class SyncDialog final : public QDialog {
public:
  explicit SyncDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Sync to Production"); setMinimumWidth(460);
    auto layout = new QVBoxLayout(this);
    for (auto& row : _rows) { row = new QLabel(this); row->setTextFormat(Qt::RichText); row->hide(); layout->addWidget(row); }
    _status = new QLabel(this); _status->setWordWrap(true); _status->setTextFormat(Qt::PlainText); _status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addSpacing(8); layout->addWidget(_status);
    _details = new QPlainTextEdit(this); _details->setReadOnly(true); _details->hide(); _details->setMinimumHeight(120); layout->addWidget(_details);
    auto buttons = new QHBoxLayout; layout->addLayout(buttons);
    _open = new QPushButton("Open Sync Folder", this); _close = new QPushButton("Close", this);
    _open->setEnabled(false); _close->setEnabled(false);
    buttons->addWidget(_open); buttons->addStretch(); buttons->addWidget(_close);
    connect(_close, &QPushButton::clicked, this, &QDialog::accept);
    connect(_open, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(_folder)); });
    _status->setText("Connecting…");
  }
  void reject() override { if (!_running) QDialog::reject(); }
  void step(Step step, State state, QString const& detail) {
    static char const* titles[] = {"SSH connected", "Backup created", "Database changes applied", "World server restarted"};
    static char const* failures[] = {"Connection", "Backup", "Applying the changes", "World server restart"};
    auto row = _rows[int(step)]; row->show();
    auto text = detail.toHtmlEscaped();
    switch (state) {
      case State::Running: row->setText("<span style='color:gray'>…</span> " + text); _status->setText(detail); break;
      case State::Done: row->setText(QString("<span style='color:#2e9d4f'>✓</span> ") + titles[int(step)] + " <span style='color:gray'>— " + text + "</span>"); break;
      case State::Skipped: row->setText("<span style='color:#c08a00'>–</span> World server not restarted <span style='color:gray'>— " + text + "</span>"); _skipped = true; break;
      case State::Failed: row->setText(QString("<span style='color:#d33'>✗</span> <b>") + failures[int(step)] + "</b> failed"); break;
    }
  }
  void finish(ProductionSync::Outcome const& outcome, QString const& folder) {
    _running = false; _folder = folder; _open->setEnabled(true); _close->setEnabled(true); _close->setDefault(true);
    if (outcome.ok) {
      _status->setText(_skipped ? "Production database is up to date. Restart the world server to load the changes."
                                : "Production is up to date.");
      _status->setStyleSheet("font-weight: bold; color: #2e9d4f;");
      return;
    }
    QString state = outcome.rollback == Rollback::Restored ? "Production was put back exactly as it was before the sync."
                  : outcome.rollback == Rollback::Failed ? "Production could NOT be put back automatically and may be partly changed. "
                                                           "Apply restore.sql from the sync folder to the production database to undo it."
                  : "Production was not changed.";
    _status->setText(outcome.error + "\n\n" + state + "\nYour local changes are still listed; nothing was marked as synced.");
    _status->setStyleSheet(outcome.rollback == Rollback::Failed ? "color: #d33; font-weight: bold;" : "color: #d33;");
    if (!outcome.details.trimmed().isEmpty()) { _details->setPlainText(outcome.details.trimmed()); _details->show(); }
  }
private:
  QLabel* _rows[4]{};
  QLabel* _status = nullptr;
  QPlainTextEdit* _details = nullptr;
  QPushButton *_open = nullptr, *_close = nullptr;
  QString _folder;
  bool _running = true, _skipped = false;
};
}
bool editProductionProfile(QWidget* parent) {
  auto profile = ProductionProfile::load();
  QDialog dialog(parent); dialog.setWindowTitle("Production Server"); dialog.setMinimumWidth(520);
  auto layout = new QVBoxLayout(&dialog);
  auto intro = new QLabel("Sync to Production sends your local changes to this server over SSH. The database is reached through "
                          "the SSH connection, so its port never needs to be open to the internet. Use a dedicated SSH key and "
                          "database user with only the rights a sync needs (see the Creator README).");
  intro->setWordWrap(true); layout->addWidget(intro);
  auto group = [&](QString const& title) { auto box = new QGroupBox(title); auto form = new QFormLayout(box); layout->addWidget(box); return form; };
  auto port = [](unsigned value) { auto spin = new QSpinBox; spin->setRange(1, 65535); spin->setValue(int(value)); return spin; };

  auto ssh = group("SSH");
  auto sshHost = new QLineEdit(profile.sshHost); sshHost->setPlaceholderText("play.example.com or 203.0.113.10"); ssh->addRow("Host", sshHost);
  auto sshPort = port(profile.sshPort); ssh->addRow("Port", sshPort);
  auto sshUser = new QLineEdit(profile.sshUser); sshUser->setPlaceholderText("noggit-sync"); ssh->addRow("User", sshUser);
  auto key = new QLineEdit(profile.keyPath); key->setPlaceholderText("~/.ssh/noggit_sync (the file without .pub)");
  auto browse = new QPushButton("Browse…"); auto keyRow = new QHBoxLayout; keyRow->addWidget(key, 1); keyRow->addWidget(browse);
  ssh->addRow("Private key", keyRow);

  auto db = group("World database (as the server sees it)");
  auto dbHost = new QLineEdit(profile.dbHost); db->addRow("Host", dbHost);
  auto dbPort = port(profile.dbPort); db->addRow("Port", dbPort);
  auto database = new QLineEdit(profile.database); db->addRow("Database", database);
  auto dbUser = new QLineEdit(profile.dbUser); dbUser->setPlaceholderText("noggit_sync"); db->addRow("User", dbUser);
  auto mode = new QComboBox;
  mode->addItem("Ask when syncing (remembered until Noggit closes)"); mode->addItem("Save in the system keyring");
  if (!PasswordStore::keyringAvailable())
    if (auto model = qobject_cast<QStandardItemModel*>(mode->model())) { model->item(1)->setEnabled(false); model->item(1)->setToolTip(PasswordStore::keyringHint()); }
  mode->setCurrentIndex(profile.passwordMode == ProductionProfile::PasswordMode::Keyring && PasswordStore::keyringAvailable() ? 1 : 0);
  db->addRow("Password", mode);
  auto password = new QLineEdit; password->setEchoMode(QLineEdit::Password);
  password->setPlaceholderText(PasswordStore::lookup(profile).isEmpty() ? "Not entered yet" : "Known (leave empty to keep it)");
  db->addRow("", password);
  if (!PasswordStore::keyringAvailable()) {
    auto hint = new QLabel(PasswordStore::keyringHint()); hint->setWordWrap(true); hint->setStyleSheet("color: gray;"); db->addRow("", hint);
  }

  auto world = group("World server");
  auto restart = new QLineEdit(profile.restartCommand); restart->setPlaceholderText("cd ~/tortoise-deploy && docker compose restart mangosd");
  world->addRow("Restart command", restart);
  auto restartNote = new QLabel("Runs on the server over SSH after the changes are applied (the world server loads NPCs and quests "
                                "when it starts). Leave empty to restart it yourself.");
  restartNote->setWordWrap(true); restartNote->setStyleSheet("color: gray;"); world->addRow("", restartNote);

  auto result = new QLabel; result->setWordWrap(true); result->setTextFormat(Qt::PlainText); result->setTextInteractionFlags(Qt::TextSelectableByMouse);
  result->hide(); layout->addWidget(result);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
  auto test = buttons->addButton("Test Connection", QDialogButtonBox::ActionRole);
  auto forgetKey = buttons->addButton("Forget Trusted Key…", QDialogButtonBox::ActionRole);
  auto save = buttons->addButton("Save", QDialogButtonBox::AcceptRole); save->setDefault(true);
  layout->addWidget(buttons);

  auto read = [&] {
    ProductionProfile p;
    p.sshHost = sshHost->text().trimmed(); p.sshPort = unsigned(sshPort->value()); p.sshUser = sshUser->text().trimmed();
    p.keyPath = key->text().trimmed();
    if (p.keyPath.startsWith("~/")) p.keyPath = QDir::homePath() + p.keyPath.mid(1);
    p.dbHost = dbHost->text().trimmed(); p.dbPort = unsigned(dbPort->value());
    p.database = database->text().trimmed(); p.dbUser = dbUser->text().trimmed();
    p.passwordMode = mode->currentIndex() == 1 ? ProductionProfile::PasswordMode::Keyring : ProductionProfile::PasswordMode::Ask;
    p.restartCommand = restart->text().trimmed();
    return p;
  };
  auto show = [&](QString const& text, bool good) {
    result->setText(text); result->setStyleSheet(good ? "color: #2e9d4f;" : "color: #d33;"); result->show();
  };
  QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
    auto start = key->text().isEmpty() ? QDir::homePath() + "/.ssh" : key->text();
    auto chosen = QFileDialog::getOpenFileName(&dialog, "SSH private key", start, {}, nullptr, QFileDialog::DontUseNativeDialog | QFileDialog::DontResolveSymlinks);
    if (!chosen.isEmpty()) key->setText(chosen);
  });
  QObject::connect(test, &QPushButton::clicked, &dialog, [&] {
    auto p = read();
    if (auto problem = p.validate(); !problem.isEmpty()) { show(problem, false); return; }
    auto secret = password->text().isEmpty() ? PasswordStore::lookup(p) : password->text();
    if (secret.isEmpty()) { show("Enter the database password to test the connection.", false); password->setFocus(); return; }
    QJsonObject local;
    try { local = ExportService::localSource(); } catch (std::exception const&) { /* compared only when the local world runs */ }
    for (bool retry = true; retry;) {
      retry = false;
      try {
        auto check = inBackground(&dialog, "Connecting to " + p.sshHost + "…", [&] { return ProductionSync::test(p, secret, local); });
        QStringList lines; for (auto const& line : check.lines) lines << "✓ " + line;
        for (auto const& warning : check.warnings) lines << "⚠ " + warning;
        show(lines.join('\n'), true);
      } catch (Ssh::Error const& e) {
        if (e.problem != Ssh::Problem::HostUnknown) { show(QString::fromUtf8(e.what()), false); continue; }
        try {
          auto keys = inBackground(&dialog, "Reading the server's host key…", [&] { return Ssh::scanHostKeys(p.sshHost, p.sshPort); });
          if (!confirmHostKey(&dialog, p, keys)) { show("The server's host key was not trusted, so Noggit did not connect.", false); continue; }
          Ssh::trust(ProductionProfile::knownHostsFile(), keys);
          retry = true;
        } catch (std::exception const& scan) { show(QString::fromUtf8(scan.what()), false); }
      } catch (std::exception const& e) { show(QString::fromUtf8(e.what()), false); }
    }
  });
  QObject::connect(forgetKey, &QPushButton::clicked, &dialog, [&] {
    auto p = read();
    if (QMessageBox::question(&dialog, "Forget Trusted Key",
          "Forget the host key Noggit trusted for " + p.sshHost + "? Only do this when the server's administrator confirmed the server "
          "was rebuilt. You will verify the new fingerprint on the next Test Connection.\n\nKeys in your own ~/.ssh/known_hosts are not changed.",
          QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    try { Ssh::forget(ProductionProfile::knownHostsFile(), p.sshHost, p.sshPort); show("The trusted key was forgotten.", true); }
    catch (std::exception const& e) { show(QString::fromUtf8(e.what()), false); }
  });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    auto p = read();
    if (auto problem = p.validate(); !problem.isEmpty()) { show(problem, false); return; }
    try { p.save(); } catch (std::exception const& e) { show(QString::fromUtf8(e.what()), false); return; }
    // A password saved under the old settings must not outlive them.
    if (p.passwordMode == ProductionProfile::PasswordMode::Ask || !password->text().isEmpty()) {
      auto old = profile; old.passwordMode = ProductionProfile::PasswordMode::Keyring; PasswordStore::forget(old);
    }
    if (!password->text().isEmpty()) {
      try { PasswordStore::store(p, password->text()); }
      catch (std::exception const& e) { QMessageBox::warning(&dialog, "Production Server", QString::fromUtf8(e.what())); }
    }
    dialog.accept();
  });
  return dialog.exec() == QDialog::Accepted;
}
void syncToProduction(QWidget* parent) {
  auto tracker = ChangeTracker::instance();
  if (!tracker) return;
  auto changes = tracker->changes();
  if (changes.isEmpty()) { QMessageBox::information(parent, "Sync to Production", "There are no local changes to sync."); return; }
  auto profile = ProductionProfile::load();
  if (!profile.configured() || !profile.validate().isEmpty()) {
    QMessageBox::information(parent, "Sync to Production", "Set up the production server first.");
    if (!editProductionProfile(parent)) return;
    profile = ProductionProfile::load();
    if (!profile.configured()) return;
  }
  ChangePackage package;
  try { package = ExportService::build(changes); }
  catch (std::exception const& e) { QMessageBox::warning(parent, "Sync to Production", QString::fromUtf8(e.what())); return; }
  if (!confirmSync(parent, profile, package)) return;

  auto password = PasswordStore::lookup(profile);
  if (password.isEmpty()) {
    bool ok = false;
    password = QInputDialog::getText(parent, "Sync to Production", "Database password for " + profile.dbUser + " on " + profile.sshHost + ":",
                                     QLineEdit::Password, {}, &ok);
    if (!ok || password.isEmpty()) return;
    try { PasswordStore::store(profile, password); }
    catch (std::exception const& e) { QMessageBox::warning(parent, "Sync to Production", QString::fromUtf8(e.what())); }
  }

  // Every sync keeps its package, backup and log, whatever the outcome.
  auto stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
  auto folder = workspace() + "/sync/" + stamp;
  try {
    QDir().mkpath(workspace() + "/sync");
    QSettings settings(workspace() + "/runtime.ini", QSettings::IniFormat);
    auto author = settings.value("export/author", qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME"))).toString();
    ExportService::write(package, "Sync to production " + stamp, author, folder,
                         {{"destination", QJsonObject{{"host", profile.sshHost}, {"database", profile.database}}}});
  } catch (std::exception const& e) { QMessageBox::warning(parent, "Sync to Production", QString::fromUtf8(e.what())); return; }

  SyncDialog progress(parent);
  ProductionSync::Outcome outcome;
  auto thread = QThread::create([&] {
    try {
      outcome = ProductionSync::run(profile, password, package, folder, [&progress](Step step, State state, QString const& detail) {
        QMetaObject::invokeMethod(&progress, [&progress, step, state, detail] { progress.step(step, state, detail); }, Qt::QueuedConnection);
      });
    } catch (std::exception const& e) { outcome.error = QString::fromUtf8(e.what()); }
  });
  QObject::connect(thread, &QThread::finished, &progress, [&] { progress.finish(outcome, folder); });
  thread->start();
  progress.exec();
  thread->wait(); delete thread;

  if (outcome.ok) {
    try { tracker->markSynced(package.changes.mid(0, package.tracked)); }
    catch (std::exception const& e) {
      QMessageBox::warning(parent, "Sync to Production", "Production is up to date, but the local list could not be updated: "
                                                         + QString::fromUtf8(e.what()) + "\nClear the synced entries by hand.");
    }
  } else if (outcome.error.contains("rejected user")) {
    PasswordStore::forget(profile); // asked again next time
  }
}
}
