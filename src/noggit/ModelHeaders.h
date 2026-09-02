// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

#pragma pack(push,1)

struct Vertex {
  float tu, tv;
  float x, y, z;
};

struct packed_quaternion
{
    int16_t x;
    int16_t y;
    int16_t z;
    int16_t w;
};

struct ModelHeader {
  char id[4];
  uint8_t version[4];
  uint32_t nameLength;
  uint32_t nameOfs;
  uint32_t Flags;

  uint32_t nGlobalSequences;
  uint32_t ofsGlobalSequences;
  uint32_t nAnimations;
  uint32_t ofsAnimations;
  uint32_t nAnimationLookup;
  uint32_t ofsAnimationLookup;
  uint32_t nBones;
  uint32_t ofsBones;
  uint32_t nKeyBoneLookup;
  uint32_t ofsKeyBoneLookup;

  uint32_t nVertices;
  uint32_t ofsVertices;
  uint32_t nViews;

  uint32_t nColors;
  uint32_t ofsColors;

  uint32_t nTextures;
  uint32_t ofsTextures;

  uint32_t nTransparency; // H
  uint32_t ofsTransparency;
  uint32_t nTexAnims; // J
  uint32_t ofsTexAnims;
  uint32_t nTexReplace;
  uint32_t ofsTexReplace;

  uint32_t nRenderFlags; // Render Flags
  uint32_t ofsRenderFlags; // Blending modes / render flags.
  uint32_t nBoneLookup; // BonesAndLookups
  uint32_t ofsBoneLookup; // A bone lookup table.

  uint32_t nTexLookup;
  uint32_t ofsTexLookup;

  uint32_t nTexUnitLookup; // L
  uint32_t ofsTexUnitLookup;
  uint32_t nTransparencyLookup; // M
  uint32_t ofsTransparencyLookup;
  uint32_t nTexAnimLookup;
  uint32_t ofsTexAnimLookup;

  glm::vec3 bounding_box_min;
  glm::vec3 bounding_box_max;
  float bounding_box_radius;
  glm::vec3 collision_box_min;
  glm::vec3 collision_box_max;
  float collision_box_radius;

  uint32_t nBoundingTriangles;
  uint32_t ofsBoundingTriangles;
  uint32_t nBoundingVertices;
  uint32_t ofsBoundingVertices;
  uint32_t nBoundingNormals;
  uint32_t ofsBoundingNormals;

  uint32_t nAttachments; // O
  uint32_t ofsAttachments;
  uint32_t nAttachLookup; // P
  uint32_t ofsAttachLookup;
  uint32_t nEvents; // SoundEvents
  uint32_t ofsEvents;
  uint32_t nLights; // R
  uint32_t ofsLights;
  uint32_t nCameras; // S
  uint32_t ofsCameras;
  uint32_t nCameraLookup;
  uint32_t ofsCameraLookup;
  uint32_t nRibbonEmitters; // U
  uint32_t ofsRibbonEmitters;
  uint32_t nParticleEmitters; // V
  uint32_t ofsParticleEmitters;

};


//only available if header->flags &0x8
//something about shadingflags
struct ofsUnk{
  int nUnk;
  int ofsnk;
};

struct ModelAnimation {
  int16_t animID;
  int16_t subAnimID;
  uint32_t length;

  float moveSpeed;

  uint32_t loopType;
  uint32_t flags;
  uint32_t uid;
  uint32_t d2;
  uint32_t playSpeed; // note: this can't be play speed because it's 0 for some models

  glm::vec3 boxA, boxB;
  float rad;

  int16_t NextAnimation;
  int16_t Index;
};

struct AnimationBlock {
  int16_t type; // interpolation type (0=none, 1=linear, 2=hermite, 3=Bezier)
  int16_t seq; // global sequence id or -1
  uint32_t nTimes;
  uint32_t ofsTimes;
  uint32_t nKeys;
  uint32_t ofsKeys;
};

struct ClassicAnimationRange {
  uint32_t start;
  uint32_t end;
};

