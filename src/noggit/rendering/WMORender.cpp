// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "WMORender.hpp"
#include <noggit/WMO.h>
#include <math/ray.hpp>
#include <noggit/Log.h>
#include <noggit/rendering/WorldRender.hpp>
#include <noggit/project/CurrentProject.hpp>

#include <cstdlib>
#include <algorithm>
#include <deque>
#include <limits>
#include <set>
#include <string>
#include <vector>
#include <QtCore/QElapsedTimer>
#include <QtCore/QSettings>

using namespace Noggit::Rendering;

namespace
{
  // Cached QSettings reads for the draw loop (checklist 20.3): QSettings construction touches the
  // registry/ini backend, and these were fetched per WMO per frame. Refresh every 500 ms -- the
  // Settings-panel sliders still take effect within half a second, with zero per-draw cost.
  float cached_water_transparency()
  {
    static QElapsedTimer timer;
    static float value = 1.0f;
    if (!timer.isValid() || timer.elapsed() > 500)
    {
      value = QSettings().value("water/transparency", 1.0f).toFloat();
      timer.restart();
    }
    return value;
  }

  bool cached_wmo_water_stencil()
  {
    static QElapsedTimer timer;
    static bool value = true;
    if (!timer.isValid() || timer.elapsed() > 500)
    {
      value = QSettings().value("water/wmo_stencil", true).toBool();
      timer.restart();
    }
    return value;
  }

  bool cached_wmo_portal_culling()
  {
    static QElapsedTimer timer;
    static bool value = true;
    if (!timer.isValid() || timer.elapsed() > 500)
    {
      value = QSettings().value("render/wmo_portal_culling", true).toBool();
      timer.restart();
    }
    return value;
  }

