// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/Animated.h> // Animation::M2Value
#include <noggit/Model.h>
#include <noggit/TextureManager.h>
#include <opengl/scoped.hpp>
#include <opengl/shader.fwd.hpp>

#include <array>
#include <cstdint>
#include <list>
#include <deque>
#include <memory>
#include <vector>

class Bone;
class Model;
class ParticleSystem;
class RibbonEmitter;

namespace BlizzardArchive
{
  class ClientFile;
}

struct Particle {
  glm::vec3 pos, speed, down, origin, dir;
  glm::vec3  corners[4];
  //glm::vec3 tpos;
  float size, life, maxlife;
  unsigned int tile;
  glm::vec4 color;
  // Stable per-particle index (the client's memory-slot index). Drives the twinkle noise-table phase
  // and the alternating spin sign (client uses slot parity when emitter flag 0x8000 is set).
  unsigned int slot = 0;
};

typedef std::list<Particle> ParticleList;

// Per-instance live particle-simulation state (see ParticleSystem::swapLiveState / Model per-spawn state).
// A shared M2 model is drawn once per creature spawn at that spawn's OWN animation phase, so a single live
// particle list can only ever stay glued to one spawn's body. Each spawn keeps its own copy of this and
// swaps it into the shared ParticleSystem around its per-instance update+draw. Only fields that persist
// across frames belong here; everything else is re-derived each frame from setup()/the emitter def.
// Standalone (not nested in ParticleSystem) so Model.h can reference it under the Model<->Particle
// circular include without needing ParticleSystem to be a complete type there.
struct ParticleSystemLiveState
{
  ParticleList particles;
  float rem = 0.0f;
  bool prewarmed = false;
};

class ParticleEmitter {
public:
  explicit ParticleEmitter() {}
  virtual ~ParticleEmitter() {}
  virtual Particle newParticle(ParticleSystem* sys, int anim, int time, int animtime, float w, float l, float spd, float var, float spr, float spr2) = 0;
};

class PlaneParticleEmitter : public ParticleEmitter {
public:
  explicit PlaneParticleEmitter() {}
  Particle newParticle(ParticleSystem* sys, int anim, int time, int animtime, float w, float l, float spd, float var, float spr, float spr2);
};

class SphereParticleEmitter : public ParticleEmitter {
public:
  explicit SphereParticleEmitter() {}
  Particle newParticle(ParticleSystem* sys, int anim, int time, int animtime, float w, float l, float spd, float var, float spr, float spr2);
};

struct TexCoordSet {
    glm::vec2 tc[4];
};

class ParticleSystem 
{
  Model *model;
  int emitter_type;
  std::unique_ptr<ParticleEmitter> emitter;
  Animation::M2Value<float> speed, variation, spread, lat, gravity, lifespan, rate, areal, areaw, deacceleration;
  Animation::M2Value<uint8_t> enabled;
  std::array<glm::vec4, 3> colors;
  std::array<float,3> sizes;
  float mid, slowdown;
  float _spin = 0.f; // emitter spin (revolutions/sec): for a spline emitter this is how fast the
                     // emission point travels along the spline path. From M2 particle params; 0 = none.
  // Spline emitter path (M2 EmitterType 3, e.g. the MC flamecircle's ring). Particles are emitted along
  // this closed loop of points; the emission position advances around it at _spin rev/sec. Empty = not a
  // spline emitter. Points are stored in Noggit render space (fixCoordSystem applied), emitter-local.
  std::vector<glm::vec3> _spline_points;

  // Client-exact per-particle motion params (RE'd from CParticleEmitter2, 1.12 build 5875).
  // WIND (§4): a young-only acceleration -- while a particle's age < _wind_time, vel += _wind * dt.
  // _wind is the authored windVector in Noggit render space (fixCoordSystem). Wind-blown dust/snow/embers.
  glm::vec3 _wind = glm::vec3(0.0f);
  float _wind_time = 0.0f;
  // TWINKLE (§5): sprite shimmer. Per particle per frame the client samples a shared 128-entry random
  // noise table at idx=(floor(twinkleSpeed*age)+slot)&0x7f -> t; culls the sprite when twinklePercent<t,
  // and scales size by lerp(scaleMin,scaleMax,t). 1.0/1.0 min==max and speed 0 percent>=1 = inert.
  float _twinkle_speed = 0.0f;
  float _twinkle_percent = 1.0f;
  float _twinkle_scale_min = 1.0f;
  float _twinkle_scale_max = 1.0f;
  // SPIN sign (§2): when emitter flag 0x8000 is set the client alternates the sprite spin direction by
  // particle slot parity (its "random" sign). Base angle = _spin * age is unchanged.
  bool _spin_alternate = false;
  // Running spawn counter -> each new particle's stable slot (see Particle::slot).
  unsigned int _spawn_seq = 0;

