// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "WorldRender.hpp"
#include <external/tracy/Tracy.hpp>
#include <math/frustum.hpp>
#include <noggit/Log.h>
#include <noggit/World.h>
#include <noggit/TileWater.hpp>
#include <noggit/ChunkWater.hpp>
#include <noggit/liquid_layer.hpp>
#include <external/PNG2BLP/Png2Blp.h>
#include <noggit/DBC.h>
#include <noggit/project/CurrentProject.hpp>

#include <QDir>
#include <QBuffer>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

using namespace Noggit::Rendering;

namespace
{
  bool classic_attachment_debug_enabled()
  {
    static bool const enabled = []
    {
      if (char const* value = std::getenv("NOGGIT_CLASSIC_M2_DEBUG"))
      {
        return std::string(value) != "0";
      }

      return false;
    }();

    return enabled;
  }

  bool capture_debug_enabled()
  {
    static bool const enabled = []
    {
      if (char const* value = std::getenv("NOGGIT_CAPTURE_DEBUG"))
      {
        return std::string(value) != "0";
      }

      return false;
    }();

    return enabled;
  }

  bool capture_lighting_trace_enabled()
  {
    static bool const enabled = []
    {
      if (char const* value = std::getenv("NOGGIT_CAPTURE_LIGHTING_TRACE"))
      {
        return std::string(value) != "0";
      }

      return false;
    }();

    return enabled;
  }

  bool classic_effect_debug_enabled()
  {
    static bool const enabled = []
    {
      if (char const* value = std::getenv("NOGGIT_CLASSIC_EFFECT_DEBUG"))
      {
        return std::string(value) != "0";
      }

      return false;
    }();

    return enabled;
  }

  bool is_classic_effect_model_path(std::string const& path)
  {
    if (path.starts_with("world/generic/passivedoodads/particleemitters/"))
    {
      return true;
    }

    return path == "world/khazmodan/ironforge/passivedoodads/lavasteam/lavasteam.m2"
        || path == "world/khazmodan/ironforge/passivedoodads/lavasteam/lavasteam_low.m2";
  }

  bool should_trace_gameobject_spawn(World::GameObjectSpawnOverlay const& spawn, float distance)
  {
    if (!capture_debug_enabled())
    {
      return false;
    }

    if (distance < 160.0f)
    {
      return true;
    }

    if (spawn.model_path.find("darkiron") != std::string::npos)
    {
      return true;
    }

    return spawn.name.find("Dark Iron") != std::string::npos
        || spawn.name.find("dark iron") != std::string::npos;
  }

  void trace_gameobject_spawn(char const* stage,
                              World::GameObjectSpawnOverlay const& spawn,
                              float distance,
                              char const* reason = nullptr,
                              ModelInstance const* instance = nullptr)
  {
    if (!should_trace_gameobject_spawn(spawn, distance))
    {
      return;
    }

    static std::set<std::string> logged_gameobject_stages;
    std::string key = std::to_string(spawn.guid) + ":" + stage;
    if (!logged_gameobject_stages.insert(std::move(key)).second)
    {
      return;
    }

    std::ostringstream line;
    line << "Gameobject spawn render trace stage=" << stage
         << " guid=" << spawn.guid
         << " entry=" << spawn.entry
         << " display=" << spawn.display_id
         << " name='" << spawn.name << "'"
         << " model='" << spawn.model_path << "'"
         << " pos={" << spawn.pos.x << ", " << spawn.pos.y << ", " << spawn.pos.z << "}"
         << " distance=" << distance
         << " templateScale=" << spawn.template_scale
         << " createFailed=" << (spawn.model_create_failed ? 1 : 0)
         << " hasInstance=" << (spawn.model_instance.has_value() ? 1 : 0);

    if (reason)
    {
      line << " reason='" << reason << "'";
    }

    if (instance)
    {
      line << " instanceScale=" << instance->scale
           << " modelLoaded=" << (instance->model->finishedLoading() ? 1 : 0)
           << " loadingFailed=" << (instance->model->loading_failed() ? 1 : 0)
           << " extentsMin={" << instance->extents[0].x << ", " << instance->extents[0].y << ", " << instance->extents[0].z << "}"
           << " extentsMax={" << instance->extents[1].x << ", " << instance->extents[1].y << ", " << instance->extents[1].z << "}";
    }

    LogDebug << line.str() << std::endl;
  }

  bool should_trace_creature_spawn(World::CreatureSpawnOverlay const& spawn, float distance)
  {
    if (!capture_debug_enabled())
    {
      return false;
    }

    if (distance < 220.0f)
    {
      return true;
    }

    if (spawn.model_path.find("garr") != std::string::npos
        || spawn.model_path.find("firesworn") != std::string::npos)
    {
      return true;
    }

    return spawn.name.find("Garr") != std::string::npos
        || spawn.name.find("Firesworn") != std::string::npos;
  }

  void trace_creature_spawn(char const* stage,
                            World::CreatureSpawnOverlay const& spawn,
                            float distance,
                            char const* reason = nullptr,
                            ModelInstance const* instance = nullptr)
  {
    if (!should_trace_creature_spawn(spawn, distance))
    {
      return;
    }

    static std::set<std::string> logged_creature_stages;
    std::string key = std::to_string(spawn.guid) + ":" + stage;
    if (!logged_creature_stages.insert(std::move(key)).second)
    {
      return;
    }

    std::ostringstream line;
    line << "Creature spawn render trace stage=" << stage
         << " guid=" << spawn.guid
         << " entry=" << spawn.entry
         << " display=" << spawn.display_id
         << " name='" << spawn.name << "'"
         << " model='" << spawn.model_path << "'"
         << " pos={" << spawn.pos.x << ", " << spawn.pos.y << ", " << spawn.pos.z << "}"
         << " distance=" << distance
         << " templateScale=" << spawn.template_scale
         << " modelScale=" << spawn.model_scale
         << " createFailed=" << (spawn.model_create_failed ? 1 : 0)
         << " hasInstance=" << (spawn.model_instance.has_value() ? 1 : 0);

    if (reason)
    {
      line << " reason='" << reason << "'";
    }

    if (instance)
    {
      line << " instanceScale=" << instance->scale
           << " modelLoaded=" << (instance->model->finishedLoading() ? 1 : 0)
           << " loadingFailed=" << (instance->model->loading_failed() ? 1 : 0)
           << " extentsMin={" << instance->extents[0].x << ", " << instance->extents[0].y << ", " << instance->extents[0].z << "}"
           << " extentsMax={" << instance->extents[1].x << ", " << instance->extents[1].y << ", " << instance->extents[1].z << "}";
    }

    LogDebug << line.str() << std::endl;
  }

  float creature_spawn_model_draw_distance()
  {
    // Dev env override still wins.
    if (char const* value = std::getenv("NOGGIT_CREATURE_MODEL_DRAW_DISTANCE"))
    {
      auto const parsed = std::strtof(value, nullptr);
      if (std::isfinite(parsed) && parsed > 0.0f)
      {
        return parsed;
      }
    }

    // Live Settings slider (creature/draw_distance). Read each frame so the slider applies
    // immediately. Default 120 (the previous hard-coded value).
    float const v = QSettings().value("creature/draw_distance", 120.0f).toFloat();
    return (std::isfinite(v) && v > 0.0f) ? v : 120.0f;
  }

  float creature_spawn_marker_draw_distance()
  {
    static float const distance = []
    {
      if (char const* value = std::getenv("NOGGIT_CREATURE_MARKER_DRAW_DISTANCE"))
      {
        auto const parsed = std::strtof(value, nullptr);
        if (std::isfinite(parsed) && parsed > 0.0f)
        {
          return parsed;
        }
      }

      return 300.0f;
    }();

    return distance;
  }

  bool creature_spawn_markers_enabled()
  {
    static bool const enabled = []
    {
      if (char const* value = std::getenv("NOGGIT_CREATURE_MARKERS"))
      {
        return std::string(value) != "0";
      }

      return true;
    }();

    return enabled;
  }

  std::size_t creature_spawn_model_create_budget()
  {
    static std::size_t const budget = []
    {
      if (char const* value = std::getenv("NOGGIT_CREATURE_MODEL_CREATE_BUDGET"))
      {
        char* end = nullptr;
        auto const parsed = std::strtoul(value, &end, 10);
        if (end != value && parsed > 0ul)
        {
          return static_cast<std::size_t>(parsed);
        }
      }

      return static_cast<std::size_t>(32);
    }();

    return budget;
  }

  bool light_effect_debug_enabled()
  {
    static bool const enabled = []
    {
      if (char const* value = std::getenv("NOGGIT_LIGHT_DEBUG"))
      {
        return std::string(value) != "0";
      }

      return false;
    }();

    return enabled;
  }

  // True for M2 models that have at least one additive pass (No_Add_Alpha / Add) and NO solid
  // (Opaque / Alpha_Key) pass — i.e. pure additive glows (god rays / lightshafts). Drawn AFTER the
  // water so it doesn't paint over them. NOTE: deliberately NOT broadened to all translucent-only
  // models — deferring Alpha effects like waterfalls and collecting their particles crashed in
  // ParticleSystem::draw (the deferred mesh path left their transform buffer in a bad state).
  bool is_pure_additive_light_effect(Model* model)
  {
    if (!model)
    {
      return false;
    }

    auto const& passes = model->renderer()->renderPasses();
    if (passes.empty())
    {
      return false;
    }

    bool has_additive = false;
    for (auto const& pass : passes)
    {
      auto const blend = static_cast<M2Blend>(pass.blend_mode);
      if (blend == M2Blend::Opaque || blend == M2Blend::Alpha_Key)
      {
        return false;
      }
      if (blend == M2Blend::Add || blend == M2Blend::No_Add_Alpha)
      {
        has_additive = true;
      }
    }

    return has_additive;
  }

  ModelAttachmentDef const* find_attachment_def(Model const* model, int attachment_id)
  {
    if (!model)
    {
      return nullptr;
    }

    if (attachment_id < 0
        || static_cast<std::size_t>(attachment_id) >= model->_attachment_lookup.size())
    {
      return nullptr;
    }

    auto const lookup = model->_attachment_lookup[attachment_id];
    auto const attachment_is_sane = [model](ModelAttachmentDef const& attachment)
    {
      return attachment.bone < model->header.nBones
          && std::isfinite(attachment.pos.x)
          && std::isfinite(attachment.pos.y)
          && std::isfinite(attachment.pos.z);
    };

    if (lookup >= 0 && static_cast<std::size_t>(lookup) < model->_attachments.size())
    {
      auto const& attachment = model->_attachments[lookup];
      if (!model->usesClassicLayout() || attachment_is_sane(attachment))
      {
        return &attachment;
      }
    }

    if (model->usesClassicLayout())
    {
      for (auto const& attachment : model->_attachments)
      {
        if (static_cast<int>(attachment.id) == attachment_id && attachment_is_sane(attachment))
        {
          return &attachment;
        }
      }
    }

    return nullptr;
  }

  glm::mat4x4 attachment_world_matrix(ModelInstance const& parent_instance,
                                      Model const* parent_model,
                                      ModelAttachmentDef const* attachment_def)
  {
    if (!parent_model || !attachment_def)
    {
      return parent_instance.transformMatrix();
    }

    glm::mat4x4 attachment_matrix = glm::translate(glm::mat4x4(1.0f), fixCoordSystem(attachment_def->pos));

    if (attachment_def->bone >= 0
        && static_cast<std::size_t>(attachment_def->bone) < parent_model->bone_matrices.size())
    {
      attachment_matrix = parent_model->bone_matrices[attachment_def->bone] * attachment_matrix;
    }

    return parent_instance.transformMatrix() * attachment_matrix;
  }

  bool should_suppress_legacy_creature_instance(World const* world, ModelInstance const& model_instance)
  {
    if (!world || !world->drawCreatureSpawns() || !model_instance.model.get())
    {
      return false;
    }

    if (!model_instance.model->file_key().hasFilepath())
    {
      return false;
    }

    auto const& model_path = model_instance.model->file_key().filepath();
    if (!model_path.starts_with("creature/"))
    {
      return false;
    }

    float legacy_radius = model_instance.model->rad * model_instance.scale;
    if (legacy_radius <= 0.0f)
    {
      legacy_radius = 1.0f;
    }

    static std::set<std::string> logged_suppressions;
    static std::set<std::string> logged_legacy_candidates;
    float nearest_distance = std::numeric_limits<float>::max();
    std::uint32_t nearest_guid = 0;
    std::string nearest_model;

    for (auto const& spawn : world->creatureSpawns())
    {
      if (!spawn.model_instance.has_value() && spawn.model_path.empty())
      {
        continue;
      }

      if (spawn.model_path != model_path)
      {
        continue;
      }

      float overlay_radius = std::max(0.5f, spawn.template_scale * spawn.model_scale);
      if (spawn.model_instance.has_value())
      {
        auto const& overlay = *spawn.model_instance;
        if (overlay.model.get())
        {
          overlay_radius = overlay.model->rad * overlay.scale;
        }
      }
      if (overlay_radius <= 0.0f)
      {
        overlay_radius = 1.0f;
      }

      float const distance = glm::distance(model_instance.get_pos(), spawn.pos);
      if (distance < nearest_distance)
      {
        nearest_distance = distance;
        nearest_guid = spawn.guid;
        if (spawn.model_instance.has_value()
            && spawn.model_instance->model.get()
            && spawn.model_instance->model->file_key().hasFilepath())
        {
          nearest_model = spawn.model_instance->model->file_key().filepath();
        }
        else
        {
          nearest_model = spawn.model_path;
        }
      }
      float const overlap_radius = std::max(4.0f, legacy_radius + overlay_radius + 1.5f);
      if (distance <= overlap_radius)
      {
        std::ostringstream key;
        key << model_instance.model->file_key().stringRepr() << '|'
            << model_instance.uid << '|'
            << spawn.guid;
        if (logged_suppressions.insert(key.str()).second)
        {
          LogDebug << "Suppressing legacy creature instance model='"
                   << model_instance.model->file_key().stringRepr()
                   << "' uid=" << model_instance.uid
                   << " overlayGuid=" << spawn.guid
                   << " distance=" << distance
                   << " radius=" << overlap_radius
                   << std::endl;
        }
        return true;
      }
    }

    if (model_path == "creature/humanmalepeasant/humanmalepeasant.m2"
        || model_path == "creature/humanfemalepeasant/humanfemalepeasant.m2"
        || model_path == "creature/elementalearth/elementalearth.m2")
    {
      std::ostringstream key;
      key << model_path << '|' << model_instance.uid;
      if (logged_legacy_candidates.insert(key.str()).second)
      {
        LogDebug << "Legacy creature candidate model='" << model_path
                 << "' uid=" << model_instance.uid
                 << " pos={" << model_instance.get_pos().x << "," << model_instance.get_pos().y << "," << model_instance.get_pos().z << "}"
                 << " nearestOverlayGuid=" << nearest_guid
                 << " nearestOverlayModel='" << nearest_model << "'"
                 << " nearestDistance=" << nearest_distance
                 << std::endl;
      }
    }

    return false;
  }
}

WorldRender::WorldRender(World* world)
: BaseRender()
, _world(world)
, _liquid_texture_manager(world->_context)
, _view_distance(world->_settings->value("view_distance", 2000.f).toFloat())
, _cull_distance(0.f)
{
}