  // Portal-visibility culling. Returns a per-group "draw this group" flag. When the camera is inside an
  // interior group, it walks the WMO's portal graph (MOPV/MOPT/MOPR) from that group, reducing the view
  // rectangle through each portal, and only marks the groups actually reachable/visible through the doors.
  // CONSERVATIVE by construction -- it returns all-true (draw everything) whenever it can't be sure:
  // no portal data, camera not inside any interior group, or a portal straddling the near plane. So it can
  // only ever REMOVE guaranteed-invisible interior rooms; it never hides geometry that might be on screen.
  std::vector<uint8_t> portal_visible_groups(WMO* wmo
                                            , glm::mat4x4 const& model_view
                                            , glm::mat4x4 const& projection
                                            , glm::mat4x4 const& transform
                                            , glm::vec3 const& camera_world)
  {
    auto const& groups = wmo->groups;
    std::size_t const ng = groups.size();
    std::vector<uint8_t> visible(ng, 1); // SAFE DEFAULT: draw everything

    if (ng == 0 || wmo->_portal_refs.empty() || wmo->_portal_info.empty() || wmo->_portal_vertices.empty())
    {
      return visible;
    }

    glm::vec3 const cam_local = glm::vec3(glm::inverse(transform) * glm::vec4(camera_world, 1.0f));

    // 1. Which interior group is the camera standing IN? [fix 2026-08-17] A point-in-AABB test (even with
    // an enclose margin) mis-fires for a 3rd-person BOOM camera outside a building: the axis-aligned
    // interior-group AABB is LOOSE, so a camera hovering over the street behind the player -- or pressed
    // against / clipping an exterior wall -- lands inside the box and portal-culled the whole structure
    // (floor + walls) while the player was OUTSIDE. Port the client's discriminator (FUN_007d59b0, see
    // noggit-wmo-portal-culling-client-valid): cast a DOWNWARD ray from the camera; you are genuinely inside
    // a room only if the NEAREST surface directly below the camera belongs to an INTERIOR group (its floor).
    // Over open street the nearest thing below is terrain -> not a WMO group -> no hit -> outside. Against an
    // exterior wall or above the roof the nearest surface below is an EXTERIOR group -> outside. Fails toward
    // start = -1 (draw everything), so it never hides on-screen geometry. The downward ray's grid query is
    // just the camera's XZ column, so this is ~1 cheap ray-cast per group the camera stands over.
    int start = -1;
    {
      math::ray const down(cam_local, glm::vec3(0.0f, -1.0f, 0.0f));
      float constexpr max_below = 500.0f;
      float nearest = std::numeric_limits<float>::max();
      for (std::size_t g = 0; g < ng; ++g)
      {
        glm::vec3 const mn = glm::min(groups[g].BoundingBoxMin, groups[g].BoundingBoxMax);
        glm::vec3 const mx = glm::max(groups[g].BoundingBoxMin, groups[g].BoundingBoxMax);
        // A straight-down ray can only hit this group if the camera's XZ sits over its footprint and the
        // camera is at/above the group's top.
        if (cam_local.x < mn.x || cam_local.x > mx.x || cam_local.z < mn.z || cam_local.z > mx.z) { continue; }
        if (cam_local.y < mn.y) { continue; }
        std::vector<float> hits;
        groups[g].intersect(down, &hits, max_below);
        for (float const h : hits)
        {
          if (h > 0.01f && h < nearest) // nearest surface below the camera
          {
            nearest = h;
            start = (groups[g].is_indoor() && !groups[g].is_exterior()) ? static_cast<int>(g) : -1;
          }
        }
      }
    }

    static QElapsedTimer portal_dbg;
    bool const log_now = (!portal_dbg.isValid() || portal_dbg.elapsed() > 1000);
    if (log_now)
    {
      portal_dbg.restart();
      LogDebug << "[portal] wmo groups=" << ng << " portalRefs=" << wmo->_portal_refs.size()
               << " portalVerts=" << wmo->_portal_vertices.size() << " camLocal=(" << cam_local.x << ","
               << cam_local.y << "," << cam_local.z << ") camStart=" << start << std::endl;
    }

    if (start < 0)
    {
      return visible; // camera is outside every interior group (exterior view) -> draw everything
    }

    // 2. Start from "only exterior groups + the camera's room", then flood through visible portals.
    std::fill(visible.begin(), visible.end(), uint8_t(0));
    for (std::size_t g = 0; g < ng; ++g)
    {
      // Always-visible groups (never portal-culled), matching the client's interior PVS marking:
      //   * exterior shell / outdoor groups (the flood never needs them)
      //   * SKYBOX groups (MOGP 0x40000): the interior sky/backdrop mesh, which the client always draws
      //   * ALWAYS_DRAW groups (MOGP 0x10000): e.g. Stormwind's cathedral banners/glass (gid 18970+) --
      //     unreachable via portals (no incoming portal ref) yet flagged always-draw, so the client shows them.
      // DO NOT force-draw a group merely because portal_count==0. portal_count is a group's OUTGOING portals;
      // a group with zero of them is STILL reached as a flood TARGET when another room's portal points at it
      // (the `visible[neighbour]=1` marking below). Exempting portal_count==0 kept Stormwind's portal-culled
      // interior rooms -- and the cathedral -- permanently drawn even from outside/below. A genuinely
      // unreachable, non-always-draw interior room (e.g. a custom group) draws only when the camera stands in
      // it (start=g -> visible[g]=1), exactly like the client. Timbermaw's backdrop is handled by the
      // start-group down-ray finder above, NOT here (none of its groups are skybox/always-draw/portal-less).
      if (!groups[g].is_indoor() || groups[g].is_exterior() || groups[g].has_skybox()
          || groups[g].always_draw())
      {
        visible[g] = 1;
      }
    }

    // Project portal vertices EXACTLY like wmo_vert.glsl does, or the screen rects are wrong and it culls
    // visible rooms. The shader is CAMERA-RELATIVE: world = transform*local; then project (world - camera)
    // with model_view's translation column zeroed (the jitter fix). A naive projection*model_view*transform
    // here put portals in the wrong place on screen.
    glm::mat4x4 view_rot = model_view;
    view_rot[3][0] = 0.0f; view_rot[3][1] = 0.0f; view_rot[3][2] = 0.0f;
    glm::mat4x4 const proj_view_rot = projection * view_rot;
    glm::vec4 const full(-1.0f, -1.0f, 1.0f, 1.0f); // NDC rect (minx, miny, maxx, maxy)

    std::vector<glm::vec4> explored(ng, glm::vec4(1e30f, 1e30f, -1e30f, -1e30f));
    std::deque<std::pair<int, glm::vec4>> queue;
    visible[start] = 1;
    explored[start] = full;
    queue.emplace_back(start, full);

    int guard = 0;
    while (!queue.empty() && guard++ < 8192)
    {
      auto const [g, rect] = queue.front();
      queue.pop_front();

      int const p_begin = groups[g].portal_start();
      int const p_end = p_begin + groups[g].portal_count();
      for (int r = p_begin; r < p_end; ++r)
      {
        if (r < 0 || r >= static_cast<int>(wmo->_portal_refs.size())) { continue; }
        WMOPR const& ref = wmo->_portal_refs[r];
        int const neighbour = ref.group;
        if (neighbour < 0 || neighbour >= static_cast<int>(ng)) { continue; }
        if (ref.portal < 0 || ref.portal >= static_cast<int>(wmo->_portal_info.size())) { continue; }

        // Project the portal polygon to an NDC rectangle. Near-plane straddle -> stay conservative.
        auto const& info = wmo->_portal_info[ref.portal];
        glm::vec4 prect(1e30f, 1e30f, -1e30f, -1e30f);
        bool any_behind = false, any_front = false;
        for (int v = 0; v < info.vertex_count; ++v)
        {
          int const vidx = info.base_vertex + v;
          if (vidx < 0 || vidx >= static_cast<int>(wmo->_portal_vertices.size())) { continue; }
          glm::vec3 const world = glm::vec3(transform * glm::vec4(wmo->_portal_vertices[vidx], 1.0f));
          glm::vec4 const clip = proj_view_rot * glm::vec4(world - camera_world, 1.0f);
          if (clip.w <= 1e-4f) { any_behind = true; continue; }
          any_front = true;
          glm::vec2 const ndc(clip.x / clip.w, clip.y / clip.w);
          prect.x = std::min(prect.x, ndc.x); prect.y = std::min(prect.y, ndc.y);
          prect.z = std::max(prect.z, ndc.x); prect.w = std::max(prect.w, ndc.y);
        }
        if (!any_front) { continue; } // portal entirely behind the camera -> not visible through it

        glm::vec4 child;
        if (any_behind)
        {
          child = rect; // straddles near plane: can't reduce reliably -> pass full parent rect (safe)
        }
        else
        {
          child = glm::vec4(std::max(rect.x, prect.x), std::max(rect.y, prect.y),
                            std::min(rect.z, prect.z), std::min(rect.w, prect.w));
          if (child.x >= child.z || child.y >= child.w) { continue; } // portal outside current view rect
        }

        visible[neighbour] = 1;
        glm::vec4& ex = explored[neighbour];
        glm::vec4 const uni(std::min(ex.x, child.x), std::min(ex.y, child.y),
                            std::max(ex.z, child.z), std::max(ex.w, child.w));
        if (uni != ex) { ex = uni; queue.emplace_back(neighbour, child); }
      }
    }

    if (log_now)
    {
      int vis = 0; for (auto v : visible) { vis += v; }
      LogDebug << "[portal]   -> drawn=" << vis << " culled=" << (static_cast<int>(ng) - vis)
               << " of " << ng << " groups" << std::endl;
    }

    return visible;
  }
}

