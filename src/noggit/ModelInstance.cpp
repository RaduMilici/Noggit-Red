// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <glm/gtx/quaternion.hpp>
#include <math/bounding_box.hpp>
#include <math/frustum.hpp>

#include <cmath>
#include <glm/glm.hpp>
#include <noggit/Log.h>
#include <noggit/Misc.h> // checkinside
#include <noggit/Model.h> // Model, etc.
#include <noggit/ModelInstance.h>
#include <noggit/WMOInstance.h>
#include <noggit/ContextObject.hpp>
#include <noggit/rendering/Primitives.hpp>
#include <opengl/scoped.hpp>
#include <opengl/shader.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace
{
  bool is_null_texture_reference(std::string filename)
  {
    auto const is_trimmed_char = [](unsigned char character)
    {
      return character == '\0' || std::isspace(character);
    };

    filename.erase(filename.begin(),
                   std::find_if(filename.begin(), filename.end(),
                                [&](unsigned char character) { return !is_trimmed_char(character); }));
    filename.erase(std::find_if(filename.rbegin(), filename.rend(),
                                [&](unsigned char character) { return !is_trimmed_char(character); }).base(),
                   filename.end());

    std::transform(filename.begin(), filename.end(), filename.begin(), [](unsigned char character)
    {
      return static_cast<char>(std::tolower(character));
    });

    std::replace(filename.begin(), filename.end(), '\\', '/');

    if (filename.empty() || filename == "0" || filename == "none" || filename == "null")
    {
      return true;
    }

    if (filename.find('/') == std::string::npos)
    {
      auto const extension_pos = filename.rfind('.');
      if (extension_pos != std::string::npos && filename.substr(0, extension_pos) == "0")
      {
        return filename.substr(extension_pos) == ".blp";
      }
    }

    return false;
  }
}

ModelInstance::ModelInstance(BlizzardArchive::Listfile::FileKey const& file_key
                             , Noggit::NoggitRenderContext context)
  : SceneObject(SceneObjectTypes::eMODEL, context)
  , model(file_key, context)
{
}

ModelInstance::ModelInstance(BlizzardArchive::Listfile::FileKey const& file_key
                             , ENTRY_MDDF const*d, Noggit::NoggitRenderContext context)
  : SceneObject(SceneObjectTypes::eMODEL, context)
  , model(file_key, context)
{
	uid = d->uniqueID;
	pos = glm::vec3(d->pos[0], d->pos[1], d->pos[2]);
    dir = math::degrees::vec3( math::degrees(d->rot[0])._, math::degrees(d->rot[1])._, math::degrees(d->rot[2])._);
	// scale factor - divide by 1024. blizzard devs must be on crack, why not just use a float?
	scale = d->scale / 1024.0f;
  _need_recalc_extents = true;
}

ModelInstance& ModelInstance::operator=(ModelInstance const& other)
{
  if (this == &other)
  {
    return *this;
  }

  model = other.model;
  light_color = other.light_color;
  model_alpha = other.model_alpha;
  size_cat = other.size_cat;
  pos = other.pos;
  dir = other.dir;
  uid = other.uid;
  scale = other.scale;
  extents[0] = other.extents[0];
  extents[1] = other.extents[1];
  _transform_mat_inverted = other._transform_mat_inverted;
  _context = other._context;
  _need_recalc_extents = other._need_recalc_extents;
  _need_gpu_transform_update = other._need_gpu_transform_update;
  _gpu_transform_uid = other._gpu_transform_uid;
  _forced_anim_id = other._forced_anim_id;
  _close_hand_main = other._close_hand_main;
  _close_hand_off = other._close_hand_off;

  _replace_textures.clear();
  for (auto const& pair : other._replace_textures)
  {
    _replace_textures.emplace(pair.first, pair.second);
  }

  _show_geosets = other._show_geosets;
  _visible_geoset_ids = other._visible_geoset_ids;
  _controlled_geoset_families = other._controlled_geoset_families;

  return *this;
}

