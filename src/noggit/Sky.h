// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <noggit/DBCFile.h>
#include <noggit/ModelInstance.h>
#include <noggit/TextureManager.h>
#include <noggit/ContextObject.hpp>
#include <noggit/rendering/Primitives.hpp>
#include <opengl/scoped.hpp>
#include <opengl/shader.fwd.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>


// Circular piecewise-linear keyframe interpolation over (day-fraction, value) pairs -- the exact
// evaluator the 1.12 client uses for its celestial paths (wow.exe 5875 FUN_006cf6c0).
float sky_keyframe(std::pair<float, float> const* keys, int count, float t);

struct OutdoorLightStats
{
  float nightIntensity;
  glm::vec3 dayDir;

  void interpolate(OutdoorLightStats *a, OutdoorLightStats *b, float r);
};

class OutdoorLighting
{
private:
  std::vector<OutdoorLightStats> lightStats;

public:
  OutdoorLighting();

  OutdoorLightStats getLightStats(int time);
};

struct SkyColor 
{
  glm::vec3 color;
  int time;

  SkyColor(int t, int col);
};

struct SkyFloatParam
{
  SkyFloatParam(int t, float val);

  float value;
  int time;
};

class SkyParam
{
public:
    std::optional<ModelInstance> skybox;
    // LightSkybox.dbc flags (field 2, WotLK-only column -- 0 on 1.12, which has no such field).
    // Determined empirically from the shipped 3.3.5a data (see 8.6): bit 0x1 = FULL-DAY skybox
    // (StormPeaks / IceCrown / ZulDrak / Coldarra -- one M2 whose animation spans the whole day and
    // is driven by time-of-day, not a free-running clock); bit 0x2 = an additive AURORA overlay
    // (Aurora, DeathKnightFireSkyBox). NOTE the checklist row 8.6 had these backwards ("0x2 =
    // full-day"); 0x1 is full-day.
    int skybox_flags = 0;
    int skybox_id = 0; // LightSkybox.dbc id: the client merges the SAME skybox reached through several lights
    int Id;

    SkyParam() = default;
    explicit SkyParam(int paramId, Noggit::NoggitRenderContext context);

    std::vector<SkyColor> colorRows[36];
    std::vector<SkyFloatParam> floatParams[6];
    int mmin[36];
    int mmin_float[6];

    bool highlight_sky() const { return _highlight_sky; }
    float river_shallow_alpha() const { return _river_shallow_alpha; }
    float river_deep_alpha() const { return _river_deep_alpha; }
    float ocean_shallow_alpha() const { return _ocean_shallow_alpha; }
    float ocean_deep_alpha() const { return _ocean_deep_alpha; }
    float glow() const { return _glow; }

    void set_glow(float glow) { _glow = glow; }
    void set_highlight_sky(bool state) { _highlight_sky = state; }
    void set_river_shallow_alpha(float alpha) { _river_shallow_alpha = alpha; }
    void set_river_deep_alpha(float alpha) { _river_deep_alpha = alpha; }
    void set_ocean_shallow_alpha(float alpha) { _ocean_shallow_alpha = alpha; }
    void set_ocean_deep_alpha(float alpha) { _ocean_deep_alpha = alpha; }

private:
  bool _highlight_sky = false;
  float _river_shallow_alpha = 0.6f;
  float _river_deep_alpha = 1.0f;
  float _ocean_shallow_alpha = 0.6f;
  float _ocean_deep_alpha = 1.0f;

  float _glow = 0.0f;

    Noggit::NoggitRenderContext _context;
};

class Sky 
{
public:
  std::optional<ModelInstance> skybox;

  int Id;
  glm::vec3 pos;
  float r1, r2;

  explicit Sky(DBCFile::Iterator data, Noggit::NoggitRenderContext context);

  SkyParam* skyParams[8];
  int curr_sky_param = 0;

