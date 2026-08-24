// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core
#ifdef batched
// [animated MDI 2026-08-07] the batched variant reads bones from an SSBO; SSBOs need GLSL 430, so enable the
// ARB extension under 330 (the context is 4.3, so it's supported). Only the batched program pays this.
#extension GL_ARB_shader_storage_buffer_object : require
#endif

// CORRECTNESS (2026-07-22): PIN attribute locations. This shader is compiled TWICE -- as _m2_program
// (transform = uniform) and _m2_instanced_program (transform = per-instance attribute) -- and BOTH share the
// same per-model VAO. Without pinned locations the linker may assign pos/normal/etc. to DIFFERENT locations
// per program; then after the individual path (creature/gameobject/minimap) binds the shared VAO at
// _m2_program's locations, the instanced draw (re-asserts only `transform`) reads `normal` from the wrong
// location -> normalize(garbage) -> interior diffuse collapses to 0 -> the doodad renders BLACK yet correctly
// positioned (the Timbermaw interior-doodad black). Pinning makes both programs agree on the shared VAO.
layout(location = 0) in vec4 pos;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texcoord1;
layout(location = 3) in vec2 texcoord2;
layout(location = 4) in uvec4 bones_weight;
layout(location = 5) in uvec4 bones_indices;

#ifdef instanced
  layout(location = 6) in mat4 transform;   // model->world (a mat4 attribute spans locations 6..9)
  layout(location = 10) in vec4 interior;  // PER-INSTANCE interior room light (perf 2026-08-05); name matches attrib("interior")
#else
  uniform mat4 transform;
#endif

#ifdef batched
  // [perf 2026-08-05] MDI cross-model batch: the texture-array LAYER differs per model within a single
  // glMultiDrawElementsIndirect call, so it rides the divisor-1 per-instance stream (selected by each draw
  // command's baseInstance -- ARB_base_instance, no gl_DrawID/4.6 needed) instead of a per-draw uniform.
  // z/w reserved to move pixel_shader/blend_mode per-instance later (widens batches); unused for now but the
  // whole ivec4 stays live because .xy is read. The batched program is compiled with `instanced` ALSO defined.
  layout(location = 11) in ivec4 inst_tex;   // x = tex1 layer, y = tex2 layer, z = pixel_shader (reserved), w = blend_mode (reserved)
  flat out ivec4 v_inst_tex;
  // [size-class draw distance 2026-08-17] per-instance size class 0-4 (client FUN_007bdb10). The batched
  // MDI path draws EVERY loaded doodad with ONE flat slice_dist; this lets the fragment shader cull each
  // instance at ITS class's distance instead (small clutter close, trees far -- matching the client).
  layout(location = 12) in float inst_cull_class;
  flat out float v_cull_class;
#endif

// Interior light for this draw: rgb = WMO room ambient; a in [0.5,1.0] indoors (carries GAP B doorway
// spill = baked MOCV floor alpha), 0 outdoors. [perf 2026-08-05] The INSTANCED path now carries this
// PER INSTANCE in interior_attr (location 10, divisor 1) so instances with different room colours batch
// in ONE draw -- it used to partition instances by interior value into per-value sub-draws (WMO doodads
// in a city split ~6 ways = the dominant SubmitInst cost). The NON-instanced (individual creature/GO)
// path still feeds it as a uniform.
uniform vec4 instance_interior;

uniform samplerBuffer bone_matrices;

// World anchor for camera-relative rendering. `transform` carries the vertex position RELATIVE to this
// anchor (small), so the large ~17000 world coordinate lives only in model_origin and never gets baked
// into an animated float matrix (which would quantize the motion = dithering attachments). 0 for the
// instanced path, where transform already holds each instance's full world position.
uniform vec3 model_origin;

out vec2 uv1;
out vec2 uv2;
out float camera_dist;
out vec3 norm;
out vec3 m2_world_pos;
flat out vec4 v_interior;        // rgb = interior room ambient; a in [0.5,1.0] indoors (carries GAP B doorway spill), 0 outdoors

// CLIENT ground-clutter distance fade (wow.exe 3.3.5a DetailDoodad.bls vs_2_0 + FUN_007b15d0, RE
// 2026-08-20): the client VS computes per-vertex alpha = clamp(viewZ*c9.x + c9.y, 0, 1) with
// c9 = (-1/(0.15*groundEffectDist), 1/0.15) -- a linear view-DEPTH ramp from 85% to 100% of the
// clutter distance (fade start fraction 0.85 @ .rdata 0x9f23d0, scale -1.0 @ 0x9e2ef4). There is
// NO chunk-level fade logic in the client at all. detail_dist = groundEffectDist for the clutter
// draw; <= 0 (every other draw / previews) = no fade. Only the detail_doodad frag path reads this.
uniform float detail_dist;
out float v_detail_fade;