float ModelInstance::selectionRingRadius() const
{
  if (!model.get() || !model->finishedLoading() || model->loading_failed())
  {
    return 0.5f * scale;
  }
  // EXACT client formula (reverse-engineered from wow.exe): ground selection circle radius =
  // scale * sqrt( sqrt(dx^2 + dy^2) * 0.5 ), where dx,dy are the stand-animation bounding-box extents
  // (Model::selection_base_radius). Byte-exact vs the live client. Falls back to the render footprint,
  // then a fraction of the header bound, only when the stand-anim box is unavailable.
  float r = model->selection_base_radius;
  if (!(r > 0.01f)) { r = model->footprint_radius; }
  if (!(r > 0.01f)) { r = model->header.bounding_box_radius * 0.35f; }
  return r * scale;
}

void ModelInstance::setReplaceTexture(std::size_t texture_type, std::string const& filename)
{
  _replace_textures.erase(texture_type);
  if (is_null_texture_reference(filename))
  {
    return;
  }

  _replace_textures.emplace(std::piecewise_construct,
                            std::forward_as_tuple(texture_type),
                            std::forward_as_tuple(filename, _context));
}


void ModelInstance::draw_box (glm::mat4x4 const& model_view
                             , glm::mat4x4 const& projection
                             , bool is_current_selection
                             )
{
  gl.enable(GL_BLEND);
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  if (is_current_selection)
  {
    Noggit::Rendering::Primitives::WireBox::getInstance(_context).draw ( model_view
      , projection
      , transformMatrix()
      , { 1.0f, 1.0f, 0.0f, 1.0f }
      , misc::transform_model_box_coords(model->header.collision_box_min)
      , misc::transform_model_box_coords(model->header.collision_box_max)
      );

    Noggit::Rendering::Primitives::WireBox::getInstance(_context).draw ( model_view
      , projection
      , transformMatrix()
      , {1.0f, 1.0f, 1.0f, 1.0f}
      , misc::transform_model_box_coords(model->header.bounding_box_min)
      , misc::transform_model_box_coords(model->header.bounding_box_max)
      );

    Noggit::Rendering::Primitives::WireBox::getInstance(_context).draw ( model_view
      , projection
      , glm::mat4x4(1)
      , {0.0f, 1.0f, 0.0f, 1.0f}
      , extents[0]
      , extents[1]
      );
  }
  else
  {
    const glm::vec4 color = _grouped ? glm::vec4(0.5f, 0.5f, 1.0f, 0.5f) : glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    Noggit::Rendering::Primitives::WireBox::getInstance(_context).draw ( model_view
      , projection
      , transformMatrix()
      , color
      , misc::transform_model_box_coords(model->header.bounding_box_min)
      , misc::transform_model_box_coords(model->header.bounding_box_max)
      );
  }
}

std::vector<std::tuple<int, int, int>> ModelInstance::intersect (glm::mat4x4 const& model_view
                              , math::ray const& ray
                              , selection_result* results
                              , int animtime
                              , bool use_collision_mesh
                              )
{
  std::vector<std::tuple<int, int, int>> triangle_indices;
  math::ray subray (_transform_mat_inverted, ray);

  if ( !subray.intersect_bounds ( fixCoordSystem (model->header.bounding_box_min)
                                , fixCoordSystem (model->header.bounding_box_max)
                                )
     )
  {
    return triangle_indices;
  }

  // [game mode] PHYSICS path: the client collides only with the M2's dedicated COLLISION MESH
  // (boundingTriangles). Models without one -- grass, ground clutter, most foliage -- are
  // walk-through, exactly like the game. Static bind-pose mesh: no animate needed either.
  if (use_collision_mesh)
  {
    if (model->finishedLoading() && !model->_collision_indices.empty())
    {
      auto const& cv = model->_collision_vertices;
      auto const& ci = model->_collision_indices;
      for (std::size_t i = 0; i + 2 < ci.size(); i += 3)
      {
        if (auto distance = subray.intersect_triangle(cv[ci[i]], cv[ci[i + 1]], cv[ci[i + 2]]))
        {
          results->emplace_back (*distance * scale, this);
        }
      }
    }
    return triangle_indices;
  }

  for (auto&& result : model->intersect (model_view, subray, animtime))
  {
    //! \todo why is only sc important? these are relative to subray,
    //! so should be inverted by model_matrix?
    results->emplace_back (result.first * scale, this);
    triangle_indices.emplace_back(result.second);
  }
  return triangle_indices;
}