  // std::vector<SkyColor> colorRows[36];
  // std::vector<SkyFloatParam> floatParams[6];
  // int mmin[36];
  // int mmin_float[6];

  char name[32];

  glm::vec3 colorFor(int r, int t) const;
  float floatParamFor(int r, int t) const;
  // Like floatParamFor, but never the *_WATER param variant: CLEAR_WATER reads CLEAR, STORM_WATER
  // reads STORM. Cloud coverage uses this (user 2026-08-26: submerging emptied the CLEAR_WATER
  // cloud-density band -> the row-budgeted cloud regen wiped back in slices on surfacing).
  float floatParamForAirVariant(int r, int t) const;

  // Evaluate a band on an EXPLICIT param set (nullptr falls back like an unauthored band). colorFor/
  // floatParamFor blend the current (clear) set toward the STORM set by the global weather intensity —
  // the client's rainy-weather light change (Light.dbc authors clear/storm/... param ids per light).
  // PUBLIC: Skies::update_sky_colors also evaluates the WATER param's river-deep band with it (the
  // exterior WMO water colour).
  glm::vec3 colorFromParam(SkyParam const* param, int r, int t) const;
  float floatFromParam(SkyParam const* param, int r, int t) const;

public:

  float weight;
  bool global;

  // [client RE 2026-09-09, wow335a.exe] weight from the client's hardcoded ZONE-LIGHT POLYGON table
  // (FUN_0077eed0 -> FUN_007ed150 -> FUN_007ee6b0): 1 deep inside the zone's polygon, fading to 0 fifty
  // yards outside its edge, and blended BEFORE every positional (r1/r2) light. zone_order is the table row,
  // i.e. the blend order among zone lights. Set by Skies::findSkyWeights, independent of `weight`.
  float zone_weight = 0.f;
  int zone_order = -1;

  bool is_new_record = false;

  bool operator<(const Sky& s) const
  {
    if (global) return false;
    else if (s.global) return true;
    else return r2 < s.r2;
  }

  // bool highlight_sky() const { return _highlight_sky; }
  // float river_shallow_alpha() const { return _river_shallow_alpha; }
  // float river_deep_alpha() const { return _river_deep_alpha; }
  // float ocean_shallow_alpha() const { return _ocean_shallow_alpha; }
  // float ocean_deep_alpha() const { return _ocean_deep_alpha; }
  // float glow() const { return _glow; }
  bool selected() const { return _selected; }
  // 
  // void set_glow(float glow) { _glow = glow; }
  // void set_highlight_sky(bool state) { _highlight_sky = state; }
  // void set_river_shallow_alpha(float alpha) { _river_shallow_alpha = alpha; }
  // void set_river_deep_alpha(float alpha) { _river_deep_alpha = alpha; }
  // void set_ocean_shallow_alpha(float alpha) { _ocean_shallow_alpha = alpha; }
  // void set_ocean_deep_alpha(float alpha) { _ocean_deep_alpha = alpha; }

  void save_to_dbc();

  Sky(int id, glm::vec3 const& position, float inner_radius, float outer_radius, std::vector<SkyParam*> params, Noggit::NoggitRenderContext context);

private:
  // bool _highlight_sky;
  // float _river_shallow_alpha;
  // float _river_deep_alpha;
  // float _ocean_shallow_alpha;
  // float _ocean_deep_alpha;

  // float _glow;
  bool _selected;

  Noggit::NoggitRenderContext _context;
};

