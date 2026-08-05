// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

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

mat4 get_bone_matrix(uint bone_index)
{
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
}

void main()
{
  mat4 boneTransformMat = mat4(0);

  if (anim_bones)
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
  vec3 anchor_rel = mat3(transform) * local_pos + transform[3].xyz;               // vertex relative to model_origin (small)
  vec3 world_rel = anchor_rel + (model_origin - camera_pos.xyz);                  // + Sterbenz-exact origin-minus-camera
  vec4 vertex = view_rot * vec4(world_rel, 1.0);
  m2_world_pos = anchor_rel + model_origin;                                       // full world (for point lights)

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
  gl_Position = projection * vertex;
}