WMORender::WMORender(WMO* wmo)
: _wmo(wmo)
{
}

void WMORender::upload()
{

}

void WMORender::unload()
{
  for (auto& group : _wmo->groups)
  {
    group.renderer()->unload();
  }
}

int g_wmo_interior_mod2x = 1;   // mirrored from the GL uniform for the VK WMO shader

void WMORender::draw(OpenGL::Scoped::use_program& wmo_shader
    , OpenGL::program* wmo_liquid_program
    , LiquidTextureManager* liquid_texture_manager
    , glm::mat4x4 const& model_view
    , glm::mat4x4 const& projection
    , glm::mat4x4 const& transform_matrix
    , bool boundingbox
    , math::frustum const& frustum
    , const float& cull_distance
    , const glm::vec3& camera
    , bool // draw_doodads
    , bool draw_fog
    , int animtime
    , bool world_has_skies
    , display_mode display
    , bool interior_only
    , WorldRender* world_renderer
    , std::vector<uint8_t>* out_group_visibility
    , uint16_t doodad_set
)
{
  if (out_group_visibility)
  {
    out_group_visibility->clear(); // empty = all visible (portal culling off / not computed)
  }

  if (!_wmo->finishedLoading())
  [[unlikely]]
  {
    return;
  }

  _vk_fog = VkFogContext{};   // zone fog unless an MFOG sphere applies below
  // [finding 100] VK owns the WMO pass: these uniforms feed a GL shader that issues no draws
  // this frame. The VK feed is built from _vk_fog and the batch table, not from GL uniform state,
  // so skipping them changes nothing that is drawn.
  extern bool g_vk_owns_wmo;   // WMOGroupRender.cpp
  bool const vk_skip_gl_uniforms = g_vk_owns_wmo;
  if (!vk_skip_gl_uniforms)
  wmo_shader.uniform("ambient_color",glm::vec3(_wmo->ambient_light_color));

  // UNIFIED RENDER PATH (MOHD flag 0x2): the client hard-branches on this flag to the additive
  // MapObjU shaders (interior = MOHD ambient + baked MOCV). Set per-WMO from its own flag so stock
  // WMOs (flags 0x0) get 0 and the shader's interior branch stays byte-identical; only the retail
  // ports and stock Stormwind carry the flag. See noggit-wmo-unified-render-path.
  if (!vk_skip_gl_uniforms)
  wmo_shader.uniform("wmo_unified", _wmo->flags.use_unified_render_path ? 1 : 0);

  // While the camera is inside a WMO, its exterior-lit + portal-spill faces are lit from THIS WMO's own
  // interior context (MOHD ambient + baked MOCV), not the outdoor map light -- there is no Light.dbc row
  // positioned inside a city WMO, so the outdoor fallback was flooding Ironforge's building fronts and the
  // gryphon-flight tunnels with Dun Morogh daylight. Viewed from outside (in the world) they keep outdoor
  // lighting. world_renderer is null in the asset-preview, where the camera is never "inside" a WMO.
  if (!vk_skip_gl_uniforms)
  wmo_shader.uniform("camera_inside_wmo",
      (world_renderer && world_renderer->cameraInsideWmo()) ? 1 : 0);

  // Diagnostic: NOGGIT_WMO_DEBUG_MOCV=3 colour-codes each WMO face by its lighting branch (WHITE=unlit,
  // RED=ExteriorLit-outdoor, ORANGE=ExteriorLit-but-inside(fixed), BLUE=portal-spill, GREEN=plain interior;
  // dimmed when the camera is NOT detected inside the WMO). =1 shows raw MOCV (magenta=no MOCV). 0/unset off.
  static int const s_debug_mocv =
      std::getenv("NOGGIT_WMO_DEBUG_MOCV") ? std::atoi(std::getenv("NOGGIT_WMO_DEBUG_MOCV")) : 0;
  if (!vk_skip_gl_uniforms)
  wmo_shader.uniform("debug_mocv", s_debug_mocv);

  // 3.3.5a interior mod2x (tex*MOCV*2). Gated to non-CLASSIC projects behind NOGGIT_335A_WMO_MOD2X, matching
  // the load-side WotLK FixColorVertexAlpha gate in WMO::load_mocv (A/B; interior-lighting rule). 0 = 1.12 x1.
  // DEFAULT-ON for non-CLASSIC (user-confirmed brighter, 2026-07-29); opt out with NOGGIT_NO_335A_WMO_MOD2X=1.
  // Must match the load-side gate in WMO::load_mocv.
  static int const s_wmo_interior_mod2x =
      (std::getenv("NOGGIT_NO_335A_WMO_MOD2X") == nullptr
       && Noggit::Project::CurrentProject::get() != nullptr
       && Noggit::Project::CurrentProject::get()->projectVersion != Noggit::Project::ProjectVersion::CLASSIC) ? 1 : 0;
  if (!vk_skip_gl_uniforms)
  wmo_shader.uniform("wmo_interior_mod2x", s_wmo_interior_mod2x);
  g_wmo_interior_mod2x = s_wmo_interior_mod2x;   // [VULKAN phase D] mirror it to the VK pass

  // [GREENDBG 2026-07-30] temporary: NOGGIT_WMO_DEBUG=1..6 isolates one shading term (see wmo_frag.glsl).
  {
    static int const s_wmo_debug_mode = []
    {
      char const* v = std::getenv("NOGGIT_WMO_DEBUG");
      return (v && *v) ? std::atoi(v) : 0;
    }();
    if (!vk_skip_gl_uniforms)
    wmo_shader.uniform("wmo_debug_mode", s_wmo_debug_mode);
  }

  // Interior tone knobs: shadow FLOOR (no interior face darker than this -> no pure-black voids) + GAIN (overall
  // interior brightness). Live-tunable so contrast can be dialled to the client without a rebuild.
  // Defaults target the client's ~0.5 interior-floor brightness (Goldshire apitrace: floor ~120-150/255).
  static float const s_wmo_interior_floor =
      std::getenv("NOGGIT_335A_INTERIOR_FLOOR") ? static_cast<float>(std::atof(std::getenv("NOGGIT_335A_INTERIOR_FLOOR"))) : 0.10f;
  if (!vk_skip_gl_uniforms)
  wmo_shader.uniform("wmo_interior_floor", s_wmo_interior_floor);
  static float const s_wmo_interior_gain =
      std::getenv("NOGGIT_335A_INTERIOR_GAIN") ? static_cast<float>(std::atof(std::getenv("NOGGIT_335A_INTERIOR_GAIN"))) : 1.30f;
  if (!vk_skip_gl_uniforms)
  wmo_shader.uniform("wmo_interior_gain", s_wmo_interior_gain);

  // (Removed the "wmo_open" outdoor-spill heuristic: it lifted EVERY interior group toward the outdoor
  // ambient whenever the WMO had any exterior group, flooding big mixed WMOs like Ironforge with Dun
  // Morogh daylight. The shader now lights interiors the canonical way -- MOHD ambient + baked MOCV.)

  std::vector<WMOGroup*> visible_groups;
  visible_groups.reserve(_wmo->groups.size());

  // Portal-visibility cull (checklist 20.1): when standing inside this WMO, only draw the interior rooms
  // actually reachable through visible portals from the camera's room. Conservative -- returns all-visible
  // whenever unsure, so it never hides on-screen geometry (see portal_visible_groups). Toggle in Settings.
  bool const use_portals = cached_wmo_portal_culling();
  std::vector<uint8_t> const portal_visible = use_portals
    ? portal_visible_groups(_wmo, model_view, projection, transform_matrix, camera)
    : std::vector<uint8_t>();

  if (out_group_visibility && use_portals)
  {
    *out_group_visibility = portal_visible;
  }

  for (std::size_t gi = 0; gi < _wmo->groups.size(); ++gi)
  {
    auto& group = _wmo->groups[gi];

    if (interior_only && !group.is_indoor())
    {
      continue;
    }

    if (use_portals && !portal_visible[gi])
    {
      continue;
    }

    if (!group.is_visible(transform_matrix, frustum, cull_distance, camera, display))
    {
      continue;
    }

    visible_groups.push_back(&group);
  }

  // Per-region MFOG (trace: wow_cap_kara_cull_fog, per-draw fog attribution): the client draws WMO
  // geometry with the group's authored MFOG (start/end/color VERBATIM -- e.g. kara #6DA3C6 83.3/208.3)
  // IN THE SAME FRAME as terrain keeps the zone fog. There is no global fog switch. The fog fades
  // toward the zone fog at the volume's outer range (the trace's intermediate blend states). Because
  // MFOG start distances (83yd in kara) usually exceed room size, interiors show almost no fog in
  // normal play -- the saturated colour only appears looking through long sightlines (out of bounds).
  glm::vec3 zone_fog_color(0.0f);
  float zone_fog_start_frac = 0.25f, zone_fog_end = 500.0f;
  bool const per_group_fog = world_renderer && draw_fog;
  if (per_group_fog)
  {
    world_renderer->getZoneFog(zone_fog_color, zone_fog_start_frac, zone_fog_end);
  }

  for (auto* group : visible_groups)
  {
    if (per_group_fog)
    {
      glm::vec3 fog_color = zone_fog_color;
      float fog_start_abs = zone_fog_start_frac * zone_fog_end;
      float fog_end = zone_fog_end;

      // CLIENT-EXACT rule (wow.exe @0069de20, note 27; replaces the earlier best-sphere heuristic):
      // a DEFAULT-ONLY WMO (single MFOG entry, e.g. Karazhan's root) keeps the ZONE fog everywhere
      // -- the trace's dominant state. A WMO with placed fogs blends them (farthest -> nearest,
      // w = 1 inside r1, linear to 0 at r2, candidates from THIS group's MOGP fog indices) over
      // the WMO's DEFAULT entry fogs[0] -- NOT over the zone fog.
      glm::vec3 mf_color;
      float mf_end = 0.0f, mf_start = 0.0f;
      // WMO camera-fog DEFAULT-OFF (2026-07-20): route WMO geometry to the ZONE fog (the default set above)
      // so EVERY draw -- WMO walls/floor, terrain, doodads -- reads the SAME fog with nothing per-region to
      // mismatch. noggit can't reproduce the client's portal/BSP camera-group walk (wow.exe FUN_006be250 ->
      // FUN_006b92b0); every AABB/floor proxy re-blued Karazhan's open Malchezaar tower (its own indoor
      // floor sits under the camera) while leaving doodads on zone fog -> the terrain-blue/doodads-gray
      // split. The authored MFOG is fog-free up close (start ~83yd) and shows only over long sightlines, so
      // the zone fog is visually close AND consistent. Interiors that genuinely need their own fog
      // (Ironforge) get it from their dim interior Light.dbc ZONE (RE §9.5) -- the SAME source the terrain
      // reads. NOGGIT_WMO_FOG=1 re-enables the per-group MFOG (only worth it with real portal resolution).
      static bool const s_wmo_mfog = std::getenv("NOGGIT_WMO_FOG") != nullptr;
      bool const mfog_applied = s_wmo_mfog
                             && _wmo->evaluate_camera_fog(*group, transform_matrix, camera,
                                                          world_renderer->cameraInsideWmo(), &mf_color, &mf_end, &mf_start);
      if (mfog_applied)
      {
        float const fog_scale = world_renderer->fogDistanceScale();
        fog_color = mf_color;
        fog_end = mf_end * fog_scale;
        fog_start_abs = mf_start * fog_scale;
      }
      // DIAGNOSTIC (NOGGIT_LIGHT_DEBUG): is the WMO-geometry fog coming from an MFOG sphere override
      // (mfog=1) or the zone fog (mfog=0)? Logs the resulting FOG_END vs the zone fog end, throttled.
      {
        static bool const s_wmf_dbg = std::getenv("NOGGIT_LIGHT_DEBUG") != nullptr;
        static int s_wmf_tick = 0;
        if (s_wmf_dbg && (++s_wmf_tick % 240) == 0)
          LogError << "WMOFOG mfog=" << (mfog_applied ? 1 : 0) << " FOG_END=" << fog_end
                   << " start_abs=" << fog_start_abs << " zone_end=" << zone_fog_end << std::endl;
      }
      // No MFOG sphere in range -> ZONE fog (the default set above), per client RE docs 22/27: WMO geometry
      // is drawn with the zone fog as its dominant state, and a fog sphere applies ONLY while the camera is
      // inside it. There is deliberately NO fogs[0] fallback -- an earlier one (and my re-added Ironforge
      // "fix") painted whole rooms / outdoor doodads near big WMOs (Karazhan Malchezaar) with the anchor/
      // default fog the live client never shows. Ironforge "interior too near" + Icecrown "too foggy" are
      // ZONE-fog problems (light-zone selection / fog-end), fixed in the lighting path, not here.

      float const wmo_fog_start_frac =
          fog_end > 0.001f ? std::clamp(fog_start_abs / fog_end, -5.0f, 0.99f) : 0.25f;
      if (!vk_skip_gl_uniforms)
      wmo_shader.uniform("use_wmo_fog", 1);
      if (!vk_skip_gl_uniforms)
      wmo_shader.uniform("wmo_fog_color", fog_color);
      if (!vk_skip_gl_uniforms)
      wmo_shader.uniform("wmo_fog_end", fog_end);
      if (!vk_skip_gl_uniforms)
      wmo_shader.uniform("wmo_fog_start", wmo_fog_start_frac);
      // [VULKAN phase D] hand the SAME context to the VK feed (read right after this draw)
      _vk_fog.use_wmo_fog = true;
      _vk_fog.color = fog_color;
      _vk_fog.end = fog_end;
      _vk_fog.start_frac = wmo_fog_start_frac;
    }

    if (world_renderer)
    {
      // Per-room lighting (client MOLR semantics): interior groups are lit ONLY by the MOLT lights
      // their MOLR chunk references -- no torch bleeding through walls, and dense interiors get the
      // RIGHT lights per room instead of the global nearest-16. Exterior / exterior-lit groups keep
      // the frame's global light set (they live under the outdoor sky like terrain does).
      if (group->is_indoor() && !group->is_exterior_lit())
      {
        // Modern groups (Shadowlands+) reference the root's MNLD lights through MNLR; older ones MOLT
        // through MOLR (docs/client_re/42 sec 15).
        bool const modern_refs = !group->new_light_refs().empty();
        world_renderer->setWmoGroupPointLights(_wmo,
                                               modern_refs ? _wmo->new_lights : _wmo->lights,
                                               modern_refs ? group->new_light_refs() : group->light_refs(),
                                               transform_matrix, camera, doodad_set);
      }
      else
      {
        world_renderer->restoreGlobalPointLights();
      }
    }

    group->renderer()->draw(wmo_shader
        , frustum
        , transform_matrix
        , cull_distance
        , camera
        , draw_fog
        , world_has_skies
    );
  }

  // Leave the UBO on the global set for everything drawn after this WMO (other WMOs re-scope
  // themselves; M2 / terrain / liquid passes expect the global pool).
  if (world_renderer)
  {
    world_renderer->restoreGlobalPointLights();
  }

  // DEFER to the water phase when we are drawing the world. The WMO pass runs BEFORE the M2/creature
  // passes, so liquid drawn here sits behind everything drawn later -- a creature standing in a WMO
  // canal painted straight over the water surface with no tint. WorldRender flushes the queue after
  // the models, exactly where ADT water already draws. The asset preview has no WorldRender, so it
  // keeps drawing inline below.
  if (wmo_liquid_program && liquid_texture_manager && world_renderer)
  {
    world_renderer->queueWmoLiquid(visible_groups, transform_matrix, interior_only, draw_fog);
  }
  else if (wmo_liquid_program && liquid_texture_manager)
  {
    OpenGL::Scoped::use_program wmo_liquid_shader{*wmo_liquid_program};
    // Dev water-opacity lever (Settings slider -> water/transparency). WMO liquid is a separate
    // shader from ADT, so it needs the uniform set here too (e.g. Timbermaw water is WMO liquid).
    // PERF (checklist 20.3): QSettings hits the registry/ini layer -- constructing one per WMO per
    // frame showed up in the draw loop. Cache the two water settings and refresh twice a second,
    // so the Settings sliders still apply near-instantly with zero per-draw cost.
    wmo_liquid_shader.uniform("water_alpha_mult", cached_water_transparency());

    // Overlapping EXTERIOR water planes (e.g. Timbermaw has 11 stacked planes at the same level)
    // alpha-blend on top of each other and multiply darker. Draw each water pixel only ONCE via
    // the stencil buffer (cleared to 0 per frame in WorldRender): first plane to cover a pixel
    // passes (stencil 0) and writes 1; later planes there fail (stencil == 1) and are skipped.
    // Scoped to exterior water so INTERIOR liquid (e.g. Molten Core lava) is never stencil-tested.
    // Toggleable via Settings (water/wmo_stencil, default on) for dev/debugging.
    bool const dedupe_overlap = !interior_only
                              && cached_wmo_water_stencil();
    if (dedupe_overlap)
    {
      gl.enable(GL_STENCIL_TEST);
      gl.stencilFunc(GL_NOTEQUAL, 1, 0xFF);
      gl.stencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }

    // Exterior liquid = translucent water: it must NOT write depth (see WMOGroup::drawLiquid). The
    // BLEND is deliberately left alone -- water keeps its normal alpha blend and its normal look.
    // Interior liquid (Molten Core lava) is genuinely opaque and keeps writing depth.
    bool const translucent_water = !interior_only;

    for (auto* group : visible_groups)
    {
      group->drawLiquid(transform_matrix,
                        wmo_liquid_shader,
                        *liquid_texture_manager,
                        draw_fog,
                        animtime,
                        /*translucent*/ translucent_water);
    }

    if (dedupe_overlap)
    {
      gl.disable(GL_STENCIL_TEST);
    }

  }

  if (boundingbox)
  {
    //OpenGL::Scoped::bool_setter<GL_BLEND, GL_TRUE> const blend;
    //gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (auto& group : _wmo->groups)
    {
      Noggit::Rendering::Primitives::WireBox::getInstance(_wmo->_context).draw( model_view
          , projection
          , transform_matrix
          , {1.0f, 1.0f, 1.0f, 1.0f}
          , group.BoundingBoxMin
          , group.BoundingBoxMax
      );
    }

    Noggit::Rendering::Primitives::WireBox::getInstance(_wmo->_context).draw ( model_view
        , projection
        , transform_matrix
        , {1.0f, 0.0f, 0.0f, 1.0f}
        , glm::vec3(_wmo->extents[0].x, _wmo->extents[0].z, -_wmo->extents[0].y)
        , glm::vec3(_wmo->extents[1].x, _wmo->extents[1].z, -_wmo->extents[1].y)
    );

  }
}