enum SkyColorNames 
{
  LIGHT_GLOBAL_DIFFUSE,
  LIGHT_GLOBAL_AMBIENT,
  SKY_COLOR_0, // top
  SKY_COLOR_1, // middle
  SKY_COLOR_2, // middle to horizon
  SKY_COLOR_3, // above horizon
  SKY_COLOR_4, // horizon
  FOG_COLOR, // fog and WDL mountains
  SHADOW_OPACITY,
  SUN_COLOR, // sun, specular light, sunrays
  SUN_HALO_COLOR, // bigger sun halo
  CLOUD_EDGE_COLOR, // cloud edge
  CLOUD_COLOR, // cloud body
  SKY_UNKNOWN_3,
  OCEAN_COLOR_LIGHT, // shallow ocean
  OCEAN_COLOR_DARK, // deep ocean
  RIVER_COLOR_LIGHT, // shallow river
  RIVER_COLOR_DARK, // deep river
  NUM_SkyColorNames
};

enum SkyFloatParamsNames
{
  FOG_DISTANCE,
  FOG_MULTIPLIER,
  CELESTIAL_FLOW,
  CLOUD_DENSITY,
  UNK_FLOAT_PARAM_1,
  UNK_FLOAT_PARAM_2,
  NUM_SkyFloatParamsNames
};

enum SkyParamsNames
{
    CLEAR,
    CLEAR_WATER,
    STORM,
    STORM_WATER,
    DEATH,
    UNK_PARAM_1,
    UNK_PARAM_2,
    UNK_PARAM_3,
    NUM_SkyParamsNames
};

class Skies 
{
private:
  int numSkies = 0;
  int cs = -1;
  ModelInstance stars;

  int _last_time = -1;
  float _celestial_flow = 0.f; // LightFloatBand[2] CELESTIAL_FLOW: dusk/dawn sky-glow weight (FUN_006d0f50)
  glm::vec3 _last_pos;

  float _river_shallow_alpha = 0.6f;
  float _river_deep_alpha = 1.0f;
  float _ocean_shallow_alpha = 0.6f;
  float _ocean_deep_alpha = 1.0f;
  float _glow = 0.0f;
  float _fog_rate = 1.5f;

  float _fog_distance = 18000.0f;
  float _fog_multiplier = 0.25f;
  int _area_light_id = 0;

  // [client RE 2026-09-09] the zone-light polygons of this map (ZONE_LIGHT_POLYGON_DEFS in Sky.cpp), already
  // mapped into world space with the client's constants; the box is padded by the 50-yard fade band.
  struct ZoneLightPolygon
  {
    int light_id;
    std::vector<glm::vec2> points; // (world x, world z)
    float min_x, min_z, max_x, max_z;
  };
  std::vector<ZoneLightPolygon> _zone_polygons;
  void build_zone_polygons(unsigned int mapid);
  // the client's per-frame test at a world position: (light id, weight) per hit polygon, table order
  void zone_polygon_weights(glm::vec3 const& pos, std::vector<std::pair<int, float>>& out) const;
  void apply_zone_polygon_weights(glm::vec3 const& pos);
  // every light with a weight in the client's blend order: zone-polygon lights first (table order), then
  // the positional lights farthest-first (`skies` is sorted that way by findSkyWeights)
  std::vector<std::pair<Sky*, float>> weighted_lights();

public:
  std::vector<Sky> skies;
  std::vector<glm::vec3> color_set = std::vector<glm::vec3>(NUM_SkyColorNames);

  explicit Skies(unsigned int mapid, Noggit::NoggitRenderContext context);
  unsigned int _map_id = 0; // continent id

  Sky* findSkyWeights(glm::vec3 pos);

  Sky* findClosestSkyByWeight();
  Sky* findClosestSkyByDistance(glm::vec3 pos);

  void setCurrentParam(int param_id);

  // Weather intensity (0 = clear .. 1 = full storm): blends every Light.dbc band toward the zone
  // light's STORM param set (the client's rainy-weather light change). Set per-frame by the renderer
  // from the editor's weather control.
  static void set_weather_intensity(float w);
  static float weather_intensity();

  // The CLEAR_WATER param's RIVER_COLOR_DARK band, weighted across the active lights each frame --
  // the flat colour the client paints exterior WMO liquid with (canal dark blue). Statics so
  // wmo_liquid::draw can read it without threading it through the draw chain (same pattern as the
  // weather intensity).
  static void set_water_river_dark(glm::vec3 const& c);
  static glm::vec3 water_river_dark();
  void setAreaLightId(int light_id);
  void update_sky_colors(glm::vec3 pos, int time);

