// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/ray.hpp>
#include <ClientFile.hpp>
#include <noggit/MapHeaders.h> // ENTRY_MDDF
#include <noggit/ModelManager.h>
#include <noggit/Selection.h>
#include <noggit/SceneObject.hpp>
#include <noggit/TextureManager.h>
#include <noggit/rendering/Primitives.hpp>
#include <noggit/TileIndex.hpp>
#include <noggit/tool_enums.hpp>
#include <opengl/shader.fwd.hpp>
#include <map>
#include <optional>
#include <cstdint>
#include <vector>

namespace math { class frustum; }
class Model;
class WMOInstance;

class ModelInstance : public SceneObject
{
public:
  constexpr static float min_scale() { return 1.f / 1024.f; };
  constexpr static float max_scale() { return static_cast<float>((1 << 16) - 1) / 1024.f; };

  scoped_model_reference model;

  glm::vec3 light_color = { 1.f, 1.f, 1.f };

  // Model-wide opacity from CreatureDisplayInfo.CreatureModelAlpha (0..1; 1 = fully opaque). The client
  // draws creatures at this opacity (translucent ghosts/elementals etc.); 1.0 means no change.
  float model_alpha = 1.0f;

  // Model-wide color multiplier from aura char-proc tints (Ghost Visual's light-blue shift).
  // (1,1,1) = no change. Applied to every render pass's mesh color alongside model_alpha.
  glm::vec3 model_tint = glm::vec3(1.0f);

  // Camera-relative render anchor (jitter fix). When set, the vertex shader uses `_render_origin` as
  // the ~17000 world anchor and `_render_transform_rel` as the vertex transform RELATIVE to it (small
  // translation). Attachments (helmet/weapon following an animated parent bone) set this from a
  // DOUBLE-precision computation so the animated offset isn't quantized to the coarse float grid at
  // world scale. The plain transformMatrix() stays world-space (particles, picking) untouched.
  glm::vec3 _render_origin = glm::vec3(0.0f);
  glm::mat4x4 _render_transform_rel = glm::mat4x4(1.0f);
  bool _has_render_anchor = false;
  void setRenderAnchor(glm::vec3 const& origin, glm::mat4x4 const& rel)
  {
    _render_origin = origin;
    _render_transform_rel = rel;
    _has_render_anchor = true;
  }

  // used when flag 0x8 is set in wdt
  // longest side of an AABB transformed model's bounding box from the M2 header
  float size_cat;

  // [game-view cull 2026-08-15] Client placed-doodad (MDDF/M2) size class, ported byte-exact from
  // wow335a.exe build 12340 FUN_007bdb10: class 0..4 by the LARGEST world-AABB axis extent E of the
  // model RENDER bounding box (header.bounding_box, transformed by this instance's placement -- scale
  // and rotation included) vs sizeThresh {1,4,15,100} @DAT_00adf378. Cached in recalcExtents(); drives
  // doodadCullFade(). See twmoa-335a-client-doodad-cull.md.
  int _cull_class = 4;

  explicit ModelInstance(BlizzardArchive::Listfile::FileKey const& file_key
                         , Noggit::NoggitRenderContext context);

  explicit ModelInstance(BlizzardArchive::Listfile::FileKey const& file_key
                         , ENTRY_MDDF const*d, Noggit::NoggitRenderContext context);

  ModelInstance(ModelInstance const& other) = default;
  ModelInstance& operator= (ModelInstance const& other);

  ModelInstance (ModelInstance&& other)
    : SceneObject(other._type, other._context)
    , model (std::move (other.model))
    , light_color (other.light_color)
    , model_alpha (other.model_alpha)
    , model_tint (other.model_tint)
    , size_cat (other.size_cat)
    , _need_recalc_extents(other._need_recalc_extents)
    , _forced_anim_id(other._forced_anim_id)
    , _close_hand_main(other._close_hand_main)
    , _close_hand_off(other._close_hand_off)
    , _replace_textures(std::move(other._replace_textures))
    , _show_geosets(std::move(other._show_geosets))
    , _visible_geoset_ids(std::move(other._visible_geoset_ids))
    , _controlled_geoset_families(std::move(other._controlled_geoset_families))
  {
    pos = other.pos;
    dir = other.dir;
    scale = other.scale;
    extents[0] = other.extents[0];
    extents[1] = other.extents[1];
    _transform_mat_inverted =  other._transform_mat_inverted;
    _context = other._context;
    uid = other.uid;
  }

