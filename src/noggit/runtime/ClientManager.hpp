#pragma once
#include <QString>
class QWidget;
namespace Noggit::Runtime {
class ClientManager {
public:
  enum class Profile { TestLocal, PlayProduction };
  static bool configure(QWidget* parent);
  static bool prepare(QWidget* parent, Profile profile);
  static void launch(Profile profile);
  static void switchRealm(QString const& executable, QString const& endpoint);
  static bool validProductionEndpoint(QString const& endpoint);
};
}