  // Positional zone-light evaluation WITHOUT touching the camera-based weights/color_set: the client
  // lights each ENTITY by the zone light at ITS position (trace-proven: wow_cap_timbermaw shows
  // different M2s carrying the zone SH scaled by different per-channel tints at the same instant).
  void light_at(glm::vec3 const& pos, int time, glm::vec3* out_diffuse, glm::vec3* out_ambient) const;

  bool draw ( glm::mat4x4 const& model_view
            , glm::mat4x4 const& projection
            , glm::vec3 const& camera_pos
            , OpenGL::Scoped::use_program& m2_shader
            , math::frustum const& frustum
            , const float& cull_distance
            , int animtime
            , OutdoorLightStats const& light_stats
            );

  void drawLightingSpheres (glm::mat4x4 const& model_view
                          , glm::mat4x4 const& projection
                          , glm::vec3 const& camera_pos
                          , math::frustum const& frustum
                          , const float& cull_distance
                          );

  void drawLightingSphereHandles (glm::mat4x4 const& model_view
                                , glm::mat4x4 const& projection
                                , glm::vec3 const& camera_pos
                                , math::frustum const& frustum
                                , const float& cull_distance
                                , bool draw_spheres
  );


  bool hasSkies() { return true; }

  float river_shallow_alpha() const { return _river_shallow_alpha; }
  float river_deep_alpha() const { return _river_deep_alpha; }
  float ocean_shallow_alpha() const { return _ocean_shallow_alpha; }
  float ocean_deep_alpha() const { return _ocean_deep_alpha; }

  // LightFloatBand fog-distance -> world units: /36 (inches->yards) is CLIENT-CANON, proven by two
  // traces: kara map 532 authored 13000 -> client FOGEND 361.1 (=13000/36); Deadwind authored 28000
  // -> 777.8, farclip-clamped to the observed 777.0 (start -155.4 = -0.2*777.0 exactly). The /20
  // that briefly lived here was an editor-comfort hand-tune and made every zone's fog reach too far.
  float fog_distance_end() const { return _fog_distance / 36.f; };
  float fog_distance_start() const { return _fog_multiplier; };

  float glow() const { return _glow; };

  float fogRate() const { return _fog_rate; }

  // Sun (day) / moon (night) direction for the procedural cloud lighting -- fed each frame by
  // WorldRender (which owns the celestial arc); one frame of lag is irrelevant.
  void set_celestial_dir(glm::vec3 const& d) { _celestial_dir = d; }
  glm::vec3 const& celestial_dir() const { return _celestial_dir; } // water glitter keys on the drawn disc

  // [VULKAN] gradient-dome mirror. GL uploads these straight into its VBOs from locals; VK needs the
  // same arrays to build its own buffers. Positions/indices are static (built once in upload());
  // colours are re-derived whenever the zone light changes, which raises _vk_sky_dirty.
  std::vector<glm::vec3> const& vkDomeVertices() const { return _vk_dome_verts; }
  std::vector<glm::vec3> const& vkDomeColors() const { return _vk_dome_colors; }
  std::vector<std::uint16_t> const& vkDomeIndices() const { return _vk_dome_indices; }
  bool vkDomeDirty() const { return _vk_sky_dirty; }
  // Set while Vulkan draws the gradient dome, so GL skips ITS dome draw (and only that -- the
  // clouds, skybox, sun/moon and stars are still GL passes). Never set in parity mode: the
  // reference image has to keep rendering the full GL scene.
  void setVkOwnsDome(bool v) { _vk_owns_dome = v; }
  // The compose needs this: when VK owns the dome, GL draws no sky, so the compose must not discard
  // VK's far-plane pixels (the dome writes no depth and so sits at 1.0).
  [[nodiscard]] bool vkOwnsDome() const { return _vk_owns_dome; }

