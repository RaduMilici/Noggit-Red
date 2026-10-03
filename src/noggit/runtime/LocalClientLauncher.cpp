#include "LocalClientLauncher.hpp"
#include "ClientManager.hpp"
#include <QMessageBox>
#include <stdexcept>
namespace Noggit::Runtime {
void launchLocalClient(QWidget* parent) {
  try {if(ClientManager::prepare(parent,ClientManager::Profile::TestLocal))ClientManager::launch(ClientManager::Profile::TestLocal);}
  catch(std::exception const& e){QMessageBox::warning(parent,"Local client",e.what());}
}
}
