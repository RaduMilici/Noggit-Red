// [VULKAN phase D, 2026-08-30] WMO group vertex stage.
//
// Geometry comes from an append-only arena keyed by (wmo file, group index), mirrored out of
// WMOGroupRender::upload() before it frees the CPU streams. Each draw replays one run that GL
// actually emitted, so the per-batch frustum culling is already applied upstream.
//
// Arena vertex (stride 56): pos@0 (3f), normal@12 (3f), uv0@24 (2f), uv1@32 (2f), colour@40 (4f)
#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv0;
layout(location = 3) in vec2 in_uv1;
layout(location = 4) in vec4 in_color;
layout(location = 5) in uint in_batch_id;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv0;
layout(location = 3) out vec2 v_uv1;
layout(location = 4) out vec4 v_color;
layout(location = 5) flat out uint v_batch_id;

layout(push_constant) uniform Push
{
  mat4 mvp;
  float time;
  float terms;
  float slice;
  int xform_index;   // into the per-instance transform SSBO
} pc;

// One entry per visible WMO instance this frame (WorldRender::vkWmoTransforms).
// Own descriptor set: set 1 binding 5 is the variable-count bindless array, and Vulkan requires
// that to carry the HIGHEST binding in its set, so the WMO transforms cannot be appended there.
layout(std430, set = 2, binding = 0) readonly buffer WmoTransforms { mat4 wmo_xforms[]; };

void main()
{
  // batch id 0 = vertex belongs to no batch. GL pushes it to NaN so the triangle is discarded;
  // do the same, or these vertices render with another batch's material.
  if (in_batch_id == 0u)
  {
    gl_Position = vec4(0.0 / 0.0);
    v_world = vec3(0.0); v_normal = vec3(0.0, 1.0, 0.0);
    v_uv0 = vec2(0.0); v_uv1 = vec2(0.0); v_color = vec4(1.0); v_batch_id = 0u;
    return;
  }

  mat4 model = wmo_xforms[pc.xform_index];
  vec4 world = model * vec4(in_pos, 1.0);

  v_world = world.xyz;
  v_normal = mat3(model) * in_normal;
  v_uv0 = in_uv0;
  v_uv1 = in_uv1;
  v_color = in_color;
  v_batch_id = in_batch_id - 1u;   // 1-based on the wire, 0-based into the batch table

  // GL clip -> VK clip (z in [0, w]); y stays (VK y-down + un-flipped GL blit cancel)
  vec4 clip = pc.mvp * world;
  clip.z = (clip.z + clip.w) * 0.5;
  gl_Position = clip;
}
