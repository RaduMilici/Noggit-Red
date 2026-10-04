#include "CreatorPreviews.hpp"
#include <noggit/DBC.h>
#include <algorithm>
namespace Noggit::Creator {
namespace {
std::string modelPath(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  for (auto ext : {".mdx", ".mdl"}) if (auto at = path.rfind(ext); at != std::string::npos) path.replace(at, 4, ".m2");
  return path;
}
}
NpcPreview::NpcPreview(World* world, QWidget* parent) : CreaturePreviewModelViewer(parent), _world(world) { setMinimumSize(240, 260); }
void NpcPreview::showNpc(Id entry) {
  _pending.reset();
  try { _pending = std::make_pair(entry, CreatureService::look(entry)); } catch (...) { /* unknown NPC: the preview stays empty */ }
  update();
}
void NpcPreview::draw() {
  if (_pending && _world) {
    auto [entry, look] = *_pending; _pending.reset();
    try {
      auto display = gCreatureDisplayInfoDB.getByID(look.display);
      auto model = gCreatureModelDataDB.getByID(display.getUInt(CreatureDisplayInfoDB::ModelID));
      World::CreatureSpawnOverlay spawn;
      spawn.entry = entry; spawn.display_id = look.display; spawn.name = look.name.toStdString();
      spawn.model_path = modelPath(model.getString(CreatureModelDataDB::ModelName));
      // Same scale rule as the world: the template's scale, else the display's; times the model's own.
      float const displayScale = display.getFloat(CreatureDisplayInfoDB::CreatureModelScale);
      spawn.template_scale = look.scale > 0 ? float(look.scale) : (displayScale > 0 ? displayScale : 1.0f);
      float const modelScale = model.getFloat(CreatureModelDataDB::ModelScale);
      spawn.model_scale = modelScale > 0 ? modelScale : 1.0f;
      spawn.is_character_model = spawn.model_path.rfind("character/", 0) == 0;
      spawn.mainhand_display_id = look.mainhand; spawn.offhand_display_id = look.offhand; spawn.ranged_display_id = look.ranged;
      spawn.offhand_inventory_type = std::uint32_t(look.offhandType);
      if (!spawn.model_path.empty()) setCreatureSpawnPreview(*_world, spawn);
    } catch (...) { /* unknown look: the preview stays empty */ }
  }
  CreaturePreviewModelViewer::draw();
}
ObjectPreview::ObjectPreview(QWidget* parent) : CreaturePreviewModelViewer(parent) { setMinimumSize(220, 200); }
void ObjectPreview::display(Id id) {
  try { _path = QString::fromStdString(modelPath(gGameObjectDisplayInfoDB.getByID(id).getString(GameObjectDisplayInfoDB::ModelName))); }
  catch (...) { _path.clear(); }
  update();
}
void ObjectPreview::draw() {
  if (!_path.isEmpty()) { auto path = _path; _path.clear(); setModel(path.toStdString()); }
  CreaturePreviewModelViewer::draw();
}
ItemPreview::ItemPreview(QWidget* parent) : CreaturePreviewModelViewer(parent) { setMinimumSize(200, 180); }
void ItemPreview::showItem(Id display, int inventoryType) {
  _pending.reset(); _has = false;
  try {
    auto models = World::resolveItemDisplayModels(display, std::uint32_t(inventoryType));
    if (!models.empty()) { _pending = models.front(); _has = true; }
  } catch (...) {}
  update();
}
void ItemPreview::draw() {
  if (_pending) {
    auto item = *_pending; _pending.reset();
    try {
      setModel(item.model_path);
      if (!_model_instances.empty()) for (auto const& [type, texture] : item.texture_overrides) _model_instances.front().setReplaceTexture(type, texture);
    } catch (...) {}
  }
  CreaturePreviewModelViewer::draw();
}
}
