// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <math/ray.hpp>

#include <noggit/ModelInstance.h> // ModelInstance
#include <noggit/ModelManager.h>
#include <noggit/AsyncObjectMultimap.hpp>
#include <noggit/TextureManager.h>
#include <noggit/tool_enums.hpp>
#include <noggit/wmo_liquid.hpp>
#include <noggit/ContextObject.hpp>
#include <noggit/rendering/WMOGroupRender.hpp>
#include <noggit/rendering/WMORender.hpp>
#include <noggit/rendering/Primitives.hpp>
#include <ClientFile.hpp>
#include <external/glm/gtc/type_precision.hpp> // glm::u8vec3 (_ground_colors)
#include <optional>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <cstdint>

class WMO;
class WMOGroup;
class WMOInstance;
class WMOManager;
class wmo_liquid;
class Model;

namespace Noggit::Rendering
{
  class LiquidTextureManager;
  class WMOGroupRender;
  class WMORender;
}


struct wmo_batch
{
  int8_t unused[12];

  uint32_t index_start;
  uint16_t index_count;
  uint16_t vertex_start;
  uint16_t vertex_end;

  uint8_t flags;
  uint8_t texture;
};

union wmo_mopy_flags
{
    int8_t value;
    struct
    {
        int8_t flag_0x01 : 1; // 0x1
        int8_t no_cam_collide : 1; // 0x2
        int8_t detail : 1; // 0x4
        int8_t collision : 1; // 0x8
        int8_t hint : 1;
        int8_t render : 1;
        int8_t flag_0x40 : 1; // 0x40
        int8_t collide_hit : 1; // 0x80

    };
};
static_assert (sizeof(wmo_mopy_flags) == sizeof(std::int8_t)
    , "bitfields shall be implemented packed"
    );

struct wmo_triangle_material_info
{
    wmo_mopy_flags flags;
    uint8_t texture;

    bool isTransFace() { return flags.flag_0x01 && (flags.detail || flags.render); }
    bool isColor() { return !flags.collision; }
    bool isRenderFace() { return flags.render && !flags.detail; }
    bool isCollidable() { return flags.collision || isRenderFace(); }

    bool isCollision() { return texture == 0xff; }
};

enum wmo_mobn_flags
{
    Flag_XAxis = 0x0,
    Flag_YAxis = 0x1,
    Flag_ZAxis = 0x2,
    Flag_AxisMask = 0x3,
    Flag_Leaf = 0x4,
    Flag_NoChild = 0xFFFF,
};

struct wmo_bsp_node
{
    uint16_t flags;
    int16_t negChild;      // index of bsp child node (right in this array)
    int16_t posChild;
    uint16_t nFaces;       // num of triangle faces in MOBR
    uint32_t faceStart;    // index of the first triangle index(in MOBR)
    float planeDist;
};

union wmo_group_flags
{
  uint32_t value;
  struct
  {
    uint32_t has_bsp_tree : 1; // 0x1
    uint32_t has_light_map : 1; // 0x2
    uint32_t has_vertex_color : 1; // 0x4
    uint32_t exterior : 1; // 0x8
    uint32_t flag_0x10 : 1;
    uint32_t flag_0x20 : 1;
    uint32_t exterior_lit : 1; // 0x40
    uint32_t unreacheable : 1; // 0x80
    uint32_t flag_0x100: 1;
    uint32_t has_light : 1; // 0x200
    uint32_t flag_0x400 : 1;
    uint32_t has_doodads : 1; // 0x800
    uint32_t has_water : 1; // 0x1000
    uint32_t indoor : 1; // 0x2000
    uint32_t flag_0x4000 : 1;
    uint32_t flag_0x8000 : 1;
    uint32_t always_draw : 1; // 0x10000
    uint32_t has_mori_morb : 1; // 0x20000, cata+ only (?)
    uint32_t skybox : 1; // 0x40000
    uint32_t ocean : 1; // 0x80000
    uint32_t flag_0x100000 : 1;
    uint32_t mount_allowed : 1; // 0x200000
    uint32_t flag_0x400000 : 1;
    uint32_t flag_0x800000 : 1;
    uint32_t use_mocv2_for_texture_blending : 1; // 0x1000000
    uint32_t has_two_motv : 1; // 0x2000000
    uint32_t antiportal : 1; // 0x4000000
    uint32_t unk : 1; // 0x8000000 requires intBatchCount == 0, extBatchCount == 0, UNREACHABLE. 
    uint32_t unused : 4;
  };
};
static_assert ( sizeof (wmo_group_flags) == sizeof (std::uint32_t)
              , "bitfields shall be implemented packed"
              );

