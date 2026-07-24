// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once
#include <math/frustum.hpp>
#include <glm/mat4x4.hpp>
#include <math/ray.hpp>
#include <noggit/Animated.h> // Animation::M2Value
#include <noggit/AsyncObject.h> // AsyncObject
#include <noggit/ModelHeaders.h>
#include <noggit/Particle.h>
#include <noggit/TextureManager.h>
#include <noggit/tool_enums.hpp>
#include <noggit/ContextObject.hpp>
#include <opengl/scoped.hpp>
#include <opengl/shader.fwd.hpp>
#include <ClientFile.hpp>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <noggit/rendering/ModelRender.hpp>

class Bone;
class Model;
class ModelInstance;
class ParticleSystem;
struct ParticleSystemLiveState;
class RibbonEmitter;

namespace Noggit::Rendering
{
  class ModelRender;
  struct ModelRenderPass;
  class WorldRender;
}


glm::vec3 fixCoordSystem(glm::vec3 v);

class Bone {
  Animation::M2Value<glm::vec3> trans;
  Animation::M2Value<glm::quat, packed_quaternion> rot;
  Animation::M2Value<glm::quat> classic_rot;
  Animation::M2Value<glm::vec3> scale;
  bool _uses_classic_rotation = false;

public:
  glm::vec3 pivot;
  int parent;

  typedef struct
  {
    uint32_t flag_0x1 : 1;
    uint32_t flag_0x2 : 1;
    uint32_t flag_0x4 : 1;
    uint32_t billboard : 1;
    uint32_t cylindrical_billboard_lock_x : 1;
    uint32_t cylindrical_billboard_lock_y : 1;
    uint32_t cylindrical_billboard_lock_z : 1;
    uint32_t flag_0x80 : 1;
    uint32_t flag_0x100 : 1;
    uint32_t transformed : 1;
    uint32_t unused : 20;
  } bone_flags;

  bone_flags flags;
  glm::mat4x4 mat = glm::mat4x4();
  glm::mat4x4 mrot = glm::mat4x4();

  // For billboard glow cards: the card's local-space basis derived from its geometry + UVs at load, so the
  // screen-aligned billboard can align the TEXTURE's authored up/right with screen up/right (cards are
  // authored with differing in-plane rolls, so a fixed local-axis mapping spins some sideways). normal is
  // the card's plane normal; up follows increasing texture V-up; right completes the frame. Defaults match
  // the old fixed mapping (local X normal / Z up / Y right) until computed.
  glm::vec3 bb_local_normal = glm::vec3(1, 0, 0);
  glm::vec3 bb_local_up     = glm::vec3(0, 0, 1);
  glm::vec3 bb_local_right  = glm::vec3(0, 1, 0);

  // Diagnostic: does this bone carry any animated transform track (translation/rotation)? Used to tell
  // whether a particle emitter's ring motion is bone-driven vs. purely a particle-param effect.
  bool hasTransformTracks()
  {
    return trans.uses(0) || rot.uses(0) || classic_rot.uses(0);
  }

  bool calc;
  void calcMatrix(glm::mat4x4 const& model_view
                 , Bone* allbones
                 , std::string const& model_name
                 , size_t bone_index
                 , int anim
                 , int time
                 , int animtime
                 );

  // Weapon-grip finger overlay: recompute THIS bone's matrix from a DIFFERENT sequence (HandsClosed),
  // using the parent's ALREADY-computed matrix -- no recursion, no calc-flag reset. Called only on the
  // finger-subtree bones after the normal (Stand) pass, so the fingers curl relative to the still-
  // Stand-posed (breathing) hand while the rest of the skeleton is untouched. Mirrors calcMatrix's
  // local-transform build exactly (same euler remap).
  void overrideLocalFromSeq(Bone* allbones, int seq, int time, int animtime);
  Bone ( const BlizzardArchive::ClientFile& f,
         const ModelBoneDef &b,
         int *global,
         const std::vector<std::unique_ptr<BlizzardArchive::ClientFile>>& animation_files
       );

  Bone ( const BlizzardArchive::ClientFile& f,
         const ClassicModelBoneDef &b,
         int *global
       );

};


