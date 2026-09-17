// [VULKAN phase C, 2026-08-29] M2 / doodad BATCHED vertex stage -- the Vulkan twin of GL's
// m2_vert.glsl "batched" path (WorldRender::drawDynamicBatched). Same arena vertex layout
// (ModelVertex, stride 48), same per-instance streams, same shared bone SSBO addressed by the
// instance's (bone_base, bone_count) -- so a batch built for GL feeds this unchanged.
//
// Vertex buffer 0 = ModelVertex: pos@0 (3f), weights@12 (4 u8), bones@16 (4 u8), normal@20 (3f),
//                                uv0@32 (2f), uv1@40 (2f)
// Vertex buffer 1..3 = per-instance transform (mat4), interior (vec4), tex/bone info (ivec4)
#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in uvec4 in_weights;   // R8G8B8A8_UINT
layout(location = 2) in uvec4 in_bones;     // R8G8B8A8_UINT
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec2 in_uv0;
layout(location = 5) in vec2 in_uv1;

layout(location = 6)  in mat4 inst_transform; // occupies 6,7,8,9
layout(location = 10) in vec4 inst_interior;
layout(location = 12) in ivec4 inst_state;   // blend, flags, pixel shader, tu lookups
layout(location = 11) in ivec4 inst_tex;      // x,y = bindless texture ids; z = bone base; w = bone count

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv0;
layout(location = 3) out vec2 v_uv1;
layout(location = 4) flat out ivec4 v_tex;
layout(location = 5) out vec4 v_interior;
layout(location = 6) flat out ivec4 v_state;

layout(push_constant) uniform Push
{
  mat4 mvp;
  float time;
  float terms;
} pc;

// shared bone block: GL uses an SSBO of mat4 addressed by inst_tex.z + bone_index (see m2_vert
// get_bone_matrix, batched path). Identical here.
layout(std430, set = 1, binding = 4) readonly buffer Bones { mat4 bones[]; };

// Only the tail of the shared lighting block is needed here (the camera basis), but a uniform block
// must declare every preceding member so the offsets line up.
layout(std140, set = 1, binding = 3) uniform Lighting
{
  vec4 DiffuseColor_FogStart;
  vec4 AmbientColor_FogEnd;
  vec4 FogColor_FogOn;
  vec4 LightDir_FogRate;
  vec4 OceanColorLight;
  vec4 OceanColorDark;
  vec4 RiverColorLight;
  vec4 RiverColorDark;
  vec4 PointLightParams;
  vec4 PointLightPos[16];
  vec4 PointLightColor[16];
  vec4 EnvFogColor_On;
  vec4 EnvFogDist;
  mat4 ShadowMatrix;
  vec4 ShadowParams;
  vec4 ShadowCenterRange;
  mat4 ShadowMatrixEnv;
  vec4 ShadowEnvCenterRange;
  vec4 Camera_Pad;
  vec4 SunSpec_Pad;
  vec4 Toggles;
  vec4 SheenDir_Pad;
  vec4 CamFwd_DetailDist;
  vec4 CamRight_Pad;
  vec4 CamUp_Pad;
};

// M2 environment mapping (m2_vert sphere_map), computed in VIEW space exactly like GL. The view
// basis is rebuilt from the camera axes above rather than shipping the whole view matrix.
vec2 sphere_map(vec3 vert_view, vec3 norm_view)
{
  vec3 normPos = -normalize(vert_view);
  vec3 temp = normPos - (norm_view * (2.0 * dot(normPos, norm_view)));
  temp = vec3(temp.x, temp.y, temp.z + 1.0);
  return (normalize(temp).xy * 0.5) + vec2(0.5);
}

mat4 get_bone_matrix(uint bone_index)
{
  int base = inst_tex.z;
  int count = inst_tex.w;
  // [CLUTTER PERSISTENT 2026-09-03] state.y bit 32: the instance data is BAKED ONCE (per-chunk
  // buffer) so it cannot carry this frame's bone base. tex.z holds a stable SPECIES SLOT instead;
  // slot k's header lives in bones[k] column 0 as (base, count), rewritten every frame -- grass
  // keeps its wind sway with zero per-instance updates.
  if ((inst_state.y & 32) != 0)
  {
    vec4 h = bones[inst_tex.z][0];
    base = int(h.x);
    count = int(h.y);
  }
  // count <= 0 (static) or out-of-range -> bind pose, exactly like GL
  if (count <= 0 || bone_index >= uint(count))
  {
    return mat4(1.0);
  }
  return bones[uint(base) + bone_index];
}

