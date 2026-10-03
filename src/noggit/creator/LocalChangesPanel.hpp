#pragma once
class QMainWindow;
namespace Noggit::Creator {
// Status-bar "Local changes" button opening the pending change list: test locally, sync to production, export.
void addLocalChangesPanel(QMainWindow* window);
}