void WorldRender::draw (glm::mat4x4 const& model_view
    , glm::mat4x4 const& projection
    , glm::vec3 const& cursor_pos
    , float cursorRotation
    , glm::vec4 const& cursor_color
    , CursorType cursor_type
    , float brush_radius
    , bool show_unpaintable_chunks
    , bool draw_only_inside_light_sphere
    , bool draw_wireframe_light_sphere
    , float alpha_light_sphere
    , float inner_radius_ratio
    , glm::vec3 const& ref_pos
    , float angle
    , float orientation
    , bool use_ref_pos
    , bool angled_mode
    , bool draw_paintability_overlay
    , editing_mode terrainMode
    , glm::vec3 const& camera_pos
    , bool camera_moved
    , bool draw_mfbo
    , bool draw_terrain
    , bool draw_wmo
    , bool draw_water
    , bool draw_wmo_doodads
    , bool draw_models
    , bool draw_model_animations
    , bool draw_models_with_box
    , bool draw_hidden_models
    , MinimapRenderSettings* minimap_render_settings
    , bool draw_fog
    , eTerrainType ground_editing_brush
    , int water_layer
    , display_mode display
    , bool draw_occlusion_boxes
    , bool minimap_render
    , bool draw_wmo_exterior
    , bool draw_bloom
)
{

  ZoneScoped;

  glm::mat4x4 const mvp(projection * model_view);
  math::frustum const frustum (mvp);

  if (capture_debug_enabled())
  {
    LogDebug << "WorldRender::draw begin camera={"
             << camera_pos.x << ", " << camera_pos.y << ", " << camera_pos.z << "}"
             << " draw_terrain=" << draw_terrain
             << " draw_wmo=" << draw_wmo
             << " draw_water=" << draw_water
             << " draw_wmo_doodads=" << draw_wmo_doodads
             << " draw_models=" << draw_models
             << " draw_creatures=" << _world->drawCreatureSpawns()
             << std::endl;
  }

  if (camera_moved)
    updateMVPUniformBlock(model_view, projection);

  gl.disable(GL_DEPTH_TEST);

  if (!minimap_render)
    updateLightingUniformBlock(draw_fog, camera_pos);
  else
    updateLightingUniformBlockMinimap(minimap_render_settings);

  // Bloom: render the whole 3D scene into an offscreen colour target first, so afterwards we can pull
  // out the bright areas, blur them and add them back (the glow/bleed of bright sky openings, light
  // shafts, additive glows). Only for the main 3D viewport -- never the minimap or 2D mode.
  bool const do_bloom = draw_bloom && !minimap_render && display == display_mode::in_3D;
  GLint bloom_prev_fbo = 0;
  GLint bloom_vp[4] = {0, 0, 0, 0};
  if (do_bloom)
  {
    gl.getIntegerv(GL_FRAMEBUFFER_BINDING, &bloom_prev_fbo);
    gl.getIntegerv(GL_VIEWPORT, bloom_vp);
    ensureBloomTargets(bloom_vp[2], bloom_vp[3]);
    gl.bindFramebuffer(GL_FRAMEBUFFER, _bloom_scene_fbo);
    gl.viewport(0, 0, _bloom_w, _bloom_h);
  }

  // Clear colour+depth of WHATEVER target is now bound -- the bloom scene FBO when do_bloom, else the
  // real (Qt) framebuffer. This MUST run in both paths: the sky sphere is drawn first at far depth, so
  // without a depth clear the non-bloom path fails the depth test against a stale depth buffer and the
  // sky renders BLACK. CRUCIAL: glClear(GL_DEPTH) obeys the depth WRITE mask -- the previous frame ends on
  // particle/additive passes that leave depthMask=FALSE, which turns the depth clear into a silent no-op.
  // Force it back on first. (With bloom the composite blits the scene colour ignoring depth, which is why
  // only the non-bloom path showed the black sky.)
  gl.depthMask(GL_TRUE);
  gl.clearColor(0.f, 0.f, 0.f, 1.f);
  gl.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // setup render settings for minimap
  if (minimap_render)
  {
    _terrain_params_ubo_data.draw_shadows = minimap_render_settings->draw_shadows;
    _terrain_params_ubo_data.draw_lines = minimap_render_settings->draw_adt_grid;
    _terrain_params_ubo_data.draw_terrain_height_contour = minimap_render_settings->draw_elevation;
    _terrain_params_ubo_data.draw_hole_lines = false;
    _terrain_params_ubo_data.draw_impass_overlay = false;
    _terrain_params_ubo_data.draw_areaid_overlay = false;
    _terrain_params_ubo_data.draw_paintability_overlay = false;
    _terrain_params_ubo_data.draw_selection_overlay = false;
    _terrain_params_ubo_data.draw_wireframe = false;
    _need_terrain_params_ubo_update = true;
  }

  if (_need_terrain_params_ubo_update)
    updateTerrainParamsUniformBlock();

  // Tile occlusion culling is currently too aggressive around large terrain/WMOs
  // and can hide visible ADTs while their objects still render.
  constexpr bool occlusion_cull = false;

  // Frustum culling
  _world->_n_loaded_tiles = 0;
  unsigned tile_counter = 0;
  for (MapTile* tile : _world->mapIndex.loaded_tiles())
  {
    tile->recalcExtents();
    tile->recalcObjectInstanceExtents();
    tile->recalcCombinedExtents();

    if (minimap_render)
    {
      auto& tile_extents = tile->getCombinedExtents();
      tile->calcCamDist(camera_pos);
      tile->renderer()->setFrustumCulled(false);
      tile->renderer()->setObjectsFrustumCullTest(2);
      tile->renderer()->setOccluded(false);
      _world->_loaded_tiles_buffer[tile_counter] = std::make_pair(std::make_pair(static_cast<int>(tile->index.x), static_cast<int>(tile->index.z)), tile);

      tile_counter++;
      _world->_n_loaded_tiles++;
      continue;
    }

    auto& tile_extents = tile->getCombinedExtents();
    bool const tile_in_frustum = frustum.intersects(tile_extents[1], tile_extents[0]) || tile->getChunkUpdateFlags();

    tile->calcCamDist(camera_pos);
    _world->_loaded_tiles_buffer[tile_counter] = std::make_pair(std::make_pair(static_cast<int>(tile->index.x), static_cast<int>(tile->index.z)), tile);

    tile->renderer()->setObjectsFrustumCullTest(tile_in_frustum ? 1 : 0);
    if (!occlusion_cull)
    {
      tile->renderer()->discardTileOcclusionQuery();
      tile->renderer()->setOccluded(false);
    }

    if (tile_in_frustum && frustum.contains(tile_extents[0]) && frustum.contains(tile_extents[1]))
    {
      tile->renderer()->setObjectsFrustumCullTest(tile->renderer()->objectsFrustumCullTest() + 1);
    }

    if (tile->renderer()->isFrustumCulled())
    {
      tile->renderer()->setOverrideOcclusionCulling(true);
      tile->renderer()->discardTileOcclusionQuery();
      tile->renderer()->setOccluded(false);
    }

    tile->renderer()->setFrustumCulled(false);
    tile_counter++;

    _world->_n_loaded_tiles++;
  }

  auto buf_end = _world->_loaded_tiles_buffer.begin() + tile_counter;
  _world->_loaded_tiles_buffer[tile_counter] = std::make_pair<std::pair<int, int>, MapTile*>(std::make_pair<int, int>(0, 0), nullptr);


  // It is always import to sort tiles __front to back__.
  // Otherwise selection would not work. Overdraw overhead is gonna occur as well.
  // TODO: perhaps parallel sort?
  std::sort(_world->_loaded_tiles_buffer.begin(), buf_end,
            [](std::pair<std::pair<int, int>, MapTile*>& a, std::pair<std::pair<int, int>, MapTile*>& b) -> bool
            {
              if (!a.second)
              {
                return false;
              }

              if (!b.second)
              {
                return true;
              }

              return a.second->camDist() < b.second->camDist();
            });

  // only draw the sky in 3D
  if(!minimap_render && display == display_mode::in_3D)
  {
    ZoneScopedN("World::draw() : Draw skies");
    OpenGL::Scoped::use_program m2_shader {*_m2_program.get()};

    bool hadSky = false;

    if (draw_wmo || _world->mapIndex.hasAGlobalWMO())
    {
      _world->_model_instance_storage.for_each_wmo_instance
          (
              [&] (WMOInstance& wmo)
              {
                if (wmo.wmo->finishedLoading() && wmo.wmo->skybox)
                {
                  if (wmo.group_extents.empty())
                  {
                    wmo.recalcExtents();
                  }

                  hadSky = wmo.wmo->renderer()->drawSkybox(model_view
                      , camera_pos
                      , m2_shader
                      , frustum
                      , _cull_distance
                      , _world->animtime
                      , draw_model_animations
                      , wmo.extents[0]
                      , wmo.extents[1]
                      , wmo.group_extents
                  );
                }

              }
              , [&] () { return hadSky; }
          );
    }

    if (!hadSky)
    {
      _skies->draw( model_view
          , projection
          , camera_pos
          , m2_shader
          , frustum
          , _cull_distance
          , _world->animtime
          , _outdoor_light_stats
      );
    }
  }

  // Read the object view distance LIVE each frame (not just once in the ctor) so changing the
  // Settings "View Distance" field applies immediately without a map reload. This governs how far
  // objects/WMOs (e.g. the lighthouse) render when fog is off (fog end caps it when fog is on).
  _view_distance = _world->_settings->value("view_distance", 2000.f).toFloat();
  // Render distance is capped at the user's view distance. With fog ON we still cull at the fog end
  // when that's NEARER (no point drawing what the fog hides), but a fog end larger than the view
  // distance must NOT push the render distance past it -- otherwise enabling fog renders further than
  // the view distance allows.
  _cull_distance = draw_fog
                 ? std::min(_skies->fog_distance_end(), _view_distance)
                 : _view_distance;

  // Draw verylowres heightmap (distant horizon backdrop). Toggleable live via Settings
  // ("render_horizon", default on) so it can be disabled to stop fog rendering distant mesh.
  bool const draw_horizon = _world->_settings->value("render_horizon", true).toBool();
  if (!_world->mapIndex.hasAGlobalWMO() && draw_fog && draw_terrain && draw_horizon)
  {
    ZoneScopedN("World::draw() : Draw horizon");
    _horizon_render->draw (model_view, projection, &_world->mapIndex, _skies->color_set[FOG_COLOR], _cull_distance, frustum, camera_pos, display);
  }

  gl.enable(GL_DEPTH_TEST);
  gl.depthFunc(GL_LEQUAL); // less z-fighting artifacts this way, I think
  //gl.disable(GL_BLEND);
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  //gl.disable(GL_CULL_FACE);

  _world->_n_rendered_tiles = 0;
  _world->_n_rendered_objects = 0;

  if (draw_terrain)
  {
    if (capture_debug_enabled())
    {
      LogDebug << "WorldRender::draw terrain begin" << std::endl;
    }

    ZoneScopedN("World::draw() : Draw terrain");

    gl.disable(GL_BLEND);

    {
      OpenGL::Scoped::use_program mcnk_shader{ *_mcnk_program.get() };

      mcnk_shader.uniform("camera", glm::vec3(camera_pos.x, camera_pos.y, camera_pos.z));
      mcnk_shader.uniform("animtime", static_cast<int>(_world->animtime));

      if (cursor_type != CursorType::NONE)
      {
        mcnk_shader.uniform("draw_cursor_circle", static_cast<int>(cursor_type));
        mcnk_shader.uniform("cursor_position", glm::vec3(cursor_pos.x, cursor_pos.y, cursor_pos.z));
        mcnk_shader.uniform("cursorRotation", cursorRotation);
        mcnk_shader.uniform("outer_cursor_radius", brush_radius);
        mcnk_shader.uniform("inner_cursor_ratio", inner_radius_ratio);
        mcnk_shader.uniform("cursor_color", cursor_color);
      }
      else
      {
        mcnk_shader.uniform("draw_cursor_circle", 0);
      }

      gl.bindVertexArray(_mapchunk_vao);
      gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, _mapchunk_index);

      for (auto& pair : _world->_loaded_tiles_buffer)
      {
        MapTile* tile = pair.second;

        if (!tile)
        {
          break;
        }

        if (minimap_render)
          tile->renderer()->setOccluded(false);

        if (tile->renderer()->isOccluded() && !tile->getChunkUpdateFlags() && !tile->renderer()->isOverridingOcclusionCulling())
          continue;

        tile->renderer()->draw(
            mcnk_shader
            , camera_pos
            , show_unpaintable_chunks
            , draw_paintability_overlay
            , terrainMode == editing_mode::minimap
              && 64 * tile->index.x + tile->index.z < minimap_render_settings->selected_tiles.size()
              && minimap_render_settings->selected_tiles[64 * tile->index.x + tile->index.z]
        );

        _world->_n_rendered_tiles++;

      }

      gl.bindVertexArray(0);
      gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    if (capture_debug_enabled())
    {
      LogDebug << "WorldRender::draw terrain end" << std::endl;
    }
  }

  if (terrainMode == editing_mode::object && _world->has_multiple_model_selected())
  {
    ZoneScopedN("World::draw() : Draw pivot point");
    OpenGL::Scoped::bool_setter<GL_DEPTH_TEST, GL_FALSE> const disable_depth_test;

    float dist = glm::distance(camera_pos, _world->_multi_select_pivot.value());
    _sphere_render.draw(mvp, _world->_multi_select_pivot.value(), cursor_color, std::min(2.f, std::max(0.15f, dist * 0.02f)));
  }

  if (use_ref_pos)
  {
    ZoneScopedN("World::draw() : Draw ref pos");
    _sphere_render.draw(mvp, ref_pos, cursor_color, 0.3f);
  }

  if (terrainMode == editing_mode::ground && ground_editing_brush == eTerrainType_Vertex)
  {
    ZoneScopedN("World::draw() : Draw vertex points");
    float size = glm::distance(_world->vertexCenter(), camera_pos);
    gl.pointSize(std::max(0.001f, 10.0f - (1.25f * size / CHUNKSIZE)));

    for (glm::vec3 const* pos : _world->_vertices_selected)
    {
      _sphere_render.draw(mvp, *pos, glm::vec4(1.f, 0.f, 0.f, 1.f), 0.5f);
    }

    _sphere_render.draw(mvp, _world->vertexCenter(), cursor_color, 2.f);
  }

  std::unordered_map<Model*, std::size_t> model_with_particles;

  // Pure-additive light effects (god rays / lighthouse beams) deferred to draw AFTER the water.
  std::vector<Model*> deferred_light_effects;

  tsl::robin_map<Model*, std::vector<glm::mat4x4>> models_to_draw;
  struct CreatureSpawnInstanceDraw
  {
    std::uint32_t guid = 0;
    ModelInstance* instance = nullptr;
    World::CreatureSpawnOverlay* spawn = nullptr;
  };
  std::vector<CreatureSpawnInstanceDraw> creature_spawn_instances_to_draw;
  std::vector<WMOInstance*> wmos_to_draw;

  // frame counter loop. pretty hacky but works
  // this is used to make sure no object is processed more than once within a frame
  static int frame = 0;

  if (frame == std::numeric_limits<int>::max())
  {
    frame = 0;
  }
  else
  {
    frame++;
  }

  for (auto& pair : _world->_loaded_tiles_buffer)
  {
    MapTile* tile = pair.second;

    if (!tile)
    {
      break;
    }

    if (minimap_render)
      tile->renderer()->setOccluded(false);

    if (tile->renderer()->isOccluded() && !tile->getChunkUpdateFlags() && !tile->renderer()->isOverridingOcclusionCulling())
      continue;

    // early dist check
    // TODO: optional
    if (tile->camDist() > _cull_distance)
      continue;


    // TODO: subject to potential generalization
    for (auto& pair : tile->getObjectInstances())
    {
      if (pair.second[0]->which() == eMODEL)
      {
        if (!draw_models && !(minimap_render && minimap_render_settings->use_filters))
          continue;

        auto& instances = models_to_draw[reinterpret_cast<Model*>(pair.first)];

        // memory allocation heuristic. all objects will pass if tile is entirely in frustum.
        // otherwise we only allocate for a half

        if (tile->renderer()->objectsFrustumCullTest() > 1)
        {
          instances.reserve(instances.size() + pair.second.size());
        }
        else
        {
          instances.reserve(instances.size() + pair.second.size() / 2);
        }


        for (auto& instance : pair.second)
        {
          // do not render twice the cross-referenced objects twice
          if (instance->frame == frame)
          {
            continue;
          }

          instance->frame = frame;

          auto m2_instance = static_cast<ModelInstance*>(instance);

          if (!minimap_render && should_suppress_legacy_creature_instance(_world, *m2_instance))
          {
            continue;
          }

          if ((tile->renderer()->objectsFrustumCullTest() > 1 || m2_instance->isInFrustum(frustum)) && m2_instance->isInRenderDist(_cull_distance, camera_pos, display))
          {
            instances.push_back(m2_instance->transformMatrix());
          }

        }

      }
      else if (pair.second[0]->which() == eWMO)
      {
        if (!draw_wmo)
            continue;

        // memory allocation heuristic. all objects will pass if tile is entirely in frustum.
        // otherwise we only allocate for a half

        if (tile->renderer()->objectsFrustumCullTest() > 1)
        {
          wmos_to_draw.reserve(wmos_to_draw.size() + pair.second.size());
        }
        else
        {
          wmos_to_draw.reserve(wmos_to_draw.size() + pair.second.size() / 2);
        }

        for (auto& instance : pair.second)
        {
          // do not render twice the cross-referenced objects twice
          if (instance->frame == frame)
          {
            continue;
          }

          instance->frame = frame;

          auto wmo_instance = static_cast<WMOInstance*>(instance);

          if (tile->renderer()->objectsFrustumCullTest() > 1 || frustum.intersects(wmo_instance->extents[1], wmo_instance->extents[0]))
          {
            wmos_to_draw.push_back(wmo_instance);
          }
        }
      }
    }
  }

  // WMOs / map objects
  if (draw_wmo || _world->mapIndex.hasAGlobalWMO())
  {
    if (capture_debug_enabled())
    {
      LogDebug << "WorldRender::draw wmo begin queued=" << wmos_to_draw.size() << std::endl;
    }

    ZoneScopedN("World::draw() : Draw WMOs");
    {
      OpenGL::Scoped::use_program wmo_program{*_wmo_program.get()};

      wmo_program.uniform("camera", glm::vec3(camera_pos.x, camera_pos.y, camera_pos.z));

      // Clear stencil to 0 on the bound render target so the WMO water "draw once per pixel"
      // stencil (applied to EXTERIOR water in WMORender::draw) starts clean each frame. This
      // stops overlapping same-level water planes (e.g. Timbermaw) from stacking and multiplying
      // darker. Harmless no-op if the framebuffer has no stencil buffer.
      gl.clearStencil(0);
      gl.clear(GL_STENCIL_BUFFER_BIT);

      // make this check per WMO or global WMO with tiles may not work
      bool disable_cull = false;

      if (_world->mapIndex.hasAGlobalWMO() && !wmos_to_draw.size())
      {
          auto global_wmo = _world->_model_instance_storage.get_wmo_instance(_world->mWmoEntry.uniqueID);
          if (global_wmo.has_value())
          {
            wmos_to_draw.push_back(global_wmo.value());
            disable_cull = true;
          }
      }


      for (auto& instance: wmos_to_draw)
      {
        bool is_hidden = instance->wmo->is_hidden();

        bool is_exclusion_filtered = false;

        // minimap render exclusion filters
        // per-model
        if (minimap_render && minimap_render_settings->use_filters)
        {
          if (instance->instance_model()->file_key().hasFilepath())
          {
            for(int i = 0; i < minimap_render_settings->wmo_model_filter_exclude->count(); ++i)
            {
              auto item = reinterpret_cast<Ui::MinimapWMOModelFilterEntry*>(
                  minimap_render_settings->wmo_model_filter_exclude->itemWidget(
                  minimap_render_settings->wmo_model_filter_exclude->item(i)));

              if (item->getFileName().toStdString() == instance->instance_model()->file_key().filepath())
              {
                is_exclusion_filtered = true;
                break;
              }
            }
          }

          // per-instance
          for(int i = 0; i < minimap_render_settings->wmo_instance_filter_exclude->count(); ++i)
          {
            auto item = reinterpret_cast<Ui::MinimapInstanceFilterEntry*>(
                minimap_render_settings->wmo_instance_filter_exclude->itemWidget(
                minimap_render_settings->wmo_instance_filter_exclude->item(i)));

            if (item->getUid() == instance->uid)
            {
              is_exclusion_filtered = true;
              break;
            }
          }

          // skip model rendering if excluded by filter
          if (is_exclusion_filtered)
            continue;
        }


        if (draw_hidden_models || !is_hidden)
        {
          instance->draw(wmo_program
              , draw_water ? _wmo_liquid_program.get() : nullptr
              , draw_water ? &_liquid_texture_manager : nullptr
              , model_view
              , projection
              , frustum
              , _cull_distance
              , camera_pos
              , is_hidden
              , draw_wmo_doodads
              , draw_fog
              , _world->current_selection()
              , _world->animtime
              , _skies->hasSkies()
              , display
              , disable_cull
              , draw_wmo_exterior

          );
        }
      }
    }

    if (capture_debug_enabled())
    {
      LogDebug << "WorldRender::draw wmo end" << std::endl;
    }
  }


  // occlusion culling
  // terrain tiles act as occluders for each other, water and M2/WMOs.
  // occlusion culling is not performed on per model instance basis
  // rendering a little extra is cheaper than querying.
  // occlusion latency has 1-2 frames delay.

  if (occlusion_cull)
  {
    if (capture_debug_enabled())
    {
      LogDebug << "WorldRender::draw occlusion begin" << std::endl;
    }

    OpenGL::Scoped::use_program occluder_shader{ *_occluder_program.get() };
    gl.colorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    gl.depthMask(GL_FALSE);
    gl.bindVertexArray(_occluder_vao);
    gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, _occluder_index);
    gl.disable(GL_CULL_FACE); // TODO: figure out why indices are bad and we need this

    for (auto& pair : _world->_loaded_tiles_buffer)
    {
      MapTile* tile = pair.second;

      if (!tile)
      {
        break;
      }

      tile->renderer()->setOccluded(!tile->renderer()->getTileOcclusionQueryResult(camera_pos));
      tile->renderer()->doTileOcclusionQuery(occluder_shader);
    }

    gl.enable(GL_CULL_FACE);
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.depthMask(GL_TRUE);
    gl.bindVertexArray(0);
    gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    if (capture_debug_enabled())
    {
      LogDebug << "WorldRender::draw occlusion end" << std::endl;
    }
  }


  // draw occlusion AABBs
  if (draw_occlusion_boxes)
  {

    for (auto& pair : _world->_loaded_tiles_buffer)
    {
      MapTile* tile = pair.second;

      if (!tile)
      {
        break;
      }

      glm::mat4x4 identity_mtx = glm::mat4x4{1};
      auto& extents = tile->getCombinedExtents();
      Noggit::Rendering::Primitives::WireBox::getInstance(_world->_context).draw ( model_view
          , projection
          , identity_mtx
          , { 1.0f, 1.0f, 0.0f, 1.0f }
          , extents[0]
          , extents[1]
      );
    }
  }

  bool draw_doodads_wmo = draw_wmo && draw_wmo_doodads;
  bool draw_creature_spawns = !minimap_render && _world->drawCreatureSpawns();
  // GameObject models render in the creature tool (alongside creatures) and in the gameobject tool.
  bool draw_gameobject_spawns = !minimap_render && (_world->drawCreatureSpawns() || _world->drawGameObjectSpawns());
  float const creature_spawn_model_distance = creature_spawn_model_draw_distance();
  float const creature_spawn_marker_distance = creature_spawn_marker_draw_distance();
  // M2s / models
  if (draw_models || draw_doodads_wmo || draw_creature_spawns || (minimap_render && minimap_render_settings->use_filters))
  {
    if (capture_debug_enabled())
    {
      LogDebug << "WorldRender::draw m2 begin draw_models=" << draw_models
               << " draw_doodads_wmo=" << draw_doodads_wmo
               << " draw_creature_spawns=" << draw_creature_spawns
               << std::endl;
    }

    ZoneScopedN("World::draw() : Draw M2s");

    if (draw_model_animations)
    {
      ModelManager::resetAnim();
    }

    if (draw_doodads_wmo)
    {
      if (draw_creature_spawns)
      {
        _world->ensureCreatureSpawnsLoaded();
      }

      if (capture_debug_enabled())
      {
        LogDebug << "WorldRender::draw wmo doodad inject begin wmos=" << wmos_to_draw.size() << std::endl;
      }

      ZoneScopedN("World::draw() : Inject visible WMO doodads");
      for (auto* wmo_instance : wmos_to_draw)
      {
        auto doodads = wmo_instance->get_visible_doodads(frustum, _cull_distance, camera_pos, draw_hidden_models, display);
        for (auto* doodad : doodads)
        {
          if (!doodad)
          {
            continue;
          }

          if (!minimap_render && should_suppress_legacy_creature_instance(_world, *doodad))
          {
            continue;
          }

          doodad->ensureExtents();
          if (!doodad->model->finishedLoading() || doodad->model->loading_failed())
          {
            continue;
          }

          models_to_draw[doodad->model.get()].push_back(doodad->transformMatrix());
        }
      }

      if (capture_debug_enabled())
      {
        std::size_t doodad_instance_count = 0;
        for (auto const& pair : models_to_draw)
        {
          doodad_instance_count += pair.second.size();
        }
        LogDebug << "WorldRender::draw wmo doodad inject end modelBuckets=" << models_to_draw.size()
                 << " instances=" << doodad_instance_count
                 << std::endl;
      }
    }

    if (draw_creature_spawns)
    {
      _world->ensureCreatureSpawnsLoaded();
      ZoneScopedN("World::draw() : Inject creature spawn models");
      std::size_t models_created_this_frame = 0;
      std::size_t const model_create_budget = creature_spawn_model_create_budget();
      for (auto& spawn : _world->creatureSpawns())
      {
        if (spawn.pending_delete) // marked for deletion -> don't draw
        {
          continue;
        }
        float const creature_distance = glm::distance(camera_pos, spawn.pos);
        trace_creature_spawn("candidate", spawn, creature_distance);

        if (creature_distance > creature_spawn_model_distance)
        {
          trace_creature_spawn("skip-distance", spawn, creature_distance, "draw distance");
          continue;
        }

        if (!spawn.model_instance.has_value())
        {
          if (models_created_this_frame >= model_create_budget)
          {
            trace_creature_spawn("skip-budget", spawn, creature_distance, "create budget");
            continue;
          }

          if (!_world->ensureCreatureSpawnModel(spawn))
          {
            trace_creature_spawn("create-failed", spawn, creature_distance, "ensureCreatureSpawnModel failed");
            continue;
          }

          ++models_created_this_frame;
          trace_creature_spawn("created", spawn, creature_distance, nullptr, &*spawn.model_instance);
        }

        if (!spawn.model_instance.has_value())
        {
          trace_creature_spawn("skip-no-instance", spawn, creature_distance, "missing instance");
          continue;
        }

        auto& mi = *spawn.model_instance;
        mi.ensureExtents();
        if (!mi.model->finishedLoading() || mi.model->loading_failed())
        {
          trace_creature_spawn("skip-load", spawn, creature_distance, "model not loaded", &mi);
          continue;
        }

        if (!mi.isInFrustum(frustum))
        {
          trace_creature_spawn("skip-frustum", spawn, creature_distance, "frustum", &mi);
          continue;
        }

        creature_spawn_instances_to_draw.push_back({spawn.guid, &mi, &spawn});
        trace_creature_spawn("draw-queued", spawn, creature_distance, nullptr, &mi);
      }
    }

    if (draw_gameobject_spawns)
    {
      _world->ensureCreatureSpawnsLoaded();
      ZoneScopedN("World::draw() : Inject gameobject spawn models");
      std::size_t models_created_this_frame = 0;
      std::size_t const model_create_budget = creature_spawn_model_create_budget();
      for (auto& spawn : _world->gameObjectSpawns())
      {
        float const gameobject_distance = glm::distance(camera_pos, spawn.pos);
        trace_gameobject_spawn("candidate", spawn, gameobject_distance);

        if (gameobject_distance > creature_spawn_model_distance)
        {
          trace_gameobject_spawn("skip-distance", spawn, gameobject_distance, "draw distance");
          continue;
        }

        if (!spawn.model_instance.has_value())
        {
          if (models_created_this_frame >= model_create_budget)
          {
            trace_gameobject_spawn("skip-budget", spawn, gameobject_distance, "create budget");
            continue;
          }

          if (!_world->ensureGameObjectSpawnModel(spawn))
          {
            trace_gameobject_spawn("create-failed", spawn, gameobject_distance, "ensureGameObjectSpawnModel failed");
            continue;
          }

          ++models_created_this_frame;
          trace_gameobject_spawn("created", spawn, gameobject_distance, nullptr, &*spawn.model_instance);
        }

        if (!spawn.model_instance.has_value())
        {
          trace_gameobject_spawn("skip-no-instance", spawn, gameobject_distance, "missing instance");
          continue;
        }

        auto& mi = *spawn.model_instance;
        mi.ensureExtents();
        if (!mi.model->finishedLoading() || mi.model->loading_failed())
        {
          trace_gameobject_spawn("skip-load", spawn, gameobject_distance, "model not loaded", &mi);
          continue;
        }

        if (!mi.isInFrustum(frustum))
        {
          trace_gameobject_spawn("skip-frustum", spawn, gameobject_distance, "frustum", &mi);
          continue;
        }

        if (draw_model_animations)
        {
          mi.model->animcalc = false;
        }
        models_to_draw[mi.model.get()].push_back(mi.transformMatrix());
        trace_gameobject_spawn("draw-queued", spawn, gameobject_distance, nullptr, &mi);
      }
    }

    /*
    if (_world->need_model_updates)
    {
      _world->update_models_by_filename();
    }*/

    std::unordered_map<Model*, std::size_t> model_boxes_to_draw;

    {
      if (draw_models || (minimap_render && minimap_render_settings->use_filters))
      {
        if (capture_debug_enabled())
        {
          std::size_t instanced_m2_count = 0;
          for (auto const& pair : models_to_draw)
          {
            instanced_m2_count += pair.second.size();
          }
          LogDebug << "WorldRender::draw instanced m2 begin modelBuckets=" << models_to_draw.size()
                   << " instances=" << instanced_m2_count
                   << std::endl;
        }

        OpenGL::Scoped::use_program m2_shader {*_m2_instanced_program.get()};

        OpenGL::M2RenderState model_render_state;
        model_render_state.tex_arrays = {0, 0};
        model_render_state.tex_indices = {0, 0};
        model_render_state.tex_unit_lookups = {0, 0};
        gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl.disable(GL_BLEND);
        gl.depthMask(GL_TRUE);
        gl.enable(GL_CULL_FACE);
        m2_shader.uniform("blend_mode", 0);
        m2_shader.uniform("unfogged", static_cast<int>(model_render_state.unfogged));
        m2_shader.uniform("unlit",  static_cast<int>(model_render_state.unlit));
        m2_shader.uniform("tex_unit_lookup_1", 0);
        m2_shader.uniform("tex_unit_lookup_2", 0);
        m2_shader.uniform("pixel_shader", 0);

        for (auto& pair : models_to_draw)
        {
          bool is_inclusion_filtered = false;

          // minimap render inclusion filters
          // per-model
          if (minimap_render && minimap_render_settings->use_filters)
          {
            if (pair.first->file_key().hasFilepath())
            {
              for(int i = 0; i < minimap_render_settings->m2_model_filter_include->count(); ++i)
              {
                auto item = reinterpret_cast<Ui::MinimapM2ModelFilterEntry*>(
                    minimap_render_settings->m2_model_filter_include->itemWidget(
                    minimap_render_settings->m2_model_filter_include->item(i)));

                if (item->getFileName().toStdString() == pair.first->file_key().filepath())
                {
                  is_inclusion_filtered = true;
                  break;
                }
              }
            }

            // skip model rendering if excluded by filter
            if (!is_inclusion_filtered)
              continue;
          }

          if (draw_hidden_models || !pair.first->is_hidden())
          {
            // Diagnostic (NOGGIT_LIGHT_DEBUG=1): dump each model bucket's path + per-pass blend
            // modes once, so a beam/glow that isn't being deferred can be identified by name.
            if (light_effect_debug_enabled() && pair.first->file_key().hasFilepath())
            {
              static std::set<std::string> logged_light_buckets;
              auto const& lp = pair.first->file_key().filepath();
              if (logged_light_buckets.insert(lp).second)
              {
                std::ostringstream blends;
                for (auto const& pass : pair.first->renderer()->renderPasses())
                  blends << static_cast<int>(pass.blend_mode) << " ";
                LogDebug << "WorldRender::light model='" << lp
                         << "' passes=[" << blends.str() << "] deferred="
                         << (is_pure_additive_light_effect(pair.first) ? 1 : 0) << std::endl;
              }
            }

            // Pure-additive glows (god rays / lighthouse beams) draw AFTER the water so it doesn't
            // paint over them. BUT only defer those with NO particle/ribbon emitters: the deferred
            // mesh pass doesn't prime the per-model transform buffer the way the particle pass
            // expects, so routing a deferred model's particles through it leaves that buffer invalid
            // (GL_INVALID_OPERATION -> driver crash). Models with particles (e.g. the dusty
            // light-rays) stay on the normal path so their falling dust still draws -- they're
            // interior god-rays with no water to be occluded by anyway. The lighthouse beam has no
            // particles, so it still defers and still renders on top of the water.
            if (is_pure_additive_light_effect(pair.first)
                && pair.first->_particles.empty()
                && pair.first->_ribbons.empty())
            {
              deferred_light_effects.push_back(pair.first);
              continue;
            }

            if (capture_debug_enabled())
            {
              LogDebug << "WorldRender::draw instanced m2 bucket begin model='"
                       << pair.first->file_key().stringRepr()
                       << "' instances=" << pair.second.size()
                       << " animated=" << pair.first->animated
                       << " animBones=" << pair.first->animBones
                       << std::endl;
            }

            if (classic_effect_debug_enabled() && pair.first->file_key().hasFilepath())
            {
              auto const& model_path = pair.first->file_key().filepath();
              if (is_classic_effect_model_path(model_path))
              {
                static std::set<std::string> logged_effect_buckets;
                if (logged_effect_buckets.insert(model_path).second)
                {
                  std::ostringstream effect_line;
                  effect_line << "WorldRender::classic effect bucket model='"
                              << model_path
                              << "' instances=" << pair.second.size();

                  std::size_t const preview_count = std::min<std::size_t>(pair.second.size(), 8);
                  for (std::size_t preview_index = 0; preview_index < preview_count; ++preview_index)
                  {
                    auto const translation = glm::vec3(pair.second[preview_index][3]);
                    effect_line << " p" << preview_index << "={"
                                << translation.x << ", "
                                << translation.y << ", "
                                << translation.z << "}";
                  }

                  LogDebug << effect_line.str() << std::endl;
                }
              }
            }

            pair.first->renderer()->draw( model_view
                , pair.second
                , m2_shader
                , model_render_state
                , frustum
                , _cull_distance
                , camera_pos
                , _world->model_animtime // frozen when animations are toggled off -> models pause
                , draw_models_with_box
                , model_boxes_to_draw
                , display
            );
            _world->_n_rendered_objects += pair.second.size();

            // Collect particle/ribbon models regardless of the animation toggle: when off we still
            // draw them, just frozen in place (their simulation isn't advanced), instead of hiding them.
            if (!pair.first->_particles.empty() || !pair.first->_ribbons.empty())
            {
              model_with_particles[pair.first] = pair.second.size();
            }

            if (capture_debug_enabled())
            {
              LogDebug << "WorldRender::draw instanced m2 bucket end model='"
                       << pair.first->file_key().stringRepr()
                       << "'" << std::endl;
            }
          }
        }

        if (capture_debug_enabled())
        {
          LogDebug << "WorldRender::draw instanced m2 end" << std::endl;
        }

        /*
        if (draw_doodads_wmo)
        {
          _model_instance_storage.for_each_wmo_instance([&] (WMOInstance& wmo)
            {
              auto doodads = wmo.get_doodads(draw_hidden_models);

              if (!doodads)
                return;

              static std::vector<ModelInstance*> instance_temp = {nullptr};
              for (auto& pair : *doodads)
              {
                for (auto& doodad : pair.second)
                {
                  instance_temp[0] = &doodad;
                  doodad.model->draw( model_view
                    , instance_temp
                    , m2_shader
                    , model_render_state
                    , frustum
                    , culldistance
                    , camera_pos
                    , animtime
                    , draw_models_with_box
                    , model_boxes_to_draw
                    , display
                  );
                }

              }
            });
        }

                  */
      }

    }

    if (!creature_spawn_instances_to_draw.empty())
    {
      // Unit blob shadows (faithful to the client's ShadowBlob decal under units): a soft dark circle
      // under each creature, grounding it on the terrain. Doodads don't get this (they use baked MCSH).
      // Procedural radial blob == ShadowBlob.blp's look without a texture. Drawn first so creature meshes
      // sit on top; depth-tested against terrain with depth-write off (terrain in front still occludes).
      {
        OpenGL::Scoped::use_program blob_shader {*_blob_shadow_program.get()};
        OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const cull;
        OpenGL::Scoped::depth_mask_setter<GL_FALSE> const depth_mask;
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        blob_shader.uniform("model_view_projection", mvp);
        blob_shader.uniform("strength", 0.45f);
        gl.bindVertexArray(_bloom_vao); // reuse an empty VAO (corners come from gl_VertexID)
        for (auto const& draw_item : creature_spawn_instances_to_draw)
        {
          ModelInstance* instance = draw_item.instance;
          if (!instance || !instance->model.get() || instance->model->loading_failed())
          {
            continue;
          }
          if (!draw_hidden_models && instance->model->is_hidden())
          {
            continue;
          }
          float foot = instance->size_cat * 0.35f; // size_cat = longest world-space AABB side (scaled)
          if (!std::isfinite(foot) || foot <= 0.0f) { foot = 2.0f; }
          float const radius = glm::clamp(foot, 0.6f, 14.0f);
          glm::vec3 const p = instance->get_pos();
          if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
          {
            continue;
          }
          blob_shader.uniform("center", glm::vec3(p.x, p.y + 0.1f, p.z));
          blob_shader.uniform("radius", radius);
          gl.drawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);
        }
      }

      OpenGL::Scoped::use_program m2_shader {*_m2_program.get()};

      OpenGL::M2RenderState model_render_state;
      model_render_state.tex_arrays = {0, 0};
      model_render_state.tex_indices = {0, 0};
      model_render_state.tex_unit_lookups = {0, 0};
      gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      gl.disable(GL_BLEND);
      gl.depthMask(GL_TRUE);
      gl.enable(GL_CULL_FACE);
      m2_shader.uniform("blend_mode", 0);
      m2_shader.uniform("unfogged", static_cast<int>(model_render_state.unfogged));
      m2_shader.uniform("unlit",  static_cast<int>(model_render_state.unlit));
      m2_shader.uniform("tex_unit_lookup_1", 0);
      m2_shader.uniform("tex_unit_lookup_2", 0);
      m2_shader.uniform("pixel_shader", 0);

      for (auto const& draw_item : creature_spawn_instances_to_draw)
      {
        auto* instance = draw_item.instance;
        if (!instance || instance->model->loading_failed())
        {
          continue;
        }

        if (draw_hidden_models || !instance->model->is_hidden())
        {
          if (capture_debug_enabled())
          {
            LogDebug << "Creature spawn model draw guid=" << draw_item.guid
                     << " model='" << instance->model->file_key().stringRepr() << "'"
                     << " pos={" << instance->pos.x << ", " << instance->pos.y << ", " << instance->pos.z << "}"
                     << " scale=" << instance->scale
                     << std::endl;
          }

          // model_animtime freezes when animations are toggled off, so the creature pauses in place
          // (per-instance models would otherwise keep animating off the ever-advancing animtime).
          int const creature_animtime = draw_item.spawn
            ? static_cast<int>(_world->model_animtime) + draw_item.spawn->animation_time_offset
            : static_cast<int>(_world->model_animtime);
          if (draw_model_animations)
          {
            instance->model->animcalc = false;
          }

          instance->model->renderer()->draw(model_view
            , *instance
            , m2_shader
            , model_render_state
            , frustum
            , _cull_distance
            , camera_pos
            , creature_animtime
            , display
          );
          ++_world->_n_rendered_objects;

          // Advance THIS spawn's OWN particle simulation now, while animate() (inside draw() above) has the
          // shared bones + emitter setup at this instance's animation phase. Each spawn keeps its own live
          // particle state (keyed by guid) so its particles stay glued to its own animated body instead of
          // matching only the last-drawn spawn (the shared/global state still updates in tick() for
          // doodads/preview). Gated on animations so particles freeze in place when animations are off.
          if (draw_model_animations && draw_item.spawn)
          {
            if (Model* pmodel = instance->model.get())
            {
              pmodel->swapInstanceEmitterState(draw_item.guid);
              float pdt = _world->models_emitter_dt();
              while (pdt > 0.1f)
              {
                pmodel->updateParticleSystems(0.1f);
                pdt -= 0.1f;
              }
              pmodel->updateParticleSystems(pdt);
              pmodel->swapInstanceEmitterState(draw_item.guid);
            }
          }

          auto* parent_model = instance->model.get();
          if (draw_item.spawn && parent_model)
          {
            for (auto& attachment : draw_item.spawn->attachment_models)
            {
              if (!attachment.model_instance.has_value())
              {
                if (capture_debug_enabled())
                {
                  LogDebug << "Creature attachment draw skip guid=" << draw_item.guid
                           << " attachmentId=" << attachment.attachment_id
                           << " reason=no-instance"
                           << std::endl;
                }
                continue;
              }

              auto& attachment_instance = *attachment.model_instance;
              auto const attachment_path = attachment_instance.model.get()
                ? attachment_instance.model->file_key().stringRepr()
                : std::string("<null>");
              if (!attachment_instance.model.get())
              {
                if (capture_debug_enabled())
                {
                  LogDebug << "Creature attachment draw skip guid=" << draw_item.guid
                           << " attachmentId=" << attachment.attachment_id
                           << " model='" << attachment_path << "'"
                           << " reason=no-model"
                           << std::endl;
                }
                continue;
              }

              if (!attachment_instance.model->finishedLoading()
                  || attachment_instance.model->loading_failed()
                  || attachment_instance.model->is_hidden())
              {
                if (capture_debug_enabled())
                {
                  LogDebug << "Creature attachment draw skip guid=" << draw_item.guid
                           << " attachmentId=" << attachment.attachment_id
                           << " model='" << attachment_path << "'"
                           << " finished=" << attachment_instance.model->finishedLoading()
                           << " failed=" << attachment_instance.model->loading_failed()
                           << " hidden=" << attachment_instance.model->is_hidden()
                           << std::endl;
                }
                continue;
              }

              auto const* attachment_def = find_attachment_def(parent_model, attachment.attachment_id);
              // Aura kit attachment fallbacks: not every creature M2 authors every point. Chest(34)
              // falls back to ChestBloodFront(15); Head(20) to Helm(11).
              if (!attachment_def && attachment.attachment_id == 34)
              {
                attachment_def = find_attachment_def(parent_model, 15);
              }
              if (!attachment_def && attachment.attachment_id == 20)
              {
                attachment_def = find_attachment_def(parent_model, 11);
              }
              if (!attachment_def)
              {
                if (capture_debug_enabled())
                {
                  LogDebug << "Creature attachment draw skip guid=" << draw_item.guid
                           << " attachmentId=" << attachment.attachment_id
                           << " model='" << attachment_path << "'"
                           << " reason=no-attachment-def"
                           << std::endl;
                }
                continue;
              }

              attachment_instance.setTransformMatrix(attachment_world_matrix(*instance, parent_model, attachment_def));
              // Particle transform: models whose emitters ride their parent (flag 0x10) use the full
              // animated attachment matrix; world-space emitters (aura sparkles like RibbonTrail) use
              // the BIND-pose placement (attachment pos only, no animated bone) so live particles stay
              // put while the body animates -- matching the live client.
              attachment.particle_transform = attachment_instance.model->particlesRideParent()
                ? attachment_instance.transformMatrix()
                : instance->transformMatrix()
                  * glm::translate(glm::mat4x4(1.0f), fixCoordSystem(attachment_def->pos));
              if (draw_model_animations)
              {
                attachment_instance.model->animcalc = false;
              }
              if (capture_debug_enabled())
              {
                LogDebug << "Creature attachment draw guid=" << draw_item.guid
                         << " attachmentId=" << attachment.attachment_id
                         << " model='" << attachment_path << "'"
                         << " bone=" << attachment_def->bone
                         << " pos={" << attachment_def->pos.x << ", "
                         << attachment_def->pos.y << ", "
                         << attachment_def->pos.z << "}"
                         << std::endl;
              }
              attachment_instance.model->renderer()->draw(model_view
                , attachment_instance
                , m2_shader
                , model_render_state
                , frustum
                , _cull_distance
                , camera_pos
                , creature_animtime
                , display
                , true
              );
              ++_world->_n_rendered_objects;

              // Advance this attachment's OWN particle simulation (aura effect models like the
              // arcane chest sparkle are pure particle emitters). Same per-instance scheme as the
              // body above, keyed by (guid, attachment slot) so shared effect models (many creatures
              // carry the same aura) each get their own sim. Runs right after draw() so the shared
              // bones/emitter setup are at this attachment's phase.
              if (draw_model_animations)
              {
                std::uint64_t const attachment_key = static_cast<std::uint64_t>(draw_item.guid)
                  | (static_cast<std::uint64_t>(&attachment - draw_item.spawn->attachment_models.data() + 1) << 32);
                Model* amodel = attachment_instance.model.get();
                amodel->swapInstanceEmitterState(attachment_key);
                float adt = _world->models_emitter_dt();
                while (adt > 0.1f)
                {
                  amodel->updateParticleSystems(0.1f);
                  adt -= 0.1f;
                }
                amodel->updateParticleSystems(adt);
                amodel->swapInstanceEmitterState(attachment_key);
              }
            }
          }
        }
      }
    }

    gl.disable(GL_BLEND);
    gl.enable(GL_CULL_FACE);
    gl.depthMask(GL_TRUE);

    // unsigned int wmos_todraw_count = wmos_to_draw.size();
    // unsigned int models_todraw_count = models_to_draw.size();
    _world->_n_rendered_objects += wmos_to_draw.size();

    models_to_draw.clear();
    wmos_to_draw.clear();

    if(draw_models_with_box || (draw_hidden_models && !model_boxes_to_draw.empty()))
    {
      OpenGL::Scoped::use_program m2_box_shader{ *_m2_box_program.get() };

      OpenGL::Scoped::bool_setter<GL_LINE_SMOOTH, GL_TRUE> const line_smooth;
      gl.hint (GL_LINE_SMOOTH_HINT, GL_NICEST);

      for (auto& it : model_boxes_to_draw)
      {
        glm::vec4 color = it.first->is_hidden()
                          ? glm::vec4(0.f, 0.f, 1.f, 1.f)
                          : ( it.first->use_fake_geometry()
                              ? glm::vec4(1.f, 0.f, 0.f, 1.f)
                              : glm::vec4(0.75f, 0.75f, 0.75f, 1.f)
                          )
        ;

        m2_box_shader.uniform("color", color);
        it.first->renderer()->drawBox(m2_box_shader, it.second);
      }
    }

    if (draw_creature_spawns)
    {
      _world->ensureCreatureSpawnsLoaded();
    }

    if (draw_creature_spawns && _world->drawCreatureMarkers() && creature_spawn_markers_enabled() && !_world->creatureSpawns().empty())
    {
      ZoneScopedN("World::draw() : Draw creature spawn markers");
      OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const disable_cull_face;
      OpenGL::Scoped::bool_setter<GL_BLEND, GL_TRUE> const enable_blend;
      OpenGL::Scoped::depth_mask_setter<GL_FALSE> const no_depth_write;
      static bool logged_creature_marker_stats = false;
      std::size_t nearby_spawns = 0;
      float closest_distance = 999999999.0f;
      std::uint32_t closest_guid = 0;
      glm::vec3 closest_pos = glm::vec3(0.0f);

      struct MarkerData { std::uint32_t guid; glm::vec4 color; };
      std::vector<MarkerData> markers;
      markers.reserve(512);

      if (capture_debug_enabled())
      {
        LogDebug << "Creature spawn marker collect begin total=" << _world->creatureSpawns().size()
                 << " camera={" << camera_pos.x << ", " << camera_pos.y << ", " << camera_pos.z << "}"
                 << std::endl;
      }

      for (auto const& spawn : _world->creatureSpawns())
      {
        if (spawn.pending_delete) // marked for deletion -> no marker
        {
          continue;
        }
        float distance = glm::distance(camera_pos, spawn.pos);
        if (distance < closest_distance)
        {
          closest_distance = distance;
          closest_guid = spawn.guid;
          closest_pos = spawn.pos;
        }
        if (distance > creature_spawn_marker_distance) continue;
        ++nearby_spawns;

        // Game-exact circle size: server bounding radius x creature scale (what the client renders
        // via UNIT_FIELD_BOUNDINGRADIUS); falls back to the model footprint when the DB lacks it.
        float const ring_radius = spawn.selectionRingWorldRadius();

        // Terrain-draped disc mesh, cached per spawn and rebuilt only when the spawn moves or its
        // radius changes -- so the circle bends with the ground like the client's instead of being
        // a flat plate clipping into slopes.
        auto& disc = _creature_disc_cache[spawn.guid];
        if (disc.radius != ring_radius || glm::distance(disc.pos, spawn.pos) > 0.001f)
        {
          disc.pos = spawn.pos;
          disc.radius = ring_radius;
          disc.vertices.clear();
          disc.locals.clear();
          disc.indices.clear();

          int constexpr segments = 24;
          float constexpr ring_fractions[] = { 0.35f, 0.65f, 0.85f, 1.0f };
          int constexpr ring_count = 4;

          auto const ground_y = [&](float x, float z) -> float
          {
            // Probe from slightly above the spawn so slopes uphill of it still get hit.
            glm::vec3 const hit = _world->get_ground_height(glm::vec3(x, spawn.pos.y + 3.0f, z));
            // Guard: failed probes return (0,0,0); also ignore hits wildly off the spawn plane
            // (overhangs / WMO roofs) and keep the disc's own plane instead.
            if (hit == glm::vec3(0.0f) || std::abs(hit.y - spawn.pos.y) > ring_radius * 2.0f + 4.0f)
            {
              return spawn.pos.y;
            }
            return hit.y;
          };

          // centre vertex
          disc.vertices.push_back(glm::vec3(spawn.pos.x, ground_y(spawn.pos.x, spawn.pos.z) + 0.08f, spawn.pos.z));
          disc.locals.push_back(glm::vec2(0.0f, 0.0f));

          for (int ring = 0; ring < ring_count; ++ring)
          {
            float const fr = ring_fractions[ring];
            for (int seg = 0; seg < segments; ++seg)
            {
              float const angle = glm::two_pi<float>() * seg / float(segments);
              float const lx = std::cos(angle) * fr;
              float const lz = std::sin(angle) * fr;
              float const wx = spawn.pos.x + lx * ring_radius;
              float const wz = spawn.pos.z + lz * ring_radius;
              disc.vertices.push_back(glm::vec3(wx, ground_y(wx, wz) + 0.08f, wz));
              disc.locals.push_back(glm::vec2(lx, lz));
            }
          }

          auto const ring_start = [&](int ring) { return static_cast<std::uint16_t>(1 + ring * segments); };
          // centre fan to ring 0
          for (int seg = 0; seg < segments; ++seg)
          {
            disc.indices.push_back(0);
            disc.indices.push_back(static_cast<std::uint16_t>(ring_start(0) + seg));
            disc.indices.push_back(static_cast<std::uint16_t>(ring_start(0) + (seg + 1) % segments));
          }
          // quads between consecutive rings
          for (int ring = 0; ring + 1 < ring_count; ++ring)
          {
            for (int seg = 0; seg < segments; ++seg)
            {
              std::uint16_t const a = static_cast<std::uint16_t>(ring_start(ring) + seg);
              std::uint16_t const b = static_cast<std::uint16_t>(ring_start(ring) + (seg + 1) % segments);
              std::uint16_t const c = static_cast<std::uint16_t>(ring_start(ring + 1) + seg);
              std::uint16_t const d = static_cast<std::uint16_t>(ring_start(ring + 1) + (seg + 1) % segments);
              disc.indices.insert(disc.indices.end(), { a, c, d, a, d, b });
            }
          }
        }

        // Selected = the client's yellow target blob; the rest keep editor-distinct colors.
        glm::vec4 color = spawn.selected ? glm::vec4(1.0f, 0.85f, 0.13f, 1.0f)
            : spawn.hovered  ? glm::vec4(0.2f, 0.9f,  1.0f,  1.0f)
            : spawn.dirty    ? glm::vec4(0.2f, 1.0f,  0.35f, 1.0f)
                 : glm::vec4(1.0f, 0.55f, 0.08f, 1.0f);
        markers.push_back({spawn.guid, color});
      }

      if (capture_debug_enabled())
      {
        LogDebug << "Creature spawn marker draw begin nearby=" << nearby_spawns
                 << " markers=" << markers.size()
                 << " closestGuid=" << closest_guid
                 << " closestDistance=" << closest_distance
                 << std::endl;
      }

      gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      gl.enable(GL_DEPTH_TEST);

      // Orient the crescent texture so its bright arc faces the camera (the client's "incomplete
      // circle" points its opening toward the viewer). Same rotation for every disc this frame.
      auto const uv_rotation_for = [&](glm::vec3 const& pos) -> float
      {
        glm::vec2 const to_cam(camera_pos.x - pos.x, camera_pos.z - pos.z);
        return std::atan2(to_cam.x, to_cam.y);
      };

      // Pass 1: visible pixels (normal depth, full alpha)
      gl.depthFunc(GL_LEQUAL);
      for (auto const& m : markers)
      {
        auto const& disc = _creature_disc_cache[m.guid];
        _circle_render.drawWorldSpace(mvp, disc.vertices, disc.locals, disc.indices, m.color,
                                      uv_rotation_for(disc.pos));
      }

      // Pass 2: occluded pixels (behind terrain, 10% alpha)
      gl.depthFunc(GL_GREATER);
      for (auto const& m : markers)
      {
        auto const& disc = _creature_disc_cache[m.guid];
        glm::vec4 c = m.color;
        c.a *= 0.1f;
        _circle_render.drawWorldSpace(mvp, disc.vertices, disc.locals, disc.indices, c,
                                      uv_rotation_for(disc.pos));
      }

      // Restore depth function
      gl.depthFunc(GL_LEQUAL);

      if (capture_debug_enabled())
      {
        LogDebug << "Creature spawn marker draw end markers=" << markers.size() << std::endl;
      }

      if (!logged_creature_marker_stats || capture_debug_enabled())
      {
        LogDebug << "Creature spawn marker draw: total=" << _world->creatureSpawns().size()
                 << ", nearby=" << nearby_spawns
                 << ", drawn=" << markers.size()
                 << ", markerDistance=" << creature_spawn_marker_distance
                 << ", camera={" << camera_pos.x << ", " << camera_pos.y << ", " << camera_pos.z << "}"
                 << ", closestGuid=" << closest_guid
                 << ", closestDistance=" << closest_distance
                 << ", closestPos={" << closest_pos.x << ", " << closest_pos.y << ", " << closest_pos.z << "}"
                 << std::endl;
        logged_creature_marker_stats = true;
      }
    }

    // GameObject spawn markers (the disc under each gameobject) -- only in the gameobject tool.
    if (draw_gameobject_spawns && _world->drawGameObjectMarkers() && creature_spawn_markers_enabled()
        && !_world->gameObjectSpawns().empty())
    {
      ZoneScopedN("World::draw() : Draw gameobject spawn markers");
      OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const disable_cull_face;
      OpenGL::Scoped::bool_setter<GL_BLEND, GL_TRUE> const enable_blend;
      OpenGL::Scoped::depth_mask_setter<GL_FALSE> const no_depth_write;

      struct MarkerData { std::uint32_t guid; glm::vec4 color; };
      std::vector<MarkerData> markers;
      markers.reserve(256);

      for (auto const& spawn : _world->gameObjectSpawns())
      {
        if (spawn.pending_delete)
          continue;
        if (glm::distance(camera_pos, spawn.pos) > creature_spawn_marker_distance)
          continue;

        float ring_radius = 0.5f;
        if (spawn.model_instance.has_value())
        {
          ring_radius = spawn.model_instance.value().selectionRingRadius();
        }
        ring_radius = std::max(0.25f, ring_radius);

        // Same terrain-draped disc mesh as creature markers, cached per spawn and rebuilt only when
        // the spawn moves or its radius changes -- so the circle bends with the ground instead of
        // being a flat plate clipping into slopes.
        auto& disc = _gameobject_disc_cache[spawn.guid];
        if (disc.radius != ring_radius || glm::distance(disc.pos, spawn.pos) > 0.001f)
        {
          disc.pos = spawn.pos;
          disc.radius = ring_radius;
          disc.vertices.clear();
          disc.locals.clear();
          disc.indices.clear();

          int constexpr segments = 24;
          float constexpr ring_fractions[] = { 0.35f, 0.65f, 0.85f, 1.0f };
          int constexpr ring_count = 4;

          auto const ground_y = [&](float x, float z) -> float
          {
            glm::vec3 const hit = _world->get_ground_height(glm::vec3(x, spawn.pos.y + 3.0f, z));
            if (hit == glm::vec3(0.0f) || std::abs(hit.y - spawn.pos.y) > ring_radius * 2.0f + 4.0f)
            {
              return spawn.pos.y;
            }
            return hit.y;
          };

          // centre vertex
          disc.vertices.push_back(glm::vec3(spawn.pos.x, ground_y(spawn.pos.x, spawn.pos.z) + 0.08f, spawn.pos.z));
          disc.locals.push_back(glm::vec2(0.0f, 0.0f));

          for (int ring = 0; ring < ring_count; ++ring)
          {
            float const fr = ring_fractions[ring];
            for (int seg = 0; seg < segments; ++seg)
            {
              float const angle = glm::two_pi<float>() * seg / float(segments);
              float const lx = std::cos(angle) * fr;
              float const lz = std::sin(angle) * fr;
              float const wx = spawn.pos.x + lx * ring_radius;
              float const wz = spawn.pos.z + lz * ring_radius;
              disc.vertices.push_back(glm::vec3(wx, ground_y(wx, wz) + 0.08f, wz));
              disc.locals.push_back(glm::vec2(lx, lz));
            }
          }

          auto const ring_start = [&](int ring) { return static_cast<std::uint16_t>(1 + ring * segments); };
          // centre fan to ring 0
          for (int seg = 0; seg < segments; ++seg)
          {
            disc.indices.push_back(0);
            disc.indices.push_back(static_cast<std::uint16_t>(ring_start(0) + seg));
            disc.indices.push_back(static_cast<std::uint16_t>(ring_start(0) + (seg + 1) % segments));
          }
          // quads between consecutive rings
          for (int ring = 0; ring + 1 < ring_count; ++ring)
          {
            for (int seg = 0; seg < segments; ++seg)
            {
              std::uint16_t const a = static_cast<std::uint16_t>(ring_start(ring) + seg);
              std::uint16_t const b = static_cast<std::uint16_t>(ring_start(ring) + (seg + 1) % segments);
              std::uint16_t const c = static_cast<std::uint16_t>(ring_start(ring + 1) + seg);
              std::uint16_t const d = static_cast<std::uint16_t>(ring_start(ring + 1) + (seg + 1) % segments);
              disc.indices.insert(disc.indices.end(), { a, c, d, a, d, b });
            }
          }
        }

        // Distinct palette from creatures (which are orange/green): gameobjects use blue/purple.
        glm::vec4 const color = spawn.selected ? glm::vec4(0.35f, 1.0f, 0.45f, 1.0f)
            : spawn.hovered  ? glm::vec4(0.45f, 0.85f, 1.0f, 1.0f)
            : spawn.dirty    ? glm::vec4(1.0f,  0.9f,  0.15f, 1.0f)
                 : glm::vec4(0.55f, 0.45f, 1.0f, 1.0f);
        markers.push_back({spawn.guid, color});
      }

      gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      gl.enable(GL_DEPTH_TEST);

      // Orient the crescent texture so its bright arc faces the camera (matches creature markers).
      auto const uv_rotation_for = [&](glm::vec3 const& pos) -> float
      {
        glm::vec2 const to_cam(camera_pos.x - pos.x, camera_pos.z - pos.z);
        return std::atan2(to_cam.x, to_cam.y);
      };

      // Pass 1: visible pixels (normal depth, full alpha)
      gl.depthFunc(GL_LEQUAL);
      for (auto const& m : markers)
      {
        auto const& disc = _gameobject_disc_cache[m.guid];
        _circle_render.drawWorldSpace(mvp, disc.vertices, disc.locals, disc.indices, m.color,
                                      uv_rotation_for(disc.pos));
      }

      // Pass 2: occluded pixels (behind terrain, 10% alpha)
      gl.depthFunc(GL_GREATER);
      for (auto const& m : markers)
      {
        auto const& disc = _gameobject_disc_cache[m.guid];
        glm::vec4 c = m.color;
        c.a *= 0.1f;
        _circle_render.drawWorldSpace(mvp, disc.vertices, disc.locals, disc.indices, c,
                                      uv_rotation_for(disc.pos));
      }
      gl.depthFunc(GL_LEQUAL);
    }

    // Creature patrol paths: draw a line through each patrolling creature's waypoints (spawn -> wp1
    // -> wp2 -> ...). Only in creature mode (drawCreatureMarkers) and when the path overlay is on.
    if (draw_creature_spawns && _world->drawCreatureMarkers() && _world->drawCreaturePatrolPaths()
        && !_world->creatureSpawns().empty())
    {
      ZoneScopedN("World::draw() : Draw creature patrol paths");
      _world->ensureCreaturePatrolPathsLoaded();

      auto const& paths = _world->creaturePatrolPaths();
      if (!paths.empty())
      {
        // A path becomes visible only once its creature is within the creature render distance (so it
        // appears together with the NPC). Once shown, the whole route is drawn regardless of how far
        // the waypoints extend.
        float const path_show_distance = creature_spawn_model_distance;

        // A small palette of bright, saturated colors that read against terrain (no greens/browns).
        // Each creature gets a stable color from its guid so neighbouring routes stay distinguishable.
        static glm::vec4 const palette[] = {
          {1.00f, 0.20f, 0.20f, 0.95f}, // red
          {1.00f, 0.55f, 0.05f, 0.95f}, // orange
          {1.00f, 0.95f, 0.15f, 0.95f}, // yellow
          {0.20f, 0.85f, 1.00f, 0.95f}, // cyan
          {0.45f, 0.45f, 1.00f, 0.95f}, // blue
          {1.00f, 0.35f, 0.95f, 0.95f}, // magenta
          {1.00f, 1.00f, 1.00f, 0.95f}, // white
        };
        constexpr int palette_size = static_cast<int>(sizeof(palette) / sizeof(palette[0]));

        // When one or more creatures are selected, only show the selected creatures' routes so the
        // focused NPC's path stands out; with nothing selected, show every nearby route.
        bool const any_selected = std::any_of(_world->creatureSpawns().begin(), _world->creatureSpawns().end(),
          [](World::CreatureSpawnOverlay const& s) { return s.selected && !s.pending_delete; });

        OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const disable_cull_face;
        OpenGL::Scoped::bool_setter<GL_BLEND, GL_TRUE> const enable_blend;
        OpenGL::Scoped::bool_setter<GL_LINE_SMOOTH, GL_TRUE> const line_smooth;
        OpenGL::Scoped::depth_mask_setter<GL_FALSE> const no_depth_write;
        gl.hint(GL_LINE_SMOOTH_HINT, GL_NICEST);
        gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl.lineWidth(2.5f);
        gl.enable(GL_DEPTH_TEST);
        gl.depthFunc(GL_LEQUAL); // respect terrain occlusion -> proper line-of-sight, not drawn through

        for (auto const& spawn : _world->creatureSpawns())
        {
          if (spawn.pending_delete)
            continue;
          if (any_selected && !spawn.selected) // only the selected creature's path while one is selected
            continue;
          if (glm::distance(camera_pos, spawn.pos) > path_show_distance)
            continue;

          auto it = paths.find(spawn.guid);
          if (it == paths.end() || it->second.empty())
            continue;

          // Start the line at the (possibly just-moved) spawn position so it tracks live edits, then
          // run through the authored waypoints. Lift slightly so it doesn't z-fight the ground.
          std::vector<glm::vec3> points;
          points.reserve(it->second.size() + 1);
          points.push_back(spawn.pos + glm::vec3(0.0f, 0.4f, 0.0f));
          for (auto const& wp : it->second)
            points.push_back(wp + glm::vec3(0.0f, 0.4f, 0.0f));

          if (points.size() < 2)
            continue;

          _line_render.draw(mvp, points, palette[spawn.guid % palette_size], false);
        }

        gl.lineWidth(1.0f);
      }
    }

    for (auto& selection : _world->current_selection())
    {
      if (selection.index() == eEntry_Object)
      {
        auto obj = std::get<selected_object_type>(selection);

        if (obj->which() != eMODEL)
          continue;

        auto model = static_cast<ModelInstance*>(obj);



        if (model->isInFrustum(frustum) && model->isInRenderDist(_cull_distance, camera_pos, display))
        {
          bool is_selected = false;
          /*
          auto id = model->uid;
          bool const is_selected = _world->current_selection().size() > 0 &&
              std::find_if(_world->current_selection().begin(), _world->current_selection().end(),
                  [id](selection_type type)
                  {
                      return var_type(type) == typeid(selected_object_type)
                          && std::get<selected_object_type>(type)->which() == SceneObjectTypes::eMODEL
                          && static_cast<ModelInstance*>(std::get<selected_object_type>(type))->uid == id;
                  }) != _world->current_selection().end();*/

          model->draw_box(model_view, projection, is_selected); // make optional!
        }
      }

    }

    if (capture_debug_enabled())
    {
      LogDebug << "WorldRender::draw m2 end" << std::endl;
    }
  }

  // render selection group boxes
  for (auto& selection_group : _world->_selection_groups)
  {
      if (!selection_group.isSelected())
          continue;

      glm::mat4x4 identity_mtx = glm::mat4x4{ 1 };
      auto& extents = selection_group.getExtents();
      Noggit::Rendering::Primitives::WireBox::getInstance(_world->_context).draw(model_view
          , projection
          , identity_mtx
          , { 0.0f, 0.0f, 1.0f, 1.0f } // blue
          , extents[0]
          , extents[1]
      );
  }

  // set anim time only once per frame
  {
    OpenGL::Scoped::use_program water_shader {*_liquid_program.get()};
    water_shader.uniform("camera", glm::vec3(camera_pos.x, camera_pos.y, camera_pos.z));
    water_shader.uniform("animtime", _world->animtime);
    water_shader.uniform("draw_shadows", _terrain_params_ubo_data.draw_shadows);


    if (draw_wmo || _world->mapIndex.hasAGlobalWMO())
    {
      water_shader.uniform("use_transform", 1);
    }
  }
  // Draw water BEFORE the additive model particles/ribbons and the deferred light effects, so
  // those additive glows paint OVER the (now opaque-in-deep-water) water instead of being hidden.
  gl.enable(GL_BLEND);
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  if (draw_water)
  {
    if (capture_debug_enabled())
    {
      LogDebug << "WorldRender::draw water begin" << std::endl;
    }

    ZoneScopedN("World::draw() : Draw water");

    // draw the water on both sides
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const cull;

    // Translucent water must NOT write depth. Solids drawn earlier already populated the depth
    // buffer (so water is still correctly occluded by piers/terrain in front of it). By leaving
    // the depth buffer at the solid geometry, the additive light effects drawn AFTER the water
    // test only against solids -> a lighthouse beam shows fully over the water surface (even the
    // part below it) yet is still hidden behind the mountain. If water wrote depth, the beam's
    // below-surface span would be depth-rejected ("pokes out only where it can").
    OpenGL::Scoped::depth_mask_setter<GL_FALSE> const no_depth_write;

    OpenGL::Scoped::use_program water_shader{ *_liquid_program.get()};

    gl.bindVertexArray(_liquid_chunk_vao);

    water_shader.uniform ("use_transform", 0);

    // Dev water-opacity lever (settings: water/transparency, 0..1; 1 = unchanged). Read live so
    // dragging the Settings slider updates the water immediately.
    water_shader.uniform ("water_alpha_mult"
                          , _world->_settings->value("water/transparency", 1.0f).toFloat());

    for (auto& pair : _world->_loaded_tiles_buffer)
    {
      MapTile* tile = pair.second;

      if (!tile)
        break;

      if (tile->renderer()->isOccluded() && !tile->Water.needsUpdate() && !tile->renderer()->isOverridingOcclusionCulling())
        continue;

      tile->Water.renderer()->draw(
          frustum
          , camera_pos
          , camera_moved
          , water_shader
          , _world->animtime
          , water_layer
          , display
          , &_liquid_texture_manager
      );
    }

    gl.bindVertexArray(0);

    if (capture_debug_enabled())
    {
      LogDebug << "WorldRender::draw water end" << std::endl;
    }
  }

  // Deferred pure-additive light effects (god rays / lighthouse beams), drawn AFTER the water so
  // the opaque deep water no longer paints over them. prepareDraw applies each pass's additive
  // blend + no-depth-write, so they brighten the water surface instead of being occluded by it.
  if (!deferred_light_effects.empty())
  {
    OpenGL::Scoped::use_program m2_shader {*_m2_instanced_program.get()};

    OpenGL::M2RenderState model_render_state;
    model_render_state.tex_arrays = {0, 0};
    model_render_state.tex_indices = {0, 0};
    model_render_state.tex_unit_lookups = {0, 0};
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.disable(GL_BLEND);
    gl.depthMask(GL_TRUE);
    gl.enable(GL_CULL_FACE);
    m2_shader.uniform("blend_mode", 0);
    m2_shader.uniform("unfogged", static_cast<int>(model_render_state.unfogged));
    m2_shader.uniform("unlit",  static_cast<int>(model_render_state.unlit));
    m2_shader.uniform("tex_unit_lookup_1", 0);
    m2_shader.uniform("tex_unit_lookup_2", 0);
    m2_shader.uniform("pixel_shader", 0);

    std::unordered_map<Model*, std::size_t> unused_boxes;

    for (Model* model : deferred_light_effects)
    {
      auto const it = models_to_draw.find(model);
      if (it == models_to_draw.end())
        continue;

      model->renderer()->draw( model_view
          , it->second
          , m2_shader
          , model_render_state
          , frustum
          , _cull_distance
          , camera_pos
          , _world->animtime
          , false
          , unused_boxes
          , display
      );
      _world->_n_rendered_objects += it->second.size();
    }

    // Restore the default opaque state for anything drawn afterwards.
    gl.disable(GL_BLEND);
    gl.enable(GL_CULL_FACE);
    gl.depthMask(GL_TRUE);
  }

  // model particles (drawn after water so additive glows appear on top of the water surface).
  // Drawn even when animations are off -> particles freeze in place (not advanced) instead of vanishing.
  if (!model_with_particles.empty() || !creature_spawn_instances_to_draw.empty())
  {
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const cull;
    OpenGL::Scoped::bool_setter<GL_DEPTH_TEST, GL_TRUE> const depth_test;
    OpenGL::Scoped::depth_mask_setter<GL_FALSE> const depth_mask;

    OpenGL::Scoped::use_program particles_shader {*_m2_particles_program.get()};

    particles_shader.uniform("model_view_projection", mvp);
    particles_shader.uniform("camera", camera_pos); // for per-particle fog distance
    OpenGL::texture::set_active_texture(0);
    particles_shader.uniform("tex", 0);

    // Particles OWN the bloom mask under themselves now (alpha write ON). Each particle's blend uses
    // glBlendFuncSeparate (see Particle.cpp) so its RGB is unchanged but its alpha MULTIPLIES the mask by
    // (1 - coverage): a bright additive fire erases the emissive mask it would otherwise inherit from the
    // lava pit behind it, so it stops blooming through the strong emissive path (the Ironforge forge /
    // firepit plumes were blowing out to a white screen-filling halo). The lava around the flame keeps
    // its mask and still glows; faint particle edges barely touch it. Particles are never themselves
    // emissive, so erasing (never raising) the mask is the data-faithful behaviour.
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    particles_shader.uniform("particle_alpha_mod", 1.0f); // batched world particles: full opacity (no creature fade)
    for (auto& it : model_with_particles)
    {
      it.first->renderer()->drawParticles(glm::transpose(model_view), particles_shader, it.second);
    }
    // Creature spawn particles (sparkle/energy spell effects on the model). Spawns render via their own
    // per-instance path and are NOT in model_with_particles, so their emitters were never drawn -- the
    // arcane-elemental sparkles etc. were missing. Draw each spawn's particles with its own transform.
    for (auto const& draw_item : creature_spawn_instances_to_draw)
    {
      auto* instance = draw_item.instance;
      if (!instance || !instance->model.get() || instance->model->loading_failed())
        continue;
      if (!draw_hidden_models && instance->model->is_hidden())
        continue;
      // Draw THIS spawn's own per-instance particle state (updated at its own phase in the body pass).
      // Only swap it in when animations are on -- otherwise fall through to the shared frozen state so
      // particles freeze in place (matching the body, which also freezes) when animations are toggled off.
      Model* pmodel = instance->model.get();
      bool const use_instance_state = draw_model_animations && draw_item.spawn;
      if (use_instance_state)
      {
        pmodel->swapInstanceEmitterState(draw_item.guid);
      }
      pmodel->renderer()->drawParticlesForInstance(
          glm::transpose(model_view), particles_shader, instance->transformMatrix(), instance->model_alpha);
      if (use_instance_state)
      {
        pmodel->swapInstanceEmitterState(draw_item.guid);
      }

      // Attachment-model particles (aura state-kit effects like the arcane chest sparkle are pure
      // particle emitters -- without this they would never render). Each attachment's transform was
      // set during the body pass (attachment_world_matrix), so it already rides the animated bone.
      if (draw_item.spawn)
      {
        for (auto& attachment : draw_item.spawn->attachment_models)
        {
          if (!attachment.model_instance.has_value())
            continue;
          Model* amodel = attachment.model_instance->model.get();
          if (!amodel || !amodel->finishedLoading() || amodel->loading_failed() || amodel->is_hidden())
            continue;

          std::uint64_t const attachment_key = static_cast<std::uint64_t>(draw_item.guid)
            | (static_cast<std::uint64_t>(&attachment - draw_item.spawn->attachment_models.data() + 1) << 32);
          if (draw_model_animations)
          {
            amodel->swapInstanceEmitterState(attachment_key);
          }
          // Full alpha: aura/spell effect models are separate from the creature, so CreatureModelAlpha
          // (the body fade, e.g. Anomalus 0.784) does NOT apply to them in the client.
          amodel->renderer()->drawParticlesForInstance(
              glm::transpose(model_view), particles_shader,
              attachment.particle_transform, 1.0f);
          if (draw_model_animations)
          {
            amodel->swapInstanceEmitterState(attachment_key);
          }
        }
      }
    }
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // restore default for following passes
  }


  if (!model_with_particles.empty()) // ribbons too: drawn frozen when animations are off
  {
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const cull;
    OpenGL::Scoped::depth_mask_setter<GL_FALSE> const depth_mask;

    OpenGL::Scoped::use_program ribbon_shader {*_m2_ribbons_program.get()};

    ribbon_shader.uniform("model_view_projection", mvp);

    // Additive RGB unchanged; alpha pulls the bloom mask down under the ribbon (same rationale as the
    // particle pass) so additive ribbons don't bloom through an emissive surface they happen to cross.
    gl.blendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);

    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (auto& it : model_with_particles)
    {
      it.first->renderer()->drawRibbons(ribbon_shader, it.second);
    }
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }

  if (angled_mode || use_ref_pos)
  {
    ZoneScopedN("World::draw() : Draw angles");
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> cull;
    OpenGL::Scoped::depth_mask_setter<GL_FALSE> const depth_mask;

    math::degrees orient = math::degrees(orientation);
    math::degrees incl = math::degrees(angle);
    glm::vec4 color = cursor_color;
    // always half transparent regardless or the cursor transparency
    color.w = 0.5f;

    float radius = 1.2f * brush_radius;

    if (angled_mode && terrainMode == editing_mode::flatten_blur)
    {
        if (angle > 49.0f) // 0.855 radian
        {
            color.x = 1.f;
            color.y = 0.f;
            color.z = 0.f;
        }
    }

    if (angled_mode && !use_ref_pos)
    {
      glm::vec3 pos = cursor_pos;
      pos.y += 0.1f; // to avoid z-fighting with the ground
      _square_render.draw(mvp, pos, radius, incl, orient, color);
    }
    else if (use_ref_pos)
    {
      if (angled_mode)
      {
        glm::vec3 pos = cursor_pos;
        pos.y = misc::angledHeight(ref_pos, pos, incl, orient);
        pos.y += 0.1f;
        _square_render.draw(mvp, pos, radius, incl, orient, color);

        // display the plane when the cursor is far from ref_point
        if (misc::dist(pos.x, pos.z, ref_pos.x, ref_pos.z) > 10.f + radius)
        {
          glm::vec3 ref = ref_pos;
          ref.y += 0.1f;
          _square_render.draw(mvp, ref, 10.f, incl, orient, color);
        }
      }
      else
      {
        glm::vec3 pos = cursor_pos;
        pos.y = ref_pos.y + 0.1f;
        _square_render.draw(mvp, pos, radius, math::degrees(0.f), math::degrees(0.f), color);
      }
    }
  }

  gl.enable(GL_BLEND);

  // draw last because of the transparency
  if (draw_mfbo)
  {
    ZoneScopedN("World::draw() : Draw flight bounds");
    // don't write on the depth buffer
    OpenGL::Scoped::depth_mask_setter<GL_FALSE> const depth_mask;

    OpenGL::Scoped::use_program mfbo_shader {*_mfbo_program.get()};

    for (MapTile* tile : _world->mapIndex.loaded_tiles())
    {
      tile->flightBoundsRenderer()->draw(mfbo_shader);
    }
  }

  if (terrainMode == editing_mode::light)
  {
      Sky* CurrentSky = skies()->findClosestSkyByDistance(camera_pos);
      if (!CurrentSky)
          return;

      int CurrentSkyID = CurrentSky->Id;
          
      const int MAX_TIME_VALUE_C = 2880;
      const int CurrenTime = static_cast<int>(_world->time) % MAX_TIME_VALUE_C;

      glCullFace(GL_FRONT);
      for (Sky& sky : skies()->skies)
      {
          if (CurrentSkyID > 1 && draw_only_inside_light_sphere)
              break;

          if (CurrentSkyID == sky.Id)
              continue;

          if (glm::distance(sky.pos, camera_pos) <= _cull_distance) // TODO: frustum cull here
          {
              glm::vec4 diffuse = { sky.colorFor(LIGHT_GLOBAL_DIFFUSE, CurrenTime), 1.f };
              glm::vec4 ambient = { sky.colorFor(LIGHT_GLOBAL_AMBIENT, CurrenTime), 1.f };

              _sphere_render.draw(mvp, sky.pos, ambient, sky.r1, 32, 18, alpha_light_sphere, false, draw_wireframe_light_sphere);
              _sphere_render.draw(mvp, sky.pos, diffuse, sky.r2, 32, 18, alpha_light_sphere, false, draw_wireframe_light_sphere);
          }
      }

      glCullFace(GL_BACK);
      if (CurrentSky && draw_only_inside_light_sphere)
      {
          glm::vec4 diffuse = { CurrentSky->colorFor(LIGHT_GLOBAL_DIFFUSE, CurrenTime), 1.f };
          glm::vec4 ambient = { CurrentSky->colorFor(LIGHT_GLOBAL_AMBIENT, CurrenTime), 1.f };

          _sphere_render.draw(mvp, CurrentSky->pos, ambient, CurrentSky->r1, 32, 18, alpha_light_sphere, false, draw_wireframe_light_sphere);
          _sphere_render.draw(mvp, CurrentSky->pos, diffuse, CurrentSky->r2, 32, 18, alpha_light_sphere, false, draw_wireframe_light_sphere);
      }
  }

  // Bloom: scene is fully rendered into the offscreen target now -> extract bright, blur, and
  // composite (scene + bloom) back onto the framebuffer that was bound when we entered (Qt's).
  if (do_bloom)
  {
    renderBloomAndComposite(static_cast<GLuint>(bloom_prev_fbo), bloom_vp[2], bloom_vp[3]);
  }
}

