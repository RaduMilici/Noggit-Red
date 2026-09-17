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
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>
#include <utility>
#include <map>
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
  class TileRender;
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
  // IDENTITY defaults, not value-init: static WotLK models (trackless bones -> animBones=false) never
  // run the bone calc, so these defaults ARE the runtime matrices. M2 bind pose = identity (vertices
  // stored posed); value-init zeroed them, which collapsed every particle spawn basis to the model
  // origin -- MC VolcanicVent tip smoke pooled at the cone base instead of the tip.
  glm::mat4x4 mat = glm::mat4x4(1.0f);
  glm::mat4x4 mrot = glm::mat4x4(1.0f);

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
  // blend_anim >= 0 && blend_w < 1: cross-fade at the TRACK level, the client's anim-slot
  // architecture -- sample BOTH sequences' raw translation/rotation/scale, mix them (lerp/slerp),
  // and build ONE matrix. Blending finished matrices instead desynced bones from their pivots
  // mid-fade (stretched fingers/limbs). blend_w = weight of the CURRENT (anim,time) pose.
  void calcMatrix(glm::mat4x4 const& model_view
                 , Bone* allbones
                 , std::string const& model_name
                 , size_t bone_index
                 , int anim
                 , int time
                 , int animtime
                 , int blend_anim = -1
                 , int blend_time = 0
                 , float blend_w = 1.0f
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
  friend class Noggit::Rendering::TileRender;
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

  // the FULL key: a fileDataID (modern CASC clients) is authoritative, the path is for display/heuristics
  Model(BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context );

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
  // World-space particle emission for this model's NON-riding emitters (riding ones stay local by
  // definition). Set per frame before the sim ticks with the emitting instance's placement matrix;
  // the particle draw for such a model must use IDENTITY. Breath bubbles (see Particle.h).
  void setWorldSpaceParticleEmission(glm::mat4x4 const& m, float kill_plane_y = 1.0e30f);
  void clearWorldSpaceParticleEmission();
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
    // A MISSING M2 CANNOT AFFECT WHAT A SAVE WRITES -- user decision 2026-08-25, and the save path
    // proves it: MapTile::saveTile collects instances BY UID and writes MDDF straight from each
    // instance's own transform (nameID, uid, pos, rot, scale, flags). MDDF stores NO derived data,
    // there is no filter on load state, so a doodad whose file is absent round-trips byte-perfect.
    // The old blanket `true` therefore raised "saving will cause collision and culling issues" for
    // a risk that does not exist for models -- and fired constantly here, because turtle's own
    // Azeroth ADTs place WotLK doodads (hu_brick_pile01 x5, zuldrak_bats_lower_01 x4) that the
    // turtle asset tree never shipped, plus editor-only overlays (equipment, creature/GO previews).
    // WMOs KEEP the guard (WMO::is_required_when_saving): MODF writes extents[] derived from the
    // loaded object, so a failed WMO really would degrade the saved tile.
    return false;
  }

  [[nodiscard]]
  Noggit::Rendering::ModelRender* renderer() { return &_renderer; }

  [[nodiscard]] bool usesClassicLayout() const { return _uses_classic_layout; }
  // MD21-chunked (modern CASC) model: see unwrapMD21
  [[nodiscard]] bool isModernMD21() const { return _modern_md21; }

  // Render era of this FILE (docs/client_re/42): v256/257 classic layout = the 1.12 engine, v264 = the 3.3.5a
  // engine, MD21 container (v27x, every modern CASC client) = the Legion+ engine. Drives the skin shader-id
  // decoding, the uv sources, the alpha-key reference and the particle record semantics in ModelRender /
  // Particle. Per file, not per project: a modern project can still open a WotLK-era M2 by path.
  enum class M2RenderEra : std::uint8_t { Vanilla, WotLK, Modern };
  [[nodiscard]] M2RenderEra renderEra() const
  {
    return _uses_classic_layout ? M2RenderEra::Vanilla : (_modern_md21 ? M2RenderEra::Modern : M2RenderEra::WotLK);
  }

  // EXP2 (M2ExtendedParticle, wowdev M2#EXP2): per emitter {zSource, colorMult, alphaMult, alphaCutoff track}.
  struct ExtendedParticle
  {
    float z_source = 0.f;
    float color_mult = 1.f;
    float alpha_mult = 1.f;
    std::vector<std::uint16_t> alpha_cutoff_times;   // fixed16 life stamps (0..32767)
    std::vector<float> alpha_cutoff_values;          // fixed16 -> 0..1
  };
  [[nodiscard]] ExtendedParticle const* extendedParticle(std::size_t emitter) const
  {
    return emitter < _extended_particles.size() ? &_extended_particles[emitter] : nullptr;
  }
  // Read-only view of the material table (renderflag/blend pairs) -- the WorldRender light-shaft
  // deferral inspects pass unlit flags through this (Model::_render_flags itself is private).
  [[nodiscard]] std::vector<ModelRenderFlags> const& renderFlagsTable() const { return _render_flags; }
  [[nodiscard]] bool supportsTrackAnimations() const { return !_uses_classic_layout && !_animations_seq_per_id.empty(); }
  // True if this model actually has the given animation id (with sequences). Used to fall back to
  // Stand (0) when a forced weapon-idle (Ready1H/Ready2H) is requested on a model that lacks it.
  [[nodiscard]] bool hasAnimationId(int anim_id) const
  {
    auto const it = _animations_seq_per_id.find(static_cast<uint16_t>(anim_id));
    return it != _animations_seq_per_id.end() && !it->second.empty();
  }

  // Total length (ms) of an animation id, as Model::animate mods time by. 0 if absent. Used by the
  // full-day skybox path to map the day cycle onto the skybox M2's single animation.
  [[nodiscard]] uint32_t animationLength(int anim_id) const
  {
    auto const it = _animation_length.find(static_cast<int16_t>(anim_id));
    return it == _animation_length.end() ? 0u : it->second;
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

  // ===== M2 ANIM EVENTS (classic v256 models; doc 38 audio RE) =====
  // Parsed from the event defs' classic tracks: fire times per ANIMATION ID (anim-local ms,
  // normalized t - times[range.start] like every classic track). Multiple events can share a
  // fourcc (e.g. one $FSD per foot); variations of one anim id are merged into one list.
  struct ModelAnimEvent
  {
    std::uint32_t fourcc = 0;
    std::int32_t data = 0;
    std::map<std::int16_t, std::vector<int>> times_per_anim;
  };
  std::vector<ModelAnimEvent> _anim_events;
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

  // Modern (MD21-chunked) M2: the MD20 payload was unwrapped from its chunk (header offsets are
  // payload-relative), and skins / textures / external animations are addressed by fileDataID from the
  // SFID / TXID / AFID sibling chunks. See Model::unwrapMD21 and docs/client_re/41 section 7.
  bool _modern_md21 = false;
  std::vector<std::uint32_t> _skin_file_ids;
  std::vector<std::uint32_t> _texture_file_ids;
  std::map<std::pair<std::uint16_t, std::uint16_t>, std::uint32_t> _anim_file_ids;
  std::uint32_t _skeleton_file_id = 0; // SKID: the .skel that owns this model's bones/sequences (Legion 7.3+)
  bool graftSkeleton(std::vector<char>& payload);
  std::vector<ExtendedParticle> _extended_particles; // EXP2, one per emitter (empty when the chunk is absent)
  bool unwrapMD21(BlizzardArchive::ClientFile& f);
  BlizzardArchive::Listfile::FileKey modelSkinKey(std::size_t skin_index) const;
  // the model's path, or "" for an id-only (modern) key -- every path-based heuristic must tolerate ""
  std::string modelPath() const { return _file_key.hasFilepath() ? _file_key.filepath() : std::string(); }
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
  void calcBones(glm::mat4x4 const& model_view, int anim, int time, int animation_time,
                 int blend_anim = -1, int blend_time = 0, float blend_w = 1.0f);

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

  // Weapon-grip hand overlay. When active (set per-draw from ModelInstance::closeHandMain/Off), calcBones
  // runs the normal body pass then re-poses ONLY the finger-subtree bones from the HandsClosed sequence so a
  // held weapon gets a closed fist while the body keeps its idle. _hand_overlay_bones is built once at load =
  // finger bones (KeyBoneID 8..17) + their descendants, parents-first; _hand_overlay_bone_is_off tags each
  // as MAINHAND/right (kb 8-12, 0) or OFFHAND/left (kb 13-17, 1) so a fist closes ONLY on the hand that
  // actually holds a weapon (2026-07-25 -- a mainhand weapon used to close BOTH hands). The HandsClosed
  // sequence index is resolved from _animations_seq_per_id[15] at apply time.
  bool _hand_overlay_active_main = false; // close the RIGHT (mainhand) fingers this draw
  bool _hand_overlay_active_off = false;  // close the LEFT (offhand) fingers this draw
  std::vector<uint16_t> _hand_overlay_bones;
  std::vector<uint8_t> _hand_overlay_bone_is_off; // parallel to _hand_overlay_bones: 1 = left/offhand
  void applyHandGripOverlay(int time, int animtime);

  // Scratch poses used by animate()'s cross-fade on animation transitions: the blend-FROM pose and
  // a copy of the blend-TO pose (needed because the hierarchical blend overwrites bones[].mat while
  // children still read their parent's ORIGINAL to-pose). Transient per-draw.
  std::vector<glm::mat4x4> _blend_scratch;
  std::vector<glm::mat4x4> _blend_scratch_to;

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

  // [game mode 2026-08-10] Cross-ANIM-ID transition blend, client-style: when an INSTANCE's animation
  // id changes (Stand->Run, Run->Jump...), cross-fade from the old sequence's last sampled pose into
  // the new one over the client-typical M2 sequence blendTime, using the same rigid TRS blend as the
  // intra-id variation fades. Keyed like the idle scheduler (instance uid via _active_idle_key).
  struct AnimSwitchState
  {
    int last_anim_id = -1;
    int last_seq = 0;
    int last_seq_time = 0;
    long long switch_time = -1; // global anim ms when the current cross-fade started (-1 = none)
    long long anim_start = -1;  // global anim ms when the CURRENT anim id started: the client plays
                                // every sequence from ITS OWN time 0 on switch (a global-clock modulo
                                // lands mid-phase -- JumpEnd began half-played)
    int from_seq = 0;
    int from_time = 0;
    int blend_ms = 150;
    // ONE-SHOT VARIATION ROLL (client CM2Model::SetAnimation, RE'd 12340 FUN_00826e60): a
    // one-shot anim id (Death/JumpStart/JumpEnd/JumpLandRun) rolls its variation ONCE at entry
    // -- rand() in [0,0x7FFF] walked down the chain's frequency bands, falling off the end keeps
    // variation 0 -- then plays that sequence on the sequence-local clock, holding its last
    // frame. Stored here by rollForcedAnimVariation (called on the forced-anim transition), read
    // by animate() (selection + hold length) and forcedAnimVariationLengthMs (MapView timers).
    int oneshot_anim_id = -1;
    int oneshot_seq = -1;
    int oneshot_len = 0;
    uint32_t oneshot_rng = 0; // xorshift32 state, entropy-seeded on first roll (client: CRT rand)
  };
  std::unordered_map<std::uint64_t, AnimSwitchState> _anim_switch_states;

public:
  // Client-exact variation roll for a one-shot anim id entering play on instance `key` (see
  // AnimSwitchState.oneshot_*). No-ops (clears the stored roll) for non-one-shot ids or when the
  // id has no variation data.
  void rollForcedAnimVariation(std::uint64_t key, int anim_id);
  // Length (ms) the active/rolled sequence of this anim id will play for on instance `key`:
  // the rolled variation's length when a roll is stored, else the FIRST variation's length.
  // (Model::animationLength sums ALL variations -- wrong for one-shot hold windows.)
  [[nodiscard]] int forcedAnimVariationLengthMs(std::uint64_t key, int anim_id);
private:

public:
  // [game mode] LOWER-BODY TWIST (client strafe look): rotate the posed skeleton about the model
  // up axis by this angle, then counter-rotate the upper body (SpineLow keybone subtree) back --
  // legs and hips face the strafe direction while the torso keeps facing forward. Set per instance
  // before animate() (like the hand-grip overlay); 0 = off.
  float _lower_body_twist = 0.0f;
  // [game mode] playback-rate scale for the ACTIVE instance's sequence-local clock: the client has
  // no sprint animation -- fast movement plays Run scaled by unit_speed / sequence.moveSpeed.
  float _anim_time_scale = 1.0f;

  // [game mode] M2 COLLISION MESH (boundingVertices/Triangles, both eras): the client's walking
  // and camera collision test ONLY this dedicated mesh -- grass/clutter models ship none and are
  // walk-through. Physics probes use it; editor picking keeps the render geometry.
  std::vector<glm::vec3> _collision_vertices;
  std::vector<uint16_t> _collision_indices;

  // Authored moveSpeed (yd/s) of an animation id's first sequence; 0 if absent.
  [[nodiscard]] float animationMoveSpeed(int anim_id)
  {
    auto const it = _animations_seq_per_id.find(static_cast<uint16_t>(anim_id));
    if (it == _animations_seq_per_id.end() || it->second.empty())
    {
      return 0.0f;
    }
    return it->second.begin()->second.moveSpeed;
  }

  // Authored cross-fade duration (ms) for switching INTO this animation, version-gated by asset
  // era. WotLK (v264) sequences carry blendTime at +0x1C -- the field ModelHeaders misnames
  // "playSpeed" (client 12340 SetAnimation reads it as blend ms; "0 for some models" is simply
  // no-blend sequences). Classic-era conversions carry it in the parsed variation table instead.
  // 150 ms = client-typical fallback when neither has it.
  [[nodiscard]] int animationBlendTimeMs(int anim_id)
  {
    if (!_uses_classic_layout)
    {
      auto const it = _animations_seq_per_id.find(static_cast<uint16_t>(anim_id));
      if (it != _animations_seq_per_id.end() && !it->second.empty())
      {
        uint32_t const bt = it->second.begin()->second.playSpeed; // = wotlk blendTime (see above)
        if (bt > 0 && bt < 5000)
        {
          return static_cast<int>(bt);
        }
      }
    }
    else
    {
      auto const it = _anim_variations.find(static_cast<uint16_t>(anim_id));
      if (it != _anim_variations.end() && !it->second.empty() && it->second.front().blend_time > 0)
      {
        return static_cast<int>(std::min<uint32_t>(it->second.front().blend_time, 5000));
      }
    }
    return 150;
  }
private:
  int _twist_anchor_bone = -1;            // first bone with KeyBoneID 4 (SpineLow)
  std::vector<uint8_t> _twist_upper_bone; // per bone: 1 = SpineLow or its descendant (counter-rotated)
  int _twist_head_bone = -1;              // first bone with KeyBoneID 6 (Head)
  std::vector<uint8_t> _twist_head_subtree; // per bone: 1 = Head or its descendant
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

  // ParticleColor.dbc recolor sets (WotLK, checklist 12.10): CreatureDisplayInfo.particleColorID
  // resolves to 3 colour-sets of 3 ramp colours; emitters authored with particleColorIndex 11/12/13
  // take set [0/1/2] while that spawn's emitter state is swapped in (see swapInstanceEmitterState).
  std::unordered_map<std::uint64_t, std::array<std::array<glm::vec4, 3>, 3>> _instance_particle_color_sets;
  std::uint64_t _live_emitter_state_key = 0; // whose per-spawn state is currently swapped in (0 = shared)

public:
  void setInstanceParticleColorSets(std::uint64_t key, std::array<std::array<glm::vec4, 3>, 3> const& sets)
  {
    _instance_particle_color_sets[key] = sets;
  }

  // [mem 2026-08-26] inverse for spawn eviction: per-instance data stored on the SHARED model
  // must die with the instance or it outlives every evicted spawn for the whole session.
  void dropInstanceParticleColorSets(std::uint64_t key)
  {
    _instance_particle_color_sets.erase(key);
  }

  // All fire times (anim-local ms, sorted) of the given M2 event FourCC in animation anim_id --
  // e.g. '$FSD' footfalls of the Run cycle. Empty when the model authors none. (doc 38)
  std::vector<int> animEventTimes(std::uint32_t fourcc, std::int16_t anim_id) const;

  // GEOMETRY-MODEL particles (checklist 12.2): true when any emitter authors a geometry model.
  // WorldRender collects per-particle world transforms (inside the placement's live-state swap) and
  // draws that model through the instanced M2 path after the particle phase. Out-of-line: Model.h
  // can be parsed with ParticleSystem still incomplete (the Model<->Particle circular include).
  bool hasGeometryParticles() const;
  void appendGeometryParticleTransforms(glm::mat4x4 const& host_transform,
                                        std::unordered_map<std::string, std::vector<glm::mat4x4>>& out) const;

private:

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