struct ClassicAnimationBlock {
  int16_t type; // interpolation type (0=none, 1=linear, 2=hermite, 3=Bezier)
  int16_t seq; // global sequence id or -1
  uint32_t nRanges;
  uint32_t ofsRanges;
  uint32_t nTimes;
  uint32_t ofsTimes;
  uint32_t nKeys;
  uint32_t ofsKeys;
};

struct FakeAnimationBlock {
  uint32_t nTimes;
  uint32_t ofsTimes;
  uint32_t nKeys;
  uint32_t ofsKeys;
};

struct AnimationBlockHeader {
  uint32_t nEntries;
  uint32_t ofsEntries;
};

struct AnimSubStructure {
  uint32_t n;
  uint32_t ofs;
};

struct ModelTexAnimDef {
  AnimationBlock trans, rot, scale;
};

struct ClassicModelTexAnimDef {
  ClassicAnimationBlock trans, rot, scale;
};

struct ModelVertex {
  glm::vec3 position;
  uint8_t weights[4];
  uint8_t bones[4];
  glm::vec3 normal;
  glm::vec2 texcoords[2];
};

struct ModelView {
  char id[4]; // Signature
  uint32_t n_index, ofs_index; // Vertices in this model (index into vertices[])
  uint32_t n_triangle, ofs_triangle; // indices
  uint32_t n_vertex_property, ofs_vertex_property; // additional vtx properties
  uint32_t n_submesh, ofs_submesh; // materials/renderops/submeshes
  uint32_t n_texture_unit, ofs_texture_unit; // material properties/textures
  int32_t lod; // LOD bones
};

/// One material + render operation
struct ModelGeoset {
  uint16_t id; // mesh part id?
  uint16_t d2; // ?
  uint16_t vstart; // first vertex
  uint16_t vcount; // num vertices
  uint16_t istart; // first index
  uint16_t icount; // num indices
  uint16_t d3; // number of bone indices
  uint16_t d4; // ? always 1 to 4
  uint16_t d5; // ?
  uint16_t d6; // root bone?
  glm::vec3 BoundingBox[2];
  float radius;
};

/// A texture unit (sub of material)
struct ModelTexUnit {
  // probably the texture units
  // size always >=number of materials it seems
  uint8_t flags;    // Flags
  uint8_t priority_plane;
  uint16_t shader_id;    // If set to 0x8000: shaders. Used in skyboxes to ditch the need for depth buffering. See below.
  uint16_t submesh;      
  uint16_t geoset_index;  
  int16_t color_index;  // color or -1
  uint16_t renderflag_index;  // more flags...
  uint16_t material_layer;    // Texture unit (0 or 1)
  uint16_t texture_count;      // ? (seems to be always 1)
  uint16_t texture_combo_index;  // Texture id (index into global texture list)
  uint16_t texture_coord_combo_index;  
  uint16_t transparency_combo_index;    // transparency id (index into transparency list)
  uint16_t animation_combo_index;  // texture animation id
};

// block X - render flags
struct ModelRenderFlags {
  struct 
  {
    uint16_t unlit : 1;
    uint16_t unfogged : 1;
    uint16_t two_sided : 1;
    uint16_t billboard : 1;
    uint16_t z_buffered : 1;
    uint16_t unused : 11;
  }flags;
  uint16_t blend;
};

// block G - color defs
struct ModelColorDef {
  AnimationBlock color;
  AnimationBlock opacity;
};

// Classic (1.12) color defs: same two tracks in the older 28-byte ClassicAnimationBlock form
// (mirrors ClassicModelTransDef below).
struct ClassicModelColorDef {
  ClassicAnimationBlock color;
  ClassicAnimationBlock opacity;
};

// block H - transp defs
struct ModelTransDef {
  AnimationBlock trans;
};

struct ClassicModelTransDef {
  ClassicAnimationBlock trans;
};

struct ModelTextureDef {
  uint32_t type;
  uint32_t flags;
  uint32_t nameLen;
  uint32_t nameOfs;
};
struct ModelLightDef {
  int16_t type;
  int16_t bone;
  glm::vec3 pos;
  AnimationBlock ambColor;
  AnimationBlock ambIntensity;
  AnimationBlock color;
  AnimationBlock intensity;
  AnimationBlock attStart;
  AnimationBlock attEnd;
  AnimationBlock Enabled;
};

