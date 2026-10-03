#include "AccountDialog.hpp"
#include "Services.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <stdexcept>
namespace Noggit::Creator {
QString createLocalAccount(QWidget* parent) {
  QDialog dialog(parent); dialog.setWindowTitle("Create Local Account");
  auto layout = new QVBoxLayout(&dialog);
  auto intro = new QLabel("This account is only for your local test server. Use it to log in to WoW when testing locally.");
  intro->setWordWrap(true); layout->addWidget(intro);
  auto form = new QFormLayout; layout->addLayout(form);
  auto name = new QLineEdit; name->setMaxLength(16); name->setPlaceholderText("3–16 letters or digits"); form->addRow("Account name", name);
  auto password = new QLineEdit, confirm = new QLineEdit;
  for (auto field : {password, confirm}) { field->setEchoMode(QLineEdit::Password); field->setMaxLength(16); }
  password->setPlaceholderText("4–16 characters"); form->addRow("Password", password); form->addRow("Repeat password", confirm);
  auto note = new QLabel("Names and passwords are not case-sensitive in WoW. Only the server's login hash is stored.");
  note->setWordWrap(true); layout->addWidget(note);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
  auto create = buttons->addButton("Create Account", QDialogButtonBox::AcceptRole); create->setDefault(true);
  layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QString created;
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    if (password->text() != confirm->text()) { QMessageBox::warning(&dialog, "Create Local Account", "The passwords do not match."); return; }
    try { AccountService::create(name->text(), password->text()); created = name->text().toUpper(); dialog.accept(); }
    catch (std::exception const& e) { QMessageBox::warning(&dialog, "Create Local Account", QString::fromUtf8(e.what())); }
  });
  if (dialog.exec() != QDialog::Accepted) return {};
  QMessageBox::information(parent, "Create Local Account", "Account " + created + " is ready. Log in to WoW with it when testing locally.");
  return created;
}
}