bool WMORender::drawSkybox(const glm::mat4x4& model_view, const glm::vec3& camera_pos,
                           OpenGL::Scoped::use_program& m2_shader, const math::frustum& frustum,
                           const float& cull_distance, int animtime, bool draw_particles, glm::vec3 aabb_min,
                           glm::vec3 aabb_max,
                           const std::map<int, std::pair<glm::vec3, glm::vec3>>& group_extents) const
{
  if (!_wmo->skybox || !math::is_inside_of(camera_pos,aabb_min, aabb_max))
  {
    return false;
  }

  for (int i=0; i < _wmo->groups.size(); ++i)
  {
    auto const& g = _wmo->groups[i];

    if (!g.has_skybox())
    {
      continue;
    }

    auto extent_it = group_extents.find(i);
    if (extent_it == group_extents.end())
    {
      continue;
    }

    auto& extent(extent_it->second);

    if (math::is_inside_of(camera_pos, extent.first, extent.second))
    {
      ModelInstance sky(_wmo->skybox.value()->file_key().filepath(), _wmo->_context);
      sky.pos = camera_pos;
      sky.scale = 2.f;
      sky.recalcExtents();

      OpenGL::M2RenderState model_render_state;
      model_render_state.tex_arrays = {0, 0};
      model_render_state.tex_indices = {0, 0};
      model_render_state.tex_unit_lookups = {-1, -1};
      gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      gl.disable(GL_BLEND);
      gl.depthMask(GL_TRUE);
      m2_shader.uniform("blend_mode", 0);
      m2_shader.uniform("unfogged", static_cast<int>(model_render_state.unfogged));
      m2_shader.uniform("unlit",  static_cast<int>(model_render_state.unlit));
      m2_shader.uniform("tex_unit_lookup_1", 0);
      m2_shader.uniform("tex_unit_lookup_2", 0);
      m2_shader.uniform("masked_additive", 0);
      m2_shader.uniform("pixel_shader", 0);

      _wmo->skybox->get()->renderer()->draw(model_view, sky, m2_shader, model_render_state, frustum, cull_distance, camera_pos, animtime, display_mode::in_3D);

      return true;
    }
  }

  return false;
}