struct wmo_group_header
{
  uint32_t group_name; // offset into MOGN
  uint32_t descriptive_group_name; // offset into MOGN
  wmo_group_flags flags;
  float box1[3];
  float box2[3];
  uint16_t portal_start;
  uint16_t portal_count;
  uint16_t transparency_batches_count;
  uint16_t interior_batch_count;
  uint16_t exterior_batch_count;
  uint16_t padding_or_batch_type_d; // probably padding, but might be data?
  uint8_t fogs[4];
  uint32_t group_liquid; // used for MLIQ
  uint32_t id;
  int32_t unk2, unk3;
};

class WMOGroup 
{
  friend class Noggit::Rendering::WMOGroupRender;

public:
  WMOGroup(WMO *wmo, BlizzardArchive::ClientFile* f, int num, char const* names);
  WMOGroup(WMOGroup const&);

  void load();

  void drawLiquid ( glm::mat4x4 const& transform
                  , OpenGL::Scoped::use_program& water_shader
                  , Noggit::Rendering::LiquidTextureManager& texture_manager
                  , bool draw_fog
                  , int animtime
                  );

  void setupFog (bool draw_fog, std::function<void (bool)> setup_fog);

  void intersect (math::ray const&, std::vector<float>* results) const;

  // todo: portal culling
  [[nodiscard]]
  bool is_visible( glm::mat4x4 const& transform_matrix
                 , math::frustum const& frustum
                 , float const& cull_distance
                 , glm::vec3 const& camera
                 , display_mode display
                 ) const;

  [[nodiscard]]
  std::vector<uint16_t> doodad_ref() const { return _doodad_ref; }

  // MOLR: indices into the root WMO's MOLT light list that illuminate this group (per-room lighting).
  [[nodiscard]]
  std::vector<int16_t> const& light_refs() const { return _light_refs; }

  glm::vec3 BoundingBoxMin;
  glm::vec3 BoundingBoxMax;
  glm::vec3 VertexBoxMin;
  glm::vec3 VertexBoxMax;

  bool use_outdoor_lights;
  std::string name;

  [[nodiscard]]
  bool has_skybox() const { return header.flags.skybox; }

  [[nodiscard]]
  bool is_indoor() const { return header.flags.indoor; }

  [[nodiscard]]
  bool is_exterior() const { return header.flags.exterior; }

  [[nodiscard]]
  bool is_exterior_lit() const { return header.flags.exterior_lit; }

  [[nodiscard]]
  bool has_mocv() const { return header.flags.has_vertex_color; }

  [[nodiscard]]
  std::uint32_t wmo_area_table_group_id() const { return header.id; }

  // This group's authored MFOG references (MOGP fogs[4]) -- the client selects the scene fog from the
  // CAMERA group's list, which is how fog colour/distance changes per room (Kara vs Deadmines ship etc).
  [[nodiscard]]
  std::uint8_t fog_id(int i) const { return header.fogs[i]; }

  // Range into the root WMO's _portal_refs list = this group's portals (for portal-visibility culling).
  [[nodiscard]]
  std::uint16_t portal_start() const { return header.portal_start; }
  [[nodiscard]]
  std::uint16_t portal_count() const { return header.portal_count; }

  [[nodiscard]]
  Noggit::Rendering::WMOGroupRender* renderer() { return &_renderer; };
  ::glm::vec3 center;

  // Root-side MOGI flags for this group, read at ROOT load (available for every group before any group
  // FILE loads). Needed by AttenTransVerts: a portal brightens only when its TARGET group has
  // MOGI.flags & 0x48 (exterior / exterior-lit).
  std::uint32_t mogi_flags = 0;