  // TAIL streak (type/ParticleType 1=Tail, 2=Both, client FUN_007b2a50 tail branch): each particle is
  // drawn as a motion streak of _tail_length * (screen-projected velocity) trailing behind it, billboarded
  // about the velocity axis, width = particle size. _tail_clamp_age (emitter flag 0x400) clamps the streak
  // to the particle's age so a fresh particle's tail grows in rather than popping to full length.
  float _tail_length = 0.0f;
  bool _tail_clamp_age = false;

  glm::vec3 pos;
  uint16_t _texture_id;
  ParticleList particles;
  int blend, order, type;
  int manim, mtime;
  int manimtime;
  int rows, cols;
  std::vector<TexCoordSet> tiles;
  // Flipbook cell animation (classic M2 lifespanUVAnim/decayUVAnim = [startCell,endCell,repeat]).
  // When the emitter authors a real cell range, the texture cell advances sequentially over each
  // particle's lifetime (explosions, spell impacts, the 8x8 effect sheets) instead of freezing on a
  // random cell. _uv_animated stays false for degenerate (0,0,1) sheets -> those keep random tiles.
  bool _uv_animated = false;
  int _uv_seq_start = 0;
  int _uv_seq_end = 0;
  int _uv_repeat = 1;
  // WotLK keys the same animation instead of ramping it: parallel arrays of normalised life time
  // (0..1, from the fixed16 track times) and cell index. Non-empty selects the keyed evaluation;
  // classic emitters leave these empty and keep using the _uv_seq_* ramp above. Constant tracks are
  // rejected at parse time (they are default-filled, not authored) and keep their random tile.
  std::vector<float> _cell_times;
  std::vector<int> _cell_values;
  // Colour / alpha / size over particle life. Classic stores exactly three inline keys and ramps them
  // through `mid`; WotLK reaches the same curves through M2PartTrack (keyed times + values) and may
  // author any number of keys at arbitrary times. Reading just the first three and evaluating them at
  // 0 / mid / 1 -- which is what the classic ramp does -- both DROPS keys and MISTIMES the ones it
  // keeps, so a flame that should hold its colour and fade late instead crossfades linearly. Non-empty
  // selects keyed evaluation; classic emitters leave these empty and keep the three-key ramp.
  std::vector<float> _color_times;
  std::vector<glm::vec3> _color_values;
  std::vector<float> _alpha_times;
  std::vector<float> _alpha_values;
  std::vector<float> _size_times;
  std::vector<float> _size_values;
  void initTile(glm::vec2 *tc, int num);
  bool billboard;
  bool classic;
  std::uint32_t debug_update_log_count = 0;
  std::uint32_t debug_draw_log_count = 0;

  // Pre-warm guard: simulate one lifespan on first tick so ambient emitters (dusty light-ray motes,
  // smoke) are already populated/scattered instead of slowly filling in from empty after a load.
  bool _prewarmed = false;

  float rem;
  //bool transform;

  // unknown parameters omitted for now ...
  Bone *parent;
  // The emitter's bone INDEX. `parent` is re-resolved from this each frame in setup() -- caching a Bone*
  // at construction goes stale (the model's bones vector is rebuilt/reallocated after the emitter is
  // created), which left parent->mat reading ZERO so every particle spawned at the model origin emitting
  // straight up instead of following the animated bone.
  int16_t _bone_index = 0;
  int32_t flags;

public:
  float tofs;

  ParticleSystem(Model*, const BlizzardArchive::ClientFile& f, const ModelParticleEmitterDef &mta,
                 int *globals, Noggit::NoggitRenderContext context);
  ParticleSystem(Model*, const BlizzardArchive::ClientFile& f, const ClassicModelParticleEmitterDef& mta,
                 int* globals, Noggit::NoggitRenderContext context);

  ParticleSystem(ParticleSystem const& other);
  ParticleSystem(ParticleSystem&&);
  ParticleSystem& operator= (ParticleSystem const&) = delete;
  ParticleSystem& operator= (ParticleSystem&&) = delete;

  void update(float dt);

  void setup(int anim, int time, int animtime);
  void draw( glm::mat4x4 const& model_view
           , OpenGL::Scoped::use_program& shader
           , GLuint const& transform_vbo
           , int instances_count
           );

  friend class PlaneParticleEmitter;
  friend class SphereParticleEmitter;

  // M2 global texture index this emitter draws with. Used to detect "particle-placeholder" mesh
  // submeshes (a mesh whose texture is also a particle texture; the client draws the particles and
  // skips the mesh -- e.g. the Anomalus MANAMISTBASE torso mesh).
  uint16_t textureId() const { return _texture_id; }

  // Raw M2 emitter flags. Bit 0x10 = particles are simulated relative to the emitter (ride its
  // transform); without it the client leaves spawned particles behind in world space.
  int32_t emitterFlags() const { return flags; }

  // Flag 0x10: the client stores these particles in EMITTER-LOCAL space (CParticle2 +0x00 is
  // emitter-local, RE handoff 6.4) so live particles RIDE the animated bone -- e.g. the instance
  // portal's swirl ring rotating with its spinning bone like a wheel. Spawn keeps positions local
  // and draw() transforms by the bone's CURRENT matrix. Without the flag, spawn bakes the bone
  // matrix and particles stay where they were emitted (trailing behind an animated bone).
  bool ridesParent() const { return (flags & 0x10) != 0 && parent != nullptr; }

