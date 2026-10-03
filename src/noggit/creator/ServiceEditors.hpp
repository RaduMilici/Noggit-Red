#pragma once
#include "LootService.hpp"
class QWidget;
class World;
namespace Noggit::Creator {
// Visual editors for what NPCs and objects give players. Original content opens read-only, with a way
// to copy it to your own. Each returns true when something was saved.
bool editLoot(QWidget* parent, World* world, LootOwner const& owner);
bool editVendor(QWidget* parent, World* world, Id npc);
bool editTrainer(QWidget* parent, World* world, Id npc);
}
