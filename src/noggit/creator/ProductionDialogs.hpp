#pragma once
class QWidget;
namespace Noggit::Creator {
// Production server settings, with Test Connection and host-key verification. True when saved.
bool editProductionProfile(QWidget* parent);
// Sync to Production for every pending local change: confirmation, progress and the result.
void syncToProduction(QWidget* parent);
}
