#pragma once
#include "Services.hpp"
class QWidget;
class World;
namespace Noggit::Creator {
// Return true only after a successful save. All persistence belongs to the services.
bool createNpc(QWidget* parent, World* world, Position const& position, Id* spawn);
bool editNpc(QWidget* parent, World* world, Id entry, Position const& position);
bool editQuest(QWidget* parent, Id npc = 0);
}