void WorldRender::ensureBloomTargets(int w, int h)
{
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  int const bw = std::max(1, w / 2);
  int const bh = std::max(1, h / 2);

  if (!_bloom_initialized)
  {
    _bloom_bright_program.reset(new OpenGL::program
      { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("bloom_quad_vs") }
      , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("bloom_bright_fs") } });
    _bloom_blur_program.reset(new OpenGL::program
      { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("bloom_quad_vs") }
      , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("bloom_blur_fs") } });
    _bloom_composite_program.reset(new OpenGL::program
      { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("bloom_quad_vs") }
      , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("bloom_composite_fs") } });

    gl.genVertexArrays(1, &_bloom_vao);
    gl.genFramebuffers(1, &_bloom_scene_fbo);
    gl.genTextures(1, &_bloom_scene_color);
    gl.genRenderbuffers(1, &_bloom_scene_depth);
    gl.genFramebuffers(2, _bloom_fbo);
    gl.genTextures(2, _bloom_tex);

    _bloom_initialized = true;
    _bloom_w = _bloom_h = -1; // force the (re)allocation below
  }

  if (_bloom_w == w && _bloom_h == h)
  {
    return;
  }

  _bloom_w = w;
  _bloom_h = h;
  _bloom_bw = bw;
  _bloom_bh = bh;

  auto alloc_color = [](GLuint tex, int tw, int th)
  {
    gl.bindTexture(GL_TEXTURE_2D, tex);
    gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  };

  gl.activeTexture(GL_TEXTURE0);

  // full-res scene colour + a combined depth/STENCIL buffer. The stencil is required by the WMO
  // water-overlap dedupe -- without it that pass silently no-ops when rendering into the bloom FBO.
  alloc_color(_bloom_scene_color, w, h);
  gl.bindRenderbuffer(GL_RENDERBUFFER, _bloom_scene_depth);
  gl.renderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
  gl.bindFramebuffer(GL_FRAMEBUFFER, _bloom_scene_fbo);
  gl.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _bloom_scene_color, 0);
  gl.framebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, _bloom_scene_depth);

  // half-res ping-pong targets for the blur
  for (int i = 0; i < 2; ++i)
  {
    alloc_color(_bloom_tex[i], bw, bh);
    gl.bindFramebuffer(GL_FRAMEBUFFER, _bloom_fbo[i]);
    gl.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _bloom_tex[i], 0);
  }
}