  ModelInstance& operator= (ModelInstance&& other) noexcept
  {
    std::swap (model, other.model);
    std::swap (pos, other.pos);
    std::swap (dir, other.dir);
    std::swap (light_color, other.light_color);
    std::swap (model_alpha, other.model_alpha);
    std::swap (model_tint, other.model_tint);
    std::swap (uid, other.uid);
    std::swap (scale, other.scale);
    std::swap (size_cat, other.size_cat);
    std::swap (_need_recalc_extents, other._need_recalc_extents);
    std::swap (_forced_anim_id, other._forced_anim_id);
    std::swap (_close_hand_main, other._close_hand_main);
    std::swap (_close_hand_off, other._close_hand_off);
    std::swap (_replace_textures, other._replace_textures);
    std::swap (_show_geosets, other._show_geosets);
    std::swap (_visible_geoset_ids, other._visible_geoset_ids);
    std::swap (_controlled_geoset_families, other._controlled_geoset_families);
    std::swap (extents, other.extents);
    std::swap(_transform_mat_inverted, other._transform_mat_inverted);
    std::swap(_context, other._context);
    return *this;
  }

  void draw_box (glm::mat4x4 const& model_view
                , glm::mat4x4 const& projection
                , bool is_current_selection
                );


  std::vector<std::tuple<int, int, int>> intersect(glm::mat4x4 const& model_view
      , math::ray const&
      , selection_result*
      , int animtime
      , bool use_collision_mesh = false // [game mode] physics: M2 collision mesh only (grass = none)
  );

  bool isInFrustum(math::frustum const& frustum);
  bool isInRenderDist(const float& cull_distance, const glm::vec3& camera, display_mode display);

  // [game-view cull 2026-08-15] Client placed-doodad distance cull+fade (wow335a.exe FUN_00791cb0),
  // byte-exact. Returns a fade alpha in [0,1]; 0.0 == hard-culled (skip the draw entirely). Uses the
  // cached _cull_class, the world-AABB centre, and the per-class {cullFar,fadeBand} tables.
  // environment_detail = client environmentDetail CVar (default 1.0, @0x009e1340), scales classes 1..3.
  // See twmoa-335a-client-doodad-cull.md.
  float doodadCullFade(glm::vec3 const& camera, float environment_detail);

  // [game-view cull 2026-08-15] Single source of truth for the client per-class doodad cull tables
  // (FUN_007bdb10/FUN_00791cb0). Given a class 0..4 and the environmentDetail CVar, yields the hard
  // cull distance and the fade band width (fadeStart = cull_far - fade_band). Used by doodadCullFade()
  // (individual path) AND WorldRender's persistent per-tile bucket cull. cullFar @DAT_00adf364 =
  // {30,100,200,750,1250}; fadeBand @DAT_00adf38c = {5,10,15,20,50}; envDetail clamp(0.5,1.5) scales
  // classes 1,2,3 only. See twmoa-335a-client-doodad-cull.md.
  static void doodadCullParams(int cull_class, float environment_detail,
                               float& cull_far, float& fade_band);

  [[nodiscard]]
  virtual glm::vec3 const& get_pos() const { return pos; }

  void recalcExtents() override;
  void ensureExtents() override;
  bool finishedLoading() override { return model->finishedLoading(); };
  glm::vec3* getExtents();

  [[nodiscard]]
  virtual bool isWMODoodad() const { return false; };

  // Ground footprint radius (world units) = the model's horizontal bounding-box extent * scale. Matches
  // the in-game selection-ring size, which follows the unit's footprint -- NOT the bounding SPHERE
  // (model->rad), which includes the full height and over-sizes tall creatures. M2 space is Z-up, so the
  // footprint is the X/Y extent. Used for the creature/gameobject selection circles (draw + pick).
  [[nodiscard]]
  float selectionRingRadius() const;

  [[nodiscard]]
  AsyncObject* instance_model() const override { return model.get(); };

  void setReplaceTexture(std::size_t texture_type, std::string const& filename);
  void setGeosetVisibility(std::vector<bool> show_geosets) { _show_geosets = std::move(show_geosets); }
  void setGeosetSelections(std::vector<std::uint16_t> visible_geoset_ids,
                           std::vector<std::uint16_t> controlled_geoset_families)
  {
    _visible_geoset_ids = std::move(visible_geoset_ids);
    _controlled_geoset_families = std::move(controlled_geoset_families);
  }

  [[nodiscard]]
  std::map<std::size_t, scoped_blp_texture_reference> const& replaceTextures() const { return _replace_textures; }
  [[nodiscard]]
  std::vector<bool> const& geosetVisibility() const { return _show_geosets; }
  [[nodiscard]]
  std::vector<std::uint16_t> const& visibleGeosetIds() const { return _visible_geoset_ids; }
  [[nodiscard]]
  std::vector<std::uint16_t> const& controlledGeosetFamilies() const { return _controlled_geoset_families; }
  [[nodiscard]]
  bool isGeosetFamilyControlled(std::uint16_t geoset_family) const
  {
    return std::find(_controlled_geoset_families.begin(), _controlled_geoset_families.end(), geoset_family)
      != _controlled_geoset_families.end();
  }
  [[nodiscard]]
  bool isGeosetIdVisible(std::uint16_t geoset_id) const
  {
    return std::find(_visible_geoset_ids.begin(), _visible_geoset_ids.end(), geoset_id) != _visible_geoset_ids.end();
  }