bool ModelInstance::isInFrustum(const math::frustum& frustum)
{
  if (_need_recalc_extents && model->finishedLoading())
  {
    recalcExtents();
  }

  if (!frustum.intersects(extents[1], extents[0]))
    return false;

  return true;
}

bool ModelInstance::isInRenderDist(const float& cull_distance, const glm::vec3& camera, display_mode display)
{
  float dist;

  if (display == display_mode::in_3D)
  {
    dist = glm::distance(camera, pos) - model->rad * scale;
  }
  else
  {
    dist = std::abs(pos.y - camera.y) - model->rad * scale;
  }

  if (dist >= cull_distance)
  {
    return false;
  }

  if (size_cat < 1.f && dist > 300.f)
  {
    return false;
  }
  else if (size_cat < 4.f && dist > 500.f)
  {
    return false;
  }
  else if (size_cat < 25.f && dist > 1000.f)
  {
    return false;
  }

  return true;
}

void ModelInstance::recalcExtents()
{
  if (!model->finishedLoading())
  {
    _need_recalc_extents = true;
    return;
  }

  if (model->loading_failed())
  {
    extents[0] = extents[1] = pos;
    _need_recalc_extents = false;
    return;
  }

  updateTransformMatrix();

  math::aabb const relative_to_model
    ( glm::min ( model->header.collision_box_min, model->header.bounding_box_min)
    , glm::max ( model->header.collision_box_max, model->header.bounding_box_max)
    );

  //! \todo If both boxes are {inf, -inf}, or well, if any min.c > max.c,
  //! the model is bad itself. We *could* detect that case and explicitly
  //! assume {-1, 1} then, to be nice to fuckported models.

  auto corners_in_world = std::vector<glm::vec3>();
  auto transform = misc::transform_model_box_coords;
  auto points = relative_to_model.all_corners();
  for (auto& point : points)
  {
    point = transform(point);
    corners_in_world.push_back(point);
  }
 
  auto rotated_corners_in_world = std::vector<glm::vec3>();
  auto transposedMat = _transform_mat;
  for (auto const& point : corners_in_world)
  {
    rotated_corners_in_world.emplace_back(transposedMat * glm::vec4(point, 1.f));
  }

  math::aabb const bounding_of_rotated_points (rotated_corners_in_world);

  extents[0] = bounding_of_rotated_points.min;
  extents[1] = bounding_of_rotated_points.max;

  // Guard the "fuckported model" case the TODO above calls out: a header collision/bounding box of
  // {inf,-inf} (or any min>max / non-finite value, common in custom/Turtle models) yields garbage
  // world extents. Downstream, world_tile_update_queue::apply() derives a TileIndex range from these
  // extents and loads EVERY tile in it -- a whole-map range means thousands of tiles get loaded ->
  // gigabytes of RAM and a frozen client. Collapse a bad box to a point at pos (same as loading_failed).
  bool const bad_extents =
       !std::isfinite(extents[0].x) || !std::isfinite(extents[0].y) || !std::isfinite(extents[0].z)
    || !std::isfinite(extents[1].x) || !std::isfinite(extents[1].y) || !std::isfinite(extents[1].z)
    || extents[0].x > extents[1].x || extents[0].y > extents[1].y || extents[0].z > extents[1].z;
  if (bad_extents)
  {
    extents[0] = extents[1] = pos;
    size_cat = 0.f;
    _cull_class = 4; // fuckported/degenerate box -> classify as largest (keep visible; noggit guard)
    _need_recalc_extents = false;
    return;
  }

  size_cat = glm::distance(bounding_of_rotated_points.max, bounding_of_rotated_points.min);

  // [game-view cull 2026-08-15] Cache the client doodad size class (wow335a.exe FUN_007bdb10). The
  // client builds the batch WORLD AABB from the model RENDER bounding box (header.bounding_box -- the
  // geometry bounds, NOT the collision box) x placement transform (scale+rotation included), then
  // classifies by its LARGEST axis extent E vs sizeThresh {1,4,15,100} @DAT_00adf378:
  //   E<=1 ->0, 1..4 ->1, 4..15 ->2, 15..100 ->3, >100 ->4. Byte-exact; see
  //   twmoa-335a-client-doodad-cull.md. Same corner pipeline as the extents above (transform_model_box
  //   -> _transform_mat), but the render box alone so collision-box union can't bump the class.
  {
    math::aabb const render_box (model->header.bounding_box_min, model->header.bounding_box_max);
    std::vector<glm::vec3> rb_corners;
    for (auto const& corner : render_box.all_corners())
    {
      rb_corners.emplace_back (_transform_mat * glm::vec4 (transform (corner), 1.f));
    }
    math::aabb const rb (rb_corners);
    float const E = std::max ({rb.max.x - rb.min.x, rb.max.y - rb.min.y, rb.max.z - rb.min.z});
    static constexpr float sizeThresh[4] = {1.f, 4.f, 15.f, 100.f}; // @DAT_00adf378
    int cls = 0;
    while (cls < 4 && E > sizeThresh[cls]) ++cls;
    _cull_class = cls;
  }

  _need_recalc_extents = false;
}