  // Client CWorldEntity::SampleGroundColor (RE_notes/15, wow.exe 0x69E4C0/0x6B9A50): straight-down ray in
  // WMO-LOCAL space from local_pos.y+1.0 to local_pos.y-12.0 against this group's triangles; on hit,
  // barycentric-interpolates the retained MOCV rgb of the face into *out (0..1). Returns false when this
  // group keeps no ground colours (non-indoor / no MOCV) or no floor is under the point. This colour is
  // the ENTIRE base light of a unit standing indoors (MOHD ambient and the sun play no part).
  // out_alpha (optional): the face's PRISTINE baked MOCV floor ALPHA barycentric-interpolated into [0..1]
  // (GAP B / checklist 8.7 doorway spill) -- ~0 deep inside a room, ramping to 1 near a portal/window.
  bool sample_ground_color(glm::vec3 const& local_pos, glm::vec3* out, float* out_alpha = nullptr) const;

private:
  void load_mocv(BlizzardArchive::ClientFile& f, uint32_t size);
  // The 1.12 client's ONLY load-time MOCV mutation (byte-matched 100% on 64,317 traced verts across 3
  // WMOs, RE_notes/19): brighten TRANSPARENCY-BATCH vertices toward white by proximity to portals that
  // lead to exterior groups (op = 1 - 0.15*d, accumulated, capped 1), written ONLY when the new alpha
  // byte exceeds the stored one. Everything else ships to the GPU verbatim -- the wowdev
  // "FixColorVertexAlpha" (ambient subtract / halve / alpha fold) does NOT exist in 1.12.
  void atten_trans_verts(std::vector<std::uint32_t>& colors); // parked (reverted; see WMO.cpp note)
  void fix_vertex_color_alpha();
  // WotLK/3.3.5a CMapObjGroup::FixColorVertexAlpha (RE'd byte-exact from stock 12340 FUN_007D7380): the
  // load-time MOCV transform 1.12 lacks. Paired with the mod2x (tex*MOCV*2) interior combine in wmo_frag.
  // Gated to non-CLASSIC projects behind NOGGIT_335A_WMO_MOD2X (A/B, interior-lighting rule).
  void fix_vertex_color_alpha_wotlk();
  void compute_portal_openness();
  // Client-faithful (SMOGroup flag `do_not_attenuate_vertices_based_on_distance_to_portal`): brighten
  // interior vertices toward the outdoor light by their proximity to this group's portals, so the light
  // spills smoothly through a doorway/window instead of a hard interior/exterior seam. Stored per-vertex
  // as an "openness" factor in the vertex-colour alpha (1 at a portal, fading to 0 inward).

  WMO *wmo;
  wmo_group_header header;
  float rad;
  int32_t num;
  int32_t fog;
  std::vector<uint16_t> _doodad_ref;
  std::vector<int16_t> _light_refs; // MOLR
  std::unique_ptr<wmo_liquid> lq;

  std::vector <wmo_triangle_material_info> _material_infos;
  std::vector<wmo_batch> _batches;

  // (Legacy, always false now: the portal-spill experiment is superseded by the byte-matched 1.12
  // AttenTransVerts + tex*MOCV*(1+4a) pipeline, RE_notes/19.)
  // this (indoor) group. The renderer flags such batches so the shader applies the outdoor-light spill.
  bool _has_portal_openness = false;

  std::vector<::glm::vec3> _vertices;
  std::vector<::glm::vec3> _normals;
  std::vector<glm::vec2> _texcoords;
  std::vector<glm::vec2> _texcoords_2;
  std::vector<glm::vec4> _vertex_colors;
  std::vector<uint16_t> _indices;
  // Compact MOCV rgb copy (post atten_trans_verts), INDOOR groups only: the renderer clears
  // _vertex_colors on GPU upload, but sample_ground_color() needs the baked floor colours on the CPU.
  std::vector<glm::u8vec3> _ground_colors;
  // Parallel to _ground_colors (same size / vertex indexing, INDOOR groups only): the PRISTINE baked
  // MOCV floor ALPHA per vertex [0..255] (GAP B / checklist 8.7 doorway spill). ~0 deep interior,
  // ramping to 255 near a portal/window. sample_ground_color() barycentric-interpolates it for the
  // doorway day/night spill. Sourced from _mocv_pristine_alpha (below), NOT from the .w that
  // fix_vertex_color_alpha / compute_portal_openness overwrite.
  std::vector<std::uint8_t> _ground_alphas;
  // Transient (load-time bridge): the pristine MOCV alpha straight from colorFromInt [0..1], captured in
  // load_mocv BEFORE fix_vertex_color_alpha (.w=1) / compute_portal_openness (.w=portal-fade) clobber it,
  // so the _ground_alphas build (end of load()) can read the true baked floor exposure.
  std::vector<float> _mocv_pristine_alpha;