void main()
{
  mat4 boneTransformMat = mat4(1.0);
  float total_weight = float(in_weights.x + in_weights.y + in_weights.z + in_weights.w);
  if (total_weight > 0.0)
  {
    boneTransformMat = mat4(0.0);
    boneTransformMat += (float(in_weights.x) / total_weight) * get_bone_matrix(in_bones.x);
    boneTransformMat += (float(in_weights.y) / total_weight) * get_bone_matrix(in_bones.y);
    boneTransformMat += (float(in_weights.z) / total_weight) * get_bone_matrix(in_bones.z);
    boneTransformMat += (float(in_weights.w) / total_weight) * get_bone_matrix(in_bones.w);
  }

  mat4 modelMatrix = inst_transform * boneTransformMat;
  // [2026-09-01 DITHER FIX] Port of the GL fix in m2_vert.glsl (2026-08-20, "instanced-path dither").
  // VK formed the FULL world position (~17000 on a real map) and multiplied it by the mvp in float:
  //   1) mvp * worldpos at that magnitude cancels catastrophically -> vertices swim as the camera moves;
  //   2) building worldpos = transform * bone * pos at ~17000 quantizes the small per-vertex ANIMATION
  //      onto the ~0.002 float grid there -> animated models dither/shake in place.
  // Never form the big world position for the clip path. Keep the animated part model-local (small),
  // rotate/scale it (still small), and add the instance-minus-camera offset -- big minus big, exact
  // near the camera. pc.mvp is the CAMERA-RELATIVE matrix (mvp * translate(camera)) for this pass, so
  // everything the clip position touches stays small. The absolute world position is rebuilt after,
  // for fog/lighting, where precision does not matter.
  vec3 local_pos = (boneTransformMat * vec4(in_pos, 1.0)).xyz;   // animated, model-local (small)
  vec3 rot_pos = mat3(inst_transform) * local_pos;               // rotated/scaled (small)
  vec3 inst_rel = inst_transform[3].xyz - Camera_Pad.xyz;        // instance-minus-camera (exact)
  vec3 world_rel = rot_pos + inst_rel;                           // small, precise
  vec4 world = vec4(world_rel + Camera_Pad.xyz, 1.0);            // absolute, for lighting/fog only

  v_world = world.xyz;
  v_normal = mat3(modelMatrix) * in_normal;
  // tu_lookup 0 = SPHERE MAP: build it here, where the view basis is available. 1/2 select an
  // authored UV set and are resolved in the fragment stage.
  v_uv0 = in_uv0;
  v_uv1 = in_uv1;
  {
    int tu0 = inst_state.w & 0xFF;
    int tu1 = (inst_state.w >> 8) & 0xFF;
    if (tu0 == 0 || tu1 == 0)
    {
      vec3 wr = world_rel;   // == world - camera, but without re-forming the big value
      vec3 vert_view = vec3(dot(wr, CamRight_Pad.xyz), dot(wr, CamUp_Pad.xyz),
                            -dot(wr, CamFwd_DetailDist.xyz));
      vec3 n = normalize(v_normal);
      vec3 norm_view = normalize(vec3(dot(n, CamRight_Pad.xyz), dot(n, CamUp_Pad.xyz),
                                      -dot(n, CamFwd_DetailDist.xyz)));
      vec2 sm = sphere_map(vert_view, norm_view);
      if (tu0 == 0) v_uv0 = sm;
      if (tu1 == 0) v_uv1 = sm;
    }
  }
  v_tex = inst_tex;
  v_interior = inst_interior;
  v_state = inst_state;

  // GL clip -> VK clip (z in [0, w]); y stays (VK y-down + un-flipped GL blit cancel)
  vec4 clip = pc.mvp * vec4(world_rel, 1.0);   // pc.mvp is camera-relative for the M2 pass
  clip.z = (clip.z + clip.w) * 0.5;
  gl_Position = clip;
}