  // Cloud deck mirror: the cap mesh is static, the 128x128 RGBA texture is re-generated on a
  // 0.1s timer (tick_clouds) and the opacity follows the render/cloud_density setting.
  std::vector<float> const& vkCloudVertices() const { return _vk_cloud_verts; }
  std::vector<std::uint16_t> const& vkCloudIndices() const { return _vk_cloud_indices; }
  std::vector<std::uint8_t> const& vkCloudRgba() const { return _clouds.rgba; }
  float vkCloudOpacity() const { return _vk_cloud_opacity; }
  bool vkCloudMeshDirty() const { return _vk_cloud_mesh_dirty; }
  void clearVkCloudMeshDirty() { _vk_cloud_mesh_dirty = false; }
  unsigned vkCloudTexSerial() const { return _vk_cloud_tex_serial; }
  void setVkOwnsClouds(bool v) { _vk_owns_clouds = v; }

  // [VULKAN] the skybox / stars M2 this frame (null when the zone authors none / it is day).
  // They are ordinary M2 instances, so WorldRender hands them to the normal VK M2 feed.
  std::vector<ModelInstance*> const& vkSkyboxInstances() const { return _vk_skybox_instances; }
  // [client RE 2026-09-09] the strongest listed skybox weight (wow335a.exe FUN_007ef6e0 scales the
  // sun/moon by 1 - this) and whether a full-weight non-overlay skybox replaces the dome, the cloud
  // deck, the stars and the celestials this frame (FUN_007f09b0). See Skies::draw.
  float skyboxCover() const { return _skybox_cover; }
  bool skyboxCovers() const { return _skybox_covers; }
  ModelInstance* vkStarsInstance() const { return _vk_stars_instance; }
  void clearVkDomeDirty() { _vk_sky_dirty = false; }
  // [underwater] hide the cloud LAYER while the camera is submerged (user 2026-08-26: "don't
  // render the sky into the water") -- the texture keeps ticking on the AIR density, so
  // surfacing shows the intact deck instantly (no row-regen banding).
  void set_cloud_draw_suppressed(bool s) { _cloud_draw_suppressed = s; }

  void unload();

private:
  bool _uploaded = false;
  bool _need_color_buffer_update = true;
  bool _need_vao_update = true;

  int _indices_count;

  void upload();
  void update_color_buffer();
  void update_vao(OpenGL::Scoped::use_program& shader);

  OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vertex_array;
  GLuint const& _vao = _vertex_array[0];
  OpenGL::Scoped::deferred_upload_buffers<3> _buffers;
  GLuint const& _vertices_vbo = _buffers[0];
  GLuint const& _colors_vbo = _buffers[1];
  GLuint const& _indices_vbo = _buffers[2];

  std::unique_ptr<OpenGL::program> _program;