class TextureAnim {
  Animation::M2Value<glm::vec3> trans;
  Animation::M2Value<glm::quat, packed_quaternion> rot;
  // Classic (1.12) rotation tracks store FLOAT quaternions; packed int16 quats are WotLK-era.
  // Same split Bone already has (rot vs classic_rot) -- without it, classic texanim rotations
  // misparsed float data as packed quats and portal swirls tumbled ("coin flip") instead of
  // spinning in the texture plane.
  Animation::M2Value<glm::quat> classic_rot;
  Animation::M2Value<glm::vec3> scale;
  bool _uses_classic_rotation = false;

public:
  glm::mat4x4 mat;

  void calc(int anim, int time, int animtime);
  TextureAnim(const BlizzardArchive::ClientFile& f, const ModelTexAnimDef &mta, int *global);
  TextureAnim(const BlizzardArchive::ClientFile& f, const ClassicModelTexAnimDef &mta, int *global);
};

struct ModelColor {
  Animation::M2Value<glm::vec3> color;
  Animation::M2Value<float, int16_t> opacity;

  ModelColor(const BlizzardArchive::ClientFile& f, const ModelColorDef &mcd, int *global);
  ModelColor(const BlizzardArchive::ClientFile& f, const ClassicModelColorDef &mcd, int *global);
};

struct ModelTransparency {
  Animation::M2Value<float, int16_t> trans;

  ModelTransparency(const BlizzardArchive::ClientFile& f, const ModelTransDef &mtd, int *global);
  ModelTransparency(const BlizzardArchive::ClientFile& f, const ClassicModelTransDef &mtd, int *global);
};


struct FakeGeometry
{
  FakeGeometry(Model* m);

  std::vector<glm::vec3> vertices;
  std::vector<uint16_t> indices;
};

struct ModelLight {
  int type, parent;
  glm::vec3 pos, tpos, dir, tdir;
  Animation::M2Value<glm::vec3> diffColor, ambColor;
  Animation::M2Value<float> diffIntensity, ambIntensity;
  // Attenuation range (model-local units, before the doodad placement scale). attEnd is the light's
  // radius; used to give point lights their REAL falloff distance instead of a hardcoded default.
  Animation::M2Value<float> attStart, attEnd;
  //Animation::M2Value<bool> Enabled;

  ModelLight(const BlizzardArchive::ClientFile&  f, const ModelLightDef &mld, int *global);
  ModelLight(const BlizzardArchive::ClientFile&  f, const ClassicModelLightDef &mld, int *global);
  void setup(int time, OpenGL::light l, int animtime);
};

class Model : public AsyncObject
{
  friend class Noggit::Rendering::ModelRender;
  friend struct Noggit::Rendering::ModelRenderPass;
  friend class Noggit::Rendering::WorldRender;
  friend class ParticleSystem;
  friend class RibbonEmitter;

public:
  template<typename T>
  static std::vector<T> M2Array(BlizzardArchive::ClientFile const& f, uint32_t offset, uint32_t count)
  {
    if (!count)
    {
      return {};
    }

    if (offset >= f.getSize() || count > (f.getSize() - offset) / sizeof(T))
    {
      return {};
    }

    T const* start = reinterpret_cast<T const*>(f.getBuffer() + offset);
    return std::vector<T>(start, start + count);
  }

  Model(const std::string& name, Noggit::NoggitRenderContext context );

  std::vector<std::pair<float, std::tuple<int, int, int>>> intersect (glm::mat4x4 const& model_view, math::ray const&, int animtime);

  void updateEmitters(float dt);

  // ---- Per-instance particle simulation (creature spawns) ----------------------------------------
  // Multiple spawns share ONE Model, but each animates at its own phase (animation_time_offset), so a
  // single shared particle list only ever tracks one spawn's body. Each spawn (keyed by guid) gets its
  // own live state, swapped into the shared ParticleSystem objects for the duration of its per-instance
  // update+draw. swapInstanceEmitterState() is symmetric -- call it once to swap a spawn's state IN,
  // again to swap it back OUT. updateParticleSystems() advances only the particle sim (no texture-anim;
  // that stays on the shared/global clock) using the bones/setup that animate() just applied.
  void swapInstanceEmitterState(std::uint64_t instance_key);
  void updateParticleSystems(float dt);
  void dropInstanceEmitterState(std::uint64_t instance_key);

