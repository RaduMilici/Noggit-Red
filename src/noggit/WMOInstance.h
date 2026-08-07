// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <noggit/SceneObject.hpp>
#include <math/ray.hpp>
#include <noggit/WMO.h>
#include <noggit/ContextObject.hpp>

#include <cstdint>
#include <set>

struct ENTRY_MODF;

namespace Noggit::Rendering
{
  class LiquidTextureManager;
  class WorldRender;
}

class WMOInstance : public SceneObject
{
public:
  scoped_wmo_reference wmo;
  std::map<int, std::pair<glm::vec3, glm::vec3>> group_extents;
  uint16_t mFlags;
  uint16_t mUnknown;
  uint16_t mNameset; 

  uint16_t doodadset() const { return _doodadset; }
  void change_doodadset(uint16_t doodad_set);

  [[nodiscard]]
  std::map<int, std::pair<glm::vec3, glm::vec3>> const& getGroupExtents() { _update_group_extents = true; ensureExtents(); return group_extents; }

private:
  void update_doodads();

  uint16_t _doodadset;

  std::map<uint32_t, std::vector<wmo_doodad_instance>> _doodads_per_group;
  bool _need_doodadset_update = true;
  bool _update_group_extents = false;

  // Self-invalidating extents cache (perf 2026-08-07). recalcExtents() used to fully rebuild the world AABB
  // + every group AABB on EVERY call, and it is called for all loaded WMOs every frame
  // (camera_is_inside_wmo / collect_camera_fog / the WMO pass) -> ~5.45ms/frame recomputing static data.
  // We now skip the rebuild when pos/dir/scale are unchanged since the last compute; any editor move/rotate/
  // scale changes one of them, so the cache self-invalidates with no per-edit-path bookkeeping. scale=-1 is
  // the "never computed" sentinel that forces the first real compute. _group_extents_computed tracks whether
  // the FULL (all-group) extents pass has run, since getExtents() only rebuilds skybox groups.
  glm::vec3 _extents_cache_pos = glm::vec3(0.f);
  glm::vec3 _extents_cache_dir = glm::vec3(0.f);
  float _extents_cache_scale = -1.f;
  bool _group_extents_computed = false;

public:
  WMOInstance(BlizzardArchive::Listfile::FileKey const& file_key, ENTRY_MODF const* d, Noggit::NoggitRenderContext context);

  explicit WMOInstance(BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context);

  WMOInstance(WMOInstance const& other) = default;
  WMOInstance& operator=(WMOInstance const& other) = default;

  WMOInstance (WMOInstance&& other)
    : SceneObject(other._type, other._context)
    , wmo (std::move (other.wmo))
    , group_extents(other.group_extents)
    , mFlags (other.mFlags)
    , mUnknown (other.mUnknown)
    , mNameset (other.mNameset)
    , _doodadset (other._doodadset)
    , _doodads_per_group(other._doodads_per_group)
    , _need_doodadset_update(other._need_doodadset_update)
  {
    std::swap (extents, other.extents);
    pos = other.pos;
    dir = other.dir;
    _context = other._context;
    uid = other.uid;

    _transform_mat = other._transform_mat;
    _transform_mat_inverted = other._transform_mat_inverted;
  }

  WMOInstance& operator= (WMOInstance&& other)
  {
    std::swap(wmo, other.wmo);
    std::swap(pos, other.pos);
    std::swap(extents, other.extents);
    std::swap(group_extents, other.group_extents);
    std::swap(dir, other.dir);
    std::swap(uid, other.uid);
    std::swap(mFlags, other.mFlags);
    std::swap(mUnknown, other.mUnknown);
    std::swap(mNameset, other.mNameset);
    std::swap(_doodadset, other._doodadset);
    std::swap(_doodads_per_group, other._doodads_per_group);
    std::swap(_need_doodadset_update, other._need_doodadset_update);
    std::swap(_transform_mat, other._transform_mat);
    std::swap(_transform_mat_inverted, other._transform_mat_inverted);
    std::swap(_context, other._context);
    return *this;
  }

  void draw ( OpenGL::Scoped::use_program& wmo_shader
            , OpenGL::program* wmo_liquid_program
            , Noggit::Rendering::LiquidTextureManager* liquid_texture_manager
            , glm::mat4x4 const& model_view
            , glm::mat4x4 const& projection
            , math::frustum const& frustum
            , const float& cull_distance
            , const glm::vec3& camera
            , bool force_box
            , bool draw_doodads
            , bool draw_fog
            , std::vector<selection_type> selection
            , int animtime
            , bool world_has_skies
            , display_mode display
            , bool no_cull = false
            , bool draw_exterior = true
            , Noggit::Rendering::WorldRender* world_renderer = nullptr // per-room (MOLR) light scoping
            );

  void intersect (math::ray const&, selection_result*, bool do_exterior = true);

  // GAMEOBJECT-owned instances aren't registered with map tiles; skip the tile-visibility gate in
  // draw() (they are frustum-culled by their gather loop instead).
  bool skip_tile_culling = false;

  // Per-frame portal visibility of this instance's groups, written by draw() (empty = all visible /
  // portal culling off). get_visible_doodads reads it in the same frame so portal-culled rooms hide
  // their doodads too, not just their geometry.
  std::vector<uint8_t> portal_group_visibility;

  void recalcExtents() override;
  void change_nameset(uint16_t name_set);
  void ensureExtents() override;
  bool finishedLoading() override { return wmo->finishedLoading(); };
  virtual void updateDetails(Noggit::Ui::detail_infos* detail_widget) override;

  [[nodiscard]]
  AsyncObject* instance_model() const override { return wmo.get(); };

  std::vector<wmo_doodad_instance*> get_visible_doodads( math::frustum const& frustum
                                                       , float const& cull_distance
                                                       , glm::vec3 const& camera
                                                       , bool draw_hidden_models
                                                       , display_mode display
                                                       );

  std::map<uint32_t, std::vector<wmo_doodad_instance>>* get_doodads(bool draw_hidden_models);
};
