#pragma once
#include <QString>
class QWidget;
namespace Noggit::Creator {
// Asks for a name and password and creates a local login account.
// Returns the account name, or an empty string when cancelled.
QString createLocalAccount(QWidget* parent);
}