  // ===== Procedural cloud layer: byte-level RE of the 1.12 client's DayNight cloud system =====
  // wow.exe 5875: regen FUN_006cffc0, per-texel lighting FUN_006cfb00, alloc FUN_006d09b0, dome
  // mesh FUN_006d0530, alpha ramp FUN_006d0900, constants .rdata 0x811548..0x811614 + the classic
  // Perlin permutation table at 0x86f2d0 + octave-step table 0x86f3dc. The client draws ONE
  // zenith-centred polar-mapped dome (12 rows, per-row vertex alpha fading at the horizon)
  // sampling ONE dynamic ARGB texture it regenerates 32 rows per 0.1s from 4-octave 3D value
  // noise (x, y + a slow time axis: 1 lattice step per 256 full passes). Alpha = density pushed
  // through an exponential ramp minus a coverage threshold from Light-DBC float band 3; RGB is
  // lit per-texel from the density gradient vs the sun/moon direction with Light-DBC colors
  // 10 (sun-facing highlight), 11 (density-shade tint) and 12 (dense-core base).
  struct CloudGen
  {
    static constexpr int SIZE = 128;         // LOD 0 texture size (DAT_00811548[0])
    static constexpr int ROWS_PER_TICK = 32; // rows regenerated per 0.1s tick (FUN_006d09b0 default 0x20)
    std::vector<std::uint8_t> rgba;          // SIZE*SIZE*4
    std::vector<float> partial;              // per-texel density partial sum (octaves 0..2) of the current rows
    std::vector<glm::vec2> grad;             // per-texel (ddx, ddy) of that partial density
    std::vector<float> prev_row;             // previous row's partial density per column (persists across ticks)
    float value_table[256];                  // lattice values in [-1,1] (client fills from rand(), FUN_006d0c90)
    float ease[256];                         // cosine ease LUT (1 - cos(i*pi/256)) * 0.5 (FUN_006d0c90)
    std::uint8_t ramp[256];                  // alpha ramp 255 - 255 * 0.96^(0.6*i) (FUN_006d0900)
    int row_cursor = 0;
    unsigned pass_counter = 0;               // low byte -> eased time fraction, high byte -> z lattice cell
    float timer = 0.f;
    GLuint texture = 0;
    bool initialized = false;
  };
  CloudGen _clouds;
  // The cloud DOME is real geometry, exactly the client mesh (FUN_006d0530): one pole vertex +
  // 11 rings x 17 columns on a cap flattened by cos(45deg), baked polar UVs (radius row/11*0.5)
  // and per-row vertex alpha. Triangle interpolation of the UVs is what gives the client its
  // soft patchy zenith -- any per-pixel analytic mapping degenerates to a point there.
  std::unique_ptr<OpenGL::program> _cloud_program;
  GLuint _cloud_vao = 0;
  GLuint _cloud_vbo = 0;
  GLuint _cloud_ibo = 0;
  int _cloud_indices_count = 0;
  void init_cloud_gen();
  void tick_clouds(float dt_sec);
  void draw_clouds(glm::mat4x4 const& mvp, glm::vec3 const& camera_pos, int animtime, bool covered = false);
  glm::vec3 _celestial_dir = glm::vec3(0.f, 1.f, 0.f); // sun by day / moon by night (WorldRender feeds this)
  std::vector<glm::vec3> _vk_dome_verts;
  std::vector<glm::vec3> _vk_dome_colors;
  std::vector<std::uint16_t> _vk_dome_indices;
  bool _vk_sky_dirty = true;
  bool _vk_owns_dome = false;
  std::vector<float> _vk_cloud_verts;
  std::vector<std::uint16_t> _vk_cloud_indices;
  bool _vk_cloud_mesh_dirty = false;
  unsigned _vk_cloud_tex_serial = 0;   // bumped every tick_clouds regen
  float _vk_cloud_opacity = 0.f;
  bool _vk_owns_clouds = false;
  // [client RE 2026-09-09] the frame's skybox list in draw order (FUN_007f3230's slot walk).
  struct SkyboxEntry { ModelInstance* model; SkyParam* param; float weight; bool overlay; };
  std::vector<SkyboxEntry> _skybox_list;
  float _skybox_cover = 0.f;
  bool _skybox_covers = false;
  std::optional<glm::vec3> _flat_dome_color; // set while a full skybox replaces the dome (the fog colour)
  std::vector<ModelInstance*> _vk_skybox_instances;
  ModelInstance* _vk_stars_instance = nullptr;
  float _cloud_coverage = 0.f;               // Light-DBC float band 3 (cloud density), interpolated
  bool _cloud_draw_suppressed = false;       // see set_cloud_draw_suppressed()
  int _last_cloud_animtime = 0;

  Noggit::NoggitRenderContext _context;

  Noggit::Rendering::Primitives::Sphere _sphere_render;
};
