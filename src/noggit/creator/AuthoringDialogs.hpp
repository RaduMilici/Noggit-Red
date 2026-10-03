#pragma once
#include "Services.hpp"
#include <optional>
class QWidget;
class World;
namespace Noggit::Creator {
// All persistence belongs to the services.
enum class NpcKind { Clone, Humanoid, Creature };
// Designs a new NPC (not yet placed in the world) and returns its entry; nothing when cancelled.
// Clone starts from `source` (0: search for one).
std::optional<Id> createNpc(QWidget* parent, World* world, NpcKind kind, Id source = 0);
// True after a save or a deletion.
bool editNpc(QWidget* parent, World* world, Id entry);
}