// Classic (1.12, M2 version 256) light definition: same leading fields, but the animated tracks use
// the older ClassicAnimationBlock layout (28 bytes, with the extra nRanges/ofsRanges) instead of the
// WotLK AnimationBlock (20 bytes). Total size = 2 + 2 + 12 + 7*28 = 212 bytes.
struct ClassicModelLightDef {
  int16_t type;
  int16_t bone;
  glm::vec3 pos;
  ClassicAnimationBlock ambColor;
  ClassicAnimationBlock ambIntensity;
  ClassicAnimationBlock color;
  ClassicAnimationBlock intensity;
  ClassicAnimationBlock attStart;
  ClassicAnimationBlock attEnd;
  ClassicAnimationBlock Enabled;
};

struct ModelCameraDef {
  int32_t id;
  float fov, farclip, nearclip;
  AnimationBlock transPos;
  glm::vec3 pos;
  AnimationBlock transTarget;
  glm::vec3 target;
  AnimationBlock rot;
};

// M2ParticleOld "old params" tail (WotLK v264). This block is byte-identical in layout to
// ClassicModelParticleParams below -- same fields in the same order -- the only difference is that the
// WotLK version reaches its per-life ramps through M2PartTrack (FakeAnimationBlock: keyed times+values)
// where classic stores 3 inline keys. It used to carry the legacy WoWModelViewer guess-names
// (unk/scales/rotation/Rot1/Rot2/Trans/f2), which hid real authored fields: what was called "Intensity"
// is the flipbook HEAD CELL track, and "scales" is actually {twinkleScaleMin, twinkleScaleMax,
// burstMultiplier} -- neither is an intensity nor a size multiplier. Field identity confirmed against
// 26030 emitters in the 3.3.5a client: twinkleScale is an ordered (min<=max) pair in every emitter,
// emitterType==3 count equals the spline-point count exactly, and the head cell keys are in-range cell
// indices for their rows*cols sheet. Offsets below are from the start of the emitter record.
struct ModelParticleParams {
  FakeAnimationBlock colors;        // 0x104 M2PartTrack<fixed16 vec3>
  FakeAnimationBlock opacity;       // 0x114 M2PartTrack<fixed16>
  FakeAnimationBlock sizes;         // 0x124 M2PartTrack<C2Vector>
  float scaleVary[2];               // 0x134 (was int32_t d[2])
  FakeAnimationBlock headCellTrack; // 0x13c flipbook cell over life (was "Intensity")
  FakeAnimationBlock tailCellTrack; // 0x14c (was unk2)
  float tailLength;                 // 0x15c (was unk[0])
  float twinkleSpeed;               // 0x160 (was unk[1])
  float twinklePercent;             // 0x164 (was unk[2])
  float twinkleScaleMin;            // 0x168 (was scales[0])
  float twinkleScaleMax;            // 0x16c (was scales[1])
  float burstMultiplier;            // 0x170 (was scales[2])
  float drag;                       // 0x174 (was slowdown)
  float baseSpin;                   // 0x178 (was unknown1[0])
  float baseSpinVary;               // 0x17c (was unknown1[1])
  float spin;                       // 0x180 sprite spin, rad/s (was rotation)
  float spinVary;                   // 0x184 (was unknown2[0])
  glm::vec3 tumbleMin;              // 0x188 (straddled unknown2[1] + Rot1)
  glm::vec3 tumbleMax;              // 0x194 (straddled Rot1 + Rot2)
  glm::vec3 windVector;             // 0x1a0 (straddled Rot2 + Trans)
  float windTime;                   // 0x1ac (was Trans[2])
  float followSpeed1;               // 0x1b0 (was f2[0])
  float followScale1;               // 0x1b4
  float followSpeed2;               // 0x1b8
  float followScale2;               // 0x1bc
  uint32_t nSplinePoints;           // 0x1c0 (was nUnknownReference)
  uint32_t ofsSplinePoints;         // 0x1c4
};