layout (std140) uniform matrices
{
  mat4 model_view;
  mat4 projection;
  vec4 camera_pos;
};

uniform int tex_unit_lookup_1;
uniform int tex_unit_lookup_2;

uniform mat4 tex_matrix_1;
uniform mat4 tex_matrix_2;

uniform bool anim_bones;
uniform int bone_matrix_count;

// Per-instance bone slices (perf 2026-07-20). 0 = one shared bone set for the whole draw (default: the
// single-instance path and the normal shared-pose instanced path). >0 = each instance owns its own
// bone_matrix_count matrices laid out contiguously in bone_matrices, so gl_InstanceID k reads its slice
// at k*per_instance_bone_stride. Lets billboard doodads (per-instance CPU-baked bones) draw INSTANCED
// (one drawElementsInstanced per model) instead of one draw per doodad.
uniform int per_instance_bone_stride;

// code from https://wowdev.wiki/M2/.skin#Environment_mapping
vec2 sphere_map(vec3 vert, vec3 norm)
{
  vec3 normPos = -(normalize(vert));
  vec3 temp = (normPos - (norm * (2.0 * dot(normPos, norm))));
  temp = vec3(temp.x, temp.y, temp.z + 1.0);
 
  return ((normalize(temp).xy * 0.5) + vec2(0.5));
}

vec2 get_texture_uv(int tex_unit_lookup, vec3 vert, vec3 norm, mat4 tex_matrix)
{
  if(tex_unit_lookup == 0)
  {
    return sphere_map(vert, norm);
  }
  else if(tex_unit_lookup == 1)
  {
    return (tex_matrix * vec4(texcoord1, 0.0, 1.0)).xy;
  }
  else if(tex_unit_lookup == 2)
  {
    return (tex_matrix * vec4(texcoord2, 0.0, 1.0)).xy;
  }
  else
  {
    return vec2(0.0);
  }
}

#ifdef batched
// [animated MDI 2026-08-07] Batched (cross-model MDI) bones live in ONE shared SSBO -- every batched model's
// bone_matrices concatenated. A single glMultiDrawElementsIndirect draws many DIFFERENT models, so bones can't
// ride the per-model bone_matrices texture; instead each instance carries, in its inst_tex stream, the base
// offset of its model's bone block (inst_tex.z, in mat4 units) and that model's bone count (inst_tex.w). w==0
// means a static/bind-pose model. All instances of one model share one block (unison sway, as the per-model
// animate() already produces), so z/w are constant across a command.
layout(std430, binding = 0) readonly buffer BatchedBones { mat4 b_batched_bones[]; };
#endif

mat4 get_bone_matrix(uint bone_index)
{
#ifdef batched
  // Batched path: read from the shared SSBO at this model's block base. w<=0 (static) or out-of-range -> bind pose.
  if (inst_tex.w <= 0 || bone_index >= uint(inst_tex.w))
  {
    return mat4(1.0);
  }
  return b_batched_bones[uint(inst_tex.z) + bone_index];
#else
  if (bone_matrix_count <= 0 || bone_index >= uint(bone_matrix_count))
  {
    return mat4(1.0);
  }

  mat4 matrix;
  // gl_InstanceID is 0 for non-instanced draws; per_instance_bone_stride is 0 for every shared-bone path;
  // so this offset is a no-op everywhere except the instanced billboard-doodad path.
  int bone_slot = int(bone_index) + per_instance_bone_stride * gl_InstanceID;
  int pixel_start = bone_slot * 4;
  matrix[0] = texelFetch(bone_matrices, pixel_start).rgba;
  matrix[1] = texelFetch(bone_matrices, pixel_start + 1).rgba;
  matrix[2] = texelFetch(bone_matrices, pixel_start + 2).rgba;
  matrix[3] = texelFetch(bone_matrices, pixel_start + 3).rgba;

  return matrix;
#endif
}