void WorldRender::renderBloomAndComposite(GLuint target_fbo, int w, int h)
{
  gl.disable(GL_DEPTH_TEST);
  gl.disable(GL_BLEND);
  gl.bindVertexArray(_bloom_vao);

  // 1) bright pass: full-res scene colour -> half-res _bloom_tex[0]
  gl.bindFramebuffer(GL_FRAMEBUFFER, _bloom_fbo[0]);
  gl.viewport(0, 0, _bloom_bw, _bloom_bh);
  {
    OpenGL::Scoped::use_program p{*_bloom_bright_program};
    gl.activeTexture(GL_TEXTURE0);
    gl.bindTexture(GL_TEXTURE_2D, _bloom_scene_color);
    p.uniform("scene", 0);
    // Emissive bloom knee (bloom_bright_frag is now emissive-ONLY -- gates emissive pixels by
    // maxchannel > threshold*0.55). 0.50 is the value Molten Core's lava read correctly at; there is no
    // longer a luminance path, so this no longer affects lit surfaces at all.
    p.uniform("threshold", 0.50f);
    gl.drawArraysInstanced(GL_TRIANGLES, 0, 3, 1);
  }

  // 2) separable gaussian blur, ping-ponging between the two half-res targets
  bool horizontal = true;
  int const passes = 6; // 3 H/V pairs -- tighter blur so bloom doesn't smear across the terrain
  for (int i = 0; i < passes; ++i)
  {
    int const src = horizontal ? 0 : 1;
    int const dst = horizontal ? 1 : 0;
    gl.bindFramebuffer(GL_FRAMEBUFFER, _bloom_fbo[dst]);
    gl.viewport(0, 0, _bloom_bw, _bloom_bh);

    OpenGL::Scoped::use_program p{*_bloom_blur_program};
    gl.activeTexture(GL_TEXTURE0);
    gl.bindTexture(GL_TEXTURE_2D, _bloom_tex[src]);
    p.uniform("image", 0);
    p.uniform("horizontal", horizontal ? 1 : 0);
    p.uniform("texel", glm::vec2(1.f / static_cast<float>(_bloom_bw), 1.f / static_cast<float>(_bloom_bh)));
    gl.drawArraysInstanced(GL_TRIANGLES, 0, 3, 1);

    horizontal = !horizontal;
  }
  // even number of passes -> final blurred result is back in _bloom_tex[0]

  // 3) composite scene + bloom -> the framebuffer that was bound on entry (Qt's default), full res
  gl.bindFramebuffer(GL_FRAMEBUFFER, target_fbo);
  gl.viewport(0, 0, w, h);
  {
    OpenGL::Scoped::use_program p{*_bloom_composite_program};
    gl.activeTexture(GL_TEXTURE0);
    gl.bindTexture(GL_TEXTURE_2D, _bloom_scene_color);
    p.uniform("scene", 0);
    gl.activeTexture(GL_TEXTURE1);
    gl.bindTexture(GL_TEXTURE_2D, _bloom_tex[0]);
    p.uniform("bloom", 1);
    p.uniform("intensity", 0.8f); // bloom add-back strength (Molten Core value; emissive-only now)
    gl.drawArraysInstanced(GL_TRIANGLES, 0, 3, 1);
  }

  gl.activeTexture(GL_TEXTURE0);
  gl.bindVertexArray(0);
  gl.enable(GL_DEPTH_TEST);
}

