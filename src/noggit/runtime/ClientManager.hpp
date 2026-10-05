#pragma once
#include <QString>
#include <functional>
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
  // Runs before the client starts (Creator puts its test client data in place, or takes it out for production).
  // Throwing cancels the launch.
  static inline std::function<void(Profile)> beforeLaunch;
};
}
