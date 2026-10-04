#pragma once
#include "LootService.hpp"
class QWidget;
class World;
namespace Noggit::Ui::Content { class ContentSession; }
namespace Noggit::Creator {
// Visual editors for what NPCs and objects give players. Original content opens read-only, with a way
// to copy it to your own. Each returns true when something was saved.
bool editLoot(QWidget* parent, World* world, LootOwner const& owner);
bool editVendor(QWidget* parent, World* world, Id npc);
bool editTrainer(QWidget* parent, World* world, Id npc);
// What an NPC says and how players answer, as a conversation tree. `session` gives the quest, item and
// spell pickers and the cursor position (teleport destinations).
bool editDialogue(QWidget* parent, World* world, Id npc, Ui::Content::ContentSession& session);
}
