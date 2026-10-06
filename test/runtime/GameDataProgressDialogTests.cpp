#include "../../src/noggit/runtime/GameDataProgressDialog.hpp"
#include <QApplication>
#include <iostream>
#include <stdexcept>

void check(bool value, char const* message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    Noggit::Runtime::GameDataProgressDialog dialog;
    int cancellations = 0;
    dialog.canceled = [&] { ++cancellations; };
    dialog.updateProgress(99, 100); app.processEvents();
    check(dialog.isVisible(), "Progress must stay visible near completion");
    dialog.updateProgress(100, 100);
    check(dialog.isVisible(), "Byte completion must not auto-close before verification");
    dialog.updateMessage("Retrying automatically");
    check(dialog.isVisible(), "Retry must keep progress visible");
    dialog.close(); app.processEvents();
    check(cancellations == 0, "Closing the window must not cancel download");
    dialog.updateProgress(50, 100);
    check(!dialog.isVisible(), "Hidden background download must not reopen every update");
    dialog.showError("Disk full");
    check(dialog.isVisible(), "Error must remain visible");
    dialog.finish();
    check(!dialog.isVisible(), "Successful finish must hide dialog");
    dialog.updateProgress(0, 100);
    check(dialog.isVisible(), "A new download must show progress again");
    auto cancel = dialog.findChild<QPushButton*>();
    check(cancel && cancel->isVisible(), "Explicit Cancel must be available");
    cancel->click();
    check(cancellations == 1 && !dialog.isVisible(), "Explicit Cancel must stop work exactly once");
    std::cout << "Game-data progress window checks passed\n";
  } catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
  return 0;
}