  std::optional<std::vector<wmo_bsp_node>> _bsp_tree_nodes;
  std::optional<std::vector<uint16_t>> _bsp_indices;

  Noggit::Rendering::WMOGroupRender _renderer;
};

struct WMOLight {
  uint32_t flags, color;
  glm::vec3 pos;
  float intensity;
  float unk[5];
  float r;

  glm::vec4 fcolor;

  void init(BlizzardArchive::ClientFile* f);
  void setup(GLint light);

  static void setupOnce(GLint light, glm::vec3 dir, glm::vec3 light_color);
};

struct WMOPV {
  glm::vec3 a, b, c, d;
};

struct WMOPR {
  int16_t portal, group, dir, reserved;
};

// One MOPT entry: the polygon of a portal is _portal_vertices[base_vertex .. base_vertex+vertex_count).
// (For CULLING the plane is recomputed from the transformed polygon; but the 1.12 AttenTransVerts pass
// (RE_notes/19) needs the AUTHORED plane, stored here in noggit's swapped coord convention -- the swap
// is orthogonal so dot(normal_swapped, v_swapped) + dist is invariant.)
struct wmo_portal_info {
  uint16_t base_vertex;
  uint16_t vertex_count;
  glm::vec3 plane_normal = glm::vec3(0.f);
  float plane_dist = 0.f;
};

struct WMODoodadSet {
  char name[0x14];
  int32_t start;
  int32_t size;
  int32_t unused;
};

struct WMOFog {
  unsigned int flags;
  glm::vec3 pos;
  float r1, r2, fogend, fogstart;
  unsigned int color1;
  float f2;
  float f3;
  unsigned int color2;
  // read to here (0x30 bytes)
  glm::vec4 color;
  void init(BlizzardArchive::ClientFile* f);
  void setup();
};

union mohd_flags
{
  std::uint16_t flags;
  struct
  {
    std::uint16_t do_not_attenuate_vertices_based_on_distance_to_portal : 1;
    std::uint16_t use_unified_render_path : 1;
    std::uint16_t use_liquid_type_dbc_id : 1;
    std::uint16_t do_not_fix_vertex_color_alpha : 1;
    std::uint16_t unused : 12;
  };
};
static_assert ( sizeof (mohd_flags) == sizeof (std::uint16_t)
              , "bitfields shall be implemented packed"
              );

class WMO : public AsyncObject
{
  friend class Noggit::Rendering::WMORender;

public:
  explicit WMO(BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context );

  [[nodiscard]]
  std::vector<float> intersect (math::ray const&, bool do_exterior = true) const;

  void finishLoading() override;

  void waitForChildrenLoaded() override;

  [[nodiscard]]
  std::map<uint32_t, std::vector<wmo_doodad_instance>> doodads_per_group(uint16_t doodadset) const;

  std::vector<WMOGroup> groups;
  std::vector<WMOMaterial> materials;
  // Per material: the material asks for an Env/EnvMetal shader but its MOTX second-texture entry is EMPTY
  // (e.g. Stormwind's SW_Harbor_Docks.wmo on the docked ship). There is no environment map to reflect, so
  // the renderer must not sample one -- see eWMOBatch_NoEnvTexture.
  std::vector<std::uint8_t> material_env_texture_missing;
  glm::vec3 extents[2];
  std::vector<scoped_blp_texture_reference> textures;
  std::vector<std::string> models;
  std::vector<wmo_doodad_instance> modelis;
  std::vector<glm::vec3> model_nearest_light_vector;

