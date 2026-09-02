// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <math/bounding_box.hpp>
#include <noggit/Log.h>
#include <noggit/MapHeaders.h>
#include <noggit/Misc.h> // checkinside
#include <noggit/ModelInstance.h>
#include <noggit/WMO.h> // WMO
#include <noggit/MapTile.h>
#include <noggit/WMOInstance.h>
#include <noggit/rendering/Primitives.hpp>
#include <opengl/scoped.hpp>

#include <sstream>

WMOInstance::WMOInstance(BlizzardArchive::Listfile::FileKey const& file_key, ENTRY_MODF const* d, Noggit::NoggitRenderContext context)
  : SceneObject(SceneObjectTypes::eWMO, context)
  , wmo(file_key, context)
  , mFlags(d->flags)
  , mUnknown(d->unknown), mNameset(d->nameSet)
  , _doodadset(d->doodadSet)
{
  pos = glm::vec3(d->pos[0], d->pos[1], d->pos[2]);
  dir = math::degrees::vec3{math::degrees(d->rot[0])._, math::degrees(d->rot[1])._, math::degrees(d->rot[2])._ };

  uid = d->uniqueID;

  extents[0] = glm::vec3(d->extents[0][0], d->extents[0][1], d->extents[0][2]);
  extents[1] = glm::vec3(d->extents[1][0], d->extents[1][1], d->extents[1][2]);

  updateTransformMatrix();
  change_doodadset(_doodadset);
}

WMOInstance::WMOInstance(BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context)
  : SceneObject(SceneObjectTypes::eWMO, context)
  , wmo(file_key, context)
  , mFlags(0)
  , mUnknown(0)
  , mNameset(0)
  , _doodadset(0)
{
  change_doodadset(_doodadset);
  pos = glm::vec3(0.0f, 0.0f, 0.0f);
  dir = math::degrees::vec3(math::degrees(0)._, math::degrees(0)._, math::degrees(0)._);
  uid = 0;
  _context = context;

  updateTransformMatrix();
}


void WMOInstance::draw ( OpenGL::Scoped::use_program& wmo_shader
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
                       , bool no_cull
                       , bool draw_exterior
                       , Noggit::Rendering::WorldRender* world_renderer
                       )
{
  if (!wmo->finishedLoading() || wmo->loading_failed())
  {
    return;
  }

  const uint id = this->uid;
  bool const is_selected = selection.size() > 0 &&
                           std::find_if(selection.begin(), selection.end(),
                                        [id](selection_type type)
                                        {
                                          return var_type(type) == typeid(selected_object_type)
                                          && std::get<selected_object_type>(type)->which() == SceneObjectTypes::eWMO
                                          && static_cast<WMOInstance*>(std::get<selected_object_type>(type))->uid == id;
                                        }) != selection.end();

  {
    unsigned region_visible = 0;

    if (!no_cull)
    {
      for (auto& tile : getTiles())
      {
        if (tile->renderer()->objectsFrustumCullTest() && !tile->renderer()->isOccluded())
        {
          region_visible = tile->renderer()->objectsFrustumCullTest();

          if (tile->renderer()->objectsFrustumCullTest() > 1)
            break;
        }
      }
    }

    if (skip_tile_culling && !no_cull)
    {
      // not tile-registered (gameobject WMO): frustum-only
      if (!frustum.intersects(extents[1], extents[0]))
      {
        return;
      }
    }
    else if (!no_cull && (!region_visible || (region_visible <= 1 && !frustum.intersects(extents[1], extents[0]))))
    {
      return;
    }

    wmo_shader.uniform("transform", _transform_mat);

    wmo->renderer()->draw( wmo_shader
              , wmo_liquid_program
              , liquid_texture_manager
              , model_view
              , projection
              , _transform_mat
              , is_selected && !_grouped
              , frustum
              , cull_distance
              , camera
              , draw_doodads
              , draw_fog
              , animtime
              , world_has_skies
              , display
              , !draw_exterior
              , world_renderer
              , &portal_group_visibility
              );
  }

  if (force_box || is_selected)
  {
    gl.enable(GL_BLEND);
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glm::vec4 color = force_box || _grouped ? glm::vec4(0.5f, 0.5f, 1.0f, 0.5f)
        : glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);

    Noggit::Rendering::Primitives::WireBox::getInstance(_context).draw(model_view
       , projection
       , glm::mat4x4(glm::mat4x4(1))
       , color
       , extents[0]
       , extents[1]);
  }
}