void WorldRender::upload()
{
  ZoneScoped;
  _world->mapIndex.setAdt(false);

  if (_world->mapIndex.hasAGlobalWMO())
  {
    WMOInstance inst(_world->mWmoFilename, &_world->mWmoEntry, _world->_context);

    _world->_model_instance_storage.add_wmo_instance(std::move(inst), false);
  }
  else
  {
    _horizon_render = std::make_unique<Noggit::map_horizon::render>(_world->horizon);
  }

  _skies = std::make_unique<Skies>(_world->mapIndex._map_id, _world->_context);

  _outdoor_lighting = std::make_unique<OutdoorLighting>();

  _m2_program.reset
    ( new OpenGL::program
          { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("m2_vs") }
              , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("m2_fs") }
          }
    );

  _m2_instanced_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("m2_vs", {"instanced"}) }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("m2_fs") }
            }
      );

  _m2_box_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("m2_box_vs") }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("m2_box_fs") }
            }
      );

  _m2_ribbons_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("ribbon_vs") }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("ribbon_fs") }
            }
      );

  _blob_shadow_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("blob_shadow_vs") }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("blob_shadow_fs") }
            }
      );

  _m2_particles_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("particle_vs") }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("particle_fs") }
            }
      );

  _mcnk_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("terrain_vs") }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("terrain_fs") }
            }
      );

  _mfbo_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("mfbo_vs") }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("mfbo_fs") }
            }
      );

  _wmo_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("wmo_vs") }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("wmo_fs") }
            }
      );

  _liquid_program.reset(
      new OpenGL::program
          { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("liquid_vs") }
              , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("liquid_fs") }
          }
  );

  _wmo_liquid_program.reset(
      new OpenGL::program
          { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("wmo_liquid_vs") }
              , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("wmo_liquid_fs") }
          }
  );

  _occluder_program.reset(
      new OpenGL::program
          { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("occluder_vs") }
              , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("occluder_fs") }
          }
  );

  _liquid_texture_manager.upload();

  _buffers.upload();
  _vertex_arrays.upload();

  setupOccluderBuffers();

  {
    OpenGL::Scoped::use_program m2_shader {*_m2_program.get()};
    m2_shader.uniform("bone_matrices", 0);
    m2_shader.uniform("tex1", 1);
    m2_shader.uniform("tex2", 2);

    m2_shader.bind_uniform_block("matrices", 0);
    gl.bindBuffer(GL_UNIFORM_BUFFER, _mvp_ubo);
    gl.bufferData(GL_UNIFORM_BUFFER, sizeof(OpenGL::MVPUniformBlock), NULL, GL_DYNAMIC_DRAW);
    gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::MVP, _mvp_ubo, 0, sizeof(OpenGL::MVPUniformBlock));
    gl.bindBuffer(GL_UNIFORM_BUFFER, 0);

    m2_shader.bind_uniform_block("lighting", 1);
    gl.bindBuffer(GL_UNIFORM_BUFFER, _lighting_ubo);
    gl.bufferData(GL_UNIFORM_BUFFER, sizeof(OpenGL::LightingUniformBlock), NULL, GL_DYNAMIC_DRAW);
    gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::LIGHTING, _lighting_ubo, 0, sizeof(OpenGL::LightingUniformBlock));
    gl.bindBuffer(GL_UNIFORM_BUFFER, 0);
  }

  {
    std::vector<int> samplers {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    OpenGL::Scoped::use_program wmo_program {*_wmo_program.get()};
    wmo_program.uniform("render_batches_tex", 0);
    wmo_program.uniform("texture_samplers", samplers);
    wmo_program.bind_uniform_block("matrices", 0);
    wmo_program.bind_uniform_block("lighting", 1);
  }

  {
    OpenGL::Scoped::use_program mcnk_shader {*_mcnk_program.get()};

    setupChunkBuffers();
    setupChunkVAO(mcnk_shader);

    mcnk_shader.bind_uniform_block("matrices", 0);
    mcnk_shader.bind_uniform_block("lighting", 1);
    mcnk_shader.bind_uniform_block("overlay_params", 2);
    mcnk_shader.bind_uniform_block("chunk_instances", 3);

    gl.bindBuffer(GL_UNIFORM_BUFFER, _terrain_params_ubo);
    gl.bufferData(GL_UNIFORM_BUFFER, sizeof(OpenGL::TerrainParamsUniformBlock), NULL, GL_STATIC_DRAW);
    gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::TERRAIN_OVERLAYS, _terrain_params_ubo, 0, sizeof(OpenGL::TerrainParamsUniformBlock));
    gl.bindBuffer(GL_UNIFORM_BUFFER, 0);

    mcnk_shader.uniform("heightmap", 0);
    mcnk_shader.uniform("mccv", 1);
    mcnk_shader.uniform("shadowmap", 2);
    mcnk_shader.uniform("alphamap", 3);
    mcnk_shader.uniform("stamp_brush", 4);
    mcnk_shader.uniform("base_instance", 0);

    std::vector<int> samplers {5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    mcnk_shader.uniform("textures", samplers);

  }

  {
    OpenGL::Scoped::use_program m2_shader_instanced {*_m2_instanced_program.get()};
    m2_shader_instanced.bind_uniform_block("matrices", 0);
    m2_shader_instanced.bind_uniform_block("lighting", 1);
    m2_shader_instanced.uniform("bone_matrices", 0);
    m2_shader_instanced.uniform("tex1", 1);
    m2_shader_instanced.uniform("tex2", 2);
  }

  {
    // Particles read the lighting UBO for fog (so smoke/dust fades into the haze like other geometry).
    OpenGL::Scoped::use_program particles_shader {*_m2_particles_program.get()};
    particles_shader.bind_uniform_block("lighting", 1);
  }

  /*
  {
    OpenGL::Scoped::use_program particles_shader {*_m2_particles_program.get()};
    particles_shader.uniform("tex", 0);
  }

  {
    OpenGL::Scoped::use_program ribbon_shader {*_m2_ribbons_program.get()};
    ribbon_shader.uniform("tex", 0);
  }

   */

  {
    OpenGL::Scoped::use_program liquid_render {*_liquid_program.get()};

    setupLiquidChunkBuffers();
    setupLiquidChunkVAO(liquid_render);

    static std::vector<int> samplers {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    liquid_render.bind_uniform_block("matrices", 0);
    liquid_render.bind_uniform_block("lighting", 1);
    liquid_render.bind_uniform_block("liquid_layers_params", 4);
    liquid_render.uniform("vertex_data", 0);
    liquid_render.uniform("shadowmap", 1);
    liquid_render.uniform("texture_samplers", samplers);

  }

  {
    OpenGL::Scoped::use_program wmo_liquid_render {*_wmo_liquid_program.get()};
    static std::vector<int> samplers {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    wmo_liquid_render.bind_uniform_block("matrices", 0);
    wmo_liquid_render.bind_uniform_block("lighting", 1);
    wmo_liquid_render.uniform("texture_samplers", samplers);
  }

  {
    OpenGL::Scoped::use_program mfbo_shader {*_mfbo_program.get()};
    mfbo_shader.bind_uniform_block("matrices", 0);
  }

  {
    OpenGL::Scoped::use_program m2_box_shader {*_m2_box_program.get()};
    m2_box_shader.bind_uniform_block("matrices", 0);
  }

  {
    OpenGL::Scoped::use_program occluder_shader {*_occluder_program.get()};
    occluder_shader.bind_uniform_block("matrices", 0);
  }


}

void WorldRender::unload()
{
  ZoneScoped;
  _mcnk_program.reset();
  _mfbo_program.reset();
  _m2_program.reset();
  _m2_instanced_program.reset();
  _m2_particles_program.reset();
  _m2_ribbons_program.reset();
  _m2_box_program.reset();
  _wmo_program.reset();
  _liquid_program.reset();
  _wmo_liquid_program.reset();

  _cursor_render.unload();
  _sphere_render.unload();
  _square_render.unload();
  _line_render.unload();
  _circle_render.unload();
  _horizon_render.reset();

  _liquid_texture_manager.unload();

  _skies->unload();

  _buffers.unload();
  _vertex_arrays.unload();

  Noggit::Rendering::Primitives::WireBox::getInstance(_world->_context).unload();
}


void WorldRender::updateMVPUniformBlock(const glm::mat4x4& model_view, const glm::mat4x4& projection)
{
  ZoneScoped;

  _mvp_ubo_data.model_view = model_view;
  _mvp_ubo_data.projection = projection;

  gl.bindBuffer(GL_UNIFORM_BUFFER, _mvp_ubo);
  gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(OpenGL::MVPUniformBlock), &_mvp_ubo_data);

}

void WorldRender::updateLightingUniformBlock(bool draw_fog, glm::vec3 const& camera_pos)
{
  ZoneScoped;

  int daytime = static_cast<int>(_world->time) % 2880;

  int area_light_id = 0;
  unsigned int wmo_area_id = static_cast<unsigned int>(-1);
  unsigned int terrain_area_id = static_cast<unsigned int>(-1);
  unsigned int area_id = static_cast<unsigned int>(-1);
  try
  {
    wmo_area_id = _world->getWMOAreaID(camera_pos);
    terrain_area_id = _world->getAreaID(camera_pos);
    area_id = wmo_area_id;
    if (area_id == static_cast<unsigned int>(-1))
    {
      area_id = terrain_area_id;
    }

    if (area_id != static_cast<unsigned int>(-1)
        && gAreaDB.getFieldCount() > AreaDB::LightId
        && gAreaDB.CheckIfIdExists(area_id))
    {
      area_light_id = gAreaDB.getByID(area_id).getInt(AreaDB::LightId);
    }
  }
  catch (...)
  {
    area_light_id = 0;
  }

  _skies->setAreaLightId(area_light_id);
  // Underwater: switch to the underwater LightParams set (CLEAR_WATER) so the fog colour/density and
  // ambient match the in-game submerged look (cool, dense fog); above water use CLEAR. Both colorFor
  // and floatParamFor read the active param, so this swaps tint and fog together. If a light doesn't
  // define the underwater param, active_sky_param falls back to defaults -- harmless.
  // GUARDED: camera_is_underwater walks the chunk + its liquid layers, which (like getAreaID /
  // getWMOAreaID above) can throw while a tile is mid-load/unload. The sibling calls are wrapped in a
  // try/catch for exactly this reason; this one was not -> an uncaught exception here crashed the
  // editor intermittently (any map, any time the camera sits over loading liquid). Default to "not
  // underwater" on failure.
  bool underwater = false;
  try
  {
    underwater = _world->camera_is_underwater(camera_pos);
  }
  catch (...)
  {
    underwater = false;
  }
  _skies->setCurrentParam(underwater ? CLEAR_WATER : CLEAR);
  _skies->update_sky_colors(camera_pos, daytime);
  _outdoor_light_stats = _outdoor_lighting->getLightStats(static_cast<int>(_world->time));

  glm::vec3 diffuse = _skies->color_set[LIGHT_GLOBAL_DIFFUSE];
  glm::vec3 ambient = _skies->color_set[LIGHT_GLOBAL_AMBIENT];
  glm::vec3 fog_color = _skies->color_set[FOG_COLOR];
  glm::vec3 ocean_color_light = _skies->color_set[OCEAN_COLOR_LIGHT];
  glm::vec3 ocean_color_dark = _skies->color_set[OCEAN_COLOR_DARK];
  glm::vec3 river_color_light = _skies->color_set[RIVER_COLOR_LIGHT];
  glm::vec3 river_color_dark = _skies->color_set[RIVER_COLOR_DARK];

  float fog_start = _skies->fog_distance_start();
  float fog_end = _skies->fog_distance_end();
  if (fog_end <= 1.0f)
  {
    fog_start = 0.25f;
    fog_end = 500.0f;
  }

  // Inside a WMO with authored interior fog (MFOG)? The client applies that fog to the WHOLE scene
  // inside, not just the WMO geometry. Override the global fog so terrain, doodads, the WMO and light
  // shafts all fade together (otherwise only the WMO fogs and doodads/shafts float clear in front).
  if (draw_fog)
  {
    try
    {
      glm::vec3 wmo_fog_color;
      float wmo_fog_start = 0.f, wmo_fog_end = 0.f;
      if (_world->getInteriorFog(camera_pos, wmo_fog_color, wmo_fog_start, wmo_fog_end))
      {
        // Warm the (cool) authored interior fog so it doesn't chill the atmosphere. Keep the model's
        // authored distance/density. Tune: the warm channel multiplier.
        fog_color = glm::clamp(wmo_fog_color * glm::vec3(1.25f, 1.02f, 0.78f), 0.0f, 1.0f);
        fog_end = wmo_fog_end;
        // The global fog formula treats fog_start as a FRACTION of fog_end (start = fog_end * fog_start),
        // but MFOG fogstart is an absolute distance -- convert to the fraction so the math matches and
        // near geometry isn't fully fogged (which painted the whole interior flat fog colour).
        fog_start = (wmo_fog_end > 0.001f) ? std::clamp(wmo_fog_start / wmo_fog_end, 0.0f, 0.99f) : 0.25f;
      }
    }
    catch (...)
    {
      // WMO extents/fog access can throw mid-load (same reason getWMOAreaID is guarded); ignore and
      // keep the outdoor fog this frame.
    }
  }

  _lighting_ubo_data.DiffuseColor_FogStart = {diffuse.x,diffuse.y,diffuse.z, fog_start};
  _lighting_ubo_data.AmbientColor_FogEnd = {ambient.x,ambient.y,ambient.z, fog_end};
  _lighting_ubo_data.FogColor_FogOn = {fog_color.x,fog_color.y,fog_color.z, static_cast<float>(draw_fog)};
  _lighting_ubo_data.LightDir_FogRate = {_outdoor_light_stats.dayDir.x, _outdoor_light_stats.dayDir.y, _outdoor_light_stats.dayDir.z, _skies->fogRate()};
  _lighting_ubo_data.OceanColorLight = { ocean_color_light.x,ocean_color_light.y,ocean_color_light.z, _skies->ocean_shallow_alpha()};
  _lighting_ubo_data.OceanColorDark = { ocean_color_dark.x,ocean_color_dark.y,ocean_color_dark.z, _skies->ocean_deep_alpha()};
  _lighting_ubo_data.RiverColorLight = { river_color_light.x,river_color_light.y,river_color_light.z, _skies->river_shallow_alpha()};
  _lighting_ubo_data.RiverColorDark = { river_color_dark.x,river_color_dark.y,river_color_dark.z, _skies->river_deep_alpha()};

  if (capture_lighting_trace_enabled())
  {
    std::ofstream trace("I:\\Twow-local\\server_dev\\noggit_captures\\lighting_trace.txt", std::ios::app);
    trace << "pos=(" << camera_pos.x << "," << camera_pos.y << "," << camera_pos.z << ")"
          << " time=" << daytime
          << " draw_fog=" << (draw_fog ? 1 : 0)
          << " wmo_area=" << wmo_area_id
          << " terrain_area=" << terrain_area_id
          << " final_area=" << area_id
          << " area_light=" << area_light_id
          << " diffuse=(" << diffuse.x << "," << diffuse.y << "," << diffuse.z << ")"
          << " ambient=(" << ambient.x << "," << ambient.y << "," << ambient.z << ")"
          << " fog=(" << fog_color.x << "," << fog_color.y << "," << fog_color.z << ")"
          << " fog_start=" << fog_start
          << " fog_end=" << fog_end
          << " fog_rate=" << _skies->fogRate()
          << '\n';
  }

  // Collect emitter point lights (campfires etc.) near the camera so the terrain / WMO / M2 shaders
  // can add their warm falloff. M2 lights are sparse in 1.12 (mostly campfires); we keep the nearest
  // MAX_POINT_LIGHTS to the camera. (M2 attenuation range isn't parsed, so we use a default radius.)
  {
    struct CollectedLight { glm::vec3 pos; glm::vec3 color; float radius; float dist2; };
    std::vector<CollectedLight> collected;

    _world->_model_instance_storage.for_each_m2_instance([&] (ModelInstance& inst)
    {
      Model* m = inst.model.get();
      if (!m || !m->finishedLoading())
      {
        return;
      }

      // CANON LIGHTING ONLY: do NOT synthesize warm point lights for "hot" props (lightray shafts,
      // braziers, fires, candles, lava, etc.). Previous code fabricated a warm light from an
      // emissive-material flag + a filename-keyword guess, which flooded prop-dense rooms (Zul'Farrak
      // troll camps) with up to 16 fake orange lights = the orange wash. A prop with no AUTHORED M2 light
      // does not illuminate its surroundings in the real client; only its own emissive material/particles
      // are visible. We keep ONLY authored lights below (standalone M2 type-1, WMO MOLT, WMO-doodad M2).
      if (m->lights().empty())
      {
        return;
      }
      glm::mat4x4 const transform = inst.transformMatrix();
      float const inst_scale = glm::length(glm::vec3(transform[0])); // world scale from the placement matrix
      for (auto& l : m->lights())
      {
        if (l.type != 1) // 0 = directional (the model's key light), 1 = point. Only points are local glows.
        {
          continue;
        }
        glm::vec3 const world = glm::vec3(transform * glm::vec4(l.pos, 1.0f));
        // Sample the light's animated colour/intensity at the running clock (not frozen time 0) so
        // campfire/torch/brazier lights flicker from their authored track instead of holding constant.
        // _world->animtime is the global continuous clock and advances even for effect models whose
        // per-instance animate() never runs (the same reason texanims are driven on a global clock).
        int const lt = static_cast<int>(_world->animtime);
        glm::vec3 const col = l.diffColor.getValue(0, lt, lt) * l.diffIntensity.getValue(0, lt, lt);
        // Real attenuation radius (attEnd) instead of a magic number. attEnd is model-local, so scale it
        // to world units; clamp to a sane range so degenerate/zero values still give a usable glow.
        float const att = l.attEnd.getValue(0, 0, 0) * inst_scale;
        float const radius = glm::clamp(att, 12.0f, 60.0f);
        glm::vec3 const d = world - camera_pos;
        collected.push_back({world, col, radius, glm::dot(d, d)});
      }
    });

    // Lightray shafts are usually WMO doodads (e.g. timbermaw_instance.wmo's dusty light set), which
    // the standalone-M2 loop above never sees -- collect their synthetic light here too.
    int lightray_lights = 0;
    int wmo_molt_lights = 0;
    _world->_model_instance_storage.for_each_wmo_instance([&] (WMOInstance& wmo)
    {
      // Authored WMO lights (MOLT) -- e.g. Ironforge's 209 forge/torch lights, dungeon braziers. These
      // are parsed but were never fed to a renderer; wire them into the same point-light set the
      // terrain/WMO/M2 shaders already consume, so interiors are lit by their real authored lights.
      if (wmo.wmo.get() && wmo.wmo->finishedLoading() && !wmo.wmo->lights.empty())
      {
        glm::mat4x4 const wmo_transform = wmo.transformMatrix();
        for (auto const& wl : wmo.wmo->lights)
        {
          glm::vec3 const world = glm::vec3(wmo_transform * glm::vec4(wl.pos, 1.0f));
          glm::vec3 const col = glm::vec3(wl.fcolor) * std::max(wl.intensity, 0.0f);
          float const radius = glm::clamp(wl.r, 6.0f, 60.0f); // MOLT attenuation-end radius
          glm::vec3 const d = world - camera_pos;
          collected.push_back({world, col, radius, glm::dot(d, d)});
          ++wmo_molt_lights;
        }
      }

      auto* doodads = wmo.get_doodads(false);
      if (!doodads)
      {
        return;
      }
      for (auto& pair : *doodads)
      {
        for (auto& doodad : pair.second)
        {
          Model* dm = doodad.model.get();
          if (!dm || !dm->finishedLoading())
          {
            continue;
          }

          // (No synthesized lightray/hot light here either -- canon authored lights only.)

          // Real authored M2 lights on the WMO doodad (candles, lamps, braziers placed inside a WMO).
          // These were previously dropped -- only the synthesize-when-empty path below ran -- so interior
          // props with their own light contributed nothing. Collect them like the standalone-M2 loop,
          // with the animated (flickering) colour sampled on the global clock.
          if (!dm->lights().empty())
          {
            glm::mat4x4 const transform = doodad.transformMatrix();
            float const inst_scale = glm::length(glm::vec3(transform[0]));
            int const lt = static_cast<int>(_world->animtime);
            for (auto& l : dm->lights())
            {
              if (l.type != 1) // point lights only
              {
                continue;
              }
              glm::vec3 const world = glm::vec3(transform * glm::vec4(l.pos, 1.0f));
              glm::vec3 const col = l.diffColor.getValue(0, lt, lt) * l.diffIntensity.getValue(0, lt, lt);
              float const att = l.attEnd.getValue(0, 0, 0) * inst_scale;
              float const radius = glm::clamp(att, 12.0f, 60.0f);
              glm::vec3 const d = world - camera_pos;
              collected.push_back({world, col, radius, glm::dot(d, d)});
            }
          }

          // (No synthesized "hot prop" light for WMO doodads -- canon authored lights only.)
        }
      }
    });

    // (No synthesized lava-liquid point lights -- canon authored lights only. Lava still self-illuminates
    // as an emissive SURFACE via the liquid/WMO-unlit shader path; terrain near lava is warmed by its
    // baked MCCV vertex colours, exactly as in the client. We do not fabricate a dynamic light from it.)
    int lava_lights = 0;

    {
      static bool s_lr_dbg = std::getenv("NOGGIT_LIGHT_DEBUG") != nullptr;
      static int s_lr_log = 0;
      if (s_lr_dbg && s_lr_log < 4)
      {
        ++s_lr_log;
        LogError << "LIGHTRAYLIGHTS wmoDoodadBeams=" << lightray_lights
                 << " wmoMoltLights=" << wmo_molt_lights
                 << " lavaLights=" << lava_lights
                 << " totalCollected=" << collected.size() << std::endl;
      }
    }

    // Drop any non-finite lights BEFORE sorting: a NaN dist2 (from a bad WMO/M2 light position, radius
    // or attenuation value) makes the comparator violate strict-weak-ordering, which is undefined
    // behaviour in std::sort and can crash. Also guard against absurd radii.
    collected.erase(std::remove_if(collected.begin(), collected.end(),
                    [] (CollectedLight const& l)
                    {
                      return !std::isfinite(l.dist2) || !std::isfinite(l.radius) || l.radius <= 0.0f
                          || !std::isfinite(l.pos.x) || !std::isfinite(l.pos.y) || !std::isfinite(l.pos.z)
                          || !std::isfinite(l.color.r) || !std::isfinite(l.color.g) || !std::isfinite(l.color.b);
                    }),
                    collected.end());

    std::sort(collected.begin(), collected.end(),
              [] (CollectedLight const& a, CollectedLight const& b) { return a.dist2 < b.dist2; });

    int const count = std::min(static_cast<int>(collected.size()), OpenGL::MAX_POINT_LIGHTS);
    _lighting_ubo_data.PointLightParams = glm::vec4(static_cast<float>(count), 0.f, 0.f, 0.f);
    for (int i = 0; i < OpenGL::MAX_POINT_LIGHTS; ++i)
    {
      if (i < count)
      {
        _lighting_ubo_data.PointLightPos[i] = glm::vec4(collected[i].pos, collected[i].radius);
        _lighting_ubo_data.PointLightColor[i] = glm::vec4(collected[i].color, 0.f);
      }
      else
      {
        _lighting_ubo_data.PointLightPos[i] = glm::vec4(0.f);
        _lighting_ubo_data.PointLightColor[i] = glm::vec4(0.f);
      }
    }
  }

  gl.bindBuffer(GL_UNIFORM_BUFFER, _lighting_ubo);
  gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(OpenGL::LightingUniformBlock), &_lighting_ubo_data);
}