  // Portal graph for interior visibility culling (MOPV/MOPT/MOPR). _portal_vertices = all portal polygon
  // corners (WMO local space); _portal_info[p] = which slice of _portal_vertices is portal p's polygon;
  // _portal_refs = the links (each group's portals are _portal_refs[group.portal_start .. +portal_count),
  // giving the neighbour group index + which side of the portal that group sits on).
  std::vector<glm::vec3> _portal_vertices;
  std::vector<wmo_portal_info> _portal_info;
  std::vector<WMOPR> _portal_refs;

  std::vector<WMOLight> lights;
  glm::vec4 ambient_light_color;

  uint32_t WmoId;

  mohd_flags flags;

  std::vector<WMOFog> fogs;

  // CLIENT-EXACT fog evaluation for one group at the camera position (wow.exe @0069de20 +
  // weight @0069e1c0 + lerp @0069efd0; docs/client_re/27). Returns false when this WMO carries
  // only the default MFOG entry -- the client evaluator bails there and the ZONE fog applies.
  // Otherwise the result STARTS as fogs[0] (the default entry, NOT the zone fog) and every
  // candidate from the group's MOGP fog indices (index != 0, !(flags & 1), camera closer than r2)
  // is lerped over it FARTHEST-first with w = 1 inside r1, linear to 0 at r2 -- so the nearest
  // fog dominates. fog_start_abs is absolute (WMOFog::init pre-multiplies the authored scaler).
  bool evaluate_camera_fog(WMOGroup const& group, glm::mat4x4 const& transform,
                           glm::vec3 const& camera, bool camera_inside_wmo, glm::vec3* color,
                           float* fog_end, float* fog_start_abs) const;

  std::vector<WMODoodadSet> doodadsets;

  std::optional<scoped_model_reference> skybox;

  Noggit::NoggitRenderContext _context;

  [[nodiscard]]
  bool is_hidden() const { return _hidden; }

  void toggle_visibility() { _hidden = !_hidden; }
  void show() { _hidden = false ; }
  void hide() { _hidden = true; }


  [[nodiscard]]
  bool is_required_when_saving()  const override
  {
    return true;
  }

  [[nodiscard]]
  Noggit::Rendering::WMORender* renderer() { return &_renderer; }

private:
  bool _hidden = false;

  Noggit::Rendering::WMORender _renderer;
};

class WMOManager
{
public:
  static void report();
  static void clear_hidden_wmos();
  static void unload_all(Noggit::NoggitRenderContext context);
private:
  friend struct scoped_wmo_reference;
  static Noggit::AsyncObjectMultimap<WMO> _;
};

struct scoped_wmo_reference
{
  scoped_wmo_reference (BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context)
    : _valid(true)
    , _file_key(file_key)
    , _context(context)
    , _wmo (WMOManager::_.emplace(file_key, context))
  {}

  scoped_wmo_reference (scoped_wmo_reference const& other)
    : _valid(other._valid)
    , _file_key(other._file_key)
    , _wmo(WMOManager::_.emplace(_file_key, other._context))
    , _context(other._context)
  {}
  scoped_wmo_reference& operator= (scoped_wmo_reference const& other)
  {
    _valid = other._valid;
    _file_key = other._file_key;
    _wmo = WMOManager::_.emplace(_file_key, other._context);
    _context = other._context;
    return *this;
  }

  scoped_wmo_reference (scoped_wmo_reference&& other)
    : _valid (other._valid)
    , _file_key (other._file_key)
    , _wmo (other._wmo)
    , _context (other._context)
  {
    other._valid = false;
  }
  scoped_wmo_reference& operator= (scoped_wmo_reference&& other)
  {
    std::swap(_valid, other._valid);
    std::swap(_file_key, other._file_key);
    std::swap(_wmo, other._wmo);
    std::swap(_context, other._context);
    other._valid = false;
    return *this;
  }

  ~scoped_wmo_reference()
  {
    if (_valid)
    {
      WMOManager::_.erase(_file_key, _context);
    }
  }

  WMO* operator->() const
  {
    return _wmo;
  }

  [[nodiscard]]
  WMO* get() const
  {
    return _wmo;
  }

private:  
  bool _valid;

  BlizzardArchive::Listfile::FileKey _file_key;
  WMO* _wmo;
  Noggit::NoggitRenderContext _context;
};
