#pragma once
#include "Services.hpp"
#include <QWidget>
#include <functional>
#include <optional>
class QLabel;
class QToolButton;
class QWidget;
namespace Noggit::Creator {
// The creature editor's NPC card (bottom panel): the NPC chosen in the list or in the world, and every
// Creator action as a button -- design or clone an NPC, place it with a click, its dialogue, quests and
// quest chain, its loot, shop and training, and testing in game. Shows state only; MapView performs the actions.
class NpcStudio final : public QWidget {
public:
  struct Actions {
    std::function<void()> newHumanoid, newCreature, clone, edit, place, quests, chain, testAtNpc, testAtSpot;
    std::function<void()> duplicatePlacement, deletePlacement, locatePlacement, patrol;
    std::function<void()> loot, vendor, trainer, dialogue;
    std::function<void()> items, spells;
  };
  struct Npc {
    Id entry = 0;
    QString name, details;
    bool own = false;     // made in Noggit: can be edited and placed
    bool placed = false;  // has a placement in the loaded world (to test at)
  };
  NpcStudio(Actions actions, QWidget* parent = nullptr);
  void setNpc(std::optional<Npc> npc);
  void setPlacement(std::optional<Id> guid);
private:
  void refresh();
  Actions _actions;
  std::optional<Npc> _npc;
  std::optional<Id> _placement;
  QLabel *_name = nullptr, *_details = nullptr, *_badge = nullptr, *_hint = nullptr;
  QToolButton *_clone = nullptr, *_edit = nullptr, *_place = nullptr, *_quests = nullptr, *_chain = nullptr, *_testNpc = nullptr;
  QToolButton *_loot = nullptr, *_vendor = nullptr, *_trainer = nullptr, *_dialogue = nullptr;
  QWidget* _placementRow = nullptr;
};
}
