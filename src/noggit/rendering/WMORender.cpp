// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "WMORender.hpp"
#include <noggit/WMO.h>
#include <noggit/Log.h>
#include <noggit/rendering/WorldRender.hpp>

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

    // 1. Which interior group is the camera standing in? (smallest containing box)
    int start = -1;
    float best_vol = std::numeric_limits<float>::max();
    for (std::size_t g = 0; g < ng; ++g)
    {
      if (!groups[g].is_indoor()) { continue; }
      glm::vec3 const mn = glm::min(groups[g].BoundingBoxMin, groups[g].BoundingBoxMax);
      glm::vec3 const mx = glm::max(groups[g].BoundingBoxMin, groups[g].BoundingBoxMax);
      if (cam_local.x >= mn.x && cam_local.x <= mx.x
        && cam_local.y >= mn.y && cam_local.y <= mx.y
        && cam_local.z >= mn.z && cam_local.z <= mx.z)
      {
        glm::vec3 const s = mx - mn;
        float const vol = s.x * s.y * s.z;
        if (vol < best_vol) { best_vol = vol; start = static_cast<int>(g); }
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
      if (!groups[g].is_indoor() || groups[g].is_exterior()) { visible[g] = 1; } // shell / outdoor: always
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
)
{

  if (!_wmo->finishedLoading())
  [[unlikely]]
  {
    return;
  }

  wmo_shader.uniform("ambient_color",glm::vec3(_wmo->ambient_light_color));

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

  for (auto* group : visible_groups)
  {
    if (world_renderer)
    {
      // Per-room lighting (client MOLR semantics): interior groups are lit ONLY by the MOLT lights
      // their MOLR chunk references -- no torch bleeding through walls, and dense interiors get the
      // RIGHT lights per room instead of the global nearest-16. Exterior / exterior-lit groups keep
      // the frame's global light set (they live under the outdoor sky like terrain does).
      if (group->is_indoor() && !group->is_exterior_lit())
      {
        world_renderer->setWmoGroupPointLights(_wmo, group->light_refs(), transform_matrix, camera);
      }
      else
      {
        world_renderer->restoreGlobalPointLights();
      }
    }

    group->renderer()->draw(wmo_shader
        , frustum
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

  if (wmo_liquid_program && liquid_texture_manager)
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

    for (auto* group : visible_groups)
    {
      group->drawLiquid(transform_matrix,
                        wmo_liquid_shader,
                        *liquid_texture_manager,
                        draw_fog,
                        animtime);
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