void WorldRender::updateLightingUniformBlockMinimap(MinimapRenderSettings* settings)
{
  ZoneScoped;

  glm::vec3 diffuse = settings->diffuse_color;
  glm::vec3 ambient = settings->ambient_color;

  _lighting_ubo_data.DiffuseColor_FogStart = { diffuse, 0 };
  _lighting_ubo_data.AmbientColor_FogEnd = { ambient, 0 };
  _lighting_ubo_data.FogColor_FogOn = { 0, 0, 0, 0 };
  _lighting_ubo_data.LightDir_FogRate = { _outdoor_light_stats.dayDir.x, _outdoor_light_stats.dayDir.y, _outdoor_light_stats.dayDir.z, _skies->fogRate() };
  _lighting_ubo_data.OceanColorLight = settings->ocean_color_light;
  _lighting_ubo_data.OceanColorDark = settings->ocean_color_dark;
  _lighting_ubo_data.RiverColorLight = settings->river_color_light;
  _lighting_ubo_data.RiverColorDark = settings->river_color_dark;
  _lighting_ubo_data.PointLightParams = glm::vec4(0.f); // no emitter point lights on the minimap

  gl.bindBuffer(GL_UNIFORM_BUFFER, _lighting_ubo);
  gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(OpenGL::LightingUniformBlock), &_lighting_ubo_data);
}

