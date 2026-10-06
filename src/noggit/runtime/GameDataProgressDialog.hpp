#pragma once
#include <QDialog>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <functional>

namespace Noggit::Runtime {
// A modeless dialog: updating it never pumps a nested event loop. Only the
// explicit Cancel button stops downloads; closing/minimizing keeps them running.
class GameDataProgressDialog : public QDialog {
public:
  explicit GameDataProgressDialog(QWidget* parent = nullptr) : QDialog(parent) {
    setWindowTitle("Creator setup");
    setAttribute(Qt::WA_QuitOnClose, false);
    resize(480, 180);
    auto layout = new QVBoxLayout(this);
    _label = new QLabel(this); _label->setWordWrap(true); _label->setTextFormat(Qt::PlainText);
    _bar = new QProgressBar(this); _bar->setRange(0, 1000);
    _notice = new QLabel(this); _notice->setWordWrap(true); _notice->setTextFormat(Qt::PlainText);
    _cancel = new QPushButton("Cancel download", this);
    layout->addWidget(_label); layout->addWidget(_bar); layout->addWidget(_notice); layout->addWidget(_cancel);
    connect(_cancel, &QPushButton::clicked, this, [this] {
      _downloading = false; hide();
      if (canceled) canceled();
    });
  }
  std::function<void()> canceled;
  bool downloading() const { return _downloading; }
  void updateProgress(qint64 received, qint64 total) {
    if (!_downloading) {
      _downloading = true; _dismissed = false; _notice->clear(); _cancel->show(); _bar->show();
    }
    _label->setText(QString("Downloading game data: %1 / %2 MB\nFour files download at once. Completed files are kept.")
        .arg(received / (1024 * 1024)).arg(total / (1024 * 1024)));
    _bar->setValue(total ? int(received * 1000 / total) : 0);
    if (!_dismissed) show();
  }
  void updateMessage(QString const& message) { _notice->setText(message); }
  void showError(QString const& error) {
    _downloading = false; _bar->hide(); _cancel->hide();
    _label->setText("Game-data setup needs attention."); _notice->setText(error); show();
  }
  void finish() { _downloading = false; hide(); }
  void reject() override {
    // Escape or the window close button hides the UI without canceling its work.
    _dismissed = true; QDialog::reject();
  }
private:
  QLabel* _label;
  QLabel* _notice;
  QProgressBar* _bar;
  QPushButton* _cancel;
  bool _downloading = false, _dismissed = false;
};
}
