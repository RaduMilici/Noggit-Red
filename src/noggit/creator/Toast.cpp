#include "Toast.hpp"
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QTimer>
namespace Noggit::Creator {
namespace {
constexpr int margin = 24, gap = 8;
void layoutToasts(QWidget* host) {
  int bottom = host->height() - margin;
  auto toasts = host->findChildren<QFrame*>("creatorToast", Qt::FindDirectChildrenOnly);
  // Newest at the bottom, older ones pushed up.
  for (int i = toasts.size() - 1; i >= 0; --i) {
    auto t = toasts[i]; t->adjustSize();
    t->move((host->width() - t->width()) / 2, bottom - t->height());
    bottom -= t->height() + gap;
  }
}
}
void toast(QWidget* host, QString const& text, QString const& action, std::function<void()> onAction, int ms) {
  if (!host) return;
  auto frame = new QFrame(host);
  frame->setObjectName("creatorToast");
  frame->setStyleSheet("#creatorToast { background: rgba(28,28,32,235); border: 1px solid rgba(255,255,255,40); border-radius: 8px; }"
                       "#creatorToast QLabel { color: white; font-size: 10pt; }"
                       "#creatorToast QPushButton { color: #8ec5ff; background: transparent; border: none; font-weight: bold; font-size: 10pt; padding: 0 4px; }"
                       "#creatorToast QPushButton:hover { color: white; text-decoration: underline; }");
  auto layout = new QHBoxLayout(frame); layout->setContentsMargins(14, 8, 10, 8); layout->setSpacing(12);
  auto label = new QLabel(text, frame); label->setTextFormat(Qt::PlainText);
  layout->addWidget(label);
  QPointer<QFrame> guard(frame);
  auto dismiss = [guard, host] {
    if (!guard || guard->property("closing").toBool()) return; // the button and the timer can both close it
    guard->setProperty("closing", true);
    auto effect = new QGraphicsOpacityEffect(guard.data()); guard->setGraphicsEffect(effect);
    auto fade = new QPropertyAnimation(effect, "opacity", guard.data());
    fade->setDuration(250); fade->setStartValue(1.0); fade->setEndValue(0.0);
    QObject::connect(fade, &QPropertyAnimation::finished, guard.data(), [guard, host] {
      if (guard) { guard->hide(); guard->deleteLater(); }
      QTimer::singleShot(0, host, [host] { layoutToasts(host); });
    });
    fade->start(QAbstractAnimation::DeleteWhenStopped);
  };
  if (!action.isEmpty()) {
    auto button = new QPushButton(action, frame); button->setCursor(Qt::PointingHandCursor);
    layout->addWidget(button);
    QObject::connect(button, &QPushButton::clicked, frame, [onAction, dismiss] { if (onAction) onAction(); dismiss(); });
  }
  frame->show(); frame->raise();
  layoutToasts(host);
  QTimer::singleShot(ms, frame, dismiss);
}
}