  // Swap this emitter's live simulation state (particle list, spawn remainder, pre-warm guard) with an
  // external holder. Symmetric: call once to swap a spawn's state IN, again to swap it back OUT. Used to
  // give each creature spawn of a shared model its own particle simulation (see ParticleSystemLiveState).
  void swapLiveState(ParticleSystemLiveState& s)
  {
    particles.swap(s.particles);
    std::swap(rem, s.rem);
    std::swap(_prewarmed, s.prewarmed);
  }

  void unload();

private:
  bool _uploaded = false;
  void upload();

  OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vertex_array;
  GLuint const& _vao = _vertex_array[0];
  OpenGL::Scoped::deferred_upload_buffers<5> _buffers;
  GLuint const& _vertices_vbo = _buffers[0];
  GLuint const& _offsets_vbo = _buffers[1];
  GLuint const& _colors_vbo = _buffers[2];
  GLuint const& _texcoord_vbo = _buffers[3];
  GLuint const& _indices_vbo = _buffers[4];
  Noggit::NoggitRenderContext _context;
};


// One ribbon EDGE, client-exact (CRibbonEmitter RE, docs/client_re/25): both vertex positions are
// BAKED at spawn (the trail does not follow the bone afterwards); gravity displaces them over time
// and the age drives the U texture coordinate (age/lifetime across the tex slot cell).
struct RibbonEdge
{
  glm::vec3 above, below;
  float age;
};

class RibbonEmitter 
{
  Model *model;

  Animation::M2Value<glm::vec3> color;
  Animation::M2Value<float, int16_t> opacity;
  Animation::M2Value<float> above, below;
  // Texture-slot + visibility tracks (client RE, docs/client_re/25): unk1 = int16 slot index into
  // the s1 x s2 cell grid (SetTexSlot @007b7b70 -> cell UV recompute @007b6f30), unk2 = uint8
  // visibility (SetVisible @007b7b40: 0 = stop emitting + drop the head; the trail still ages out).
  Animation::M2Value<int, int16_t> tex_slot_track;
  Animation::M2Value<int, uint8_t> visibility_track;

  Bone *parent;

  glm::vec3 pos;

  int manim, mtime;
  // Client-exact parameters (CRibbonEmitter::Initialize RE): edges/second emission rate, edge
  // lifetime in seconds (= trail duration), gravity, and the texture slot grid (rows x cols).
  float edges_per_sec;
  float edge_lifetime;
  float gravity;
  int tex_rows, tex_cols;

  glm::vec3 tpos;
  glm::vec4 tcolor;
  float tabove, tbelow;

  // Emission state: fractional edge accumulator + the previous frame's anchor points for the
  // client's sub-frame interpolated spawning; last anim timestamp for dt.
  float _emit_accum = 0.0f;
  bool _have_prev = false;
  glm::vec3 _prev_above = glm::vec3(0.f), _prev_below = glm::vec3(0.f);
  glm::vec3 _cur_above = glm::vec3(0.f), _cur_below = glm::vec3(0.f);
  int _last_animtime = -1;
  // Sampled per frame from the tracks above (client defaults: slot 0, visible).
  int _cur_slot = 0;
  bool _visible = true;

  std::vector<uint16_t> _texture_ids;
  std::vector<uint16_t> _material_ids;

  std::deque<RibbonEdge> edges; // front = newest

public:
  RibbonEmitter(Model*, const BlizzardArchive::ClientFile &f, ModelRibbonEmitterDef const& mta, int *globals
                , Noggit::NoggitRenderContext context);
  // Classic (1.12) overload: ClassicAnimationBlock-based tracks (lava-fall, weapon/spell/mount trails).
  RibbonEmitter(Model*, const BlizzardArchive::ClientFile &f, ClassicModelRibbonEmitterDef const& mta, int *globals
                , Noggit::NoggitRenderContext context);

  RibbonEmitter(RibbonEmitter const& other);
  RibbonEmitter(RibbonEmitter&&);
  RibbonEmitter& operator= (RibbonEmitter const&) = delete;
  RibbonEmitter& operator= (RibbonEmitter&&) = delete;

  void setup(int anim, int time, int animtime);
  void draw( OpenGL::Scoped::use_program& shader
           , GLuint const& transform_vbo
           , int instances_count
           );

  void unload();

private:
  bool _uploaded = false;
  void upload();

  OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vertex_array;
  GLuint const& _vao = _vertex_array[0];
  OpenGL::Scoped::deferred_upload_buffers<3> _buffers;
  GLuint const& _vertices_vbo = _buffers[0];
  GLuint const& _texcoord_vbo = _buffers[1];
  GLuint const& _indices_vbo = _buffers[2];
  Noggit::NoggitRenderContext _context;
};