void main()
{
  mat4 boneTransformMat = mat4(0);

#ifdef batched
  bool do_bones = (inst_tex.w > 0); // batched: per-instance bone count drives deform (SSBO); 0 = static bind pose
#else
  bool do_bones = anim_bones;
#endif
  if (do_bones)
  {
    float total_weight = float(bones_weight.x + bones_weight.y + bones_weight.z + bones_weight.w);
    if (total_weight > 0.0)
    {
      boneTransformMat += (float(bones_weight.x) / total_weight) * get_bone_matrix(bones_indices.x);
      boneTransformMat += (float(bones_weight.y) / total_weight) * get_bone_matrix(bones_indices.y);
      boneTransformMat += (float(bones_weight.z) / total_weight) * get_bone_matrix(bones_indices.z);
      boneTransformMat += (float(bones_weight.w) / total_weight) * get_bone_matrix(bones_indices.w);
    }
    else
    {
      boneTransformMat = mat4(1);
    }
  }
  else
  {
    boneTransformMat = mat4(1);
  }

  mat4 modelMatrix = transform * boneTransformMat;
  mat3 normMatrix = mat3(modelMatrix);

  // PS1-style vertex jitter fix -- CAMERA-RELATIVE, keeping the ANIMATED part at small magnitude.
  // Two precision traps at world scale (~17000 on a real map):
  //   1) multiplying model_view*worldpos in float cancels catastrophically (camera slide);
  //   2) building worldpos = transform*bone*pos at ~17000 QUANTIZES the small per-vertex animation to
  //      the ~0.002 float grid there -> the animation dithers.
  // Fix both: never form the full ~17000 world position for the clip path. Transform the animated
  // LOCAL vertex by the model's rotation/scale ONLY (small), and add the model-origin-minus-camera
  // offset (Sterbenz-exact, small). Everything the clip position touches stays small = precise. Then
  // apply the rotation-only view (its translation is irrelevant).
  mat4 view_rot = model_view;
  view_rot[3].xyz = vec3(0.0);
  mat3 cameraNormMatrix = mat3(view_rot) * normMatrix;

  vec3 local_pos = (boneTransformMat * pos).xyz;                                  // animated, model-local (small)
  vec3 rot_pos = mat3(transform) * local_pos;                                     // animated, rotated/scaled (small)
  // [2026-08-20 instanced-path dither fix] The INSTANCED paths pass transform[3] = FULL world position
  // (model_origin = 0), and the old order (rot_pos + transform[3], THEN - camera) formed the ~9000-unit
  // world position in float FIRST -- quantizing the small animated component onto the ~1mm grid: every
  // batched model shimmered/"dithered", worse the more it animates (the release exe predates instancing
  // and never showed it). Group the BIG terms first: (transform[3] + (model_origin - camera)) is
  // Sterbenz-exact near the camera for both paths (individual: small + exact-small; instanced:
  // big - big cancels), and the small animated part is added LAST at full precision.
  vec3 inst_rel = transform[3].xyz + (model_origin - camera_pos.xyz);             // instance-minus-camera (small, exact)
  vec3 world_rel = rot_pos + inst_rel;
  vec4 vertex = view_rot * vec4(world_rel, 1.0);
  m2_world_pos = rot_pos + transform[3].xyz + model_origin;                       // full world (for point lights)

  // Client clutter fade (see detail_dist above). View-space forward is -z in GL; the client's viewZ
  // is the equivalent positive depth. fade = (dist - viewZ) / (0.15 * dist), clamped -- 1.0 inside
  // 85% of the clutter distance, 0.0 at it.
  v_detail_fade = (detail_dist > 0.0)
    ? clamp((detail_dist - (-vertex.z)) / (0.15 * detail_dist), 0.0, 1.0)
    : 1.0;

  // important to normalize because of the scaling !!
  norm = normalize(normMatrix * normal);
  vec3 camera_norm = normalize(cameraNormMatrix * normal);

  uv1 = get_texture_uv(tex_unit_lookup_1, vertex.xyz, camera_norm, tex_matrix_1);
  uv2 = get_texture_uv(tex_unit_lookup_2, vertex.xyz, camera_norm, tex_matrix_2);

  // Fog distance = TRUE Euclidean distance to the camera (view space is a rigid transform, so
  // length(view_pos) == world distance). Every other shader -- terrain, WMO, liquid, wmo_liquid,
  // particle -- fogs by distance(camera, world_pos); M2 alone used -vertex.z (view DEPTH), which is
  // shorter off-axis, so doodads/creatures fogged by a different amount than the ground and "stuck
  // out" of the fog. Matching Euclidean puts M2 objects on the identical fog ramp as the terrain.
  camera_dist = length(vertex.xyz);
#ifdef instanced
  v_interior = interior;             // per-instance room light (batches mixed-interior instances in one draw)
#else
  v_interior = instance_interior;    // individual path: uniform
#endif
#ifdef batched
  v_inst_tex = inst_tex;             // per-instance texture layers to the fragment shader
  v_cull_class = inst_cull_class;    // per-instance size class -> per-class draw distance in the frag shader
#endif
  gl_Position = projection * vertex;
}