#define  MODELPARTICLE_DONOTTRAIL      0x10
#define  MODELPARTICLE_DONOTBILLBOARD  0x1000
struct ModelParticleEmitterDef {
  int32_t id;
  int32_t flags;
  glm::vec3 pos; // The position. Relative to the following bone.
  int16_t bone; // The bone its attached to.
  int16_t texture; // And the texture that is used.
  int32_t nModelFileName;
  int32_t ofsModelFileName;
  int32_t nParticleFileName;
  int32_t ofsParticleFileName;
  int8_t blend;
  int8_t EmitterType;
  int16_t ParticleColor;
  int8_t ParticleType;
  int8_t HeadorTail;
  int16_t TextureTileRotation;
  int16_t cols;
  int16_t rows;
  AnimationBlock EmissionSpeed; // All of the following blocks should be floats.
  AnimationBlock SpeedVariation; // Variation in the flying-speed. (range: 0 to 1)
  AnimationBlock VerticalRange; // Drifting away vertically. (range: 0 to pi)
  AnimationBlock HorizontalRange; // They can do it horizontally too! (range: 0 to 2*pi)
  AnimationBlock Gravity; // Fall down, apple!
  AnimationBlock Lifespan; // Everyone has to die.
  int32_t unknown;
  AnimationBlock EmissionRate; // Stread your particles, emitter.
  int32_t unknown2;
  AnimationBlock EmissionAreaLength; // Well, you can do that in this area.
  AnimationBlock EmissionAreaWidth;
  AnimationBlock Gravity2; // A second gravity? Its strong.
  ModelParticleParams p;
  AnimationBlock en;
};

// M2ParticleOld is 476 bytes in v264 and the params block 196; the renaming above is a pure relabel of
// the same bytes, so pin both down. If either fires, every field read below is reading the wrong offset.
static_assert(sizeof(ModelParticleParams) == 196, "WotLK particle params must stay 196 bytes");
static_assert(sizeof(ModelParticleEmitterDef) == 476, "WotLK particle emitter must stay 476 bytes");
static_assert(offsetof(ModelParticleEmitterDef, p.headCellTrack) == 0x13c, "head cell track offset");
static_assert(offsetof(ModelParticleEmitterDef, p.spin) == 0x180, "spin offset");

struct ClassicParticleColor
{
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
  std::uint8_t alpha;
};

struct ClassicModelParticleParams
{
  float midPoint;
  ClassicParticleColor colorValues[3];
  float scalesValues[3];
  std::uint16_t lifespanUVAnim[3];
  std::uint16_t decayUVAnim[3];
  std::uint16_t tailUVAnim[2];
  std::uint16_t tailDecayUVAnim[2];
  float tailLength;
  float twinkleSpeed;
  float twinklePercent;
  float twinkleScaleMin;
  float twinkleScaleMax;
  float burstMultiplier;
  float drag;
  float spin;
  glm::vec3 tumbleMin;
  glm::vec3 tumbleMax;
  glm::vec3 windVector;
  float windTime;
  float followSpeed1;
  float followScale1;
  float followSpeed2;
  float followScale2;
  std::uint32_t nSplinePoints;
  std::uint32_t ofsSplinePoints;
};

struct ClassicModelParticleEmitterDef {
  int32_t id;
  int32_t flags;
  glm::vec3 pos;
  int16_t bone;
  int16_t texture;
  int32_t nModelFileName;
  int32_t ofsModelFileName;
  int32_t nParticleFileName;
  int32_t ofsParticleFileName;
  uint16_t blend;
  uint16_t EmitterType;
  uint8_t ParticleType;
  uint8_t HeadorTail;
  int16_t TextureTileRotation;
  int16_t cols;
  int16_t rows;
  ClassicAnimationBlock EmissionSpeed;
  ClassicAnimationBlock SpeedVariation;
  ClassicAnimationBlock VerticalRange;
  ClassicAnimationBlock HorizontalRange;
  ClassicAnimationBlock Gravity;
  ClassicAnimationBlock Lifespan;
  ClassicAnimationBlock EmissionRate;
  ClassicAnimationBlock EmissionAreaLength;
  ClassicAnimationBlock EmissionAreaWidth;
  ClassicAnimationBlock Gravity2;
  ClassicModelParticleParams p;
  ClassicAnimationBlock en;
};