  void updateDetails(Noggit::Ui::detail_infos* detail_widget) override;

  [[nodiscard]]
  std::uint32_t gpuTransformUid() const { return _gpu_transform_uid; }

  void setForcedAnimationId(int anim_id) { _forced_anim_id = anim_id; }
  [[nodiscard]] int forcedAnimationId() const { return _forced_anim_id; }

  // Weapon grip: overlay the HandsClosed pose onto the finger bones only (fist closes around a held weapon)
  // while the body keeps its normal idle. PER-HAND (2026-07-25): main = right/mainhand, off = left/offhand,
  // so an empty hand stays open. See Model::applyHandGripOverlay.
  // [game mode] lower-body twist (radians): legs+hips rotated toward the strafe direction, torso
  // counter-rotated to keep facing forward. Copied onto the shared Model just before this
  // instance's animate(), like the hand-grip overlay. 0 for everything but the player character.
  float lower_body_twist = 0.0f;
  // [game mode] playback-rate scale for this instance's forced animation (see Model::_anim_time_scale)
  float anim_time_scale = 1.0f;

  // CLIENT-PARITY per-instance bone palette (2026-08-14). The WoW client stores each CM2Model's bone
  // matrices ON the instance, computed once per frame, and BOTH the color pass and the shadow depth
  // pass read that same palette (it never separately animates shadows). noggit historically shared ONE
  // bone buffer per Model across all instances, so a unit's shadow / attachment placement picked up the
  // LAST-drawn instance's pose. This palette makes each unit own its bones: the per-frame creature
  // animate fills it, and the color pass, the shadow pass, and attachment (helmet/shoulder/weapon)
  // placement all restore from it. Empty = not animated / not yet computed (fallback: animate in-draw).
  std::vector<glm::mat4x4> _animation_bones;

  void setCloseHandMain(bool v) { _close_hand_main = v; }
  void setCloseHandOff(bool v) { _close_hand_off = v; }
  [[nodiscard]] bool closeHandMain() const { return _close_hand_main; }
  [[nodiscard]] bool closeHandOff() const { return _close_hand_off; }

protected:
  bool _need_recalc_extents = true;
  bool _need_gpu_transform_update = true;
  std::uint32_t _gpu_transform_uid;
  int _forced_anim_id = -1;
  bool _close_hand_main = false;
  bool _close_hand_off = false;
  std::map<std::size_t, scoped_blp_texture_reference> _replace_textures;
  std::vector<bool> _show_geosets;
  std::vector<std::uint16_t> _visible_geoset_ids;
  std::vector<std::uint16_t> _controlled_geoset_families;

};

class wmo_doodad_instance : public ModelInstance
{
public:
  glm::quat doodad_orientation;
  glm::vec3 world_pos;

  explicit wmo_doodad_instance(BlizzardArchive::Listfile::FileKey const& file_key
      , BlizzardArchive::ClientFile* f
      , Noggit::NoggitRenderContext context );

  wmo_doodad_instance(wmo_doodad_instance const& other)
  : ModelInstance(other)
  , doodad_orientation(other.doodad_orientation)
  , world_pos(other.world_pos)
  , _need_matrix_update(other._need_matrix_update)
  {

  };

  wmo_doodad_instance& operator= (wmo_doodad_instance const& other) = delete;

  wmo_doodad_instance(wmo_doodad_instance&& other) noexcept
    : ModelInstance(reinterpret_cast<ModelInstance&&>(other))
    , doodad_orientation(other.doodad_orientation)
    , world_pos(other.world_pos)
    , _need_matrix_update(other._need_matrix_update)
  {
  }

  wmo_doodad_instance& operator= (wmo_doodad_instance&& other)
  {
    ModelInstance::operator= (reinterpret_cast<ModelInstance&&>(other));
    std::swap (doodad_orientation, other.doodad_orientation);
    std::swap (world_pos, other.world_pos);
    std::swap (_need_matrix_update, other._need_matrix_update);
    return *this;
  }

  [[nodiscard]]
  bool need_matrix_update() const { return _need_matrix_update; }

  void update_transform_matrix_wmo(WMOInstance* wmo);

  [[nodiscard]]
  glm::vec3 const& get_pos() const override { return world_pos; };

  [[nodiscard]]
  bool isWMODoodad() const override { return true; };

protected:
  // to avoid redefining recalcExtents
  void updateTransformMatrix() override { }

private:
  bool _need_matrix_update = true;
};