void WMOInstance::intersect (math::ray const& ray, selection_result* results, bool do_exterior, float max_dist)
{
  if (!ray.intersect_bounds (extents[0], extents[1]))
  {
    return;
  }

  math::ray subray(_transform_mat_inverted, ray);

  // WMO instances are unscaled (3.3.5), so local-space distances equal world distances and
  // max_dist passes through unchanged for the per-group reach cull.
  for (auto&& result : wmo->intersect(subray, do_exterior, max_dist))
  {
    results->emplace_back (result, this);
  }
}

std::optional<std::pair<float, int>> WMOInstance::groundHit(math::ray const& ray, float max_dist)
{
  // [doc 38 footsteps] world-ray -> model space like intersect(); unscaled instances keep
  // distances 1:1. Returns {world distance, the hit face's authored TerrainType row}.
  if (!ray.intersect_bounds (extents[0], extents[1]))
  {
    return std::nullopt;
  }
  if (!wmo->finishedLoading() || wmo->loading_failed())
  {
    return std::nullopt;
  }
  math::ray subray(_transform_mat_inverted, ray);
  return wmo->groundHit(subray, max_dist);
}

std::optional<float> WMOInstance::liquidHeightAt (glm::vec3 const& world_pos, int* out_liquid_id)
{
  if (!wmo->finishedLoading() || wmo->loading_failed())
  {
    return std::nullopt;
  }
  if (world_pos.x < extents[0].x || world_pos.x > extents[1].x
      || world_pos.z < extents[0].z || world_pos.z > extents[1].z)
  {
    return std::nullopt;
  }
  glm::vec3 const local(_transform_mat_inverted * glm::vec4(world_pos, 1.0f));
  std::optional<float> best;
  for (auto const& group : wmo->groups)
  {
    int group_liquid_id = 0;
    if (auto const h = group.liquidHeightAtLocal(local, &group_liquid_id))
    {
      glm::vec3 const world_surface(_transform_mat * glm::vec4(local.x, *h, local.z, 1.0f));
      if (!best || world_surface.y > *best)
      {
        best = world_surface.y;
        if (out_liquid_id)
        {
          *out_liquid_id = group_liquid_id;
        }
      }
    }
  }
  return best;
}

void WMOInstance::intersect_doodads (glm::mat4x4 const& model_view, math::ray const& ray, selection_result* results, int animtime, float max_dist)
{
  if (!ray.intersect_bounds (extents[0], extents[1]))
  {
    return;
  }

  for (auto& [group_index, doodads] : _doodads_per_group)
  {
    for (auto& doodad : doodads)
    {
      if (!doodad.model->finishedLoading() || doodad.model->loading_failed())
      {
        continue;
      }
      // [perf 2026-08-17] Physics probes (game-mode collision) collide ONLY with a doodad's dedicated M2
      // collision mesh; a doodad that ships none (decorations, furniture, clutter -- the large majority in
      // a city WMO) is walk-through, so skip it BEFORE the transform + sphere-reject + intersect work. This
      // is behaviour-identical (those doodads already contributed no collision) and removes the per-ray cost
      // of a WMO's hundreds of non-colliding doodads (measured ~3.6ms/frame). Editor picking (max_dist==0)
      // still walks every doodad's render geometry so they stay clickable.
      if (max_dist > 0.0f && doodad.model->_collision_indices.empty())
      {
        continue;
      }
      if (doodad.need_matrix_update())
      {
        doodad.update_transform_matrix_wmo(this);
      }
      // short physics probes: sphere-reject doodads farther than max_dist from the probe origin --
      // a city WMO always contains the player, so without this every probe ray walks EVERY doodad
      // in it (the game-mode framerate hit). 0 = unlimited (editor picking).
      if (max_dist > 0.0f)
      {
        glm::vec3 const world_pos(doodad.transformMatrix()[3]);
        if (glm::distance(world_pos, ray.origin()) - doodad.model->rad * doodad.scale > max_dist)
        {
          continue;
        }
      }
      // per-doodad bbox reject + per-triangle test happen inside (instance-local subray).
      // max_dist > 0 marks a PHYSICS probe -> collision-mesh only (plants pass through);
      // unlimited (editor picking) keeps the render geometry.
      doodad.intersect(model_view, ray, results, animtime, max_dist > 0.0f);
    }
  }
}