// [game-view cull 2026-08-15] Client placed-doodad (MDDF/M2) distance cull + fade, ported byte-exact
// from wow335a.exe build 12340 (cull reader FUN_00791cb0, class writer FUN_007bdb10). Returns a fade
// alpha in [0,1]; 0.0 == hard-culled. Per-class tables, verbatim:
//   cullFar  @DAT_00adf364 = {30,100,200,750,1250}   (hard cull beyond)
//   fadeBand @DAT_00adf38c = { 5, 10, 15, 20,  50}   (linear fade width)
//   fadeStart = cullFar - fadeBand = {25,90,185,730,1200}
// environmentDetail s = clamp(v,0.5,1.5) (default 1.0 @0x009e1340) scales ONLY classes 1,2,3 (0 and 4
// are unscaled). Cull test uses distSq vs cullFarSq off the world-AABB centre (client batch centre
// +0x38). Fade: alpha = 1 - (dist - fadeStart)/fadeBand, with the min-alpha cull floor 0.01
// (_DAT_009f1968). See twmoa-335a-client-doodad-cull.md.
void ModelInstance::doodadCullParams(int cull_class, float environment_detail,
                                     float& cull_far, float& fade_band)
{
  static constexpr float cullFar[5]  = {30.f, 100.f, 200.f, 750.f, 1250.f};
  static constexpr float fadeBand[5] = { 5.f,  10.f,  15.f,  20.f,   50.f};

  int cls = cull_class;
  if (cls < 0) cls = 0;
  else if (cls > 4) cls = 4;

  float s = environment_detail;
  if (s < 0.5f) s = 0.5f;
  else if (s > 1.5f) s = 1.5f;

  cull_far = cullFar[cls];
  if (cls >= 1 && cls <= 3)
    cull_far *= s;
  fade_band = fadeBand[cls];
}

float ModelInstance::doodadCullFade(glm::vec3 const& camera, float environment_detail)
{
  ensureExtents(); // guarantees extents + _cull_class are current

  float cull_far, band;
  doodadCullParams(_cull_class, environment_detail, cull_far, band);
  float const fade_start = cull_far - band;

  glm::vec3 const center ((extents[0] + extents[1]) * 0.5f);
  glm::vec3 const d (camera - center);
  float const distSq = glm::dot (d, d);

  if (distSq > cull_far * cull_far)
    return 0.0f; // FUN_00791cb0: distSq > cullFarSq -> hard cull

  float const dist = std::sqrt (distSq);
  if (dist <= fade_start)
    return 1.0f;

  float const alpha = 1.0f - (dist - fade_start) / band;
  return alpha < 0.01f ? 0.0f : alpha; // min-alpha cull floor
}

