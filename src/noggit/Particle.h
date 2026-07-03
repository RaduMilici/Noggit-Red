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


struct RibbonSegment 
{
  glm::vec3 pos, up, back;
  float len, len0;
  RibbonSegment (::glm::vec3 pos_, float len_)
    : pos (pos_)
    , len (len_)
  {}
};

class RibbonEmitter 
{
  Model *model;

  Animation::M2Value<glm::vec3> color;
  Animation::M2Value<float, int16_t> opacity;
  Animation::M2Value<float> above, below;

  Bone *parent;

  glm::vec3 pos;

  int manim, mtime;
  int seglen;
  float length;

  glm::vec3 tpos;
  glm::vec4 tcolor;
  float tabove, tbelow;

  std::vector<uint16_t> _texture_ids;
  std::vector<uint16_t> _material_ids;

  std::list<RibbonSegment> segs;

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