  // True if any particle emitter has flag 0x10 ("particles ride the emitter transform"). Used by the
  // attachment renderer: aura effect models without it draw their particles with the BIND-pose
  // attachment placement so existing particles don't get dragged around by the animated bone
  // (matching the client, which simulates such particles in world space).
  bool particlesRideParent() const;

  void finishLoading() override;
  void waitForChildrenLoaded() override;

  [[nodiscard]]
  bool is_hidden() const { return _hidden; }

  void toggle_visibility() { _hidden = !_hidden; }
  void show() { _hidden = false ; }
  void hide() { _hidden = true; }

  [[nodiscard]]
  bool use_fake_geometry() const { return !!_fake_geometry; }

  // True if the model has an UNLIT + additive material (a self-illuminated glow layer: fires, braziers,
  // lamps, lava props). Data-driven signal for synthesizing a warm light from a doodad that carries no
  // authored M2 light -- replaces brittle filename-keyword matching.
  bool emitsLight() const { return _emits_light; }

  [[nodiscard]]
  bool animated_mesh() const { return (animGeometry || animBones); }

  [[nodiscard]]
  bool is_required_when_saving() const override
  {
    return true;
  }

  [[nodiscard]]
  Noggit::Rendering::ModelRender* renderer() { return &_renderer; }

  [[nodiscard]] bool usesClassicLayout() const { return _uses_classic_layout; }
  [[nodiscard]] bool supportsTrackAnimations() const { return !_uses_classic_layout && !_animations_seq_per_id.empty(); }
  // True if this model actually has the given animation id (with sequences). Used to fall back to
  // Stand (0) when a forced weapon-idle (Ready1H/Ready2H) is requested on a model that lacks it.
  [[nodiscard]] bool hasAnimationId(int anim_id) const
  {
    auto const it = _animations_seq_per_id.find(static_cast<uint16_t>(anim_id));
    return it != _animations_seq_per_id.end() && !it->second.empty();
  }

  // ===============================
  // Toggles
  // ===============================
  std::vector<bool> showGeosets;

  // ===============================
  // Texture data
  // ===============================
  std::vector<scoped_blp_texture_reference> _textures;
  std::vector<std::string> _textureFilenames;
  // ModelTextureDef.flags per texture slot (0x1 wrap X, 0x2 wrap Y; unset bit = clamp on that axis)
  std::vector<uint32_t> _texture_flags;
  std::map<std::size_t, scoped_blp_texture_reference> _replaceTextures;
  std::vector<int> _specialTextures;
  std::vector<bool> _useReplaceTextures;
  std::vector<int16_t> _texture_unit_lookup;
  std::vector<ModelAttachmentDef> _attachments;
  std::vector<int16_t> _attachment_lookup;

  // ===============================
  // Misc ?
  // ===============================
  std::vector<Bone> bones;
  std::vector<glm::mat4x4> bone_matrices;
  ModelHeader header;
  std::vector<uint16_t> blend_override;

  float rad;
  // Particle-free ground-footprint radius of the RENDER mesh (horizontal distance from the mesh's
  // horizontal centre to its farthest render vertex), in model space, in the BIND pose.
  float footprint_radius = 0.f;

  // EXACT client selection-circle base radius, reverse-engineered from wow.exe (FUN_00608e00 /
  // FUN_0060aee0): the ground circle radius = OBJECT_FIELD_SCALE_X * sqrt( sqrt(dx^2 + dy^2) * 0.5 ),
  // where dx,dy are the X/Y extents of the model's STAND animation (anim id 0) bounding box (each M2
  // sequence stores its own bounds). Using the stand box -- not the global header box -- is what
  // excludes attack/particle extents. Verified byte-exact vs apitrace (ManaFiend 1.834, Anomalus
  // 1.144, Drake 3.346). 0 = unavailable (fall back to footprint_radius). See noggit-selection-circle.
  float selection_base_radius = 0.f;

