// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/rendering/vulkan/VkParticleFeed.hpp>
#include "WorldRender.hpp"
#include <external/tracy/Tracy.hpp>
#include <math/frustum.hpp>
#include <optional>
#include <math/ray.hpp>
#include <noggit/Log.h>
#include <noggit/render_thread_pool.hpp>
#include <noggit/World.h>
#include <noggit/TileWater.hpp>
#include <noggit/ChunkWater.hpp>
#include <noggit/liquid_layer.hpp>
#include <external/PNG2BLP/Png2Blp.h>
#include <noggit/DBC.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/frame_profiler.hpp>

#include <atomic>   // g_texture_upload_epoch (MDI batcher cache signature)
#include <cstdint>

#include <QDir>
#include <QBuffer>
#include <QtCore/QElapsedTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

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

  // Focused creature-cull diagnostic: NOGGIT_CREATURE_CULL_DEBUG=1 logs the cull decision for EVERY
  // creature at any distance (one line per guid per stage), without the full NOGGIT_CAPTURE_DEBUG flood.
  // Use it to see exactly which stage (skip-distance / skip-frustum / skip-load / draw-queued) drops a
  // given spawn and at what distance.
  bool creature_cull_debug_enabled()
  {
    static bool const enabled = []
    {
      char const* v = std::getenv("NOGGIT_CREATURE_CULL_DEBUG");
      return v != nullptr && std::string(v) != "0";
    }();
    return enabled;
  }

  // --- Coarse WDL-horizon occlusion (checklist D3/15.3) -----------------------------------------
  // Cull tiles whose ENTIRE AABB sits below the terrain silhouette formed by NEARER low-res (WDL)
  // terrain along the camera ray. Uses the WDL heightmap already loaded for the horizon render, so
  // it costs no extra I/O. Default OFF (opt-in QSetting) + deliberately CONSERVATIVE (safety margin,
  // only far tiles) because OVER-culling is exactly what got the old GPU-query occlusion disabled.

  // Coarse terrain height (world units) from the WDL 17x17-per-tile grid. Very low where no WDL tile
  // exists (== "no occluder here").
  float wdl_height_at(World* world, float wx, float wz)
  {
    if (wx < 0.0f || wz < 0.0f) return -1.0e9f;
    int const tx = static_cast<int>(wx / TILESIZE);
    int const tz = static_cast<int>(wz / TILESIZE);
    if (tx < 0 || tx > 63 || tz < 0 || tz > 63) return -1.0e9f;
    auto const* ht = world->horizon.get_horizon_tile(tz, tx);
    if (!ht) return -1.0e9f;
    float const cell = TILESIZE / 16.0f; // 17x17 grid spans the 533yd tile
    int const gi = std::clamp(static_cast<int>((wx - tx * TILESIZE) / cell + 0.5f), 0, 16);
    int const gj = std::clamp(static_cast<int>((wz - tz * TILESIZE) / cell + 0.5f), 0, 16);
    return static_cast<float>(ht->height_17[gj][gi]);
  }

  // True when the tile AABB [mn,mx] is fully hidden behind nearer WDL terrain from the camera.
  // [2026-08-06] MULTI-RAY (was single-ray to the nearest point, which over-culled: a tile mostly in view
  // but whose nearest edge alone was tucked behind a ridge got the WHOLE tile culled -> empty visible tiles).
  // Now sample an NxN grid across the tile's TOP face and march a ray to EACH; the tile is occluded ONLY when
  // EVERY sample is hidden. Early-out the instant one sample is visible, so a tile with any visible part is
  // never culled. Each sample uses the AABB top (mx.y = the tile's highest point = hardest to hide) so the
  // test stays conservative. Still whole-tile granularity (coarse WDL data can't drive per-chunk well).
  bool wdl_horizon_occluded(World* world, glm::vec3 const& cam, glm::vec3 const& mn, glm::vec3 const& mx)
  {
    constexpr float min_dist = 2.5f * TILESIZE;   // never occlude nearby tiles (raised from 2.0)
    constexpr float margin = 0.03f;               // ~1.7deg safety (raised from 0.02) -- bias to NOT cull
    constexpr int N = 3;                          // 3x3 = 9 sample points across the tile top
    float const step = TILESIZE / 16.0f;          // ~33yd (one WDL cell)

    for (int j = 0; j < N; ++j)
    {
      for (int i = 0; i < N; ++i)
      {
        float const px = mn.x + (mx.x - mn.x) * (static_cast<float>(i) / (N - 1));
        float const pz = mn.z + (mx.z - mn.z) * (static_cast<float>(j) / (N - 1));
        float const dx = px - cam.x;
        float const dz = pz - cam.z;
        float const d = std::sqrt(dx * dx + dz * dz);
        if (d < min_dist)
          return false;                           // any sample this close -> tile is near -> never cull

        float const occludee_angle = std::atan2(mx.y - cam.y, d); // this sample at the tile TOP
        float const inv = 1.0f / d;
        float const ux = dx * inv;
        float const uz = dz * inv;
        float max_terrain_angle = -3.15f;
        for (float t = step; t < d - step; t += step)
        {
          float const h = wdl_height_at(world, cam.x + ux * t, cam.z + uz * t);
          if (h <= -1.0e8f) continue;
          float const a = std::atan2(h - cam.y, t);
          if (a > max_terrain_angle) max_terrain_angle = a;
        }
        if (occludee_angle + margin >= max_terrain_angle)
          return false;                           // this sample is VISIBLE -> don't cull the tile
      }
    }
    return true;                                  // every sample hidden -> genuinely occluded, safe to cull
  }

  bool should_trace_creature_spawn(World::CreatureSpawnOverlay const& spawn, float distance)
  {
    if (creature_cull_debug_enabled())
    {
      return true;
    }

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

    // Live Settings slider (creature/draw_distance): the UNIFORM render distance for ALL creatures/
    // gameobjects regardless of model size -- a giant and a critter both draw out to this same distance.
    // Default 500 (the client's DistCull default, RE'd in RE_notes/14, used here as a flat cap). Raise
    // for further draw, lower for fps.
    float const v = QSettings().value("creature/draw_distance", 500.0f).toFloat();
    return (std::isfinite(v) && v > 0.0f) ? v : 500.0f;
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

  // True for M2 "light shaft" models that must draw AFTER the water so it doesn't paint over
  // them. Two shapes qualify (both with NO solid Opaque/Alpha_Key pass):
  //  (a) at least one additive pass (No_Add_Alpha / Add) — the classic god-ray glows;
  //  (b) 2026-08-26: every pass UNLIT and translucent — the Stormwind LIGHTHOUSEEFFECT beam is a
  //      single blend=2 Alpha pass with renderflags unlit+unfogged, which (a) never matched (the
  //      "beam renders under the harbor water" bug on every draw path).
  // Ordinary LIT alpha doodads (trees, waterfalls) stay excluded by the unlit test, and every
  // deferral call site additionally requires no particle/ribbon emitters (the historical
  // waterfall-particle crash), so this widening cannot reroute them.
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
    bool all_unlit = true;
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
      if (pass.renderflag_index >= model->renderFlagsTable().size()
          || !model->renderFlagsTable()[pass.renderflag_index].flags.unlit)
      {
        all_unlit = false;
      }
    }

    return has_additive || all_unlit;
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

  // Uses the caller's prebuilt per-frame index (WorldRender::_legacy_suppress_index) instead of walking
  // every creature spawn: this used to be O(creature instances x ~35k spawns) PER FRAME -- the dominant
  // creature-view cost. Now it's an O(1) map lookup by model path + a scan of only the spawns that share
  // that exact model (a handful). Behaviour is identical: the old loop skipped every non-matching-path
  // spawn anyway, so the index bucket is exactly the set it used to consider.
  bool should_suppress_legacy_creature_instance
    ( World const* world
    , ModelInstance const& model_instance
    , std::unordered_map<std::string, std::vector<Noggit::Rendering::LegacyOverlayInfo>> const& index
    )
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

    auto const bucket_it = index.find(model_path);
    if (bucket_it == index.end())
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

    for (auto const& overlay : bucket_it->second)
    {
      float const distance = glm::distance(model_instance.get_pos(), overlay.pos);
      if (distance < nearest_distance)
      {
        nearest_distance = distance;
        nearest_guid = overlay.guid;
        nearest_model = overlay.overlay_model;
      }
      float const overlap_radius = std::max(4.0f, legacy_radius + overlay.overlay_radius + 1.5f);
      if (distance <= overlap_radius)
      {
        std::ostringstream key;
        key << model_instance.model->file_key().stringRepr() << '|'
            << model_instance.uid << '|'
            << overlay.guid;
        if (logged_suppressions.insert(key.str()).second)
        {
          LogDebug << "Suppressing legacy creature instance model='"
                   << model_instance.model->file_key().stringRepr()
                   << "' uid=" << model_instance.uid
                   << " overlayGuid=" << overlay.guid
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

// Rebuild the per-frame legacy-suppression index from the world's creature spawns. Keyed by spawn
// model_path (exactly the field the old inner loop compared against), one bucket entry per usable spawn.
// clear() keeps the map's capacity so this is allocation-light across frames.
void Noggit::Rendering::WorldRender::rebuildLegacySuppressIndex()
{
  // THROTTLE (2026-07-25 perf): this map is derived from STATIC spawn placements; the only per-frame drift
  // is a lazily-loaded model refining its entry's overlay radius/path. Rebuilding the whole string-keyed
  // map every frame cost ~3ms in Stormwind (measured, ~10% of the frame) for no visible benefit. Rebuild
  // only when the spawn count changes (immediate -- covers spawns first loading in) or every 30 frames (to
  // fold in model-radius refinement as distant spawns come into range). The stale window is <=0.5s and the
  // suppression test is a coarse visual doodad-dedup, so a slightly-stale radius is imperceptible.
  std::size_t const spawn_count =
    _world->drawCreatureSpawns() ? _world->creatureSpawns().size() : 0;
  bool const dirty =
    (spawn_count != _legacy_suppress_last_count) || ((_legacy_suppress_tick++ % 30) == 0);
  if (!dirty)
  {
    return;
  }
  _legacy_suppress_last_count = spawn_count;

  _legacy_suppress_index.clear();

  if (!_world->drawCreatureSpawns())
  {
    return;
  }

  for (auto const& spawn : _world->creatureSpawns())
  {
    if (spawn.model_path.empty())
    {
      continue;
    }

    float overlay_radius = std::max(0.5f, spawn.template_scale * spawn.model_scale);
    std::string overlay_model = spawn.model_path;
    if (spawn.model_instance.has_value() && spawn.model_instance->model.get())
    {
      overlay_radius = spawn.model_instance->model->rad * spawn.model_instance->scale;
      if (spawn.model_instance->model->file_key().hasFilepath())
      {
        overlay_model = spawn.model_instance->model->file_key().filepath();
      }
    }
    if (overlay_radius <= 0.0f)
    {
      overlay_radius = 1.0f;
    }

    _legacy_suppress_index[spawn.model_path].push_back(
      Noggit::Rendering::LegacyOverlayInfo{spawn.pos, overlay_radius, spawn.guid, std::move(overlay_model)});
  }
}

WorldRender::WorldRender(World* world)
: BaseRender()
, _world(world)
, _liquid_texture_manager(world->_context)
, _view_distance(world->_settings->value("view_distance", 900.f).toFloat())
, _cull_distance(0.f)
, _terrain_cull_distance(0.f)
{
}

void WorldRender::ensureMdiArena()
{
  if (_mdi_ready)
    return;

  _mdi_buffers.upload();
  _mdi_vao_arr.upload();

  GLuint const arena_vbo     = _mdi_buffers[0];
  GLuint const arena_ibo     = _mdi_buffers[1];
  GLuint const inst_tf       = _mdi_buffers[2];
  GLuint const inst_interior = _mdi_buffers[3];
  GLuint const inst_tex      = _mdi_buffers[4];
  GLuint const vao           = _mdi_vao_arr[0];

  // Fixed, generous append-only geometry arena (never freed/compacted). 128MB vtx (~2.7M ModelVertex) +
  // 32MB idx (~16M uint16) holds all doodad geometry a session touches; overflow just draws unbatched.
  _mdi_arena_vbo_cap = static_cast<GLsizeiptr>(128) * 1024 * 1024;
  _mdi_arena_ibo_cap = static_cast<GLsizeiptr>(32) * 1024 * 1024;
  gl.bindBuffer(GL_ARRAY_BUFFER, arena_vbo);
  gl.bufferData(GL_ARRAY_BUFFER, _mdi_arena_vbo_cap, nullptr, GL_STATIC_DRAW);
  gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, arena_ibo);
  gl.bufferData(GL_ELEMENT_ARRAY_BUFFER, _mdi_arena_ibo_cap, nullptr, GL_STATIC_DRAW);
  _mdi_arena_vtx = 0;
  _mdi_arena_idx = 0;

  // Batched VAO: base ModelVertex attributes from the arena VBO (divisor 0) + the three per-instance streams
  // (transform loc6-9, interior loc10, inst_tex loc11, divisor 1). Locations are pinned in m2_vert.glsl. The
  // instance buffers are re-filled every frame -- the VAO keeps their name bindings, so orphaning is fine.
  gl.bindVertexArray(vao);

  gl.bindBuffer(GL_ARRAY_BUFFER, arena_vbo);
  gl.enableVertexAttribArray(0); gl.vertexAttribPointer (0, 3, GL_FLOAT,         GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*>(0));
  gl.enableVertexAttribArray(1); gl.vertexAttribPointer (1, 3, GL_FLOAT,         GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*>(20));
  gl.enableVertexAttribArray(2); gl.vertexAttribPointer (2, 2, GL_FLOAT,         GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*>(32));
  gl.enableVertexAttribArray(3); gl.vertexAttribPointer (3, 2, GL_FLOAT,         GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*>(40));
  gl.enableVertexAttribArray(4); gl.vertexAttribIPointer(4, 4, GL_UNSIGNED_BYTE,           sizeof(ModelVertex), reinterpret_cast<void*>(12));
  gl.enableVertexAttribArray(5); gl.vertexAttribIPointer(5, 4, GL_UNSIGNED_BYTE,           sizeof(ModelVertex), reinterpret_cast<void*>(16));

  gl.bindBuffer(GL_ARRAY_BUFFER, inst_tf);
  for (int i = 0; i < 4; ++i)
  {
    gl.enableVertexAttribArray(6 + i);
    gl.vertexAttribPointer(6 + i, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4x4),
                           reinterpret_cast<void*>(static_cast<std::size_t>(i) * sizeof(glm::vec4)));
    gl.vertexAttribDivisor(6 + i, 1);
  }

  gl.bindBuffer(GL_ARRAY_BUFFER, inst_interior);
  gl.enableVertexAttribArray(10);
  gl.vertexAttribPointer(10, 4, GL_FLOAT, GL_FALSE, sizeof(glm::vec4), reinterpret_cast<void*>(0));
  gl.vertexAttribDivisor(10, 1);

  gl.bindBuffer(GL_ARRAY_BUFFER, inst_tex);
  gl.enableVertexAttribArray(11);
  gl.vertexAttribIPointer(11, 4, GL_INT, sizeof(glm::ivec4), reinterpret_cast<void*>(0));
  gl.vertexAttribDivisor(11, 1);

  gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, arena_ibo);
  gl.bindVertexArray(0);
  gl.bindBuffer(GL_ARRAY_BUFFER, 0);

  _mdi_ready = true;
}

bool WorldRender::mdiEnsureModelInArena(Model* m)
{
  // STABLE key = the model's file path. Model* is reused after a model unloads, so a Model* key would alias a
  // different (unloaded) model's arena geometry -> exploded vertices in dense streaming. Same file == same geom.
  std::string const key = m->file_key().stringRepr();
  if (key.empty())
    return false; // no stable identity -> don't risk aliasing; draw it unbatched

  auto const it = _mdi_slots.find(key);
  if (it != _mdi_slots.end())
    return it->second.ok;

  MdiArenaSlot slot;
  std::vector<ModelVertex> const& verts = m->_vertices;
  std::vector<std::uint16_t> const& indices = m->_indices;
  if (verts.empty() || indices.empty())
  {
    _mdi_slots.emplace(key, slot); // ok = false
    return false;
  }

  GLsizeiptr const vbytes = static_cast<GLsizeiptr>(verts.size()) * sizeof(ModelVertex);
  GLsizeiptr const ibytes = static_cast<GLsizeiptr>(indices.size()) * sizeof(std::uint16_t);
  GLsizeiptr const voff = static_cast<GLsizeiptr>(_mdi_arena_vtx) * sizeof(ModelVertex);
  GLsizeiptr const ioff = static_cast<GLsizeiptr>(_mdi_arena_idx) * sizeof(std::uint16_t);
  if (voff + vbytes > _mdi_arena_vbo_cap || ioff + ibytes > _mdi_arena_ibo_cap)
  {
    static int s_overflow_logs = 0;
    if (s_overflow_logs < 3)
    {
      ++s_overflow_logs;
      LogError << "[MDI] geometry arena full -- model draws unbatched: " << m->file_key().stringRepr() << std::endl;
    }
    _mdi_slots.emplace(key, slot);
    return false;
  }

  slot.base_vertex = _mdi_arena_vtx;
  slot.index_base  = static_cast<GLuint>(_mdi_arena_idx);
  gl.bindBuffer(GL_ARRAY_BUFFER, _mdi_buffers[0]);
  gl.bufferSubData(GL_ARRAY_BUFFER, voff, vbytes, verts.data());
  gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, _mdi_buffers[1]);
  gl.bufferSubData(GL_ELEMENT_ARRAY_BUFFER, ioff, ibytes, indices.data());
  // [VULKAN phase C] mirror the same bytes at the same offsets for the VK M2 arena
  {
    auto const* vb = reinterpret_cast<unsigned char const*>(verts.data());
    _mdi_mirror_verts.resize(static_cast<std::size_t>(voff));
    _mdi_mirror_verts.insert(_mdi_mirror_verts.end(), vb, vb + vbytes);
    _mdi_mirror_idx.resize(static_cast<std::size_t>(_mdi_arena_idx));
    _mdi_mirror_idx.insert(_mdi_mirror_idx.end(), indices.begin(), indices.end());
    _mdi_mirror_dirty = true;
  }
  _mdi_arena_vtx += static_cast<GLsizei>(verts.size());
  _mdi_arena_idx += static_cast<GLsizei>(indices.size());
  slot.ok = true;
  _mdi_slots.emplace(key, slot);
  return true;
}

// [perf 2026-08-06] bumped per successful texture upload (TextureManager.cpp); folded into the batch cache
// signature so a model that becomes batchable once its texture loads correctly forces a rebuild.
extern std::atomic<unsigned long long> g_texture_upload_epoch;

// Defined in ModelRender.cpp -- controlled-geoset hide test with the model-aware bare-arm fallback (Jaina fix):
// a controlled family whose selected id is absent from the model (human sleeve default 801) falls back to the
// model's lowest existing variant instead of culling the real arm geometry. Shared so the batch filters below
// match the individual draw path exactly.
bool noggit_geoset_hidden_by_controlled_family(Model* m, std::vector<std::uint16_t> const& controlled_families,
                                               std::vector<std::uint16_t> const& visible_ids, std::uint16_t geoset_id);

void WorldRender::drawDoodadsBatched(
    std::vector<std::pair<Model*, TileRender::DoodadInstanceBuffer const*>> const& draws,
    glm::mat4x4 const& model_view, bool draw_hidden_models,
    math::frustum const& frustum, int animtime)
{
  if (draws.empty())
  {
    _mdi_batched_models.clear();
    _mdi_cache_valid = false;
    return;
  }

  ensureMdiArena();

  // AMORTIZATION: a cheap signature over the visible doodad set (+ the texture-upload epoch, so a model that
  // becomes batchable on texture-load invalidates). Unchanged since the last rebuild => the cached groups AND
  // the GPU instance/indirect buffers are still valid, so skip ALL classify/gather/upload and just re-issue.
  // Stationary + loaded (the common editing case) => a pure cache hit every frame = the amortization win.
  // [perf 2026-08-07] GPU-driven P1: `draws` is now ALL LOADED tiles' doodads (camera-independent), so this
  // signature changes ONLY on tile load/unload / doodad edit / texture-load -- NOT on camera movement. Result:
  // the batch stays cached even while flying (the old visible-set signature rebuilt every camera move). The
  // per-entry contribution is XORed (order-independent) since loaded_tiles() iteration order may vary.
  unsigned long long sig = 1469598103934665603ull;
  auto mix = [&](unsigned long long v) { sig = (sig ^ v) * 1099511628211ull; };
  mix(draws.size());
  mix(draw_hidden_models ? 1ull : 0ull);
  mix(g_texture_upload_epoch.load(std::memory_order_relaxed));
  unsigned long long set_hash = 0;
  for (auto const& pd : draws)
  {
    unsigned long long h = 1469598103934665603ull;
    h = (h ^ static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pd.first))) * 1099511628211ull;
    h = (h ^ static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pd.second))) * 1099511628211ull;
    h = (h ^ static_cast<unsigned long long>(pd.second->count)) * 1099511628211ull;
    set_hash ^= h; // XOR = order-independent
  }
  mix(set_hash);

  if (_mdi_cache_valid && sig == _mdi_last_sig)
  {
    static int s_hit_dbg = 0;
    if ((s_hit_dbg++ % 240) == 0)
      LogError << "[MDI] cache HIT groups=" << _mdi_cached_groups.size()
               << " instances=" << _mdi_cached_instances << std::endl;
    fillMdiBones(model_view, frustum, animtime); // refresh visible shared poses BEFORE the draws read the SSBO
    drawMdiGroups();
    return;
  }

  // ---- CACHE MISS: full rebuild of the batch ----
  _mdi_batched_models.clear();

  // 1) classify each UNIQUE model once (cached in `decision`) + gather its instances across all its tiles.
  //    batchable = geometry fits the arena AND every render pass resolves as a static batch.
  std::unordered_map<Model*, std::vector<glm::mat4x4>> model_instances; // batchable model -> all its transforms
  std::unordered_map<Model*, std::vector<StaticBatchKey>> model_keys;   // batchable model -> per-pass batch keys
  std::unordered_map<Model*, int> decision;                            // 0 unknown, 1 batchable, -1 not

  for (auto const& pd : draws)
  {
    Model* const m = pd.first;
    if (!draw_hidden_models && m->is_hidden())
      continue;

    int& d = decision[m];
    if (d == 0)
    {
      d = -1;
      auto const& passes = m->renderer()->renderPasses();
      if (!passes.empty() && mdiEnsureModelInArena(m))
      {
        std::vector<StaticBatchKey> keys;
        keys.reserve(passes.size());
        bool all_ok = true;
        for (auto const& p : passes)
        {
          StaticBatchKey k;
          if (!p.resolveStaticBatch(m, k)) { all_ok = false; break; }
          keys.push_back(k);
        }
        if (all_ok)
        {
          d = 1;
          model_keys.emplace(m, std::move(keys));
          model_instances.emplace(m, std::vector<glm::mat4x4>{});
          _mdi_batched_models.emplace(m, static_cast<std::uint8_t>(1));
        }
      }
    }

    if (d == 1)
    {
      auto& dst = model_instances[m];
      auto const& src = pd.second->cpu_transforms;
      dst.insert(dst.end(), src.begin(), src.end());
    }
  }

  if (model_instances.empty())
  {
    _mdi_cache_valid = false; // nothing batchable this frame; retry next (don't serve a stale cache)
    return;
  }

  // 2a) [animated MDI 2026-08-07] assign each SHARED-pose animated batched model a FIXED bone-block slot in
  //     the batched bone SSBO, and compute its whole-instance-set WORLD bbox (for the per-frame "animate only
  //     visible" cull in fillMdiBones). Both are cached with the structure (stable across cache hits), so
  //     inst_tex.z/w in the instance stream stay valid; only the SSBO CONTENT refreshes per frame.
  _mdi_bone_models.clear();
  _mdi_bone_total = 0;
  std::unordered_map<Model*, std::pair<std::uint32_t, std::uint32_t>> bone_block; // model -> (base, count)
  for (auto const& mi : model_instances)
  {
    Model* const m = mi.first;
    if (!m->animBones || m->bone_matrices.empty())
      continue;
    std::uint32_t const count = static_cast<std::uint32_t>(m->bone_matrices.size());
    std::uint32_t const base = static_cast<std::uint32_t>(_mdi_bone_total);
    bone_block.emplace(m, std::make_pair(base, count));
    // World bounds over all instances, sphere-based: per instance take its origin +- (model rad x the
    // transform's max axis scale). Model exposes only `rad` (no local AABB member), and a union-of-spheres
    // AABB is plenty for the "any instance visible -> animate this model" cull.
    glm::vec3 bb_min(std::numeric_limits<float>::max());
    glm::vec3 bb_max(std::numeric_limits<float>::lowest());
    for (glm::mat4x4 const& t : mi.second)
    {
      float const sx = glm::length(glm::vec3(t[0]));
      float const sy = glm::length(glm::vec3(t[1]));
      float const sz = glm::length(glm::vec3(t[2]));
      float const r = m->rad * std::max(sx, std::max(sy, sz));
      glm::vec3 const c(t[3]);
      bb_min = glm::min(bb_min, c - glm::vec3(r));
      bb_max = glm::max(bb_max, c + glm::vec3(r));
    }
    _mdi_bone_models.push_back(MdiBoneModel{ m, base, count, bb_min, bb_max });
    _mdi_bone_total += count;
  }

  // 2) group (model, pass) into commands by batch identity. A model's transforms are replicated per pass
  //    (transform + inst_tex are co-located divisor-1 streams selected together by each command's baseInstance).
  struct CmdSpec { Model* model; std::uint32_t pass; int layer0; int layer1; };
  std::map<StaticBatchKey, std::vector<CmdSpec>> groups;
  for (auto const& mi : model_instances)
  {
    Model* const m = mi.first;
    auto const& keys = model_keys[m];
    for (std::uint32_t pi = 0; pi < keys.size(); ++pi)
      groups[keys[pi]].push_back(CmdSpec{ m, pi, keys[pi].layer0, keys[pi].layer1 });
  }

  // 3) lay out the per-instance streams + indirect commands, grouped (group0 cmds, group1 cmds, ...).
  _mdi_scratch_tf.clear();
  _mdi_scratch_interior.clear();
  _mdi_scratch_tex.clear();
  _mdi_scratch_cmds.clear();
  _mdi_issued = 0;
  _mdi_cached_groups.clear();
  _mdi_cached_groups.reserve(groups.size());

  for (auto const& g : groups)
  {
    MdiGroup gr;
    gr.key = g.first;
    gr.first_cmd = static_cast<std::uint32_t>(_mdi_scratch_cmds.size());
    for (CmdSpec const& c : g.second)
    {
      auto const& transforms = model_instances[c.model];
      if (transforms.empty())
        continue;
      auto const slot_it = _mdi_slots.find(c.model->file_key().stringRepr());
      if (slot_it == _mdi_slots.end() || !slot_it->second.ok)
        continue; // arena entry gone/invalid -> skip (drawn unbatched via the persistent fallback)
      MdiArenaSlot const& slot = slot_it->second;
      ModelRenderPass const& pass = c.model->renderer()->renderPasses()[c.pass];

      OpenGL::DrawElementsIndirectCommand cmd;
      cmd.count = pass.index_count;
      cmd.instanceCount = static_cast<GLuint>(transforms.size());
      cmd.firstIndex = slot.index_base + pass.index_start;
      cmd.baseVertex = slot.base_vertex;
      cmd.baseInstance = static_cast<GLuint>(_mdi_scratch_tf.size());
      _mdi_scratch_cmds.push_back(cmd);

      // inst_tex.z/w = this model's bone block (base, count) in the batched bone SSBO; (0,0) = static ->
      // the batched vertex shader keeps the bind pose for it.
      auto const bb_it = bone_block.find(c.model);
      glm::ivec4 const tex(c.layer0, c.layer1,
                           bb_it != bone_block.end() ? static_cast<int>(bb_it->second.first) : 0,
                           bb_it != bone_block.end() ? static_cast<int>(bb_it->second.second) : 0);
      for (glm::mat4x4 const& t : transforms)
      {
        _mdi_scratch_tf.push_back(t);
        _mdi_scratch_interior.emplace_back(0.0f);
        _mdi_scratch_tex.push_back(tex);
      }
    }
    gr.cmd_count = static_cast<std::uint32_t>(_mdi_scratch_cmds.size()) - gr.first_cmd;
    if (gr.cmd_count)
      _mdi_cached_groups.push_back(gr);
  }

  if (_mdi_scratch_cmds.empty())
  {
    _mdi_cache_valid = false;
    return;
  }

  // 4) upload the per-instance streams + indirect command buffer (orphan + refill). These GPU buffers stay
  //    valid across subsequent cache HITS (no re-upload) -- that is the whole point of the amortization.
  GLuint const inst_tf = _mdi_buffers[2], inst_interior = _mdi_buffers[3], inst_tex = _mdi_buffers[4], indirect = _mdi_buffers[5];
  gl.bindBuffer(GL_ARRAY_BUFFER, inst_tf);
  gl.bufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(_mdi_scratch_tf.size() * sizeof(glm::mat4x4)), _mdi_scratch_tf.data(), GL_STREAM_DRAW);
  gl.bindBuffer(GL_ARRAY_BUFFER, inst_interior);
  gl.bufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(_mdi_scratch_interior.size() * sizeof(glm::vec4)), _mdi_scratch_interior.data(), GL_STREAM_DRAW);
  gl.bindBuffer(GL_ARRAY_BUFFER, inst_tex);
  gl.bufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(_mdi_scratch_tex.size() * sizeof(glm::ivec4)), _mdi_scratch_tex.data(), GL_STREAM_DRAW);
  gl.bindBuffer(GL_DRAW_INDIRECT_BUFFER, indirect);
  gl.bufferData(GL_DRAW_INDIRECT_BUFFER, static_cast<GLsizeiptr>(_mdi_scratch_cmds.size() * sizeof(OpenGL::DrawElementsIndirectCommand)), _mdi_scratch_cmds.data(), GL_STREAM_DRAW);

  std::size_t total_instances = 0;
  for (auto const& mi : model_instances)
    total_instances += mi.second.size();
  _mdi_cached_instances = total_instances;
  _mdi_last_sig = sig;
  _mdi_cache_valid = true;

  static int s_mdi_dbg = 0;
  if ((s_mdi_dbg++ % 60) == 0)
  {
    LogError << "[MDI] REBUILD batchedModels=" << model_instances.size()
             << " groups(drawcalls)=" << _mdi_cached_groups.size()
             << " commands=" << _mdi_scratch_cmds.size()
             << " instances=" << total_instances
             << " arenaVtx=" << _mdi_arena_vtx << " arenaIdx=" << _mdi_arena_idx << std::endl;
  }

  fillMdiBones(model_view, frustum, animtime); // fresh block layout -> upload the poses before the first draw
  drawMdiGroups();
}

// Issue the cached groups: constant (batch-invariant) uniforms + GL state once, then one MDI call per group
// with per-group state applied via CHECK-BEFORE-SET. The groups iterate in std::map key order (tex_array0
// first), so consecutive groups usually share the bound array -> most rebinds/uniform sets collapse.
void WorldRender::drawMdiGroups()
{
  if (_mdi_cached_groups.empty())
    return;

  static bool const s_no_dist_fade = std::getenv("NOGGIT_NO_DIST_FADE") != nullptr;
  GLuint const indirect = _mdi_buffers[5];

  OpenGL::Scoped::use_program batched {*_m2_batched_program.get()};
  batched.uniform("model_origin", glm::vec3(0.0f));
  batched.uniform("slice_dist", s_no_dist_fade ? 0.0f : _cull_distance);
  batched.uniform("unfogged", 0);
  batched.uniform("unlit", 0);
  batched.uniform("masked_additive", 0);
  batched.uniform("detail_doodad", -1);
  // NOTE (2026-08-14): tree/doodad A2C on the batched draws was REVERTED -- enabling
  // GL_SAMPLE_ALPHA_TO_COVERAGE across the batched pass punched black holes in canopies and let
  // A2C bleed onto other geometry (the "black world"). foliage_aa stays 0 here; only the ground-
  // clutter grass (detail_doodad path, ModelRender) keeps the distance threshold fix, which is
  // MSAA-independent and safe. Tree canopy AA needs a scoped per-blend-mode approach (TODO).
  batched.uniform("foliage_aa", 0);
  batched.uniform("water_surface_effect", -1);
  batched.uniform("creature_bloom", -1);
  batched.uniform("mesh_color", glm::vec4(1.0f));
  batched.uniform("anim_bones", false);
  batched.uniform("bone_matrix_count", 0);
  batched.uniform("per_instance_bone_stride", 0);
  batched.uniform("tex_matrix_1", glm::mat4x4(1.0f));
  batched.uniform("tex_matrix_2", glm::mat4x4(1.0f));

  gl.disable(GL_BLEND);
  gl.depthMask(GL_TRUE);

  gl.bindVertexArray(_mdi_vao_arr[0]);
  gl.bindBuffer(GL_DRAW_INDIRECT_BUFFER, indirect);

  int last_cull = -1, last_blend = -1, last_ps = -1, last_tu0 = -1, last_tu1 = -1, last_c0 = -1, last_c1 = -1;
  GLuint last_a0 = 0xFFFFFFFFu, last_a1 = 0xFFFFFFFFu;
  for (MdiGroup const& gr : _mdi_cached_groups)
  {
    int const want_cull = gr.key.backface_cull ? 1 : 0;
    if (want_cull != last_cull)
    {
      if (want_cull) gl.enable(GL_CULL_FACE); else gl.disable(GL_CULL_FACE);
      last_cull = want_cull;
    }
    if (static_cast<int>(gr.key.blend_mode) != last_blend)
    { batched.uniform("blend_mode", static_cast<int>(gr.key.blend_mode)); last_blend = static_cast<int>(gr.key.blend_mode); }
    if (gr.key.pixel_shader != last_ps)
    { batched.uniform("pixel_shader", gr.key.pixel_shader); last_ps = gr.key.pixel_shader; }
    if (gr.key.tu_lookup0 != last_tu0)
    { batched.uniform("tex_unit_lookup_1", gr.key.tu_lookup0); last_tu0 = gr.key.tu_lookup0; }
    if (gr.key.tu_lookup1 != last_tu1)
    { batched.uniform("tex_unit_lookup_2", gr.key.tu_lookup1); last_tu1 = gr.key.tu_lookup1; }
    if (gr.key.tex_clamp0 != last_c0)
    { batched.uniform("tex1_clamp", gr.key.tex_clamp0); last_c0 = gr.key.tex_clamp0; }
    if (gr.key.tex_clamp1 != last_c1)
    { batched.uniform("tex2_clamp", gr.key.tex_clamp1); last_c1 = gr.key.tex_clamp1; }
    if (gr.key.tex_array0 != last_a0)
    { gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + 1)); gl.bindTexture(GL_TEXTURE_2D_ARRAY, gr.key.tex_array0); last_a0 = gr.key.tex_array0; }
    if (gr.key.tex_array1 && gr.key.tex_array1 != last_a1)
    { gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + 2)); gl.bindTexture(GL_TEXTURE_2D_ARRAY, gr.key.tex_array1); last_a1 = gr.key.tex_array1; }

    // [phase I] VK owns the M2 batches: skip GL's MDI draw but keep everything above it, because the
    // VK feed is built out of this same grouping. Gating only the CLASSIC per-bucket path left these
    // five batched draws still submitting the models Vulkan had already drawn.
    if (!vk_owns_m2)
    gl.multiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT,
        reinterpret_cast<void*>(static_cast<std::size_t>(gr.first_cmd) * sizeof(OpenGL::DrawElementsIndirectCommand)),
        static_cast<GLsizei>(gr.cmd_count), 0);
    _mdi_issued += gr.cmd_count;   // [VK-DIFF] GL-only doodad path
  }

  gl.bindVertexArray(0);
  gl.bindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

  _world->_n_rendered_objects += _mdi_cached_instances;
}

// [animated MDI 2026-08-07] Per-frame bone refresh for the batched draw. Re-animates ONLY the batched
// animated models whose instance-set world bounds intersect the frustum (an off-screen model keeps last
// frame's pose -- all its instances are GPU-frustum-culled anyway, so the stale pose is never visible),
// writes each model's shared pose into its FIXED block in the scratch, and uploads the whole SSBO once,
// bound at binding 0 = the batched vertex shader's BatchedBones. Bones are computed CPU-side WITHOUT the
// per-model GL TBO upload (upload_bones=false) -- the batch reads only the SSBO. Called right before
// drawDoodadsBatched each frame; cheap when nothing is visible.
void WorldRender::fillMdiBones(glm::mat4x4 const& model_view, math::frustum const& frustum, int animtime)
{
  if (_mdi_bone_models.empty() || _mdi_bone_total == 0)
    return;

  _mdi_scratch_bones.resize(_mdi_bone_total, glm::mat4x4(1.0f));
  bool any = false;
  for (auto const& bm : _mdi_bone_models)
  {
    Model* const m = bm.model;
    if (!m->finishedLoading() || m->loading_failed())
      continue;
    if (!frustum.intersects(bm.bbox_max, bm.bbox_min))
      continue; // off-screen -> keep last pose (GPU-culled)
    if (!m->animcalc)
    {
      // upload_bones stays TRUE: other draw paths (a WMO-doodad/GO copy of the SAME Model) see animcalc==true,
      // skip their own animate, and bind the per-model bone TBO -- with false here that TBO would never update
      // again and those copies would freeze. Cost matches the pre-MDI behaviour (one TBO upload per model).
      m->animate(model_view, 0, animtime);
      m->animcalc = true; // the shared pose is now current for this frame (other paths reuse it)
    }
    std::size_t const n = std::min<std::size_t>(bm.bone_count, m->bone_matrices.size());
    if (n == 0)
      continue;
    std::copy_n(m->bone_matrices.begin(), n, _mdi_scratch_bones.begin() + bm.bone_base);
    any = true;
  }
  constexpr GLenum SSBO_TARGET = 0x90D2; // GL_SHADER_STORAGE_BUFFER (header-independent)
  GLuint const bone_ssbo = _mdi_buffers[6];
  GLsizeiptr const bytes = static_cast<GLsizeiptr>(_mdi_scratch_bones.size() * sizeof(glm::mat4x4));
  if (!any && _mdi_bones_uploaded_once)
  {
    // Nothing visible changed -- the SSBO content is still valid, but ALWAYS re-assert indexed binding 0:
    // another pass (the pib-doodad batch) shares that binding point, and skipping the rebind here would leave
    // the tile batch reading the pib bones on quiet frames.
    gl.bindBufferRange(SSBO_TARGET, 0, bone_ssbo, 0, bytes);
    return;
  }
  gl.bindBuffer(SSBO_TARGET, bone_ssbo);
  gl.bufferData(SSBO_TARGET, bytes, _mdi_scratch_bones.data(), GL_STREAM_DRAW);
  gl.bindBufferRange(SSBO_TARGET, 0, bone_ssbo, 0, bytes);
  gl.bindBuffer(SSBO_TARGET, 0);
  _mdi_bones_uploaded_once = true;
}

// [pib-MDI 2026-08-07] VAO for the billboard-doodad batch: SAME geometry arena (attribs 0-5 + element buffer)
// as the tile batch, but the divisor-1 instance streams point at the pib-owned buffers, which re-upload every
// frame (the visible pib set is per-frame; the tile batch's streams must stay cached, so they can't be shared).
void WorldRender::ensurePibMdi()
{
  if (_pib_ready)
    return;

  _pib_buffers.upload();
  _pib_vao_arr.upload();

  GLuint const arena_vbo     = _mdi_buffers[0];
  GLuint const arena_ibo     = _mdi_buffers[1];
  GLuint const inst_tf       = _pib_buffers[0];
  GLuint const inst_interior = _pib_buffers[1];
  GLuint const inst_tex      = _pib_buffers[2];
  GLuint const vao           = _pib_vao_arr[0];

  gl.bindVertexArray(vao);

  gl.bindBuffer(GL_ARRAY_BUFFER, arena_vbo);
  gl.enableVertexAttribArray(0); gl.vertexAttribPointer (0, 3, GL_FLOAT,         GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*>(0));
  gl.enableVertexAttribArray(1); gl.vertexAttribPointer (1, 3, GL_FLOAT,         GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*>(20));
  gl.enableVertexAttribArray(2); gl.vertexAttribPointer (2, 2, GL_FLOAT,         GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*>(32));
  gl.enableVertexAttribArray(3); gl.vertexAttribPointer (3, 2, GL_FLOAT,         GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*>(40));
  gl.enableVertexAttribArray(4); gl.vertexAttribIPointer(4, 4, GL_UNSIGNED_BYTE,           sizeof(ModelVertex), reinterpret_cast<void*>(12));
  gl.enableVertexAttribArray(5); gl.vertexAttribIPointer(5, 4, GL_UNSIGNED_BYTE,           sizeof(ModelVertex), reinterpret_cast<void*>(16));

  gl.bindBuffer(GL_ARRAY_BUFFER, inst_tf);
  for (int i = 0; i < 4; ++i)
  {
    gl.enableVertexAttribArray(6 + i);
    gl.vertexAttribPointer(6 + i, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4x4),
                           reinterpret_cast<void*>(static_cast<std::size_t>(i) * sizeof(glm::vec4)));
    gl.vertexAttribDivisor(6 + i, 1);
  }

  gl.bindBuffer(GL_ARRAY_BUFFER, inst_interior);
  gl.enableVertexAttribArray(10);
  gl.vertexAttribPointer(10, 4, GL_FLOAT, GL_FALSE, sizeof(glm::vec4), reinterpret_cast<void*>(0));
  gl.vertexAttribDivisor(10, 1);

  gl.bindBuffer(GL_ARRAY_BUFFER, inst_tex);
  gl.enableVertexAttribArray(11);
  gl.vertexAttribIPointer(11, 4, GL_INT, sizeof(glm::ivec4), reinterpret_cast<void*>(0));
  gl.vertexAttribDivisor(11, 1);

  gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, arena_ibo);
  gl.bindVertexArray(0);
  gl.bindBuffer(GL_ARRAY_BUFFER, 0);

  _pib_ready = true;
}

// [pib-MDI 2026-08-07] Collapse the billboard-doodad per-group instanced draws (PibDrawGL ~7.5ms) into a few
// glMultiDrawElementsIndirect calls. Runs IN-PLACE of the old serial loop (same pipeline position -> same
// ordering semantics vs the rest of the frame). Per group: classify every pass via resolveStaticBatch(for_pib)
// (widened: additive/alpha blends + unlit/unfogged carried per-group in the key); consumed groups get
// per-INSTANCE bone blocks in the pib SSBO (big_bones is already the per-instance concatenation, so each
// instance's inst_tex.z = its own pose). Non-batchable groups keep the classic per-group fallback (caller
// skips g.batched). Opaque/alpha-key groups draw before blended ones (strictly better ordering than the old
// interleaved per-model loop); depth-write stays ON for every batched pass (z_buffered passes are rejected,
// mirroring prepareDraw's flag-driven depthMask).
void WorldRender::drawPibBatched(std::vector<PibGroup>& groups)
{
  if (groups.empty())
    return;

  ensureMdiArena();
  ensurePibMdi();

  _pib_scratch_tf.clear();
  _pib_scratch_blp_idx.clear();   // [VULKAN phase C] stays in lockstep with _pib_scratch_tf
  _pib_scratch_state.clear();
  _pib_scratch_src = 1;   // [VK-DIFF] provenance of the arrays VK feeds from
  _pib_scratch_groups.clear();
  _pib_scratch_issued = 0;
  _pib_batch_blps.clear();
  _pib_scratch_interior.clear();
  _pib_scratch_tex.clear();
  _pib_scratch_cmds.clear();
  _pib_scratch_bones.clear();

  std::map<StaticBatchKey, std::vector<OpenGL::DrawElementsIndirectCommand>> keyed;

  for (PibGroup& g : groups)
  {
    g.batched = false; // groups persist across frames (PibPrep cache) -- re-classify every frame
    Model* const m = g.pmodel;
    if (g.transforms.empty() || !m)
      continue;
    if (!mdiEnsureModelInArena(m))
      continue; // geometry not batchable -> fallback
    auto const& passes = m->renderer()->renderPasses();
    if (passes.empty())
      continue;
    std::vector<StaticBatchKey> keys;
    keys.reserve(passes.size());
    bool all_ok = true;
    for (auto const& p : passes)
    {
      StaticBatchKey k;
      if (!p.resolveStaticBatch(m, k, /*for_pib=*/ true)) { all_ok = false; break; }
      keys.push_back(k);
    }
    if (!all_ok)
      continue; // any non-batchable pass -> whole group falls back (no per-pass split)

    auto const slot_it = _mdi_slots.find(m->file_key().stringRepr());
    if (slot_it == _mdi_slots.end() || !slot_it->second.ok)
      continue;
    MdiArenaSlot const& slot = slot_it->second;

    // per-instance bone blocks: big_bones = [inst0 bones..][inst1 bones..].. appended verbatim
    std::uint32_t const bone_count = g.has_bones ? static_cast<std::uint32_t>(m->bone_matrices.size()) : 0u;
    std::uint32_t const bone_base = static_cast<std::uint32_t>(_pib_scratch_bones.size());
    if (g.has_bones && bone_count)
    {
      _pib_scratch_bones.insert(_pib_scratch_bones.end(), g.big_bones.begin(), g.big_bones.end());
    }

    for (std::uint32_t pi = 0; pi < keys.size(); ++pi)
    {
      ModelRenderPass const& pass = m->renderer()->renderPasses()[pi];
      OpenGL::DrawElementsIndirectCommand cmd;
      cmd.count = pass.index_count;
      cmd.instanceCount = static_cast<GLuint>(g.transforms.size());
      cmd.firstIndex = slot.index_base + pass.index_start;
      cmd.baseVertex = slot.base_vertex;
      cmd.baseInstance = static_cast<GLuint>(_pib_scratch_tf.size());
      keyed[keys[pi]].push_back(cmd);

      for (std::size_t i = 0; i < g.transforms.size(); ++i)
      {
        _pib_scratch_tf.push_back(g.transforms[i]);
        _pib_scratch_blp_idx.push_back(batchBlpPairIndex(keys[pi].blp0, keys[pi].blp1));
        _pib_scratch_state.push_back(batchStateVec(keys[pi]));
        _pib_scratch_interior.push_back(g.interiors.size() > i ? g.interiors[i] : glm::vec4(0.0f));
        _pib_scratch_tex.push_back(glm::ivec4(keys[pi].layer0, keys[pi].layer1,
                                              static_cast<int>(bone_base + static_cast<std::uint32_t>(i) * bone_count),
                                              static_cast<int>(bone_count)));
      }
    }
    g.batched = true;
  }

  if (keyed.empty())
    return;

  // Command layout: opaque/alpha-key groups first, then blended (additive glows composite over them).
  struct PibDrawGroup { StaticBatchKey key; std::uint32_t first_cmd; std::uint32_t cmd_count; };
  std::vector<PibDrawGroup> draw_groups;
  draw_groups.reserve(keyed.size());
  for (int blended = 0; blended <= 1; ++blended)
  {
    for (auto const& kv : keyed)
    {
      bool const is_blended = kv.first.blend_mode >= 2;
      if (static_cast<int>(is_blended) != blended)
        continue;
      PibDrawGroup dg;
      dg.key = kv.first;
      dg.first_cmd = static_cast<std::uint32_t>(_pib_scratch_cmds.size());
      _pib_scratch_cmds.insert(_pib_scratch_cmds.end(), kv.second.begin(), kv.second.end());
      dg.cmd_count = static_cast<std::uint32_t>(kv.second.size());
      draw_groups.push_back(dg);
      // [VULKAN] y carries cull in bit0 and CLASSIC ALPHA-KEY in bit1: a v256 alpha-key pass blends
      // with SRC_ALPHA in GL (doc 40 two-era law), so the VK pipeline must pick the blending variant
      // for it. Keying the variant on blend_mode alone drew every classic leaf/blade cutout opaque.
      _pib_scratch_groups.emplace_back(static_cast<int>(dg.key.blend_mode),
                                       (dg.key.backface_cull ? 1 : 0) | (dg.key.classic_alpha ? 2 : 0),
                                       static_cast<int>(dg.first_cmd),
                                       static_cast<int>(dg.cmd_count));
    }
  }

  // [finding 101] VK owns M2: the arrays above are the VK feed and are already built. The GL
  // upload + program + state loop below serve a multiDrawElementsIndirect that is gated off, so
  // they move several hundred KB per frame into buffers nothing draws from.
  if (vk_owns_m2)
  {
    _pib_scratch_slice = 0.0f;   // VK reads this back for its own slice
    return;
  }

  // Upload the per-frame streams + commands + bone SSBO (all STREAM -- the visible pib set changes per frame).
  gl.bindBuffer(GL_ARRAY_BUFFER, _pib_buffers[0]);
  gl.bufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_tf.size() * sizeof(glm::mat4x4)), _pib_scratch_tf.data(), GL_STREAM_DRAW);
  gl.bindBuffer(GL_ARRAY_BUFFER, _pib_buffers[1]);
  gl.bufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_interior.size() * sizeof(glm::vec4)), _pib_scratch_interior.data(), GL_STREAM_DRAW);
  gl.bindBuffer(GL_ARRAY_BUFFER, _pib_buffers[2]);
  gl.bufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_tex.size() * sizeof(glm::ivec4)), _pib_scratch_tex.data(), GL_STREAM_DRAW);
  gl.bindBuffer(GL_ARRAY_BUFFER, 0);
  gl.bindBuffer(GL_DRAW_INDIRECT_BUFFER, _pib_buffers[3]);
  gl.bufferData(GL_DRAW_INDIRECT_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_cmds.size() * sizeof(OpenGL::DrawElementsIndirectCommand)), _pib_scratch_cmds.data(), GL_STREAM_DRAW);

  constexpr GLenum SSBO_TARGET = 0x90D2; // GL_SHADER_STORAGE_BUFFER
  if (!_pib_scratch_bones.empty())
  {
    // Upload + bind the PIB bone SSBO at binding 0 for these draws. fillMdiBones re-asserts the tile batch's
    // SSBO every frame (including cache-hit frames), so this cannot leak into the tile draws. When NO pib
    // model has bones, skip both -- every instance carries w=0 (bind pose) and never reads the SSBO, and
    // bindBufferRange on a store-less buffer would raise GL_INVALID_VALUE.
    GLsizeiptr const bone_bytes = static_cast<GLsizeiptr>(_pib_scratch_bones.size() * sizeof(glm::mat4x4));
    gl.bindBuffer(SSBO_TARGET, _pib_buffers[4]);
    gl.bufferData(SSBO_TARGET, bone_bytes, _pib_scratch_bones.data(), GL_STREAM_DRAW);
    gl.bindBuffer(SSBO_TARGET, 0);
    gl.bindBufferRange(SSBO_TARGET, 0, _pib_buffers[4], 0, bone_bytes);
  }

  OpenGL::Scoped::use_program batched {*_m2_batched_program.get()};
  batched.uniform("model_origin", glm::vec3(0.0f));
  batched.uniform("slice_dist", 0.0f); // WMO doodads: no distance slice (matches the old pib path)
  _pib_scratch_slice = 0.0f;   // VK reads this back for its own slice
  batched.uniform("masked_additive", 0);
  batched.uniform("detail_doodad", -1);
  batched.uniform("foliage_aa", 0); // A2C reverted (black-canopy/black-world regression)
  batched.uniform("water_surface_effect", -1);
  batched.uniform("creature_bloom", -1);
  batched.uniform("mesh_color", glm::vec4(1.0f));
  batched.uniform("anim_bones", false);
  batched.uniform("bone_matrix_count", 0);
  batched.uniform("per_instance_bone_stride", 0);
  batched.uniform("tex_matrix_1", glm::mat4x4(1.0f));
  batched.uniform("tex_matrix_2", glm::mat4x4(1.0f));

  gl.depthMask(GL_TRUE); // z_buffered passes rejected -> every batched pib pass writes depth (prepareDraw parity)

  gl.bindVertexArray(_pib_vao_arr[0]);
  gl.bindBuffer(GL_DRAW_INDIRECT_BUFFER, _pib_buffers[3]);

  int last_cull = -1, last_blend = -1, last_ps = -1, last_tu0 = -1, last_tu1 = -1, last_c0 = -1, last_c1 = -1;
  int last_unfogged = -1, last_unlit = -1;
  GLuint last_a0 = 0xFFFFFFFFu, last_a1 = 0xFFFFFFFFu;
  for (PibDrawGroup const& gr : draw_groups)
  {
    int const want_cull = gr.key.backface_cull ? 1 : 0;
    if (want_cull != last_cull)
    {
      if (want_cull) gl.enable(GL_CULL_FACE); else gl.disable(GL_CULL_FACE);
      last_cull = want_cull;
    }
    int const cur_blend = static_cast<int>(gr.key.blend_mode) | (gr.key.classic_alpha ? 0x100 : 0);
    if (cur_blend != last_blend)
    {
      batched.uniform("alpha_key_classic", gr.key.classic_alpha ? 1 : 0);
      switch (static_cast<M2Blend>(gr.key.blend_mode))
      {
        default:
        case M2Blend::Opaque:
          gl.disable(GL_BLEND);
          break;
        case M2Blend::Alpha_Key:
          gl.disable(GL_BLEND); // alpha-key never blends (opaque cloaks; see m2_frag)
          break;
        case M2Blend::Alpha:
          gl.enable(GL_BLEND);
          gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
          break;
        case M2Blend::No_Add_Alpha:
        case M2Blend::Add: // premultiplied additive: m2_frag folds alpha into RGB for Add (prepareDraw parity)
          gl.enable(GL_BLEND);
          gl.blendFunc(GL_ONE, GL_ONE);
          break;
      }
      batched.uniform("blend_mode", static_cast<int>(gr.key.blend_mode));
      last_blend = cur_blend;
    }
    if (static_cast<int>(gr.key.unfogged) != last_unfogged)
    { batched.uniform("unfogged", static_cast<int>(gr.key.unfogged)); last_unfogged = static_cast<int>(gr.key.unfogged); }
    if (static_cast<int>(gr.key.unlit) != last_unlit)
    { batched.uniform("unlit", static_cast<int>(gr.key.unlit)); last_unlit = static_cast<int>(gr.key.unlit); }
    if (gr.key.pixel_shader != last_ps)
    { batched.uniform("pixel_shader", gr.key.pixel_shader); last_ps = gr.key.pixel_shader; }
    if (gr.key.tu_lookup0 != last_tu0)
    { batched.uniform("tex_unit_lookup_1", gr.key.tu_lookup0); last_tu0 = gr.key.tu_lookup0; }
    if (gr.key.tu_lookup1 != last_tu1)
    { batched.uniform("tex_unit_lookup_2", gr.key.tu_lookup1); last_tu1 = gr.key.tu_lookup1; }
    if (gr.key.tex_clamp0 != last_c0)
    { batched.uniform("tex1_clamp", gr.key.tex_clamp0); last_c0 = gr.key.tex_clamp0; }
    if (gr.key.tex_clamp1 != last_c1)
    { batched.uniform("tex2_clamp", gr.key.tex_clamp1); last_c1 = gr.key.tex_clamp1; }
    if (gr.key.tex_array0 != last_a0)
    { gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + 1)); gl.bindTexture(GL_TEXTURE_2D_ARRAY, gr.key.tex_array0); last_a0 = gr.key.tex_array0; }
    if (gr.key.tex_array1 && gr.key.tex_array1 != last_a1)
    { gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + 2)); gl.bindTexture(GL_TEXTURE_2D_ARRAY, gr.key.tex_array1); last_a1 = gr.key.tex_array1; }

    // [phase I] VK owns the M2 batches: skip GL's MDI draw but keep everything above it, because the
    // VK feed is built out of this same grouping. Gating only the CLASSIC per-bucket path left these
    // five batched draws still submitting the models Vulkan had already drawn.
    if (!vk_owns_m2)
    gl.multiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT,
        reinterpret_cast<void*>(static_cast<std::size_t>(gr.first_cmd) * sizeof(OpenGL::DrawElementsIndirectCommand)),
        static_cast<GLsizei>(gr.cmd_count), 0);
    _pib_scratch_issued += gr.cmd_count;   // [VK-DIFF] what GL really drew
  }

  gl.bindVertexArray(0);
  gl.bindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
  gl.disable(GL_BLEND);
  gl.depthMask(GL_TRUE);
  gl.enable(GL_CULL_FACE);
}

// [dyn-MDI 2026-08-07] The last big per-model draw loop: the DYNAMIC instanced pool (per-frame gathered WMO
// doodads + gameobjects, ~17ms of SubmitInst/DoodadDraw in dense areas) through the per-frame MDI machinery
// proven by the pib pass. SHARED-pose bone block per model (one animate per model per frame -- identical to
// what the classic loop's instanced draw did), every instance points at it via inst_tex.z/w. Runs BEFORE the
// classic models_to_draw loop; consumed buckets go in _dyn_batched_models and the loop keeps all its
// side-effects (additive-light deferral, particle collection) but skips their draws. Exclusions -> fallback:
// GO buckets (own fade/slice semantics), ground clutter (_force_unlit: needs the detail_doodad day/night +
// A2C path), deferred pure-additive light effects (must draw after water), any bucket mid-distance-fade
// (fade < 1 needs the per-instance translucent promote), and anything resolveStaticBatch rejects.
// [VULKAN phase C] index of a batch's (blp0, blp1) pair in the small unique table the VK feed reads.
// EVERY path that pushes to _pib_scratch_tf must push one of these too, or the streams desynchronise
// and the Vulkan feed rejects the frame.
glm::ivec4 WorldRender::batchStateVec(StaticBatchKey const& k)
{
  int flags = 0;
  if (k.unlit) flags |= 1;
  if (k.unfogged) flags |= 2;
  if (k.classic_alpha) flags |= 4;
  if (k.backface_cull) flags |= 8;
  // w packs the tu lookups AND the per-unit UV clamp masks (bits 16-17 / 18-19). GL clamps blade and
  // leaf cards to the texture edge (clamp_uv); VK sampled them unclamped, which differed on exactly
  // the edge texels of every alpha-tested cutout.
  return glm::ivec4(static_cast<int>(k.blend_mode), flags, k.pixel_shader,
                    (k.tu_lookup0 & 0xFF) | ((k.tu_lookup1 & 0xFF) << 8)
                      | ((k.tex_clamp0 & 0x3) << 16) | ((k.tex_clamp1 & 0x3) << 18));
}

int WorldRender::batchBlpPairIndex(std::string const& blp0, std::string const& blp1)
{
  std::pair<std::string, std::string> const names(blp0, blp1);
  for (std::size_t i = 0; i < _pib_batch_blps.size(); ++i)
    if (_pib_batch_blps[i] == names) return static_cast<int>(i);
  _pib_batch_blps.push_back(names);
  return static_cast<int>(_pib_batch_blps.size() - 1);
}

// [VULKAN phase D] Per-frame WMO feed. Called straight after the instance has been drawn by GL, so
// GL's own rendering is untouched; this only appends to the arrays MapView hands to Vulkan.
//
// Geometry lands in an append-only arena keyed by (wmo file, group index) -- the same design the M2
// arena uses -- and each of the group's draw calls becomes one VK draw carrying its blend mode and
// cull flag (the pipeline key). Groups whose mirror is empty are counted as fallback: they are still
// GL-only and phase D cannot close while that counter is non-zero.
extern unsigned g_gl_wmo_draw_calls;   // WMOGroupRender.cpp -- what GL really issued
extern unsigned g_wmo_frame_stamp;     // WMOGroupRender.cpp -- marks this frame's run captures
// [phase J] Where GL's remaining scene pass goes in VK mode. VK's feeds are produced as a side
// effect of these sections, so this says which of them are real scene work VK needs vs GL-only cost.
// The scene pass costs the SAME under both APIs (6.96 GL vs 7.01 VK), so gating GL's draws
// bought nothing -- the VK feed construction that replaced them costs about the same. This
// measures that construction directly: how much of the scene pass is building VK's feeds.
double g_vk_feedbuild_ms = 0.0;
double g_vk_sec_terrain_ms = 0.0;
double g_vk_sec_wmo_ms = 0.0;
double g_vk_sec_m2_ms = 0.0;
double g_vk_sec_gather_ms = 0.0;
double g_vk_sec_ddraw_ms = 0.0;
double g_vk_sec_indiv_ms = 0.0;
double g_vk_sec_clutter_ms = 0.0;
double g_vk_sec_submit_ms = 0.0;
namespace
{
  struct SecTimer
  {
    double& sink;
    std::chrono::steady_clock::time_point t0;
    explicit SecTimer(double& s) : sink(s), t0(std::chrono::steady_clock::now()) {}
    ~SecTimer() { sink += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count(); }
  };
}

extern bool g_vk_owns_wmo;             // WMOGroupRender.cpp -- gate GL's WMO draw, keep its recording

void WorldRender::vkResetWmoFrame()
{
  g_gl_wmo_draw_calls = 0;
  ++g_wmo_frame_stamp;
  // NOGGIT_VK_NO_WMO_GATE=1 puts GL's WMO draw back (A/B for isolating the compose from the port)
  static bool const s_no_gate = std::getenv("NOGGIT_VK_NO_WMO_GATE") != nullptr;
  g_vk_owns_wmo = _vk_owns_wmo && !s_no_gate;
  _vk_wmo_draws.clear();
  _vk_wmo_xforms.clear();
  _vk_wmo_ambients.clear();
  _vk_wmo_fallback = 0;
}

std::size_t WorldRender::glWmoDrawCalls() const { return g_gl_wmo_draw_calls; }

// De-duplicated (tex0, tex1) name pairs for the WMO arena batches -- VK addresses textures by
// bindless id, resolved from these names in MapView using the same cache the M2 batches use.
int WorldRender::vkWmoBlpPairIndex(std::string const& a, std::string const& b)
{
  for (std::size_t i = 0; i < _vk_wmo_blp_pairs.size(); ++i)
  {
    if (_vk_wmo_blp_pairs[i].first == a && _vk_wmo_blp_pairs[i].second == b)
      return static_cast<int>(i);
  }
  _vk_wmo_blp_pairs.emplace_back(a, b);
  return static_cast<int>(_vk_wmo_blp_pairs.size() - 1);
}

// [PIPELINE step 1] Replay the buckets Vulkan refused, on the render thread, after draw() has
// returned. This exists so the traversal can eventually run on a worker: GL calls are not legal
// there, and these instances cannot just be dropped (finding 54 -- doing that loses ~230 pixels).
// Mirrors the instanced-bucket program and state setup exactly, because that is the scope the
// original inline draw ran in.
void WorldRender::drawDeferredGlFallback()
{
  if ((_vk_gl_deferred.empty() && _vk_gl_deferred_pi.empty()) || !_m2_instanced_program)
    return;

  math::frustum const frustum(_vk_def_mvp);
  // debug boxes (draw_models_with_box, off by default): replayed models get their own scratch map
  // rather than the main list, which the box pass has already consumed by now
  std::unordered_map<Model*, std::size_t> boxes;

  OpenGL::Scoped::use_program m2_shader{*_m2_instanced_program.get()};

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
  m2_shader.uniform("unlit", static_cast<int>(model_render_state.unlit));
  m2_shader.uniform("tex_unit_lookup_1", 0);
  m2_shader.uniform("tex_unit_lookup_2", 0);
  m2_shader.uniform("pixel_shader", 0);
  m2_shader.uniform("model_origin", glm::vec3(0.0f));

  for (auto& d : _vk_gl_deferred)
  {
    if (!d.model || d.transforms.empty() || !d.model->renderer())
      continue;
    m2_shader.uniform("slice_dist", d.slice_dist);   // per bucket, as the inline draw had it
    d.interiors.resize(d.transforms.size(), glm::vec4(0.0f));
    d.fades.resize(d.transforms.size(), 1.0f);
    d.model->renderer()->draw(_vk_def_model_view, d.transforms, m2_shader, model_render_state,
                              frustum, _cull_distance, _vk_def_camera_pos,
                              _world->model_animtime, _vk_def_boxes, boxes, _vk_def_display,
                              /*no_cull*/ false, /*representative*/ nullptr,
                              d.interiors, d.fades);
  }
  _vk_gl_deferred.clear();

  // per-instance doodads: non-instanced overload, _m2_program, mirroring the per-instance section
  if (!_vk_gl_deferred_pi.empty() && _m2_program)
  {
    OpenGL::Scoped::use_program doodad_shader{*_m2_program.get()};
    OpenGL::M2RenderState doodad_render_state;
    doodad_render_state.tex_arrays = {0, 0};
    doodad_render_state.tex_indices = {0, 0};
    doodad_render_state.tex_unit_lookups = {0, 0};
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.disable(GL_BLEND);
    gl.depthMask(GL_TRUE);
    gl.enable(GL_CULL_FACE);
    doodad_shader.uniform("blend_mode", 0);
    doodad_shader.uniform("unfogged", static_cast<int>(doodad_render_state.unfogged));
    doodad_shader.uniform("unlit", static_cast<int>(doodad_render_state.unlit));
    doodad_shader.uniform("tex_unit_lookup_1", 0);
    doodad_shader.uniform("tex_unit_lookup_2", 0);
    doodad_shader.uniform("pixel_shader", 0);
    for (auto& d : _vk_gl_deferred_pi)
    {
      if (!d.model || !d.instance || !d.model->renderer())
        continue;
      d.model->renderer()->draw(_vk_def_model_view, *d.instance, doodad_shader, doodad_render_state,
                                frustum, _cull_distance, _vk_def_camera_pos,
                                static_cast<int>(_world->model_animtime), _vk_def_display,
                                /*no_cull*/ false, /*bloom_mask_only*/ false, d.interior);
    }
    _vk_gl_deferred_pi.clear();
  }
}

void WorldRender::vkFeedWmoInstance(WMOInstance* instance)
{
  if (!vk_feeding)
    return;                                     // pure GL frame: no VK consumer
  SecTimer _feed_t(g_vk_feedbuild_ms);
  if (!instance || !instance->wmo.get() || !instance->wmo->finishedLoading())
    return;

  WMO* wmo = instance->wmo.get();
  std::string const& wmo_key = wmo->file_key().stringRepr();

  std::uint32_t const xform_index = static_cast<std::uint32_t>(_vk_wmo_xforms.size());
  _vk_wmo_xforms.push_back(instance->transformMatrix());
  // MOHD ambient: the interior branch's light for this WMO (GL sets it per WMO as a uniform)
  // [0] = MOHD ambient rgb, w = fog mode (0 = zone fog, else 1.0 + start_frac; the bias keeps
  // "off" exactly 0 while still allowing GL's negative mist scaler). [1] = WMO fog colour + end.
  auto const& fogctx = wmo->renderer()->vkFogContext();
  _vk_wmo_ambients.emplace_back(wmo->ambient_light_color.x, wmo->ambient_light_color.y,
                                wmo->ambient_light_color.z,
                                fogctx.use_wmo_fog ? (1.0f + fogctx.start_frac) : 0.0f);
  _vk_wmo_ambients.emplace_back(fogctx.color.x, fogctx.color.y, fogctx.color.z, fogctx.end);
  // [2].x = MOHD 0x2 unified render path: the MapObjU shaders ADD the baked MOCV on top of the
  // light, on both interior and exterior batches.
  _vk_wmo_ambients.emplace_back(wmo->flags.use_unified_render_path ? 1.0f : 0.0f, 0.f, 0.f, 0.f);

  // Portal/frustum visibility for THIS frame, written by the draw above. Empty = all visible.
  auto const& vis = instance->portal_group_visibility;

  bool any = false;
  for (std::size_t gi = 0; gi < wmo->groups.size(); ++gi)
  {
    // NOTE: no portal_group_visibility test here. The frame stamp below is the authoritative
    // "GL drew this group this frame" signal -- it is set by the group draw itself. Testing
    // visibility as well over-culled badly (1 draw emitted against 6 GL issued), because the
    // vector's semantics do not line up one-to-one with what the group draw actually decided.
    auto* gr = wmo->groups[gi].renderer();
    if (!gr)
      continue;

    auto const& verts = gr->vkVertices();
    auto const& idx = gr->vkIndices();
    bool const already_in_arena = gr->vkMirrorTaken();
    if ((verts.empty() || idx.empty()) && !already_in_arena)
    {
      // Not yet uploaded (transient, self-heals as streaming completes) or genuinely empty. Either
      // way VK cannot draw it this frame -- counted so the remaining GL-only geometry stays visible.
      ++_vk_wmo_fallback;
      continue;
    }

    std::string const key = wmo_key + "#" + std::to_string(gi);
    auto slot_it = _vk_wmo_slots.find(key);
    if (slot_it == _vk_wmo_slots.end())
    {
      VkWmoSlot slot;
      if (verts.empty() || idx.empty())
      {
        ++_vk_wmo_fallback;   // taken flag set but no arena slot -> nothing to draw
        continue;
      }
      slot.base_vertex = static_cast<std::int32_t>(_vk_wmo_verts.size());
      slot.index_base = static_cast<std::uint32_t>(_vk_wmo_idx.size());
      slot.batch_base = static_cast<std::uint32_t>(_vk_wmo_batches.size());

      // Copy the vertices, rebasing each batch id into the GLOBAL batch table.
      std::size_t const first_v = _vk_wmo_verts.size();
      _vk_wmo_verts.insert(_vk_wmo_verts.end(), verts.begin(), verts.end());
      // The mapping is 1-BASED (WMOGroupRender writes batch_counter + 1); 0 = no batch, which GL
      // discards outright. Convert to a 0-based GLOBAL index here and keep 0 as the sentinel.
      for (std::size_t vi = first_v; vi < _vk_wmo_verts.size(); ++vi)
      {
        std::uint32_t const local = _vk_wmo_verts[vi].batch_id;
        _vk_wmo_verts[vi].batch_id = local ? (local - 1u + slot.batch_base + 1u) : 0u;
      }

      // Concatenate this group's batch records, keyed to the shared BLP pair table.
      auto const& blps = gr->vkBatchBlps();
      auto const& bts = gr->vkBatches();
      // A group whose batch table is EMPTY while its vertices carry batch ids would leave those ids
      // pointing into the NEXT group's records -- wrong textures on that geometry. Refuse it instead
      // (counted as fallback) so the global table stays consistent.
      if (bts.empty())
      {
        std::uint32_t max_id = 0;
        for (auto const& vtx : verts) max_id = std::max(max_id, vtx.batch_id);
        if (max_id != 0)
        {
          ++_vk_wmo_fallback;
          _vk_wmo_verts.resize(first_v);   // roll the copy back
          continue;
        }
      }
      for (std::size_t bi = 0; bi < bts.size(); ++bi)
      {
        std::string const& b0 = bi < blps.size() ? blps[bi].first : std::string();
        std::string const& b1 = bi < blps.size() ? blps[bi].second : std::string();
        _vk_wmo_batches.emplace_back(vkWmoBlpPairIndex(b0, b1),
                                     static_cast<int>(bts[bi].shader),
                                     static_cast<int>(bts[bi].flags),
                                     static_cast<int>(bts[bi].alpha_test_mode));
      }
      _vk_wmo_batches_dirty = true;

      _vk_wmo_idx.insert(_vk_wmo_idx.end(), idx.begin(), idx.end());
      slot.ok = true;
      _vk_wmo_arena_dirty = true;
      slot_it = _vk_wmo_slots.emplace(key, slot).first;
      gr->releaseVkMirror();   // the arena owns it now -- do not keep a second copy in RAM
    }
    if (!slot_it->second.ok)
    {
      ++_vk_wmo_fallback;
      continue;
    }

    // Replay exactly the runs GL emitted for this group (per-batch frustum culling already
    // applied inside its draw). Re-deriving them here would be a second source of truth and would
    // drift -- the feed over-drew 8x before this.
    if (gr->vkLastRunFrame() != g_wmo_frame_stamp)
    {
      // GL did not draw this group this frame (frustum/portal culled inside WMORender::draw), so
      // its capture is stale. Not a fallback -- there is simply nothing to draw.
      continue;
    }

    for (auto const& run : gr->vkLastRuns())
    {
      if (!run.index_count)
        continue;
      VkWmoDraw d;
      d.index_count = run.index_count;
      d.first_index = slot_it->second.index_base + run.index_start;
      d.base_vertex = slot_it->second.base_vertex;
      d.xform_index = xform_index;
      d.blend_mode = run.blend_mode;
      d.backface_cull = run.backface_cull;
      _vk_wmo_draws.push_back(d);
      any = true;
    }
  }
  (void)any;
}

int WorldRender::vkM2BlpPairIndex(std::string const& a, std::string const& b)
{
  std::string key;
  key.reserve(a.size() + b.size() + 1);
  key.append(a).push_back('\x1f');   // unit separator: cannot appear in a file path
  key.append(b);

  auto const it = _vk_m2_blp_pair_index.find(key);
  if (it != _vk_m2_blp_pair_index.end())
    return it->second;

  int const idx = static_cast<int>(_vk_m2_blp_pairs.size());
  _vk_m2_blp_pairs.emplace_back(a, b);
  _vk_m2_blp_pair_index.emplace(std::move(key), idx);
  return idx;
}

void WorldRender::vkM2SnapshotClear()
{
  _vk_m2_tf.clear(); _vk_m2_interior.clear(); _vk_m2_tex.clear(); _vk_m2_blp_idx.clear();
  _vk_m2_state.clear(); _vk_m2_bones.clear(); _vk_m2_cmds.clear(); _vk_m2_groups.clear();
  _vk_m2_blp_pairs.clear();
  _vk_m2_blp_pair_index.clear();
}

// Append whatever is currently in the scratch arrays, rebasing instance and bone indices onto what
// the snapshot already holds. Called after each producer, because the next one clears the scratch.
void WorldRender::vkM2SnapshotAppend()
{
  if (_pib_scratch_tf.empty() || _pib_scratch_cmds.empty())
    return;
  if (_pib_scratch_blp_idx.size() != _pib_scratch_tf.size()
      || _pib_scratch_state.size() != _pib_scratch_tf.size()
      || _pib_scratch_interior.size() != _pib_scratch_tf.size()
      || _pib_scratch_tex.size() != _pib_scratch_tf.size())
    return;   // desynced streams -- never hand VK a mismatched set

  std::uint32_t const inst_base = static_cast<std::uint32_t>(_vk_m2_tf.size());
  std::uint32_t const bone_base = static_cast<std::uint32_t>(_vk_m2_bones.size());
  std::uint32_t const cmd_base = static_cast<std::uint32_t>(_vk_m2_cmds.size());

  _vk_m2_tf.insert(_vk_m2_tf.end(), _pib_scratch_tf.begin(), _pib_scratch_tf.end());
  _vk_m2_interior.insert(_vk_m2_interior.end(), _pib_scratch_interior.begin(), _pib_scratch_interior.end());
  // blpidx points into the PRODUCER's pair table, which the next producer clears -- remap each one
  // into the snapshot's own table or the textures resolve to -1.
  for (int idx : _pib_scratch_blp_idx)
  {
    if (idx >= 0 && idx < static_cast<int>(_pib_batch_blps.size()))
      _vk_m2_blp_idx.push_back(vkM2BlpPairIndex(_pib_batch_blps[idx].first, _pib_batch_blps[idx].second));
    else
      _vk_m2_blp_idx.push_back(-1);
  }
  _vk_m2_state.insert(_vk_m2_state.end(), _pib_scratch_state.begin(), _pib_scratch_state.end());
  _vk_m2_bones.insert(_vk_m2_bones.end(), _pib_scratch_bones.begin(), _pib_scratch_bones.end());

  // tex info carries the bone base in .z -- rebase it onto the snapshot's bone array
  for (glm::ivec4 t : _pib_scratch_tex)
  {
    if (t.w > 0)
      t.z += static_cast<int>(bone_base);
    _vk_m2_tex.push_back(t);
  }
  for (OpenGL::DrawElementsIndirectCommand c : _pib_scratch_cmds)
  {
    c.baseInstance += inst_base;
    _vk_m2_cmds.push_back(c);
  }
  for (glm::ivec4 g : _pib_scratch_groups)
  {
    g.z += static_cast<int>(cmd_base);   // first_cmd
    _vk_m2_groups.push_back(g);
  }
}

// [VULKAN phase C] Append a CLASSIC-path bucket to the VK feed arrays.
//
// GL draws these buckets with the per-model instanced loop and is not touched here: this runs AFTER
// GL has issued its draws for the frame, and only appends to the scratch arrays MapView hands to
// Vulkan. Anything whose passes do not resolve stays GL-only and is a tracked TODO (the port is not
// done while a single fallback remains).
bool WorldRender::vkFeedClassicBucket(Model* m, std::vector<glm::mat4x4> const& transforms,
                                      std::vector<glm::vec4> const* interiors)
{
  _vk_last_feed_nothing = false;
  if (!vk_feeding)
    return false;                               // pure GL frame: nobody consumes this bucket
  SecTimer _feed_t(g_vk_feedbuild_ms);
  if (!m || transforms.empty() || !m->renderer())
  {
    if (m && !transforms.empty())
      _vk_fb_empty += transforms.size();
    return false;
  }

  auto const& passes = m->renderer()->renderPasses();
  if (passes.empty())
  {
    _vk_fb_empty += transforms.size();
    if (_vk_fb_names.size() < 8)
      _vk_fb_names.push_back("empty " + m->file_key().stringRepr());
    return false;
  }

  // rej code 4 = the pass's geoset is HIDDEN. GL rejects the whole model for that so the individual
  // path can skip the submesh; here the equivalent is to skip just that pass -- GL does not draw it
  // either -- and batch the rest. Any other rejection still drops the model (those passes need
  // rendering a static batch cannot express).
  extern thread_local int g_last_static_batch_reject;
  constexpr int kRejHiddenGeoset = 4;

  std::vector<StaticBatchKey> keys;
  std::vector<std::uint32_t> pass_index;
  keys.reserve(passes.size());
  pass_index.reserve(passes.size());
  for (std::uint32_t pi = 0; pi < passes.size(); ++pi)
  {
    StaticBatchKey k;
    g_last_static_batch_reject = 0;
    if (!passes[pi].resolveStaticBatch(m, k, /*for_pib=*/ true))
    {
      if (g_last_static_batch_reject == kRejHiddenGeoset)
        continue;                         // hidden submesh -- GL skips it too
      _vk_fb_pass += transforms.size();   // TODO: billboards / animated UV / clamp variants
      {
        // One line per distinct model, once: which model still needs GL and WHY. The port is not
        // done while this prints anything.
        extern thread_local int g_last_tex_unit_reject;
        if (_vk_fb_names.size() < 8)
          _vk_fb_names.push_back("rej" + std::to_string(g_last_static_batch_reject)
                                 + "/tex" + std::to_string(g_last_tex_unit_reject) + " "
                                 + m->file_key().stringRepr());
      }
      return false;                             // stays GL-only
    }
    keys.push_back(k);
    pass_index.push_back(pi);
  }
  if (keys.empty())
  {
    // [GL-COST HUNT] This return counted nothing, so these instances showed up as "GL still drew
    // them" with no recorded reason. Every pass is a hidden geoset: VK correctly declines, and GL
    // then draws a model whose every submesh it also skips -- work that produces no pixels.
    _vk_fb_hidden += transforms.size();
    // ~98% of the instances GL still draws in VK mode land here (allHidden 135 of 138). It is
    // TEMPTING to skip GL's draw for them on the grounds that every submesh is hidden -- that was
    // tried and is WRONG: it dropped ~226 pixels (static check 3.70% -> 3.720%, stable over repeats).
    // VK's static-batch hidden test is not equivalent to "GL renders nothing"; GL's own geoset test
    // still draws something here. These have to be deferred to the render thread, not dropped.
    _vk_last_feed_nothing = true;
    return false;   // every pass hidden for the STATIC BATCH path -> GL still owns it
  }

  // shared arena slot (append-only, and it mirrors itself into the VK arena)
  if (!mdiEnsureModelInArena(m))
  {
    _vk_fb_arena += transforms.size();
    return false;
  }
  auto const slot_it = _mdi_slots.find(m->file_key().stringRepr());
  if (slot_it == _mdi_slots.end() || !slot_it->second.ok)
  {
    _vk_fb_arena += transforms.size();
    return false;
  }
  MdiArenaSlot const& slot = slot_it->second;

  // shared pose, exactly like the batched path
  std::uint32_t bone_base = 0, bone_count = 0;
  if (m->animBones && !m->bone_matrices.empty())
  {
    bone_count = static_cast<std::uint32_t>(m->bone_matrices.size());
    bone_base = static_cast<std::uint32_t>(_vk_m2_bones.size());
    _vk_m2_bones.insert(_vk_m2_bones.end(), m->bone_matrices.begin(), m->bone_matrices.end());
  }

  for (std::uint32_t ki = 0; ki < keys.size(); ++ki)
  {
    std::uint32_t const pi = ki;   // index into keys
    ModelRenderPass const& pass = passes[pass_index[ki]];
    OpenGL::DrawElementsIndirectCommand cmd;
    cmd.count = pass.index_count;
    cmd.instanceCount = static_cast<GLuint>(transforms.size());
    cmd.firstIndex = slot.index_base + pass.index_start;
    cmd.baseVertex = slot.base_vertex;
    cmd.baseInstance = static_cast<GLuint>(_vk_m2_tf.size());

    // one group per command: these are appended after the pib groups, so blended classic passes
    // still composite after every opaque pib pass (GL orders opaque-then-blended the same way).
    _vk_m2_groups.emplace_back(static_cast<int>(keys[pi].blend_mode),
                               (keys[pi].backface_cull ? 1 : 0)
                                 | (keys[pi].classic_alpha ? 2 : 0),   // [VULKAN] see below
                               static_cast<int>(_vk_m2_cmds.size()), 1);
    _vk_m2_cmds.push_back(cmd);

    glm::ivec4 const tex(keys[pi].layer0, keys[pi].layer1,
                         static_cast<int>(bone_base), static_cast<int>(bone_count));
    int const blp_idx = vkM2BlpPairIndex(keys[pi].blp0, keys[pi].blp1);
    for (std::size_t i = 0; i < transforms.size(); ++i)
    {
      _vk_m2_tf.push_back(transforms[i]);
      _vk_m2_interior.push_back(interiors && interiors->size() > i ? (*interiors)[i] : glm::vec4(0.0f));
      _vk_m2_tex.push_back(tex);
      _vk_m2_blp_idx.push_back(blp_idx);
      // [VULKAN] flag bit 16 = GROUND CLUTTER. GL decides this per draw from m->_force_unlit and
      // runs a completely different shading law for it (detail_doodad in m2_frag); the batched GL
      // program never takes that branch, so the VK M2 shader has to carry the flag itself.
      glm::ivec4 state = batchStateVec(keys[pi]);
      if (m->_force_unlit)
        state.y |= 16;
      // Sphere-mapped (env) texture units are the one UV mode the VK shader cannot express without
      // the view basis. Count them so "is this content affected?" is a number, not a guess.
      if (keys[pi].tu_lookup0 == 0 || keys[pi].tu_lookup1 == 0)
      {
        static std::set<std::string> s_sphere_seen;
        std::string const key = m->file_key().stringRepr();
        if (s_sphere_seen.insert(key).second)
          LogError << "[VK] M2 SPHERE-MAP uv unit (tracked TODO): " << key
                   << " tu=(" << keys[pi].tu_lookup0 << "," << keys[pi].tu_lookup1 << ")" << std::endl;
      }
      _vk_m2_state.push_back(state);
    }
    _vk_classic_fed += transforms.size();
  }
  return true;   // VK took this bucket -- the caller may skip GL's draw for it
}

void WorldRender::drawDynamicBatched(tsl::robin_map<Model*, std::vector<glm::mat4x4>> const& buckets,
                                     tsl::robin_map<Model*, std::vector<glm::vec4>>& interiors,
                                     tsl::robin_map<Model*, std::vector<float>>& fades,
                                     std::set<Model*> const& go_buckets,
                                     glm::mat4x4 const& model_view, int animtime, bool draw_hidden_models)
{
  _dyn_batched_models.clear();
  if (buckets.empty())
    return;

  ensureMdiArena();
  ensurePibMdi();

  _pib_scratch_tf.clear();
  _pib_scratch_blp_idx.clear();   // [VULKAN phase C] stays in lockstep with _pib_scratch_tf
  _pib_scratch_state.clear();
  _pib_scratch_src = 2;   // [VK-DIFF] provenance of the arrays VK feeds from
  _pib_scratch_groups.clear();
  _pib_scratch_issued = 0;
  _pib_batch_blps.clear();
  _pib_scratch_interior.clear();
  _pib_scratch_tex.clear();
  _pib_scratch_cmds.clear();
  _pib_scratch_bones.clear();

  std::map<StaticBatchKey, std::vector<OpenGL::DrawElementsIndirectCommand>> keyed;

  for (auto const& bucket : buckets)
  {
    Model* const m = bucket.first;
    auto const& transforms = bucket.second;
    if (!m || transforms.empty())
      continue;
    if (!m->finishedLoading() || m->loading_failed())
      continue;
    if (!draw_hidden_models && m->is_hidden())
      continue;
    if (go_buckets.count(m))
      continue; // gameobject buckets keep their own fade/slice semantics
    if (m->_force_unlit)
      continue; // ground clutter: needs the detail_doodad (day/night + A2C) path
    if (is_pure_additive_light_effect(m) && m->_particles.empty() && m->_ribbons.empty())
      continue; // deferred past the water by the classic loop
    // any instance mid-distance-fade -> whole bucket falls back (fade needs the translucent promote)
    {
      auto const fit = fades.find(m);
      if (fit != fades.end())
      {
        bool fading = false;
        for (float f : fit->second)
          if (f < 0.999f) { fading = true; break; }
        if (fading)
          continue;
      }
    }
    if (!mdiEnsureModelInArena(m))
      continue;
    auto const& passes = m->renderer()->renderPasses();
    if (passes.empty())
      continue;
    std::vector<StaticBatchKey> keys;
    keys.reserve(passes.size());
    bool all_ok = true;
    for (auto const& p : passes)
    {
      StaticBatchKey k;
      if (!p.resolveStaticBatch(m, k, /*for_pib=*/ true)) { all_ok = false; break; }
      keys.push_back(k);
    }
    if (!all_ok)
      continue;

    auto const slot_it = _mdi_slots.find(m->file_key().stringRepr());
    if (slot_it == _mdi_slots.end() || !slot_it->second.ok)
      continue;
    MdiArenaSlot const& slot = slot_it->second;

    // SHARED pose: animate once per model per frame (upload_bones stays true so unbatched copies of the same
    // Model elsewhere keep a live TBO), then one bone block all instances share.
    std::uint32_t bone_base = 0, bone_count = 0;
    if (m->animBones)
    {
      if (!m->animcalc)
      {
        m->animate(model_view, 0, animtime);
        m->animcalc = true;
      }
      if (!m->bone_matrices.empty())
      {
        bone_count = static_cast<std::uint32_t>(m->bone_matrices.size());
        bone_base = static_cast<std::uint32_t>(_pib_scratch_bones.size());
        _pib_scratch_bones.insert(_pib_scratch_bones.end(), m->bone_matrices.begin(), m->bone_matrices.end());
      }
    }

    auto const int_it = interiors.find(m);
    std::vector<glm::vec4> const* const inter = int_it != interiors.end() ? &int_it->second : nullptr;

    for (std::uint32_t pi = 0; pi < keys.size(); ++pi)
    {
      ModelRenderPass const& pass = m->renderer()->renderPasses()[pi];
      OpenGL::DrawElementsIndirectCommand cmd;
      cmd.count = pass.index_count;
      cmd.instanceCount = static_cast<GLuint>(transforms.size());
      cmd.firstIndex = slot.index_base + pass.index_start;
      cmd.baseVertex = slot.base_vertex;
      cmd.baseInstance = static_cast<GLuint>(_pib_scratch_tf.size());
      keyed[keys[pi]].push_back(cmd);

      glm::ivec4 const tex(keys[pi].layer0, keys[pi].layer1,
                           static_cast<int>(bone_base), static_cast<int>(bone_count));
      // [VULKAN phase C] which BLP pair this batch samples -- VK addresses textures by bindless index
      // keyed on the file name, so the per-instance stream carries an index into a small unique table.
      int const blp_idx = batchBlpPairIndex(keys[pi].blp0, keys[pi].blp1);
      for (std::size_t i = 0; i < transforms.size(); ++i)
      {
        _pib_scratch_tf.push_back(transforms[i]);
        _pib_scratch_interior.push_back(inter && inter->size() > i ? (*inter)[i] : glm::vec4(0.0f));
        _pib_scratch_tex.push_back(tex);
        _pib_scratch_blp_idx.push_back(blp_idx);
        _pib_scratch_state.push_back(batchStateVec(keys[pi]));
      }
    }
    _dyn_batched_models.emplace(m, static_cast<std::uint8_t>(1));
    _world->_n_rendered_objects += transforms.size();
  }

  if (keyed.empty())
    return;

  // Command layout: opaque/alpha-key groups first, then blended.
  struct DynDrawGroup { StaticBatchKey key; std::uint32_t first_cmd; std::uint32_t cmd_count; };
  std::vector<DynDrawGroup> draw_groups;
  draw_groups.reserve(keyed.size());
  for (int blended = 0; blended <= 1; ++blended)
  {
    for (auto const& kv : keyed)
    {
      bool const is_blended = kv.first.blend_mode >= 2;
      if (static_cast<int>(is_blended) != blended)
        continue;
      DynDrawGroup dg;
      dg.key = kv.first;
      dg.first_cmd = static_cast<std::uint32_t>(_pib_scratch_cmds.size());
      _pib_scratch_cmds.insert(_pib_scratch_cmds.end(), kv.second.begin(), kv.second.end());
      dg.cmd_count = static_cast<std::uint32_t>(kv.second.size());
      draw_groups.push_back(dg);
      // [VULKAN] y carries cull in bit0 and CLASSIC ALPHA-KEY in bit1: a v256 alpha-key pass blends
      // with SRC_ALPHA in GL (doc 40 two-era law), so the VK pipeline must pick the blending variant
      // for it. Keying the variant on blend_mode alone drew every classic leaf/blade cutout opaque.
      _pib_scratch_groups.emplace_back(static_cast<int>(dg.key.blend_mode),
                                       (dg.key.backface_cull ? 1 : 0) | (dg.key.classic_alpha ? 2 : 0),
                                       static_cast<int>(dg.first_cmd),
                                       static_cast<int>(dg.cmd_count));
    }
  }

  // [phase J] VK owns these batches, so GL's copies of the instance / interior / tex /
  // indirect / bone streams are pure waste -- the draw that would read them is gated. The
  // SCRATCH arrays above are still built, because the VK feed is made out of them; only the
  // GL uploads are skipped. Measured: the M2 section was 4.13 of the 4.84 ms GL still cost.
  if (!vk_owns_m2)
  {
    // Upload (bufferData orphaning -> safe to reuse the pib buffers; the later pib pass re-orphans them).
    gl.bindBuffer(GL_ARRAY_BUFFER, _pib_buffers[0]);
    gl.bufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_tf.size() * sizeof(glm::mat4x4)), _pib_scratch_tf.data(), GL_STREAM_DRAW);
    gl.bindBuffer(GL_ARRAY_BUFFER, _pib_buffers[1]);
    gl.bufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_interior.size() * sizeof(glm::vec4)), _pib_scratch_interior.data(), GL_STREAM_DRAW);
    gl.bindBuffer(GL_ARRAY_BUFFER, _pib_buffers[2]);
    gl.bufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_tex.size() * sizeof(glm::ivec4)), _pib_scratch_tex.data(), GL_STREAM_DRAW);
    gl.bindBuffer(GL_ARRAY_BUFFER, 0);
    gl.bindBuffer(GL_DRAW_INDIRECT_BUFFER, _pib_buffers[3]);
    gl.bufferData(GL_DRAW_INDIRECT_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_cmds.size() * sizeof(OpenGL::DrawElementsIndirectCommand)), _pib_scratch_cmds.data(), GL_STREAM_DRAW);

    constexpr GLenum SSBO_TARGET = 0x90D2;
    if (!_pib_scratch_bones.empty())
    {
      GLsizeiptr const bone_bytes = static_cast<GLsizeiptr>(_pib_scratch_bones.size() * sizeof(glm::mat4x4));
      gl.bindBuffer(SSBO_TARGET, _pib_buffers[4]);
      gl.bufferData(SSBO_TARGET, bone_bytes, _pib_scratch_bones.data(), GL_STREAM_DRAW);
      gl.bindBuffer(SSBO_TARGET, 0);
      gl.bindBufferRange(SSBO_TARGET, 0, _pib_buffers[4], 0, bone_bytes);
    }
  }

  OpenGL::Scoped::use_program batched {*_m2_batched_program.get()};
  static bool const s_no_dist_fade = std::getenv("NOGGIT_NO_DIST_FADE") != nullptr;
  batched.uniform("model_origin", glm::vec3(0.0f));
  batched.uniform("slice_dist", s_no_dist_fade ? 0.0f : _cull_distance); // dynamic doodads slice like the loop
  _pib_scratch_slice = s_no_dist_fade ? 0.0f : _cull_distance;   // VK reads this back for its own slice
  batched.uniform("masked_additive", 0);
  batched.uniform("detail_doodad", -1);
  batched.uniform("foliage_aa", 0); // A2C reverted (black-canopy/black-world regression)
  batched.uniform("water_surface_effect", -1);
  batched.uniform("creature_bloom", -1);
  batched.uniform("mesh_color", glm::vec4(1.0f));
  batched.uniform("anim_bones", false);
  batched.uniform("bone_matrix_count", 0);
  batched.uniform("per_instance_bone_stride", 0);
  batched.uniform("tex_matrix_1", glm::mat4x4(1.0f));
  batched.uniform("tex_matrix_2", glm::mat4x4(1.0f));

  gl.depthMask(GL_TRUE);

  gl.bindVertexArray(_pib_vao_arr[0]);
  gl.bindBuffer(GL_DRAW_INDIRECT_BUFFER, _pib_buffers[3]);

  int last_cull = -1, last_blend = -1, last_ps = -1, last_tu0 = -1, last_tu1 = -1, last_c0 = -1, last_c1 = -1;
  int last_unfogged = -1, last_unlit = -1;
  GLuint last_a0 = 0xFFFFFFFFu, last_a1 = 0xFFFFFFFFu;
  for (DynDrawGroup const& gr : draw_groups)
  {
    int const want_cull = gr.key.backface_cull ? 1 : 0;
    if (want_cull != last_cull)
    {
      if (want_cull) gl.enable(GL_CULL_FACE); else gl.disable(GL_CULL_FACE);
      last_cull = want_cull;
    }
    int const cur_blend = static_cast<int>(gr.key.blend_mode) | (gr.key.classic_alpha ? 0x100 : 0);
    if (cur_blend != last_blend)
    {
      batched.uniform("alpha_key_classic", gr.key.classic_alpha ? 1 : 0);
      switch (static_cast<M2Blend>(gr.key.blend_mode))
      {
        default:
        case M2Blend::Opaque:
          gl.disable(GL_BLEND);
          break;
        case M2Blend::Alpha_Key:
          gl.disable(GL_BLEND); // alpha-key never blends (opaque cloaks; see m2_frag)
          break;
        case M2Blend::Alpha:
          gl.enable(GL_BLEND);
          gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
          break;
        case M2Blend::No_Add_Alpha:
        case M2Blend::Add:
          gl.enable(GL_BLEND);
          gl.blendFunc(GL_ONE, GL_ONE);
          break;
      }
      batched.uniform("blend_mode", static_cast<int>(gr.key.blend_mode));
      last_blend = cur_blend;
    }
    if (static_cast<int>(gr.key.unfogged) != last_unfogged)
    { batched.uniform("unfogged", static_cast<int>(gr.key.unfogged)); last_unfogged = static_cast<int>(gr.key.unfogged); }
    if (static_cast<int>(gr.key.unlit) != last_unlit)
    { batched.uniform("unlit", static_cast<int>(gr.key.unlit)); last_unlit = static_cast<int>(gr.key.unlit); }
    if (gr.key.pixel_shader != last_ps)
    { batched.uniform("pixel_shader", gr.key.pixel_shader); last_ps = gr.key.pixel_shader; }
    if (gr.key.tu_lookup0 != last_tu0)
    { batched.uniform("tex_unit_lookup_1", gr.key.tu_lookup0); last_tu0 = gr.key.tu_lookup0; }
    if (gr.key.tu_lookup1 != last_tu1)
    { batched.uniform("tex_unit_lookup_2", gr.key.tu_lookup1); last_tu1 = gr.key.tu_lookup1; }
    if (gr.key.tex_clamp0 != last_c0)
    { batched.uniform("tex1_clamp", gr.key.tex_clamp0); last_c0 = gr.key.tex_clamp0; }
    if (gr.key.tex_clamp1 != last_c1)
    { batched.uniform("tex2_clamp", gr.key.tex_clamp1); last_c1 = gr.key.tex_clamp1; }
    if (gr.key.tex_array0 != last_a0)
    { gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + 1)); gl.bindTexture(GL_TEXTURE_2D_ARRAY, gr.key.tex_array0); last_a0 = gr.key.tex_array0; }
    if (gr.key.tex_array1 && gr.key.tex_array1 != last_a1)
    { gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + 2)); gl.bindTexture(GL_TEXTURE_2D_ARRAY, gr.key.tex_array1); last_a1 = gr.key.tex_array1; }

    // [phase I] VK owns the M2 batches: skip GL's MDI draw but keep everything above it, because the
    // VK feed is built out of this same grouping. Gating only the CLASSIC per-bucket path left these
    // five batched draws still submitting the models Vulkan had already drawn.
    if (!vk_owns_m2)
    gl.multiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT,
        reinterpret_cast<void*>(static_cast<std::size_t>(gr.first_cmd) * sizeof(OpenGL::DrawElementsIndirectCommand)),
        static_cast<GLsizei>(gr.cmd_count), 0);
    _pib_scratch_issued += gr.cmd_count;   // [VK-DIFF] what GL really drew
  }

  gl.bindVertexArray(0);
  gl.bindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
  gl.disable(GL_BLEND);
  gl.depthMask(GL_TRUE);
  gl.enable(GL_CULL_FACE);
}

// [creature MDI 2026-08-18] PHASE A. Fold the far/simple instanced creature groups through the SAME per-frame
// pib-MDI machinery as drawDynamicBatched: one shared bone block per model + one MDI draw per state group,
// replacing the per-(model,display) animate() + bone-TBO STREAM upload churn that dominates M2Creatures. Each
// group carries its representative instance (rep) so resolveStaticBatch can resolve the replaceable skin
// (array,layer) and per-display geoset visibility. A group that doesn't FULLY resolve (hidden geoset,
// unbatchable pass, mid-fade, unresolved skin) is left OUT of out_batched so the classic loop still draws it.
void WorldRender::drawCreaturesBatched(
    std::map<std::pair<Model*, std::uint32_t>, std::vector<glm::mat4x4>> const& creatures,
    std::map<std::pair<Model*, std::uint32_t>, ModelInstance const*> const& reps,
    std::map<std::pair<Model*, std::uint32_t>, std::vector<float>>& fades,
    std::function<glm::vec4(glm::vec3 const&)> const& interior_at,
    glm::mat4x4 const& model_view, int animtime, bool draw_hidden_models,
    std::set<std::pair<Model*, std::uint32_t>>& out_batched)
{
  out_batched.clear();
  if (creatures.empty())
    return;

  ensureMdiArena();
  ensurePibMdi();

  _pib_scratch_tf.clear();
  _pib_scratch_blp_idx.clear();   // [VULKAN phase C] stays in lockstep with _pib_scratch_tf
  _pib_scratch_state.clear();
  _pib_scratch_src = 3;   // [VK-DIFF] provenance of the arrays VK feeds from
  _pib_scratch_groups.clear();
  _pib_scratch_issued = 0;
  _pib_batch_blps.clear();
  _pib_scratch_interior.clear();
  _pib_scratch_tex.clear();
  _pib_scratch_cmds.clear();
  _pib_scratch_bones.clear();

  // The classic creature loop resets animcalc=false per group at DRAW time so the shared pose is recomputed
  // this frame; we run before it, and a creature model that is ONLY ever batched is reset nowhere else -- so
  // reset up front here. animate() in the loop below then latches once per model (all its display groups share
  // the pose). model_animtime is frozen when animations are toggled off, so a re-animate there is idempotent
  // (same frozen pose). Without this a batched-only creature animates once and then freezes forever.
  for (auto const& e : creatures)
    if (e.first.first) { e.first.first->animcalc = false; }

  std::map<StaticBatchKey, std::vector<OpenGL::DrawElementsIndirectCommand>> keyed;
  std::size_t batched_groups = 0, batched_instances = 0;

  for (auto const& entry : creatures)
  {
    Model* const m = entry.first.first;
    auto const& transforms = entry.second;
    if (!m || transforms.empty())
      continue;
    if (!m->finishedLoading() || m->loading_failed())
      continue;
    if (!draw_hidden_models && m->is_hidden())
      continue;
    // any instance mid-distance-fade -> whole group falls back (the constant-alpha batch can't fade)
    {
      auto const fit = fades.find(entry.first);
      if (fit != fades.end())
      {
        bool fading = false;
        for (float f : fit->second)
          if (f < 0.999f) { fading = true; break; }
        if (fading)
          continue;
      }
    }
    auto const rep_it = reps.find(entry.first);
    ModelInstance const* const rep = rep_it != reps.end() ? rep_it->second : nullptr;
    if (!rep)
      continue; // a creature group needs its representative instance to resolve the skin + geosets
    if (!mdiEnsureModelInArena(m))
      continue;
    auto const& passes = m->renderer()->renderPasses();
    if (passes.empty())
      continue;
    // Per-display geoset visibility (mirror prepareDraw): a pass whose geoset this DISPLAY hides is simply not
    // drawn (skipped) -- it is NOT a reason to fail the whole group. Creatures routinely hide geosets, so the
    // doodad-style "any pass fails -> fall the whole group back" would batch almost nothing. Only a genuinely
    // UNBATCHABLE visible pass (animated-uv, disallowed blend, z_buffered, unresolved skin) drops the whole
    // group to the classic per-group path.
    std::vector<bool> const& vis =
      !rep->geosetVisibility().empty() ? rep->geosetVisibility() : m->showGeosets;
    std::vector<std::pair<std::uint32_t, StaticBatchKey>> pass_keys;
    pass_keys.reserve(passes.size());
    bool all_ok = true;
    for (std::uint32_t pi = 0; pi < passes.size(); ++pi)
    {
      ModelRenderPass const& p = passes[pi];
      if (p.submesh >= vis.size() || !vis[p.submesh])
        continue; // geoset hidden for this display -> don't draw this pass (not a group failure)
      if (!rep->controlledGeosetFamilies().empty()
          && noggit_geoset_hidden_by_controlled_family(m, rep->controlledGeosetFamilies(),
                                                       rep->visibleGeosetIds(), static_cast<std::uint16_t>(p.geoset_id)))
        continue; // controlled family hides this geoset id (with bare-arm fallback) -> don't draw this pass
      StaticBatchKey k;
      if (!p.resolveStaticBatch(m, k, /*for_pib=*/ true, rep)) { all_ok = false; break; }
      pass_keys.emplace_back(pi, k);
    }
    if (!all_ok || pass_keys.empty())
      continue; // unbatchable visible pass -> classic per-group draw; or nothing visible -> nothing to batch

    auto const slot_it = _mdi_slots.find(m->file_key().stringRepr());
    if (slot_it == _mdi_slots.end() || !slot_it->second.ok)
      continue;
    MdiArenaSlot const& slot = slot_it->second;

    // SHARED pose: one animate per model per frame (animcalc latch), one bone block all its instances share.
    // Different display groups of the same model share the animation (only the skin differs). This matches the
    // classic instanced creature draw, which also animates the model once and reuses the pose for the group.
    std::uint32_t bone_base = 0, bone_count = 0;
    if (m->animBones)
    {
      if (!m->animcalc)
      {
        m->animate(model_view, 0, animtime);
        m->animcalc = true;
      }
      if (!m->bone_matrices.empty())
      {
        bone_count = static_cast<std::uint32_t>(m->bone_matrices.size());
        bone_base = static_cast<std::uint32_t>(_pib_scratch_bones.size());
        _pib_scratch_bones.insert(_pib_scratch_bones.end(), m->bone_matrices.begin(), m->bone_matrices.end());
      }
    }

    // Interior light per INSTANCE (once), not per pass -- interior_at is a real spatial volume lookup, so
    // calling it passes*instances times (the naive nested form) is a large hidden cost on creatures.
    std::vector<glm::vec4> inter(transforms.size());
    for (std::size_t i = 0; i < transforms.size(); ++i)
      inter[i] = interior_at ? interior_at(glm::vec3(transforms[i][3])) : glm::vec4(0.0f);

    for (auto const& pk : pass_keys)
    {
      ModelRenderPass const& pass = passes[pk.first];
      StaticBatchKey const& key = pk.second;
      OpenGL::DrawElementsIndirectCommand cmd;
      cmd.count = pass.index_count;
      cmd.instanceCount = static_cast<GLuint>(transforms.size());
      cmd.firstIndex = slot.index_base + pass.index_start;
      cmd.baseVertex = slot.base_vertex;
      cmd.baseInstance = static_cast<GLuint>(_pib_scratch_tf.size());
      keyed[key].push_back(cmd);

      glm::ivec4 const tex(key.layer0, key.layer1,
                           static_cast<int>(bone_base), static_cast<int>(bone_count));
      for (std::size_t i = 0; i < transforms.size(); ++i)
      {
        _pib_scratch_tf.push_back(transforms[i]);
        _pib_scratch_interior.push_back(inter[i]);
        _pib_scratch_tex.push_back(tex);
        _pib_scratch_blp_idx.push_back(batchBlpPairIndex(key.blp0, key.blp1));
        _pib_scratch_state.push_back(batchStateVec(key));
      }
    }
    out_batched.insert(entry.first);
    ++batched_groups;
    batched_instances += transforms.size();
    _world->_n_rendered_objects += transforms.size();
  }

  // [CRE-MDI diag] coverage: how many (model,display) groups + instances actually folded into the batch vs
  // fell back to the classic per-group path. Throttled; only runs when NOGGIT_CREATURE_MDI is on.
  {
    static int s_diag_tick = 0;
    if ((s_diag_tick++ % 120) == 0)
      LogError << "[CRE-MDI] batched " << batched_groups << "/" << creatures.size() << " groups, "
               << batched_instances << " instances, " << keyed.size() << " state-groups" << std::endl;
  }

  if (keyed.empty())
    return;

  // Command layout: opaque/alpha-key groups first, then blended (identical to drawDynamicBatched).
  struct CreDrawGroup { StaticBatchKey key; std::uint32_t first_cmd; std::uint32_t cmd_count; };
  std::vector<CreDrawGroup> draw_groups;
  draw_groups.reserve(keyed.size());
  for (int blended = 0; blended <= 1; ++blended)
  {
    for (auto const& kv : keyed)
    {
      bool const is_blended = kv.first.blend_mode >= 2;
      if (static_cast<int>(is_blended) != blended)
        continue;
      CreDrawGroup dg;
      dg.key = kv.first;
      dg.first_cmd = static_cast<std::uint32_t>(_pib_scratch_cmds.size());
      _pib_scratch_cmds.insert(_pib_scratch_cmds.end(), kv.second.begin(), kv.second.end());
      dg.cmd_count = static_cast<std::uint32_t>(kv.second.size());
      draw_groups.push_back(dg);
      // [VULKAN] y carries cull in bit0 and CLASSIC ALPHA-KEY in bit1: a v256 alpha-key pass blends
      // with SRC_ALPHA in GL (doc 40 two-era law), so the VK pipeline must pick the blending variant
      // for it. Keying the variant on blend_mode alone drew every classic leaf/blade cutout opaque.
      _pib_scratch_groups.emplace_back(static_cast<int>(dg.key.blend_mode),
                                       (dg.key.backface_cull ? 1 : 0) | (dg.key.classic_alpha ? 2 : 0),
                                       static_cast<int>(dg.first_cmd),
                                       static_cast<int>(dg.cmd_count));
    }
  }

  // [phase J] VK owns these batches, so GL's copies of the instance / interior / tex /
  // indirect / bone streams are pure waste -- the draw that would read them is gated. The
  // SCRATCH arrays above are still built, because the VK feed is made out of them; only the
  // GL uploads are skipped. Measured: the M2 section was 4.13 of the 4.84 ms GL still cost.
  if (!vk_owns_m2)
  {
    // Upload (bufferData orphaning -> safe to reuse the pib buffers; the later pib pass re-orphans them).
    gl.bindBuffer(GL_ARRAY_BUFFER, _pib_buffers[0]);
    gl.bufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_tf.size() * sizeof(glm::mat4x4)), _pib_scratch_tf.data(), GL_STREAM_DRAW);
    gl.bindBuffer(GL_ARRAY_BUFFER, _pib_buffers[1]);
    gl.bufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_interior.size() * sizeof(glm::vec4)), _pib_scratch_interior.data(), GL_STREAM_DRAW);
    gl.bindBuffer(GL_ARRAY_BUFFER, _pib_buffers[2]);
    gl.bufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_tex.size() * sizeof(glm::ivec4)), _pib_scratch_tex.data(), GL_STREAM_DRAW);
    gl.bindBuffer(GL_ARRAY_BUFFER, 0);
    gl.bindBuffer(GL_DRAW_INDIRECT_BUFFER, _pib_buffers[3]);
    gl.bufferData(GL_DRAW_INDIRECT_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_cmds.size() * sizeof(OpenGL::DrawElementsIndirectCommand)), _pib_scratch_cmds.data(), GL_STREAM_DRAW);

    constexpr GLenum SSBO_TARGET = 0x90D2;
    if (!_pib_scratch_bones.empty())
    {
      GLsizeiptr const bone_bytes = static_cast<GLsizeiptr>(_pib_scratch_bones.size() * sizeof(glm::mat4x4));
      gl.bindBuffer(SSBO_TARGET, _pib_buffers[4]);
      gl.bufferData(SSBO_TARGET, bone_bytes, _pib_scratch_bones.data(), GL_STREAM_DRAW);
      gl.bindBuffer(SSBO_TARGET, 0);
      gl.bindBufferRange(SSBO_TARGET, 0, _pib_buffers[4], 0, bone_bytes);
    }
  }

  OpenGL::Scoped::use_program batched {*_m2_batched_program.get()};
  batched.uniform("model_origin", glm::vec3(0.0f));
  batched.uniform("slice_dist", 0.0f); // creatures fade by OPACITY, never pixel-slice (mirror the classic loop)
  _pib_scratch_slice = 0.0f;   // VK reads this back for its own slice
  batched.uniform("masked_additive", 0);
  batched.uniform("detail_doodad", -1);
  batched.uniform("foliage_aa", 0);
  batched.uniform("water_surface_effect", -1);
  batched.uniform("creature_bloom", -1);
  batched.uniform("mesh_color", glm::vec4(1.0f));
  batched.uniform("anim_bones", false);
  batched.uniform("bone_matrix_count", 0);
  batched.uniform("per_instance_bone_stride", 0);
  batched.uniform("tex_matrix_1", glm::mat4x4(1.0f));
  batched.uniform("tex_matrix_2", glm::mat4x4(1.0f));

  gl.depthMask(GL_TRUE);

  gl.bindVertexArray(_pib_vao_arr[0]);
  gl.bindBuffer(GL_DRAW_INDIRECT_BUFFER, _pib_buffers[3]);

  int last_cull = -1, last_blend = -1, last_ps = -1, last_tu0 = -1, last_tu1 = -1, last_c0 = -1, last_c1 = -1;
  int last_unfogged = -1, last_unlit = -1;
  GLuint last_a0 = 0xFFFFFFFFu, last_a1 = 0xFFFFFFFFu;
  for (CreDrawGroup const& gr : draw_groups)
  {
    int const want_cull = gr.key.backface_cull ? 1 : 0;
    if (want_cull != last_cull)
    {
      if (want_cull) gl.enable(GL_CULL_FACE); else gl.disable(GL_CULL_FACE);
      last_cull = want_cull;
    }
    int const cur_blend = static_cast<int>(gr.key.blend_mode) | (gr.key.classic_alpha ? 0x100 : 0);
    if (cur_blend != last_blend)
    {
      batched.uniform("alpha_key_classic", gr.key.classic_alpha ? 1 : 0);
      switch (static_cast<M2Blend>(gr.key.blend_mode))
      {
        default:
        case M2Blend::Opaque:
          gl.disable(GL_BLEND);
          break;
        case M2Blend::Alpha_Key:
          gl.disable(GL_BLEND); // alpha-key never blends (opaque cloaks; see m2_frag)
          break;
        case M2Blend::Alpha:
          gl.enable(GL_BLEND);
          gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
          break;
        case M2Blend::No_Add_Alpha:
        case M2Blend::Add:
          gl.enable(GL_BLEND);
          gl.blendFunc(GL_ONE, GL_ONE);
          break;
      }
      batched.uniform("blend_mode", static_cast<int>(gr.key.blend_mode));
      last_blend = cur_blend;
    }
    if (static_cast<int>(gr.key.unfogged) != last_unfogged)
    { batched.uniform("unfogged", static_cast<int>(gr.key.unfogged)); last_unfogged = static_cast<int>(gr.key.unfogged); }
    if (static_cast<int>(gr.key.unlit) != last_unlit)
    { batched.uniform("unlit", static_cast<int>(gr.key.unlit)); last_unlit = static_cast<int>(gr.key.unlit); }
    if (gr.key.pixel_shader != last_ps)
    { batched.uniform("pixel_shader", gr.key.pixel_shader); last_ps = gr.key.pixel_shader; }
    if (gr.key.tu_lookup0 != last_tu0)
    { batched.uniform("tex_unit_lookup_1", gr.key.tu_lookup0); last_tu0 = gr.key.tu_lookup0; }
    if (gr.key.tu_lookup1 != last_tu1)
    { batched.uniform("tex_unit_lookup_2", gr.key.tu_lookup1); last_tu1 = gr.key.tu_lookup1; }
    if (gr.key.tex_clamp0 != last_c0)
    { batched.uniform("tex1_clamp", gr.key.tex_clamp0); last_c0 = gr.key.tex_clamp0; }
    if (gr.key.tex_clamp1 != last_c1)
    { batched.uniform("tex2_clamp", gr.key.tex_clamp1); last_c1 = gr.key.tex_clamp1; }
    if (gr.key.tex_array0 != last_a0)
    { gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + 1)); gl.bindTexture(GL_TEXTURE_2D_ARRAY, gr.key.tex_array0); last_a0 = gr.key.tex_array0; }
    if (gr.key.tex_array1 && gr.key.tex_array1 != last_a1)
    { gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + 2)); gl.bindTexture(GL_TEXTURE_2D_ARRAY, gr.key.tex_array1); last_a1 = gr.key.tex_array1; }

    // [phase I] VK owns the M2 batches: skip GL's MDI draw but keep everything above it, because the
    // VK feed is built out of this same grouping. Gating only the CLASSIC per-bucket path left these
    // five batched draws still submitting the models Vulkan had already drawn.
    if (!vk_owns_m2)
    gl.multiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT,
        reinterpret_cast<void*>(static_cast<std::size_t>(gr.first_cmd) * sizeof(OpenGL::DrawElementsIndirectCommand)),
        static_cast<GLsizei>(gr.cmd_count), 0);
    _pib_scratch_issued += gr.cmd_count;   // [VK-DIFF] what GL really drew
  }

  gl.bindVertexArray(0);
  gl.bindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
  gl.disable(GL_BLEND);
  gl.depthMask(GL_TRUE);
  gl.enable(GL_CULL_FACE);
}

// [creature body MDI 2026-08-18] PHASE A (real). Batch the INDIVIDUAL creature BODY draws with PER-INSTANCE
// bone blocks -- mirrors drawPibBatched's per-instance-bone MDI + drawCreaturesBatched's rep skin/geoset
// resolve, but animates each body itself (the individual pre-pass recipe) so every near creature keeps its own
// live pose. BODIES ONLY; the caller keeps mounts/attachments/particles/mid-fade on the classic path and
// restores each batched body's _animation_bones before its attachment draws (attachment placement reads it).
void WorldRender::drawCreatureBodiesBatched(
    std::vector<CreatureBodyBatchItem> const& items,
    std::function<glm::vec4(glm::vec3 const&)> const& interior_at,
    glm::mat4x4 const& model_view, int animtime, bool draw_hidden_models,
    std::set<ModelInstance const*>& out_batched)
{
  out_batched.clear();
  if (items.empty())
    return;

  ensureMdiArena();
  ensurePibMdi();

  // 1) Classify the batchable BODIES. Anything rejected stays on the individual path (safe fallback).
  struct BItem { ModelInstance* inst; Model* m; std::uint32_t display_id; int anim_time_offset; };
  std::vector<BItem> batchable;
  batchable.reserve(items.size());
  // [CRE-BODY-DIAG 2026-08-19] tally WHY bodies reject (all 0/N -> find the universal gate). Remove once fixed.
  int cr_null=0, cr_load=0, cr_hidden=0, cr_fade=0, cr_alpha=0, cr_tint=0, cr_mount=0, cr_part=0, cr_bones=0;
  for (auto const& it : items)
  {
    ModelInstance* const inst = it.instance;
    if (!inst) { ++cr_null; continue; }
    Model* const m = inst->model.get();
    if (!m || !m->finishedLoading() || m->loading_failed()) { ++cr_load; continue; }
    if (!draw_hidden_models && m->is_hidden()) { ++cr_hidden; continue; }
    if (it.fade < 0.999f) { ++cr_fade; continue; }                  // mid-fade -> constant-alpha batch can't fade
    if (inst->model_alpha < 0.999f) { ++cr_alpha; continue; }       // translucent creature
    if (inst->model_tint != glm::vec3(1.0f)) { ++cr_tint; continue; } // tinted creature
    if (it.has_mount) { ++cr_mount; continue; }                     // mounted -> individual (seat bones)
    if (!m->_particles.empty() || !m->_ribbons.empty()) { ++cr_part; continue; } // particle/ribbon body
    if (!m->animBones) { ++cr_bones; continue; }                    // no skeleton -> not this path
    batchable.push_back({inst, m, it.display_id, it.anim_time_offset});
  }
  {
    static int s_cd_tick = 0;
    if ((s_cd_tick++ % 120) == 0)
      LogError << "[CRE-BODY-CLASS] items=" << items.size() << " batchable=" << batchable.size()
               << " rej: null=" << cr_null << " load=" << cr_load << " hidden=" << cr_hidden
               << " fade=" << cr_fade << " alpha=" << cr_alpha << " tint=" << cr_tint
               << " mount=" << cr_mount << " particle=" << cr_part << " bones=" << cr_bones << std::endl;
  }
  if (batchable.empty())
    return;

  // 2) PER-INSTANCE animate (the individual pre-pass recipe, upload_bones=false = CPU only, no GL on workers).
  //    Grouped BY MODEL so same-model instances stay serial (animate() mutates shared Model state); distinct
  //    models run across the pool. Store each pose locally AND into inst->_animation_bones -- the attachment
  //    placement + shadow pass read that, so a batched body still seats its gear and casts a correct shadow.
  std::vector<std::vector<glm::mat4x4>> per_instance_bones(batchable.size());
  {
    noggit::perf::Scoped _prof_ca(noggit::perf::Phase::AnimateCPU);
    std::unordered_map<Model*, std::vector<std::size_t>> by_model;
    for (std::size_t i = 0; i < batchable.size(); ++i) { by_model[batchable[i].m].push_back(i); }
    std::vector<std::pair<Model*, std::vector<std::size_t>>> groups(by_model.begin(), by_model.end());
    auto const animate_group = [&](std::pair<Model*, std::vector<std::size_t>>& grp)
    {
      Model* const m = grp.first;
      for (std::size_t idx : grp.second)
      {
        ModelInstance* const inst = batchable[idx].inst;
        int const c_animtime = animtime + batchable[idx].anim_time_offset;
        int anim_id = inst->forcedAnimationId() >= 0 ? inst->forcedAnimationId() : 0;
        if (anim_id != 0 && !m->hasAnimationId(anim_id)) { anim_id = 0; }
        m->_hand_overlay_active_main = inst->closeHandMain();
        m->_hand_overlay_active_off = inst->closeHandOff();
        m->_lower_body_twist = inst->lower_body_twist;
        m->_anim_time_scale = inst->anim_time_scale;
        m->_active_idle_key = static_cast<std::uint64_t>(inst->uid);
        m->animcalc = false;
        m->animate(model_view * inst->transformMatrix(), anim_id, c_animtime, /*upload_bones=*/false);
        per_instance_bones[idx] = m->bone_matrices;
        inst->_animation_bones = m->bone_matrices; // client-parity: the unit OWNS its bones
      }
    };
    if (auto* tp = noggit::render_pool())
      tp->parallel_for(groups.size(), [&](std::size_t gi) { animate_group(groups[gi]); });
    else
      for (auto& g : groups) { animate_group(g); }
  }

  // 3) Group by (model, display_id): one skin + geoset per group, PER-INSTANCE bones.
  std::map<std::pair<Model*, std::uint32_t>, std::vector<std::size_t>> grouped;
  for (std::size_t i = 0; i < batchable.size(); ++i)
    grouped[std::make_pair(batchable[i].m, batchable[i].display_id)].push_back(i);

  _pib_scratch_tf.clear();
  _pib_scratch_blp_idx.clear();   // [VULKAN phase C] stays in lockstep with _pib_scratch_tf
  _pib_scratch_state.clear();
  _pib_scratch_src = 4;   // [VK-DIFF] provenance of the arrays VK feeds from
  _pib_scratch_groups.clear();
  _pib_scratch_issued = 0;
  _pib_batch_blps.clear();
  _pib_scratch_interior.clear();
  _pib_scratch_tex.clear();
  _pib_scratch_cmds.clear();
  _pib_scratch_bones.clear();
  std::map<StaticBatchKey, std::vector<OpenGL::DrawElementsIndirectCommand>> keyed;
  std::size_t batched_bodies = 0;
  // [CRE-BODY-DIAG] per-group reject tally + resolveStaticBatch reject-code histogram (see log below).
  extern thread_local int g_last_static_batch_reject; // set by ModelRenderPass::resolveStaticBatch's rej()
  int g_arena=0, g_passes=0, g_resolve=0, g_novis=0, g_slot=0, g_bones=0;
  int resolve_codes[16] = {0};

  for (auto const& gkv : grouped)
  {
    Model* const m = gkv.first.first;
    auto const& idxs = gkv.second;
    ModelInstance* const rep = batchable[idxs[0]].inst; // skin + geoset representative for the group
    if (!mdiEnsureModelInArena(m)) { ++g_arena; continue; }
    auto const& passes = m->renderer()->renderPasses();
    if (passes.empty()) { ++g_passes; continue; }

    // visible batchable passes (rep skin/geoset) -- same rule as drawCreaturesBatched.
    std::vector<bool> const& vis = !rep->geosetVisibility().empty() ? rep->geosetVisibility() : m->showGeosets;
    std::vector<std::pair<std::uint32_t, StaticBatchKey>> pass_keys;
    pass_keys.reserve(passes.size());
    bool all_ok = true;
    for (std::uint32_t pi = 0; pi < passes.size(); ++pi)
    {
      ModelRenderPass const& p = passes[pi];
      if (p.submesh >= vis.size() || !vis[p.submesh])
        continue; // geoset hidden -> don't draw this pass (not a group failure)
      if (!rep->controlledGeosetFamilies().empty()
          && noggit_geoset_hidden_by_controlled_family(m, rep->controlledGeosetFamilies(),
                                                       rep->visibleGeosetIds(), static_cast<std::uint16_t>(p.geoset_id)))
        continue;
      StaticBatchKey k;
      if (!p.resolveStaticBatch(m, k, /*for_pib=*/ true, rep))
      { all_ok = false; resolve_codes[g_last_static_batch_reject & 15]++; break; }
      pass_keys.emplace_back(pi, k);
    }
    if (!all_ok || pass_keys.empty())
    { if (!all_ok) { ++g_resolve; } else { ++g_novis; } continue; } // unbatchable pass / nothing visible

    auto const slot_it = _mdi_slots.find(m->file_key().stringRepr());
    if (slot_it == _mdi_slots.end() || !slot_it->second.ok) { ++g_slot; continue; }
    MdiArenaSlot const& slot = slot_it->second;

    // PER-INSTANCE bone blocks: append each instance's own pose; inst_tex.z indexes its block base.
    std::uint32_t const bone_count = static_cast<std::uint32_t>(m->bone_matrices.size());
    std::uint32_t const group_bone_base = static_cast<std::uint32_t>(_pib_scratch_bones.size());
    bool bones_ok = bone_count > 0;
    for (std::size_t gi = 0; gi < idxs.size() && bones_ok; ++gi)
    {
      auto const& bones = per_instance_bones[idxs[gi]];
      if (bones.size() != bone_count) { bones_ok = false; break; }
      _pib_scratch_bones.insert(_pib_scratch_bones.end(), bones.begin(), bones.end());
    }
    if (!bones_ok) { ++g_bones; continue; } // pose missing / size mismatch -> group stays individual (rare)

    // interior light per INSTANCE (once, not per pass) -- interior_at is a real spatial lookup.
    std::vector<glm::vec4> inter(idxs.size());
    for (std::size_t gi = 0; gi < idxs.size(); ++gi)
    {
      glm::mat4x4 const tf = batchable[idxs[gi]].inst->transformMatrix();
      inter[gi] = interior_at ? interior_at(glm::vec3(tf[3])) : glm::vec4(0.0f);
    }

    for (auto const& pk : pass_keys)
    {
      ModelRenderPass const& pass = passes[pk.first];
      StaticBatchKey const& key = pk.second;
      OpenGL::DrawElementsIndirectCommand cmd;
      cmd.count = pass.index_count;
      cmd.instanceCount = static_cast<GLuint>(idxs.size());
      cmd.firstIndex = slot.index_base + pass.index_start;
      cmd.baseVertex = slot.base_vertex;
      cmd.baseInstance = static_cast<GLuint>(_pib_scratch_tf.size());
      keyed[key].push_back(cmd);

      for (std::size_t gi = 0; gi < idxs.size(); ++gi)
      {
        _pib_scratch_tf.push_back(batchable[idxs[gi]].inst->transformMatrix());
        _pib_scratch_blp_idx.push_back(batchBlpPairIndex(key.blp0, key.blp1));
        _pib_scratch_state.push_back(batchStateVec(key));
        _pib_scratch_interior.push_back(inter[gi]);
        _pib_scratch_tex.push_back(glm::ivec4(key.layer0, key.layer1,
            static_cast<int>(group_bone_base + static_cast<std::uint32_t>(gi) * bone_count),
            static_cast<int>(bone_count)));
      }
    }
    for (std::size_t gi = 0; gi < idxs.size(); ++gi)
    {
      out_batched.insert(batchable[idxs[gi]].inst);
      ++batched_bodies;
      ++_world->_n_rendered_objects;
    }
  }

  {
    static int s_diag_tick = 0;
    if ((s_diag_tick++ % 120) == 0)
      LogError << "[CRE-BODY-MDI] batched " << batched_bodies << "/" << items.size()
               << " bodies, " << keyed.size() << " state-groups; groups=" << grouped.size()
               << " grej: arena=" << g_arena << " passes=" << g_passes << " resolve=" << g_resolve
               << " novis=" << g_novis << " slot=" << g_slot << " bones=" << g_bones
               << " | resolve-code: blend(3)=" << resolve_codes[3] << " flags(4)=" << resolve_codes[4]
               << " color(5)=" << resolve_codes[5] << " alpha(7)=" << resolve_codes[7]
               << " tex0(12)=" << resolve_codes[12] << " tex1(13)=" << resolve_codes[13]
               << " animuv(14)=" << resolve_codes[14] << std::endl;
  }

  if (keyed.empty())
    return;

  // Command layout: opaque/alpha-key groups first, then blended (identical to drawCreaturesBatched).
  struct BodyDrawGroup { StaticBatchKey key; std::uint32_t first_cmd; std::uint32_t cmd_count; };
  std::vector<BodyDrawGroup> draw_groups;
  draw_groups.reserve(keyed.size());
  for (int blended = 0; blended <= 1; ++blended)
  {
    for (auto const& kv : keyed)
    {
      bool const is_blended = kv.first.blend_mode >= 2;
      if (static_cast<int>(is_blended) != blended)
        continue;
      BodyDrawGroup dg;
      dg.key = kv.first;
      dg.first_cmd = static_cast<std::uint32_t>(_pib_scratch_cmds.size());
      _pib_scratch_cmds.insert(_pib_scratch_cmds.end(), kv.second.begin(), kv.second.end());
      dg.cmd_count = static_cast<std::uint32_t>(kv.second.size());
      draw_groups.push_back(dg);
      // [VULKAN] y carries cull in bit0 and CLASSIC ALPHA-KEY in bit1: a v256 alpha-key pass blends
      // with SRC_ALPHA in GL (doc 40 two-era law), so the VK pipeline must pick the blending variant
      // for it. Keying the variant on blend_mode alone drew every classic leaf/blade cutout opaque.
      _pib_scratch_groups.emplace_back(static_cast<int>(dg.key.blend_mode),
                                       (dg.key.backface_cull ? 1 : 0) | (dg.key.classic_alpha ? 2 : 0),
                                       static_cast<int>(dg.first_cmd),
                                       static_cast<int>(dg.cmd_count));
    }
  }

  gl.bindBuffer(GL_ARRAY_BUFFER, _pib_buffers[0]);
  gl.bufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_tf.size() * sizeof(glm::mat4x4)), _pib_scratch_tf.data(), GL_STREAM_DRAW);
  gl.bindBuffer(GL_ARRAY_BUFFER, _pib_buffers[1]);
  gl.bufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_interior.size() * sizeof(glm::vec4)), _pib_scratch_interior.data(), GL_STREAM_DRAW);
  gl.bindBuffer(GL_ARRAY_BUFFER, _pib_buffers[2]);
  gl.bufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_tex.size() * sizeof(glm::ivec4)), _pib_scratch_tex.data(), GL_STREAM_DRAW);
  gl.bindBuffer(GL_ARRAY_BUFFER, 0);
  gl.bindBuffer(GL_DRAW_INDIRECT_BUFFER, _pib_buffers[3]);
  gl.bufferData(GL_DRAW_INDIRECT_BUFFER, static_cast<GLsizeiptr>(_pib_scratch_cmds.size() * sizeof(OpenGL::DrawElementsIndirectCommand)), _pib_scratch_cmds.data(), GL_STREAM_DRAW);

  constexpr GLenum SSBO_TARGET = 0x90D2;
  if (!_pib_scratch_bones.empty())
  {
    GLsizeiptr const bone_bytes = static_cast<GLsizeiptr>(_pib_scratch_bones.size() * sizeof(glm::mat4x4));
    gl.bindBuffer(SSBO_TARGET, _pib_buffers[4]);
    gl.bufferData(SSBO_TARGET, bone_bytes, _pib_scratch_bones.data(), GL_STREAM_DRAW);
    gl.bindBuffer(SSBO_TARGET, 0);
    gl.bindBufferRange(SSBO_TARGET, 0, _pib_buffers[4], 0, bone_bytes);
  }

  OpenGL::Scoped::use_program batched {*_m2_batched_program.get()};
  batched.uniform("model_origin", glm::vec3(0.0f));
  batched.uniform("slice_dist", 0.0f); // creatures fade by OPACITY, never pixel-slice
  _pib_scratch_slice = 0.0f;   // VK reads this back for its own slice
  batched.uniform("masked_additive", 0);
  batched.uniform("detail_doodad", -1);
  batched.uniform("foliage_aa", 0);
  batched.uniform("water_surface_effect", -1);
  batched.uniform("creature_bloom", -1);
  batched.uniform("mesh_color", glm::vec4(1.0f));
  batched.uniform("anim_bones", false);
  batched.uniform("bone_matrix_count", 0);
  batched.uniform("per_instance_bone_stride", 0);
  batched.uniform("tex_matrix_1", glm::mat4x4(1.0f));
  batched.uniform("tex_matrix_2", glm::mat4x4(1.0f));

  gl.depthMask(GL_TRUE);
  gl.bindVertexArray(_pib_vao_arr[0]);
  gl.bindBuffer(GL_DRAW_INDIRECT_BUFFER, _pib_buffers[3]);

  int last_cull = -1, last_blend = -1, last_ps = -1, last_tu0 = -1, last_tu1 = -1, last_c0 = -1, last_c1 = -1;
  int last_unfogged = -1, last_unlit = -1;
  GLuint last_a0 = 0xFFFFFFFFu, last_a1 = 0xFFFFFFFFu;
  for (BodyDrawGroup const& gr : draw_groups)
  {
    int const want_cull = gr.key.backface_cull ? 1 : 0;
    if (want_cull != last_cull)
    {
      if (want_cull) gl.enable(GL_CULL_FACE); else gl.disable(GL_CULL_FACE);
      last_cull = want_cull;
    }
    int const cur_blend = static_cast<int>(gr.key.blend_mode) | (gr.key.classic_alpha ? 0x100 : 0);
    if (cur_blend != last_blend)
    {
      batched.uniform("alpha_key_classic", gr.key.classic_alpha ? 1 : 0);
      switch (static_cast<M2Blend>(gr.key.blend_mode))
      {
        default:
        case M2Blend::Opaque:
          gl.disable(GL_BLEND);
          break;
        case M2Blend::Alpha_Key:
          gl.disable(GL_BLEND); // alpha-key never blends (opaque cloaks; see m2_frag)
          break;
        case M2Blend::Alpha:
          gl.enable(GL_BLEND);
          gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
          break;
        case M2Blend::No_Add_Alpha:
        case M2Blend::Add:
          gl.enable(GL_BLEND);
          gl.blendFunc(GL_ONE, GL_ONE);
          break;
      }
      batched.uniform("blend_mode", static_cast<int>(gr.key.blend_mode));
      last_blend = cur_blend;
    }
    if (static_cast<int>(gr.key.unfogged) != last_unfogged)
    { batched.uniform("unfogged", static_cast<int>(gr.key.unfogged)); last_unfogged = static_cast<int>(gr.key.unfogged); }
    if (static_cast<int>(gr.key.unlit) != last_unlit)
    { batched.uniform("unlit", static_cast<int>(gr.key.unlit)); last_unlit = static_cast<int>(gr.key.unlit); }
    if (gr.key.pixel_shader != last_ps)
    { batched.uniform("pixel_shader", gr.key.pixel_shader); last_ps = gr.key.pixel_shader; }
    if (gr.key.tu_lookup0 != last_tu0)
    { batched.uniform("tex_unit_lookup_1", gr.key.tu_lookup0); last_tu0 = gr.key.tu_lookup0; }
    if (gr.key.tu_lookup1 != last_tu1)
    { batched.uniform("tex_unit_lookup_2", gr.key.tu_lookup1); last_tu1 = gr.key.tu_lookup1; }
    if (gr.key.tex_clamp0 != last_c0)
    { batched.uniform("tex1_clamp", gr.key.tex_clamp0); last_c0 = gr.key.tex_clamp0; }
    if (gr.key.tex_clamp1 != last_c1)
    { batched.uniform("tex2_clamp", gr.key.tex_clamp1); last_c1 = gr.key.tex_clamp1; }
    if (gr.key.tex_array0 != last_a0)
    { gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + 1)); gl.bindTexture(GL_TEXTURE_2D_ARRAY, gr.key.tex_array0); last_a0 = gr.key.tex_array0; }
    if (gr.key.tex_array1 && gr.key.tex_array1 != last_a1)
    { gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + 2)); gl.bindTexture(GL_TEXTURE_2D_ARRAY, gr.key.tex_array1); last_a1 = gr.key.tex_array1; }

    // [phase I] VK owns the M2 batches: skip GL's MDI draw but keep everything above it, because the
    // VK feed is built out of this same grouping. Gating only the CLASSIC per-bucket path left these
    // five batched draws still submitting the models Vulkan had already drawn.
    if (!vk_owns_m2)
    gl.multiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT,
        reinterpret_cast<void*>(static_cast<std::size_t>(gr.first_cmd) * sizeof(OpenGL::DrawElementsIndirectCommand)),
        static_cast<GLsizei>(gr.cmd_count), 0);
    _pib_scratch_issued += gr.cmd_count;   // [VK-DIFF] what GL really drew
  }

  gl.bindVertexArray(0);
  gl.bindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
  gl.disable(GL_BLEND);
  gl.depthMask(GL_TRUE);
  gl.enable(GL_CULL_FACE);
}

namespace
{
  // [GPU-driven 2026-08-17] ONE-TIME validation that GL compute shaders actually run + produce correct
  // results on THIS GPU/driver, logged before we build GPU skeleton animation on top of them. Compiles a
  // trivial compute program that writes a known pattern to an SSBO, dispatches it, reads it back, verifies.
  // If this logs FAIL we know not to trust the compute path on this machine (rather than shipping broken
  // animation). Runs exactly once. Result in log.txt: "[compute-validate] PASS" / "FAIL".
  void validate_compute_shader_once()
  {
    static bool ran = false;
    if (ran) { return; }
    ran = true;

    if (!gl.hasComputeShaders())
    {
      LogError << "[compute-validate] SKIP: GL context < 4.3 -- no compute shaders on this context."
               << std::endl;
      return;
    }

    constexpr GLenum COMPUTE_SHADER = 0x91B9;     // GL_COMPUTE_SHADER
    constexpr GLenum SSBO = 0x90D2;               // GL_SHADER_STORAGE_BUFFER
    constexpr GLbitfield SSBO_BARRIER = 0x2000;   // GL_SHADER_STORAGE_BARRIER_BIT
    constexpr GLenum READ_ONLY = 0x88B8;          // GL_READ_ONLY
    constexpr GLenum DYNAMIC_DRAW = 0x88E8;       // GL_DYNAMIC_DRAW
    constexpr int N = 256;

    char const* const src =
      "#version 430 core\n"
      "layout(local_size_x = 64) in;\n"
      "layout(std430, binding = 0) buffer Out { uint result[]; };\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  result[i] = i * 2u + 7u;\n"
      "}\n";

    try
    {
      OpenGL::program prog({ { COMPUTE_SHADER, std::string(src) } });

      GLuint ssbo = 0;
      gl.genBuffers(1, &ssbo);
      gl.bindBuffer(SSBO, ssbo);
      gl.bufferData(SSBO, N * sizeof(std::uint32_t), nullptr, DYNAMIC_DRAW);
      gl.bindBufferRange(SSBO, 0, ssbo, 0, N * sizeof(std::uint32_t));

      {
        OpenGL::Scoped::use_program use(prog);
        gl.dispatchCompute(N / 64, 1, 1);
      }
      gl.memoryBarrier(SSBO_BARRIER);

      gl.bindBuffer(SSBO, ssbo);
      auto const* p = static_cast<std::uint32_t const*>(gl.mapBuffer(SSBO, READ_ONLY));
      bool ok = (p != nullptr);
      if (ok)
      {
        for (int i = 0; i < N; ++i)
        {
          if (p[i] != static_cast<std::uint32_t>(i) * 2u + 7u) { ok = false; break; }
        }
      }
      gl.unmapBuffer(SSBO);
      gl.bindBuffer(SSBO, 0);
      gl.deleteBuffers(1, &ssbo);

      LogError << "[compute-validate] " << (ok ? "PASS" : "FAIL")
               << " -- GPU compute shaders " << (ok ? "work" : "did NOT produce correct results")
               << " on this GPU (GPU skeleton animation " << (ok ? "can proceed)." : "would be unsafe).")
               << std::endl;
    }
    catch (std::exception const& e)
    {
      LogError << "[compute-validate] FAIL: compute program build/dispatch threw: " << e.what() << std::endl;
    }
  }
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
    , bool draw_ground_clutter
)
{

  ZoneScoped;
  noggit::perf::Scoped _prof_world(noggit::perf::Phase::WorldDraw);

  // [GPU-driven 2026-08-17] validate the compute pipeline once (logs PASS/FAIL to log.txt) -- see below.
  validate_compute_shader_once();

  // [localize the ~12ms unprofiled-in-WorldDraw 2026-08-07] time everything from here up to the Terrain
  // block (frustum-cull loop, tile sort, camera-volume walks, lighting/bloom setup, clears). Manual timer
  // (not RAII) because the region declares mvp/frustum/buffers used by the later draw phases -- can't wrap
  // it in a scope. Added into FrameSetup just before the Terrain Scoped fires.
  auto const _setup_t0 = std::chrono::steady_clock::now();

  // GL-API-error safety net (2026-07-23): the per-call glGetError() is dropped by default for perf
  // (context.inl -- it was ~half the dense frame and caught nothing once the instanced path was off). This
  // ONE glGetError/frame drains any GL error the previous frame produced, so a future regression can't
  // silently corrupt (green/black) UNlogged. One round-trip/frame is negligible. Set
  // NOGGIT_GL_ERROR_CHECK_PER_CALL=1 to restore per-call checking and pinpoint the exact bad call.
  gl.check_gl_errors("WorldRender::draw");

  // Anisotropic filtering (Settings -> Anisotropic filtering) applies LIVE. Unlike MSAA (a per-frame
  // framebuffer realloc), AF is a per-texture parameter set at upload, so a change means re-applying
  // it to every loaded array. Polling the setting each frame is cheap (Qt caches it); the re-apply
  // pass only runs on an actual change.
  {
    float const af = QSettings().value("render/anisotropic_filtering", 16.0f).toFloat();
    // [VULKAN parity diagnostic] NOGGIT_PARITY_NO_AF isolates anisotropic-filtering implementation
    // differences between GL and VK by turning AF off on BOTH sides for a harness run.
    float const af_eff = std::getenv("NOGGIT_PARITY_NO_AF") ? 1.0f : af;
    if (af_eff != _last_anisotropy)
    {
      _last_anisotropy = af_eff;
      TextureManager::reapply_anisotropy();
      _liquid_texture_manager.reapply_anisotropy();
    }
  }

  glm::mat4x4 const mvp(projection * model_view);
  math::frustum const frustum (mvp);

  // [PIPELINE step 1] Context the deferred GL replay needs, stashed once per frame. The replay runs
  // on the render thread AFTER this walk returns, so it cannot read these locals.
  _vk_gl_deferred.clear();
  _vk_gl_deferred_pi.clear();
  _vk_def_model_view = model_view;
  _vk_def_mvp = mvp;
  _vk_def_camera_pos = camera_pos;
  _vk_def_display = display;
  _vk_def_boxes = draw_models_with_box;

  // Camera-relative view-projection (rotation only) for the shaders that take a single combined MVP
  // uniform (particles, ribbons): they subtract camera_pos in the shader, same jitter fix as the
  // terrain/WMO/M2 vertex shaders. view_rot = model_view with its translation zeroed.
  glm::mat4x4 view_rot = model_view;
  view_rot[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
  glm::mat4x4 const mvp_rel(projection * view_rot);

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
    updateMVPUniformBlock(model_view, projection, camera_pos);

  gl.disable(GL_DEPTH_TEST);

  // Cache "camera inside a WMO" for this frame -- the WMO shader (camera_inside_wmo uniform) uses it to
  // light exterior-lit / portal-spill faces from the WMO's own interior context instead of the outdoor map
  // light when viewed from inside. Ironforge's building fronts + gryphon tunnels were getting Dun Morogh
  // daylight because no Light.dbc row sits inside a city WMO. Guarded like the sibling camera_is_* calls,
  // which can throw while a tile streams in. Computed BEFORE updateLightingUniformBlock (2026-07-25) so the
  // fog block picks the INTERIOR vs OUTDOOR fog-distance scale for THIS frame, not a frame stale.
  // [fog anchor 2026-08-18, user] The indoor/outdoor state must follow the CHARACTER's placement, NOT the
  // camera: in 3rd person the camera orbits (and can clip through a wall) while the character stands still,
  // and the fog must NOT change just because the camera crossed a boundary. Probe from the game character's
  // position when it is active (3rd person / game view); fall back to the camera in the editor and 1st person
  // (where the camera already sits at the character). Every fog SELECTION below (interior test, sphere blend,
  // global-WMO override) uses this probe; the per-pixel fog DISTANCE still comes from the real camera.
  // [fog transition 2026-08-18] The indoor/outdoor fog follows the CHARACTER, not the camera. RE-diagnosis
  // (FOGFINAL trace, Stormwind): a 3rd-person camera orbiting across a building's WMO boundary FLICKERED the
  // interior test on/off frame-to-frame while the character stood still, and with the exterior/interior fog
  // scales differing (4x vs 1x) that slammed the fog band back and forth = the snap. Probing from the
  // character's position makes the interior test stable (only the character's own crossing moves it). Editor /
  // 1st person: no visible character -> falls back to the camera (which already sits at the character there).
  // Opt out with NOGGIT_NO_FOG_TRANSITION=1 (restores camera-anchored + binary-snap behaviour).
  static bool const s_no_fog_transition = std::getenv("NOGGIT_NO_FOG_TRANSITION") != nullptr;
  _fog_probe_pos = (!s_no_fog_transition && _world->_game_character_visible
                    && _world->_game_character.model_instance.has_value())
                 ? _world->_game_character.model_instance->get_pos()
                 : camera_pos;
  _camera_inside_wmo = false;
  float wmo_interior_depth = 0.0f;
  if (!minimap_render)
  {
    noggit::perf::Scoped _prof_camvol(noggit::perf::Phase::CamVolume); // localize: per-frame WMO-containment walk
    try { _camera_inside_wmo = _world->camera_is_inside_wmo(_fog_probe_pos, &wmo_interior_depth); }
    catch (...) { _camera_inside_wmo = false; wmo_interior_depth = 0.0f; }
  }
  // [fog transition 2026-08-18] The client selects fog by the camera's WMO area but hides the source switch
  // behind a DISTANCE falloff -- so the visible fog eases as you move into the room, it does not snap. RE'd
  // from wow335a.exe (fog area DAT_00cd8794 switches instantly in FUN_00790920, but the fog value is blended
  // by the camera's signed depth into the fog volume; no temporal timer -- the "rate" _DAT_009e8d2c is a
  // sin-approx constant, not a fade). We reproduce that spatially: ramp a 0..1 factor over the first
  // `falloff` yards INSIDE the room and lerp the interior fog in by it, replacing the binary snap. The
  // client uses portal geometry for the depth; we use the horizontal depth into the room's AABB (the
  // available proxy). Falloff yards tunable via NOGGIT_FOG_INTERIOR_FALLOFF.
  static float const s_fog_interior_falloff = []
  {
    char const* const e = std::getenv("NOGGIT_FOG_INTERIOR_FALLOFF");
    float const v = e ? static_cast<float>(std::atof(e)) : 8.0f;
    return v > 0.25f ? v : 8.0f;
  }();
  _camera_wmo_interior_factor = glm::clamp(wmo_interior_depth / s_fog_interior_falloff, 0.0f, 1.0f);

  // 3.3.5a-style shadow quality (client extShadowQuality 0-5, RE doc 35). Level 0 = the client's own
  // "LOWEST" = baked terrain + BLOB unit shadows (the 1.12 look). >=1 renders the dynamic unit shadow
  // map at end-of-frame (sampled NEXT frame via the lighting UBO ShadowMatrix). Read per-frame like
  // the msaa setting so the Graphics dropdown applies live.
  // [VULKAN parity harness] the VK terrain has no dynamic shadow map yet, so an off-screen parity run
  // disables the GL one too -- otherwise the comparison measures a KNOWN missing feature instead of the
  // ported shading. Only ever true inside --vk-parity runs; the user's saved setting is untouched.
  extern bool g_noggit_harness_silent; // MapView.cpp (set by MapView::muteAudioForHarness)
  _shadow_quality = (minimap_render || g_noggit_harness_silent)
                      ? 0 : QSettings().value("graphics/shadow_quality", 3).toInt();
  if (_shadow_quality < 0 || _shadow_quality > 5) { _shadow_quality = 3; }
  // Hardware gate: the shadow sampler lives on texture unit 16, which only exists when the driver
  // exposes >16 fragment texture units. On a 16-unit driver the sampler uniform is rejected and
  // stays 0 -> mixed sampler TYPES on unit 0 -> every terrain/M2/WMO draw INVALID_OPERATION =
  // skipped geometry (black holes). Force the feature off there.
  {
    static GLint s_max_frag_units = [] {
      GLint v = 0;
      gl.getIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &v);
      LogDebug << "GL_MAX_TEXTURE_IMAGE_UNITS = " << v
               << (v < 18 ? " -> dynamic shadows DISABLED (need units 16/17)" : "") << std::endl;
      return v;
    }();
    if (s_max_frag_units < 18) { _shadow_quality = 0; }
  }
  if (_shadow_quality < 1) { _shadow_map_valid = false; } // dropdown to 0 -> stop sampling immediately

  if (!minimap_render)
    updateLightingUniformBlock(draw_fog, camera_pos);
  else
    updateLightingUniformBlockMinimap(minimap_render_settings);

  // Bind the (last frame's) shadow maps for every receiver shader; units 16/17 are reserved.
  if (_shadow_initialized && _shadow_quality >= 1)
  {
    gl.activeTexture(GL_TEXTURE0 + 16);
    gl.bindTexture(GL_TEXTURE_2D, _shadow_tex);
    gl.activeTexture(GL_TEXTURE0 + 17);
    gl.bindTexture(GL_TEXTURE_2D, _shadow_env_tex);
    gl.activeTexture(GL_TEXTURE0);
  }

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
    gl.bindFramebuffer(GL_FRAMEBUFFER, _msaa_samples > 0 ? _msaa_fbo : _bloom_scene_fbo);
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
  // [2026-08-27 underwater horizon stripe, round 57] UNDERWATER the client shows FOG ONLY -- no
  // sky dome, no celestials (the CLEAR_WATER param's bright sky bands are tint sources, not a
  // dome to draw; our dome rendered them as an unfogged bright-blue horizon ring). The dome is
  // skipped below, so the background must BE the fog colour here. Alpha 0.0, NOT 1.0: the
  // framebuffer alpha is the bloom emissive mask (>0.88 re-brightens after fog) and a teal
  // background at alpha 1 would bloom the whole screen.
  if (_camera_underwater)
  {
    glm::vec3 const uw_fog = _skies->color_set[FOG_COLOR];
    gl.clearColor(uw_fog.x, uw_fog.y, uw_fog.z, 0.f);
  }
  else
  {
    gl.clearColor(0.f, 0.f, 0.f, 1.f);
  }
  gl.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // ([VULKAN] the compose hook runs at the TERRAIN pass slot below, not here: the sky dome is drawn
  // after this clear without a depth test and would paint over anything composed this early.)

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

  // Opt-in coarse WDL-horizon occlusion (default off; conservative -- see wdl_horizon_occluded).
  bool const wdl_occ_on = !minimap_render
    && QSettings().value("render/wdl_horizon_occlusion", false).toBool();

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

    // Coarse WDL-horizon occlusion (opt-in): mark a frustum-visible tile occluded when it is fully
    // hidden behind nearer low-res terrain. setOccluded(true) makes the terrain/object/water draws
    // skip it (see isOccluded() gates). Clear any stale frustum-cull override so it takes effect.
    if (wdl_occ_on && tile_in_frustum
        && wdl_horizon_occluded(_world, camera_pos, tile_extents[0], tile_extents[1]))
    {
      tile->renderer()->setOverrideOcclusionCulling(false);
      tile->renderer()->setOccluded(true);
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

    // [A2C leak fix 2026-08-18] The grass/detail-doodad path enables GL_SAMPLE_ALPHA_TO_COVERAGE, and it can
    // persist to the END of a frame (the env shadow-caster pass re-enables it after the last defensive disable
    // at ~5954) -> the NEXT frame's SKY + CLOUDS, drawn HERE (first, before the pre-terrain disable at ~2750),
    // inherit it. With A2C on: the sky dome writes alpha 0 -> 0 coverage -> BLACK sky; the cloud dome's
    // fractional alpha -> an ordered coverage "dots on a grid" dither. Force A2C off before any sky draw.
    gl.disable(GL_SAMPLE_ALPHA_TO_COVERAGE);

    bool hadSky = false;

    // BLACK-SKY FIX (2026-07-22): the WMO skybox (MOSB) may only REPLACE the outdoor sky when the camera
    // is GENUINELY inside an indoor group -- not merely inside the WMO's loose outer AABB. drawSkybox()
    // itself tests only raw is_inside_of(group AABB) with NO indoor/exterior filter, so the OPEN Karazhan/
    // Malchezaar tower (camera high in the exterior shell, still within the huge AABB) drew the dark
    // interior skybox and SUPPRESSED the outdoor sky -> black sky. _camera_inside_wmo already applies the
    // correct test (World::camera_is_inside_wmo: indoor && !exterior_lit && !exterior). Global-WMO maps
    // (dungeons that ARE one WMO) keep trying the skybox unconditionally; placed-WMO terrain maps only
    // when actually inside. Outside an indoor group -> fall through to the outdoor _skies->draw.
    bool const try_wmo_skybox = _world->mapIndex.hasAGlobalWMO() || (draw_wmo && _camera_inside_wmo);
    if (try_wmo_skybox)
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

    // TRACE (NOGGIT_LIGHT_DEBUG): which sky path ran. hadSky=1 -> a WMO skybox drew and the outdoor sky
    // was SUPPRESSED (the black-sky path); hadSky=0 -> outdoor _skies->draw ran (if THAT is black, the
    // zone-light sky color is the culprit, not the skybox). inside_wmo pairs with the FOGFINAL line.
    {
      static bool const s_skp_dbg = std::getenv("NOGGIT_LIGHT_DEBUG") != nullptr;
      static int s_skp_tick = 0;
      if (s_skp_dbg && (++s_skp_tick % 30) == 0)
      {
        LogError << "SKYPATH hadSky=" << (hadSky ? 1 : 0)
                 << " inside_wmo=" << (_camera_inside_wmo ? 1 : 0)
                 << " try_wmo_skybox=" << (try_wmo_skybox ? 1 : 0)
                 << " cam=(" << camera_pos.x << "," << camera_pos.y << "," << camera_pos.z << ")"
                 << std::endl;
      }
    }

    // [round 57] !_camera_underwater: no sky dome, sun/moon disc, or skybox from below the
    // surface -- the whole view is water fog (the clear colour above). The round-56 water
    // emissive cap was correct but the STRIPE was the dome itself: the CLEAR_WATER param's
    // bright sky band drawn unfogged behind the fogged terrain.
    // [VULKAN] the celestial list is filled by the sky pass just below. It MUST be cleared here and
    // not with the other per-frame VK arrays further down -- those clears run AFTER this point in the
    // frame and wiped the list before MapView ever saw it (the same trap the WMO liquid feed hit).
    _vk_celestials.clear();

    if (!hadSky && !_camera_underwater)
    {
      // no cloud deck INTO the water (its texture keeps ticking on the air density -- Sky.cpp)
    _skies->set_cloud_draw_suppressed(_camera_underwater);
    _skies->draw( model_view
          , projection
          , camera_pos
          , m2_shader
          , frustum
          , _cull_distance
          , _world->animtime
          , _outdoor_light_stats
      );

      // [VULKAN phase G] the SKYBOX and STARS are ordinary M2 instances -- hand them to the same VK
      // M2 feed every other doodad uses instead of inventing a second path for them. Skies has no
      // WorldRender pointer, so it records what it drew and we feed it here.
      for (ModelInstance* sky_m2 : { _skies->vkSkyboxInstance(), _skies->vkStarsInstance() })
      {
        if (!sky_m2)
          continue;
        std::vector<glm::mat4x4> const one{ sky_m2->transformMatrix() };
        std::vector<glm::vec4> const one_int{ glm::vec4(0.f) };
        vkFeedClassicBucket(sky_m2->model.get(), one, &one_int);
        static bool s_sky_m2_log = false;
        if (!s_sky_m2_log)
        {
          s_sky_m2_log = true;
          LogError << "[VK] sky M2 fed: " << sky_m2->model->file_key().stringRepr() << std::endl;
        }
      }

      // THE SUN (1.12 sky): a bright disc billboard at the sun direction, drawn into the scene so
      // the existing FFXGlow hazes it (the halo you see in-game). Only outdoors (no global-WMO/
      // interior) and only when the sun is above the horizon. Additive; depth-tested so mountains/
      // terrain occlude it, no depth write so it never occludes the world.
      if (!_world->mapIndex.hasAGlobalWMO())
      {
        // ===== CLIENT-EXACT CELESTIAL PATHS (wow.exe 5875, key tables built in FUN_006d3b80,
        // values .rdata 0x811508..0x811650, evaluator FUN_006cf6c0 = sky_keyframe): the vanilla
        // sun/moon have a FIXED compass azimuth all day (the scene light's bearing -- so the disc
        // and the terrain lighting always agree) and only their POLAR angle is keyframed. The sun
        // NEVER crosses the sky: 10 deg below the horizon overnight, rising from 05:30 to 85 deg
        // elevation at noon (plateau 11:55-12:05), back below the horizon by 21:30. =====
        // day_t: 0 = midnight, 0.5 = noon (time is in half-minutes, 2880/day).
        float const day_t = glm::fract(_world->time / 2880.0f);
        static std::pair<float, float> const sun_phi_keys[] = {
          { 0.229167f, 1.7453293f }, // 05:30  polar 100 deg = 10 deg below the horizon
          { 0.496528f, 0.0872665f }, // 11:55  polar 5 deg = 85 deg up
          { 0.503472f, 0.0872665f }, // 12:05  noon plateau
          { 0.895833f, 1.7453293f }, // 21:30  below the horizon again (constant overnight via wrap)
        };
        static std::pair<float, float> const moon_phi_keys[] = {
          { 0.0f,      0.6108652f }, // 00:00  polar 35 deg = 55 deg up
          { 0.003472f, 0.6108652f },
          { 0.166667f, 1.7453293f }, // 04:00  sets 10 deg below the horizon
          { 0.916667f, 1.7453293f }, // 22:00  starts rising
          { 0.996528f, 0.6108652f }, // 23:55  back at 55 deg
        };
        // sun GLARE grows modestly near dawn/dusk (a low-sun looks larger/warmer). Kept gentle
        // (1.3x, was a too-aggressive 2x that ballooned the disc) and applied to the halo only --
        // the hard bloomed disc stays a fixed angular size like the client's sunCenter.
        static std::pair<float, float> const sun_scale_keys[] = {
          { 0.25f, 1.3f }, { 0.30f, 1.0f }, { 0.83f, 1.0f }, { 0.895833f, 1.3f },
        };
        // Blue Child (moon02): its OWN client path (FUN_006d3b80 key tables, flags 0x10/0x20):
        // SAME elevation keyframes as the White Lady -- they rise and set together -- but its own
        // wow-frame azimuth that DRIFTS through the night, ~90-120 deg around the horizon from
        // the Lady. Its time input is the plain day fraction (offset global 0xce9b68 stays 0).
        static std::pair<float, float> const moon2_theta_keys[] = {
          { 0.0f,      2.3561945f }, // 00:00  pi*0.75      (135 deg)
          { 0.166667f, 2.6179939f }, // 04:00  pi*0.833333  (150 deg)
          { 0.916667f, 2.8797933f }, // 22:00  pi*0.916667  (165 deg)
        };
        // Direction in the render frame: same conversion as the scene light above (dayDir is the
        // light FROM the celestial at wow azimuth theta+180, so the celestial itself sits on the
        // (cos(theta+180), sin(theta+180)) horizontal bearing in render space).
        auto const celestial_dir = [](float phi, float theta_wow) {
          float const a = theta_wow + glm::pi<float>();
          return glm::normalize(glm::vec3(std::cos(a) * std::sin(phi), std::cos(phi), std::sin(a) * std::sin(phi)));
        };
        // Azimuth = quarter_pi (45 deg). This is DELIBERATELY not the same numeric value as the scene
        // light's 225 deg (Sky.cpp thetaValue): the disc's celestial_dir applies a +180 flip
        // (a = theta_wow + pi) and renders in a different frame than the scene-light shader swizzle
        // (to_light = -normalize(dayDir.x, dayDir.z, dayDir.y)), so 45 HERE and 225 THERE both produce
        // the CLIENT-correct result -- the disc sits where the sun is, and terrain is lit from it.
        // (2026-07-26: flipping the disc to 225 to "match" the scene light put the visible sun 180 deg
        // to the wrong side -- user-confirmed. The disc was already correct; only the SCENE LIGHT
        // azimuth was the 180-off bug. Leave the disc at 45.)
        float const moon_phi = sky_keyframe(moon_phi_keys, 5, day_t);
        glm::vec3 const to_sun = celestial_dir(sky_keyframe(sun_phi_keys, 4, day_t), glm::quarter_pi<float>());
        glm::vec3 const to_moon = celestial_dir(moon_phi, glm::quarter_pi<float>());
        glm::vec3 const to_moon2 = celestial_dir(moon_phi, sky_keyframe(moon2_theta_keys, 3, day_t));
        float const sun_scale = sky_keyframe(sun_scale_keys, 4, day_t);
        // DAY/NIGHT: also fade by nightIntensity (the signal that fades the Stars.m2 skybox IN),
        // so the sun is gone at night regardless of the path.
        float const day_factor = glm::clamp(1.0f - _outdoor_light_stats.nightIntensity, 0.0f, 1.0f);
        float const moon_factor = glm::clamp(_outdoor_light_stats.nightIntensity, 0.0f, 1.0f);
        // feed the procedural cloud lighting -- the client lights cloud texels from the SUN while
        // day_t is in (0.229167-0.027778, 0.895833+0.027778], else the moon (FUN_006cfb00 window)
        _skies->set_celestial_dir((day_t > 0.2013889f && day_t <= 0.9236111f) ? to_sun : to_moon);
        glm::vec3 const fwd = -glm::vec3(model_view[0][2], model_view[1][2], model_view[2][2]);

        // Celestial billboards (sun/moon disc + glare) all use the CANON Textures\ BLPs (from the
        // wow.exe strings: sunCenter/sunGlare/moon/moon02/moonGlare) drawn through one textured-quad
        // shader. Lazily create the program + textures.
        if (!_moon_program)
        {
          _moon_program.reset(new OpenGL::program(
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("moon_vs") }
            , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("moon_fs") } }));
        }
        if (!_bloom_vao) { gl.genVertexArrays(1, &_bloom_vao); }
        auto const ctx = _world->getRenderContext();
        if (!_moon_texture)       _moon_texture       = std::make_unique<scoped_blp_texture_reference>("textures/moon.blp", ctx);
        if (!_moon2_texture)      _moon2_texture      = std::make_unique<scoped_blp_texture_reference>("textures/moon02.blp", ctx);
        if (!_moon_glare_texture) _moon_glare_texture = std::make_unique<scoped_blp_texture_reference>("textures/moonglare.blp", ctx);
        if (!_sun_center_texture) _sun_center_texture = std::make_unique<scoped_blp_texture_reference>("textures/suncenter.blp", ctx);
        if (!_sun_glare_texture)  _sun_glare_texture  = std::make_unique<scoped_blp_texture_reference>("textures/sunglare.blp", ctx);

        glm::mat4 const cel_mvp = projection * model_view;
        glm::vec3 const cam_right(model_view[0][0], model_view[1][0], model_view[2][0]);
        glm::vec3 const cam_up   (model_view[0][1], model_view[1][1], model_view[2][1]);
        float const cel_dist = std::min(_view_distance * 0.5f, 900.0f);

        // Draw one textured celestial billboard. `additive` = glow (glare/sun disc); else alpha-blend
        // (moon discs, which carry their shape + craters in the BLP). Depth-tested (mountains occlude).
        auto draw_celestial = [&](scoped_blp_texture_reference& tex, glm::vec3 const& dir,
                                  float half_frac, glm::vec3 const& color, float opacity, bool additive)
        {
          {
            static std::set<std::string> s_cel_tex_seen;
            std::string const key = (tex->file_key().hasFilepath() ? tex->file_key().filepath() : std::string("<no path>"))
                                  + " L" + std::to_string(tex->finishedLoading() ? 1 : 0)
                                  + " F" + std::to_string(tex->loading_failed() ? 1 : 0);
            if (s_cel_tex_seen.insert(key).second)
              LogError << "[VK] celestial tex '" << key << "' loaded=" << (tex->finishedLoading() ? 1 : 0)
                       << " failed=" << (tex->loading_failed() ? 1 : 0)
                       << " opacity=" << opacity << std::endl;
          }
          if (opacity <= 0.002f) return;
          if (!tex->finishedLoading() || tex->loading_failed()) return;
          glm::vec3 const center = camera_pos + dir * cel_dist;

          // [VULKAN] record the billboard for the VK celestial pipeline. Camera-relative, because
          // the VK push block has no room for both the centre and the camera (see celestial.vert).
          {
            VkCelestial rec;
            rec.center_rel = dir * cel_dist;
            rec.half_size = cel_dist * half_frac;
            rec.right = cam_right;
            rec.opacity = opacity;
            rec.up = cam_up;
            rec.color = color;
            rec.additive = additive;
            rec.blp = tex->file_key().hasFilepath() ? tex->file_key().filepath() : std::string();
            _vk_celestials.push_back(rec);
          }
          if (_vk_owns_celestials)
            return;   // Vulkan draws this one; GL keeps everything else in the sky pass
          OpenGL::Scoped::use_program sh{*_moon_program.get()};
          // additive = premultiplied glow added with GL_ONE (opacity is a real, unclamped multiplier);
          // else standard alpha blend for the moon discs.
          gl.blendFunc(additive ? GL_ONE : GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
          gl.activeTexture(GL_TEXTURE0);
          tex->bind();
          sh.uniform("moon_tex", 0);
          sh.uniform("additive", additive ? 1 : 0);
          sh.uniform("tex_index", tex->array_index());
          sh.uniform("model_view_projection", cel_mvp);
          sh.uniform("center", center);
          sh.uniform("camera_pos", camera_pos);
          sh.uniform("cam_right", cam_right);
          sh.uniform("cam_up", cam_up);
          sh.uniform("half_size", cel_dist * half_frac);
          sh.uniform("moon_color", color);
          sh.uniform("opacity", opacity);
          gl.bindVertexArray(_bloom_vao);
          gl.drawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);
        };

        gl.enable(GL_DEPTH_TEST);
        gl.depthFunc(GL_LEQUAL);
        OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const cel_cull;
        OpenGL::Scoped::depth_mask_setter<GL_FALSE> const cel_depth_mask;
        gl.enable(GL_BLEND);

        // ===== SUN: the CANON client textures only -- sunCenter.blp (disc) + sunGlare.blp (glow). No
        // procedural star. Both decoded from the BLPs as filled disc + filled glow (no ray texture
        // exists); the client's few soft rays are the FFXGlow blooming the bright disc. =====
        // -0.05: keep drawing while any part of the disc is still above the horizon; the shader's
        // per-fragment horizon clip cuts everything below it (the sprite sets BEHIND the fog band).
        // 3.3.5a: Northrend (sky-type-2 zones) draws NO sun/moon -- the client hard-gates the celestial
        // draw off (Wow335.exe FUN_004f7020 case 2 zeroes the DAT_00d38ccc gate; the Icecrown apitrace
        // shows zero celestial draws, any time of day). WotLK Northrend (map 571) only; 1.12 keeps its
        // sun/moon. See docs/client_re/29 + memory noggit-335a-sun-moon-gate-sky-type2.
        bool const celestial_off = []{
          auto const* p = Noggit::Project::CurrentProject::get();
          return p && p->projectVersion != Noggit::Project::ProjectVersion::CLASSIC;
        }() && _world->mapIndex._map_id == 571;
        {
          static int s_celgate = 0;
          if ((s_celgate++ % 240) == 0)
            LogError << "[VK] celestial gate: off=" << (celestial_off ? 1 : 0)
                     << " toSun.y=" << to_sun.y << " dayFactor=" << day_factor
                     << " drawSun=" << (_world->_settings->value("render/draw_sun", true).toBool() ? 1 : 0)
                     << " drawMoon=" << (_world->_settings->value("render/draw_moon", true).toBool() ? 1 : 0)
                     << " time=" << _world->time << std::endl;
        }
        if (!celestial_off && to_sun.y > -0.05f && day_factor > 0.02f
            && _world->_settings->value("render/draw_sun", true).toBool())
        {
          float const align = glm::clamp(glm::dot(glm::normalize(fwd), to_sun), 0.0f, 1.0f);
          float const grow  = glm::smoothstep(0.82f, 0.999f, align);
          glm::vec3 const sun_band = _skies->color_set[SUN_COLOR];
          glm::vec3 const sun_col = glm::clamp(glm::mix(sun_band, glm::vec3(1.0f), 0.6f), 0.0f, 1.0f);

          // 1) HALO: the CANON sunGlare.blp glow (a filled radial glow, 256px) drawn around the disc --
          // this is the visible sun halo, same idea as the moon halo. Always visible in daytime, and it
          // grows + brightens as you look straight at the sun.
          // 1) GLARE: sunGlare.blp (the halo + faint rays live in this one texture). Opacity is
          // ELEVATION-driven: brightest with the sun high at noon, dropping as it sinks (the
          // dusk trace measured alpha 0.15..0.23 at ~17 deg elevation, anchoring the low end; no
          // opacity growth with view alignment -- the apparent growth is size + FFXGlow). The
          // sprite doubles in size near dawn/dusk (client sun_scale curve).
          float const elev_f = glm::clamp(to_sun.y / 0.9962f, 0.0f, 1.0f); // 1 at the 85 deg noon peak
          // VIEW-ALIGNMENT growth, verified in the binary + traces: the client's glare quad SIZE
          // is STATIC (init consts 0xce9838-40, only the time-of-day 2x dawn/dusk slot 0xce986c is
          // animated) -- the growth when facing the sun is additive LAYER STACKING (3 draws away
          // -> 25 centred), i.e. BRIGHTNESS. Emulate the stack with an alignment opacity ramp; the
          // size ramp below stands in for the footprint widening that stacking produces.
          float const glare_op = glm::mix(0.22f, 0.45f, elev_f) * glm::mix(0.3f, 1.0f, grow);
          draw_celestial(*_sun_glare_texture, to_sun, glm::mix(0.16f, 0.48f, grow) * sun_scale, sun_col,
                         glare_op * day_factor, true);

          // 2) DISC: sunCenter.blp hot bright core, always on. Additive -> blooms via the FFXGlow.
          // The dawn/dusk 2x curve stays on the GLARE only: doubling the DISC in its last ~2.5 deg
          // of elevation read as the sun ballooning to planet size right before it set.
          draw_celestial(*_sun_center_texture, to_sun, 0.04f, sun_col,
                         1.3f * day_factor, true);
        }

        // ===== MOONS (night): White Lady (moon.blp disc + moonGlare halo) and the smaller Blue Child
        // (moon02.blp, no halo). The white moon's HALO grows in size + opacity with aim, disc fixed. =====
        if (!celestial_off && moon_factor > 0.02f && _world->_settings->value("render/draw_moon", true).toBool())
        {
          float const disc_half = 0.07f;
          if (to_moon.y > -0.05f) // shader horizon clip handles the below-horizon part
          {
            float const m_align = glm::clamp(glm::dot(glm::normalize(fwd), to_moon), 0.0f, 1.0f);
            float const m_grow  = glm::smoothstep(0.82f, 0.999f, m_align);
            glm::vec3 const white_col(0.86f, 0.92f, 1.0f);
            // moonGlare.blp is a RING (bright annulus peaking at ~0.54 of the quad radius, dark centre,
            // decoded from the BLP). To read as a GLOW (brightest at the disc, fading out) rather than a
            // detached ring, size the quad so the ring's PEAK sits at/just inside the disc edge -> only
            // the ring's OUTWARD falloff is visible past the disc. peak = 0.54*glare, so glare ~= disc/0.54.
            draw_celestial(*_moon_glare_texture, to_moon, glm::mix(0.115f, 0.135f, m_grow), white_col, m_grow * moon_factor, true);
            draw_celestial(*_moon_texture,       to_moon, disc_half, white_col, moon_factor, false); // disc
          }
          // Blue Child: rides its OWN client keyframe path (to_moon2 above) -- rises from the
          // horizon with the White Lady at 22:00 but ~90-120 deg around the horizon from her,
          // drifting slowly through the night. Independent; no screen-space relation to the Lady.
          if (to_moon2.y > -0.05f) // shader horizon clip sinks it behind the fog band
          {
            draw_celestial(*_moon2_texture, to_moon2, 0.042f, glm::vec3(0.55f, 0.74f, 1.0f), moon_factor, false);
          }
        }
        gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      }
    }
  }

  // Read the distances LIVE each frame (not just once in the ctor) so the Settings sliders apply
  // immediately without a map reload.
  //
  // Two INDEPENDENT distances:
  //   * view_distance ("View / Render Distance" field)  -> terrain, horizon, sky, AND FOG.
  //   * object_render_distance ("Object render distance" slider) -> objects/WMOs/models only.
  // Fog is deliberately tied to the RENDER distance, not the object distance, so dragging the object
  // slider never moves the fog wall (that coupling was the reported bug).
  _view_distance = _world->_settings->value("view_distance", 900.f).toFloat();
  // Fog does NOT drive render distance (user rule: "fog is fog -- it adds the fog band, nothing
  // else"). The old min(fog_end, view_distance) clamp was tolerable while fog ends were inflated by
  // the /20 divisor, but with authored /36 distances (kara: 361yd) it visibly ate doodads/terrain.
  // The fog band itself still hides geometry past the fog end visually; the shader fog is the sole
  // fog effect.
  _terrain_cull_distance = _view_distance;
  // Editor levers: scale the authored fog start/end distances, default 2.0. SEPARATE outdoor (zone) vs
  // indoor (WMO) multipliers (2026-07-25) -- updateLightingUniformBlock picks the one matching whether the
  // camera is inside a WMO. Applied there + in WMORender's per-group fog (via fogDistanceScale()).
  _fog_distance_scale = _world->_settings->value("fog_distance_scale", 2.0f).toFloat();
  _fog_distance_scale_interior = _world->_settings->value("fog_distance_scale_interior", 2.0f).toFloat();
  // Object render distance: its own slider, defaulting to the view distance (so first run / unset =
  // old behaviour). Clamped to the terrain distance -- objects past the terrain/fog horizon would just
  // float in the void, so there's no point drawing them further than the world itself renders.
  float const object_render_distance =
    _world->_settings->value("object_render_distance", 925.0f).toFloat();
  _cull_distance = std::min(_terrain_cull_distance, object_render_distance);
  _decal_depth_ready = false; // fresh depth snapshot needed this frame (shadows + selection circles)
  _world_depth_ready = false; // and a fresh world-only one, taken between the WMO and M2 passes

  // Draw verylowres heightmap (distant horizon backdrop). Toggleable live via Settings
  // ("render_horizon", default on) so it can be disabled to stop fog rendering distant mesh.
  bool const draw_horizon = _world->_settings->value("render_horizon", false).toBool();
  if (!_world->mapIndex.hasAGlobalWMO() && draw_fog && draw_terrain && draw_horizon)
  {
    ZoneScopedN("World::draw() : Draw horizon");
    _horizon_render->draw (model_view, projection, &_world->mapIndex, _skies->color_set[FOG_COLOR], _terrain_cull_distance, frustum, camera_pos, display);
  }

  gl.enable(GL_DEPTH_TEST);
  gl.depthFunc(GL_LEQUAL); // less z-fighting artifacts this way, I think
  //gl.disable(GL_BLEND);
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  //gl.disable(GL_CULL_FACE);

  _world->_n_rendered_tiles = 0;
  _world->_n_rendered_objects = 0;

  // [localize 2026-08-07] close the FrameSetup window (everything above, minus the separately-timed
  // LightCollect/CamVolume that overlap it). FrameSetup - LightCollect - CamVolume = the pure setup cost
  // (cull loop + tile sort + horizon + GL state/query round-trips) inside the ~12ms unprofiled-in-WorldDraw.
  noggit::perf::FrameProfiler::get().add(noggit::perf::Phase::FrameSetup,
    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _setup_t0).count());

  // [VULKAN phase B] when Vulkan owns the terrain pass, its colour+depth are composed HERE -- in the
  // terrain pass slot (after the sky dome, which draws without a depth test) -- and the GL terrain pass
  // is gated off. GL code untouched, runs as always with Graphics API = OpenGL.
  if (draw_terrain && vk_owns_terrain && pre_scene_compose && !minimap_render && display == display_mode::in_3D)
  {
    pre_scene_compose();
  }
  if (draw_terrain && !vk_owns_terrain)
  {
    if (capture_debug_enabled())
    {
      LogDebug << "WorldRender::draw terrain begin" << std::endl;
    }

    ZoneScopedN("World::draw() : Draw terrain");
    noggit::perf::Scoped _prof_terrain(noggit::perf::Phase::Terrain);
    SecTimer _sec(g_vk_sec_terrain_ms);

    gl.disable(GL_BLEND);
    // ★★★ BLACK-WORLD FIX (2026-08-14): the detail-doodad GRASS path enables GL_SAMPLE_ALPHA_TO_COVERAGE
    // and (being state-tracked on a per-scope M2RenderState) LEAVES IT ON at frame end. Terrain writes
    // out_color.a = 0.0 (its bloom-opt-out mask, terrain_frag ~576) -- with A2C on, alpha 0 -> ZERO sample
    // coverage -> the terrain fragment is fully masked and writes NOTHING = the intermittent "black world".
    // The earlier defensive disables (post-grass ~4470, pre-shadow ~5420) run AFTER terrain in frame order,
    // so they never protect the NEXT frame's terrain, which draws FIRST. Disabling it HERE, before any
    // terrain draw, closes the cross-frame leak for good (grass re-enables it per-draw). No-op without MSAA.
    gl.disable(GL_SAMPLE_ALPHA_TO_COVERAGE);

    {
      OpenGL::Scoped::use_program mcnk_shader{ *_mcnk_program.get() };

      mcnk_shader.uniform("camera", glm::vec3(camera_pos.x, camera_pos.y, camera_pos.z));
      mcnk_shader.uniform("animtime", static_cast<int>(_world->animtime));
      // TERRAIN SPECULAR (client-exact, Westfall trace): sun-band colour (SUN_COLOR = LightIntBand
      // band 9, "sun/specular") drives the FF specular the client adds on terrain. Toggleable
      // (render/terrain_specular, default on -- the client ships with `specular` enabled).
      {
        static QElapsedTimer spec_timer;
        static bool spec_on = true;
        if (!spec_timer.isValid() || spec_timer.elapsed() > 500)
        {
          spec_on = _world->_settings->value("render/terrain_specular", true).toBool();
          spec_timer.restart();
        }
        glm::vec3 const sun_spec = _skies->color_set[SUN_COLOR];
        mcnk_shader.uniform("draw_terrain_specular", spec_on ? 1 : 0);
        mcnk_shader.uniform("sun_spec_color", sun_spec);
      }

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

        // Frustum-cull terrain tiles. The per-tile frustum test was already computed above and stored in
        // objectsFrustumCullTest() (0 = fully outside the frustum; it ORs in getChunkUpdateFlags() so an
        // updating tile is never 0), but the terrain loop never used it -- every loaded tile drew all 256
        // chunks even directly behind the camera. Skipping off-frustum tiles cuts ~half the terrain draws
        // with zero visual change. Minimap render forces the test to 2, so it is unaffected.
        if (!minimap_render && tile->renderer()->objectsFrustumCullTest() == 0)
          continue;

        // DISTANCE CULL (user 2026-08-28: "we are under culling in big cities like stormwind -- in
        // game the terrain for the ground and the mountain in distance cull as well"). The object
        // gather loop below has always had this test; the TERRAIN loop never did, so every loaded
        // tile drew all 256 chunks no matter how far away it was -- distant ground and mountains
        // kept rasterising behind the fog wall that already hides them. The client clips terrain at
        // the farclip plane (CVar "farclip", clamp FUN_00780770), which is what view_distance
        // stands in for here. camDist is to the tile CENTRE, so allow one TILESIZE of slack: a tile
        // whose centre is just past the limit can still have a near edge inside it.
        if (!minimap_render && tile->camDist() > _terrain_cull_distance + TILESIZE)
          continue;

        // [perf 2026-08-06] This tile is in the frustum -> keep it loaded (MapIndex::unloadTiles won't drop it
        // even beyond unload_dist). Mark BEFORE the occlusion cull so an occluded-but-in-frustum tile stays
        // resident (avoids a reload pop when occlusion clears). Covers ocean tiles (no doodads) too.
        tile->rendered_recently.store(true, std::memory_order_relaxed);

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

  // Model -> instance transforms for the particle/ribbon pass. ROOT CAUSE of the IF forge white
  // column ([GLSTATE] probe, 2026-07-03): the shared particle sim was drawn INSTANCED at every
  // visible placement (instances=7 at the forge) -- identical clone clouds stacking additively
  // into a white-out, while ONE cloud's data matches the client's faint look (software A/B
  // rasters: 0.80 vs 0.89 peak). FIX: every placement now runs its OWN independent simulation
  // (per-instance live state keyed by the placement position, same swapInstanceEmitterState
  // scheme creature spawns use) and is drawn with its own transform -- so all four forge pots
  // steam simultaneously like the client, with incoherent (non-clone) overlap that never
  // multiplies into a hot core.
  std::unordered_map<Model*, std::vector<glm::mat4x4>> model_with_particles;

  // Pure-additive light effects (god rays / lighthouse beams) deferred to draw AFTER the water.
  std::vector<Model*> deferred_light_effects;
  // Per-instance WMO-doodad light shafts (billboarded lightray cards -- the Stormwind lighthouse
  // beam) deferred past ALL water like the bucket path: they drew in the doodad pass and the
  // harbor water painted over the beam (user 2026-08-26: "renders underwater instead of over").
  std::vector<ModelInstance*> deferred_pi_light_doodads;

  tsl::robin_map<Model*, std::vector<glm::mat4x4>> models_to_draw;
  // Models used by GAMEOBJECT spawns this frame: gameobjects are fade-mechanic objects (like
  // creatures), so their buckets draw with the per-pixel slice OFF -- the fade in
  // models_to_draw_fades is their boundary, not the slice knife.
  std::set<Model*> go_bucket_models;
  // Gameobject spawns that are MID-FADE this frame: routed through the exact INDIVIDUAL draw path
  // creatures use (continuous alpha, depth prepass, identical blend promotion) so their fade is
  // pixel-identical to a creature's. Full-alpha gameobjects stay batched in models_to_draw.
  std::vector<std::pair<ModelInstance*, float>> go_fading_individual;
  // Parallel per-instance cull-fade alphas (same push order as models_to_draw / creature_instanced).
  // Sites that don't fade (WMO doodads, ground clutter) push 1.0 to keep the vectors aligned.
  tsl::robin_map<Model*, std::vector<float>> models_to_draw_fades;
  // [perf 2026-08-04] Parallel per-instance INTERIOR light (same push order as models_to_draw). Assigned
  // at injection like fades, so the draw loop never recomputes it. CLIENT-TRUE (docs/client_re/31): only
  // WMO-linked doodads (MODD) + gameobjects get the interior sample; terrain/tile doodads (MDDF) and
  // ground clutter are OUTDOOR by construction and push (0,0,0,0) -- the client never runs an interior
  // test on them, which is why it never spikes here (noggit's old per-instance interior_light_at over the
  // whole doodad flood every frame was the 1300ms M2 hitch).
  tsl::robin_map<Model*, std::vector<glm::vec4>> models_to_draw_interior;
  std::map<std::pair<Model*, std::uint32_t>, std::vector<float>> creature_instanced_fades;
  // Instanced creature batches keyed by (model, display_id): each batch is ONE skin, so the representative
  // instance's replaceable creature textures + geoset selection apply to the whole draw. Molten Giant /
  // Destroyer / Golemagg share mountaingiant.m2 but have different display skins, so keying by model alone
  // would paint them all identically. The generic models_to_draw path (doodads) can't carry per-instance
  // skins, which is why instanced creatures rendered invisible before.
  std::map<std::pair<Model*, std::uint32_t>, std::vector<glm::mat4x4>> creature_instanced;
  std::map<std::pair<Model*, std::uint32_t>, ModelInstance const*> creature_instanced_rep;
  struct CreatureSpawnInstanceDraw
  {
    std::uint32_t guid = 0;
    ModelInstance* instance = nullptr;
    World::CreatureSpawnOverlay* spawn = nullptr;
    // The unit's cull-fade alpha this frame: ONE value shared by the body, attachments, blob
    // shadow and bloom-mask re-stamp, so every visual component fades in lockstep.
    float fade = 1.0f;
  };
  std::vector<CreatureSpawnInstanceDraw> creature_spawn_instances_to_draw;
  // WMO doodads needing PER-INSTANCE animation (billboarded glow cards, global-seq flicker): drawn
  // individually so each copy animates with its own transform, like the client's per-doodad CM2Models.
  // Per-frame list of NON-OWNING pointers into _pi_doodad_cache (a persistent member map). The OWNING
  // ModelInstance copy lives in that cache -- its scoped model reference keeps the Model alive, so these
  // pointers can't dangle even if a WMO streams out (the cache is flushed on the WMO-set fingerprint
  // change before it's repopulated). The expensive by-value copy is thus paid ONCE per placement, not
  // once per frame (see _pi_doodad_cache + the add_per_instance helper below).
  std::vector<ModelInstance*> per_instance_wmo_doodads;
  std::vector<WMOInstance*> wmos_to_draw;

  // Interior lighting for objects (creatures/gameobjects/doodads): if a spawn stands inside a WMO indoor
  // group, light it by that room's ambient with the outdoor sun suppressed -- the client's per-object
  // interior lighting (1.12 has no interior shadow maps; the "shadow" is just the missing sun). Cached by
  // a coarse world-position cell (interior is spatial), flushed every ~60 frames so WMOs that stream in
  // late are honoured.
  // Rebuild the (expensive) indoor-volume list and flush the per-position cache only once every 60 frames,
  // so WMOs that stream in late are honoured without paying the gather cost every frame.
  // NOGGIT_NO_INTERIOR_OBJECT_LIGHT=1 disables (objects indoors then get the outdoor sun like before).
  static bool const s_no_interior_object_light = std::getenv("NOGGIT_NO_INTERIOR_OBJECT_LIGHT") != nullptr;
  static bool const s_interior_light_debug = std::getenv("NOGGIT_LIGHT_DEBUG") != nullptr;
  // [perf 2026-08-04] Refresh ONLY when the loaded-WMO set changes (fingerprint delta), NOT on a fixed
  // 60-frame epoch. interior_light_at is now time-independent (zone tint is off), so its result only
  // changes when a WMO streams in/out -- exactly what the fingerprint catches (getGroupExtents forces
  // current extents at collect time, so there is no "volumes gathered a second late" race the epoch used
  // to cover). The periodic epoch was wiping _interior_light_cache every ~1s, forcing EVERY visible
  // doodad to re-run its volume test that frame = the periodic DoodadDraw hitch. Fingerprint-only makes
  // the steady state (no WMO streaming) do zero interior-light recompute.
  std::uint64_t const wmo_fingerprint = _world->loaded_wmo_fingerprint();
  ++_interior_light_epoch;
  if (wmo_fingerprint != _last_wmo_fingerprint)
  {
    // WMO-set change (load/unload/move/rotate/doodadset edit -> fingerprint delta): rebuild the copy cache
    // and the volume list. But do NOT wipe the interior-LIGHT cache: a WMO doodad's interior light depends
    // ONLY on its own (static) WMO, so a DIFFERENT WMO streaming in must not force every already-computed
    // doodad to recompute -- that whole-cache wipe on every stream was the 1300ms GatherMerge spike. Cache
    // entries are keyed by world cell; a doodad that MOVES lands in a new cell -> fresh sample (so edits
    // self-heal), and a newly-streamed WMO's doodads simply miss once and compute. [perf 2026-08-04]
    _pi_doodad_cache.clear();
    // [GL perf 2026-08-08] the pib-group cache holds Model* whose lifetime rides the pi-doodad cache's scoped
    // references -- hard-invalidate it with the cache (the signature alone could alias reused node addresses).
    _pib_groups_cache.clear();
    _pib_groups_valid = false;
    _last_wmo_fingerprint = wmo_fingerprint;
    if (!s_no_interior_object_light)
    {
      _world->collect_interior_volumes(_interior_volumes);
    }
    else
    {
      _interior_volumes.clear(); // empty -> interior_light_at returns outdoor for everything
    }
    _world->collect_fog_volumes(_env_fog_volumes);
  }

  // Cold-cache interior computes allowed this frame (see interior_light_at). Kept modest so a streamed
  // WMO's doodads fill in over a few frames while capping the per-frame cost well under the 10ms target.
  _interior_miss_budget = 48;

  // Hard cap on the (now persistent) interior-light cache so a long session can never blow up RAM. Only
  // STATIC objects populate it, so this bounds at a few MB (~40 B/entry); on overflow, drop it and let the
  // miss budget refill -- rare, and gradual. (Moving objects use cache=false and never grow it.)
  if (_interior_light_cache.size() > 262144u)
  {
    _interior_light_cache.clear();
  }

  // Client unit interior lighting (RE_notes/15): a unit standing in a WMO indoor group is lit from the
  // baked MOCV floor colour under its feet -- NOT the sun, NOT MOHD ambient, and (for classic WMOs with
  // attenEnd=0) not MOLT either. Returns (C.rgb, 1) with C = the sampled floor colour; the shader splits
  // C into ambient (cap 96/255) + diffuse (boost 168/255) along the client's fixed interior direction.
  // (0,0,0,0) = outdoor -> normal sun path.
  // Stable per-placement particle-state key from a doodad's (static) world position -- the same hash
  // the placement particle loop uses, so a copy's state is unique and consistent across frames.
  auto wmo_doodad_placement_key = [](glm::vec3 const& p) -> std::uint64_t
  {
    auto mix = [](std::uint64_t h, std::int64_t v)
    {
      return h ^ (static_cast<std::uint64_t>(v) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
    };
    std::uint64_t key = 0x517CC1B727220A95ull;
    key = mix(key, static_cast<std::int64_t>(std::llround(p.x * 8.0f)));
    key = mix(key, static_cast<std::int64_t>(std::llround(p.y * 8.0f)));
    key = mix(key, static_cast<std::int64_t>(std::llround(p.z * 8.0f)));
    return key;
  };

  // Camera-only zone lighting is the client-faithful DEFAULT. The retail client (1.12 and 3.3.5a)
  // computes ONE outdoor light per frame from the CAMERA position and lights every outdoor doodad
  // with it -- there is no per-doodad zone lookup in the render loop. The earlier per-object ZONE tint
  // (encoded here as a==0, rgb = light_at(object)/light_at(camera)) came from a wow_cap_timbermaw
  // reading that was actually the camera moving THROUGH the zone (which shifts the global light for
  // terrain + doodads together), misread as per-object tinting. Left on, it renders doodads near a
  // zone-light boundary toward the neighbouring zone's colour relative to the camera -- e.g. blue
  // Duskwood-edge trees while the camera sits in green Elwynn, snapping neutral only once the camera
  // enters that light. NOGGIT_ZONE_TINT=1 re-enables the experimental per-object tint for A/B.
  static bool const s_no_zone_tint = std::getenv("NOGGIT_ZONE_TINT") == nullptr;
  glm::vec3 const cam_light_sum = glm::vec3(_lighting_ubo_data.DiffuseColor_FogStart)
                                + glm::vec3(_lighting_ubo_data.AmbientColor_FogEnd);
  int const zone_tint_time = static_cast<int>(_world->time);

  // GAP B (checklist 8.7): NOGGIT_NO_INTERIOR_SPILL=1 forces the interior encoding to a flat 0.5 (no
  // doorway spill) -> byte-identical to the pre-GAP-B behaviour, for A/B. Static local -> the lambda
  // reads it without capturing.
  static bool const s_no_interior_spill = std::getenv("NOGGIT_NO_INTERIOR_SPILL") != nullptr;

  // cache=true for STATIC objects (WMO doodads, gameobjects) -- their cell is stable so the value is
  // stored and reused. cache=false for MOVING objects (creatures/mounts/attachments): they land in a new
  // grid cell every frame, so storing would grow the cache without bound (the RAM blowup, 2026-08-04) --
  // they compute fresh each frame and are NOT budget-gated (they need their light every frame).
  // [perf 2026-08-18] Per-FRAME cache for MOVING objects (cache=false: creatures/mounts/attachments). They
  // cannot use the persistent _interior_light_cache (their cell changes every frame -> unbounded RAM), but the
  // EXPENSIVE part -- WMOGroup::sample_ground_color's O(WMO-triangle) floor scan -- is IDENTICAL for every
  // query landing in the same 1-unit cell key THIS frame: a creature's body + ALL its attachments + any other
  // creature sharing that cell. Deduping them was the whole M2Creatures cost (measured interior_light_at ~20ms
  // for ~25 creatures, dominated by re-scanning the WMO floor per body+attachment). Local -> reset free each
  // frame, bounded by the number of distinct occupied cells.
  std::unordered_map<std::int64_t, glm::vec4> interior_frame_cache;
  auto interior_light_at = [this, &camera_pos, cam_light_sum, zone_tint_time, &interior_frame_cache]
    (glm::vec3 const& pos, bool cache = true) -> glm::vec4
  {
    // [CRE-PROFILE 2026-08-18 TEMP] all interior-light lookups (creatures call it cache=false per draw).
    noggit::perf::Scoped _prof_int(noggit::perf::Phase::BucketInterior);
    std::int64_t const kx = static_cast<std::int64_t>(std::floor(pos.x));
    std::int64_t const ky = static_cast<std::int64_t>(std::floor(pos.y));
    std::int64_t const kz = static_cast<std::int64_t>(std::floor(pos.z));
    std::int64_t const key = (kx * 73856093LL) ^ (ky * 19349663LL) ^ (kz * 83492791LL);
    auto const it = _interior_light_cache.find(key);
    if (it != _interior_light_cache.end())
    {
      return it->second;
    }
    if (!cache)
    {
      auto const fit = interior_frame_cache.find(key);
      if (fit != interior_frame_cache.end())
      {
        return fit->second;
      }
    }
    if (cache)
    {
      // [perf 2026-08-04] STATIC-object cold-cache MISS budget: when a WMO streams in, all its doodads miss
      // at once. The client bakes interior light at load; we spread the cold computes over frames -- once
      // the budget is spent, defer the rest (draw outdoor this frame, retry next) so a streamed WMO fills
      // in over a few frames instead of one 500ms GatherMerge hitch. Moving objects (cache=false) skip this
      // -- they must light every frame. (interior_light_at is only ever called serially -> race-free.)
      if (_interior_miss_budget <= 0)
      {
        return glm::vec4(0.0f); // deferred: not cached, so it retries next frame
      }
      --_interior_miss_budget;
    }
    glm::vec4 light(0.f);
    for (auto const& v : _interior_volumes)
    {
      if (pos.x >= v.min.x && pos.x <= v.max.x && pos.y >= v.min.y && pos.y <= v.max.y
          && pos.z >= v.min.z && pos.z <= v.max.z
          && v.group_index < static_cast<int>(v.wmo->groups.size()))
      {
        glm::vec3 const local = glm::vec3(v.inv_transform * glm::vec4(pos, 1.f));
        glm::vec3 sample;
        float spill = 0.f;
        if (v.wmo->groups[v.group_index].sample_ground_color(local, &sample, &spill))
        {
          // [perf 2026-08-04] The old terrain-separation gate (a get_ground_height RAYCAST per miss) was
          // added for the Moonbrook pulley -- a TERRAIN doodad above the underground Deadmines. Terrain
          // doodads no longer call interior_light_at at all (they're outdoor by construction at injection,
          // client-true), so the only callers left are WMO doodads (genuinely inside their own WMO, where
          // the gate never triggered) + gameobjects/creatures. The raycast was ~0.7ms/miss = the residual
          // GatherMerge stream spike. Removed. (A gameobject directly above an underground WMO is the only
          // regression -- rare; if it matters, gate on the object's own WMO linkage, not a terrain probe.)
          // GAP B (checklist 8.7): encode the doorway spill in a. a in [0.5,1.0]: 0.5 = deep interior
          // (spill 0, > 0.25 -> interior, m2 shader mix is a no-op = byte-identical), ramping to 1.0 at
          // an opening where the m2 shader lerps the room light toward the outdoor day/night light.
          float const enc = s_no_interior_spill ? 0.5f : (0.5f + 0.5f * glm::clamp(spill, 0.f, 1.f));
          light = glm::vec4(sample, enc);
          break;
        }
        // no floor under the point in this group -> try other overlapping volumes; if none hit,
        // the object stays on the outdoor path (matches the client's "not linked to a WMO" fallback)
      }
    }

    if (light.a == 0.f && !s_no_zone_tint)
    {
      glm::vec3 dif, amb;
      _skies->light_at(pos, zone_tint_time, &dif, &amb);
      glm::vec3 const obj_sum = dif + amb;
      glm::vec3 tint(1.0f);
      for (int ch = 0; ch < 3; ++ch)
      {
        tint[ch] = cam_light_sum[ch] > 0.004f ? obj_sum[ch] / cam_light_sum[ch] : 1.0f;
      }
      // quantize so instanced draws group well; near-1 stays the exact legacy path (0,0,0,0)
      tint = glm::clamp(glm::round(tint * 32.0f) / 32.0f, glm::vec3(0.03125f), glm::vec3(4.0f));
      if (tint != glm::vec3(1.0f))
      {
        light = glm::vec4(tint, 0.0f);
      }
    }

    if (cache) // static objects: persistent cache (their cell is stable)
    {
      _interior_light_cache.emplace(key, light);
    }
    else // moving objects: per-frame cache -> body + attachments + cell-neighbours reuse this scan this frame
    {
      interior_frame_cache.emplace(key, light);
    }
    return light;
  };


  // Live diagnosis for "doodads sunlit indoors" reports: prints the interior classification of the
  // CAMERA position (stand next to the mis-lit doodad) + how many indoor volumes exist.
  if (s_interior_light_debug)
  {
    static QElapsedTimer dbg_timer;
    if (!dbg_timer.isValid() || dbg_timer.elapsed() > 2000)
    {
      dbg_timer.restart();
      glm::vec4 const probe = interior_light_at(camera_pos);
      int containing = 0;
      for (auto const& v : _interior_volumes)
      {
        if (camera_pos.x >= v.min.x && camera_pos.x <= v.max.x
         && camera_pos.y >= v.min.y && camera_pos.y <= v.max.y
         && camera_pos.z >= v.min.z && camera_pos.z <= v.max.z) { ++containing; }
      }
      LogError << "[interior-light] volumes=" << _interior_volumes.size()
               << " containing_cam=" << containing
               << " probe=(" << probe.x << "," << probe.y << "," << probe.z << " a=" << probe.a << ")"
               << " cam=(" << camera_pos.x << "," << camera_pos.y << "," << camera_pos.z << ")" << std::endl;
    }
  }

  // Cull-range fade -- CREATURES AND GAMEOBJECTS ONLY (tile doodads/trees do NOT fade: they get
  // the terrain-parity per-pixel slice, see slice_dist). CLIENT-EXACT timer model (note 24,
  // user-confirmed): the range test is BINARY -- inside the spawn render distance the unit plays
  // the 2000 ms cubic fade-IN (t^3) to full and STAYS full; the moment it is outside, it plays the
  // 2000 ms fade-OUT (smoothstep((1-t) * a0)) to zero regardless of further camera movement.
  // Alpha is never tied to distance itself, so stopping mid-flight never leaves a half-faded unit.
  // NOGGIT_NO_DIST_FADE=1 disables fade + slice.
  static bool const s_no_dist_fade = std::getenv("NOGGIT_NO_DIST_FADE") != nullptr;
  float const fade_now_ms = static_cast<float>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count() % 360000000LL);
  // Phases: 0 = gone, 1 = fading in (t^3 / 2000 ms), 2 = fully shown, 3 = fading OUT
  // (client SWModelFadeout, note 24: alpha = smoothstep((1-t) * a0) over 2000 ms, a0 = the alpha the
  // object was last shown at). An object leaving its range NEVER hard-disappears: it keeps drawing
  // through the fade-out and is dropped only at alpha 0. Reversals are continuous (client behaviour).
  auto cull_fade_alpha = [&](SceneObject* o, bool drawable, float dist_alpha) -> float
  {
    if (s_no_dist_fade)
    {
      return drawable ? 1.0f : 0.0f;
    }
    bool const want_visible = drawable && dist_alpha > 0.0f;
    if (!want_visible)
    {
      if (o->_cull_fade_phase == 0)
      {
        return 0.0f;
      }
      if (o->_cull_fade_phase != 3)
      {
        // start the timed fade-out from the last SHOWN alpha (_cull_fade_out_a0, tracked below)
        o->_cull_fade_ms_ref = fade_now_ms;
        o->_cull_fade_phase = 3;
      }
      float const t = (fade_now_ms - o->_cull_fade_ms_ref) / 2000.0f;
      if (t >= 1.0f)
      {
        o->_cull_fade_phase = 0; // gone: next appearance fades in from zero
        return 0.0f;
      }
      float const x = std::clamp((1.0f - t) * o->_cull_fade_out_a0, 0.0f, 1.0f);
      return x * x * (3.0f - 2.0f * x); // client smoothstep (note 24)
    }

    if (o->_cull_fade_phase == 3)
    {
      // back in range mid-fade-out: resume the fade-in from the CURRENT fade-out alpha
      float const t = std::clamp((fade_now_ms - o->_cull_fade_ms_ref) / 2000.0f, 0.0f, 1.0f);
      float const x = std::clamp((1.0f - t) * o->_cull_fade_out_a0, 0.0f, 1.0f);
      float const cur = x * x * (3.0f - 2.0f * x);
      float const t_resume = std::cbrt(std::clamp(cur, 0.0f, 1.0f));
      o->_cull_fade_ms_ref = fade_now_ms - t_resume * 2000.0f;
      o->_cull_fade_phase = t_resume >= 1.0f ? 2 : 1;
    }

    float t_in = 1.0f;
    if (o->_cull_fade_phase == 0)
    {
      o->_cull_fade_phase = 1;
      o->_cull_fade_ms_ref = fade_now_ms;
      t_in = 0.0f;
    }
    else if (o->_cull_fade_phase == 1)
    {
      float const t = (fade_now_ms - o->_cull_fade_ms_ref) / 2000.0f;
      if (t >= 1.0f)
      {
        o->_cull_fade_phase = 2;
      }
      else
      {
        t_in = t * t * t; // client cubic (note 24)
      }
    }
    float const alpha = std::min(t_in, dist_alpha);
    o->_cull_fade_out_a0 = alpha; // a later fade-out starts exactly here -- no pop-up, no pop-out
    return alpha;
  };

  // Tile-M2 range test (BINARY, no fade). Limit = the object render distance (defaults to the
  // view distance), radius-extended so a model is gathered while ANY part of it is inside the
  // boundary; the fragment-shader slice (slice_dist) then clips its pixels at the exact distance,
  // so trees slice in/out of view the same way terrain does at the far plane -- never as a whole.
  // [client cull 2026-08-16] The client's placed-doodad (MDDF/M2) distance cull+fade (FUN_00791cb0 +
  // FUN_007bdb10), byte-exact per-instance port -- see ModelInstance::doodadCullFade /
  // twmoa-335a-client-doodad-cull.md. Returns a fade alpha; 0 drops the instance at the GATHER (a real
  // CPU win, not a mere fragment discard), >0..1 feeds the doodad's opacity fade. Controlled by the
  // Performance-tab toggle "render/gv_doodad_cull" (DEFAULT ON -> applies in ALL views incl. the editor,
  // so it's immediately visible/testable); OFF restores the old see-all _cull_distance envelope. (NB it
  // is NOT gated on game view anymore: _game_character_visible is only true in 3rd-person game view, so
  // gating on it silently disabled the cull in 1st person / the editor.)
  // [2026-08-23 UNIFORM doodad draw distance — user decision, supersedes the Aug-16 default.] The
  // client size-class cull (doodadCullFade, FUN_00791cb0 port) culls SMALL trees at ~200yd while big
  // ones draw to 750 — client-accurate, but the user explicitly wants NO early differentiated cull:
  // every doodad draws to the uniform view distance like terrain. It also only ever applied on the
  // INDIVIDUAL path: with the perf env vars (NOGGIT_PERSISTENT_DOODADS/NOGGIT_DOODAD_MDI, armed by
  // the launcher bat) most doodads ride the MDI/persistent paths which slice at the flat
  // _cull_distance — so the bare release exe (no env) showed early tree culling the bat launch
  // didn't. Uniform on every path = both launch modes render identically. The client cull remains
  // available as an opt-IN (render/gv_doodad_cull, now default OFF).
  bool const use_client_doodad_cull = _world->_settings->value("render/gv_doodad_cull", false).toBool();
  // [2026-08-25 DRESSING CLASS-SPLIT — user: "grass too bright / should look brown not green".]
  // The Un'Goro/mountain "grass" is PLACED M2 vegetation (MDDF ferns/shrubs/clumps), and the client
  // hard-culls it by size class: class 0 (<1yd) at 30yd, class 1 (1-4yd) at 100yd. With the Aug-23
  // uniform draw distance those props render to the horizon here, painting green over vistas the
  // client shows as bare ground. Synthesis of both decisions: SMALL DRESSING (classes 0-1) gets the
  // client cull by default; trees/landmarks (classes 2-4) keep the uniform distance (no early tree
  // popping) unless the full client cull is opted in. (Ground CLUTTER is unrelated here: turtle's
  // re-minted GroundEffectTexture orphans ~9.5M of 9.6M subcell effect ids -> the live client has
  // essentially no clutter, and noggit resolves the same ids to the same nothing -- doc 37.)
  bool const use_dressing_cull = _world->_settings->value("render/gv_dressing_cull", true).toBool();
  auto m2_dist_envelope = [&](ModelInstance* mi) -> float
  {
    if ((use_client_doodad_cull || use_dressing_cull) && display == display_mode::in_3D)
    {
      mi->ensureExtents(); // makes _cull_class current
      if (use_client_doodad_cull || mi->_cull_class <= 1)
      {
        return mi->doodadCullFade(camera_pos, 1.0f); // environmentDetail default 1.0 (@0x009e1340)
      }
    }

    float const radius = mi->model->rad * mi->scale;
    float const dist = (display == display_mode::in_3D
                        ? glm::distance(camera_pos, mi->pos)
                        : std::abs(mi->pos.y - camera_pos.y)) - radius;
    float const limit = _cull_distance;
    // Binary: the per-pixel slice in the shader (slice_dist) handles the visual boundary exactly
    // like terrain's far plane; the model is gathered while ANY of it is inside the boundary.
    return dist <= limit ? 1.0f : 0.0f;
  };

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

  // Build the per-frame creature-spawn index (THROTTLED inside -- see rebuildLegacySuppressIndex) so the
  // legacy-doodad overlap test below is an O(1) lookup instead of an O(instances x spawns) walk.
  rebuildLegacySuppressIndex();

  // [perf 2026-08-05] PERSISTENT tile-doodad path (NOGGIT_PERSISTENT_DOODADS=1). Static tile M2 doodads are
  // drawn from tile-owned GPU buffers (uploaded once) with TILE-level cull + shader slice_dist clip, instead
  // of the per-instance gather + per-frame upload below. Eligible (model, tile) buckets are skipped in the
  // gather and collected here to draw in the instanced-doodad section. Off = unchanged behaviour.
  static bool const s_persistent_doodads = []
  {
    char const* v = std::getenv("NOGGIT_PERSISTENT_DOODADS");
    return v && *v && *v != '0';
  }();
  // [perf 2026-08-05] MDI cross-model doodad batching (needs persistent doodads: it consumes their per-tile
  // instance buffers). Dev A/B toggle; make always-on once validated. See drawDoodadsBatched.
  static bool const s_doodad_mdi = []
  {
    char const* v = std::getenv("NOGGIT_DOODAD_MDI");
    return v && *v && *v != '0';
  }();
  // Pointers into the tiles' own robin_maps (stable for the frame -- no tile is rebuilt/unloaded between the
  // gather here and the draw), so no per-frame copy of the buffer struct (which now carries cpu_transforms).
  std::vector<std::pair<Model*, Noggit::Rendering::TileRender::DoodadInstanceBuffer const*>> persistent_doodad_draws;
  // [doodad distance fade 2026-08-07] parallel to persistent_doodad_draws: per-bucket fade (1=solid ..
  // 0=gone) by TILE distance. Drives extra_alpha in the draw loop -> smooth alpha-blend fade (still lit).
  std::vector<float> persistent_doodad_fades;

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

    // Early dist check. camDist is to the tile CENTER, so gather objects one TILESIZE beyond the
    // terrain distance: objects near a far tile's close edge (and tall trees whose radius extends
    // their fade range) must keep drawing until their own distance envelope reaches zero --
    // otherwise the whole tile's objects pop at full alpha while still visible above the fog.
    if (tile->camDist() > _terrain_cull_distance + TILESIZE)
      continue;

    // Frustum-cull the whole tile's OBJECTS. objectsFrustumCullTest()==0 means the tile's COMBINED extents
    // (terrain + every object's bounds -- conservative) are fully outside the view frustum, so none of its
    // objects can be visible. Previously the loop below still walked EVERY instance of EVERY off-screen tile
    // and per-instance frustum-tested each (the "27895 loaded / 46 rendered" walk). Skipping the tile
    // outright removes that iteration with zero visual change: a tall object straddling the edge keeps the
    // tile in-frustum via the combined extents, and cross-referenced objects still draw via an in-frustum
    // tile. Minimap render forces the test to >=1 so it draws everything.
    if (!minimap_render && tile->renderer()->objectsFrustumCullTest() == 0)
      continue;

    // [perf 2026-08-05] Persistent tile-doodad buckets for THIS (visible) tile. Fetch once (rebuilds lazily
    // if the tile's object set changed), collect them for the draw phase, and skip their models in the
    // per-instance gather below so they aren't drawn twice. NB cross-tile-referenced doodads can appear in
    // two tiles' buffers -> drawn twice, but tile doodads are opaque / alpha-key so the second draw is
    // pixel-identical (no double-blend) -- harmless overdraw on a handful of border objects.
    tsl::robin_map<Model*, Noggit::Rendering::TileRender::DoodadInstanceBuffer> const* tile_pbuf = nullptr;
    if (s_persistent_doodads && !minimap_render && draw_models)
    {
      auto const& b = tile->renderer()->doodadInstanceBuffers();
      if (!b.empty())
      {
        tile_pbuf = &b;
        // [perf 2026-08-06] SIZE-WEIGHTED doodad distance cull (client-like). Cull distance scales with each
        // model's bounding radius: small clutter (rocks/bushes) drops out at a SHORT distance where it's
        // imperceptible, while large landmarks (trees) stay out to the terrain horizon so nothing prominent
        // "pops in bunches". Per (tile,model), so it's free (no extra per-instance work). tile_pbuf stays set
        // regardless, so a culled model is dropped from BOTH draw paths (never redrawn dynamically), cutting
        // the per-(tile,model) draws in DoodadDraw/SubmitInst. NOGGIT_DOODAD_CULL_BASE (frac at radius 0) +
        // NOGGIT_DOODAD_CULL_PER_RAD tune it; BASE>=1 draws everything (old behaviour).
        static float const s_cull_base = []
        {
          char const* v = std::getenv("NOGGIT_DOODAD_CULL_BASE");
          float const f = v ? static_cast<float>(std::atof(v)) : 0.4f;
          return f > 0.0f ? f : 0.4f;
        }();
        static float const s_cull_per_rad = []
        {
          char const* v = std::getenv("NOGGIT_DOODAD_CULL_PER_RAD");
          float const f = v ? static_cast<float>(std::atof(v)) : 0.03f;
          return f >= 0.0f ? f : 0.03f;
        }();
        float const tile_dist = tile->camDist();
        for (auto const& kv : b)
        {
          float fade;
          if (use_client_doodad_cull || (use_dressing_cull && kv.second.cull_class <= 1))
          {
            // [client cull 2026-08-16] CLIENT per-class doodad cull (FUN_00791cb0 tables), byte-exact
            // distances. The persistent bucket is per-(model,tile) and dropped by TILE distance -- a
            // noggit batching construct with no client equivalent -- so we apply the client's exact
            // cull_far/fade_band for the bucket's class (MAX class over its instances) to the bucket's
            // NEAREST possible instance distance (tile centre minus half an ADT diagonal, 377y: the most
            // any instance can sit closer than the tile centre). Conservative: never drops a bucket while
            // any instance could still be within the client's cull_far -> no popping. envDetail = 1.0
            // (client default @0x009e1340). See twmoa-335a-client-doodad-cull.md.
            float cull_far, band;
            ModelInstance::doodadCullParams(kv.second.cull_class, 1.0f, cull_far, band);
            float const near_dist = tile_dist - 377.0f;
            if (near_dist > cull_far)
            {
              continue; // whole bucket past the client cull distance -> drop (draw-call CPU win)
            }
            fade = (near_dist <= (cull_far - band)) ? 1.0f
              : std::clamp((cull_far - near_dist) / band, 0.0f, 1.0f);
          }
          else
          {
            // EDITOR view: noggit's own perf cull (NOT client-exact -- an editor convenience so authors
            // aren't drawing 37k objects). Size-weighted HARD cap (fully gone), pushed ~1.5x so doodads
            // reach further; SOFT cap = 90% of it. The bucket fades by TILE distance over [soft, hard]
            // (fade -> extra_alpha -> smooth alpha blend in the draw loop). Keep it collected until past
            // hard + a tile-edge margin (a tile is culled by its CENTRE, its near instances sit ~half a
            // diagonal closer), so it's already invisible before it drops.
            // [2026-08-23 UNIFORM draw distance — user decision.] The old size-weighted hard cap
            // (0.4 + rad*0.03 of the view distance) faded small-model buckets much earlier than big
            // ones — the same differentiated early cull the user rejected on the individual path.
            // Uniform boundary: every bucket draws to the flat _cull_distance (the shader slice
            // handles the visual edge), dropped only past it plus the tile-edge margin.
            float const hard = _cull_distance;
            float const soft = hard; // no early fade band — binary like the individual path
            if (tile_dist > hard + 380.0f)
            {
              continue; // fully past the boundary and the tile-edge margin -> drop
            }
            fade = (hard <= soft) ? 1.0f
              : std::clamp((hard - tile_dist) / (hard - soft), 0.0f, 1.0f);
          }
          persistent_doodad_draws.emplace_back(kv.first, &kv.second);
          persistent_doodad_fades.push_back(fade);
        }
      }
    }


    // Per-chunk object gather (client MCRF semantics, checklist D1): on a PARTIALLY visible tile,
    // frustum-test the tile's 16x16 object buckets (~33yd cells) and only per-instance-test the
    // survivors -- instead of testing every instance on the tile. Fully-visible tiles keep the
    // straight per-model walk below (bucket tests would only add work there). The frame stamp
    // already dedupes instances that sit in several buckets/tiles. NOGGIT_NO_MCRF_BUCKETS=1 bisect.
    static bool const mcrf_buckets_disabled = []
    {
      char const* v = std::getenv("NOGGIT_NO_MCRF_BUCKETS");
      return v && *v && *v != '0';
    }();

    if (!minimap_render && !mcrf_buckets_disabled && tile->renderer()->objectsFrustumCullTest() == 1)
    {
      for (auto const& bucket : tile->getObjectBuckets())
      {
        if (bucket.instances.empty() || !frustum.intersects(bucket.aabb_max, bucket.aabb_min))
        {
          continue;
        }

        for (auto const& [obj, instance] : bucket.instances)
        {
          if (instance->frame == frame)
          {
            continue;
          }

          if (instance->which() == eMODEL)
          {
            if (!draw_models)
            {
              continue;
            }

            instance->frame = frame;
            auto m2_instance = static_cast<ModelInstance*>(instance);

            // Drawn from the tile's persistent buffer instead (skip the per-frame gather for this model).
            if (tile_pbuf && tile_pbuf->count(reinterpret_cast<Model*>(obj)))
            {
              continue;
            }

            if (should_suppress_legacy_creature_instance(_world, *m2_instance, _legacy_suppress_index))
            {
              continue;
            }

            // [game-view cull 2026-08-15] In game view m2_dist_envelope returns the client's per-class
            // doodad fade alpha (0 = hard-culled, dropped here; <1 = fading over the class band, which
            // the translucent-promote path honours as opacity -- client behaviour, FUN_00791cb0). In
            // the editor it stays the flat see-all envelope (fade == 1) and the shader's per-pixel
            // slice (slice_dist) clips at _cull_distance like the far plane clips terrain.
            float const fade = m2_dist_envelope(m2_instance);
            bool const drawable = m2_instance->model->finishedLoading() && fade > 0.0f;
            if (drawable && m2_instance->isInFrustum(frustum))
            {
              models_to_draw[reinterpret_cast<Model*>(obj)].push_back(m2_instance->transformMatrix());
              models_to_draw_fades[reinterpret_cast<Model*>(obj)].push_back(fade);
              models_to_draw_interior[reinterpret_cast<Model*>(obj)].push_back(glm::vec4(0.0f)); // tile doodad = outdoor (client MDDF)
            }
          }
          else if (instance->which() == eWMO)
          {
            if (!draw_wmo)
            {
              continue;
            }

            instance->frame = frame;
            auto wmo_instance = static_cast<WMOInstance*>(instance);

            if (frustum.intersects(wmo_instance->extents[1], wmo_instance->extents[0]))
            {
              wmos_to_draw.push_back(wmo_instance);
            }
          }
        }
      }

      continue; // this tile is done -- skip the per-model walk below
    }

    // TODO: subject to potential generalization
    for (auto& pair : tile->getObjectInstances())
    {
      if (pair.second[0]->which() == eMODEL)
      {
        if (!draw_models && !(minimap_render && minimap_render_settings->use_filters))
          continue;

        // Whole bucket drawn from the tile's persistent buffer -> skip the per-frame gather for it.
        if (tile_pbuf && tile_pbuf->count(reinterpret_cast<Model*>(pair.first)))
          continue;

        auto& instances = models_to_draw[reinterpret_cast<Model*>(pair.first)];
        auto& fades = models_to_draw_fades[reinterpret_cast<Model*>(pair.first)];
        auto& interiors = models_to_draw_interior[reinterpret_cast<Model*>(pair.first)];

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

          if (!minimap_render && should_suppress_legacy_creature_instance(_world, *m2_instance, _legacy_suppress_index))
          {
            continue;
          }

          // [game-view cull 2026-08-15] game view: m2_dist_envelope returns the client per-class doodad
          // fade (0 = hard-culled here, <1 = client fade band via the translucent promote). Editor /
          // minimap keep the flat boundary (fade == 1) + per-pixel slice.
          float const fade = minimap_render
                             ? (m2_instance->isInRenderDist(_cull_distance, camera_pos, display) ? 1.0f : 0.0f)
                             : m2_dist_envelope(m2_instance);
          bool const drawable = m2_instance->model->finishedLoading() && fade > 0.0f;
          if (drawable
              && (tile->renderer()->objectsFrustumCullTest() > 1 || m2_instance->isInFrustum(frustum)))
          {
            instances.push_back(m2_instance->transformMatrix());
            fades.push_back(fade);
            interiors.push_back(glm::vec4(0.0f)); // tile doodad = outdoor (client MDDF)
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

  // WMO-model GAMEOBJECTS (Turtle player housing etc.): their WMOInstances join the same draw list as
  // world WMOs -- walls, doodad sets, fog and liquids all render through the standard WMO pipeline.
  if (!minimap_render && (_world->drawCreatureSpawns() || _world->drawGameObjectSpawns()))
  {
    _world->ensureGameObjectSpawnsLoaded();
    static int s_gowmo_logs = 0;
    int gowmo_total = 0, gowmo_queued = 0, gowmo_failed = 0, gowmo_loading = 0;
    for (auto& spawn : _world->gameObjectSpawns())
    {
      if (spawn.pending_delete || spawn.event_suppressed || !spawn.model_path.ends_with(".wmo"))
      {
        continue;
      }
      ++gowmo_total;
      if (!_world->ensureGameObjectSpawnModel(spawn) || !spawn.wmo_instance.has_value())
      {
        ++gowmo_failed;
        continue;
      }
      WMOInstance* wi = &*spawn.wmo_instance;
      if (!wi->finishedLoading())
      {
        // async load in flight: extents are not valid yet -- queue it anyway (the WMO draw path
        // guards unloaded instances) so it appears the moment loading completes.
        wmos_to_draw.push_back(wi);
        ++gowmo_loading;
        continue;
      }
      wi->ensureExtents();
      if (frustum.intersects(wi->extents[1], wi->extents[0]))
      {
        wmos_to_draw.push_back(wi);
        ++gowmo_queued;
      }
    }
    if (gowmo_total > 0 && s_gowmo_logs < 8)
    {
      ++s_gowmo_logs;
      LogError << "GOWMO gather: total=" << gowmo_total << " queued=" << gowmo_queued
               << " loading=" << gowmo_loading << " failed=" << gowmo_failed << std::endl;
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
    _deferred_wmo_liquid.clear(); // refilled by WMORender, flushed in the water phase below
    _vk_wmo_liquids.clear();      // [VULKAN] same lifetime, but survives the flush below
    // [VULKAN] the particle pass runs LATER this frame, so clearing here is correct (its
    // producers all run after this point -- unlike the celestial list, which is filled earlier).
    Noggit::Rendering::VK::particleFeed().clear();

    noggit::perf::Scoped _prof_wmo(noggit::perf::Phase::WMO);
    SecTimer _sec(g_vk_sec_wmo_ms);
    {
      OpenGL::Scoped::use_program wmo_program{*_wmo_program.get()};

      wmo_program.uniform("camera", glm::vec3(camera_pos.x, camera_pos.y, camera_pos.z));
      static int const s_wmo_debug_mocv = []{ char const* v = std::getenv("NOGGIT_WMO_SHOW_MOCV");
                                              return v ? std::atoi(v) : 0; }();
      wmo_program.uniform("debug_mocv", s_wmo_debug_mocv);

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


      // [VULKAN phase D] one WMO feed per frame -- reset before the loop that fills it, or the
      // per-frame draw/transform arrays grow without bound.
      vkResetWmoFrame();
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
              , this // per-room (MOLR) point-light scoping for interior groups
          );
          // [VULKAN phase D] feed the SAME instance to Vulkan. AFTER the GL draw on purpose:
          // draw() is what writes portal_group_visibility, and the feed must use THIS frame's.
          // GL's rendering above is untouched -- this only appends to the arrays MapView hands to VK.
          vkFeedWmoInstance(instance);
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

  // [game-view cull 2026-08-16] CLIENT unit cull (wow335a.exe FUN_00791cb0), byte-exact. RE proved a
  // CGUnit's M2 is an ordinary CM2 scene node in the SAME grid as static doodads and goes through the
  // IDENTICAL size-class distance cull as MDDF doodads -- there is NO unitDrawDistance CVar; a unit's
  // draw distance is its SIZE CLASS's cull distance ({30,100,200,750,1250}, humanoid extent ~2-3y ->
  // class 1 -> ~100y). We cull off the unit's LIVE position (client node +0x38, updated as it moves),
  // NOT the possibly-stale AABB centre. Controlled by the Performance-tab toggle "render/gv_creature_cull"
  // (DEFAULT ON -> applies in ALL views so it's immediately visible/testable); OFF restores the flat
  // creature_spawn_model_distance (500y). See twmoa-335a-client-doodad-cull.md.
  bool const use_client_creature_cull = _world->_settings->value("render/gv_creature_cull", true).toBool();
  auto unit_dist_alpha = [&](ModelInstance& u) -> float
  {
    u.ensureExtents(); // guarantees _cull_class is computed (bbox x scale x rotation -> class)
    float cull_far, band;
    ModelInstance::doodadCullParams(u._cull_class, 1.0f, cull_far, band); // envDetail 1.0 (@0x009e1340)
    float const d = glm::distance(camera_pos, u.pos);
    if (d > cull_far) return 0.0f;                 // FUN_00791cb0: dist > cull_far[class] -> cull
    if (d <= cull_far - band) return 1.0f;
    float const a = 1.0f - (d - (cull_far - band)) / band;
    return a < 0.01f ? 0.0f : a;                   // min-alpha cull floor (_DAT_009f1968)
  };
  // Capture the world-only depth HERE, between the WMO pass and the M2 pass: terrain and buildings
  // are in the depth buffer, nothing model-shaped is yet. The ground decals drawn later (selection
  // circles, patrol routes) need this to tell "the ground at this pixel" apart from "whatever model
  // is standing in front of it".
  if (draw_creature_spawns || draw_gameobject_spawns
      || terrainMode == editing_mode::creature || terrainMode == editing_mode::gameobject)
  {
    snapshotWorldDepth();
  }

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
    noggit::perf::Scoped _prof_m2(noggit::perf::Phase::M2);
    SecTimer _sec(g_vk_sec_m2_ms);

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
      noggit::perf::Scoped _prof_gather(noggit::perf::Phase::M2Gather); // M2 spike hunt: whole WMO-doodad gather
    SecTimer _sec2(g_vk_sec_gather_ms);

      // Parallelized WMO-doodad gather (was ~35ms single-threaded per frame in Ironforge). Behaviour is
      // IDENTICAL to the old serial loop -- same instances routed the same way, same order after the merge.
      //
      // The split is per DOODAD, not per WMO: a single huge interior (e.g. Ironforge) is ONE WMOInstance
      // with thousands of doodads, so per-WMO tasking would hand it a single task = zero parallelism --
      // exactly the case we most need to accelerate. Phase A flattens every surviving doodad (in WMO order)
      // into one vector that Phase B slices into equal contiguous ranges.
      //
      // CRITICAL PERF INVARIANT: the parallel phase must be LOCK-FREE and ALLOC-LIGHT. Copying a
      // ModelInstance (the per-instance `push_back(*doodad)`) bumps the ModelManager + TextureManager
      // refcounts through AsyncObjectMultimap's shared std::mutex; doing that on N worker threads over
      // Ironforge's thousands of animated braziers formed a lock convoy on that one mutex + hammered the
      // heap allocator -> 150-275ms (far WORSE than serial). So the workers only COLLECT raw doodad
      // pointers (per-instance) and Model*+matrix (instanced) into their own buffers -- zero refcount, zero
      // mutex, zero shared state. The mutex-guarded ModelInstance copy is DEFERRED to the serial merge,
      // where it runs single-threaded with no contention (byte-identical to the original serial code).

      // NOGGIT_SERIAL_GATHER=1 -> run the ORIGINAL fully-serial gather at this exact spot (A/B baseline,
      // for measuring serial vs parallel). NOGGIT_NO_PIDOODAD=1 -> disable per-instance routing entirely
      // (doodads all instanced, glow static -- the stable fallback).
      static bool const s_serial_gather = std::getenv("NOGGIT_SERIAL_GATHER") != nullptr;
      static bool const s_no_pidoodad = std::getenv("NOGGIT_NO_PIDOODAD") != nullptr;

      // Per-doodad classify, shared by EVERY path (serial baseline, serial gate, parallel) so they can
      // never diverge. Thread-safety when invoked from a worker:
      //   * ensureExtents()/recalcExtents(): writes ONLY this doodad's own members (extents, size_cat,
      //     _need_recalc_extents). Each doodad appears exactly once in `flat` and each flat index belongs
      //     to exactly one worker range, so no two threads ever touch the same doodad. recalcExtents only
      //     READS the shared Model (header box + load flags), never writes it, and for a
      //     wmo_doodad_instance its updateTransformMatrix() override is a no-op -> _transform_mat untouched.
      //   * finishedLoading() is an atomic load; loading_failed() / _per_instance_animation /
      //     transformMatrix() / model.get() are reads of Model state fully published before the model's
      //     `finished` atomic is set (every such read is gated behind finishedLoading()). Same guarantee
      //     the old serial code relied on.
      //   * the sinks (add_instanced / add_per_instance) write ONLY the caller's own buffers. The PARALLEL
      //     sinks push a raw pointer / Model*+matrix -- NO ModelInstance copy, NO refcount, NO mutex.
      auto classify_doodad = [&](wmo_doodad_instance* doodad, auto&& add_instanced, auto&& add_per_instance)
      {
        doodad->ensureExtents();
        if (!doodad->model->finishedLoading() || doodad->model->loading_failed())
        {
          return;
        }

        if (!s_no_pidoodad && doodad->model->_per_instance_animation)
        {
          // NOT registered in model_with_particles: their particles use the creature-style per-copy state
          // swap (mesh pass + own particle loop) -- feeding them to the placement loop too would
          // double-advance the same state (the previous AV/freeze).
          add_per_instance(doodad);
        }
        else
        {
          add_instanced(doodad->model.get(), doodad->transformMatrix());
        }
      };

      // Direct-to-container sink for the instanced side (main thread, no contention). Shared by every
      // serial path (baseline, <256 gate) via classify_doodad.
      auto add_instanced_direct = [&](Model* m, glm::mat4x4 const& mat)
      {
        models_to_draw[m].push_back(mat);
        models_to_draw_fades[m].push_back(1.0f); // WMO doodads cull with their group
        models_to_draw_interior[m].push_back(interior_light_at(glm::vec3(mat[3]))); // WMO doodad = interior (client MODD)
      };
      // Per-instance sink shared by ALL THREE gather paths (parallel Phase-C merge, the <256 serial gate,
      // and the NOGGIT_SERIAL_GATHER baseline). It resolves the doodad to its persistent cache entry,
      // copying the ModelInstance ONCE per placement (on first sighting) -- that one-time copy is the only
      // place the mutex-guarded ModelManager/TextureManager refcount bump happens now, and it runs on the
      // main thread (serial), so no lock convoy. Every subsequent frame is a pure map lookup + push of a
      // stable pointer into the cache. Called only serially (never from the parallel Phase B workers).
      auto add_per_instance = [&](wmo_doodad_instance* d)
      {
        std::uint64_t const key = wmo_doodad_placement_key(d->get_pos());
        auto it = _pi_doodad_cache.find(key);
        if (it == _pi_doodad_cache.end())
        {
          it = _pi_doodad_cache.emplace(key, *d).first; // ONE-TIME by-value copy (refcount bump) here
        }
        per_instance_wmo_doodads.push_back(&it->second); // non-owning pointer into the stable cache node
      };

      if (s_serial_gather)
      {
        // A/B BASELINE: the original fully-serial gather -- get_visible_doodads + inline classify straight
        // into the real containers, no flat vector, no threads. get_visible_doodads + suppress must be
        // serial (see below); here the whole gather is serial.
        for (auto* wmo_instance : wmos_to_draw)
        {
          auto doodads = wmo_instance->get_visible_doodads(frustum, _cull_distance, camera_pos, draw_hidden_models, display);
          for (auto* doodad : doodads)
          {
            if (!doodad)
            {
              continue;
            }

            if (!minimap_render && should_suppress_legacy_creature_instance(_world, *doodad, _legacy_suppress_index))
            {
              continue;
            }

            classify_doodad(doodad, add_instanced_direct, add_per_instance);
          }
        }
      }
      else
      {
        // PHASE A (SERIAL, this thread): call get_visible_doodads for every WMO and pre-filter the results
        // into a single flat, WMO-ordered vector. get_visible_doodads MUST stay serial -- it lazily mutates
        // WMO state (change_doodadset when _need_doodadset_update, plus per-doodad update_transform_matrix_wmo
        // when need_matrix_update). should_suppress_legacy_creature_instance ALSO must stay serial: it mutates
        // two function-local static std::set<> (logged_suppressions / logged_legacy_candidates) via insert()
        // for debug-log dedup, so calling it concurrently would race on those sets. We drop null + suppressed
        // doodads HERE (identical to the old null/suppress `continue`s), leaving Phase B nothing unsafe.
        std::vector<wmo_doodad_instance*> flat;
        {
          // GatherCull = Phase A wall-time (get_visible_doodads cull + null/legacy-suppress filter + flat
          // build). Explicit braces so this Scoped covers ONLY Phase A -- it must NOT bleed into the
          // parallel Phase B or the Phase-C merge. Timed for the parallel path AND the <256 serial gate
          // (both build `flat`); in the s_serial_gather baseline above, cull is fused with classify in one
          // pass so GatherCull reads 0 there (that mode is the baseline; we profile the parallel split).
          noggit::perf::Scoped _prof_cull(noggit::perf::Phase::GatherCull);
          for (auto* wmo_instance : wmos_to_draw)
          {
            auto doodads = wmo_instance->get_visible_doodads(frustum, _cull_distance, camera_pos, draw_hidden_models, display);
            flat.reserve(flat.size() + doodads.size());
            for (auto* doodad : doodads)
            {
              if (!doodad)
              {
                continue;
              }

              if (!minimap_render && should_suppress_legacy_creature_instance(_world, *doodad, _legacy_suppress_index))
              {
                continue;
              }

              flat.push_back(doodad);
            }
          }
        }

        std::size_t const flat_count = flat.size();
        constexpr std::size_t parallel_threshold = 256; // below this, threads cost more than they save
        constexpr std::size_t doodads_per_worker = 128; // >= this many doodads per worker so we never over-thread

        if (flat_count < parallel_threshold)
        {
          // SERIAL GATE: on light frames a handful of doodads isn't worth the thread-spawn overhead (it
          // would regress them). Classify inline, straight into the real containers, skip the threads.
          for (auto* doodad : flat)
          {
            classify_doodad(doodad, add_instanced_direct, add_per_instance);
          }
        }
        else
        {
          // PHASE B (PARALLEL, LOCK-FREE): slice [0, flat_count) into N contiguous ascending ranges of
          // ~doodads_per_worker each. Worker k fills ONLY results[k] with raw pointers / Model*+matrix --
          // no ModelInstance copy, no refcount, no mutex, no shared mutable state. STATIC contiguous ranges
          // (not a work-stealing counter) guarantee chunk k precedes chunk k+1 in the merge -> deterministic
          // serial-identical order.
          struct GatherLocal
          {
            std::vector<std::pair<Model*, glm::mat4x4>> instanced;
            std::vector<wmo_doodad_instance*> per_instance_ptrs; // POINTERS -- the copy is deferred to merge
          };

          unsigned int hw = std::thread::hardware_concurrency();
          if (hw == 0)
          {
            hw = 4;
          }
          std::size_t const N = std::min<std::size_t>(hw, std::max<std::size_t>(1, flat_count / doodads_per_worker));

          std::vector<GatherLocal> results(N);
          std::size_t const base = flat_count / N;
          std::size_t const rem = flat_count % N;

          auto run_range = [&](std::size_t k)
          {
            std::size_t const lo = k * base + std::min<std::size_t>(k, rem);
            std::size_t const hi = lo + base + (k < rem ? 1 : 0);
            GatherLocal& out = results[k];
            std::size_t const range_size = hi - lo;
            // Reserve the (upper-bound) range size up front so the fill loop never reallocs -> cuts
            // allocator churn on the worker threads. A doodad goes to exactly one of the two, so this
            // slightly over-reserves; the waste is trivial and bounded.
            out.instanced.reserve(range_size);
            out.per_instance_ptrs.reserve(range_size);
            // Phase B stays LOCK-FREE: collect raw pointers / Model*+matrix into this worker's own
            // thread-local buffers only. The mutex-guarded ModelInstance copy is deferred to the serial
            // Phase-C merge (via the block-scope add_per_instance cache helper).
            auto collect_instanced = [&](Model* m, glm::mat4x4 const& mat) { out.instanced.emplace_back(m, mat); };
            auto collect_per_instance = [&](wmo_doodad_instance* d) { out.per_instance_ptrs.push_back(d); };
            for (std::size_t i = lo; i < hi; ++i)
            {
              classify_doodad(flat[i], collect_instanced, collect_per_instance);
            }
          };

          // [threading 2026-08-18] persistent pool instead of per-frame std::thread spawns; each of the N
          // static ranges is one work item (run_range(k) fills results[k], lock-free). Serial fallback when
          // the pool is disabled (NOGGIT_NO_THREAD_POOL).
          if (auto* tp = noggit::render_pool())
          {
            tp->parallel_for(N, [&](std::size_t k) { run_range(k); });
          }
          else
          {
            for (std::size_t k = 0; k < N; ++k) { run_range(k); }
          }

          // PHASE C (SERIAL MERGE, this thread): append each chunk's results in ascending index order.
          // `flat` is WMO-ordered and the chunks are contiguous ascending ranges, so this reproduces the
          // exact push order of the old serial loop. instanced -> models_to_draw and per_instance ->
          // per_instance_wmo_doodads are disjoint containers, so emitting all of a chunk's instanced
          // entries before its per-instance ones does not perturb either container's internal order.
          // The mutex-guarded ModelInstance copy (`*p`) happens HERE, serially on the main thread with
          // zero contention -> byte-for-byte identical to the original serial version.
          {
            // GatherMerge = Phase C wall-time: the serial merge (instanced -> models_to_draw + the
            // mutex-guarded per_instance_wmo_doodads copies). Explicit braces so this Scoped covers ONLY
            // the merge -- never the parallel Phase B above it. (In the s_serial_gather baseline and the
            // <256 gate the copy happens inline during their single pass, so GatherMerge reads 0 there.)
            noggit::perf::Scoped _prof_merge(noggit::perf::Phase::GatherMerge);
            for (auto& out : results)
            {
              for (auto& entry : out.instanced)
              {
                models_to_draw[entry.first].push_back(entry.second);
                models_to_draw_fades[entry.first].push_back(1.0f); // WMO doodads cull with their group
                models_to_draw_interior[entry.first].push_back(interior_light_at(glm::vec3(entry.second[3]))); // WMO doodad = interior
              }
              for (auto* p : out.per_instance_ptrs)
              {
                add_per_instance(p); // cache lookup (copy only on first sighting) -- serial, no convoy
              }
            }
          }
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
      noggit::perf::Scoped _prof_cinject(noggit::perf::Phase::CreatureInject); // M2 spike hunt: per-frame spawn iterate
      std::size_t models_created_this_frame = 0;
      std::size_t const model_create_budget = creature_spawn_model_create_budget();
      // Live-tunable near/far split for hybrid creature instancing (see below). Set very high to disable
      // (all creatures individual = original), or low (e.g. 0) to batch every simple creature.
      // Near creatures stay on the individual path (own animation pose, tint, shadow) so close-up visuals
      // are unchanged; only creatures past this range fall back to synced instanced rendering, where the
      // shared pose is imperceptible. Live-tunable: set very high to disable, low to batch more aggressively.
      // Hybrid instancing is DISABLED by default (1e9). Confirmed: the instanced creature draw path
      // renders NOTHING for these creature models (e.g. mountaingiant.m2) even when the model is entirely
      // far with no individual copy -- it's a model-specific instanced-render bug, not the dual-path case.
      // So a giant blinked out the instant it crossed this distance into the instanced path. Keeping every
      // creature on the (working) individual path renders them all to the full creature draw distance.
      // The dual-path exclusion below is still correct and stays ready for when the instanced-render bug
      // is fixed; set this setting low to re-enable batching then.
      // DEFAULT 1e9 = instancing OFF (as the comment above documents). It was WRONGLY 80.0f, so
      // instancing kicked in at 80 yd and crossing that boundary switched a creature between the
      // INDIVIDUAL path (per-spawn animation timeline + guid-keyed idle schedule) and the INSTANCED
      // path (shared timeline, stale idle key) -- desyncing its idle schedule so the animation RESET
      // every time the camera crossed 80 yd in or out (user-reported bird/dragon wing-flap reset at
      // a fixed distance). Instancing stays off until the instanced-render path is repaired.
      // NOGGIT_CREATURE_INSTANCE_LOD=<yards> overrides the QSettings default for A/B testing the hybrid
      // instancing LOD (batch distant simple creatures -> 1 draw + no per-instance bone compute). Default
      // 1e9 = off. [2026-07-24 perf hunt: AnimateCPU+M2Creatures dominate the M2 phase once the inject cost
      // was pre-culled away; batching far simple creatures is the next lever.]
      float const creature_instance_lod_dist = [] {
        if (char const* e = std::getenv("NOGGIT_CREATURE_INSTANCE_LOD"))
          return static_cast<float>(std::atof(e));
        return QSettings().value("render/creature_instance_lod_distance", 1.0e9f).toFloat();
      }();
      // Models that have at least one INDIVIDUAL copy this frame (near, or complex at any distance). Any
      // far/simple copy of such a model must also go individual, never instanced.
      std::set<Model*> models_drawn_individually;
      _creature_fade_by_guid.clear();
      // Simple + far copies whose instanced-vs-individual routing is deferred until the set above is complete.
      std::vector<World::CreatureSpawnOverlay*> far_instance_candidates;
      for (auto& spawn : _world->creatureSpawns())
      {
        if (spawn.pending_delete) // marked for deletion -> don't draw
        {
          continue;
        }
        if (spawn.event_suppressed) // hidden by the Seasonal Events filter
        {
          continue;
        }
        float const creature_distance = glm::distance(camera_pos, spawn.pos);
        trace_creature_spawn("candidate", spawn, creature_distance);

        bool const creature_in_range = creature_distance <= creature_spawn_model_distance;
        // Out of range: still process spawns whose cull fade is mid-flight (they keep drawing while
        // fading out, client behaviour); everything else skips as before.
        if (!creature_in_range
            && (!spawn.model_instance.has_value() || spawn.model_instance->_cull_fade_phase == 0))
        {
          // [mem 2026-08-26] EVICT far spawns' models: creation was lazy but release never
          // happened, so every spawn ever approached kept its model+attachment instances (and
          // their shared model/texture refs) for the whole session. 100 yd of hysteresis past
          // the draw distance so the boundary never thrashes create/evict.
          if (spawn.model_instance.has_value()
              && creature_distance > creature_spawn_model_distance + 100.0f)
          {
            _world->releaseCreatureSpawnModel(spawn);
          }
          trace_creature_spawn("skip-distance", spawn, creature_distance, "draw distance");
          continue;
        }

        // [PERF 2026-07-24] Early frustum cull on the spawn POSITION (+ generous margin) BEFORE the expensive
        // per-spawn work (model create, ensureExtents, cull_fade). In a packed city most in-range spawns are
        // off-screen (behind/beside the camera) -- this loop was fully processing every one of them (the
        // untimed ~3-34ms "M2" cost) even though only the few IN-frustum ones draw (M2Creatures=0 while the
        // inject burned 30ms). The precise isInFrustum test further down still runs for survivors; the 64yd
        // box margin means no creature whose model straddles the frustum edge is ever dropped here.
        // NOGGIT_NO_CREATURE_PRECULL=1 disables. (An off-screen creature's cull-fade isn't advanced while
        // skipped, so it may fade-pop on first appearance -- imperceptible next to a static 30ms/frame cost.)
        {
          static bool const s_no_creature_precull = std::getenv("NOGGIT_NO_CREATURE_PRECULL") != nullptr;
          if (!s_no_creature_precull)
          {
            float const m = 64.0f;
            glm::vec3 const lo(spawn.pos.x - m, spawn.pos.y - m, spawn.pos.z - m);
            glm::vec3 const hi(spawn.pos.x + m, spawn.pos.y + m, spawn.pos.z + m);
            if (!frustum.intersects(hi, lo))
            {
              continue;
            }
          }
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

        // BINARY range test (client model, user-confirmed): inside the knob -> play the 2 s fade-in
        // to FULL and stay full; outside -> play the 2 s fade-out to zero. No distance-proportional
        // alpha -- stopping mid-flight never leaves a half-faded creature. [game-view cull 2026-08-16]
        // In game view the range test is the CLIENT size-class unit cull (unit_dist_alpha, FUN_00791cb0)
        // instead of the flat editor distance; the 2 s fade still rides on top.
        float const creature_dist_alpha = use_client_creature_cull
          ? unit_dist_alpha(mi)
          : (creature_in_range ? 1.0f : 0.0f);
        float const creature_fade = cull_fade_alpha(&mi, true, creature_dist_alpha);
        _creature_fade_by_guid[spawn.guid] = creature_fade; // for the selection circle fade
        if (creature_fade <= 0.0f)
        {
          trace_creature_spawn("skip-faded", spawn, creature_distance, "fade complete", &mi);
          continue;
        }

        // UNIFORM creature render distance: every creature draws out to the full creature/draw_distance
        // knob (the create-gate distance above) regardless of model size. No size-proportional culling --
        // a big giant and a small critter both render to the same knob distance (user preference; the
        // client's size-scaled SmallCull made noggit's small model->rad cull giants too soon).
        if (!mi.isInFrustum(frustum))
        {
          static bool const s_anim_skip_dbg = std::getenv("NOGGIT_ANIM_SKIP_DEBUG") != nullptr;
          if (s_anim_skip_dbg && mi._cull_fade_phase != 0)
          {
            LogError << "ANIMSKIP guid=" << spawn.guid << " '" << mi.model->file_key().stringRepr()
                     << "' DROPPED (out of frustum) while fadePhase=" << (int)mi._cull_fade_phase
                     << " fade=" << creature_fade << " dist=" << creature_distance << std::endl;
          }
          trace_creature_spawn("skip-frustum", spawn, creature_distance, "frustum", &mi);
          continue;
        }

        // Draw-call LOD (hybrid instancing). Creatures are normally drawn ONE AT A TIME so each can have
        // its own animation pose, tint, alpha, particles, weapons and blob shadow -- but that is ~1 draw
        // call (plus a shadow draw) PER creature, which is the dominant frame cost in populated zones
        // (animation itself is free here -- toggling it changes nothing). Distant creatures that need NONE
        // of the per-instance treatment (opaque, untinted, no attached weapons/auras, no particles/ribbons,
        // not hidden) are instead pushed into the SAME instanced bucket gameobjects use, so e.g. 200 far
        // wolves collapse from ~200 draws to one instanced draw per model. Their animation syncs with the
        // shared model_animtime and they lose the soft blob shadow -- both imperceptible at range, and it
        // matches how the game simplifies distant units. Near / translucent / tinted / attachment / particle
        // creatures keep the individual path, so up-close visuals are unchanged.
        // "Simple" = needs no per-instance treatment, so it CAN be batched (subject to the dual-path
        // rule below). The distance check is intentionally NOT part of this -- it's handled separately.
        bool const simple_for_instancing =
             mi.model_alpha >= 0.999f
          && mi.model_tint == glm::vec3(1.0f)
          && spawn.attachment_models.empty()
          && mi.model->_particles.empty()
          && mi.model->_ribbons.empty()
          && (draw_hidden_models || !mi.model->is_hidden());
        bool const is_near = creature_distance < creature_instance_lod_dist;

        if (is_near || !simple_for_instancing)
        {
          // Individual path (near, or a complex creature at any distance). Mark the model so no far/simple
          // copy of it gets instanced -- a model drawn on BOTH paths in one frame renders nothing when
          // instanced (the molten-giant-invisibility bug).
          creature_spawn_instances_to_draw.push_back({spawn.guid, &mi, &spawn, creature_fade});
          models_drawn_individually.insert(mi.model.get());
          trace_creature_spawn("draw-queued", spawn, creature_distance, nullptr, &mi);
        }
        else
        {
          // Simple + far: batching candidate, but only if this model has NO individual copy this frame.
          // Decided after the loop (a near copy of the same model may appear later in the iteration).
          far_instance_candidates.push_back(&spawn);
          trace_creature_spawn("draw-queued-instanced", spawn, creature_distance, nullptr, &mi);
        }
      }

      // Resolve the deferred far/simple candidates now that every model with an individual copy is known.
      // No individual copy -> batch into one instanced draw (the perf win, e.g. a distant wolf pack).
      // Has an individual copy -> draw individually too, so the model is never split across both paths.
      for (auto* spawnp : far_instance_candidates)
      {
        if (!spawnp->model_instance.has_value())
        {
          continue;
        }
        auto& fmi = *spawnp->model_instance;
        Model* const fmodel = fmi.model.get();
        // second cull_fade_alpha call this frame is a stable read (same frame timestamp)
        // [game-view cull 2026-08-16] same client size-class unit cull as the primary path above.
        bool const f_in_range = glm::distance(camera_pos, spawnp->pos) <= creature_spawn_model_distance;
        float const f_dist_alpha = use_client_creature_cull
          ? unit_dist_alpha(fmi)
          : (f_in_range ? 1.0f : 0.0f);
        float const f_fade = cull_fade_alpha(&fmi, true, f_dist_alpha);
        _creature_fade_by_guid[spawnp->guid] = f_fade; // for the selection circle fade
        if (models_drawn_individually.find(fmodel) != models_drawn_individually.end())
        {
          creature_spawn_instances_to_draw.push_back({spawnp->guid, &fmi, spawnp, f_fade});
        }
        else
        {
          // Group by (model, display) so the batch is one skin; keep a representative for the per-instance
          // skin/geoset resolve the instanced draw needs.
          auto const key = std::make_pair(fmodel, spawnp->display_id);
          creature_instanced[key].push_back(fmi.transformMatrix());
          creature_instanced_fades[key].push_back(f_fade);
          creature_instanced_rep.emplace(key, &fmi); // first instance of this group wins
        }
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
        if (spawn.event_suppressed) // hidden by the Seasonal Events filter
        {
          continue;
        }
        float const gameobject_distance = glm::distance(camera_pos, spawn.pos);
        trace_gameobject_spawn("candidate", spawn, gameobject_distance);

        bool const go_in_range = gameobject_distance <= creature_spawn_model_distance;
        if (!go_in_range
            && (!spawn.model_instance.has_value() || spawn.model_instance->_cull_fade_phase == 0))
        {
          trace_gameobject_spawn("skip-distance", spawn, gameobject_distance, "draw distance");
          continue;
        }

        // [PERF 2026-07-24] Early frustum cull on the spawn POSITION (+ margin) before the expensive per-spawn
        // work -- same rationale as the creature loop above, and far more impactful here: a Turtle city has
        // ~45k gameobject spawns and this loop walked + fully processed every in-range one each frame.
        {
          static bool const s_no_go_precull = std::getenv("NOGGIT_NO_CREATURE_PRECULL") != nullptr;
          if (!s_no_go_precull)
          {
            float const m = 64.0f;
            glm::vec3 const lo(spawn.pos.x - m, spawn.pos.y - m, spawn.pos.z - m);
            glm::vec3 const hi(spawn.pos.x + m, spawn.pos.y + m, spawn.pos.z + m);
            if (!frustum.intersects(hi, lo))
            {
              continue;
            }
          }
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

        // binary range test (client model): the 2 s fade animations do all the work
        float const go_fade = cull_fade_alpha(&mi, true, go_in_range ? 1.0f : 0.0f);
        if (go_fade <= 0.0f)
        {
          trace_gameobject_spawn("skip-faded", spawn, gameobject_distance, "fade complete", &mi);
          continue;
        }

        // UNIFORM gameobject render distance: draw to the full knob distance regardless of model size
        // (same as creatures -- no size-proportional culling).
        if (!mi.isInFrustum(frustum))
        {
          trace_gameobject_spawn("skip-frustum", spawn, gameobject_distance, "frustum", &mi);
          continue;
        }

        if (draw_model_animations)
        {
          mi.model->animcalc = false;
        }
        // [2026-08-20 billboard parity] Cylindrical-billboard models (mage portals: root bone lock-Z)
        // must draw INDIVIDUALLY: the instanced bucket computes ONE shared bone set, so every GO
        // instance reused a billboard yaw computed for a single transform -- portals tracked the camera
        // but with a constant per-instance yaw error ("facing sideways"). The individual path re-animates
        // with this instance's own model_view, so the lock-Z spin is exact per portal. Direct bone-flag
        // scan (no cache: Model* reuse could alias); cylindrical models are rare (portals, forge chains)
        // so the perf cost is negligible -- spherical flame cards stay instanced as before.
        bool go_needs_per_instance_billboard = false;
        for (auto const& bone : mi.model->bones)
        {
          if (bone.flags.cylindrical_billboard_lock_x
              || bone.flags.cylindrical_billboard_lock_y
              || bone.flags.cylindrical_billboard_lock_z)
          {
            go_needs_per_instance_billboard = true;
            break;
          }
        }

        if (go_fade < 0.999f || go_needs_per_instance_billboard)
        {
          // mid-fade: EXACT creature treatment (individual draw, continuous alpha) -- see the
          // dedicated loop after the creature spawn draws
          go_fading_individual.push_back({&mi, go_fade});
        }
        else
        {
          models_to_draw[mi.model.get()].push_back(mi.transformMatrix());
          models_to_draw_fades[mi.model.get()].push_back(1.0f);
          models_to_draw_interior[mi.model.get()].push_back(interior_light_at(glm::vec3(mi.transformMatrix()[3]))); // GO can be indoors
          go_bucket_models.insert(mi.model.get());
        }
        trace_gameobject_spawn("draw-queued", spawn, gameobject_distance, nullptr, &mi);
      }
    }

    /*
    if (_world->need_model_updates)
    {
      _world->update_models_by_filename();
    }*/

    // GROUND CLUTTER (checklist 14.1): scatter the DBC-driven detail doodads (grass/flowers/pebbles)
    // on near terrain and feed them into the instanced-M2 buckets below. Client-faithful in WHICH
    // models appear and their density; drawn only within a short radius of the camera (the client
    // fades clutter out at close range too) and behind a toggle (default on -- the client shows it).
    // Hoisted for the instanced-M2 uniform setup below (the clutter block is its own scope): the
    // groundEffectDist fed to the shader's client-exact per-vertex fade ramp.
    float clutter_detail_dist = 0.0f;
    {
      // Density (0..100, like the in-game slider) + draw distance come from the graphics settings;
      // the on/off is the toolbar/menu toggle passed in as draw_ground_clutter. Read live each frame
      // so the slider takes effect without a restart.
      QSettings clutter_settings;
      float const clutter_density = std::clamp(clutter_settings.value("render/ground_clutter_density", 100.0f).toFloat(), 0.0f, 100.0f) / 100.0f;
      // Client-matching default: 3.3.5a ships groundEffectDist at 70.0 (string at 0x00a3f2ec).
      // The slider still reaches 500 so the editor can see further than the client does.
      float const clutter_dist = clutter_settings.value("render/ground_clutter_distance", 70.0f).toFloat();
      clutter_detail_dist = clutter_dist;

      auto dist2 = [](glm::vec3 const& a, glm::vec3 const& b)
      {
        glm::vec3 const d = a - b;
        return d.x * d.x + d.y * d.y + d.z * d.z;
      };

      if (draw_ground_clutter && clutter_density > 0.0f && draw_models && !minimap_render)
      {
        noggit::perf::Scoped _prof_clutter(noggit::perf::Phase::Clutter); // M2 spike hunt: ground-clutter inject
    SecTimer _sec2(g_vk_sec_clutter_ms);
        // Density-parity diagnostics (Westfall tile 30_52 ground truth = ~426 instances/chunk,
        // simulated from the ADT+DBC): log the funnel every ~5s so instance loss is attributable.
        static int clutter_dbg_frame = 0;
        bool const clutter_dbg = (++clutter_dbg_frame % 300) == 0;
        int dbg_chunks = 0, dbg_deferred = 0, dbg_placed = 0, dbg_submitted = 0, dbg_notloaded = 0, dbg_far = 0;
        // First-time clutter computation (DBC pick + RNG scatter + height interp per chunk) is done lazily
        // on this draw thread. When crossing into a new area ~40 chunks would compute in ONE frame -> a
        // visible hitch. Budget it: at most a few NEW chunks per frame, so clutter fades in over a handful
        // of frames instead of stalling. Already-computed chunks are free (cached).
        int clutter_compute_budget = 6;
        for (auto& tile_pair : _world->_loaded_tiles_buffer)
        {
          MapTile* tile = tile_pair.second;
          // _loaded_tiles_buffer is a fixed 4096-entry array refilled each frame up to a single nullptr
          // sentinel; entries PAST the sentinel are stale dangling MapTile* from earlier frames (tiles
          // since unloaded). Every other loop over this buffer BREAKs at the null sentinel -- this one
          // used `continue`, so it walked into the stale tail and dereferenced a freed MapTile in
          // tile->finishedLoading() -> use-after-free crash in WorldRender::draw (nvoglv64 __fastfail /
          // heap corruption that also surfaced as the AsyncLoader AV). MUST break, not continue.
          if (!tile)
          {
            break;
          }
          if (!tile->finishedLoading())
          {
            continue;
          }
          for (int cz = 0; cz < 16; ++cz)
          {
            for (int cx = 0; cx < 16; ++cx)
            {
              MapChunk* chunk = tile->getChunk(cx, cz);
              if (!chunk)
              {
                continue;
              }
              // Cheap chunk-centre distance gate before touching per-doodad work. Slack must be
              // added to the RADIUS before squaring (adding to the squared value shrinks the slack
              // to ~3yd at range and clipped whole edge chunks whose content was in radius).
              glm::vec3 const ccenter = chunk->vcenter;
              float const gate = clutter_dist + 24.0f; // ~chunk half-diagonal
              if (dist2(camera_pos, ccenter) > gate * gate)
              {
                continue;
              }
              // Frustum-cull the whole chunk before touching its (up to ~350) doodads. Ground clutter was
              // by far the dominant Elwynn cost -- ~8-12k doodads re-collected every frame with NO frustum
              // test, so roughly half (everything behind the camera) was string-looked-up, pushed, uploaded
              // and vertex-processed for nothing. Skipping off-screen chunks removes that with zero visual
              // change (off-screen doodads aren't visible). vmin/vmax is the chunk's world AABB.
              if (!frustum.intersects(chunk->vmax, chunk->vmin))
              {
                continue;
              }
              // Spread first-time clutter computation across frames (see budget note above).
              if (!chunk->_detail_doodads_computed)
              {
                if (clutter_compute_budget <= 0)
                {
                  ++dbg_deferred;
                  continue; // defer this chunk's clutter to a later frame -> no load hitch
                }
                --clutter_compute_budget;
              }
              auto const& doodads = chunk->detailDoodads();
              ++dbg_chunks;
              dbg_placed += static_cast<int>(doodads.size());
              // CLIENT-EXACT density model (wow.exe FUN_006b2b80/FUN_006bfc10): grass draws at FULL
              // authored density across the whole radius -- the client NEVER drops instances by
              // distance. The density slider remains as a uniform user override (100% = client).
              // [2026-08-20 CLIENT-EXACT FADE] There is NO per-blade CPU fade or distance cull here
              // any more: the 3.3.5a client computes the clutter fade PER VERTEX in the DetailDoodad
              // vertex shader -- alpha = clamp((dist - viewZ)/(0.15*dist), 0, 1), a view-depth ramp
              // over the last 15% of groundEffectDist (FUN_007b15d0 c9; fade start 0.85 @ 0x9f23d0).
              // m2_vert now computes exactly that (detail_dist uniform below); every CPU band/gate
              // variant produced visible chunk seams or brightening lines. Blades past the distance
              // fade to alpha 0 in-shader; the chunk-centre gate above bounds the overdraw.
              std::size_t idx = 0;
              // [finding 123] one lookup per RUN of identical models, not three per doodad
              Model* cl_last = nullptr;
              tsl::robin_map<Model*, std::uint32_t>* cl_counts = nullptr;
              std::vector<glm::mat4x4>* cl_tf = nullptr;
              std::vector<float>* cl_fade = nullptr;
              std::vector<glm::vec4>* cl_int = nullptr;
              for (auto const& dd : doodads)
              {
                if (clutter_density < 1.0f)
                {
                  std::uint32_t const cut = static_cast<std::uint32_t>(clutter_density * 65536.0f);
                  std::uint32_t const h = ((static_cast<std::uint32_t>(idx++) * 2654435761u) >> 16) & 0xFFFFu;
                  if (h >= cut)
                  {
                    continue;
                  }
                }
                Model* m = dd.cached_model;
                if (!m)
                {
                  auto it = _detail_doodad_models.find(dd.model_path);
                  if (it == _detail_doodad_models.end())
                  {
                    it = _detail_doodad_models.try_emplace(
                      dd.model_path,
                      BlizzardArchive::Listfile::FileKey(dd.model_path),
                      _world->getRenderContext()).first;
                  }
                  m = it->second.get();
                  if (!m || !m->finishedLoading() || m->loading_failed())
                  {
                    ++dbg_notloaded;
                    continue; // not loaded yet -> don't cache; retry next frame
                  }
                  // Marker for the DETAIL-DOODAD shading path: dimmed by day/night BRIGHTNESS but
                  // NOT the zone light COLOUR (which greened the yellow atlas) -- grass keeps its own
                  // hue and just darkens at night, like the client's ground effects.
                  m->_force_unlit = true;
                  dd.cached_model = m; // loaded -> cache so this doodad never string-hashes again
                }
                if (m != cl_last)
                {
                  cl_last = m;
                  cl_tf   = &models_to_draw[m];
                  cl_fade = &models_to_draw_fades[m];
                  cl_int  = &models_to_draw_interior[m];
                  // [finding 127] models_to_draw* are per-frame LOCALS, so every frame these
                  // vectors grow from zero capacity to ~132k entries -- geometric reallocation
                  // moving roughly twice the final bytes, on top of the copy itself. Reserve
                  // from last frame's count; the clutter set barely changes frame to frame.
                  static tsl::robin_map<Model*, std::uint32_t> s_last_clutter_count;
                  // Reserve whenever capacity is short of last frame's total for this model --
                  // NOT only when the vector is empty. The tile-doodad and WMO-doodad producers
                  // run BEFORE clutter, so a model they also use would already be non-empty here
                  // and would have missed the reserve entirely, leaving the 132k-entry growth in
                  // place for exactly the models most likely to be shared.
                  {
                    auto const lc = s_last_clutter_count.find(m);
                    if (lc != s_last_clutter_count.end() && lc->second > cl_tf->capacity())
                    {
                      cl_tf->reserve(lc->second);
                      cl_fade->reserve(lc->second);
                      cl_int->reserve(lc->second);
                    }
                  }
                  cl_counts = &s_last_clutter_count;
                }
                if (cl_counts) { (*cl_counts)[m] = static_cast<std::uint32_t>(cl_tf->size() + 1u); }
                cl_tf->push_back(dd.transform);
                cl_fade->push_back(1.0f); // fade is per-vertex in the shader (client c9 ramp)
                // For the detail_doodad shader path this attribute carries the PACKED per-blade bake
                // (MapChunk::computeDetailDoodads): x = mccv_r*256+g, y = b*2+shadowBit+1024 (marker),
                // z/w = ground-normal x/z. Only the detail branch decodes it; clutter never takes the
                // interior branch.
                cl_int->push_back(dd.tint);
                ++dbg_submitted;
              }
            }
          }
        }
        // DIAGNOSTIC (grass saga, one-shot, always-on): the first frame that actually SUBMITS
        // clutter prints totals + species to LogError, so a normal user run answers "is the green
        // stuff clutter at all, and which models" straight from log.txt (LogDebug is env-gated off).
        static bool s_clutter_diag_done = false;
        if (!s_clutter_diag_done && dbg_submitted > 0)
        {
          s_clutter_diag_done = true;
          LogError << "GRASS-DIAG clutter first-submit: chunks=" << dbg_chunks
                   << " placed=" << dbg_placed << " submitted=" << dbg_submitted << std::endl;
          int diag_species = 0;
          for (auto const& mp : _detail_doodad_models)
          {
            Model* m = mp.second.get();
            auto it = models_to_draw.find(m);
            if (it != models_to_draw.end() && !it->second.empty() && diag_species < 8)
            {
              ++diag_species;
              LogError << "GRASS-DIAG species " << mp.first << " x" << it->second.size() << std::endl;
            }
          }
        }
        // [GRASS-LOSS 2026-08-28] "grass in ascension goes patchy/missing after travelling a bit".
        // The funnel below already counts every way a blade can be dropped, but it only went to
        // LogDebug (env-gated off), so a normal run says nothing. Report it at LogError whenever
        // clutter is actually being LOST -- chunks whose first-time compute was deferred past the
        // per-frame budget, or doodad models that are not loaded -- throttled to the same ~5s tick
        // so it cannot spam. A healthy frame prints nothing.
        if (clutter_dbg && (dbg_deferred > 0 || dbg_notloaded > 0))
        {
          LogError << "GRASS-LOSS chunks=" << dbg_chunks << " deferred=" << dbg_deferred
                   << " notloaded=" << dbg_notloaded << " placed=" << dbg_placed
                   << " submitted=" << dbg_submitted << " far=" << dbg_far
                   << " species=" << _detail_doodad_models.size()
                   << " dist=" << clutter_dist << " density=" << clutter_density << std::endl;
        }
        if (clutter_dbg)
        {
          LogDebug << "[clutter] chunks=" << dbg_chunks << " deferred=" << dbg_deferred
                   << " placed=" << dbg_placed << " submitted=" << dbg_submitted
                   << " far=" << dbg_far << " notloaded=" << dbg_notloaded
                   << " dist=" << clutter_dist << " density=" << clutter_density << std::endl;
          // Species histogram: which detail models are actually drawing (answers "wrong asset for
          // the zone" reports without guessing -- compare against the zone's GroundEffect rows).
          for (auto const& mp : _detail_doodad_models)
          {
            Model* m = mp.second.get();
            auto it = models_to_draw.find(m);
            if (it != models_to_draw.end() && !it->second.empty())
            {
              LogDebug << "[clutter]   " << mp.first << " x" << it->second.size()
                       << (m->loading_failed() ? " LOAD-FAILED" : "") << std::endl;
            }
            else if (m && m->loading_failed())
            {
              LogDebug << "[clutter]   " << mp.first << " LOAD-FAILED" << std::endl;
            }
          }
        }
      }
    }

    std::unordered_map<Model*, std::size_t> model_boxes_to_draw;

    {
      if (draw_models || (minimap_render && minimap_render_settings->use_filters))
      {
        // [perf 2026-08-05] MDI cross-model doodad batch pass. Runs BEFORE the instanced program scope below
        // (so program scopes don't nest); draws every batchable static doodad in a few glMultiDrawElements-
        // Indirect calls and records them in _mdi_batched_models so the persistent loop below skips them.
        // Non-batchable doodads still draw via drawPersistent. Needs persistent doodads (its instance source).
        if (s_doodad_mdi && s_persistent_doodads && !minimap_render && draw_models)
        {
          noggit::perf::Scoped _prof_mdi(noggit::perf::Phase::SubmitInst);
    SecTimer _sec2(g_vk_sec_submit_ms);
          // [perf 2026-08-07] GPU-driven P1: collect ALL LOADED tiles' doodads (camera-independent) so the batch
          // caches across camera movement and rebuilds ONLY on tile load/unload -- killing the per-frame assembly
          // that made the old visible-set MDI net-flat. Draw all -> the idle GPU frustum-clips the off-screen
          // ones. _mdi_batched_models still makes the persistent loop below skip these models (no double-draw).
          _mdi_all_loaded.clear();
          for (MapTile* t : _world->mapIndex.loaded_tiles())
          {
            if (!t)
              continue;
            auto const& b = t->renderer()->doodadInstanceBuffers();
            float const t_dist = t->camDist();
            for (auto const& kv : b)
            {
              // [2026-08-25 dressing class-split] the MDI batch bypassed every distance rule, so
              // small vegetation drew to the horizon even with the cull on. Same conservative
              // bucket test as the persistent path (tile centre - half diagonal vs client cull_far):
              // a dropped bucket is one whose EVERY instance is past the client's hard cull.
              if (use_client_doodad_cull || (use_dressing_cull && kv.second.cull_class <= 1))
              {
                float cull_far, band;
                ModelInstance::doodadCullParams(kv.second.cull_class, 1.0f, cull_far, band);
                if (t_dist - 377.0f > cull_far)
                {
                  continue;
                }
              }
              _mdi_all_loaded.emplace_back(kv.first, &kv.second);
            }
          }
          drawDoodadsBatched(_mdi_all_loaded, model_view, draw_hidden_models, frustum, _world->model_animtime);
        }

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

        // [dyn-MDI 2026-08-07] batch the dynamic instanced pool FIRST (before this program scope opens);
        // the loop below keeps every side-effect (additive-light deferral, particle collection) and just
        // skips the draws of consumed buckets (_dyn_batched_models).
        // [FALLS-DIAG 2026-08-08] NOGGIT_NO_DYN_MDI=1 skips the dyn batch entirely -> every bucket
        // draws via the instanced fallback (prepareDraw path). Path-isolation bisect switch.
        static bool const s_no_dyn_mdi = std::getenv("NOGGIT_NO_DYN_MDI") != nullptr;
        vkM2SnapshotClear();   // [VULKAN] one snapshot per frame, appended by every producer below
        if (!s_no_dyn_mdi)
        {
          drawDynamicBatched(models_to_draw, models_to_draw_interior, models_to_draw_fades,
                             go_bucket_models, model_view, static_cast<int>(_world->model_animtime),
                             draw_hidden_models);
          vkM2SnapshotAppend();
        }

        // [creature MDI 2026-08-18] PHASE A: batch the far/simple creature groups here too, BEFORE the
        // m2_shader scope opens (program scopes must not nest). The classic creature loop below skips groups
        // recorded in _creature_batched. Toggle NOGGIT_CREATURE_MDI (default off until validated); scoped as
        // M2Creatures so the profiler shows the cost move off the per-group animate()/TBO path.
        static bool const s_creature_mdi = std::getenv("NOGGIT_CREATURE_MDI") != nullptr;
        _creature_batched.clear();
        if (s_creature_mdi && draw_models)
        {
          noggit::perf::Scoped _prof_crembatch(noggit::perf::Phase::M2Creatures);
          drawCreaturesBatched(creature_instanced, creature_instanced_rep, creature_instanced_fades,
                               [&](glm::vec3 const& p) { return interior_light_at(p, false); },
                               model_view, static_cast<int>(_world->model_animtime), draw_hidden_models,
                               _creature_batched);
        }

        // [creature body MDI 2026-08-18] PHASE A (real): batch the INDIVIDUAL creature BODY draws with
        // per-instance bones, BEFORE the m2_shader scope opens. The classic loop below skips a batched body's
        // OWN animate + body-draw but still draws its mount/attachments/particles/shadow (and restores its
        // _animation_bones so gear seats correctly). The game character is pushed to the vector AFTER this
        // point -> never batched (stays individual). Toggle NOGGIT_CREATURE_BODY_MDI (default off).
        static bool const s_creature_body_mdi = std::getenv("NOGGIT_CREATURE_BODY_MDI") != nullptr;
        _creature_body_batched.clear();
        if (s_creature_body_mdi && draw_models && draw_model_animations)
        {
          std::vector<CreatureBodyBatchItem> body_items;
          body_items.reserve(creature_spawn_instances_to_draw.size());
          for (auto const& di : creature_spawn_instances_to_draw)
          {
            CreatureBodyBatchItem bi;
            bi.instance = di.instance;
            bi.fade = di.fade;
            bi.display_id = di.spawn ? di.spawn->display_id : 0;
            bi.anim_time_offset = di.spawn ? di.spawn->animation_time_offset : 0;
            bi.has_mount = di.spawn && di.spawn->mount_instance.has_value();
            body_items.push_back(bi);
          }
          noggit::perf::Scoped _prof_body(noggit::perf::Phase::M2Creatures);
          drawCreatureBodiesBatched(body_items,
                                    [&](glm::vec3 const& p) { return interior_light_at(p, false); },
                                    model_view, static_cast<int>(_world->model_animtime), draw_hidden_models,
                                    _creature_body_batched);
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
        m2_shader.uniform("model_origin", glm::vec3(0.0f)); // instanced: transform carries full world pos
        // Terrain-parity boundary for tile doodads: pixels past the object cull distance are
        // discarded in the fragment shader, slicing models in/out like the far plane does terrain.
        m2_shader.uniform("slice_dist", s_no_dist_fade ? 0.0f : _cull_distance);
        // CLIENT clutter fade distance (m2_vert c9-equivalent view-depth ramp over the last 15% of
        // groundEffectDist). Only the detail_doodad frag path reads the resulting fade.
        m2_shader.uniform("detail_dist", clutter_detail_dist);
        _vk_clutter_detail_dist = clutter_detail_dist;   // [VULKAN] same ramp in the VK M2 shader

        {
        // [GL-COST HUNT] report, once every 300 frames, how much of the M2 load GL is STILL
        // carrying in VK mode. The GL scene pass costs 5.15 ms/frame of which only 0.14 ms builds
        // VK's feeds; if these counts are non-zero the rest is GL drawing what VK would not take.
        {
          static int s_glm2_frames = 0;
          static std::size_t s_still = 0, s_fed = 0, s_pass = 0, s_arena = 0, s_empty = 0, s_hidden = 0;
          s_still += _vk_gl_still_drew; s_fed += _vk_classic_fed;
          s_pass += _vk_fb_pass; s_arena += _vk_fb_arena; s_empty += _vk_fb_empty;
          s_hidden += _vk_fb_hidden;
          if (++s_glm2_frames >= 300)
          {
            LogError << "[VK] GL-M2/frame: stillDrewByGL=" << (s_still / 300)
                     << " fedToVK=" << (s_fed / 300)
                     << " | reject pass=" << (s_pass / 300)
                     << " arena=" << (s_arena / 300)
                     << " empty=" << (s_empty / 300)
                     << " allHidden=" << (s_hidden / 300) << std::endl;
            s_glm2_frames = 0; s_still = s_fed = s_pass = s_arena = s_empty = s_hidden = 0;
          }
        }
        _vk_gl_still_drew = 0;
        _classic_issued = 0;   // [VK-DIFF] per-FRAME count of the GL-only classic doodad path
        _vk_classic_fed = 0;
        _vk_fb_pass = _vk_fb_arena = _vk_fb_empty = _vk_fb_hidden = 0;
        _vk_fb_names.clear();
        noggit::perf::Scoped _prof_ddraw(noggit::perf::Phase::DoodadDraw); // M2 spike hunt: instanced doodad buckets only
    SecTimer _sec2(g_vk_sec_ddraw_ms);
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

            // INDOOR doodads/gameobjects: the baked MOCV floor colour as FLAT EVEN room light deep inside
            // (matching the live client's even dark-all-around barrel look; RE_notes/15 section 4+11), with
            // GAP B doorway spill in a (a in [0.5,1.0]); outdoor instances carry (0,0,0,0) -> sun path.
            // [perf 2026-08-04] The interior light is now ASSIGNED AT INJECTION (models_to_draw_interior, in
            // lockstep with models_to_draw) -- CLIENT-TRUE: only WMO doodads + gameobjects sampled it;
            // terrain/tile doodads + clutter are outdoor by construction (the client runs NO interior test
            // on them). The old per-instance interior_light_at loop HERE, over the whole doodad flood every
            // frame, was the 1300ms M2 spike. Just read the parallel vector now (defensive size pad).
            std::vector<glm::vec4>& bucket_interior = models_to_draw_interior[pair.first];
            bucket_interior.resize(pair.second.size(), glm::vec4(0.0f));
            // Per-instance cull-fade alphas gathered in lockstep with the transforms (defensive pad
            // with 1.0 if a push site missed the parallel vector).
            std::vector<float>& bucket_fades = models_to_draw_fades[pair.first];
            bucket_fades.resize(pair.second.size(), 1.0f);

            // [dyn-MDI 2026-08-07] consumed by drawDynamicBatched -> already drawn via MDI; keep every
            // side-effect around this point (deferral above, particle collection below), skip only the draw.
            if (!_dyn_batched_models.count(pair.first))
            {
            // Gameobject buckets fade (creature mechanic) instead of slicing: slice off for them.
            // (A model shared by tile doodads AND a GO spawn loses the slice for its doodads too --
            // acceptable, the bucket is drawn in one call.)
            m2_shader.uniform("slice_dist",
                              (s_no_dist_fade || go_bucket_models.count(pair.first)) ? 0.0f : _cull_distance);

            {
              noggit::perf::Scoped _prof_submit(noggit::perf::Phase::SubmitInst);
              _classic_issued += pair.second.size();   // [VK-DIFF] GL-only classic doodad path
              // [VULKAN phase C] hand the SAME bucket to Vulkan (GL's own draw below is untouched)
              bool const vk_took_bucket = vkFeedClassicBucket(pair.first, pair.second, &bucket_interior);
              // GL skips ONLY the buckets Vulkan actually took; a rejected bucket still needs GL or
              // the model would simply vanish.
              // [GL-COST HUNT] instances GL still draws while VK owns M2 -- every one of these is a
              // bucket Vulkan refused, and GL doing the scene twice is exactly what we are paying for.
              if (vk_owns_m2 && !vk_took_bucket)
              {
                // [PIPELINE step 1] Defer to the render thread instead of drawing here, so this walk
                // can eventually leave that thread. Same instances, same transforms, replayed below.
                _vk_gl_still_drew += pair.second.size();
                VkGlDeferredBucket d;
                d.model = pair.first;
                d.transforms = pair.second;
                d.interiors = bucket_interior;
                d.fades = bucket_fades;
                d.slice_dist = (s_no_dist_fade || go_bucket_models.count(pair.first)) ? 0.0f
                                                                                      : _cull_distance;
                _vk_gl_deferred.push_back(std::move(d));
              }
              if (!vk_owns_m2)
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
                , /*no_cull*/ false
                , /*representative*/ nullptr
                , bucket_interior
                , bucket_fades
              );
            }
            _world->_n_rendered_objects += pair.second.size();
            }

            // Collect particle/ribbon models regardless of the animation toggle: when off we still
            // draw them, just frozen in place (their simulation isn't advanced), instead of hiding them.
            if (!pair.first->_particles.empty() || !pair.first->_ribbons.empty())
            {
              model_with_particles[pair.first] = pair.second;
            }

            if (capture_debug_enabled())
            {
              LogDebug << "WorldRender::draw instanced m2 bucket end model='"
                       << pair.first->file_key().stringRepr()
                       << "'" << std::endl;
            }
          }
        }

        // [perf 2026-08-05] PERSISTENT tile-doodad buckets: drawn straight from tile-owned GPU buffers (no
        // gather, no per-frame transform upload, no per-instance cull -- tile-level cull + shader slice_dist
        // clip). Same instanced program + shared M2RenderState as the models_to_draw loop above. Tile doodads
        // slice at the object cull distance like terrain. (Pure-additive light doodads draw here rather than
        // deferring past the water -- rare for tile MDDF, so the water-occlusion nicety is skipped.)
        if (s_persistent_doodads && !persistent_doodad_draws.empty())
        {
          // Distance fade is now a smooth alpha blend via extra_alpha (below), not the hard pixel slice.
          m2_shader.uniform("slice_dist", 0.0f);
          noggit::perf::Scoped _prof_submit(noggit::perf::Phase::SubmitInst);
          for (std::size_t pdi = 0; pdi < persistent_doodad_draws.size(); ++pdi)
          {
            auto const& pd = persistent_doodad_draws[pdi];
            float const pd_fade = s_no_dist_fade ? 1.0f : persistent_doodad_fades[pdi];
            if (pd_fade <= 0.0f)
            {
              continue; // fully faded (kept collected only so nothing pops when the tile drops)
            }
            Model* const m = pd.first;
            if (!draw_hidden_models && m->is_hidden())
            {
              continue;
            }
            // [perf 2026-08-05] batchable models were already drawn by the MDI pass (drawDoodadsBatched, before
            // this program's scope); skip them here so they aren't drawn twice.
            if (s_doodad_mdi && _mdi_batched_models.count(m))
            {
              continue;
            }
            // [phase J] PERSISTENT doodads are the third M2 path, and VK feeds all three. When VK
            // owns M2 this draw is redundant -- the models are already in the VK frame. Verified by
            // the bench screenshot diff, since parity mode cannot cover a gated path.
            if (!vk_owns_m2)
            m->renderer()->drawPersistent(model_view, pd.second->transform_vbo, pd.second->interior_vbo,
                                          static_cast<int>(pd.second->count), m2_shader, model_render_state,
                                          _world->model_animtime, pd_fade);
            _world->_n_rendered_objects += pd.second->count;
          }
        }
        } // end DoodadDraw scope (instanced doodad buckets)

        // Everything below in the M2 phase is INDIVIDUAL / non-instanced draws (creatures, per-instance
        // billboard doodads, mounts, attachments, gameobjects) -- the suspected CPU-bound draw-submission
        // storm. IndivDraw times it as one bucket; the nested M2Creatures/M2Particles split it further.
        noggit::perf::Scoped _prof_indiv(noggit::perf::Phase::IndivDraw);
        // [finding 96] IndivDraw is the largest child of M2 and the hash-lookup theory was wrong
        // (finding 95). Split it: the head (state + creature groups), the per-doodad bucket loop,
        // and the flush that feeds VK. Reported every 300 frames.
        static double s_id_head = 0.0, s_id_loop = 0.0, s_id_flush = 0.0;
        static unsigned s_id_frames = 0;
        auto const _id_t0 = std::chrono::steady_clock::now();
        auto _id_t1 = _id_t0, _id_t2 = _id_t0;
    SecTimer _sec2(g_vk_sec_indiv_ms);

        // A grass bucket may have left alpha-to-coverage enabled; clear it before the rest of the
        // draws so it can't affect creatures / other passes.
        gl.disable(GL_SAMPLE_ALPHA_TO_COVERAGE);

        // Creatures keep the opacity-fade mechanic (no pixel slicing) -- turn the slice off for
        // the rest of this program's draws (occlusion/other passes inherit 0 = off too).
        m2_shader.uniform("slice_dist", 0.0f);

        // [CRE-GATE diag 2026-08-18] definitive: how many creature groups are in the INSTANCED pool (what
        // Phase A can batch) vs how many the batch actually consumed. Fires from the loop that always runs.
        {
          static int s_gate_tick = 0;
          if ((s_gate_tick++ % 120) == 0)
            LogError << "[CRE-GATE] mdi=" << (s_creature_mdi ? 1 : 0) << " draw_models=" << (draw_models ? 1 : 0)
                     << " creature_instanced=" << creature_instanced.size()
                     << " batched=" << _creature_batched.size()
                     << " individual=" << creature_spawn_instances_to_draw.size() << std::endl;
        }

        // Instanced creature batches, grouped by (model, display) so each call is a single skin. Same
        // instanced program/state as the doodad buckets above, but each gets its group's REPRESENTATIVE
        // instance so the replaceable creature skin texture + geoset selection resolve -- without it the
        // instanced path can't see per-spawn textures and every far creature rendered invisible.
        for (auto& entry : creature_instanced)
        {
          if (_creature_batched.count(entry.first))
            continue; // consumed by the creature MDI batch above (NOGGIT_CREATURE_MDI)
          Model* const m = entry.first.first;
          auto const& transforms = entry.second;
          if (transforms.empty() || (!draw_hidden_models && m->is_hidden()))
          {
            continue;
          }
          if (draw_model_animations)
          {
            m->animcalc = false;
          }
          auto const rep_it = creature_instanced_rep.find(entry.first);
          ModelInstance const* const rep = rep_it != creature_instanced_rep.end() ? rep_it->second : nullptr;
          std::vector<glm::vec4> creature_interior;
          creature_interior.reserve(transforms.size());
          for (auto const& tr : transforms)
          {
            creature_interior.push_back(interior_light_at(glm::vec3(tr[3]), false)); // creatures move -> no cache
          }
          std::vector<float>& creature_fades = creature_instanced_fades[entry.first];
          creature_fades.resize(transforms.size(), 1.0f);
          {
            noggit::perf::Scoped _prof_cre2(noggit::perf::Phase::M2Creatures);
            m->renderer()->draw(model_view, transforms, m2_shader, model_render_state, frustum,
                                _cull_distance, camera_pos, _world->model_animtime, draw_models_with_box,
                                model_boxes_to_draw, display, /*no_cull*/ false, rep, creature_interior,
                                creature_fades);
          }
          _world->_n_rendered_objects += transforms.size();
        }

        // Per-instance-animation WMO doodads (billboarded glow cards, global-seq flicker): each copy
        // animates with its own transform so billboards face the camera per copy, mirroring the
        // creature path 1:1 -- draw (animate inside), then advance THIS copy's OWN particle state
        // (keyed by placement position) while the bones/emitters are at this copy's phase. Their
        // particles draw in a dedicated loop in the particle pass (NOT the placement loop -- feeding
        // both would double-advance the same state).
        // CRITICAL: these are NON-instanced draws, so they need the NON-instanced m2 program bound --
        // in the instanced program "transform" is a vertex ATTRIBUTE (4 slots, divisor 1), and calling
        // the non-instanced draw under it left the VAO/attrib state inconsistent, which the NVIDIA
        // driver eventually faulted on in a later (particle) draw. Own program scope, like creatures.
        // INSTANCED billboard-doodad path: OPT-IN, DEFAULT OFF (restored 2026-07-22 per RE doc 23 and the
        // prior session's finding above). doc 23 = disassembled Wow.exe ground truth: the 3.3.5a client
        // draws ANIMATED + BILLBOARD doodads INDIVIDUALLY, material-sorted, and DELIBERATELY EXCLUDES them
        // from batching (anim gate 0x824580) -- merge bakes rest-pose matrices and can't do per-frame
        // camera-facing billboards. Our instanced billboard path is the exact "instancing detour" doc 23
        // calls WRONG: it shares ONE VAO between the instanced and individual m2 programs, leaving a
        // per-instance-attribute GL error that renders as the "black mesh slices" (Timbermaw). The DEFAULT
        // is now the client's real mechanism -- the individual path below, material-sorted (doc 23
        // technique #2, the std::sort at ~line 3125). The STATIC-doodad bucket instancing (SubmitInst
        // above, = client Path A) is a SEPARATE path and stays on. Opt into the (still-buggy) instanced
        // billboard path. NOW DEFAULT ON (2026-07-23): GpuWait proved we are CPU-BOUND on draw submission, so
        // instancing the animated doodads to cut draw-call count is a big win (SubmitIndiv ~15->~2.8ms, WorldDraw
        // ~40->~13ms, Timbermaw ~11->~45fps) -- the client (D3D9-cheap submission) doesn't bother, but we must.
        // The recurring Timbermaw BLACK was a premature GL bone-TBO upload during the per-instance bake (FIXED in
        // ModelRender::updateBoneMatrices via a buffer-existence guard), NOT the shared VAO. A separate sporadic
        // GL_INVALID_OPERATION remains (a benign RACE the per-call check masks; no visual corruption) -- watch it.
        // Opt OUT with NOGGIT_NO_INSTANCED_DOODADS=1 if a scene ever blacks.
        static bool const s_inst_doodads = std::getenv("NOGGIT_NO_INSTANCED_DOODADS") == nullptr;
        // Partition pure-additive light shafts OUT of both per-instance paths and defer them past
        // the water (see deferred_pi_light_doodads): the lighthouse beam etc. must composite over
        // the harbor surface exactly like the bucketed god-rays at the models_to_draw deferral.
        if (!per_instance_wmo_doodads.empty())
        {
          auto const mid = std::stable_partition(
            per_instance_wmo_doodads.begin(), per_instance_wmo_doodads.end(),
            [](ModelInstance* mi)
            {
              Model* m = mi ? mi->model.get() : nullptr;
              return !(m && m->finishedLoading()
                       && is_pure_additive_light_effect(m)
                       && m->_particles.empty() && m->_ribbons.empty());
            });
          deferred_pi_light_doodads.assign(mid, per_instance_wmo_doodads.end());
          per_instance_wmo_doodads.erase(mid, per_instance_wmo_doodads.end());
        }
        if (!per_instance_wmo_doodads.empty() && !s_inst_doodads)
        {
          OpenGL::Scoped::use_program doodad_shader {*_m2_program.get()};

          OpenGL::M2RenderState doodad_render_state;
          doodad_render_state.tex_arrays = {0, 0};
          doodad_render_state.tex_indices = {0, 0};
          doodad_render_state.tex_unit_lookups = {0, 0};
          gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
          gl.disable(GL_BLEND);
          gl.depthMask(GL_TRUE);
          gl.enable(GL_CULL_FACE);
          doodad_shader.uniform("blend_mode", 0);
          doodad_shader.uniform("unfogged", static_cast<int>(doodad_render_state.unfogged));
          doodad_shader.uniform("unlit",  static_cast<int>(doodad_render_state.unlit));
          doodad_shader.uniform("tex_unit_lookup_1", 0);
          doodad_shader.uniform("tex_unit_lookup_2", 0);
          doodad_shader.uniform("pixel_shader", 0);

          // [STUDY APPLY, RE doc 23] The 3.3.5a client material-sorts every doodad (group-hash 0x81cc50 +
          // heapsort) so consecutive individual draws share texture/blend/state and prepareDraw's
          // check-before-set M2RenderState cache HITS instead of re-issuing full state per doodad. noggit
          // drew them in gather order -> cache miss on nearly every doodad = redundant blend/cull/tex/shader
          // uniform churn in the SubmitIndiv wall. Group by model (same model == same materials/passes).
          // Cheap (model.get() is a plain pointer read); the earlier "3000ms" from this was the fog bug,
          // not the sort. NOT batching -- still one draw per doodad, like the client's individual path.
          // [phase L] That material sort exists ONLY so GL's M2RenderState cache hits on consecutive
          // individual draws. Vulkan is bindless and buckets per model, so draw order buys it nothing --
          // and in VK mode the draws this sort was ordering are gated off anyway. Skip it. (Anything VK
          // rejects still falls back to a GL draw; those are rare, and an unsorted handful is far cheaper
          // than sorting the whole list every frame.)
          if (!vk_owns_m2)
          {
            std::sort(per_instance_wmo_doodads.begin(), per_instance_wmo_doodads.end(),
              [](ModelInstance* a, ModelInstance* b) { return a->model.get() < b->model.get(); });
          }

          // reused across frames: a fresh unordered_set here allocated and rehashed every frame
          static std::unordered_set<std::uint64_t> seen_doodad_keys;
          seen_doodad_keys.clear();
          // [GL-COST] Vulkan was fed ONE BUCKET PER DOODAD from this loop -- a 1-element vector per
          // per-instance WMO doodad, so N doodads became N buckets and N VK draws. Group them by
          // model and feed one bucket each after the loop: same instances, same relative order within
          // a model, a fraction of the calls on both sides. Pure-GL frames (vk_feeding false) keep the
          // original inline draw untouched.
          // [finding 95] ONE map, one hash per doodad. These were three maps keyed by the same
          // Model*, so each doodad hashed the same pointer three times to append three values.
          struct PiBucket
          {
            std::vector<glm::mat4x4> tf;
            std::vector<glm::vec4> intr;
            std::vector<ModelInstance*> inst;
          };
          static std::unordered_map<Model*, PiBucket> pi;
          if (vk_feeding)
          {
            for (auto& e : pi) { e.second.tf.clear(); e.second.intr.clear(); e.second.inst.clear(); }
          }
          _id_t1 = std::chrono::steady_clock::now();
          for (ModelInstance* _dptr : per_instance_wmo_doodads)
          {
            if (!_dptr) { continue; } // defensive: cache pointers are never null in practice
            ModelInstance& doodad = *_dptr;
            Model* pmodel = doodad.model.get();
            if (!pmodel || !pmodel->finishedLoading() || pmodel->loading_failed()
                || (!draw_hidden_models && pmodel->is_hidden()))
            {
              continue;
            }
            std::uint64_t const key = wmo_doodad_placement_key(doodad.get_pos());
            if (!seen_doodad_keys.insert(key).second)
            {
              continue; // duplicate placement already drawn this frame
            }
            if (draw_model_animations)
            {
              pmodel->animcalc = false;
            }
            {
              noggit::perf::Scoped _prof_submit2(noggit::perf::Phase::SubmitIndiv);
              // [VULKAN phase D] hand this per-instance WMO doodad to Vulkan -- now accumulated into
              // its model's bucket and fed once after the loop (see the flush below).
              if (vk_feeding)
              {
                glm::mat4x4 const tf = doodad.transformMatrix();
                PiBucket& b = pi[pmodel];   // one hash for all three appends
                b.tf.push_back(tf);
                // same source as before: the transform's translation, NOT get_pos()
                b.intr.push_back(interior_light_at(glm::vec3(tf[3])));
                b.inst.push_back(&doodad);
              }
              else
              pmodel->renderer()->draw(model_view
                , doodad
                , doodad_shader
                , doodad_render_state
                , frustum
                , _cull_distance
                , camera_pos
                , static_cast<int>(_world->model_animtime)
                , display
                , /*no_cull*/ false
                , /*bloom_mask_only*/ false
                , interior_light_at(doodad.get_pos())
              );
            }
            ++_world->_n_rendered_objects;

            if (draw_model_animations && !pmodel->_particles.empty())
            {
              pmodel->swapInstanceEmitterState(key);
              float pdt = _world->models_emitter_dt();
              while (pdt > 0.1f)
              {
                pmodel->updateParticleSystems(0.1f);
                pdt -= 0.1f;
              }
              pmodel->updateParticleSystems(pdt);
              pmodel->swapInstanceEmitterState(key);
            }
          }
          // one bucket per MODEL instead of one per doodad. Anything Vulkan refuses still gets its
          // GL draw, per instance, exactly as the inline path did.
          _id_t2 = std::chrono::steady_clock::now();
          if (vk_feeding)
          {
            for (auto& e : pi)
            {
              if (e.second.tf.empty()) continue;
              Model* const pm = e.first;
              bool const took = vkFeedClassicBucket(pm, e.second.tf, &e.second.intr);
              if (vk_owns_m2 && took) continue;
              if (vk_owns_m2)
              {
                // [PIPELINE step 1] deferred to the render thread, same as the bucket path.
                // On the slice_dist question: this section does not set it, but _m2_program's value
                // IS set every frame by the shadow pass (L7530, slice_dist = 0), which runs before
                // the replay -- so the inline draw and the replay see the same 0. The earlier "+30
                // pixel regression" was inside the check's own +/-30 band (finding 57).
                for (ModelInstance* mi : e.second.inst)
                  _vk_gl_deferred_pi.push_back({pm, mi, interior_light_at(mi->get_pos())});
                continue;
              }
              for (ModelInstance* mi : e.second.inst)
                pm->renderer()->draw(model_view, *mi, doodad_shader, doodad_render_state, frustum,
                                     _cull_distance, camera_pos,
                                     static_cast<int>(_world->model_animtime), display,
                                     /*no_cull*/ false, /*bloom_mask_only*/ false,
                                     interior_light_at(mi->get_pos()));
            }
          }
          {
            using ms = std::chrono::duration<double, std::milli>;
            auto const _id_t3 = std::chrono::steady_clock::now();
            s_id_head  += ms(_id_t1 - _id_t0).count();
            s_id_loop  += ms(_id_t2 - _id_t1).count();
            s_id_flush += ms(_id_t3 - _id_t2).count();
            if (++s_id_frames >= 300u)
            {
              LogError << "[VK] INDIV split/frame: head=" << (s_id_head / s_id_frames)
                       << " bucketLoop=" << (s_id_loop / s_id_frames)
                       << " vkFlush=" << (s_id_flush / s_id_frames)
                       << " ms  doodads=" << per_instance_wmo_doodads.size()
                       << " models=" << pi.size() << std::endl;
              s_id_head = s_id_loop = s_id_flush = 0.0;
              s_id_frames = 0;
            }
          }
        }

        // INSTANCED billboard-doodad path (perf 2026-07-20): default ON (disable with
        // NOGGIT_NO_INSTANCED_DOODADS). Batches each model's billboard doodads into ONE instanced draw per
        // interior group instead of one draw per doodad. Each doodad's billboard bones are animated serially
        // (per-instance, into a big bone buffer) exactly as the individual path did; the instanced m2 program
        // reads them via gl_InstanceID*per_instance_bone_stride. The per-copy particle sim is advanced per
        // doodad here too, so the particle DRAW pass below renders each flame's own state.
        if (!per_instance_wmo_doodads.empty() && s_inst_doodads)
        {
          OpenGL::Scoped::use_program m2_shader {*_m2_instanced_program.get()};

          OpenGL::M2RenderState doodad_render_state;
          doodad_render_state.tex_arrays = {0, 0};
          doodad_render_state.tex_indices = {0, 0};
          doodad_render_state.tex_unit_lookups = {0, 0};
          gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
          gl.disable(GL_BLEND);
          gl.depthMask(GL_TRUE);
          gl.enable(GL_CULL_FACE);
          m2_shader.uniform("blend_mode", 0);
          m2_shader.uniform("unfogged", static_cast<int>(doodad_render_state.unfogged));
          m2_shader.uniform("unlit",  static_cast<int>(doodad_render_state.unlit));
          m2_shader.uniform("tex_unit_lookup_1", 0);
          m2_shader.uniform("tex_unit_lookup_2", 0);
          m2_shader.uniform("pixel_shader", 0);

          std::unordered_map<Model*, std::size_t> pib_boxes; // unused (all_boxes = false)

          // [GL perf 2026-08-08] PibPrep CACHE. The dedupe/group/transform/interior prep (~1.7ms) only
          // changes when the VISIBLE pib set changes; a cheap pointer-XOR signature over the per-frame
          // vector (stable cache-node pointers) detects that. Standing/editing = pure cache hits; flying
          // rebuilds. Hard-invalidated with the pi-doodad cache on WMO-fingerprint change (Model* lifetime).
          std::optional<noggit::perf::Scoped> _prof_pprep;
          _prof_pprep.emplace(noggit::perf::Phase::PibPrep);
          unsigned long long pib_sig = 1469598103934665603ull ^ per_instance_wmo_doodads.size();
          for (ModelInstance* _dptr : per_instance_wmo_doodads)
            pib_sig ^= (reinterpret_cast<std::uintptr_t>(_dptr) * 1099511628211ull);
          pib_sig ^= draw_hidden_models ? 0x9E3779B97F4A7C15ull : 0ull;
          // [perf 2026-08-19] Fold the texture-upload epoch into the signature, exactly like the
          // drawDoodadsBatched cache (~line 835): when a model finishes its async load its textures upload and
          // the epoch bumps, invalidating this cache so the now-loaded model's doodads get re-gathered. This
          // closes the old "a group built while a model was still loading excludes it FOREVER" gap that had
          // kept the whole prep opt-in and rebuilding every frame (~2.3ms in heavy city scenes).
          pib_sig = (pib_sig ^ g_texture_upload_epoch.load(std::memory_order_relaxed)) * 1099511628211ull;
          // With the epoch in the signature the cache is safe to default ON in GAME VIEW: WMO doodads are
          // position-static and their baked interior light is time-invariant, so a cache hit reproduces the
          // gather exactly. In the EDITOR a user can toggle a model's hidden flag (not captured by the
          // signature), so keep rebuilding every frame there. NOGGIT_PIB_PREP_CACHE forces it on for A/B.
          static bool const s_pib_prep_cache_env = std::getenv("NOGGIT_PIB_PREP_CACHE") != nullptr;
          bool const pib_prep_cache_on = s_pib_prep_cache_env || _world->_game_character_visible;
          if (!pib_prep_cache_on || !_pib_groups_valid || pib_sig != _pib_groups_sig)
          {
            _pib_groups_cache.clear();
            // Group visible (deduped-by-placement) doodads by model.
            std::unordered_map<Model*, std::vector<ModelInstance*>> pib_by_model;
            {
              std::unordered_set<std::uint64_t> seen_doodad_keys;
              for (ModelInstance* _dptr : per_instance_wmo_doodads)
              {
                if (!_dptr) { continue; }
                ModelInstance& doodad = *_dptr;
                Model* pmodel = doodad.model.get();
                if (!pmodel || !pmodel->finishedLoading() || pmodel->loading_failed()
                    || (!draw_hidden_models && pmodel->is_hidden()))
                {
                  continue;
                }
                std::uint64_t const key = wmo_doodad_placement_key(doodad.get_pos());
                if (!seen_doodad_keys.insert(key).second) { continue; }
                pib_by_model[pmodel].push_back(_dptr);
              }
            }

            // [2026 MODERNIZATION -- parallel bone-animate across models] Distinct Model objects per group ->
            // each animates on its own core (see the bake below). interior_light_at writes a shared position
            // cache, so interiors + transforms are gathered SERIALLY here; results are POSITION-static, which
            // is what makes this whole prep cacheable across frames.
            _pib_groups_cache.reserve(pib_by_model.size());
            for (auto& mg : pib_by_model)
            {
              PibGroup g;
              g.pmodel = mg.first;
              g.has_bones = mg.first->animBones && mg.first->bone_matrices.size() > 0;
              g.transforms.reserve(mg.second.size());
              g.interiors.reserve(mg.second.size());
              g.keys.reserve(mg.second.size());
              for (ModelInstance* _dptr : mg.second)
              {
                g.transforms.push_back(_dptr->transformMatrix());
                g.interiors.push_back(interior_light_at(_dptr->get_pos()));
                g.keys.push_back(wmo_doodad_placement_key(_dptr->get_pos()));
              }
              g.doodads = nullptr; // prep-only view; never dereferenced after this point
              _pib_groups_cache.push_back(std::move(g));
            }
            _pib_groups_sig = pib_sig;
            _pib_groups_valid = true;
          }
          std::vector<PibGroup>& pib_groups = _pib_groups_cache; // downstream bake/draw code unchanged
          for (auto const& g : pib_groups)
            _world->_n_rendered_objects += static_cast<int>(g.transforms.size());
          _prof_pprep.reset(); // PibPrep ends here; the bake below is SubmitIndiv, the draws PibDrawGL

          // Parallel across models: the expensive animate() + per-model particle sim. Each group is a UNIQUE
          // Model, so no two workers touch the same Model state, and nothing global is written here.
          {
            noggit::perf::Scoped _prof_submit2(noggit::perf::Phase::SubmitIndiv);
            auto const animate_group = [&](PibGroup& g)
            {
              Model* const pmodel = g.pmodel;
              g.big_bones.clear(); // groups persist across frames now (PibPrep cache) -- bones are per-frame
              if (g.has_bones) { g.big_bones.reserve(g.transforms.size() * pmodel->bone_matrices.size()); }
              for (std::size_t di = 0; di < g.transforms.size(); ++di)
              {
                if (g.has_bones)
                {
                  if (draw_model_animations) { pmodel->animcalc = false; }
                  // upload_bones=false: compute bones on this worker thread but DON'T touch GL. The bake
                  // copies bone_matrices into big_bones below; the per-model TBO is unused by the instanced
                  // draw, so its upload was both redundant AND the GL-on-worker-thread crash (SIGABRT).
                  pmodel->animate(model_view * g.transforms[di], 0, static_cast<int>(_world->model_animtime), false);
                  g.big_bones.insert(g.big_bones.end(), pmodel->bone_matrices.begin(), pmodel->bone_matrices.end());
                }
                if (draw_model_animations && !pmodel->_particles.empty())
                {
                  pmodel->swapInstanceEmitterState(g.keys[di]);
                  float pdt = _world->models_emitter_dt();
                  while (pdt > 0.1f) { pmodel->updateParticleSystems(0.1f); pdt -= 0.1f; }
                  pmodel->updateParticleSystems(pdt);
                  pmodel->swapInstanceEmitterState(g.keys[di]);
                }
              }
            };

            // DEFAULT PARALLEL (2026-07-24): the SIGABRT was animate() doing a GL bone-TBO upload on the
            // worker thread -- NOT "shared state beyond per-Model". Fixed by the upload_bones=false split
            // (compute bones on the worker, touch no GL; the instanced draw uses big_bones, not the per-model
            // TBO). User-validated at Karazhan: no crash. Opt OUT with NOGGIT_NO_PARALLEL_PIB_ANIMATE=1.
            static bool const s_parallel_pib = std::getenv("NOGGIT_NO_PARALLEL_PIB_ANIMATE") == nullptr;
            std::size_t const ng = pib_groups.size();
            // [threading 2026-08-18] persistent pool instead of per-frame std::thread spawns.
            auto* const tp = s_parallel_pib ? noggit::render_pool() : nullptr;
            if (tp)
            {
              tp->parallel_for(ng, [&](std::size_t gi) { animate_group(pib_groups[gi]); });
            }
            else
            {
              for (auto& g : pib_groups) { animate_group(g); }
            }
          }

          // [pib-MDI 2026-08-07] Batch what classifies into a few MDI calls (per-instance bone blocks in the
          // pib SSBO); the serial per-group fallback below draws only what didn't. PibDrawGL times BOTH, so
          // the win shows directly in the same phase (was ~7.5ms of serial per-group draws).
          {
            noggit::perf::Scoped _prof_pdraw(noggit::perf::Phase::PibDrawGL);
            // [FALLS-DIAG 2026-08-08] NOGGIT_NO_PIB_MDI=1 skips the pib batch -> every group draws via
            // the per-group fallback below (prepareDraw path). Path-isolation bisect switch.
            static bool const s_no_pib_mdi = std::getenv("NOGGIT_NO_PIB_MDI") != nullptr;
            if (!s_no_pib_mdi)
            {
              drawPibBatched(pib_groups);
              vkM2SnapshotAppend();
            }
            for (auto& g : pib_groups)
            {
              if (g.batched || g.transforms.empty()) { continue; }
              std::vector<float> const fades(g.transforms.size(), 1.0f);
              std::vector<glm::mat4x4> const no_bones;
              g.pmodel->renderer()->draw(model_view, g.transforms, m2_shader, doodad_render_state, frustum,
                  _cull_distance, camera_pos, static_cast<int>(_world->model_animtime), false, pib_boxes,
                  display, /*no_cull*/ false, /*representative*/ nullptr, g.interiors, fades,
                  g.has_bones ? g.big_bones : no_bones);
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

    // [game mode] the Game View player character rides the per-instance creature path (own animation
    // pose, skin, attachments, blob shadow) -- independent of the creature-spawns overlay toggle.
    if (!minimap_render && _world->_game_character_visible && _world->_game_character.model_instance.has_value())
    {
      auto& char_mi = *_world->_game_character.model_instance;
      if (char_mi.model->finishedLoading() && !char_mi.model->loading_failed()
          && char_mi.isInFrustum(frustum))
      {
        creature_spawn_instances_to_draw.push_back(
          {_world->_game_character.guid, &char_mi, &_world->_game_character, 1.0f});
      }
    }

    if (!creature_spawn_instances_to_draw.empty())
    {
      // Times the whole near/individual creature-draw region (shadow blobs + mesh + attachments).
      noggit::perf::Scoped _prof_cre(noggit::perf::Phase::M2Creatures);
      // Transparency ordering: draw OPAQUE creatures first, then TRANSLUCENT ones back-to-front. A
      // translucent creature (Anomalus, ghosts) blends against whatever colour is already in the
      // framebuffer, and it lays a depth prepass -- so an opaque creature standing BEHIND it must be
      // drawn FIRST or it gets depth-rejected and you see the background through the ghost instead of
      // the creature behind it. (Ley-Watcher Incantagos behind Anomalus.) The particle pass below
      // reuses this same ordering. Opaque order is left untouched (stable).
      std::stable_sort(creature_spawn_instances_to_draw.begin(), creature_spawn_instances_to_draw.end(),
        [&camera_pos](CreatureSpawnInstanceDraw const& a, CreatureSpawnInstanceDraw const& b)
        {
          bool const a_translucent = a.instance && a.instance->model_alpha < 0.999f;
          bool const b_translucent = b.instance && b.instance->model_alpha < 0.999f;
          if (a_translucent != b_translucent)
          {
            return !a_translucent; // opaque before translucent
          }
          if (a_translucent && b_translucent)
          {
            float const da = a.instance ? glm::distance(camera_pos, a.instance->get_pos()) : 0.0f;
            float const db = b.instance ? glm::distance(camera_pos, b.instance->get_pos()) : 0.0f;
            return da > db; // farther first (back-to-front)
          }
          return false; // both opaque: keep original order
        });

      // Unit blob shadows, CLIENT-EXACT (wow.exe 5875 @006d7920/@006d7480, RE'd): the REAL
      // Textures\ShadowBlob.blp decal (32x32 grayscale, white rim -> ~0.63 gray centre, 1-bit
      // alpha oval), drawn MODULATE (dst * src) over the terrain. The quad is the unit's model
      // bbox FOOTPRINT (x/y extents clamped to +-5 like the client) transformed by the instance
      // matrix -- the iconic 1.12 oval that elongates with the creature and turns with its facing.
      // Doodads don't get this (they use baked MCSH). Drawn first so creature meshes sit on top;
      // depth-tested against terrain with depth-write off. (Client refinement not replicated:
      // terrain-triangle projection + height-fade ramp; ours is a flat decal at foot height.)
      // Client rule (wow335a.exe FUN_007e49e0): the blob decal draws ONLY at extShadowQuality < 1 --
      // at >=1 the dynamic unit shadow map replaces it.
      if (_shadow_quality < 1)
      {
        if (!_shadow_blob_texture)
        {
          _shadow_blob_texture = std::make_unique<scoped_blp_texture_reference>(
            "textures/ShadowBlob.blp", _world->getRenderContext());
        }
        if ((*_shadow_blob_texture)->finishedLoading() && !(*_shadow_blob_texture)->loading_failed()
            && snapshotDecalDepth())
        {
          GLint vp[4] = {0, 0, 0, 0};
          gl.getIntegerv(GL_VIEWPORT, vp);

          OpenGL::Scoped::use_program blob_shader {*_blob_shadow_program.get()};
          OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const cull;
          OpenGL::Scoped::bool_setter<GL_DEPTH_TEST, GL_FALSE> const no_depth_test;
          OpenGL::Scoped::depth_mask_setter<GL_FALSE> const depth_mask;
          gl.enable(GL_BLEND);
          gl.blendFunc(GL_DST_COLOR, GL_ZERO); // modulate, like the client
          blob_shader.uniform("model_view_projection", mvp);
          blob_shader.uniform("inv_view_projection", glm::inverse(mvp));
          blob_shader.uniform("inv_viewport", glm::vec2(1.0f / std::max(vp[2], 1),
                                                        1.0f / std::max(vp[3], 1)));
          gl.activeTexture(GL_TEXTURE0);
          (*_shadow_blob_texture)->bind();
          blob_shader.uniform("shadow_tex", 0);
          blob_shader.uniform("tex_index", (*_shadow_blob_texture)->array_index());
          gl.activeTexture(GL_TEXTURE1);
          gl.bindTexture(GL_TEXTURE_2D, _decal_depth_tex);
          blob_shader.uniform("scene_depth", 1);
          gl.activeTexture(GL_TEXTURE0);
          gl.bindVertexArray(_bloom_vao); // reuse an empty VAO (corners come from gl_VertexID)
          for (auto const& draw_item : creature_spawn_instances_to_draw)
          {
            ModelInstance* instance = draw_item.instance;
            if (!instance || !instance->model.get() || instance->model->loading_failed()
                || !instance->model->finishedLoading())
            {
              continue;
            }
            if (!draw_hidden_models && instance->model->is_hidden())
            {
              continue;
            }
            glm::vec3 const p = instance->get_pos();
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            {
              continue;
            }

            // Footprint = the STAND SEQUENCE's bounds box x/y clamped to +-5. CLIENT-EXACT source
            // (@00711a20): the client walks the sequence table and uses the CURRENT animation's
            // bounds -- NOT the header bbox, which unions every animation's extremes and oversizes
            // the oval (that mismatch is also why the shadow looked bigger than the selection
            // circle and its dark centre spread too thin). Header bbox stays as the fallback.
            auto const& hdr = instance->model->header;
            glm::vec3 bmin = hdr.bounding_box_min;
            glm::vec3 bmax = hdr.bounding_box_max;
            {
              auto const it = instance->model->_animations_seq_per_id.find(0);
              if (it != instance->model->_animations_seq_per_id.end() && !it->second.empty())
              {
                auto const& seq = it->second.begin()->second;
                glm::vec3 const a = glm::min(seq.boxA, seq.boxB);
                glm::vec3 const b = glm::max(seq.boxA, seq.boxB);
                if (b.x > a.x && b.y > a.y)
                {
                  bmin = a;
                  bmax = b;
                }
              }
            }
            float x0 = std::max(bmin.x, -5.0f);
            float x1 = std::min(bmax.x,  5.0f);
            float y0 = std::max(bmin.y, -5.0f);
            float y1 = std::min(bmax.y,  5.0f);
            if (!(x1 > x0) || !(y1 > y0)
                || !std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(y0) || !std::isfinite(y1))
            {
              // degenerate bbox: square footprint from size_cat, like the old fallback
              float foot = instance->size_cat * 0.35f;
              if (!std::isfinite(foot) || foot <= 0.0f) { foot = 2.0f; }
              float const half = glm::clamp(foot, 0.6f, 14.0f) / std::max(instance->scale, 0.001f);
              x0 = y0 = -half;
              x1 = y1 =  half;
            }
            glm::mat4 const tr = instance->transformMatrix();
            glm::mat3 const rot(tr);
            glm::vec3 const c_m(0.5f * (x0 + x1), 0.0f, -0.5f * (y0 + y1));
            glm::vec3 axis_x = rot * glm::vec3(0.5f * (x1 - x0), 0.0f, 0.0f);
            glm::vec3 axis_y = rot * glm::vec3(0.0f, 0.0f, -0.5f * (y1 - y0));
            glm::vec3 center = glm::vec3(tr * glm::vec4(c_m, 1.0f));
            axis_x.y = 0.0f;
            axis_y.y = 0.0f;
            center.y = p.y; // foot plane; the decal projects onto the actual ground around it

            // vertical fade range: size-proportional like the client's ramp; the coverage quad is
            // expanded so sloped ground within that range still falls inside it on screen
            float const half_x = glm::length(axis_x);
            float const half_y = glm::length(axis_y);
            float const v_range = std::max(std::max(half_x, half_y), 0.5f);
            glm::vec2 const expand(1.0f + v_range / std::max(half_x, 0.001f),
                                   1.0f + v_range / std::max(half_y, 0.001f));

            blob_shader.uniform("center", center);
            blob_shader.uniform("axis_x", axis_x);
            blob_shader.uniform("axis_y", axis_y);
            blob_shader.uniform("expand", expand);
            blob_shader.uniform("v_range", v_range);
            // shadow fades with its owner (modulates toward white = no darkening)
            blob_shader.uniform("fade", draw_item.fade);
            gl.drawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);
          }
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

      // [2026-07-24] PARALLEL CREATURE BONE-ANIMATE (DEFAULT ON; opt out NOGGIT_NO_PARALLEL_CREATURE_ANIMATE).
      // The dense-city AnimateCPU cost is per-instance creature bone computation on the main thread. Thread
      // it BY MODEL (each worker owns ALL instances of one Model -> disjoint bones[]/bone_matrices, no
      // cross-worker clobber of per-Model state) with upload_bones=false (compute only, no GL on the worker;
      // see the animate() CPU/GL split). FIRST CUT: only NON-mounted, NON-particle creatures are pre-computed
      // -- mounted NPCs need live mount bones for rider seating, and particle creatures need per-instance
      // emitter setup at draw time; both fall through to the serial draw below. The draw loop restores each
      // precomputed set into bone_matrices + draws with skip_animate=true (upload + draw, no recompute).
      static bool const s_parallel_creature = std::getenv("NOGGIT_NO_PARALLEL_CREATURE_ANIMATE") == nullptr;
      std::size_t const n_creature_items = creature_spawn_instances_to_draw.size();
      std::vector<std::vector<glm::mat4x4>> creature_precomputed_bones;
      std::vector<char> creature_bones_ready;
      if (s_parallel_creature && draw_model_animations && n_creature_items > 1)
      {
        // Time the whole pre-pass (grouping + worker compute + join) into AnimateCPU so the profile shows
        // the TRUE main-thread animate cost (parallelised) vs the old serial 5-10ms -- else it hides in WorldDraw.
        noggit::perf::Scoped _prof_ca(noggit::perf::Phase::AnimateCPU);
        creature_precomputed_bones.assign(n_creature_items, {});
        creature_bones_ready.assign(n_creature_items, 0);
        std::unordered_map<Model*, std::vector<std::size_t>> creature_by_model;
        for (std::size_t i = 0; i < n_creature_items; ++i)
        {
          auto const& di = creature_spawn_instances_to_draw[i];
          auto* inst = di.instance;
          if (!inst || inst->model->loading_failed()) { continue; }
          if (_creature_body_batched.count(inst)) { continue; } // body already animated + drawn by the MDI batch
          Model* m = inst->model.get();
          if (!m->animated || !m->animBones) { continue; }                   // nothing to compute
          if (di.spawn && di.spawn->mount_instance.has_value()) { continue; } // mount: serial (seat bones)
          if (!m->_particles.empty()) { continue; }                          // particle: serial (emitter)
          if (!draw_hidden_models && m->is_hidden()) { continue; }
          creature_by_model[m].push_back(i);
        }
        std::vector<std::pair<Model*, std::vector<std::size_t>>> creature_groups(
            creature_by_model.begin(), creature_by_model.end());
        auto const animate_creature_group = [&](std::pair<Model*, std::vector<std::size_t>>& grp)
        {
          Model* const m = grp.first;
          for (std::size_t idx : grp.second)
          {
            auto const& di = creature_spawn_instances_to_draw[idx];
            auto* inst = di.instance;
            int const c_animtime = di.spawn
              ? static_cast<int>(_world->model_animtime) + di.spawn->animation_time_offset
              : static_cast<int>(_world->model_animtime);
            int anim_id = inst->forcedAnimationId() >= 0 ? inst->forcedAnimationId() : 0;
            if (anim_id != 0 && !m->hasAnimationId(anim_id)) { anim_id = 0; }
            m->_hand_overlay_active_main = inst->closeHandMain();
            m->_hand_overlay_active_off = inst->closeHandOff();
            m->_lower_body_twist = inst->lower_body_twist;
            m->_anim_time_scale = inst->anim_time_scale;
            m->_active_idle_key = static_cast<std::uint64_t>(inst->uid);
            m->animcalc = false;
            m->animate(model_view * inst->transformMatrix(), anim_id, c_animtime, /*upload_bones=*/false);
            creature_precomputed_bones[idx] = m->bone_matrices; // per-instance copy of the computed result
            inst->_animation_bones = m->bone_matrices;          // client-parity: unit OWNS its bones (shadow/attachment read this)
            creature_bones_ready[idx] = 1;
          }
        };
        std::size_t const ng = creature_groups.size();
        // [threading 2026-08-18] Persistent worker pool instead of spawning + joining a fresh std::thread
        // vector EVERY frame (the ~50-100us/thread Windows spawn churn the old code warned about, on a
        // 24-thread CPU that noggit barely used). Work unit = one creature GROUP (a model's instances):
        // same-model instances stay serial inside the group (animate() writes shared Model state), distinct
        // models run in parallel across the pool + the calling thread. NOGGIT_NO_THREAD_POOL=1 -> serial.
        if (auto* tp = noggit::render_pool())
        {
          tp->parallel_for(ng, [&](std::size_t gi) { animate_creature_group(creature_groups[gi]); });
        }
        else
        {
          for (auto& g : creature_groups) { animate_creature_group(g); }
        }
      }

      for (std::size_t _ci = 0; _ci < n_creature_items; ++_ci)
      {
        auto const& draw_item = creature_spawn_instances_to_draw[_ci];
        auto* instance = draw_item.instance;
        if (!instance || instance->model->loading_failed())
        {
          continue;
        }
        // [creature body MDI 2026-08-18] this body was drawn (and animated) by the per-instance-bone MDI batch
        // above -> skip its body draw + particle sim here, but STILL draw its mount/attachments below and
        // restore its own pose into the shared bone buffer so attachment placement + shadow seat correctly.
        bool const body_batched = _creature_body_batched.count(instance) != 0;

        if (draw_hidden_models || !instance->model->is_hidden())
        {
          if (capture_debug_enabled())
          {
            LogDebug << "Creature spawn model draw guid=" << draw_item.guid
                     << " model='" << instance->model->file_key().stringRepr() << "'"
                     << " pos={" << instance->pos.x << ", " << instance->pos.y << ", " << instance->pos.z << "}"
                     << " scale=" << instance->scale
                     << " modelAlpha=" << instance->model_alpha
                     << " tint={" << instance->model_tint.x << ", " << instance->model_tint.y << ", " << instance->model_tint.z << "}"
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

          // One fade for the whole unit (computed once at gather, carried on the draw item): the
          // body AND every attached model (helmet, shoulders, weapon, aura kits) share this alpha.
          float const creature_fade = draw_item.fade;

          // MOUNT: a mounted NPC (creature_addon.mount_display_id) rides a mount model. Draw the mount
          // at the spawn's ground position, then re-seat the rider onto the mount's MountMain point
          // (attachment 0) so it sits on the saddle instead of standing inside the mount. Drawing the
          // mount first populates its animated bone_matrices, which the seat position reads.
          if (draw_item.spawn && draw_item.spawn->mount_instance.has_value())
          {
            auto& mount = *draw_item.spawn->mount_instance;
            Model* mount_model = mount.model.get();
            if (mount_model && mount_model->finishedLoading() && !mount_model->loading_failed()
                && (draw_hidden_models || !mount_model->is_hidden()))
            {
              if (draw_model_animations)
              {
                mount_model->animcalc = false;
              }
              // GHOST PARITY (user 2026-08-25): the mount renders as part of the unit -- one
              // translucency/tint for the whole render (body, gear, mount), so a ghost rider's
              // mount is ghosted too instead of staying solid under a see-through rider.
              mount.model_alpha = instance->model_alpha;
              mount.model_tint = instance->model_tint;
              mount_model->renderer()->draw(model_view, mount, m2_shader, model_render_state, frustum,
                _cull_distance, camera_pos, creature_animtime, display, /*no_cull*/ true, // GPU-clip, no bbox snap
                /*bloom_mask_only*/ false, interior_light_at(mount.get_pos(), false), creature_fade);
              ++_world->_n_rendered_objects;

              // World seat = mount transform * (mount bone matrix * fixCoordSystem(attachment.pos)).
              // Keep the rider's OWN scale; take the mount's position + facing.
              if (auto const* seat = find_attachment_def(mount_model, 0))
              {
                glm::mat4x4 att = glm::translate(glm::mat4x4(1.0f), fixCoordSystem(seat->pos));
                if (seat->bone >= 0
                    && static_cast<std::size_t>(seat->bone) < mount_model->bone_matrices.size())
                {
                  att = mount_model->bone_matrices[seat->bone] * att;
                }
                glm::vec4 const seat_world = mount.transformMatrix() * att * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                instance->pos = glm::vec3(seat_world);
                instance->dir = mount.dir;
                instance->updateTransformMatrix();
              }
            }
          }

          if (!body_batched)
          {
          // Parallel-animate: if this instance's bones were pre-computed on a worker thread, restore them
          // and draw with skip_animate (upload + draw, no main-thread recompute). Else the serial path.
          bool const use_precomputed_bones = _ci < creature_bones_ready.size() && creature_bones_ready[_ci];
          if (use_precomputed_bones)
          {
            instance->model->bone_matrices = creature_precomputed_bones[_ci];
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
            // [fix 2026-08-18] no_cull=TRUE: the body's bounding-box frustum cull SNAPS the whole body off the
            // instant its box crosses the screen edge, even while part of the body -- or an attachment offset
            // from it (weapon/helm/shoulder, drawn no_cull) -- is still on screen. Let the GPU clip the body
            // per-triangle instead so it slides off smoothly WITH its gear. Off-screen bodies are already
            // bounded away by the gather's 64yd frustum pre-cull, so this adds no unbounded draw work.
            , /*no_cull*/ true
            , /*bloom_mask_only*/ false
            , interior_light_at(instance->get_pos(), false) // moving (creature/attachment) -> no cache
            , creature_fade
            , /*skip_animate*/ use_precomputed_bones
          );
          ++_world->_n_rendered_objects;

          // Client-parity per-instance bones: the pre-pass already stored _animation_bones for the
          // parallelised majority. For instances it SKIPPED (mounts with seat bones, particle
          // creatures, or the whole serial path) the in-draw animate() above just left THIS unit's
          // pose in the shared model buffer -- capture it onto the instance now so the shadow pass and
          // attachment placement can read the unit's OWN pose (not the next-drawn instance's). One
          // per-instance palette, computed once, read by both passes -- the client's model.
          if (!use_precomputed_bones && instance->model->animated
              && !instance->model->bone_matrices.empty())
          {
            instance->_animation_bones = instance->model->bone_matrices;
          }
          }
          else
          {
            // [creature body MDI] the body was drawn by the per-instance-bone MDI batch above. Restore THIS
            // instance's captured pose into the shared model bone buffer so the attachment placement below
            // (reads parent_model->bone_matrices[attachment_def->bone]) and the shadow pass seat on the RIGHT
            // pose -- not whichever instance the batch's parallel animate wrote last.
            if (instance->model->animated && !instance->_animation_bones.empty()
                && instance->_animation_bones.size() == instance->model->bone_matrices.size())
            {
              instance->model->bone_matrices = instance->_animation_bones;
            }
          }

          // Advance THIS spawn's OWN particle simulation now, while animate() (inside draw() above) has the
          // shared bones + emitter setup at this instance's animation phase. Each spawn keeps its own live
          // particle state (keyed by guid) so its particles stay glued to its own animated body instead of
          // matching only the last-drawn spawn (the shared/global state still updates in tick() for
          // doodads/preview). Gated on animations so particles freeze in place when animations are off.
          // Batched bodies are particle-less by predicate, so skip (they never ran the body draw/animate here).
          if (!body_batched && draw_model_animations && draw_item.spawn)
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

              // Jitter-free anchor: recompute the attachment's world matrix in DOUBLE and split it into
              // the parent's STATIC world origin + a small RELATIVE transform. The attachment follows the
              // animated parent bone, so its world position moves a little each frame; baking that into a
              // float matrix at ~17000 world scale quantizes the motion to the ~0.002 grid = dithering
              // helmets/shoulders/weapons. Double preserves the small animated offset; the shader then
              // renders it camera-relative. (The float attachment_world_matrix above still feeds particles.)
              {
                glm::dmat4x4 att_d = glm::translate(glm::dmat4x4(1.0), glm::dvec3(fixCoordSystem(attachment_def->pos)));
                if (attachment_def->bone >= 0
                    && static_cast<std::size_t>(attachment_def->bone) < parent_model->bone_matrices.size())
                {
                  att_d = glm::dmat4x4(parent_model->bone_matrices[attachment_def->bone]) * att_d;
                }
                glm::dmat4x4 const parent_d(instance->transformMatrix());
                glm::dmat4x4 const world_d = parent_d * att_d;
                glm::dvec3 const origin_d(parent_d[3]);
                glm::mat4x4 rel(world_d);
                rel[3] = glm::vec4(glm::vec3(glm::dvec3(world_d[3]) - origin_d), 1.0f);
                attachment_instance.setRenderAnchor(glm::vec3(origin_d), rel);
              }
              // Particle transform: models whose emitters ride their parent (flag 0x10) use the full
              // animated attachment matrix. NON-riding emitters (aura sparkles, BREATH BUBBLES) are
              // TRUE world-space in the client: bake the emitter's current placement into each
              // particle at SPAWN and draw with IDENTITY -- live particles stay put in the world
              // (bubbles trail behind a swimming character) instead of riding the instance matrix.
              if (attachment_instance.model->particlesRideParent())
              {
                attachment_instance.model->clearWorldSpaceParticleEmission();
                attachment.particle_transform = attachment_instance.transformMatrix();
              }
              else
              {
                attachment_instance.model->setWorldSpaceParticleEmission(
                  instance->transformMatrix()
                  * glm::translate(glm::mat4x4(1.0f), fixCoordSystem(attachment_def->pos)),
                  attachment.particle_kill_plane_y);
                attachment.particle_transform = glm::mat4x4(1.0f);
              }
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
              // GHOST PARITY (user 2026-08-25: "ghost effect on npcs not applying to equipment"):
              // worn equipment (helm/shoulders/weapons) follows the UNIT's translucency -- copy the
              // body's CreatureModelAlpha x aura alpha and char-proc tint onto the attachment every
              // frame (auras/visuals can change live). Aura state-kit models stay exempt
              // (client-verified 2026-08-22: Anomalus body alpha 200, its sparkles full-opacity).
              attachment_instance.model_alpha = attachment.is_aura_kit ? 1.0f : instance->model_alpha;
              attachment_instance.model_tint = attachment.is_aura_kit ? glm::vec3(1.0f) : instance->model_tint;
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
                , /*bloom_mask_only*/ false
                // Attachments (weapons, shoulders, helmets) share their OWNER's light: without this they
                // defaulted to the outdoor sun and glowed on a dark interior body (visible by day only).
                , interior_light_at(instance->get_pos(), false) // moving (creature/attachment) -> no cache
                // ...and their OWNER's cull fade capped by their OWN stream-in fade: item models
                // load async AFTER the body, and without their own 2 s ramp they popped in at the
                // body's current mid-fade alpha instead of fading in from zero.
                , std::min(creature_fade, cull_fade_alpha(&attachment_instance, true, 1.0f))
              );
              ++_world->_n_rendered_objects;

              // Capture this attachment's own bones (animated weapons/effect models) so the shadow pass
              // can redraw it in the SAME pose. Static equipment (no bones) leaves this empty and the
              // shadow draw treats it as rigid.
              if (attachment_instance.model->animated
                  && !attachment_instance.model->bone_matrices.empty())
              {
                attachment_instance._animation_bones = attachment_instance.model->bone_matrices;
              }

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
    // DEFENSIVE (2026-08-14): the detail-doodad grass path toggles GL_SAMPLE_ALPHA_TO_COVERAGE via
    // state tracking and can leave it ENABLED if grass was the last M2 drawn. Now that the grass
    // shader emits REAL sub-1 coverage (the mesh_color.a-vs-texture-alpha fix), a leaked A2C can
    // punch coverage holes into the shadow depth render + subsequent passes = the intermittent
    // "black world". Force it off before the shadow pass and keep it off for the rest of the frame;
    // the grass path re-enables it per-draw as needed.
    gl.disable(GL_SAMPLE_ALPHA_TO_COVERAGE);

    // ---- 3.3.5a dynamic UNIT shadow map (client extShadowQuality >= 1, RE doc 35) ----
    // Renders this frame's visible units (creatures + the game character; the list above) into a
    // directional depth map from the scene light; terrain/WMO/M2 receivers PCF-sample it NEXT frame
    // (1-frame latency by design -- the receiver draws already happened this frame). Level 0 = off
    // (blob decals above, the client's own level-0 behaviour). Environmental casters (WMO/doodads,
    // client levels 3-5 -- e.g. the Stormwind bank door) are the next stage; levels 3-5 currently
    // raise resolution/range only.
    if (_shadow_quality >= 1 && !minimap_render && display == display_mode::in_3D
        && (!creature_spawn_instances_to_draw.empty() || _shadow_quality >= 3))
    {
      // CLIENT-EXACT sizes (wow335a.exe FUN_00875d30 @0x875d32: size = 0x400; quality>3 or ==2 ->
      // 0x800): 1024 at levels 1/3, 2048 at 2/4/5 (level 5 = same 2048 rings, round-robin CSM).
      int const shadow_size = (_shadow_quality == 2 || _shadow_quality >= 4) ? 2048 : 1024;
      // CLIENT-SPLIT maps: UNIT bubble (client 20yd around the PLAYER, DAT_009f281c; the flying
      // editor camera gets 60yd) + ENVIRONMENTAL ring (middle of the client's 40/160/640 cascade,
      // DAT_00b1d520) at levels >= 3. Separate maps so a unit's shadow MULTIPLIES on top of a
      // building's (the client projects each map as its own Mod pass, FUN_00875f80/FUN_00874fb0).
      bool const have_game_char =
          _world->_game_character_visible && _world->_game_character.model_instance.has_value();
      glm::vec3 const unit_center = have_game_char
          ? _world->_game_character.model_instance->get_pos() : camera_pos;
      float const unit_range = have_game_char ? 20.0f : 60.0f;
      float const env_range = 160.0f;
      bool const do_env = _shadow_quality >= 3;
      ensureShadowTarget(shadow_size);

      // Toward-sun in the POSITION frame (vary_position/f_position/m2_world_pos). dayDir is the wow-
      // space travel direction; position frame = (-wow.y, wow.z, -wow.x) (terrain_frag spec note), so
      // toward-sun = (dayDir.y, -dayDir.z, dayDir.x).
      glm::vec3 day = _outdoor_light_stats.dayDir;
      // CLIENT shadow-sun STEEPEN (wow.exe FUN_007bb570 @0x7bb570, the shadow driver): the 3.3.5
      // shadow light is NOT the raw scene light. The client scales the light direction's VERTICAL
      // component x5 and clamps it to >= -1.2, THEN renormalizes -- lifting the sun so shadows are
      // steeper/shorter than the flat 1.12 scene light. RE-proven against
      // wow_cap_stormwind_shadows_130pm.trace: at 1:30pm the traced shadow/lighting sun basis is
      // ~46 deg elevation, while the flat scene-light law (FUN_007eea90, phi 110-127 @ az 225 --
      // which noggit copies into dayDir) sits at only ~33 deg. Constants are exact:
      // _DAT_009ebf34 = 5.0 (vertical scale), _DAT_00a400fc = -1.2 (floor). dayDir.z = cos(phi) is
      // the wow-frame vertical (points down for a sun above, so it is negative; x5 then clamp makes
      // it steeper). Without this, our shadows tracked the flat light and never matched the game.
      day.z = std::max(day.z * 5.0f, -1.2f);
      float const day_len = glm::length(day);
      day = day_len > 1e-4f ? day / day_len : glm::vec3(0.0f, -1.0f, 0.0f);
      glm::vec3 const to_sun = glm::normalize(glm::vec3(day.y, -day.z, day.x));
      glm::vec3 const up = (std::abs(to_sun.y) > 0.98f) ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);

      // Build a TEXEL-SNAPPED light VP (client-exact mechanism, FUN_00875f80: the projected offset
      // is floor(size*x+0.5)/size -- the map translates in whole-texel steps as the center moves, so
      // shadow edges don't crawl/"chainsaw" while moving). World-anchored: snap the world origin's
      // light-view position to the texel grid.
      auto const build_snapped_vp = [&](glm::vec3 const& center, float range) -> glm::mat4
      {
        float const eye_dist = range * 2.0f + 200.0f;
        glm::mat4 const lv = glm::lookAt(center + to_sun * eye_dist, center, up);
        glm::mat4 const lp = glm::ortho(-range, range, -range, range, 1.0f, eye_dist + range * 2.0f);
        float const texel = (2.0f * range) / static_cast<float>(shadow_size);
        glm::vec4 const o_lv = lv * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        glm::vec3 const snap(std::fmod(o_lv.x, texel), std::fmod(o_lv.y, texel), 0.0f);
        return lp * glm::translate(glm::mat4(1.0f), -snap) * lv;
      };
      glm::mat4 const shadow_vp = build_snapped_vp(unit_center, unit_range);
      glm::mat4 const shadow_env_vp = build_snapped_vp(camera_pos, env_range);

      // m2_vert computes vertex = view_rot * (world - cam), gl_Position = projection * vertex. With
      // projection := shadow_vp * T(cam) * view_rotT the SAME vertex path lands in light clip space --
      // bones, billboards and camera-relative precision all reused untouched.
      glm::mat4 view_rot4 = model_view;
      view_rot4[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
      glm::mat4 const cam_unview = glm::translate(glm::mat4(1.0f), camera_pos) * glm::transpose(view_rot4);
      glm::mat4 const shadow_proj_override = shadow_vp * cam_unview;
      glm::mat4 const shadow_env_proj_override = shadow_env_vp * cam_unview;
      math::frustum const light_frustum {shadow_vp};
      math::frustum const light_env_frustum {shadow_env_vp};

      GLint prev_fbo = 0;
      GLint prev_vp[4] = {0, 0, 0, 0};
      gl.getIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
      gl.getIntegerv(GL_VIEWPORT, prev_vp);

      // FEEDBACK-LOOP GUARD (GL spec): the shadow textures must NOT stay bound for sampling on
      // units 16/17 while they are this pass's depth attachments -- undefined behaviour, and the
      // observed driver-dependent "world turns black" corruption. Unbind for the pass, rebind after.
      gl.activeTexture(GL_TEXTURE0 + 16);
      gl.bindTexture(GL_TEXTURE_2D, 0);
      gl.activeTexture(GL_TEXTURE0 + 17);
      gl.bindTexture(GL_TEXTURE_2D, 0);
      gl.activeTexture(GL_TEXTURE0);

      gl.bindFramebuffer(GL_FRAMEBUFFER, _shadow_fbo);
      gl.viewport(0, 0, shadow_size, shadow_size);
      gl.clear(GL_DEPTH_BUFFER_BIT);
      gl.enable(GL_POLYGON_OFFSET_FILL);
      gl.polygonOffset(2.0f, 4.0f); // depth bias against acne (the client biases via hwPCF/lod too)
      gl.disable(GL_CULL_FACE);     // thin/one-sided caster geometry still occludes

      {
        updateMVPUniformBlock(model_view, shadow_proj_override, camera_pos);

        OpenGL::Scoped::use_program shadow_m2_shader {*_m2_program.get()};
        OpenGL::M2RenderState shadow_render_state;
        shadow_render_state.tex_arrays = {0, 0};
        shadow_render_state.tex_indices = {0, 0};
        shadow_render_state.tex_unit_lookups = {0, 0};
        shadow_m2_shader.uniform("blend_mode", 0);
        shadow_m2_shader.uniform("unfogged", 1);
        shadow_m2_shader.uniform("unlit", 1);
        shadow_m2_shader.uniform("tex_unit_lookup_1", 0);
        shadow_m2_shader.uniform("tex_unit_lookup_2", 0);
        shadow_m2_shader.uniform("pixel_shader", 0);
        shadow_m2_shader.uniform("model_origin", glm::vec3(0.0f));
        shadow_m2_shader.uniform("slice_dist", 0.0f);

        for (std::size_t si = 0; si < creature_spawn_instances_to_draw.size(); ++si)
        {
          auto const& draw_item = creature_spawn_instances_to_draw[si];
          ModelInstance* instance = draw_item.instance;
          if (!instance || !instance->model.get() || instance->model->loading_failed()
              || !instance->model->finishedLoading())
          {
            continue;
          }
          if (!draw_hidden_models && instance->model->is_hidden())
          {
            continue;
          }
          // [perf 2026-08-15] CULL casters outside the UNIT shadow box. The unit map only covers ~20yd
          // around the player, but creature_spawn_instances_to_draw holds every creature visible in the
          // MAIN camera -- in a crowded city that's dozens of NPCs, nearly all of them outside the box.
          // We were submitting them ALL (body + equipment + per-instance bone restore) every frame; the
          // light-frustum vertex clip then threw the geometry away = pure wasted CPU submission. An
          // explicit AABB-vs-light-frustum test skips them before any of that work. (Game view sits the
          // camera in the crowd, so this is where its extra shadow cost came from.)
          if (!instance->isInFrustum(light_frustum))
          {
            continue;
          }
          int const c_animtime = draw_item.spawn
            ? static_cast<int>(_world->model_animtime) + draw_item.spawn->animation_time_offset
            : static_cast<int>(_world->model_animtime);
          Model* const parent_model = instance->model.get();
          // CLIENT-PARITY: draw the shadow from THIS unit's OWN per-instance bone palette (filled during
          // the per-frame animate; the client stores bones on the instance and both the color pass and
          // this depth pass read them -- it never separately animates shadows). Restoring the instance
          // palette makes the shadow byte-identical to the on-screen silhouette: idle NPCs stay idle, the
          // running player runs. Fallback to an in-draw re-animate only when the palette is empty (unit
          // main-pass culled but inside the light frustum, or a non-per-instance static model).
          bool const has_bones = instance->_animation_bones.size() == parent_model->bone_matrices.size()
                                 && !instance->_animation_bones.empty() && parent_model->animated;
          if (has_bones)
          {
            parent_model->bone_matrices = instance->_animation_bones;
          }
          parent_model->renderer()->draw(model_view, *instance, shadow_m2_shader,
                                            shadow_render_state, light_frustum,
                                            100000.0f /*no dist cull; the light frustum culls*/,
                                            camera_pos, c_animtime,
                                            display, true /*no_cull: light frustum differs*/,
                                            false, glm::vec4(0.0f), 1.0f, has_bones /*skip_animate: use palette*/);

          // EQUIPMENT casts shadow too (helmet / shoulders / weapon / aura kits). Each attachment is
          // placed on the PARENT's bone at its attach point (parent_model->bone_matrices, just restored
          // to this unit's pose above) exactly like the main pass, then drawn from its OWN captured
          // palette. Mirrors the color-pass attachment block so the shadow silhouette includes gear.
          if (draw_item.spawn)
          {
            for (auto& attachment : draw_item.spawn->attachment_models)
            {
              if (!attachment.model_instance.has_value()) { continue; }
              auto& att_inst = *attachment.model_instance;
              Model* const att_model = att_inst.model.get();
              if (!att_model || !att_model->finishedLoading() || att_model->loading_failed()
                  || (!draw_hidden_models && att_model->is_hidden()))
              {
                continue;
              }
              auto const* att_def = find_attachment_def(parent_model, attachment.attachment_id);
              if (!att_def && attachment.attachment_id == 34) { att_def = find_attachment_def(parent_model, 15); }
              if (!att_def && attachment.attachment_id == 20) { att_def = find_attachment_def(parent_model, 11); }
              if (!att_def) { continue; }
              att_inst.setTransformMatrix(attachment_world_matrix(*instance, parent_model, att_def));
              bool const att_has_bones = att_inst._animation_bones.size() == att_model->bone_matrices.size()
                                         && !att_inst._animation_bones.empty() && att_model->animated;
              if (att_has_bones)
              {
                att_model->bone_matrices = att_inst._animation_bones;
              }
              att_model->renderer()->draw(model_view, att_inst, shadow_m2_shader,
                                          shadow_render_state, light_frustum,
                                          100000.0f, camera_pos, c_animtime,
                                          display, true /*no_cull*/, false,
                                          glm::vec4(0.0f), 1.0f, att_has_bones /*skip_animate*/);
            }
          }
        }
      }

      // ---- ENVIRONMENTAL casters (client levels 3-5: "Full environmental and PC/NPC shadows") ----
      // The client's SEPARATE env map (its ring cascade): WMOs + the batched M2 doodad buckets
      // depth-only. wmo_vert uses the IDENTICAL camera-relative chain as m2_vert, so the same kind
      // of projection override lands them in light clip space. Separate from the unit map so unit
      // shadows MULTIPLY on top of building shadows like the client's per-map Mod passes.
      if (do_env)
      {
        gl.bindFramebuffer(GL_FRAMEBUFFER, _shadow_env_fbo);
        gl.viewport(0, 0, shadow_size, shadow_size);
        gl.clear(GL_DEPTH_BUFFER_BIT);
        updateMVPUniformBlock(model_view, shadow_env_proj_override, camera_pos);
        if (!wmos_to_draw.empty())
        {
          OpenGL::Scoped::use_program shadow_wmo_shader {*_wmo_program.get()};
          shadow_wmo_shader.uniform("camera", glm::vec3(camera_pos.x, camera_pos.y, camera_pos.z));
          for (auto& winst : wmos_to_draw)
          {
            if (!winst->wmo->finishedLoading() || winst->wmo->loading_failed())
            {
              continue;
            }
            if (!draw_hidden_models && winst->wmo->is_hidden())
            {
              continue;
            }
            winst->draw(shadow_wmo_shader
                , nullptr /*no liquid in the shadow map*/
                , nullptr
                , model_view
                , shadow_env_proj_override
                , light_env_frustum
                , 100000.0f /*no dist cull; the light frustum culls*/
                , camera_pos
                , false /*force_box*/
                , false /*draw_doodads: WMO doodads ride the m2 buckets below*/
                , false /*draw_fog*/
                , {} /*selection*/
                , _world->animtime
                , false /*world_has_skies*/
                , display
                , false /*no_cull: light frustum culls*/
                , true  /*draw_exterior*/
                , nullptr /*world_renderer: no point-light scoping / deferred liquid in this pass*/
            );
          }
        }

        // batched doodads/gameobjects: this frame's visible instance buckets, depth-only re-draw
        {
          OpenGL::Scoped::use_program shadow_m2i {*_m2_instanced_program.get()};
          OpenGL::M2RenderState env_state;
          env_state.tex_arrays = {0, 0};
          env_state.tex_indices = {0, 0};
          env_state.tex_unit_lookups = {0, 0};
          shadow_m2i.uniform("blend_mode", 0);
          shadow_m2i.uniform("unfogged", 1);
          shadow_m2i.uniform("unlit", 1);
          shadow_m2i.uniform("tex_unit_lookup_1", 0);
          shadow_m2i.uniform("tex_unit_lookup_2", 0);
          shadow_m2i.uniform("pixel_shader", 0);
          shadow_m2i.uniform("model_origin", glm::vec3(0.0f));
          shadow_m2i.uniform("slice_dist", 0.0f);
          std::vector<glm::vec4> env_interior;
          std::vector<float> env_fades;
          for (auto& pair : models_to_draw)
          {
            Model* m = pair.first;
            if (!m || !m->finishedLoading() || m->loading_failed() || pair.second.empty())
            {
              continue;
            }
            if (!draw_hidden_models && m->is_hidden())
            {
              continue;
            }
            env_interior.assign(pair.second.size(), glm::vec4(0.0f));
            env_fades.assign(pair.second.size(), 1.0f);
            m->renderer()->draw(model_view, pair.second, shadow_m2i, env_state, light_env_frustum,
                                100000.0f /*no dist cull*/, camera_pos,
                                static_cast<int>(_world->model_animtime),
                                false /*boxes*/, model_boxes_to_draw, display,
                                false /*no_cull: light frustum culls*/,
                                nullptr /*representative*/, env_interior, env_fades);
          }
        }
      }

      updateMVPUniformBlock(model_view, projection, camera_pos); // restore the camera matrices

      gl.disable(GL_POLYGON_OFFSET_FILL);
      gl.enable(GL_CULL_FACE);
      gl.bindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
      gl.viewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);

      // safe to re-bind for the remaining shadow-sampling draws this frame (fading GOs etc.);
      // the maps are no longer the active render targets
      gl.activeTexture(GL_TEXTURE0 + 16);
      gl.bindTexture(GL_TEXTURE_2D, _shadow_tex);
      gl.activeTexture(GL_TEXTURE0 + 17);
      gl.bindTexture(GL_TEXTURE_2D, _shadow_env_tex);
      gl.activeTexture(GL_TEXTURE0);

      // world -> [0,1]^3 for next frame's receiver sampling
      glm::mat4 const bias(0.5f, 0.0f, 0.0f, 0.0f,
                           0.0f, 0.5f, 0.0f, 0.0f,
                           0.0f, 0.0f, 0.5f, 0.0f,
                           0.5f, 0.5f, 0.5f, 1.0f);
      _shadow_matrix = bias * shadow_vp;
      _shadow_center_range = glm::vec4(unit_center, unit_range);
      _shadow_env_matrix = bias * shadow_env_vp;
      _shadow_env_center_range = glm::vec4(camera_pos, env_range);
      _shadow_env_valid = do_env;
      _shadow_map_valid = true;
    }
    else
    {
      // pass skipped (level 0, minimap, non-3D display or no units) -> next frame must not sample
      // a stale map (departed units would leave frozen shadows)
      _shadow_map_valid = false;
      _shadow_env_valid = false;
    }

    // FADING gameobjects: the same individual body draw creatures get, so gameobject fades are
    // EXACTLY the creature fade -- continuous alpha (no instanced quantization), depth prepass,
    // identical blend promotion and bloom-mask handling.
    if (!go_fading_individual.empty())
    {
      OpenGL::Scoped::use_program go_m2_shader {*_m2_program.get()};

      OpenGL::M2RenderState go_render_state;
      go_render_state.tex_arrays = {0, 0};
      go_render_state.tex_indices = {0, 0};
      go_render_state.tex_unit_lookups = {0, 0};
      gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      gl.disable(GL_BLEND);
      gl.depthMask(GL_TRUE);
      gl.enable(GL_CULL_FACE);
      go_m2_shader.uniform("blend_mode", 0);
      go_m2_shader.uniform("unfogged", static_cast<int>(go_render_state.unfogged));
      go_m2_shader.uniform("unlit",  static_cast<int>(go_render_state.unlit));
      go_m2_shader.uniform("tex_unit_lookup_1", 0);
      go_m2_shader.uniform("tex_unit_lookup_2", 0);
      go_m2_shader.uniform("pixel_shader", 0);

      for (auto const& go_item : go_fading_individual)
      {
        ModelInstance* go_instance = go_item.first;
        if (!go_instance->model.get() || !go_instance->model->finishedLoading()
            || go_instance->model->loading_failed()
            || (!draw_hidden_models && go_instance->model->is_hidden()))
        {
          continue;
        }
        if (draw_model_animations)
        {
          go_instance->model->animcalc = false;
        }
        go_instance->model->renderer()->draw(model_view
          , *go_instance
          , go_m2_shader
          , go_render_state
          , frustum
          , _cull_distance
          , camera_pos
          , static_cast<int>(_world->model_animtime)
          , display
          , /*no_cull*/ false
          , /*bloom_mask_only*/ false
          , interior_light_at(go_instance->get_pos())
          , go_item.second
        );
        ++_world->_n_rendered_objects;
      }

      gl.disable(GL_BLEND);
      gl.enable(GL_CULL_FACE);
      gl.depthMask(GL_TRUE);
    }

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

    // Opacity of the selection/state discs drawn under creature + gameobject spawns. These are an
    // editor overlay, not client art, so they are held back from full strength to keep the model
    // readable underneath. 0.8 = 20% less opaque than the authored colours.
    constexpr float spawn_marker_opacity = 0.8f;

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

      struct MarkerData { std::uint32_t guid; glm::vec4 color; glm::vec3 pos; float radius; };
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
        if (spawn.event_suppressed) // hidden by the Seasonal Events filter
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
        // Only show the selection circle when the creature is actually within its RENDER range
        // (the same knob the creature model uses) -- previously it used a separate 300yd marker
        // distance, so circles appeared for creatures that weren't even being drawn.
        if (distance > creature_spawn_model_distance) continue;
        ++nearby_spawns;

        // Game-exact circle size: server bounding radius x creature scale (what the client renders
        // via UNIT_FIELD_BOUNDINGRADIUS); falls back to the model footprint when the DB lacks it.
        float const ring_radius = spawn.selectionRingWorldRadius();

        // Base color = the creature's HOSTILITY toward players, resolved from its faction via
        // FactionTemplate.dbc (calibrated: mask bit 0x1 = all-players, 0x2 = Alliance, 0x4 = Horde):
        //   hostile-to-all-players (HostileMask & 0x1) -> RED, friendly-to-any-player-faction
        //   (FriendlyMask & 0x7) -> GREEN, otherwise -> YELLOW (neutral). Non-partisan (both
        //   Alliance and Horde town NPCs read friendly; only mobs hostile to everyone read red).
        // Cached per faction id so we don't walk the DBC every spawn every frame.
        glm::vec4 reaction_color(1.0f, 1.0f, 0.0f, 1.0f); // neutral yellow (also the fallback)
        {
          auto const it = _faction_reaction_cache.find(spawn.faction);
          if (it != _faction_reaction_cache.end())
          {
            reaction_color = it->second;
          }
          else
          {
            glm::vec4 c(1.0f, 1.0f, 0.0f, 1.0f);
            if (spawn.faction != 0)
            {
              try
              {
                auto const rec = gFactionTemplateDB.getByID(spawn.faction);
                std::uint32_t const friendly = rec.getUInt(FactionTemplateDB::FriendlyMask);
                std::uint32_t const hostile  = rec.getUInt(FactionTemplateDB::HostileMask);
                // Client reaction colors (UnitReactionColor): pure red / green / yellow.
                if (hostile & 0x1u)        { c = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f); } // RED hostile
                else if (friendly & 0x7u)  { c = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f); } // GREEN friendly
                // else stays neutral yellow (1, 1, 0)
              }
              catch (...) { /* unknown faction template -> neutral */ }
            }
            _faction_reaction_cache.emplace(spawn.faction, c);
            reaction_color = c;
          }
        }

        // Editor states override the hostility color so selection/hover/edit stay obvious.
        glm::vec4 color = spawn.selected ? glm::vec4(1.0f, 0.85f, 0.13f, 1.0f)   // selected: bright yellow
            : spawn.hovered  ? glm::vec4(0.2f, 0.9f,  1.0f,  1.0f)               // hovered: cyan
            : spawn.dirty    ? glm::vec4(0.2f, 1.0f,  0.35f, 1.0f)               // unsaved edit: green
                 : reaction_color;                                              // default: hostility

        // Fade the circle WITH the creature (same cull-fade pipeline): multiply its alpha by the
        // spawn's fade so the ring fades in/out in lockstep with the model instead of popping.
        auto const fade_it = _creature_fade_by_guid.find(spawn.guid);
        if (fade_it != _creature_fade_by_guid.end())
        {
          color.a *= fade_it->second;
        }
        color.a *= spawn_marker_opacity; // editor overlay, kept subtle so it does not fight the model
        markers.push_back({spawn.guid, color, spawn.pos, ring_radius});
      }

      // Wander-distance visualization (creature editor): while the wander_distance field is focused,
      // draw a ground ring of that radius (server yards == world units) around the edited spawn so
      // the editor SEES how far movement_type-1 wandering reaches. Same projected-decal machinery
      // as the selection circles; orange so it never reads as a selection/hostility ring.
      if (_world->wander_viz && _world->wander_viz->radius > 0.01f)
      {
        markers.push_back({0u,
                           glm::vec4(1.0f, 0.55f, 0.1f, 0.9f),
                           _world->wander_viz->center,
                           _world->wander_viz->radius});
      }

      if (capture_debug_enabled())
      {
        LogDebug << "Creature spawn marker draw begin nearby=" << nearby_spawns
                 << " markers=" << markers.size()
                 << " closestGuid=" << closest_guid
                 << " closestDistance=" << closest_distance
                 << std::endl;
      }

      // Selection circle as a SCREEN-SPACE PROJECTED DECAL, identical machinery to the unit blob
      // shadow above (which works): snapshot the scene depth (terrain + WMO + doodads), and for each
      // covered pixel reconstruct its world position and paint UnitSelectTexture's ring onto whatever
      // ground is actually there. This DRAPES over the real WMO mesh (Molten Core floor) with ZERO
      // CPU raycasts -- so it never clips through the floor and never stutters. A flat/conforming
      // vertex mesh can't be both non-clipping on a bumpy floor AND non-stuttering; the decal is.
      // (The client does the fixed-function equivalent -- projective texgen onto the re-drawn ground
      // geometry, RE'd from wow.exe/apitrace; the depth decal is the modern-GL equivalent.)
      // Blend: additive on RGB (SRCALPHA/ONE), leave framebuffer alpha (the bloom emissive mask) alone.
      gl.blendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);

      auto const uv_rotation_for = [&](glm::vec3 const& pos) -> float
      {
        glm::vec2 const to_cam(camera_pos.x - pos.x, camera_pos.z - pos.z);
        return std::atan2(to_cam.x, to_cam.y);
      };

      // DEPTH CORRECTNESS: re-snapshot the depth NOW (creatures + gameobjects are already drawn into
      // the scene framebuffer at this point) instead of reusing the blob-shadow pass's pre-creature
      // snapshot. With the models in the depth buffer, the decal's per-pixel reconstruction lands on
      // the creature SURFACE (high rel.y) over the body -- which the shader's height-band then rejects
      // -- so the ring is occluded by the model and only paints the visible ground around it.
      _decal_depth_ready = false;
      if (snapshotDecalDepth())
      {
        GLint cvp[4] = {0, 0, 0, 0};
        gl.getIntegerv(GL_VIEWPORT, cvp);
        glm::vec2 const inv_vp(1.0f / std::max(cvp[2], 1), 1.0f / std::max(cvp[3], 1));
        // Reconstruct CAMERA-RELATIVE positions in the frag shader (inverse of the rotation-only MVP,
        // NOT the world MVP). At Karazhan's ~19000 world coords, reconstructing the full world position
        // lost float precision, so the ring shook as the camera moved. mvp_rel * v_rel == mvp * world for
        // the same clip point, so the same screen NDC reconstructs to (world - camera) -- small, precise.
        glm::mat4x4 const inv_mvp_rel = glm::inverse(mvp_rel);
        for (auto const& m : markers)
        {
          _circle_render.drawProjectedDecal(mvp, inv_mvp_rel, inv_vp, _decal_depth_tex, _world_depth_tex, _bloom_vao,
                                            m.pos, camera_pos, m.radius, m.color, uv_rotation_for(m.pos));
        }
      }

      // Restore blend func for subsequent passes.
      gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

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

      struct MarkerData { std::uint32_t guid; glm::vec4 color; glm::vec3 pos; float radius; };
      std::vector<MarkerData> markers;
      markers.reserve(256);

      for (auto const& spawn : _world->gameObjectSpawns())
      {
        if (spawn.pending_delete)
          continue;
        if (spawn.event_suppressed) // hidden by the Seasonal Events filter
          continue;
        if (glm::distance(camera_pos, spawn.pos) > creature_spawn_marker_distance)
          continue;

        float ring_radius = 0.5f;
        if (spawn.model_instance.has_value())
        {
          ring_radius = spawn.model_instance.value().selectionRingRadius();
        }
        ring_radius = std::max(0.25f, ring_radius);

        // Distinct palette from creatures (which are orange/green): gameobjects use blue/purple.
        glm::vec4 color = spawn.selected ? glm::vec4(0.35f, 1.0f, 0.45f, 1.0f)
            : spawn.hovered  ? glm::vec4(0.45f, 0.85f, 1.0f, 1.0f)
            : spawn.dirty    ? glm::vec4(1.0f,  0.9f,  0.15f, 1.0f)
                 : glm::vec4(0.55f, 0.45f, 1.0f, 1.0f);
        color.a *= spawn_marker_opacity; // same overlay opacity as the creature markers
        markers.push_back({spawn.guid, color, spawn.pos, ring_radius});
      }

      // Projected-decal circle (see the creature block): painted onto the real terrain/WMO ground via
      // the scene depth, draping over the WMO mesh with no raycasts, no clipping, no stutter.
      gl.blendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);

      auto const uv_rotation_for = [&](glm::vec3 const& pos) -> float
      {
        glm::vec2 const to_cam(camera_pos.x - pos.x, camera_pos.z - pos.z);
        return std::atan2(to_cam.x, to_cam.y);
      };

      if (snapshotDecalDepth())
      {
        GLint gvp[4] = {0, 0, 0, 0};
        gl.getIntegerv(GL_VIEWPORT, gvp);
        glm::vec2 const inv_vp(1.0f / std::max(gvp[2], 1), 1.0f / std::max(gvp[3], 1));
        // Reconstruct CAMERA-RELATIVE positions in the frag shader (inverse of the rotation-only MVP,
        // NOT the world MVP). At Karazhan's ~19000 world coords, reconstructing the full world position
        // lost float precision, so the ring shook as the camera moved. mvp_rel * v_rel == mvp * world for
        // the same clip point, so the same screen NDC reconstructs to (world - camera) -- small, precise.
        glm::mat4x4 const inv_mvp_rel = glm::inverse(mvp_rel);
        for (auto const& m : markers)
        {
          _circle_render.drawProjectedDecal(mvp, inv_mvp_rel, inv_vp, _decal_depth_tex, _world_depth_tex, _bloom_vao,
                                            m.pos, camera_pos, m.radius, m.color, uv_rotation_for(m.pos));
        }
      }
      gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    // Mouse aim circle for the spawn tools, as a PROJECTED DECAL. The normal cursor circle is painted
    // by the TERRAIN shader (mcnk outer_cursor_radius), so it exists only on terrain: over a WMO it
    // vanished / showed through the floor, and you could not see where you were about to place. Same
    // depth-decal machinery as the spawn markers above, so it drapes onto whatever surface is actually
    // under the cursor -- terrain, WMO floor or roof, doodad. Terrain mode keeps its shader circle;
    // this only runs for the two spawn tools.
    if ((terrainMode == editing_mode::creature || terrainMode == editing_mode::gameobject)
        && cursor_type != CursorType::NONE
        && brush_radius > 0.0f)
    {
      ZoneScopedN("World::draw() : Draw spawn-tool cursor decal");
      OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const disable_cull_face;
      OpenGL::Scoped::bool_setter<GL_BLEND, GL_TRUE> const enable_blend;
      OpenGL::Scoped::depth_mask_setter<GL_FALSE> const no_depth_write;

      gl.blendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);

      // Re-snapshot: the spawn models are in the depth buffer by now, so the ring is correctly
      // occluded by whatever it is drawn behind instead of painting over it.
      _decal_depth_ready = false;
      if (snapshotDecalDepth())
      {
        GLint cvp[4] = {0, 0, 0, 0};
        gl.getIntegerv(GL_VIEWPORT, cvp);
        glm::vec2 const inv_vp(1.0f / std::max(cvp[2], 1), 1.0f / std::max(cvp[3], 1));
        glm::mat4x4 const inv_mvp_rel = glm::inverse(mvp_rel);
        glm::vec2 const to_cam(camera_pos.x - cursor_pos.x, camera_pos.z - cursor_pos.z);

        // simple_ring: a plain outline, NOT the client's UnitSelectTexture -- this is an editor aim
        // cursor, so it should not borrow the in-game selection-circle art.
        _circle_render.drawProjectedDecal(mvp, inv_mvp_rel, inv_vp, _decal_depth_tex, _world_depth_tex, _bloom_vao,
                                          cursor_pos, camera_pos, brush_radius, cursor_color,
                                          std::atan2(to_cam.x, to_cam.y), /*simple_ring*/ true);
      }

      gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
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
        OpenGL::Scoped::depth_mask_setter<GL_FALSE> const no_depth_write;
        // Straight alpha on RGB, but leave the framebuffer ALPHA alone -- that channel is the bloom
        // emissive mask, and writing route colour into it would make the paths bloom.
        gl.blendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

        // Ribbon width, three independent knobs. world_width sets the MIDDLE-distance thickness (it
        // is what you see whenever the clamps are not biting); max_pixels is what you see up close,
        // where a fixed world width would otherwise fill the screen; min_pixels is the far-distance
        // floor that keeps a route visible instead of thinning to nothing.
        constexpr float path_world_width = 0.40f; // was 0.6 -- mid distance read too thick
        constexpr float path_min_pixels = 2.0f;
        constexpr float path_max_pixels = 18.0f;  // the close-up width; 14 read too thin zoomed in

        // Editor overlay, not client art: hold the routes back from full strength so the ground and
        // its texture read through them. 0.7 = 30% more transparent than the authored palette.
        constexpr float path_opacity = 0.7f;

        // World units per pixel, per unit of view depth: projection[1][1] is 1 / tan(fovy / 2), so
        // this stays correct if the FOV or the window size changes.
        GLint pvp[4] = {0, 0, 0, 0};
        gl.getIntegerv(GL_VIEWPORT, pvp);
        float const px_scale = 2.0f / (projection[1][1] * static_cast<float>(std::max(pvp[3], 1)));
        glm::vec2 const inv_vp(1.0f / std::max(pvp[2], 1), 1.0f / std::max(pvp[3], 1));
        // Camera forward in world space (third row of the view rotation, negated).
        glm::vec3 const view_axis(-model_view[0][2], -model_view[1][2], -model_view[2][2]);

        // The route is painted as a PROJECTED DECAL onto the scene depth, exactly like the spawn
        // selection circles: a plain 3D line between two waypoints cuts straight through every hill
        // and building in between, whereas the decal reconstructs the surface visible at each pixel
        // and paints the route onto it. So the whole path stays on top of the mesh, and geometry in
        // front of it occludes it for free (its surface simply isn't near the route).
        // Re-snapshot so the creature/gameobject models are in the depth used here.
        _decal_depth_ready = false;
        if (snapshotDecalDepth())
        {
          glm::mat4x4 const inv_mvp_rel = glm::inverse(mvp_rel);

          std::vector<glm::vec3> points;
          std::vector<glm::vec3> waypoint_dots; // every authored waypoint of every drawn route

          for (auto const& spawn : _world->creatureSpawns())
          {
            if (spawn.pending_delete)
              continue;
            if (spawn.event_suppressed) // hidden by the Seasonal Events filter
              continue;
            if (any_selected && !spawn.selected) // only the selected creature's path while one is selected
              continue;
            if (glm::distance(camera_pos, spawn.pos) > path_show_distance)
              continue;

            auto it = paths.find(spawn.guid);
            if (it == paths.end() || it->second.empty())
              continue;

            // Start at the (possibly just-moved) spawn position so the route tracks live edits, then
            // run through the authored waypoints. No vertical lift and no ground probing: the decal
            // takes its height from the surface it lands on, and the shader's depth test is what
            // keeps it off models -- so the raw route is all this pass needs.
            points.clear();
            points.reserve(it->second.size() + 1);
            points.push_back(spawn.pos);
            for (auto const& wp : it->second)
              points.push_back(wp);

            if (points.size() < 2)
              continue;

            glm::vec4 color = palette[spawn.guid % palette_size];
            color.a *= path_opacity;

            _path_decal_render.draw(mvp_rel, inv_mvp_rel, inv_vp, _decal_depth_tex, _world_depth_tex, points,
                                    camera_pos, view_axis, color,
                                    path_world_width, path_min_pixels, path_max_pixels, px_scale);

            // The AUTHORED waypoints only (not the live spawn position): these are the coordinates
            // the creature actually walks between, i.e. what a path editor needs to see.
            waypoint_dots.insert(waypoint_dots.end(), it->second.begin(), it->second.end());
          }

          // Waypoint nodes: one near-black dot per authored coordinate, drawn AFTER every route so
          // the nodes sit on top of their lines. Black contrasts against every palette entry (all
          // bright/saturated, incl. white) and against terrain; sized a bit wider than the ribbon
          // with its own pixel clamps so the nodes read as beads at any distance.
          if (!waypoint_dots.empty())
          {
            constexpr float dot_world_diameter = 0.85f;         // ~2x the ribbon width
            constexpr float dot_min_pixels = 5.0f;
            constexpr float dot_max_pixels = 26.0f;
            glm::vec4 const dot_color(0.03f, 0.03f, 0.03f, 0.92f);
            _path_decal_render.draw_dots(mvp_rel, inv_mvp_rel, inv_vp, _decal_depth_tex, _world_depth_tex,
                                         waypoint_dots, camera_pos, view_axis, dot_color,
                                         dot_world_diameter, dot_min_pixels, dot_max_pixels, px_scale);
          }
        }

        gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
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
    water_shader.uniform("sheen_dir", _skies->celestial_dir()); // glitter tracks the drawn sun/moon disc
    water_shader.uniform("animtime", _world->animtime);
    water_shader.uniform("draw_shadows", _terrain_params_ubo_data.draw_shadows);


    if (draw_wmo || _world->mapIndex.hasAGlobalWMO())
    {
      water_shader.uniform("use_transform", 1);
    }
  }
  // Draw water BEFORE the additive model particles/ribbons and the deferred light effects, so
  // those additive glows paint OVER the (now opaque-in-deep-water) water instead of being hidden.
  // DECOUPLE water opacity from the bloom mask: RGB uses normal src-alpha translucency, but the
  // ALPHA channel (= the scene bloom-emissive mask) is only PULLED DOWN by water coverage
  // (GL_ZERO, GL_ONE_MINUS_SRC_ALPHA), never raised. Without this the water's own opacity leaked into
  // the mask, so deep water had to be capped at 0.85 to avoid blooming -- which left ~15% of the
  // bright green seafloor bleeding through (the "green transparent water"). Now deep water can be
  // fully OPAQUE dark blue (like the 1.12 client) and still never blooms. Matches the particle pass.
  gl.enable(GL_BLEND);
  gl.blendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);

  if (draw_water && !vk_owns_water)
  {
    if (capture_debug_enabled())
    {
      LogDebug << "WorldRender::draw water begin" << std::endl;
    }

    ZoneScopedN("World::draw() : Draw water");
    noggit::perf::Scoped _prof_water(noggit::perf::Phase::Water);

    // [A2C leak fix 2026-08-18] The env shadow-caster pass above re-enables GL_SAMPLE_ALPHA_TO_COVERAGE after
    // the frame's last defensive disable (it draws the grass detail-doodads into the depth FBO), so water --
    // which writes fractional per-depth alpha -- inherits it and shows the same coverage dither. Force it off.
    gl.disable(GL_SAMPLE_ALPHA_TO_COVERAGE);

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

    // Water surface specular (sun sheen): same sun-band colour as the terrain specular; toggle via
    // render/water_specular (default on, like the client's reflective water).
    water_shader.uniform ("camera", glm::vec3(camera_pos.x, camera_pos.y, camera_pos.z));
    water_shader.uniform ("sheen_dir", _skies->celestial_dir());
    water_shader.uniform ("sun_spec_color", _skies->color_set[SUN_COLOR]);
    water_shader.uniform ("draw_water_specular"
                          , _world->_settings->value("render/water_specular", true).toBool() ? 1 : 0);

    for (auto& pair : _world->_loaded_tiles_buffer)
    {
      MapTile* tile = pair.second;

      if (!tile)
        break;

      if (tile->renderer()->isOccluded() && !tile->Water.needsUpdate() && !tile->renderer()->isOverridingOcclusionCulling())
        continue;

      // Same distance cull as the terrain pass -- without it the far tiles' WATER would keep
      // drawing after their ground was culled, leaving lakes/ocean floating in the fog.
      if (!minimap_render && tile->camDist() > _terrain_cull_distance + TILESIZE)
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

  // Flush the WMO liquid queued during the WMO pass (see _deferred_wmo_liquid). Drawn HERE, after the
  // models, so a creature standing in WMO water is blended over by the surface -- same ordering ADT
  // water already had. Inherits the water pass's blendFuncSeparate, which keeps water opacity out of
  // the bloom mask.
  if (draw_water && !vk_owns_water && !_deferred_wmo_liquid.empty() && _wmo_liquid_program)
  {
    ZoneScopedN("World::draw() : Draw deferred WMO water");
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const cull;
    OpenGL::Scoped::use_program wmo_liquid_shader{*_wmo_liquid_program.get()};
    // User-directed sun/moon glitter on exterior WMO water (doc 37): the lobe needs the view origin.
    wmo_liquid_shader.uniform("camera", glm::vec3(camera_pos.x, camera_pos.y, camera_pos.z));
    wmo_liquid_shader.uniform("sheen_dir", _skies->celestial_dir());

    wmo_liquid_shader.uniform("water_alpha_mult",
                              _world->_settings->value("water/transparency", 1.0f).toFloat());

    bool const dedupe_overlap = _world->_settings->value("water/wmo_stencil", true).toBool();

    for (auto const& entry : _deferred_wmo_liquid)
    {
      // Stencil de-dupe for overlapping EXTERIOR planes only, exactly as the inline path did.
      bool const stencil = dedupe_overlap && !entry.interior_only;
      if (stencil)
      {
        gl.enable(GL_STENCIL_TEST);
        gl.stencilFunc(GL_NOTEQUAL, 1, 0xFF);
        gl.stencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
      }

      for (auto* group : entry.groups)
      {
        group->drawLiquid(entry.transform, wmo_liquid_shader, _liquid_texture_manager,
                          entry.draw_fog, _world->animtime,
                          /*translucent*/ !entry.interior_only);
      }

      if (stencil)
      {
        gl.disable(GL_STENCIL_TEST);
      }
    }

    _deferred_wmo_liquid.clear();
  }

  // Weather precipitation (rain streaks / snow flakes) around the camera -- the visible half of the
  // editor weather; the light/fog half is the STORM-param blend in Sky::colorFor. Drawn after the
  // water so drops overlay it like everything else.
  // INTERIOR SUPPRESSION (user 2026-08-26): no precipitation while the camera stands in a TRUE
  // indoor WMO group -- the client kills weather rendering inside buildings. Exterior city groups
  // and open ext-lit channels keep raining, and (client parity) rain still falls through OUTDOOR
  // overhangs/bridges -- vanilla has no per-particle roof test.
  if (_world->weather_type != 0 && _world->weather_intensity > 0.0f
      && !(_camera_inside_wmo && _camera_in_indoor_group))
  {
    glm::vec3 const precip_light = _skies->color_set[LIGHT_GLOBAL_AMBIENT] * 0.75f
                                 + _skies->color_set[LIGHT_GLOBAL_DIFFUSE] * 0.45f;
    // RAIN-DIAG (one-shot, 2026-08-27 "rain color is black"): name the light inputs feeding the
    // precip tint so a zeroed band vs a texture/draw problem is decidable from one session.
    {
      static bool s_rain_diag = false;
      if (!s_rain_diag)
      {
        s_rain_diag = true;
        glm::vec3 const amb = _skies->color_set[LIGHT_GLOBAL_AMBIENT];
        glm::vec3 const dif = _skies->color_set[LIGHT_GLOBAL_DIFFUSE];
        LogError << "RAIN-DIAG type=" << _world->weather_type
                 << " intensity=" << _world->weather_intensity
                 << " amb=(" << amb.x << "," << amb.y << "," << amb.z << ")"
                 << " dif=(" << dif.x << "," << dif.y << "," << dif.z << ")"
                 << " precip=(" << precip_light.x << "," << precip_light.y << "," << precip_light.z << ")"
                 << std::endl;
      }
    }
    _weather_effect.draw(mvp, camera_pos, _world->weather_type, _world->weather_intensity,
                         _world->animtime, precip_light, _world->_context);
  }

  // Surface splash/wake ripples (client Water0Ripple port): drain the game-tick spawn requests
  // into the pool and draw the live rings flat on the water, additive, after the surface.
  // RIPPLE-DIAG (one-shot): proves the DRAIN stage receives spawns.
  {
    static bool s_ripple_drain_diag = false;
    if (!s_ripple_drain_diag && !_world->pending_ripples.empty())
    {
      s_ripple_drain_diag = true;
      LogError << "RIPPLE-DIAG drain received " << _world->pending_ripples.size()
               << " spawn(s) this frame" << std::endl;
    }
  }
  for (auto const& spawn_request : _world->pending_ripples)
  {
    _water_ripples.spawn(spawn_request.pos, spawn_request.rot, spawn_request.size0,
                         spawn_request.growth, spawn_request.lifetime_s, spawn_request.alpha_peak,
                         spawn_request.kind);
  }
  _world->pending_ripples.clear();
  _water_ripples.draw(mvp, _world->animtime, _world->_context);

  // Underwater particulates -- the client's waterParticulates motes (Textures\WaterPoop02.blp).
  // CLIENT RE 2026-08-25 (doc 37, FUN_006809c0/0066fd50/0068efe0 chain): the motes run under
  // EVERY liquid -- the tick gate is only `liquidType != 0xf`. Per-liquid look = a FAMILY row of
  // sheet cells (DAT_0086a0a0): water/ocean {0..7}, MAGMA {9,10,11,12}x2, SLIME {0x8}. The old
  // "no motes under lava/slime" note was an unverified assumption (user caught it).
  if (_camera_underwater)
  {
    glm::vec3 const mote_light = _skies->color_set[LIGHT_GLOBAL_AMBIENT] * 0.8f
                               + _skies->color_set[LIGHT_GLOBAL_DIFFUSE] * 0.3f;
    _underwater_motes.draw(mvp, camera_pos, 3, 1.0f, _world->animtime, mote_light, _world->_context,
                           _camera_liquid_family);
  }
  else
  {
    _underwater_motes.draw(mvp, camera_pos, 0, 0.0f, _world->animtime, glm::vec3(0.0f), _world->_context);
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
    m2_shader.uniform("model_origin", glm::vec3(0.0f)); // instanced: transform carries full world pos

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

  // Deferred PER-INSTANCE light-shaft doodads (lighthouse beam & co): individual draws with each
  // instance's own (billboarded) transform, after every water pass -- see the partition above the
  // per-instance doodad paths.
  if (!deferred_pi_light_doodads.empty())
  {
    OpenGL::Scoped::use_program pi_light_shader {*_m2_program.get()};
    OpenGL::M2RenderState pi_light_state;
    pi_light_state.tex_arrays = {0, 0};
    pi_light_state.tex_indices = {0, 0};
    pi_light_state.tex_unit_lookups = {0, 0};
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.disable(GL_BLEND);
    gl.depthMask(GL_TRUE);
    gl.enable(GL_CULL_FACE);
    pi_light_shader.uniform("blend_mode", 0);
    pi_light_shader.uniform("unfogged", static_cast<int>(pi_light_state.unfogged));
    pi_light_shader.uniform("unlit", static_cast<int>(pi_light_state.unlit));
    pi_light_shader.uniform("tex_unit_lookup_1", 0);
    pi_light_shader.uniform("tex_unit_lookup_2", 0);
    pi_light_shader.uniform("pixel_shader", 0);
    std::unordered_set<std::uint64_t> seen_light_keys;
    for (ModelInstance* _dptr : deferred_pi_light_doodads)
    {
      if (!_dptr)
      {
        continue;
      }
      ModelInstance& doodad = *_dptr;
      Model* pmodel = doodad.model.get();
      if (!pmodel || !pmodel->finishedLoading() || pmodel->loading_failed()
          || (!draw_hidden_models && pmodel->is_hidden()))
      {
        continue;
      }
      if (!seen_light_keys.insert(wmo_doodad_placement_key(doodad.get_pos())).second)
      {
        continue; // duplicate cross-tile placement
      }
      if (draw_model_animations)
      {
        pmodel->animcalc = false;
      }
      pmodel->renderer()->draw(model_view
        , doodad
        , pi_light_shader
        , pi_light_state
        , frustum
        , _cull_distance
        , camera_pos
        , static_cast<int>(_world->model_animtime)
        , display
        , /*no_cull*/ false
        , /*bloom_mask_only*/ false
        , interior_light_at(doodad.get_pos())
      );
      ++_world->_n_rendered_objects;
    }
    gl.disable(GL_BLEND);
    gl.enable(GL_CULL_FACE);
    gl.depthMask(GL_TRUE);
  }

  // GEOMETRY-MODEL particle collector (checklist 12.2): path -> world transforms, one per live
  // particle, filled inside each placement's live-state swap below and drawn after the phase.
  std::unordered_map<std::string, std::vector<glm::mat4x4>> geometry_particle_draws;

  // model particles (drawn after water so additive glows appear on top of the water surface).
  // Drawn even when animations are off -> particles freeze in place (not advanced) instead of vanishing.
  if (!model_with_particles.empty() || !creature_spawn_instances_to_draw.empty())
  {
    noggit::perf::Scoped _prof_part(noggit::perf::Phase::M2Particles);
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const cull;
    OpenGL::Scoped::bool_setter<GL_DEPTH_TEST, GL_TRUE> const depth_test;
    OpenGL::Scoped::depth_mask_setter<GL_FALSE> const depth_mask;

    OpenGL::Scoped::use_program particles_shader {*_m2_particles_program.get()};

    particles_shader.uniform("model_view_projection", mvp_rel); // camera-relative (subtracts camera in-shader)
    particles_shader.uniform("camera", camera_pos); // per-particle fog distance AND the camera-relative origin
    OpenGL::texture::set_active_texture(0);
    particles_shader.uniform("tex", 0);

    // [PERF 2026-07-24] Particle draw-distance cull. Particles are small; the client draws them only up
    // close, but noggit simulated + drew EVERY emitter across a whole city -- Stormwind's hundreds of
    // torches/fountains cost 5-11ms (M2Particles). Skip emitters past NOGGIT_PARTICLE_DIST yards (default
    // 250; a flame is ~1px there), applied in BOTH the threaded sim and the main-thread draw below.
    static float const s_particle_dist2 = [] { char const* e = std::getenv("NOGGIT_PARTICLE_DIST");
      float const d = e ? static_cast<float>(std::atof(e)) : 250.0f; return d * d; }();
    auto const particle_too_far = [&](glm::vec3 const& p) -> bool
    { glm::vec3 const d = p - camera_pos; return glm::dot(d, d) > s_particle_dist2; };

    // Particles OWN the bloom mask under themselves now (alpha write ON). Each particle's blend uses
    // glBlendFuncSeparate (see Particle.cpp) so its RGB is unchanged but its alpha MULTIPLIES the mask by
    // (1 - coverage): a bright additive fire erases the emissive mask it would otherwise inherit from the
    // lava pit behind it, so it stops blooming through the strong emissive path (the Ironforge forge /
    // firepit plumes were blowing out to a white screen-filling halo). The lava around the flame keeps
    // its mask and still glows; faint particle edges barely touch it. Particles are never themselves
    // emissive, so erasing (never raising) the mask is the data-faithful behaviour.
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    particles_shader.uniform("particle_alpha_mod", 1.0f); // batched world particles: full opacity (no creature fade)
    // Per-instance doodad particle sims: each placement swaps in its OWN live state (keyed by its
    // static position), advances it, and draws with its own transform. Independent clouds = no
    // clone stacking (the IF forge white-column root cause) and every placement emits like the
    // client (all four forge pots steam at once).

    // [doc 23 #3, M2UseThreads] Threaded particle SIM pre-pass. The 3.3.5a client offloads the CPU particle
    // simulation to a worker (the draw stays on the main thread). We thread it BY MODEL: each worker owns a
    // DISJOINT set of models, so no two threads ever touch the same model's shared _particles / instance
    // state -> no race (the shared-scratch hazard that parked the older per-instance attempt). frand() is
    // thread-local and updateParticleSystems() touches no async-loader/GL state, so per-model sim is safe.
    // Each instance's advanced state is stored back; the draw loop below then only swaps-in + draws (its own
    // update is gated off by threaded_sim). Default ON (2026-07-23, user-confirmed correct + no crash; the
    // particle-heavy M2Particles dropped ~8->~5ms). Set NOGGIT_NO_PARALLEL_PARTICLE_SIM=1 to disable.
    static bool const s_parallel_particle_sim = std::getenv("NOGGIT_NO_PARALLEL_PARTICLE_SIM") == nullptr;
    bool const threaded_sim = s_parallel_particle_sim && draw_model_animations && !model_with_particles.empty();
    if (threaded_sim)
    {
      // NOTE: no perf::Scoped here -- this threaded sim is already inside the outer M2Particles scope
      // (_prof_part); adding one double-counted the sim into M2Particles. [2026-07-25]
      std::vector<Model*> sim_models;
      sim_models.reserve(model_with_particles.size());
      for (auto& mp : model_with_particles)
      {
        if (!mp.first->_particles.empty()) { sim_models.push_back(mp.first); }
      }
      float const sim_dt = _world->models_emitter_dt();
      auto sim_range = [&](std::size_t rbegin, std::size_t rend)
      {
        auto mix = [](std::uint64_t h, std::int64_t v)
        { return h ^ (static_cast<std::uint64_t>(v) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2)); };
        for (std::size_t si = rbegin; si < rend; ++si)
        {
          Model* pmodel = sim_models[si];
          auto const mit = model_with_particles.find(pmodel);
          if (mit == model_with_particles.end()) { continue; }
          std::unordered_set<std::uint64_t> seen;
          for (auto const& transform : mit->second)
          {
            glm::vec3 const inst_pos(transform[3]);
            if (particle_too_far(inst_pos)) { continue; } // [PERF] skip the sim for far emitters too
            std::uint64_t key = 0x517CC1B727220A95ull;
            key = mix(key, static_cast<std::int64_t>(std::llround(inst_pos.x * 8.0f)));
            key = mix(key, static_cast<std::int64_t>(std::llround(inst_pos.y * 8.0f)));
            key = mix(key, static_cast<std::int64_t>(std::llround(inst_pos.z * 8.0f)));
            if (!seen.insert(key).second) { continue; }
            pmodel->swapInstanceEmitterState(key);
            float adt = sim_dt;
            while (adt > 0.1f) { pmodel->updateParticleSystems(0.1f); adt -= 0.1f; }
            pmodel->updateParticleSystems(adt);
            pmodel->swapInstanceEmitterState(key); // store the advanced state for the draw pass
          }
        }
      };
      std::size_t const n = sim_models.size();
      unsigned const hw = std::max(1u, std::thread::hardware_concurrency());
      std::size_t const workers = std::min<std::size_t>(hw, n);
      if (workers <= 1)
      {
        sim_range(0, n);
      }
      else
      {
        // [threading 2026-08-18] persistent pool instead of per-frame std::thread spawns; each of the
        // `workers` contiguous ranges is one work item.
        std::size_t const per = (n + workers - 1) / workers;
        if (auto* tp = noggit::render_pool())
        {
          tp->parallel_for(workers, [&](std::size_t w)
          {
            std::size_t const b = w * per, e = std::min(n, b + per);
            if (b < e) { sim_range(b, e); }
          });
        }
        else
        {
          sim_range(0, n);
        }
      }
    }

    {
      float const emitter_frame_dt = _world->models_emitter_dt();
      for (auto& it : model_with_particles)
      {
        Model* pmodel = it.first;
        if (pmodel->_particles.empty())
        {
          continue; // ribbon-only models handled below
        }
        // The instance list contains DUPLICATE transforms (WMO doodads get injected once per tile
        // the WMO spans -- the [GLSTATE] probe showed instances=7 for 5 placements). Without this
        // dedupe the SAME placement is updated and drawn several times per frame -- the same
        // additive cloud stamped on itself = the white column all over again.
        std::unordered_set<std::uint64_t> seen_placements;
        for (auto const& transform : it.second)
        {
          glm::vec3 const inst_pos(transform[3]);
          if (particle_too_far(inst_pos)) { continue; } // [PERF] skip drawing far emitters' particles
          // Stable per-placement key from the (static) doodad position.
          auto mix = [](std::uint64_t h, std::int64_t v)
          {
            return h ^ (static_cast<std::uint64_t>(v) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
          };
          std::uint64_t key = 0x517CC1B727220A95ull;
          key = mix(key, static_cast<std::int64_t>(std::llround(inst_pos.x * 8.0f)));
          key = mix(key, static_cast<std::int64_t>(std::llround(inst_pos.y * 8.0f)));
          key = mix(key, static_cast<std::int64_t>(std::llround(inst_pos.z * 8.0f)));

          if (!seen_placements.insert(key).second)
          {
            continue; // duplicate entry of a placement already simulated+drawn this frame
          }

          pmodel->swapInstanceEmitterState(key);
          if (draw_model_animations && !threaded_sim) // when threaded, the sim already advanced in the pre-pass
          {
            float adt = emitter_frame_dt;
            while (adt > 0.1f)
            {
              pmodel->updateParticleSystems(0.1f);
              adt -= 0.1f;
            }
            pmodel->updateParticleSystems(adt);
          }
          pmodel->renderer()->drawParticlesForInstance(glm::transpose(model_view), particles_shader, transform);
          if (pmodel->hasGeometryParticles())
          {
            pmodel->appendGeometryParticleTransforms(transform, geometry_particle_draws);
          }
          pmodel->swapInstanceEmitterState(key);
        }
      }
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
      if (particle_too_far(instance->get_pos())) { continue; } // [PERF] skip far creature-effect particles
      bool const use_instance_state = draw_model_animations && draw_item.spawn;
      if (use_instance_state)
      {
        pmodel->swapInstanceEmitterState(draw_item.guid);
      }
      // x draw_item.fade (user 2026-08-25): the stream-in/cull fade is ONE alpha for the whole unit
      // -- the body mesh eases in over 2 s while its emitters popped at full opacity. Particles now
      // carry CreatureModelAlpha AND the unit fade, like every other component.
      pmodel->renderer()->drawParticlesForInstance(
          glm::transpose(model_view), particles_shader, instance->transformMatrix(),
          instance->model_alpha * draw_item.fade);
      if (pmodel->hasGeometryParticles())
      {
        pmodel->appendGeometryParticleTransforms(instance->transformMatrix(), geometry_particle_draws);
      }
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

          // Surface-bounded particle systems (breath bubbles, kill plane = the liquid surface):
          // when the camera is ABOVE water, the client hides them behind the ~0.9-alpha surface
          // (it draws particles BEFORE water; measured: the water base pass has ZWRITE OFF, so
          // there is no real depth occlusion either way). noggit draws particles AFTER water (the
          // god-ray ordering), so the equivalent is skipping the draw -- at the canal's full
          // opacity that is visually identical, and submerged cameras still see the bubbles.
          if (!_camera_underwater && attachment.particle_kill_plane_y < 1.0e29f)
          {
            continue;
          }

          std::uint64_t const attachment_key = static_cast<std::uint64_t>(draw_item.guid)
            | (static_cast<std::uint64_t>(&attachment - draw_item.spawn->attachment_models.data() + 1) << 32);
          if (draw_model_animations)
          {
            amodel->swapInstanceEmitterState(attachment_key);
          }
          // CreatureModelAlpha does NOT apply here (aura/spell effect models are separate from the
          // creature in the client -- Anomalus body 0.784, sparkles full), but the unit's stream-in/
          // cull FADE is one alpha for every component: attachment emitters fade in with the body.
          amodel->renderer()->drawParticlesForInstance(
              glm::transpose(model_view), particles_shader,
              attachment.particle_transform, draw_item.fade);
          if (draw_model_animations)
          {
            amodel->swapInstanceEmitterState(attachment_key);
          }
        }
      }
    }

    // Individually-drawn GAMEOBJECTS (mid-fade + cylindrical-billboard models like the mage portals):
    // their transforms are no longer in models_to_draw, so the bucket placement loop above never sees
    // them -- without this loop the portals' swirl emitters vanished the moment they were rerouted to
    // the per-instance path. Same swap-by-key pattern as WMO doodads (GOs are static placements).
    for (auto const& go_item : go_fading_individual)
    {
      ModelInstance* go_instance = go_item.first;
      if (!go_instance || !go_instance->model.get() || !go_instance->model->finishedLoading()
          || go_instance->model->loading_failed()
          || (!draw_hidden_models && go_instance->model->is_hidden())
          || go_instance->model->_particles.empty())
      {
        continue;
      }
      glm::vec3 const go_pos(go_instance->transformMatrix()[3]);
      if (particle_too_far(go_pos)) { continue; }
      Model* pmodel = go_instance->model.get();
      // Unlike creatures (whose body pass advances their per-guid particle state), the individual GO
      // body draw does NOT advance emitters -- advance this placement's own state here before submitting,
      // the same dt-stepping the bucket placement loop uses.
      std::uint64_t const key = wmo_doodad_placement_key(go_pos);
      if (draw_model_animations)
      {
        pmodel->swapInstanceEmitterState(key);
        float pdt = _world->models_emitter_dt();
        while (pdt > 0.1f)
        {
          pmodel->updateParticleSystems(0.1f);
          pdt -= 0.1f;
        }
        pmodel->updateParticleSystems(pdt);
      }
      // go_item.second = the GO's 2 s fade alpha (user 2026-08-25): the mesh eased in while the
      // emitters (mining-vein smoke etc.) popped at full opacity the frame the GO entered range.
      pmodel->renderer()->drawParticlesForInstance(
          glm::transpose(model_view), particles_shader, go_instance->transformMatrix(),
          go_item.second);
      if (draw_model_animations)
      {
        pmodel->swapInstanceEmitterState(key);
      }
    }

    // Per-instance-animation WMO doodads (candle flames on billboarded candelabras etc.): draw each
    // copy's OWN particle state, advanced during its mesh draw -- the same swap-by-key pattern as the
    // creature spawns above. (These models are excluded from model_with_particles / the placement loop.)
    {
      static bool const s_no_pidoodad_particles = std::getenv("NOGGIT_NO_PIDOODAD_PARTICLES") != nullptr;
      std::unordered_set<std::uint64_t> seen_doodad_keys;
      for (ModelInstance* _dptr : per_instance_wmo_doodads)
      {
        if (s_no_pidoodad_particles)
        {
          break;
        }
        if (!_dptr) { continue; } // defensive: cache pointers are never null in practice
        ModelInstance& doodad = *_dptr;
        // Cull on the WORLD position: for WMO doodads get_pos() is the WMO-LOCAL position (the draw below
        // uses transformMatrix()), so comparing it to the world camera made EVERY WMO-doodad emitter read as
        // "too far" and culled all of them at any distance -- steam/lavasteam vanished. [2026-07-25 fix]
        if (particle_too_far(glm::vec3(doodad.transformMatrix()[3]))) { continue; } // [PERF] skip far per-instance doodad particles
        Model* pmodel = doodad.model.get();
        if (!pmodel || !pmodel->finishedLoading() || pmodel->loading_failed()
            || (!draw_hidden_models && pmodel->is_hidden())
            || pmodel->_particles.empty())
        {
          continue;
        }
        std::uint64_t const key = wmo_doodad_placement_key(doodad.get_pos());
        if (!seen_doodad_keys.insert(key).second)
        {
          continue;
        }
        if (draw_model_animations)
        {
          pmodel->swapInstanceEmitterState(key);
        }
        // Crash instrumentation (NOGGIT_CLASSIC_EFFECT_DEBUG): the LAST such line in the log before an
        // EXCEPTION names the model whose particle submission the driver faulted on. Uncapped by design.
        static bool const s_pid_dbg = std::getenv("NOGGIT_CLASSIC_EFFECT_DEBUG") != nullptr;
        if (s_pid_dbg)
        {
          LogError << "PIDOODAD particles model='" << pmodel->file_key().stringRepr()
                   << "' key=" << key << " systems=" << pmodel->_particles.size() << std::endl;
        }
        pmodel->renderer()->drawParticlesForInstance(
            glm::transpose(model_view), particles_shader, doodad.transformMatrix());
        if (draw_model_animations)
        {
          pmodel->swapInstanceEmitterState(key);
        }
      }
    }
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // restore default for following passes
  }


  if (!model_with_particles.empty()) // ribbons too: drawn frozen when animations are off
  {
    noggit::perf::Scoped _prof_part2(noggit::perf::Phase::M2Particles);
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const cull;
    OpenGL::Scoped::depth_mask_setter<GL_FALSE> const depth_mask;

    OpenGL::Scoped::use_program ribbon_shader {*_m2_ribbons_program.get()};

    ribbon_shader.uniform("model_view_projection", mvp_rel); // camera-relative (subtracts camera in-shader)
    ribbon_shader.uniform("camera", camera_pos);

    // Additive RGB unchanged; alpha pulls the bloom mask down under the ribbon (same rationale as the
    // particle pass) so additive ribbons don't bloom through an emissive surface they happen to cross.
    gl.blendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);

    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (auto& it : model_with_particles)
    {
      it.first->renderer()->drawRibbonsFiltered(ribbon_shader, it.second);
    }
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }

  // GEOMETRY-MODEL PARTICLES (checklist 12.2): draw the collected mesh instances -- one transform
  // per live particle -- through the instanced M2 path (the deferred-additive pattern). Falling
  // embers / Dark Iron node rocks (Burning Steppes), SandVortex cyclone, cone-of-cold shards.
  if (!geometry_particle_draws.empty())
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
    m2_shader.uniform("unlit", static_cast<int>(model_render_state.unlit));
    m2_shader.uniform("tex_unit_lookup_1", 0);
    m2_shader.uniform("tex_unit_lookup_2", 0);
    m2_shader.uniform("pixel_shader", 0);
    m2_shader.uniform("model_origin", glm::vec3(0.0f));

    std::unordered_map<Model*, std::size_t> unused_boxes;

    for (auto& entry : geometry_particle_draws)
    {
      auto ref = _geometry_particle_models.find(entry.first);
      if (ref == _geometry_particle_models.end())
      {
        ref = _geometry_particle_models.emplace(
            entry.first,
            scoped_model_reference(BlizzardArchive::Listfile::FileKey(entry.first), _world->_context)).first;
      }
      Model* geometry_model = ref->second.get();
      if (!geometry_model || !geometry_model->finishedLoading() || geometry_model->loading_failed())
      {
        continue;
      }
      geometry_model->renderer()->draw(model_view
          , entry.second
          , m2_shader
          , model_render_state
          , frustum
          , _cull_distance
          , camera_pos
          , _world->model_animtime
          , false
          , unused_boxes
          , display
      );
    }

    gl.disable(GL_BLEND);
    gl.enable(GL_CULL_FACE);
    gl.depthMask(GL_TRUE);
  }

  // RE-STAMP creature bloom masks AFTER the particle + ribbon passes. Those passes multiply the FBO
  // bloom-mask alpha down under every particle (so additive fire doesn't over-bloom the lava it crosses)
  // -- which also erased the emissive mask of a bright energy creature whenever ANY particle (e.g. an
  // arcane elemental's smoke) passed in front of it, killing its bloom. The live client blooms the
  // creature through the smoke. Re-draw each energy creature's body into the ALPHA CHANNEL only, at its
  // own depth, re-asserting the brightness-driven mask so the bloom survives whatever drew over it.
  if (!creature_spawn_instances_to_draw.empty())
  {
    OpenGL::Scoped::use_program m2_shader {*_m2_program.get()};
    OpenGL::M2RenderState mask_state;
    mask_state.tex_arrays = {0, 0};
    mask_state.tex_indices = {0, 0};
    mask_state.tex_unit_lookups = {0, 0};
    gl.disable(GL_BLEND);
    m2_shader.uniform("blend_mode", 0);
    m2_shader.uniform("unfogged", 0);
    m2_shader.uniform("unlit", 0);
    m2_shader.uniform("tex_unit_lookup_1", 0);
    m2_shader.uniform("tex_unit_lookup_2", 0);
    m2_shader.uniform("pixel_shader", 0);

    for (auto const& draw_item : creature_spawn_instances_to_draw)
    {
      auto* instance = draw_item.instance;
      if (!instance || !instance->model.get() || instance->model->loading_failed())
      {
        continue;
      }
      if (!draw_hidden_models && instance->model->is_hidden())
      {
        continue;
      }
      Model* m = instance->model.get();
      if (!m->file_key().hasFilepath())
      {
        continue;
      }
      auto const& path = m->file_key().filepath();
      if (path.rfind("creature/", 0) != 0 && path.rfind("character/", 0) != 0)
      {
        continue; // only creature/character energy models carry the bloom mask
      }
      // Reproduce the body pass's exact animation phase (per-spawn offset) + force re-animation, so the
      // re-stamped mask lines up with the animated body geometry rather than the last-drawn model's pose.
      int const mask_animtime = draw_item.spawn
        ? static_cast<int>(_world->model_animtime) + draw_item.spawn->animation_time_offset
        : static_cast<int>(_world->model_animtime);
      if (draw_model_animations)
      {
        m->animcalc = false;
      }
      m->renderer()->draw(model_view, *instance, m2_shader, mask_state, frustum,
                          _cull_distance, camera_pos, mask_animtime, display,
                          /*no_cull*/ false, /*bloom_mask_only*/ true,
                          /*interior_light*/ glm::vec4(0.f),
                          /*dist_fade*/ draw_item.fade);
    }
    gl.enable(GL_CULL_FACE);
    gl.depthMask(GL_TRUE);
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

          if (glm::distance(sky.pos, camera_pos) <= _terrain_cull_distance) // TODO: frustum cull here
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
    noggit::perf::Scoped _prof_post(noggit::perf::Phase::Post);
    renderBloomAndComposite(static_cast<GLuint>(bloom_prev_fbo), bloom_vp[2], bloom_vp[3], camera_pos);
  }
}

bool WorldRender::snapshotDecalDepth()
{
  if (_decal_depth_ready)
  {
    return _decal_depth_tex != 0;
  }

  GLint vp[4] = {0, 0, 0, 0};
  gl.getIntegerv(GL_VIEWPORT, vp);
  if (vp[2] <= 0 || vp[3] <= 0)
  {
    return false;
  }

  GLint prev_draw_fbo = 0;
  gl.getIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);

  if (_decal_depth_w != vp[2] || _decal_depth_h != vp[3] || !_decal_depth_tex)
  {
    if (!_decal_depth_tex)
    {
      gl.genTextures(1, &_decal_depth_tex);
      gl.genFramebuffers(1, &_decal_depth_fbo);
    }
    _decal_depth_w = vp[2];
    _decal_depth_h = vp[3];
    gl.activeTexture(GL_TEXTURE1);
    gl.bindTexture(GL_TEXTURE_2D, _decal_depth_tex);
    gl.texImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, vp[2], vp[3], 0,
                  GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, _decal_depth_fbo);
    gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                            GL_TEXTURE_2D, _decal_depth_tex, 0);
    gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_draw_fbo));
  }

  gl.bindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_draw_fbo));
  gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, _decal_depth_fbo);
  gl.blitFramebuffer(0, 0, vp[2], vp[3], 0, 0, vp[2], vp[3], GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  gl.bindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_draw_fbo));

  _decal_inv_viewport = glm::vec2(1.0f / static_cast<float>(vp[2]), 1.0f / static_cast<float>(vp[3]));
  _decal_depth_ready = true;
  return true;
}

bool WorldRender::snapshotWorldDepth()
{
  if (_world_depth_ready)
  {
    return _world_depth_tex != 0;
  }

  GLint vp[4] = {0, 0, 0, 0};
  gl.getIntegerv(GL_VIEWPORT, vp);
  if (vp[2] <= 0 || vp[3] <= 0)
  {
    return false;
  }

  GLint prev_draw_fbo = 0;
  gl.getIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);

  if (_world_depth_w != vp[2] || _world_depth_h != vp[3] || !_world_depth_tex)
  {
    if (!_world_depth_tex)
    {
      gl.genTextures(1, &_world_depth_tex);
      gl.genFramebuffers(1, &_world_depth_fbo);
    }
    _world_depth_w = vp[2];
    _world_depth_h = vp[3];
    gl.activeTexture(GL_TEXTURE2);
    gl.bindTexture(GL_TEXTURE_2D, _world_depth_tex);
    gl.texImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, vp[2], vp[3], 0,
                  GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, _world_depth_fbo);
    gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                            GL_TEXTURE_2D, _world_depth_tex, 0);
    gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_draw_fbo));
    gl.activeTexture(GL_TEXTURE0);
  }

  gl.bindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_draw_fbo));
  gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, _world_depth_fbo);
  gl.blitFramebuffer(0, 0, vp[2], vp[3], 0, 0, vp[2], vp[3], GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  gl.bindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_draw_fbo));

  _world_depth_ready = true;
  return true;
}

void WorldRender::ensureBloomTargets(int w, int h)
{
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  // QUARTER res, matching the client's measured FFXGlow chain (wow_cap_* traces: Box4 downsample to
  // 640x360 at 2560x1440). Half-res spread the halo only half as far in screen space -- after the
  // blur^2 composite that read as "bloom not bright/soft enough" (Anomalus's energy body in Kara).
  int const bw = std::max(1, w / 4);
  int const bh = std::max(1, h / 4);

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

  // MSAA sample count (Settings -> Render features): 0/2/4/8, clamped to the driver max. Read every
  // call so a settings change reallocates live. DEFAULT 4 (2026-07-25 perf): the internal scene FBO is
  // fragment-shaded per sample, so 8x MSAA roughly DOUBLED the frame in Stormwind (Frame ~40 -> ~20ms, fps
  // ~24 -> ~40 at 4x, user-confirmed still smooth). 8x was the single largest cost and hid inside WorldDraw.
  int msaa = QSettings().value("render/msaa", 4).toInt();
  if (char const* e = std::getenv("NOGGIT_MSAA")) { msaa = std::atoi(e); } // [PERF A/B 2026-07-21] override
  if (msaa != 0 && msaa != 2 && msaa != 4 && msaa != 8) { msaa = 0; }
  if (msaa > 0)
  {
    GLint max_samples = 0;
    gl.getIntegerv(GL_MAX_SAMPLES, &max_samples);
    msaa = std::min(msaa, static_cast<int>(max_samples));
  }

  if (_bloom_w == w && _bloom_h == h && _bloom_bw == bw && _bloom_bh == bh && _msaa_samples == msaa)
  {
    return; // also compare blur-buffer dims + msaa: either change alone must reallocate
  }
  _msaa_samples = msaa;

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

  // Multisampled scene targets (resolved into _bloom_scene_color before the bloom chain).
  if (_msaa_samples > 0)
  {
    if (!_msaa_fbo)
    {
      gl.genFramebuffers(1, &_msaa_fbo);
      gl.genRenderbuffers(1, &_msaa_color_rb);
      gl.genRenderbuffers(1, &_msaa_depth_rb);
    }
    gl.bindRenderbuffer(GL_RENDERBUFFER, _msaa_color_rb);
    gl.renderbufferStorageMultisample(GL_RENDERBUFFER, _msaa_samples, GL_RGBA8, w, h);
    gl.bindRenderbuffer(GL_RENDERBUFFER, _msaa_depth_rb);
    gl.renderbufferStorageMultisample(GL_RENDERBUFFER, _msaa_samples, GL_DEPTH24_STENCIL8, w, h);
    gl.bindFramebuffer(GL_FRAMEBUFFER, _msaa_fbo);
    gl.framebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _msaa_color_rb);
    gl.framebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, _msaa_depth_rb);
  }

  // half-res ping-pong targets for the blur
  for (int i = 0; i < 2; ++i)
  {
    alloc_color(_bloom_tex[i], bw, bh);
    gl.bindFramebuffer(GL_FRAMEBUFFER, _bloom_fbo[i]);
    gl.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _bloom_tex[i], 0);
  }
}

void WorldRender::renderBloomAndComposite(GLuint target_fbo, int w, int h, glm::vec3 const& camera_pos)
{
  // MSAA resolve: blit the multisampled scene into the single-sample colour texture the bloom chain
  // (and the final composite) samples from.
  if (_msaa_samples > 0)
  {
    gl.bindFramebuffer(GL_READ_FRAMEBUFFER, _msaa_fbo);
    gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, _bloom_scene_fbo);
    gl.blitFramebuffer(0, 0, _bloom_w, _bloom_h, 0, 0, _bloom_w, _bloom_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  }
  // CANON FFXGlow strength: the 1.12 client's full-screen additive glow weight = the per-zone glow FIELD
  // (LightParams field 4, 0..1; `Skies::glow()` reads it now -- NOT LightFloatBand band 3, which is cloud
  // density). RE'd from wow.exe FFXEffects.cpp (FUN_006cb020): the per-zone glow is packed into the
  // glow-quad's vertex ALPHA and gates the composite. A SEPARATE RGB scene-glow is floored at
  // 84/255=0.329 outdoors, but that is a DIFFERENT channel and does not gate the per-zone bloom -- so we
  // drive the bloom by the per-zone glow with NO floor. Zones authoring glow 0 (Dun Morogh day 406/34)
  // get NO full-screen bloom, matching in-game (user-confirmed: the 0.329 floor still over-bloomed
  // Steelgrill's fog). Bloomy zones author their own glow (Kara 0.6, default day 0.65, night 1.0) and
  // keep it. (Emissive lava/unlit surfaces still bloom via the bright-pass alpha mask in zones whose
  // glow > 0; a glow-0 zone matches the client, which likewise runs no FFXGlow there.)
  float const zone_glow = skies() ? std::clamp(skies()->glow(), 0.0f, 1.0f) : 0.0f;
  float const glow_strength = zone_glow; // NO floor -- per-zone glow (the composite-alpha weight) gates it

  static bool const s_glow_dbg = std::getenv("NOGGIT_LIGHT_DEBUG") != nullptr;
  if (s_glow_dbg)
  {
    LogError << "FFXGLOW  zone_glow=" << zone_glow << " strength=" << glow_strength
             << " inside_wmo=" << (_world->camera_is_inside_wmo(camera_pos) ? 1 : 0) << std::endl;
  }

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
    // Emissive knee (for the authored emissive/unlit surfaces -- lava, glows): gates emissive pixels by
    // maxchannel > threshold*0.55. 0.50 = the value Molten Core's lava read correctly at.
    p.uniform("threshold", 0.50f);
    // Canon full-screen glow weight (see above): scene*glow is added to the bright buffer, so the blur +
    // composite yield final = scene + blur(scene)*glow, exactly the client's additive FFXGlow.
    p.uniform("glow", glow_strength);
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
    // MEASURED composite (wow_cap_upstairs.trace, RE_notes/16): final = scene + zoneGlow * blur^2.
    // The bright pass is now a plain passthrough (no mask, no pre-scale), so the zone glow weight is
    // applied HERE, exactly where the client carries it (the composite quad's vertex alpha, 0.647 in
    // the captured inn frame).
    p.uniform("intensity", glow_strength);
    // Underwater wave warp (1.12 FFXGlowWave, RE doc 36): submerged camera -> the client's DuDv
    // screen warp + 0.329 base blur mix engage in the composite. The CLEAR_WATER param swap
    // (updateLightingUniformBlock) has already raised glow_strength to the water param's glow (1.0
    // on the stock map-0 lights), so the blur^2 term is at full client strength down here too.
    // NO TRANSITION RAMP (user 2026-08-27: "all RE from client, nothing guessed"): the warp is
    // ON underwater and OFF above -- binary. The old 5 s ease-in was a DERIVED invention (its
    // "5 s fade" decompile turned out to be the AUDIO ambience volume, doc 37); removed.
    _uw_ffx_weight = _camera_underwater ? 1.0f : 0.0f;
    p.uniform("wave_on", (_uw_ffx_weight > 0.001f) ? 1 : 0);
    p.uniform("wave_time", static_cast<float>(_world->animtime) * 0.001f);
    p.uniform("viewport_px", glm::vec2(static_cast<float>(w), static_cast<float>(h)));
    // Coupled wobble lever x USER-TUNED 0.25 (2026-08-27 "just make it quarter the strength" --
    // a DELIBERATE deviation from the client-exact 3px/0.329 pair; lever 100 = this quarter
    // look, 400 = the traced client pair) x the ramp-in weight above.
    p.uniform("wave_strength",
              _uw_ffx_weight * 0.25f
                * std::clamp(_world->_settings->value("render/underwater_wobble_strength", 100.0f)
                               .toFloat(), 0.0f, 400.0f) / 100.0f);
    gl.drawArraysInstanced(GL_TRIANGLES, 0, 3, 1);
  }

  gl.activeTexture(GL_TEXTURE0);
  gl.bindVertexArray(0);
  gl.enable(GL_DEPTH_TEST);
}

void WorldRender::ensureShadowTarget(int size)
{
  // 3.3.5a-style dynamic shadow map (RE doc 35): the client renders D24X8 DEPTH TEXTURES and samples
  // them with hardware PCF (`hwPCF`; Ascension trace: 1024x1024 at extShadowQuality 3). GL mirror:
  // a DEPTH_COMPONENT24 texture with COMPARE_REF_TO_TEXTURE -> sampler2DShadow PCF in the receivers.
  if (!_shadow_initialized)
  {
    gl.genFramebuffers(1, &_shadow_fbo);
    gl.genTextures(1, &_shadow_tex);
    gl.genFramebuffers(1, &_shadow_env_fbo);
    gl.genTextures(1, &_shadow_env_tex);
    _shadow_initialized = true;
    _shadow_size = 0;
  }
  if (_shadow_size == size)
  {
    return;
  }
  _shadow_size = size;

  // MUST restore the caller's framebuffer: this runs MID-FRAME while the scene renders into the
  // bloom/MSAA FBO -- leaving binding 0 here made the caller save-and-"restore" the DEFAULT
  // framebuffer, breaking the whole frame on every realloc (the black-world bug).
  GLint prev_fbo_binding = 0;
  gl.getIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo_binding);

  gl.activeTexture(GL_TEXTURE0);
  auto const alloc = [size](GLuint fbo, GLuint tex)
  {
    gl.bindTexture(GL_TEXTURE_2D, tex);
    gl.texImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); // LINEAR + compare = 2x2 hw PCF per tap
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    gl.bindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, 0);
    gl.drawBuffer(GL_NONE);
    gl.readBuffer(GL_NONE);
  };
  alloc(_shadow_fbo, _shadow_tex);
  alloc(_shadow_env_fbo, _shadow_env_tex);
  gl.bindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo_binding));
  gl.bindTexture(GL_TEXTURE_2D, 0);
}

glm::vec3 WorldRender::sunSpecColor() const
{
  return _skies ? _skies->color_set[SUN_COLOR] : glm::vec3(0.f);
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

  // [perf 2026-08-05] MDI cross-model batching variant. `instanced` = per-instance transform (loc6-9) +
  // interior (loc10); `batched` = per-instance texture LAYER (loc11) so models sampling different layers of
  // the SAME array object collapse into one glMultiDrawElementsIndirect call. The fragment shader needs
  // `batched` too (tex1_index/tex2_index -> per-instance flat varying). Draws from the shared geometry arena.
  _m2_batched_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("m2_vs", {"instanced", "batched"}) }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("m2_fs", {"batched"}) }
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
    // 3.3.5a dynamic shadow map: unit 16 (units 1-15 belong to the texture arrays; desktop GL exposes 32)
    m2_shader.uniform("shadow_map", 16);
    m2_shader.uniform("shadow_map_env", 17);
  }

  {
    std::vector<int> samplers {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    OpenGL::Scoped::use_program wmo_program {*_wmo_program.get()};
    wmo_program.uniform("render_batches_tex", 0);
    wmo_program.uniform("texture_samplers", samplers);
    wmo_program.bind_uniform_block("matrices", 0);
    wmo_program.bind_uniform_block("lighting", 1);
    wmo_program.uniform("shadow_map", 16);
    wmo_program.uniform("shadow_map_env", 17);
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
    mcnk_shader.uniform("shadow_map", 16);
    mcnk_shader.uniform("shadow_map_env", 17);

  }

  {
    OpenGL::Scoped::use_program m2_shader_instanced {*_m2_instanced_program.get()};
    m2_shader_instanced.bind_uniform_block("matrices", 0);
    m2_shader_instanced.bind_uniform_block("lighting", 1);
    m2_shader_instanced.uniform("bone_matrices", 0);
    m2_shader_instanced.uniform("tex1", 1);
    m2_shader_instanced.uniform("tex2", 2);
    m2_shader_instanced.uniform("shadow_map", 16);
    m2_shader_instanced.uniform("shadow_map_env", 17);
  }

  {
    // [perf 2026-08-05] MDI batching program: same UBO blocks + sampler units as the instanced program.
    OpenGL::Scoped::use_program m2_shader_batched {*_m2_batched_program.get()};
    m2_shader_batched.bind_uniform_block("matrices", 0);
    m2_shader_batched.bind_uniform_block("lighting", 1);
    m2_shader_batched.uniform("bone_matrices", 0);
    m2_shader_batched.uniform("tex1", 1);
    m2_shader_batched.uniform("tex2", 2);
    m2_shader_batched.uniform("shadow_map", 16);
    m2_shader_batched.uniform("shadow_map_env", 17);
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
  _m2_batched_program.reset();
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
  _path_decal_render.unload();
  _horizon_render.reset();

  _liquid_texture_manager.unload();

  _skies->unload();

  _buffers.unload();
  _vertex_arrays.unload();

  Noggit::Rendering::Primitives::WireBox::getInstance(_world->_context).unload();
}


void WorldRender::updateMVPUniformBlock(const glm::mat4x4& model_view, const glm::mat4x4& projection, glm::vec3 const& camera_pos)
{
  ZoneScoped;

  _mvp_ubo_data.model_view = model_view;
  _mvp_ubo_data.projection = projection;
  _mvp_ubo_data.camera_pos = glm::vec4(camera_pos, 1.0f);

  gl.bindBuffer(GL_UNIFORM_BUFFER, _mvp_ubo);
  gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(OpenGL::MVPUniformBlock), &_mvp_ubo_data);

}

void WorldRender::updateLightingUniformBlock(bool draw_fog, glm::vec3 const& camera_pos)
{
  ZoneScoped;
  noggit::perf::Scoped _prof_lc(noggit::perf::Phase::LightCollect);

  int daytime = static_cast<int>(_world->time) % 2880;

  // Zone light selection is the CLIENT-FAITHFUL Light.dbc position + distance-falloff blend
  // (Skies::findSkyWeights) -- spatial and continuous. We deliberately do NOT apply the AreaTable.dbc
  // area->light override (field 35 `LightId`): that column exists ONLY in the 3.3.5a 36-field
  // AreaTable; the 1.12 Turtle DBC has 25 fields and no such field, so 1.12 has no per-area light at
  // all. Applying it hard-snapped the zone light/fog to weight=1 the instant the camera crossed a
  // sub-area boundary, a WMO edge (indoor<->outdoor), or a terrain hole (getAreaID -> -1 dropped the
  // override) -- the "fog + lighting shoots up when I move a little" jump. The smooth position blend is
  // the whole story in 1.12, so force the override off. (getWMOAreaID/getAreaID were read ONLY to feed
  // this override; dropping them also removes their per-frame chunk-walk cost.)
  _skies->setAreaLightId(0);
  // Underwater: switch to the underwater LightParams set (CLEAR_WATER) so the fog colour/density and
  // ambient match the in-game submerged look (cool, dense fog); above water use CLEAR. Both colorFor
  // and floatParamFor read the active param, so this swaps tint and fog together. If a light doesn't
  // define the underwater param, active_sky_param falls back to defaults -- harmless.
  // GUARDED: camera_is_underwater walks the chunk + its liquid layers, which can throw while a tile is
  // mid-load/unload (the same hazard as any per-frame chunk query). Wrapped in a try/catch for that
  // reason; this one was not -> an uncaught exception here crashed the
  // editor intermittently (any map, any time the camera sits over loading liquid). Default to "not
  // underwater" on failure.
  bool underwater = false;
  int liquid_family = -1;
  try
  {
    // Typed query so the submersion effects know WHICH liquid: ADT layers + WMO group liquids --
    // Stormwind canals and MC lava are WMO liquid, invisible to the old chunk-only
    // camera_is_underwater ("underwater has no effect" in canals). The WMO half needs the probe
    // cache, which only game-mode tick used to build -- ensure it HERE so submersion works in
    // editor mode too (cheap: rebuilds only on >10yd movement / every 240 frames).
    _world->ensureProbeCache(camera_pos);
    if (auto const lq = _world->getLiquidAt(camera_pos))
    {
      // EXACT point test (round 19): the 0.35yd early-engage band predated the camera water-wall;
      // with the wall holding the camera ~0.24yd off the surface, the band made "touching the
      // plane" flip the underwater look while still above water. The wall prevents surface
      // clipping now, so the band is obsolete.
      underwater = lq->first > camera_pos.y;
      _camera_liquid_surface_y = lq->first;
      if (underwater)
      {
        // family from the id: LiquidType ids follow id%4 = {1 water, 2 ocean, 3 magma, 0 slime}
        // across both eras ({1,5,13,17}, {2,14}, {3,15,19}, {4,20}); 21 (Naxx slime) is the exception.
        int const id = lq->second;
        int const m = id % 4;
        liquid_family = (id == 21) ? 3 : (m == 1) ? 0 : (m == 2) ? 1 : (m == 3) ? 2 : 3;
      }
    }
  }
  catch (...)
  {
    underwater = false;
    liquid_family = -1;
  }
  _camera_underwater = underwater; // cached for the underwater-particulates pass in draw()
  _camera_liquid_family = liquid_family;
  // CLEAR_WATER params are the WATER submersion set -- under MAGMA/SLIME they wrongly blued the
  // scene ("underlava shows underwater blue"). Those liquids keep the CLEAR params and get their
  // own dense fog override below (tint measured from the client's lava/slime textures).
  _skies->setCurrentParam((underwater && liquid_family <= 1) ? CLEAR_WATER : CLEAR);
  // Weather: every band (fog/diffuse/ambient/sky/water) blends toward the STORM param set by the
  // editor weather intensity (Sky::colorFor / floatParamFor read it).
  Skies::set_weather_intensity(_world->weather_type != 0 ? _world->weather_intensity : 0.0f);
  _skies->update_sky_colors(camera_pos, daytime);
  _outdoor_light_stats = _outdoor_lighting->getLightStats(static_cast<int>(_world->time));

  glm::vec3 diffuse = _skies->color_set[LIGHT_GLOBAL_DIFFUSE];
  glm::vec3 ambient = _skies->color_set[LIGHT_GLOBAL_AMBIENT];
  glm::vec3 fog_color = _skies->color_set[FOG_COLOR];
  // NORTHREND FOG = neutral grey, not the DBC band's saturated cyan. The 3.3.5a client forces sky-type-2
  // zones (Northrend outdoor) to grey; noggit's param-569 FOG band is bright cyan RGB(86,178,211) = the
  // blue/purple fog. Client trace wow_cap_icecrown_sky: D3DRS_FOGCOLOR = 0xFF808080 = RGB(128,128,128).
  // Gate on WotLK + Northrend continent (map 571). WotLK/other continents untouched. See docs/client_re/30.
  {
    bool const wotlk_fog = []{ auto const* p = Noggit::Project::CurrentProject::get();
      return p && p->projectVersion != Noggit::Project::ProjectVersion::CLASSIC; }();
    if (wotlk_fog && _world->mapIndex._map_id == 571)
      fog_color = glm::vec3(72.0f / 255.0f, 72.0f / 255.0f, 76.0f / 255.0f); // dark neutral grey (gloomy)
  }
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
  // The fog START fraction is CLIENT-CANON and is NOT clamped -- a negative start is authored and correct.
  // Dun Morogh's Steelgrill's Depot light id=22 authors start=-0.3, distance=15000. apitrace of the 1.12
  // client at that exact spot (wow_cap_streelgrill.trace) records D3DRS_FOGSTART=-125.0, D3DRS_FOGEND=416.67
  // -- i.e. the client runs the negative start verbatim (start = -0.3 * 416.67 = -125.0 EXACTLY), putting
  // ~23% fog on the camera plane. That "overblown fog in your face" is the real in-game look; the client
  // jumps to it whenever this underground zone light becomes nearest-dominant (the trace shows both regimes:
  // -125/416.67 overblown and 0/1200 clear). An earlier std::max(0,fog_start) clamp here forced the start to
  // 0 and made noggit SOFTER than the client -- removed so noggit is bit-exact to the traced client fog.
  // Editor fog-distance scale (Settings, default 2.0). Pick the INTERIOR (WMO) multiplier when the camera
  // is inside a WMO, else the OUTDOOR (zone) one; store it so WMORender's per-group fog (fogDistanceScale())
  // uses the SAME band. fog_start is a fraction of fog_end, so scaling the end stretches the whole band.
  // [fog transition 2026-08-18] Blend the OUTDOOR->INDOOR fog-distance band CONTINUOUSLY by the camera's
  // depth into the room (client-spatial, RE note above) instead of snapping on the binary _camera_inside_wmo.
  // factor 0 at the entrance = outdoor band, 1 deep inside = interior band. collect_camera_fog below reads
  // this same _active_fog_distance_scale, so the WMO sphere fog eases with it too.
  static bool const s_no_fog_transition = std::getenv("NOGGIT_NO_FOG_TRANSITION") != nullptr;
  float const fog_scale_target = _camera_inside_wmo ? _fog_distance_scale_interior : _fog_distance_scale;
  if (s_no_fog_transition)
  {
    _active_fog_distance_scale = fog_scale_target; // original binary flip
  }
  else
  {
    // Ease the fog-distance SCALE toward its target so crossing the WMO boundary FADES the band instead of
    // snapping (the fog COLOUR is unchanged across the boundary -- only the band scales, e.g. Stormwind 4x->1x).
    // _camera_inside_wmo follows the CHARACTER (see _fog_probe_pos), so the target moves only on the
    // character's own crossing -- camera orbit never does. A map whose scale never flips (Timbermaw: fog from
    // MFOG spheres) holds a CONSTANT target here -> the ease is an exact no-op, its sphere blend untouched.
    static float const s_fog_scale_tau = []
    {
      char const* const e = std::getenv("NOGGIT_FOG_FADE_SECONDS");
      float const v = e ? static_cast<float>(std::atof(e)) : 0.5f;
      return v > 0.02f ? v : 0.5f;
    }();
    float const sdt = _world->models_emitter_dt();
    if (!_fog_scale_eased_valid || sdt <= 0.0f || sdt > 1.0f)
    {
      _fog_scale_eased = fog_scale_target; // first frame / paused / hitch -> snap (never fade from a stale value)
      _fog_scale_eased_valid = true;
    }
    else
    {
      _fog_scale_eased += (fog_scale_target - _fog_scale_eased) * (1.0f - std::exp(-sdt / s_fog_scale_tau));
    }
    _active_fog_distance_scale = _fog_scale_eased;
  }
  fog_end *= _active_fog_distance_scale;

  // NOTE (trace: wow_cap_kara_cull_fog, per-draw fog attribution): the client does NOT override the
  // scene fog when the camera enters a WMO. Fog is PER-REGION within the frame -- terrain/doodads keep
  // the zone fog while WMO geometry inside an MFOG volume is drawn with that fog's authored values
  // (blended toward zone fog by the camera's position in the fog sphere's r_start..r_end falloff).
  // The per-WMO fog is applied in WMORender::draw via shader uniforms; the global UBO fog here is
  // always the zone fog.

  // Client-canon farclip clamp (verified via apitrace on Elwynn): the world fog END is clamped to the
  // view distance (farclip). At a short view distance the fog is pulled in to the horizon; extending the
  // view lets fog reach its full authored distance and NO further. Measured: farclip 777 -> fogEnd 777;
  // farclip 2098 -> fogEnd 1250 (Elwynn's authored value). fog_start is a FRACTION of fog_end, so the
  // start scales with the clamp automatically (0.25 * 777 = 194.25, matching the trace).
  fog_end = std::min(fog_end, _view_distance);

  // CAMERA FOG: the map's authored WMO fog (MOFG spheres) is real content -- render it WHERE it's placed.
  // Blend any fog sphere the camera is inside over the zone fog (in absolute yards), then feed the result to
  // the MAIN fog below so EVERY draw -- terrain, doodads AND WMO geometry (via getZoneFog) -- reads the SAME
  // fog. Outside all spheres this is exactly the zone fog; inside one it fades to the sphere's colour /
  // distance (Karazhan's authored blue), fog-free up close via the authored start. This replaces the old
  // per-WMO-geometry-only MFOG (which mismatched terrain/doodads); NOGGIT_NO_WMO_FOG=1 = pure zone fog.
  {
    static bool const s_no_wmo_fog = std::getenv("NOGGIT_NO_WMO_FOG") != nullptr;
    if (!s_no_wmo_fog && draw_fog)
    {
      glm::vec3 cam_color = fog_color;
      float cam_end = fog_end;
      float cam_start_abs = fog_start * fog_end;
      // SELECT the fog from the character's placement (see _fog_probe_pos), not the camera -- orbiting the
      // 3rd-person camera into/out of a fog sphere must not change the scene fog.
      _world->collect_camera_fog(_fog_probe_pos, _active_fog_distance_scale, cam_color, cam_end, cam_start_abs);
      fog_color = cam_color;
      fog_end = std::min(cam_end, _view_distance);
      fog_start = (fog_end > 0.001f) ? (cam_start_abs / fog_end) : fog_start;
    }
  }

  // [MC red fog 2026-08-08, scoped 2026-08-09] WMO-ONLY-MAP camera fog: on maps that are one global
  // WMO (Molten Core -- no terrain, no meaningful zone fog to fall back to), the scene fog is the
  // camera's interior group's blended MFOG. noggit showed MC in default blue while all 10 authored
  // MFOG entries are lava-red (255,37,8), which also washed the lavafall particles gold. Entities
  // inherit automatically (the Env-UBO fog defaults to the scene fog).
  // SCOPED to hasAGlobalWMO after two field regressions: on NORMAL maps the client's scene fog stays
  // the ZONE fog even with the camera inside a WMO (trace note above, wow_cap_kara_cull_fog) -- the
  // unscoped override painted Karazhan's Malchezaar room heavy blue (the exact case the July RE said
  // the live client never shows) and grabbed Stormwind streets from overhanging interior group AABBs.
  // Normal maps keep zone fog + the placed-sphere composite; per-geometry WMO fog stays in WMORender.
  if (draw_fog && _world->mapIndex.hasAGlobalWMO())
  {
    WmoGroupFogVolume const* cam_int_group = nullptr;
    float best_vol = std::numeric_limits<float>::max();
    for (auto const& v : _env_fog_volumes)
    {
      // Which interior group the fog is taken from follows the CHARACTER's placement, not the camera.
      if (_fog_probe_pos.x < v.min.x || _fog_probe_pos.x > v.max.x
       || _fog_probe_pos.y < v.min.y || _fog_probe_pos.y > v.max.y
       || _fog_probe_pos.z < v.min.z || _fog_probe_pos.z > v.max.z)
      {
        continue;
      }
      if (!v.group->is_indoor() || v.group->is_exterior() || v.group->is_exterior_lit())
      {
        continue;
      }
      glm::vec3 const e = v.max - v.min;
      float const vol = e.x * e.y * e.z;
      if (vol < best_vol)
      {
        best_vol = vol;
        cam_int_group = &v;
      }
    }
    glm::vec3 mf_color;
    float mf_end = 0.0f, mf_start = 0.0f;
    if (cam_int_group
        && cam_int_group->wmo->evaluate_camera_fog(*cam_int_group->group, cam_int_group->transform,
                                                   _fog_probe_pos, /*camera_inside_wmo=*/ true,
                                                   &mf_color, &mf_end, &mf_start))
    {
      fog_color = mf_color;
      fog_end = std::min(mf_end * _active_fog_distance_scale, _view_distance);
      fog_start = (fog_end > 0.001f) ? (mf_start * _active_fog_distance_scale / fog_end) : fog_start;
      // sticky state: hold this fog over group-AABB gaps (below)
      _int_fog_color = fog_color;
      _int_fog_end = fog_end;
      _int_fog_start = fog_start;
      _int_fog_valid = true;
    }
    else if (_int_fog_valid && _camera_inside_wmo)
    {
      // STICKY HOLD: group AABBs don't tile the WMO seamlessly (bridge spans, doorways), so the
      // containment test can miss for a few yards while the camera is still deep inside the WMO --
      // without this the fog SNAPPED red<->blue with camera distance in MC. Keep the last interior
      // fog until the camera actually leaves the WMO.
      fog_color = _int_fog_color;
      fog_end = _int_fog_end;
      fog_start = _int_fog_start;
    }
    else if (!_camera_inside_wmo && !_world->mapIndex.hasAGlobalWMO())
    {
      // left the WMO -> zone fog again, and no stale hold on re-entry. EXCEPT on WMO-only maps
      // (Molten Core): the client camera can never exist outside the global WMO, so stepping the
      // editor camera through the shell must NOT flip the world to the meaningless zone fog
      // (the "lava turns yellow outside the map" report) -- hold the interior fog for the map.
      _int_fog_valid = false;
    }
  }

  // (The scene fog COLOUR is unchanged across a WMO boundary; the only thing that flips is the distance
  // scale, which is eased above -- so no whole-fog temporal ease is needed here, and none is applied so maps
  // whose fog comes from spatial MFOG spheres never get temporal lag.)

  // TRACE (NOGGIT_LIGHT_DEBUG): the FINAL zone-fog state fed to the render, logged so overblown-fog spots
  // can be diagnosed straight from log.txt. Pairs with the Sky.cpp ZONEFOG line (weighted-light list).
  {
    static bool const s_ff_dbg = std::getenv("NOGGIT_LIGHT_DEBUG") != nullptr;
    static int s_ff_tick = 0;
    if (s_ff_dbg && draw_fog && (++s_ff_tick % 30) == 0)
    {
      LogError << "FOGFINAL pos=(" << camera_pos.x << "," << camera_pos.y << "," << camera_pos.z << ")"
               << " raw_end(/36)=" << _skies->fog_distance_end()
               << " scale=" << _active_fog_distance_scale
               << " view_dist=" << _view_distance
               << " -> FOG_END=" << fog_end
               << " fog_start_abs=" << (fog_start * fog_end)
               << " fog_color=(" << fog_color.x << "," << fog_color.y << "," << fog_color.z << ")"
               << " inside_wmo=" << (_camera_inside_wmo ? 1 : 0)
               << std::endl;
    }
  }

  // (An earlier temporal fog lerp lived here; the per-draw trace showed the client's fog transitions
  // are SPATIAL -- Light.dbc falloff radii for the zone fog, MFOG sphere falloff for WMO fog -- both
  // of which are handled at their sources, so no temporal smoothing is needed or canon.)

  // SUBMERGED IN MAGMA/SLIME: dense fog in the liquid's own colour ("super foggy and red"),
  // replacing every other fog source. Tints MEASURED from the client's liquid textures
  // (lava.1.blp mean RGB 176,23,0; slime.1.blp mean 68,132,19); range = a short 25yd wall.
  // Water/ocean submersion keeps the CLEAR_WATER param fog (client law); this override is only
  // for the liquids the param system has no set for.
  if (_camera_underwater && _camera_liquid_family >= 2)
  {
    fog_color = (_camera_liquid_family == 2)
              ? glm::vec3(176.0f / 255.0f, 23.0f / 255.0f, 0.0f)
              : glm::vec3(68.0f / 255.0f, 132.0f / 255.0f, 19.0f / 255.0f);
    fog_start = 0.0f;   // fraction of fog_end -- dense from the camera
    fog_end = 25.0f;
  }

  // UNDERWATER DEPTH DARKENING: REMOVED 2026-08-26 (user: "not canon -- double check"). It was
  // an ADMITTED APPROXIMATION (the hypothesised 1-eyeZ*k law was never RE'd; neither capture has
  // a dive, and no depth-scaled light multiplier exists in any decompiled underwater chain --
  // the client's whole submersion look is the CLEAR_WATER param swap + its short fog). Do NOT
  // reintroduce without trace/decomp evidence.

  _lighting_ubo_data.DiffuseColor_FogStart = {diffuse.x,diffuse.y,diffuse.z, fog_start};
  _lighting_ubo_data.AmbientColor_FogEnd = {ambient.x,ambient.y,ambient.z, fog_end};
  _lighting_ubo_data.FogColor_FogOn = {fog_color.x,fog_color.y,fog_color.z, static_cast<float>(draw_fog)};

  // ENTITY fog (client-canon, note 16: inside the inn the M2 fog constants equal the inn MFOG): the
  // camera's fog context = zone fog blended toward the strongest containing MFOG volume. Written to
  // the Env UBO slots read by the M2/particle/ribbon/WMO-liquid shaders; terrain keeps zone fog.
  // NOGGIT_NO_M2_ENV_FOG=1 disables (entities then always use the zone fog, the old behaviour).
  {
    // Entity/camera MFOG is OPT-IN (default OFF). It resolves the camera's WMO group by the SMALLEST
    // containing group AABB (World::collect_fog_volumes) -- a loose box that extends well outside the
    // WMO's real geometry, so it over-applies a WMO's placed MFOG spheres to nearby doodads. Per the
    // MFOG RE (docs/client_re/27, wow.exe @0069de20): "the live client never shows" that heavy blue fog
    // near the Tower of Karazhan -- it uses the ZONE fog there. Karazhan's open Malchezaar tower doodads
    // were taking the WMO's cyan/purple fog spheres (env_w flipping 0<->1 = the intensity "spike") while
    // the terrain kept the zone fog -> doodads-blue/terrain-gray split. Default OFF => doodads use the
    // SAME zone fog as the terrain (consistent + client-correct). Re-enable via NOGGIT_M2_ENV_FOG=1 only
    // once a precise portal-based group resolution replaces the leaky AABB test.
    static bool const s_env_fog_on = std::getenv("NOGGIT_M2_ENV_FOG") != nullptr;
    float env_w = 0.0f;
    glm::vec3 env_color = fog_color;
    float env_end = fog_end;
    float env_start_abs = fog_start * fog_end;

    if (s_env_fog_on && draw_fog)
    {
      // CLIENT-EXACT (wow.exe @0069de20, note 27): the camera fog comes from the GROUP the camera
      // is standing in (smallest containing group AABB approximates the client's portal-resolved
      // current group). The evaluator blends that group's in-range MOGP fogs (farthest -> nearest,
      // w = 1 inside r1 -> 0 at r2) over the WMO's DEFAULT MFOG entry; default-only WMOs keep the
      // zone fog. The old best-sphere heuristic (any WMO, blend toward zone) is replaced.
      WmoGroupFogVolume const* cam_group = nullptr;
      float best_volume = std::numeric_limits<float>::max();
      for (auto const& v : _env_fog_volumes)
      {
        if (camera_pos.x < v.min.x || camera_pos.x > v.max.x
         || camera_pos.y < v.min.y || camera_pos.y > v.max.y
         || camera_pos.z < v.min.z || camera_pos.z > v.max.z)
        {
          continue;
        }
        glm::vec3 const e = v.max - v.min;
        float const vol = e.x * e.y * e.z;
        if (vol < best_volume)
        {
          best_volume = vol;
          cam_group = &v;
        }
      }
      glm::vec3 mf_color;
      float mf_end = 0.0f, mf_start = 0.0f;
      // Weather gate input (user 2026-08-26: "rain/snow showing inside buildings"): remember
      // whether the camera's group is a TRUE interior. Exterior groups (Stormwind streets) and
      // ext-lit open channels (canals/harbours) are NOT interiors -- they keep their rain.
      _camera_in_indoor_group = cam_group
        && !cam_group->group->is_exterior()
        && !cam_group->group->is_exterior_lit();
      if (cam_group
          && cam_group->wmo->evaluate_camera_fog(*cam_group->group, cam_group->transform,
                                                 camera_pos, /*camera_inside_wmo=*/ true,
                                                 &mf_color, &mf_end, &mf_start))
      {
        env_w = 1.0f;
        env_color = mf_color;
        env_end = std::min(mf_end * _active_fog_distance_scale, _view_distance);
        env_start_abs = mf_start * _active_fog_distance_scale;
      }

      // DIAGNOSTIC (NOGGIT_LIGHT_DEBUG): the ENTITY fog fed to M2/doodads. env_w=1 => doodads use this
      // instead of the zone fog (terrain always uses the zone fog). grp_ext/grp_extlit = the camera
      // group's MOGP 0x8/0x40 flags: an exterior group must resolve env_w=0 (zone fog) so doodads match
      // the ground -- the Malchezaar-tower doodads-blue-terrain-gray split.
      {
        static bool const s_env_dbg = std::getenv("NOGGIT_LIGHT_DEBUG") != nullptr;
        static int s_env_tick = 0;
        if (s_env_dbg && (++s_env_tick % 30) == 0)
        {
          LogError << "ENVFOG env_w=" << env_w
                   << " cam_group=" << (cam_group ? 1 : 0)
                   << " grp_ext=" << (cam_group ? (cam_group->group->is_exterior() ? 1 : 0) : -1)
                   << " grp_extlit=" << (cam_group ? (cam_group->group->is_exterior_lit() ? 1 : 0) : -1)
                   << " env_color=(" << env_color.x << "," << env_color.y << "," << env_color.z << ")"
                   << " env_end=" << env_end << std::endl;
        }
      }
    }

    float const env_start_frac = env_end > 0.001f ? std::clamp(env_start_abs / env_end, -5.0f, 0.99f) : 0.25f;
    _lighting_ubo_data.EnvFogColor_On = {env_color.x, env_color.y, env_color.z, env_w};
    _lighting_ubo_data.EnvFogDist = {env_start_frac, env_end, 0.0f, 0.0f};

    // SUBMERGED IN MAGMA/SLIME (round 27): the dense liquid fog must replace the ENTITY/Env fog
    // context too. Molten Core is a WMO interior, so its models AND geometry read the Env slots --
    // with only the zone fog overridden they kept the room's MFOG and showed as silhouettes far
    // past the lava fog ("I shouldn't be able to see far past the fog in the lava"). Same measured
    // liquid-texture tints and the same short 25yd wall.
    if (_camera_underwater && _camera_liquid_family >= 2)
    {
      glm::vec3 const liquid_fog = (_camera_liquid_family == 2)
        ? glm::vec3(176.0f / 255.0f, 23.0f / 255.0f, 0.0f)
        : glm::vec3(68.0f / 255.0f, 132.0f / 255.0f, 19.0f / 255.0f);
      _lighting_ubo_data.EnvFogColor_On = {liquid_fog.x, liquid_fog.y, liquid_fog.z, 1.0f};
      _lighting_ubo_data.EnvFogDist = {0.0f, 25.0f, 0.0f, 0.0f};
    }
  }
  _lighting_ubo_data.LightDir_FogRate = {_outdoor_light_stats.dayDir.x, _outdoor_light_stats.dayDir.y, _outdoor_light_stats.dayDir.z, _skies->fogRate()};
  _lighting_ubo_data.OceanColorLight = { ocean_color_light.x,ocean_color_light.y,ocean_color_light.z, _skies->ocean_shallow_alpha()};
  _lighting_ubo_data.OceanColorDark = { ocean_color_dark.x,ocean_color_dark.y,ocean_color_dark.z, _skies->ocean_deep_alpha()};
  _lighting_ubo_data.RiverColorLight = { river_color_light.x,river_color_light.y,river_color_light.z, _skies->river_shallow_alpha()};
  _lighting_ubo_data.RiverColorDark = { river_color_dark.x,river_color_dark.y,river_color_dark.z, _skies->river_deep_alpha()};

  if (capture_lighting_trace_enabled())
  {
    // Area IDs are diagnostic-only now (the area->light override is disabled above); resolve them here so
    // the trace still reports them -- and the per-frame chunk-walk cost is only paid while tracing.
    unsigned int wmo_area_id = static_cast<unsigned int>(-1);
    unsigned int terrain_area_id = static_cast<unsigned int>(-1);
    unsigned int area_id = static_cast<unsigned int>(-1);
    int area_light_id = 0;
    try
    {
      wmo_area_id = _world->getWMOAreaID(camera_pos);
      terrain_area_id = _world->getAreaID(camera_pos);
      area_id = (wmo_area_id != static_cast<unsigned int>(-1)) ? wmo_area_id : terrain_area_id;
      if (area_id != static_cast<unsigned int>(-1)
          && gAreaDB.getFieldCount() > AreaDB::LightId && gAreaDB.CheckIfIdExists(area_id))
      {
        area_light_id = gAreaDB.getByID(area_id).getInt(AreaDB::LightId); // would-be override (NOT applied)
      }
    }
    catch (...) {}

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
    // [PERF 2026-07-21] The two authored-light walks below iterate ALL m2 + ALL wmo instances (the WMO one
    // calls get_doodads per WMO) EVERY frame -- ~4-5ms, the bulk of the LightCollect phase -- even though
    // almost every instance early-outs at lights().empty(). But point lights are STATIC world-space
    // (campfires don't move): only WHICH nearest-16 are selected changes as the camera moves, and only the
    // flicker COLOUR animates. So rebuild the collected set every 3rd frame, or immediately on a >20yd
    // camera jump (teleport / click-move); on the other frames reuse the cached _lighting_ubo_data.Point*
    // set, which is re-uploaded verbatim below. Flicker then updates at ~20Hz (imperceptible; film is 24),
    // a light fades in/out at most ~3 frames late -- and the per-frame instance walk cost drops ~3x. Same
    // caching principle as the interior-light 60-frame epoch and the collect_camera_fog cull.
    static unsigned s_light_collect_epoch = 0;
    static glm::vec3 s_last_light_collect_pos(1e18f, 1e18f, 1e18f);
    bool const rebuild_point_lights = (s_light_collect_epoch++ % 3u == 0u)
        || glm::distance(camera_pos, s_last_light_collect_pos) > 20.0f;
    if (rebuild_point_lights)
    {
    s_last_light_collect_pos = camera_pos;
    // is_molt: WMO MOLT lights are flagged so the m2 shader can EXCLUDE them for units -- the client
    // never lights units with MOLT point lights (they convert to linear-falloff directionals that are
    // hard-skipped at d>=attenEnd, i.e. always skipped in classic WMOs with attenEnd=0; RE_notes/15
    // section 5). Doodads/WMO geometry keep them.
    struct CollectedLight { glm::vec3 pos; glm::vec3 color; float radius; float dist2; bool is_molt = false; };
    std::vector<CollectedLight> collected;

    // [PERF 2026-07-21] AABB distance cull for the authored-light walks below. Both walk uncached over ALL
    // m2 + ALL wmo instances (the WMO one calls get_doodads per WMO). A collected point light's radius is
    // clamped to <=60yd, so any instance whose bounding box is farther than view_distance+64 from the
    // camera CANNOT illuminate a visible surface -- skip it. Using the AABB (not the origin) keeps big
    // WMOs correct when the camera stands inside them (Ironforge). Exact: no visible light is dropped.
    // (The dominant ~3.3s/frame outdoor cost was World::collect_camera_fog, fixed there with the same
    // cull; this walk was the cheaper sibling.) NOGGIT_NO_LIGHT_CULL=1 disables for A/B.
    static bool const s_no_light_cull = std::getenv("NOGGIT_NO_LIGHT_CULL") != nullptr;
    float const _light_cull_d2 = (_view_distance + 64.0f) * (_view_distance + 64.0f);
    auto const _too_far_for_light = [&](glm::vec3 const& mn, glm::vec3 const& mx) -> bool
    {
      glm::vec3 const nearest = glm::clamp(camera_pos, mn, mx);
      glm::vec3 const dd = camera_pos - nearest;
      return glm::dot(dd, dd) > _light_cull_d2;
    };

    // CANON LIGHTING ONLY: do NOT synthesize warm point lights for "hot" props (lightray shafts,
    // braziers, fires, candles, lava, etc.). Previous code fabricated a warm light from an
    // emissive-material flag + a filename-keyword guess, which flooded prop-dense rooms (Zul'Farrak
    // troll camps) with up to 16 fake orange lights = the orange wash. A prop with no AUTHORED M2 light
    // does not illuminate its surroundings in the real client; only its own emissive material/particles
    // are visible. We keep ONLY authored lights below (standalone M2 type-1, WMO MOLT, WMO-doodad M2).
    //
    // [20.4 PERF 2026-08-22] Registry-driven collection: light carriers are a tiny fraction of the
    // instance flood (0.6% of M2 models have lights), so the per-rebuild walk visits ONLY the uids in
    // _point_light_registry instead of every instance. The registry re-derives (a full scan, identical
    // coverage to the old walk) whenever storage.light_epoch() changes -- bumped by every instance
    // add/move/remove/doodad-set change and tile load/unload. The per-instance collection math below is
    // UNCHANGED, so the collected set is identical; only which instances get VISITED differs.
    int lightray_lights = 0;
    int wmo_molt_lights = 0;

    auto const collect_m2_lights = [&] (ModelInstance& inst)
    {
      Model* m = inst.model.get();
      if (!m || !m->finishedLoading() || m->lights().empty())
      {
        return;
      }
      {
        auto const& _e = inst.getExtents();
        if (!s_no_light_cull && _too_far_for_light(_e[0], _e[1]))
        {
          return; // its lights (<=60yd radius) can't reach any visible surface
        }
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
    };

    // Collects the WMO's MOLT + doodad lights AND classifies it for the registry: has_lights = any
    // authored light found; fully_classified = it could be fully inspected (near enough, WMO and all
    // its doodad models finished loading). A has_lights WMO is re-walked every rebuild, so doodad
    // models that finish loading later still join in; !fully_classified stays pending.
    auto const collect_wmo_lights = [&] (WMOInstance& wmo, bool& has_lights, bool& fully_classified)
    {
      has_lights = false;
      fully_classified = true;
      {
        auto const& _e = wmo.getExtents();
        if (!s_no_light_cull && _too_far_for_light(_e[0], _e[1]))
        {
          fully_classified = false; // too far to inspect -- its lights can't reach anyway
          return;                   // WMO's MOLT + doodad lights (<=60yd) can't reach a visible surface -- skip get_doodads
        }
      }
      if (!wmo.wmo.get() || !wmo.wmo->finishedLoading())
      {
        fully_classified = false;
      }
      // Authored WMO lights (MOLT) -- e.g. Ironforge's 209 forge/torch lights, dungeon braziers. These
      // are parsed but were never fed to a renderer; wire them into the same point-light set the
      // terrain/WMO/M2 shaders already consume, so interiors are lit by their real authored lights.
      if (wmo.wmo.get() && wmo.wmo->finishedLoading() && !wmo.wmo->lights.empty())
      {
        has_lights = true;
        glm::mat4x4 const wmo_transform = wmo.transformMatrix();
        for (auto const& wl : wmo.wmo->lights)
        {
          glm::vec3 const world = glm::vec3(wmo_transform * glm::vec4(wl.pos, 1.0f));
          glm::vec3 const col = glm::vec3(wl.fcolor) * std::max(wl.intensity, 0.0f);
          float const radius = glm::clamp(wl.r, 6.0f, 60.0f); // MOLT attenuation-end radius
          glm::vec3 const d = world - camera_pos;
          collected.push_back({world, col, radius, glm::dot(d, d), /*is_molt*/ true});
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
          if (!dm)
          {
            continue;
          }
          if (!dm->finishedLoading())
          {
            fully_classified = false;
            continue;
          }

          // Real authored M2 lights on the WMO doodad (candles, lamps, braziers placed inside a WMO),
          // with the animated (flickering) colour sampled on the global clock.
          if (!dm->lights().empty())
          {
            has_lights = true;
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
        }
      }
    };

    auto& light_storage = _world->_model_instance_storage;
    auto& reg = _point_light_registry;
    std::uint64_t const light_epoch = light_storage.light_epoch();
    if (reg.epoch != light_epoch)
    {
      // FULL scan: same coverage as the pre-registry walk, deriving the registry as it goes.
      reg.m2_uids.clear();
      reg.pending_m2.clear();
      reg.wmo_uids.clear();
      reg.pending_wmo.clear();
      light_storage.for_each_m2_instance([&] (ModelInstance& inst)
      {
        Model* m = inst.model.get();
        if (!m)
        {
          return; // an instance never gains a model -- final
        }
        if (!m->finishedLoading())
        {
          reg.pending_m2.push_back(inst.uid);
          return;
        }
        if (m->lights().empty())
        {
          return; // final: a finished model's light list never changes
        }
        reg.m2_uids.push_back(inst.uid);
        collect_m2_lights(inst);
      });
      light_storage.for_each_wmo_instance([&] (WMOInstance& wmo)
      {
        bool has_lights = false, fully_classified = true;
        collect_wmo_lights(wmo, has_lights, fully_classified);
        if (has_lights)
        {
          reg.wmo_uids.push_back(wmo.uid);
        }
        else if (!fully_classified)
        {
          reg.pending_wmo.push_back(wmo.uid);
        }
      });
      reg.epoch = light_epoch;
    }
    else
    {
      light_storage.visit_m2_uids(reg.m2_uids, collect_m2_lights);
      if (!reg.pending_m2.empty())
      {
        std::vector<std::uint32_t> still_pending;
        still_pending.reserve(reg.pending_m2.size());
        light_storage.visit_m2_uids(reg.pending_m2, [&] (ModelInstance& inst)
        {
          Model* m = inst.model.get();
          if (!m)
          {
            return;
          }
          if (!m->finishedLoading())
          {
            still_pending.push_back(inst.uid);
            return;
          }
          if (m->lights().empty())
          {
            return;
          }
          reg.m2_uids.push_back(inst.uid);
          collect_m2_lights(inst);
        });
        reg.pending_m2.swap(still_pending);
      }
      light_storage.visit_wmo_uids(reg.wmo_uids, [&] (WMOInstance& wmo)
      {
        bool has_lights = false, fully_classified = true;
        collect_wmo_lights(wmo, has_lights, fully_classified);
      });
      if (!reg.pending_wmo.empty())
      {
        std::vector<std::uint32_t> still_pending;
        still_pending.reserve(reg.pending_wmo.size());
        light_storage.visit_wmo_uids(reg.pending_wmo, [&] (WMOInstance& wmo)
        {
          bool has_lights = false, fully_classified = true;
          collect_wmo_lights(wmo, has_lights, fully_classified);
          if (has_lights)
          {
            reg.wmo_uids.push_back(wmo.uid);
          }
          else if (!fully_classified)
          {
            still_pending.push_back(wmo.uid);
          }
        });
        reg.pending_wmo.swap(still_pending);
      }
    }

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
        // color.w = 1 marks a WMO MOLT light: the m2 shader skips those for UNITS (client rule).
        _lighting_ubo_data.PointLightColor[i] = glm::vec4(collected[i].color, collected[i].is_molt ? 1.f : 0.f);
      }
      else
      {
        _lighting_ubo_data.PointLightPos[i] = glm::vec4(0.f);
        _lighting_ubo_data.PointLightColor[i] = glm::vec4(0.f);
      }
    }
    } // end if (rebuild_point_lights) -- else frames reuse last-built cached point-light UBO set
  }

  // 3.3.5a dynamic shadow map: the matrix of the LAST rendered map (end-of-frame pass) + params.
  // x = effective level (0 disables sampling entirely -- also while no map has been rendered yet),
  // y = shadowed-light floor (residual sun in shadow; the client's Mod pass leaves ~35%),
  // z = 1/map size for the PCF taps.
  _lighting_ubo_data.ShadowMatrix = _shadow_matrix;
  _lighting_ubo_data.ShadowParams = glm::vec4(
      (_shadow_map_valid && _shadow_quality >= 1) ? static_cast<float>(_shadow_quality) : 0.0f,
      0.35f,
      _shadow_size > 0 ? 1.0f / static_cast<float>(_shadow_size) : 0.0f,
      (_shadow_env_valid && _shadow_quality >= 3) ? 1.0f : 0.0f);
  _lighting_ubo_data.ShadowCenterRange = _shadow_center_range;
  _lighting_ubo_data.ShadowMatrixEnv = _shadow_env_matrix;
  _lighting_ubo_data.ShadowEnvCenterRange = _shadow_env_center_range;

  gl.bindBuffer(GL_UNIFORM_BUFFER, _lighting_ubo);
  gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(OpenGL::LightingUniformBlock), &_lighting_ubo_data);
  _point_lights_scoped = false; // full-block upload == global set is live
}

namespace
{
  // The point-light region of the lighting UBO (params + pos[] + color[]) sits at the tail of the
  // block; per-group scoping rewrites ONLY this region so fog/ambient/etc. stay untouched.
  constexpr std::size_t point_light_region_offset = offsetof(OpenGL::LightingUniformBlock, PointLightParams);
  constexpr std::size_t point_light_region_size =
      sizeof(glm::vec4) * (1 + 2 * OpenGL::MAX_POINT_LIGHTS);
}

void WorldRender::setWmoGroupPointLights(WMO const* wmo, std::vector<int16_t> const& light_refs,
                                         glm::mat4x4 const& transform, glm::vec3 const& camera_pos)
{
  // Per-room lighting (client MOLR semantics): this group's surfaces are lit ONLY by the MOLT
  // lights its MOLR chunk references -- no light bleeding in from the next hall, and dense
  // interiors (Ironforge, 209 MOLT lights) get the RIGHT lights per room instead of the global
  // nearest-16. An interior group with no refs authored gets no dynamic point lights (its look
  // comes from baked MOCV, exactly like the client).
  struct Scoped { glm::vec4 pos; glm::vec4 color; float dist2; };
  std::vector<Scoped> scoped;
  scoped.reserve(light_refs.size());

  for (int16_t ref : light_refs)
  {
    if (ref < 0 || static_cast<std::size_t>(ref) >= wmo->lights.size())
    {
      continue;
    }
    auto const& wl = wmo->lights[ref];
    glm::vec3 const world = glm::vec3(transform * glm::vec4(wl.pos, 1.0f));
    glm::vec3 const col = glm::vec3(wl.fcolor) * std::max(wl.intensity, 0.0f);
    float const radius = glm::clamp(wl.r, 6.0f, 60.0f); // same MOLT radius rule as the global pool
    if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z)
        || !std::isfinite(radius) || !std::isfinite(col.r) || !std::isfinite(col.g) || !std::isfinite(col.b))
    {
      continue;
    }
    glm::vec3 const d = world - camera_pos;
    scoped.push_back({glm::vec4(world, radius), glm::vec4(col, 0.f), glm::dot(d, d)});
  }

  // Rooms rarely reference more than 16 lights; when they do, keep the nearest to the camera.
  if (scoped.size() > static_cast<std::size_t>(OpenGL::MAX_POINT_LIGHTS))
  {
    std::sort(scoped.begin(), scoped.end(),
              [] (Scoped const& a, Scoped const& b) { return a.dist2 < b.dist2; });
  }

  struct Region
  {
    glm::vec4 params;
    glm::vec4 pos[OpenGL::MAX_POINT_LIGHTS];
    glm::vec4 color[OpenGL::MAX_POINT_LIGHTS];
  } region;

  int const count = std::min(static_cast<int>(scoped.size()), OpenGL::MAX_POINT_LIGHTS);
  region.params = glm::vec4(static_cast<float>(count), 0.f, 0.f, 0.f);
  for (int i = 0; i < OpenGL::MAX_POINT_LIGHTS; ++i)
  {
    region.pos[i] = i < count ? scoped[i].pos : glm::vec4(0.f);
    region.color[i] = i < count ? scoped[i].color : glm::vec4(0.f);
  }

  static_assert(sizeof(Region) == point_light_region_size, "point-light UBO region layout mismatch");

  gl.bindBuffer(GL_UNIFORM_BUFFER, _lighting_ubo);
  gl.bufferSubData(GL_UNIFORM_BUFFER, point_light_region_offset, point_light_region_size, &region);
  _point_lights_scoped = true;
}

void WorldRender::restoreGlobalPointLights()
{
  if (!_point_lights_scoped)
  {
    return;
  }
  // _lighting_ubo_data still holds this frame's global nearest-16 set (updateLightingUniformBlock
  // fills it and never sees the per-group swaps) -- re-upload just the point-light region from it.
  gl.bindBuffer(GL_UNIFORM_BUFFER, _lighting_ubo);
  gl.bufferSubData(GL_UNIFORM_BUFFER, point_light_region_offset, point_light_region_size,
                   reinterpret_cast<char const*>(&_lighting_ubo_data) + point_light_region_offset);
  _point_lights_scoped = false;
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
  _lighting_ubo_data.ShadowParams = glm::vec4(0.f);     // no dynamic shadows on the minimap
  _lighting_ubo_data.ShadowCenterRange = glm::vec4(0.f);

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