void WorldRender::updateTerrainParamsUniformBlock()
{
  ZoneScoped;
  gl.bindBuffer(GL_UNIFORM_BUFFER, _terrain_params_ubo);
  gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(OpenGL::TerrainParamsUniformBlock), &_terrain_params_ubo_data);
  _need_terrain_params_ubo_update = false;
}

void WorldRender::setupChunkVAO(OpenGL::Scoped::use_program& mcnk_shader)
{
  ZoneScoped;
  OpenGL::Scoped::vao_binder const _ (_mapchunk_vao);

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder(_mapchunk_texcoord);
    mcnk_shader.attrib("texcoord", 2, GL_FLOAT, GL_FALSE, 0, 0);
  }

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder(_mapchunk_vertex);
    mcnk_shader.attrib("position", 2, GL_FLOAT, GL_FALSE, 0, 0);
  }
}

void WorldRender::setupChunkBuffers()
{
  ZoneScoped;

  // vertices

  glm::vec2 vertices[mapbufsize];
  glm::vec2 *ttv = vertices;

  for (int j = 0; j < 17; ++j)
  {
    bool is_lod = j % 2;
    for (int i = 0; i < (is_lod ? 8 : 9); ++i)
    {
      float xpos, zpos;
      xpos = i * UNITSIZE;
      zpos = j * 0.5f * UNITSIZE;

      if (is_lod)
      {
        xpos += UNITSIZE*0.5f;
      }

      auto v = glm::vec2(xpos, zpos);
      *ttv++ = v;
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER>(_mapchunk_vertex, sizeof(vertices), vertices, GL_STATIC_DRAW);


  static constexpr std::array<std::uint16_t, 768 + 192> indices {

      9, 0, 17, 9, 17, 18, 9, 18, 1, 9, 1, 0, 26, 17, 34, 26,
      34, 35, 26, 35, 18, 26, 18, 17, 43, 34, 51, 43, 51, 52, 43, 52,
      35, 43, 35, 34, 60, 51, 68, 60, 68, 69, 60, 69, 52, 60, 52, 51,
      77, 68, 85, 77, 85, 86, 77, 86, 69, 77, 69, 68, 94, 85, 102, 94,
      102, 103, 94, 103, 86, 94, 86, 85, 111, 102, 119, 111, 119, 120, 111, 120,
      103, 111, 103, 102, 128, 119, 136, 128, 136, 137, 128, 137, 120, 128, 120, 119,
      10, 1, 18, 10, 18, 19, 10, 19, 2, 10, 2, 1, 27, 18, 35, 27,
      35, 36, 27, 36, 19, 27, 19, 18, 44, 35, 52, 44, 52, 53, 44, 53,
      36, 44, 36, 35, 61, 52, 69, 61, 69, 70, 61, 70, 53, 61, 53, 52,
      78, 69, 86, 78, 86, 87, 78, 87, 70, 78, 70, 69, 95, 86, 103, 95,
      103, 104, 95, 104, 87, 95, 87, 86, 112, 103, 120, 112, 120, 121, 112, 121,
      104, 112, 104, 103, 129, 120, 137, 129, 137, 138, 129, 138, 121, 129, 121, 120,
      11, 2, 19, 11, 19, 20, 11, 20, 3, 11, 3, 2, 28, 19, 36, 28,
      36, 37, 28, 37, 20, 28, 20, 19, 45, 36, 53, 45, 53, 54, 45, 54,
      37, 45, 37, 36, 62, 53, 70, 62, 70, 71, 62, 71, 54, 62, 54, 53,
      79, 70, 87, 79, 87, 88, 79, 88, 71, 79, 71, 70, 96, 87, 104, 96,
      104, 105, 96, 105, 88, 96, 88, 87, 113, 104, 121, 113, 121, 122, 113, 122,
      105, 113, 105, 104, 130, 121, 138, 130, 138, 139, 130, 139, 122, 130, 122, 121,
      12, 3, 20, 12, 20, 21, 12, 21, 4, 12, 4, 3, 29, 20, 37, 29,
      37, 38, 29, 38, 21, 29, 21, 20, 46, 37, 54, 46, 54, 55, 46, 55,
      38, 46, 38, 37, 63, 54, 71, 63, 71, 72, 63, 72, 55, 63, 55, 54,
      80, 71, 88, 80, 88, 89, 80, 89, 72, 80, 72, 71, 97, 88, 105, 97,
      105, 106, 97, 106, 89, 97, 89, 88, 114, 105, 122, 114, 122, 123, 114, 123,
      106, 114, 106, 105, 131, 122, 139, 131, 139, 140, 131, 140, 123, 131, 123, 122,
      13, 4, 21, 13, 21, 22, 13, 22, 5, 13, 5, 4, 30, 21, 38, 30,
      38, 39, 30, 39, 22, 30, 22, 21, 47, 38, 55, 47, 55, 56, 47, 56,
      39, 47, 39, 38, 64, 55, 72, 64, 72, 73, 64, 73, 56, 64, 56, 55,
      81, 72, 89, 81, 89, 90, 81, 90, 73, 81, 73, 72, 98, 89, 106, 98,
      106, 107, 98, 107, 90, 98, 90, 89, 115, 106, 123, 115, 123, 124, 115, 124,
      107, 115, 107, 106, 132, 123, 140, 132, 140, 141, 132, 141, 124, 132, 124, 123,
      14, 5, 22, 14, 22, 23, 14, 23, 6, 14, 6, 5, 31, 22, 39, 31,
      39, 40, 31, 40, 23, 31, 23, 22, 48, 39, 56, 48, 56, 57, 48, 57,
      40, 48, 40, 39, 65, 56, 73, 65, 73, 74, 65, 74, 57, 65, 57, 56,
      82, 73, 90, 82, 90, 91, 82, 91, 74, 82, 74, 73, 99, 90, 107, 99,
      107, 108, 99, 108, 91, 99, 91, 90, 116, 107, 124, 116, 124, 125, 116, 125,
      108, 116, 108, 107, 133, 124, 141, 133, 141, 142, 133, 142, 125, 133, 125, 124,
      15, 6, 23, 15, 23, 24, 15, 24, 7, 15, 7, 6, 32, 23, 40, 32,
      40, 41, 32, 41, 24, 32, 24, 23, 49, 40, 57, 49, 57, 58, 49, 58,
      41, 49, 41, 40, 66, 57, 74, 66, 74, 75, 66, 75, 58, 66, 58, 57,
      83, 74, 91, 83, 91, 92, 83, 92, 75, 83, 75, 74, 100, 91, 108, 100,
      108, 109, 100, 109, 92, 100, 92, 91, 117, 108, 125, 117, 125, 126, 117, 126,
      109, 117, 109, 108, 134, 125, 142, 134, 142, 143, 134, 143, 126, 134, 126, 125,
      16, 7, 24, 16, 24, 25, 16, 25, 8, 16, 8, 7, 33, 24, 41, 33,
      41, 42, 33, 42, 25, 33, 25, 24, 50, 41, 58, 50, 58, 59, 50, 59,
      42, 50, 42, 41, 67, 58, 75, 67, 75, 76, 67, 76, 59, 67, 59, 58,
      84, 75, 92, 84, 92, 93, 84, 93, 76, 84, 76, 75, 101, 92, 109, 101,
      109, 110, 101, 110, 93, 101, 93, 92, 118, 109, 126, 118, 126, 127, 118, 127,
      110, 118, 110, 109, 135, 126, 143, 135, 143, 144, 135, 144, 127, 135, 127, 126,

      // lod
      0, 34, 18, 18, 34, 36, 18, 36, 2, 18, 2, 0, 34, 68, 52, 52,
      68, 70, 52, 70, 36, 52, 36, 34, 68, 102, 86, 86, 102, 104, 86, 104,
      70, 86, 70, 68, 102, 136, 120, 120, 136, 138, 120, 138, 104, 120, 104, 102,
      2, 36, 20, 20, 36, 38, 20, 38, 4, 20, 4, 2, 36, 70, 54, 54,
      70, 72, 54, 72, 38, 54, 38, 36, 70, 104, 88, 88, 104, 106, 88, 106,
      72, 88, 72, 70, 104, 138, 122, 122, 138, 140, 122, 140, 106, 122, 106, 104,
      4, 38, 22, 22, 38, 40, 22, 40, 6, 22, 6, 4, 38, 72, 56, 56,
      72, 74, 56, 74, 40, 56, 40, 38, 72, 106, 90, 90, 106, 108, 90, 108,
      74, 90, 74, 72, 106, 140, 124, 124, 140, 142, 124, 142, 108, 124, 108, 106,
      6, 40, 24, 24, 40, 42, 24, 42, 8, 24, 8, 6, 40, 74, 58, 58,
      74, 76, 58, 76, 42, 58, 42, 40, 74, 108, 92, 92, 108, 110, 92, 110,
      76, 92, 76, 74, 108, 142, 126, 126, 142, 144, 126, 144, 110, 126, 110, 108};

  /*
  // indices
  std::uint16_t indices[768];
  int flat_index = 0;

  for (int x = 0; x<8; ++x)
  {
    for (int y = 0; y<8; ++y)
    {
      indices[flat_index++] = MapChunk::indexLoD(y, x); //9
      indices[flat_index++] = MapChunk::indexNoLoD(y, x); //0
      indices[flat_index++] = MapChunk::indexNoLoD(y + 1, x); //17
      indices[flat_index++] = MapChunk::indexLoD(y, x); //9
      indices[flat_index++] = MapChunk::indexNoLoD(y + 1, x); //17
      indices[flat_index++] = MapChunk::indexNoLoD(y + 1, x + 1); //18
      indices[flat_index++] = MapChunk::indexLoD(y, x); //9
      indices[flat_index++] = MapChunk::indexNoLoD(y + 1, x + 1); //18
      indices[flat_index++] = MapChunk::indexNoLoD(y, x + 1); //1
      indices[flat_index++] = MapChunk::indexLoD(y, x); //9
      indices[flat_index++] = MapChunk::indexNoLoD(y, x + 1); //1
      indices[flat_index++] = MapChunk::indexNoLoD(y, x); //0
    }
  }

   */

  {
    OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> const _ (_mapchunk_index);
    gl.bufferData (GL_ELEMENT_ARRAY_BUFFER, (768 + 192) * sizeof(std::uint16_t), static_cast<void const*>(indices.data()), GL_STATIC_DRAW);
  }

  // tex coords
  glm::vec2 temp[mapbufsize], *vt;
  float tx, ty;

  // init texture coordinates for detail map:
  vt = temp;
  const float detail_half = 0.5f * detail_size / 8.0f;
  for (int j = 0; j < 17; ++j)
  {
    bool is_lod = j % 2;

    for (int i = 0; i< (is_lod ? 8 : 9); ++i)
    {
      tx = detail_size / 8.0f * i;
      ty = detail_size / 8.0f * j * 0.5f;

      if (is_lod)
        tx += detail_half;

      *vt++ = glm::vec2(tx, ty);
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER> (_mapchunk_texcoord, sizeof(temp), temp, GL_STATIC_DRAW);

}

void WorldRender::setupLiquidChunkVAO(OpenGL::Scoped::use_program& water_shader)
{
  ZoneScoped;
  OpenGL::Scoped::vao_binder const _ (_liquid_chunk_vao);

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder(_liquid_chunk_vertex);
    water_shader.attrib("position", 2, GL_FLOAT, GL_FALSE, 0, 0);
  }
}

void WorldRender::setupLiquidChunkBuffers()
{
  ZoneScoped;

  // vertices
  glm::vec2 vertices[768 / 2];
  glm::vec2* vt = vertices;

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      // first triangle
      *vt++ = glm::vec2(UNITSIZE * x, UNITSIZE * z);
      *vt++ = glm::vec2(UNITSIZE * x, UNITSIZE * (z + 1));
      *vt++ = glm::vec2(UNITSIZE * (x + 1), UNITSIZE * z);

      // second triangle
      *vt++ = glm::vec2(UNITSIZE * (x + 1), UNITSIZE * z);
      *vt++ = glm::vec2(UNITSIZE * x, UNITSIZE * (z + 1));
      *vt++ = glm::vec2(UNITSIZE * (x + 1), UNITSIZE * (z + 1));
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER> (_liquid_chunk_vertex, sizeof(vertices), vertices, GL_STATIC_DRAW);

}



void WorldRender::setupOccluderBuffers()
{
  ZoneScoped;
  static constexpr std::array<std::uint16_t, 36> indices
      {
          /*Above ABC,BCD*/
          0,1,2,
          1,2,3,
          /*Following EFG,FGH*/
          4,5,6,
          5,6,7,
          /*Left ABF,AEF*/
          1,0,5,
          0,4,5,
          /*Right side CDH,CGH*/
          3,2,7,
          2,6,7,
          /*ACG,AEG*/
          2,0,6,
          0,4,6,
          /*Behind BFH,BDH*/
          5,1,7,
          1,3,7
      };

  {
    OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> const _ (_occluder_index);
    gl.bufferData (GL_ELEMENT_ARRAY_BUFFER, 36 * sizeof(std::uint16_t), indices.data(), GL_STATIC_DRAW);
  }

}

void WorldRender::drawMinimap ( MapTile *tile
    , glm::mat4x4 const& model_view
    , glm::mat4x4 const& projection
    , glm::vec3 const& camera_pos
    , MinimapRenderSettings* settings
)
{
  ZoneScoped;

  // Also load a tile above the current one to correct the lookat approximation
  TileIndex m_tile = TileIndex(camera_pos);
  m_tile.z -= 1;

  bool unload = !_world->mapIndex.has_unsaved_changes(m_tile);

  MapTile* mTile = _world->mapIndex.loadTile(m_tile);

  if (mTile)
  {
    mTile->wait_until_loaded();

  }

  draw(model_view, projection, glm::vec3(), 0, glm::vec4(),
       CursorType::NONE, 0.f, false, false, false, 0.3f, 0.f, glm::vec3(), 0.f, 0.f, false, false, false, editing_mode::minimap, camera_pos, true, false, true, settings->draw_wmo, settings->draw_water, false, settings->draw_m2, false, false, true, settings, false, eTerrainType::eTerrainType_Linear, 0, display_mode::in_3D, false, true);


  if (unload)
  {
    _world->mapIndex.unloadTile(m_tile);
  }
}

bool WorldRender::saveMinimap(TileIndex const& tile_idx, MinimapRenderSettings* settings, std::optional<QImage>& combined_image)
{
  ZoneScoped;
  // Setup framebuffer
  QOpenGLFramebufferObjectFormat fmt;
  fmt.setSamples(0);
  fmt.setInternalTextureFormat(GL_RGBA8);
  fmt.setAttachment(QOpenGLFramebufferObject::Depth);

  QOpenGLFramebufferObject pixel_buffer(settings->resolution, settings->resolution, fmt);
  pixel_buffer.bind();

  gl.viewport(0, 0, settings->resolution, settings->resolution);
  gl.clearColor(.0f, .0f, .0f, 1.f);
  gl.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // Load tile
  bool unload = !_world->mapIndex.has_unsaved_changes(tile_idx);

  if (!_world->mapIndex.tileLoaded(tile_idx) && !_world->mapIndex.tileAwaitingLoading(tile_idx))
  {
    MapTile* tile = _world->mapIndex.loadTile(tile_idx);
    tile->wait_until_loaded();
    _world->wait_for_all_tile_updates();
    tile->waitForChildrenLoaded();
  }

  MapTile* mTile = _world->mapIndex.getTile(tile_idx);

  if (mTile)
  {
    unsigned counter = 0;
    constexpr unsigned TIMEOUT = 5000;

    while (AsyncLoader::instance().is_loading() || !mTile->finishedLoading())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      counter++;

      if (counter >= TIMEOUT)
        break;
    }

    float max_height = std::max(_world->getMaxTileHeight(tile_idx), 200.f);

    // setup view matrices
    auto projection = glm::ortho( -TILESIZE / 2.0f,TILESIZE / 2.0f,-TILESIZE / 2.0f,TILESIZE / 2.0f,0.f,100000.0f);

    auto eye = glm::vec3(TILESIZE * tile_idx.x + TILESIZE / 2.0f, max_height + 10.0f, TILESIZE * tile_idx.z + TILESIZE / 2.0f);
    auto center = glm::vec3(TILESIZE * tile_idx.x + TILESIZE / 2.0f, max_height + 5.0f, TILESIZE * tile_idx.z + TILESIZE / 2.0 - 0.005f);
    auto up = glm::vec3(0.f, 1.f, 0.f);

    glm::vec3 const z = glm::normalize(eye - center);
    glm::vec3 const x = glm::normalize(glm::cross(up, z));
    glm::vec3 const y = glm::normalize(glm::cross(z, x));

    auto look_at = glm::transpose(glm::mat4x4(x.x, x.y, x.z, glm::dot(x, glm::vec3(-eye.x, -eye.y, -eye.z))
        , y.x, y.y, y.z, glm::dot(y, glm::vec3(-eye.x, -eye.y, -eye.z))
        , z.x, z.y, z.z, glm::dot(z, glm::vec3(-eye.x, -eye.y, -eye.z))
        , 0.f, 0.f, 0.f, 1.f
    ));

    glFinish();

    drawMinimap(mTile
        , look_at
        , projection
        , glm::vec3(TILESIZE * tile_idx.x + TILESIZE / 2.0f
            , max_height + 15.0f, TILESIZE * tile_idx.z + TILESIZE / 2.0f)
        , settings);

    // Clearing alpha from image
    gl.colorMask(false, false, false, true);
    gl.clearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.clear(GL_COLOR_BUFFER_BIT);
    gl.colorMask(true, true, true, true);

    assert(pixel_buffer.isValid() && pixel_buffer.isBound());

    QImage image = pixel_buffer.toImage();

    image = image.convertToFormat(QImage::Format_RGBA8888);

    QSettings app_settings;
    QString str = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
    if (!(str.endsWith('\\') || str.endsWith('/')))
    {
      str += "/";
    }

    QDir dir(str + "/textures/minimap/");
    if (!dir.exists())
      dir.mkpath(".");

    std::string tex_name = std::string(_world->basename + "_" + std::to_string(tile_idx.x) + "_" + std::to_string(tile_idx.z) + ".blp");

    if (settings->file_format == ".png")
    {
      image.save(dir.filePath(std::string(_world->basename + "_" + std::to_string(tile_idx.x) + "_" + std::to_string(tile_idx.z) + ".png").c_str()));
    }
    else if (settings->file_format == ".blp")
    {
      QByteArray bytes;
      QBuffer buffer( &bytes );
      buffer.open( QIODevice::WriteOnly );

      image.save( &buffer, "PNG" );

      auto blp = Png2Blp();
      blp.load(reinterpret_cast<const void*>(bytes.constData()), bytes.size());

      uint32_t file_size;
      void* blp_image = blp.createBlpDxtInMemory(true, FORMAT_DXT5, file_size);

      QFile file(dir.filePath(tex_name.c_str()));
      file.open(QIODevice::WriteOnly);

      QDataStream out(&file);
      out.writeRawData(reinterpret_cast<char*>(blp_image), file_size);

      file.close();
    }

    // Write combined file
    if (settings->combined_minimap && combined_image.has_value())
    {
      QImage scaled_image = image.scaled(128, 128,  Qt::KeepAspectRatio);

      for (int i = 0; i < 128; ++i)
      {
        for (int j = 0; j < 128; ++j)
        {
          combined_image->setPixelColor(static_cast<int>(tile_idx.x) * 128 + j, static_cast<int>(tile_idx.z) * 128 + i, scaled_image.pixelColor(j, i));
        }
      }

    }

    // Register in md5translate.trs
    std::string map_name = gMapDB.getByID(_world->mapIndex._map_id).getString(MapDB::InternalName);

    auto sstream = std::stringstream();
    sstream << map_name << "\\map" << std::setfill('0') << std::setw(2) << tile_idx.x << "_" << std::setfill('0') << std::setw(2) << tile_idx.z << ".blp";
    std::string tilename_left = sstream.str();
    _world->mapIndex._minimap_md5translate[map_name][tilename_left] = tex_name;

    if (unload)
    {
      _world->mapIndex.unloadTile(tile_idx);
    }

  }

  pixel_buffer.release();

  return true;
}