struct ModelRibbonEmitterDef {
  int32_t id;
  int32_t bone;
  glm::vec3 pos;
  int32_t nTextures;
  int32_t ofsTextures;
  int32_t nMaterials;
  int32_t ofsMaterials;
  AnimationBlock color;
  AnimationBlock opacity;
  AnimationBlock above;
  AnimationBlock below;
  float res, length, Emissionangle;
  int16_t s1, s2;
  AnimationBlock unk1;
  AnimationBlock unk2;
  int32_t unknown;
};

// Classic (1.12, M2 version 256) ribbon emitter. Same field order as the WotLK def, but the animated
// tracks use the 28-byte ClassicAnimationBlock instead of the 20-byte WotLK AnimationBlock, and there
// is NO trailing 'unknown' int32. Total = 36 (head) + 4*28 (color/opacity/above/below) + 12 (res,
// length, angle) + 4 (s1,s2) + 2*28 (unk1,unk2) = 220 bytes (verified against phoenix/kaelthas/mounts).
struct ClassicModelRibbonEmitterDef {
  int32_t id;
  int32_t bone;
  glm::vec3 pos;
  int32_t nTextures;
  int32_t ofsTextures;
  int32_t nMaterials;
  int32_t ofsMaterials;
  ClassicAnimationBlock color;
  ClassicAnimationBlock opacity;
  ClassicAnimationBlock above;
  ClassicAnimationBlock below;
  float res, length, Emissionangle;
  int16_t s1, s2;
  ClassicAnimationBlock unk1;
  ClassicAnimationBlock unk2;
};
static_assert(sizeof(ClassicModelRibbonEmitterDef) == 220,
              "Classic ribbon emitter def must be 220 bytes (verified stride); padding would misparse it");


// Classic (v256) M2 event def: header block + a classic TRACK BASE (interp/gseq + per-sequence
// ranges + global times, NO keys). The client fires these from the anim tick (doc 38:
// CGUnit_C::HandleAnimEvent FUN_005ffbd0 -- $FSD footsteps, $FD1-4 fidgets, $CSD custom, ...).
struct ClassicModelEventDef {
  char id[4];        // FourCC ("$FSD", "$FD1", ...)
  int32_t data;      // event payload ($CSD: the SoundEntries id)
  uint32_t bone;
  glm::vec3 pos;
  int16_t type;      // track interpolation (unused for events)
  int16_t seq;       // global sequence or -1
  uint32_t nRanges;  // per-SEQUENCE ranges into the times array
  uint32_t ofsRanges;
  uint32_t nTimes;
  uint32_t ofsTimes;
};
static_assert(sizeof(ClassicModelEventDef) == 44,
              "Classic event def must be 44 bytes (id+data+bone+pos + 20-byte classic track base)");

struct ModelEvents {
  char id[4];
  int32_t data;
  int32_t bone;
  glm::vec3 pos;
  int16_t type;
  int16_t seq;
  uint32_t nTimes;
  uint32_t ofsTimes;
};

struct ModelAttachmentDef {
  uint32_t id;
  uint16_t bone;
  uint16_t unknown1;
  glm::vec3 pos;
  AnimationBlock Enabled;
};

struct ClassicModelBoneDef {
  int32_t KeyBoneID;
  uint32_t flags;
  int16_t parent; // parent bone index
  uint16_t submesh_id;
  ClassicAnimationBlock translation;
  ClassicAnimationBlock rotation;
  ClassicAnimationBlock scaling;
  glm::vec3 pivot;
};

struct ModelBoneDef {
  int32_t KeyBoneID;
  uint32_t flags;
  int16_t parent; // parent bone index
  uint16_t unk[3];
  AnimationBlock translation;
  AnimationBlock rotation;
  AnimationBlock scaling;
  glm::vec3 pivot;
};

struct ModelBoundTriangle {
  uint16_t index[3];
};

#pragma pack(pop)