void WMOInstance::collect_colliding_doodads_near (glm::vec3 const& center, float radius,
                                                  std::vector<wmo_doodad_instance*>& out)
{
  for (auto& [group_index, doodads] : _doodads_per_group)
  {
    for (auto& doodad : doodads)
    {
      // Same eligibility as intersect_doodads's PHYSICS path (max_dist > 0): loaded, with a dedicated
      // collision mesh (decorations/furniture ship none and are walk-through), within reach of the centre.
      if (!doodad.model->finishedLoading() || doodad.model->loading_failed()
          || doodad.model->_collision_indices.empty())
      {
        continue;
      }
      if (doodad.need_matrix_update())
      {
        doodad.update_transform_matrix_wmo(this);
      }
      glm::vec3 const world_pos(doodad.transformMatrix()[3]);
      if (glm::distance(world_pos, center) - doodad.model->rad * doodad.scale > radius)
      {
        continue;
      }
      out.push_back(&doodad);
    }
  }
}

void WMOInstance::ensureExtents()
{
  recalcExtents();
  // TODO: optimize
}

void WMOInstance::updateDetails(Noggit::Ui::detail_infos* detail_widget)
{
  std::stringstream select_info;

  select_info << "<b>filename: </b>" << wmo->file_key().filepath()
    // << "<br><b>FileDataID: </b>" << wmo->file_key().fileDataID() not in wrath
    << "<br><b>unique ID: </b>" << uid
    << "<br><b>position X/Y/Z: </b>{" << pos.x << ", " << pos.y << ", " << pos.z << "}"
    << "<br><b>rotation X/Y/Z: </b>{" << dir.x << ", " << dir.y << ", " << dir.z << "}"
    << "<br><b>WMO Id: </b>" << wmo->WmoId
    << "<br><b>doodad set: </b>" << doodadset()
    << "<br><b>name set: </b>" << mNameset

    << "<br><b>server position X/Y/Z: </b>{" << (ZEROPOINT - pos.z) << ", " << (ZEROPOINT - pos.x) << ", " << pos.y << "}"
    << "<br><b>server orientation:  </b>" << fabs(2 * glm::pi<float>() - glm::pi<float>() / 180.0 * (float(dir.y) < 0 ? fabs(float(dir.y)) + 180.0 : fabs(float(dir.y) - 180.0)))

    << "<br><b>textures used: </b>" << wmo->textures.size()
    << "<span>";

  for (unsigned j = 0; j < wmo->textures.size(); j++)
  {
    bool stuck = !wmo->textures[j]->finishedLoading();
    bool error = wmo->textures[j]->finishedLoading() && !wmo->textures[j]->is_uploaded();

    select_info << "<br> ";

    if (stuck)
      select_info << "<font color=\"Orange\">";

    if (error)
      select_info << "<font color=\"Red\">";

    select_info  << "<b>" << (j + 1) << ":</b> " << wmo->textures[j]->file_key().stringRepr();

    if (stuck || error)
      select_info << "</font>";
  }

  select_info << "<br></span>";

  detail_widget->setText(select_info.str());
}