  // (unused, kept for reference) idle-pose footprint experiment -- superseded by selection_base_radius.
  float _idle_footprint_radius = -1.f;
  float idlePoseFootprint();
  float trans;
  bool animcalc;

  // ===============================
  // Geometry
  // ===============================

  std::vector<ModelVertex> _vertices;
  std::vector<ModelVertex> _current_vertices;

  std::vector<uint16_t> _indices;

  std::optional<FakeGeometry> _fake_geometry;

  // Emitter lights (campfires etc.) so the world renderer can collect them as scene point lights.
  [[nodiscard]] std::vector<ModelLight>& lights() { return _lights; }

private:
  struct ClassicStaticBone
  {
    std::uint32_t flags = 0;
    int parent = -1;
    glm::vec3 pivot = {};
    // Billboard glow/flame card texture basis (derived once from this card's geometry+UVs, like the
    // animated path's bb_local_*). basis_ok = a non-degenerate card was found for this billboard bone.
    bool basis_ok = false;
    glm::vec3 bb_local_normal = glm::vec3(1, 0, 0);
    glm::vec3 bb_local_up     = glm::vec3(0, 0, 1);
    glm::vec3 bb_local_right  = glm::vec3(0, 1, 0);
  };
  bool _static_bb_bases_computed = false;

  bool _per_instance_animation;
  bool _uses_classic_layout = false;
  bool _emits_light = false; // has an unlit+additive (emissive glow) material -- see emitsLight()
public:
  // Water-surface effect model (fishing-pool "school": foam ring + bubbles + sparkles). Its effect
  // geosets are authored blend=Opaque in the M2 (verified identical across Turtle/WotLK/converted
  // copies), so Noggit would draw the flat foam disc as a solid bright quad that z-fights the water.
  // Flagged at load (path = Tradeskill_FishSchool_*) so prepareDraw promotes the water-EFFECT geosets
  // (foam/bubble/sparkle textures) to alpha-blend + no depth-write -- translucent, flush on the water,
  // matching the in-game look -- while the solid debris (crates/barrels/fish) stay opaque.
  bool _water_surface_effect = false;
private:
public:
  // Ground-clutter detail doodads: the client renders these through a dedicated FULLBRIGHT detail
  // shader (FUN_006b2b80), ignoring per-material lighting. Left lit, the yellow Westfall atlas
  // picks up the zone's cyan daytime ambient and reads green. Set on the shared detail models.
  bool _force_unlit = false;
private:
  uint32_t _embedded_view_offset = 0;
  bool _logged_layout_summary = false;
  bool _logged_animation_branch = false;
  bool _bb_bases_computed = false;
  bool _logged_classic_character_geosets = false;
  std::uint32_t _logged_missing_special_texture_mask = 0;
  std::vector<ClassicStaticBone> _classic_static_bones;
  int _current_anim_seq;
  int _anim_time;
  int _global_animtime;

  Noggit::NoggitRenderContext _context;

  void initCommon(const BlizzardArchive::ClientFile& f);
  bool isAnimated(const BlizzardArchive::ClientFile& f);
  void initAnimated(const BlizzardArchive::ClientFile& f);
  bool initClassicStaticBones(const BlizzardArchive::ClientFile& f);
  void calcClassicStaticBones(glm::mat4x4 const& model_view);

  // upload_bones: when false, compute the bone matrices into bone_matrices[] but DO NOT touch GL (skip the
  // updateBoneMatrices TBO upload). This lets the CPU bone math run on a worker thread; the caller uploads
  // on the main thread afterwards. Default true preserves every single-threaded caller unchanged.
  void animate(glm::mat4x4 const& model_view, int anim_id, int anim_time, bool upload_bones = true);
  void calcBones(glm::mat4x4 const& model_view, int anim, int time, int animation_time);

  void lightsOn(OpenGL::light lbase);
  void lightsOff(OpenGL::light lbase);



  // ===============================
  // Animation
  // ===============================
  bool animated;
  bool animGeometry, animTextures, animBones;

