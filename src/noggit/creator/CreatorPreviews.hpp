#pragma once
#include "Services.hpp"
#include <noggit/ui/tools/PreviewRenderer/CreaturePreviewViewer.hpp>
#include <optional>
class World;
namespace Noggit::Creator {
// 3D previews for Creator's editors, on the creature picker's orbit camera: drag to turn, wheel to zoom.
// Each one loads its model on its next frame, so it can be set before the widget is shown.

// An NPC as the world draws it, with its helm, shoulders and weapons.
class NpcPreview final : public Ui::Tools::CreaturePreviewModelViewer {
public:
  NpcPreview(World* world, QWidget* parent = nullptr);
  void showNpc(Id entry);
protected:
  void draw() override;
private:
  World* _world;
  std::optional<std::pair<Id, NpcLook>> _pending;
};
// A GameObject display.
class ObjectPreview final : public Ui::Tools::CreaturePreviewModelViewer {
public:
  explicit ObjectPreview(QWidget* parent = nullptr);
  void display(Id display);
protected:
  void draw() override;
private:
  QString _path;
};
// An item's own model (weapons, shields, held items, helms, shoulders). hasModel() is false for items
// the game only draws as body textures.
class ItemPreview final : public Ui::Tools::CreaturePreviewModelViewer {
public:
  explicit ItemPreview(QWidget* parent = nullptr);
  void showItem(Id display, int inventoryType);
  bool hasModel() const { return _has; }
protected:
  void draw() override;
private:
  std::optional<World::CreaturePreviewAttachment> _pending;
  bool _has = false;
};
}