void WMOInstance::recalcExtents()
{
  // keep the old extents since they are saved in the adt
  if (wmo->loading_failed() || !wmo->finishedLoading())
  {
    return;
  }

  // [perf 2026-08-07] Self-invalidating cache: the full rebuild below (matrix-transform 8 corners per group,
  // heap allocs, update_doodads) ran on EVERY call, and camera_is_inside_wmo/collect_camera_fog/the WMO pass
  // call this for all ~236 loaded WMOs every frame = ~5.45ms/frame recomputing values that never change
  // (WMOs are static after load). Skip the rebuild when the transform is unchanged and the requested group
  // extents are already built. Change is detected on pos/dir/scale, which the editor mutates on any move/
  // rotate/scale -> no edit path can leave a stale AABB, and no per-edit bookkeeping is needed.
  bool const transform_changed = !(_extents_cache_scale == scale
                                   && _extents_cache_pos == pos
                                   && _extents_cache_dir == dir);
  if (transform_changed)
  {
    _group_extents_computed = false; // any cached group AABBs are now stale
  }
  bool const need_groups = _update_group_extents && !_group_extents_computed;
  if (!transform_changed && !need_groups)
  {
    _update_group_extents = false; // request satisfied from the cache
    return;
  }

  updateTransformMatrix();
  update_doodads();

  std::vector<glm::vec3> points;

  glm::vec3 wmo_min(misc::transform_model_box_coords(wmo->extents[0]));
  glm::vec3 wmo_max(misc::transform_model_box_coords(wmo->extents[1]));

  auto&& root_points = math::aabb(wmo_min, wmo_max).all_corners(); 
  auto adjustedPoints = std::vector<glm::vec3>();

  for (auto const& point : root_points)
  {
    adjustedPoints.push_back(_transform_mat * glm::vec4(point, 1.f));
  }

  points.insert(points.end(), adjustedPoints.begin(), adjustedPoints.end());

  for (int i = 0; i < (int)wmo->groups.size(); ++i)
  {
    auto const& group = wmo->groups[i];

    auto&& group_points = math::aabb(group.BoundingBoxMin, group.BoundingBoxMax).all_corners();
    auto adjustedGroupPoints = std::vector<glm::vec3>();

    for (auto const& point : group_points)
    {
      adjustedGroupPoints.push_back(_transform_mat * glm::vec4(point, 1.f));
    }


    points.insert(points.end(), adjustedGroupPoints.begin(), adjustedGroupPoints.end());

    if (group.has_skybox() || _update_group_extents)
    {
      math::aabb const group_aabb(adjustedGroupPoints);

      group_extents[i] = {group_aabb.min, group_aabb.max};
    }
  }
  if (_update_group_extents)
  {
    _group_extents_computed = true; // this pass filled ALL group AABBs, not just skybox groups
  }
  _update_group_extents = false;

  math::aabb const wmo_aabb(points);

  extents[0] = wmo_aabb.min;
  extents[1] = wmo_aabb.max;

  // Remember the transform we just built for; the next call with an unchanged transform is a cache hit.
  _extents_cache_pos = pos;
  _extents_cache_dir = dir;
  _extents_cache_scale = scale;
}

void WMOInstance::change_nameset(uint16_t name_set)
{
    mNameset = name_set;
}

void WMOInstance::change_doodadset(uint16_t doodad_set)
{
  if (!wmo->finishedLoading())
  {
    _need_doodadset_update = true;
    return;
  }

  // don't set an invalid doodad set
  if (doodad_set >= wmo->doodadsets.size())
  {
    return;
  }

  _doodadset = doodad_set;
  _doodads_per_group = wmo->doodads_per_group(_doodadset);
  _need_doodadset_update = false;

  update_doodads();
}

void WMOInstance::update_doodads()
{
  for (auto& group_doodads : _doodads_per_group)
  {
    for (auto& doodad : group_doodads.second)
    {
      if (!doodad.need_matrix_update())
        continue;

      doodad.update_transform_matrix_wmo(this);
    }
  }
}

std::vector<wmo_doodad_instance*> WMOInstance::get_visible_doodads
  ( math::frustum const& frustum
  , float const& cull_distance
  , glm::vec3 const& camera
  , bool draw_hidden_models
  , display_mode display
  )
{
  std::vector<wmo_doodad_instance*> doodads;

  if (!wmo->finishedLoading() || wmo->loading_failed())
  {
    return doodads;
  }

  if (_need_doodadset_update)
  {
    change_doodadset(_doodadset);
  }

  if (!wmo->is_hidden() || draw_hidden_models)
  {
    // Portal culling parity for props: groups the last draw() portal-culled hide their doodads too.
    bool const use_portal_vis = portal_group_visibility.size() == wmo->groups.size();

    for (int i = 0; i < wmo->groups.size(); ++i)
    {
      if (use_portal_vis && !portal_group_visibility[i])
      {
        continue;
      }

      if (wmo->groups[i].is_visible(_transform_mat, frustum, cull_distance, camera, display))
      {
        for (auto& doodad : _doodads_per_group[i])
        {
          if (doodad.need_matrix_update())
          {
            doodad.update_transform_matrix_wmo(this);
          }

          doodads.push_back(&doodad);
        }
      }
    }
  }

  return doodads;
}

std::map<uint32_t, std::vector<wmo_doodad_instance>>* WMOInstance::get_doodads(bool draw_hidden_models)
{

  if (!wmo->finishedLoading() || wmo->loading_failed())
  {
    return nullptr;
  }

  if (_need_doodadset_update)
  {
    change_doodadset(_doodadset);
  }

  if (wmo->is_hidden() && !draw_hidden_models)
  {
    return nullptr;
  }
  else
  {
    for (int i = 0; i < wmo->groups.size(); ++i)
    {
      for (auto& doodad : _doodads_per_group[i])
      {
        if (doodad.finishedLoading() && doodad.need_matrix_update())
        {
          doodad.update_transform_matrix_wmo(this);
        }
      }
    }
  }

  return &_doodads_per_group;
}