void ModelInstance::ensureExtents()
{
  if (_need_recalc_extents && model->finishedLoading())
  {
    recalcExtents();
  }
}

glm::vec3* ModelInstance::getExtents()
{
  if (_need_recalc_extents && model->finishedLoading())
  {
    recalcExtents();
  }

  return &extents[0];
}

void ModelInstance::updateDetails(Noggit::Ui::detail_infos* detail_widget)
{
  std::stringstream select_info;

  select_info << "<b>filename:</b> " << model->file_key().filepath()
    // << "<br><b>FileDataID:</b> " << model->file_key().fileDataID() // not in WOTLK
    << "<br><b>unique ID:</b> " << uid
    << "<br><b>position X/Y/Z:</b> {" << pos.x << " , " << pos.y << " , " << pos.z << "}"
    << "<br><b>rotation X/Y/Z:</b> {" << dir.x << " , " << dir.y << " , " << dir.z << "}"
    << "<br><b>scale:</b> " << scale

    << "<br><b>server position X/Y/Z: </b>{" << (ZEROPOINT - pos.z) << ", " << (ZEROPOINT - pos.x) << ", " << pos.y << "}"
    << "<br><b>server orientation:  </b>" << fabs(2 * glm::pi<float>() - glm::pi<float>() / 180.0 * (float(dir.y) < 0 ? fabs(float(dir.y)) + 180.0 : fabs(float(dir.y) - 180.0)))

    << "<br><b>textures Used:</b> " << model->header.nTextures
    << "<br><b>size category:</b><span> " << size_cat;

  for (unsigned j  = 0; j < model->header.nTextures; j++)
  {
    bool stuck = !model->_textures[j]->finishedLoading();
    bool error = model->_textures[j]->finishedLoading() && !model->_textures[j]->is_uploaded();

    select_info << "<br> ";

    if (stuck)
      select_info << "<font color=\"Orange\">";

    if (error)
      select_info << "<font color=\"Red\">";

    select_info << "<b>" << (j + 1) << ":</b> " << model->_textures[j]->file_key().stringRepr();

    if (stuck || error)
      select_info << "</font>";
  }

  select_info << "</span>";

  detail_widget->setText(select_info.str());
}

wmo_doodad_instance::wmo_doodad_instance(BlizzardArchive::Listfile::FileKey const& file_key
                                         , BlizzardArchive::ClientFile* f
                                         , Noggit::NoggitRenderContext context)
  : ModelInstance(file_key, context)
{
  float ff[4];

  f->read(ff, 12);
  pos = glm::vec3(ff[0], ff[2], -ff[1]);

  f->read(ff, 16);
  doodad_orientation = glm::quat(ff[3], ff[0], ff[2], -ff[1]);

  f->read(&scale, 4);

  union
  {
    uint32_t packed;
    struct
    {
      uint8_t b, g, r, a;
    }bgra;
  } color;

  f->read(&color.packed, 4);

  light_color = glm::vec3(color.bgra.r / 255.f, color.bgra.g / 255.f, color.bgra.b / 255.f);
}

void wmo_doodad_instance::update_transform_matrix_wmo(WMOInstance* wmo)
{
  if (!model->finishedLoading())
  {
    return;
  }  

  world_pos = wmo->transformMatrix() * glm::vec4(pos,1);

  auto m2_mat = glm::mat4x4(1);
  m2_mat = glm::translate(m2_mat, pos);
  m2_mat = m2_mat * glm::toMat4(doodad_orientation);
  m2_mat = glm::scale(m2_mat, glm::vec3(scale));

  glm::mat4x4 mat
  (
    wmo->transformMatrix() * m2_mat
  );

  _transform_mat = mat;
  _transform_mat_inverted = glm::inverse(mat);

  // to compute the size category (used in culling)
  recalcExtents();

  _need_matrix_update = false;
}
