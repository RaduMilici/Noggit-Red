#pragma once
#include <QString>
#include <functional>
class QWidget;
namespace Noggit::Creator {
// A short non-blocking message at the bottom of `host` (the 3D view), with an optional action button
// ("Tree removed · Undo"). Newer toasts stack above older ones; each fades after `ms`.
void toast(QWidget* host, QString const& text, QString const& action = {}, std::function<void()> onAction = {}, int ms = 5000);
}