  //      <anim_id, <sub_anim_id, animation>
  std::map<uint16_t, std::map<uint16_t, ModelAnimation>> _animations_seq_per_id;
  std::map<int16_t, uint32_t> _animation_length;

  // Weapon-grip hand overlay. When _hand_overlay_active (set per-draw from ModelInstance::closeHands),
  // calcBones runs the normal body pass then re-poses ONLY the finger-subtree bones from the HandsClosed
  // sequence so a held weapon gets a closed fist while the body keeps its idle. _hand_overlay_bones is
  // built once at load = finger bones (KeyBoneID 8..17) + their descendants, in parents-first order.
  // The HandsClosed sequence index is resolved from _animations_seq_per_id[15] at apply time.
  bool _hand_overlay_active = false;
  std::vector<uint16_t> _hand_overlay_bones;
  void applyHandGripOverlay(int time, int animtime);

  // Scratch pose (final bone matrices of the blend-FROM sequence) used by animate()'s cross-fade on
  // animation transitions. Transient per-draw, like the bone pose itself.
  std::vector<glm::mat4x4> _blend_scratch;

  // Idle-variation scheduling (client-accurate). Per animID, the playable variations with the authored
  // M2Sequence fields the 1.12 client uses to schedule them: a frequency-weighted roulette picks a
  // variation, it plays for length*replayCount (replay in [replayMin,replayMax]), then re-rolls; each
  // transition cross-fades over blendTime. Parsed from ClassicModelAnimation at load.
  struct AnimVariation
  {
    int      seq_index;   // sequence index (== ModelAnimation.Index), what the bone tracks are keyed by
    uint32_t length;      // duration ms
    uint16_t frequency;   // roulette weight (variations of one animID sum to ~0x7FFF)
    uint32_t replay_min;
    uint32_t replay_max;
    uint32_t blend_time;  // ms
  };
  std::map<uint16_t, std::vector<AnimVariation>> _anim_variations;

  // Per-instance idle schedule state (keyed by ModelInstance uid). Persists across draws so each spawn
  // follows its OWN randomized variation timeline (they don't lean in unison). Advanced incrementally in
  // animate() from the per-instance anim_time clock.
  struct IdleSchedule
  {
    bool     init = false;
    int      anim_id = -1;
    uint32_t rng = 0;
    int      cur_seq = -1;
    int      cur_len = 1;
    int      cur_blend = 0;
    long long play_start = 0;
    long long play_end = 0;
    int      prev_seq = -1;
    int      prev_len = 1;
  };
  std::unordered_map<std::uint64_t, IdleSchedule> _idle_schedules;
  std::uint64_t _active_idle_key = 0;
  // Advances the active instance's idle schedule to anim_time and fills the current/blend selection.
  // Returns false if this animID has no variation data (caller uses the legacy path). do_blend/out params
  // mirror animate()'s cross-fade inputs.
  bool advanceIdleSchedule(int anim_id, long long anim_time,
                           int& out_seq, int& out_time,
                           bool& out_do_blend, int& out_blend_seq_from, int& out_blend_time_from,
                           float& out_blend_w);

  std::vector<ModelRenderFlags> _render_flags;
  std::vector<ParticleSystem> _particles;
  std::vector<RibbonEmitter> _ribbons;

  // Per-spawn live particle state (see swapInstanceEmitterState). Keyed by creature spawn guid; each
  // entry holds one ParticleSystemLiveState per _particles emitter. Default states pre-warm on first update.
  std::unordered_map<std::uint64_t, std::vector<ParticleSystemLiveState>> _instance_emitter_states;

  std::vector<int> _global_sequences;
  std::vector<TextureAnim> _texture_animations;
  float _emitter_anim_accum_ms = 0.f; // continuous clock to drive texture anims for models that never reach animate()
  std::vector<int16_t> _texture_animation_lookups;
  std::vector<uint16_t> _texture_lookup;

  // ===============================
  // Material
  // ===============================
  std::vector<ModelColor> _colors;
  std::vector<ModelTransparency> _transparency;
  std::vector<int16_t> _transparency_lookup;
  std::vector<ModelLight> _lights;

  Noggit::Rendering::ModelRender _renderer;

  bool _hidden = false;
};
