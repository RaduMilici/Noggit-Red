#pragma once
// The NPC/GameObject 3D preview with an orbit camera: shows a creature as the world draws it, with its
// helm, shoulders and weapons. Used by the creature editor's picker and Creator's NPC editors.
#include <noggit/ui/tools/AssetBrowser/ModelView.hpp>
#include <noggit/World.h>
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <math/trig.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <QSettings>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <vector>

namespace Noggit::Ui::Tools
{
// Bind-pose attachment lookup for the picker preview (copy of WorldRender's find_attachment_def:
// direct lookup, classic-layout sanity fallback scan).
inline ModelAttachmentDef const* preview_find_attachment_def(Model const* model, int attachment_id)
{
  if (!model || attachment_id < 0
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

// NPC/GO picker 3D preview with an ORBIT camera: the camera hangs on a fixed "pole" aimed at the
// model's center -- dragging (either button) spins around the model (yaw/pitch), the wheel zooms
// along the pole, and every new model auto-frames as close as the near plane and the frustum
// allow. The base class's free-fly camera (WASD + look) is fully disabled here; the asset
// browser keeps it.
class CreaturePreviewModelViewer final : public Noggit::Ui::Tools::AssetBrowser::ModelViewer
{
public:
  explicit CreaturePreviewModelViewer(QWidget* parent = nullptr)
    : Noggit::Ui::Tools::AssetBrowser::ModelViewer(parent, Noggit::NoggitRenderContext::ASSET_BROWSER)
  {
    // The WORLD view gets its antialiasing from its own multisampled offscreen FBO
    // (render/msaa, WorldRender::setupBloom) -- not from the widget framebuffer -- so preview
    // widgets were rendering with ZERO samples and every model edge aliased. Request the same
    // MSAA level on this widget's backing framebuffer (must happen before the widget is realized).
    {
      int msaa = QSettings().value("render/msaa", 4).toInt();
      // Dev-only override for the VK parity harness: GL renders 4x MSAA by default while the VK
      // backend is still single-sampled, so every silhouette differs. NOGGIT_MSAA isolates that.
      if (char const* env_msaa = std::getenv("NOGGIT_MSAA"))
        msaa = std::atoi(env_msaa);
      if (msaa != 0 && msaa != 2 && msaa != 4 && msaa != 8)
      {
        msaa = 4;
      }
      if (msaa > 0)
      {
        QSurfaceFormat fmt = format();
        fmt.setSamples(msaa);
        setFormat(fmt);
      }
    }

    // Re-frame on widget resizes (the fit depends on the aspect ratio); keep the user's zoom
    // unless they were sitting at the default.
    connect(this, &Noggit::Ui::Tools::AssetBrowser::ModelViewer::resized, [this]
    {
      bool const at_default_zoom = std::abs(_orbit_distance - _fit_distance) < 0.01f;
      refitCamera(at_default_zoom);
    });
  }

  std::function<void()> on_double_click;

  void setModel(std::string const& filename) override
  {
    _preview_attachment_ids.clear(); // the base setModel replaces the instance list
    Noggit::Ui::Tools::AssetBrowser::ModelViewer::setModel(filename);
    refitCamera(true);
  }

  void setCreatureSpawnPreview(World& world, World::CreatureSpawnOverlay const& spawn)
  {
    setModel(spawn.model_path);
    if (_model_instances.empty())
    {
      return;
    }

    auto& instance = _model_instances.front();
    instance.scale = std::clamp(spawn.template_scale * spawn.model_scale,
                                ModelInstance::min_scale(),
                                ModelInstance::max_scale());
    instance.dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
    instance.updateTransformMatrix();
    world.applyCreatureSpawnModelAppearance(spawn, instance, _context);
    instance.recalcExtents();

    // Attachments (helm / shoulders / weapons): the world draws these as separate models on the
    // body's attachment points -- without them a helmeted NPC previews BALD (the helmet rule hid
    // the hair, and nothing drew the helmet: Lakeshire Guard 10037). Bind pose: bone matrices are
    // identity, so the point is just parent_transform x translate(fixCoordSystem(attachment.pos)).
    for (auto const& spec : world.resolveCreaturePreviewAttachments(spawn))
    {
      try
      {
        auto& att = _model_instances.emplace_back(BlizzardArchive::Listfile::FileKey(spec.model_path), _context);
        att.model->wait_until_loaded();
        if (att.model->loading_failed())
        {
          _model_instances.pop_back();
          continue;
        }
        for (auto const& tex : spec.texture_overrides)
        {
          att.setReplaceTexture(tex.first, tex.second);
        }
        // Express the placement through pos/dir/scale (NOT setTransformMatrix): the draw path's
        // lazy recalcExtents rebuilds the matrix from these, which wiped a directly-set matrix
        // and dropped every attachment to the body's origin (helmets at the feet). For this
        // rigid composition the two forms are identical: body x T(att) == T(body x att_point) x
        // R(body yaw) x S(body scale).
        auto& body = _model_instances.front();
        glm::vec3 att_point = body.pos;
        if (auto const* def = preview_find_attachment_def(body.model.get(), spec.attachment_id))
        {
          att_point = glm::vec3(body.transformMatrix() * glm::vec4(fixCoordSystem(def->pos), 1.0f));
        }
        att.pos = att_point;
        att.dir = body.dir;
        att.scale = body.scale;
        att.updateTransformMatrix();
        att.recalcExtents();
        _preview_attachment_ids.push_back(spec.attachment_id);
      }
      catch (...)
      {
        // missing/broken attachment model -> preview the body alone
      }
    }

    // Face the creature from a three-quarter front view, slightly above: the camera's look yaw
    // is opposite the model's facing.
    _orbit_yaw = spawn.orientation + 205.0f;
    _orbit_pitch = 12.0f;
    refitCamera(true);
  }

protected:
  // attachment_id per extra instance (_model_instances[1..]), parallel order
  std::vector<int> _preview_attachment_ids;

  // Animate the attachments with the body's skeleton: re-anchor each on its parent bone's CURRENT
  // matrix every frame, exactly like the world's attachment draw -- the bind-pose-only placement
  // left helmets/weapons frozen mid-air while the body idled. Uses the bone matrices the body's
  // previous draw computed (one frame of lag, invisible). setRenderAnchor feeds the shader
  // directly, so the lazy extent/transform rebuilds can't clobber it.
  void tick(float dt) override
  {
    Noggit::Ui::Tools::AssetBrowser::ModelViewer::tick(dt);
    if (_model_instances.size() < 2 || _preview_attachment_ids.empty())
    {
      return;
    }
    auto& body = _model_instances.front();
    if (!body.model->finishedLoading() || body.model->bone_matrices.empty())
    {
      return;
    }
    for (std::size_t i = 1; i < _model_instances.size() && i - 1 < _preview_attachment_ids.size(); ++i)
    {
      auto& att = _model_instances[i];
      auto const* def = preview_find_attachment_def(body.model.get(), _preview_attachment_ids[i - 1]);
      if (!def)
      {
        continue;
      }
      glm::mat4x4 att_local = glm::translate(glm::mat4x4(1.0f), fixCoordSystem(def->pos));
      if (def->bone >= 0 && static_cast<std::size_t>(def->bone) < body.model->bone_matrices.size())
      {
        att_local = body.model->bone_matrices[def->bone] * att_local;
      }
      glm::mat4x4 const world = body.transformMatrix() * att_local;
      glm::mat4x4 rel = world;
      rel[3] = glm::vec4(glm::vec3(world[3]) - body.pos, 1.0f);
      att.setRenderAnchor(body.pos, rel);
    }
  }

  // --- orbit ("pole") camera state ---
  float _orbit_yaw = 25.0f;       // camera look yaw, degrees
  float _orbit_pitch = 12.0f;     // degrees above horizontal
  float _orbit_distance = 12.0f;  // pole length
  float _fit_distance = 12.0f;    // the auto-framed default zoom
  float _scene_radius = 1.0f;     // fit-box worst-case horizontal half-extent
  float _scene_half_height = 1.0f;
  float _orbit_height_offset = 0.0f; // vertical pivot pan (middle-drag); pole stays centered in XZ
  glm::vec3 _orbit_center = glm::vec3(0.0f);
  bool _orbiting = false;
  bool _panning = false;

  void applyOrbitCamera()
  {
    _orbit_pitch = std::clamp(_orbit_pitch, -80.0f, 80.0f);
    _camera.yaw(math::degrees(_orbit_yaw));
    _camera.pitch(math::degrees(_orbit_pitch));
    // The camera always LOOKS at the (vertically pannable) pivot; backing up along its own view
    // direction is the pole (the same trick PreviewRenderer::resetCamera uses to frame a scene).
    _camera.position = _orbit_center + glm::vec3(0.0f, _orbit_height_offset, 0.0f);
    _camera.move_forward_factor(-1.0f, _orbit_distance);
  }

  // The box the auto-framing fits. The instance extents union the header's anim-padded bounding
  // box with the collision box (weapon-swing/effect space -- often 2-3x the visible mesh), which
  // framed models tiny in lots of empty space; and header boxes themselves are unreliable
  // per-model (dragonkin/whelps author huge or degenerate ones -- The Beast, Lost Whelp,
  // Rivendare all framed far). Ground truth = the MESH: the bind-pose vertex AABB transformed by
  // the instance (Model::_vertices are already in the noggit frame from load-time
  // fixCoordSystem). Fallbacks: collision box -> bounding box -> scene extents (WMO previews).
  std::vector<glm::vec3> fitExtents()
  {
    if (!_model_instances.empty())
    {
      // front() = the previewed BODY (attachment instances are appended after it)
      auto const& inst = _model_instances.front();
      if (inst.model->finishedLoading() && !inst.model->loading_failed())
      {
        glm::mat4x4 const mat = inst.transformMatrix();

        if (!inst.model->_vertices.empty())
        {
          glm::vec3 wmin(std::numeric_limits<float>::max());
          glm::vec3 wmax(std::numeric_limits<float>::lowest());
          std::size_t used = 0;
          for (auto const& v : inst.model->_vertices)
          {
            if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y) || !std::isfinite(v.position.z))
            {
              continue;
            }
            glm::vec3 const world = glm::vec3(mat * glm::vec4(v.position, 1.0f));
            wmin = glm::min(wmin, world);
            wmax = glm::max(wmax, world);
            ++used;
          }
          if (used > 2 && wmin.x < wmax.x && wmin.y < wmax.y && wmin.z < wmax.z)
          {
            return {wmin, wmax};
          }
        }

        auto const& hdr = inst.model->header;
        auto const finite_box = [](glm::vec3 const& a, glm::vec3 const& b)
        {
          return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z)
              && std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.z)
              && a.x < b.x && a.y < b.y && a.z < b.z;
        };
        glm::vec3 bmin(0.0f), bmax(0.0f);
        bool have_box = false;
        if (finite_box(hdr.collision_box_min, hdr.collision_box_max))
        {
          bmin = hdr.collision_box_min;
          bmax = hdr.collision_box_max;
          have_box = true;
        }
        if (!have_box && finite_box(hdr.bounding_box_min, hdr.bounding_box_max))
        {
          bmin = hdr.bounding_box_min;
          bmax = hdr.bounding_box_max;
          have_box = true;
        }
        if (have_box)
        {
          glm::vec3 wmin(std::numeric_limits<float>::max());
          glm::vec3 wmax(std::numeric_limits<float>::lowest());
          for (int i = 0; i < 8; ++i)
          {
            glm::vec3 const corner((i & 1) ? bmax.x : bmin.x,
                                   (i & 2) ? bmax.y : bmin.y,
                                   (i & 4) ? bmax.z : bmin.z);
            glm::vec3 const world = glm::vec3(mat * glm::vec4(misc::transform_model_box_coords(corner), 1.0f));
            wmin = glm::min(wmin, world);
            wmax = glm::max(wmax, world);
          }
          return {wmin, wmax};
        }
      }
    }
    return calcSceneExtents();
  }

  // Recompute the model's fit box and the auto-framed distance. reset_zoom snaps the pole back
  // to the default fit (and clears the vertical pan); otherwise the current zoom is kept.
  void refitCamera(bool reset_zoom)
  {
    auto const extents = fitExtents();
    auto const valid = [](glm::vec3 const& v)
    {
      return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    glm::vec3 half(1.0f);
    if (!valid(extents[0]) || !valid(extents[1])
        || extents[0].x > extents[1].x
        || extents[0].y > extents[1].y
        || extents[0].z > extents[1].z)
    {
      _orbit_center = glm::vec3(0.0f);
    }
    else
    {
      _orbit_center = (extents[0] + extents[1]) * 0.5f;
      half = glm::max((extents[1] - extents[0]) * 0.5f, glm::vec3(0.05f));
    }
    // Worst-case horizontal half-extent while orbiting = the XZ half-diagonal; vertical = half.y.
    _scene_half_height = half.y;
    _scene_radius = std::max(std::sqrt(half.x * half.x + half.z * half.z), 0.25f);

    // Default zoom = "as close as possible without clipping or cropping": per-axis box fit (a
    // tall thin creature closes in until its HEIGHT fills the frame, instead of a conservative
    // bounding-sphere fit), plus the box's front depth, and never inside the near plane (1.0).
    float const fov_y = _camera.fov()._;
    float const aspect = std::max(aspect_ratio(), 0.2f);
    float const fov_x = 2.0f * std::atan(std::tan(fov_y * 0.5f) * aspect);
    float const dist_v = _scene_half_height / std::max(std::tan(fov_y * 0.5f), 0.05f);
    float const dist_h = _scene_radius / std::max(std::tan(fov_x * 0.5f), 0.05f);
    float fit = (std::max(dist_v, dist_h) + _scene_radius) * 1.03f;
    fit = std::max(fit, _scene_radius + 0.15f);
    if (!std::isfinite(fit))
    {
      fit = _scene_radius * 2.0f + 2.0f;
    }
    _fit_distance = fit;
    if (reset_zoom)
    {
      _orbit_distance = fit;
      _orbit_height_offset = 0.0f;
    }
    else
    {
      _orbit_distance = std::clamp(_orbit_distance, minZoom(), maxZoom());
      _orbit_height_offset = std::clamp(_orbit_height_offset,
                                        -2.0f * _scene_half_height, 2.0f * _scene_half_height);
    }
    applyOrbitCamera();
  }

  // Near plane is 0.05 in the preview projection -- allow zooming nearly to the surface.
  float minZoom() const { return std::max(0.12f, _scene_radius * 0.05f); }
  float maxZoom() const { return std::max(_fit_distance * 5.0f, minZoom() + 1.0f); }

  void mousePressEvent(QMouseEvent* event) override
  {
    if (event->button() == Qt::LeftButton)
    {
      _orbiting = true;
    }
    else if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton)
    {
      _panning = true; // right (or middle) drag = vertical pivot pan
    }
    _last_mouse_pos = event->pos();
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent* event) override
  {
    if (event->button() == Qt::LeftButton)
    {
      _orbiting = false;
    }
    else if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton)
    {
      _panning = false;
    }
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent* event) override
  {
    QLineF const relative_movement(_last_mouse_pos, event->pos());
    if (_orbiting)
    {
      // Grab-the-model sense: dragging right spins the model rightward (camera orbits left).
      _orbit_yaw -= static_cast<float>(relative_movement.dx()) * 0.4f;
      _orbit_pitch += static_cast<float>(relative_movement.dy()) * 0.4f;
      applyOrbitCamera();
    }
    else if (_panning)
    {
      // Vertical pivot pan ONLY (middle-drag): slide the whole pole up/down so a zoomed-in shot
      // can re-center on the head/torso -- the model tracks the cursor 1:1 at the pivot plane.
      float const world_per_pixel =
        2.0f * std::tan(_camera.fov()._ * 0.5f) * _orbit_distance / std::max(height(), 1);
      _orbit_height_offset += static_cast<float>(relative_movement.dy()) * world_per_pixel;
      _orbit_height_offset = std::clamp(_orbit_height_offset,
                                        -2.0f * _scene_half_height, 2.0f * _scene_half_height);
      applyOrbitCamera();
    }
    _last_mouse_pos = event->pos();
    event->accept();
  }

  void wheelEvent(QWheelEvent* event) override
  {
    float const step = event->angleDelta().y() > 0 ? 1.0f / 1.12f : 1.12f;
    _orbit_distance = std::clamp(_orbit_distance * step, minZoom(), maxZoom());
    applyOrbitCamera();
    event->accept();
  }

  // The pole replaces free-fly entirely: no WASD/arrow-key flying in the picker preview.
  void keyPressEvent(QKeyEvent* event) override { event->accept(); }
  void keyReleaseEvent(QKeyEvent* event) override { event->accept(); }

  void mouseDoubleClickEvent(QMouseEvent* event) override
  {
    if (on_double_click)
    {
      on_double_click();
    }
    event->accept();
  }
};
}
