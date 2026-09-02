// [VULKAN phase E] Liquid surface vertex stage. Water had been reusing terrain.vert, which only
// carries position/normal/colour; the liquid shader needs the per-vertex depth and UV plus the
// per-chunk type and animation that the feed now bakes in.
//
// Layout (stride 68): pos xyz@0 | normal xyz@12 | uv xy@24 | depth,type@32 | anim_u,anim_v@40
//                     tex id@48 | wmo flags + material colour@52
#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec2 in_depth_type;
layout(location = 4) in vec2 in_anim;
layout(location = 5) in float in_tex_id;
layout(location = 6) in vec4 in_wmo;   // x = flags (1 wmo, 2 material colour, 4 city channel), yzw = MOMT.diffColor

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec2 v_depth_type;   // depth interpolates; type is uniform per chunk anyway
layout(location = 4) flat out vec2 v_anim;
layout(location = 5) flat out int v_tex_id;
layout(location = 6) flat out vec4 v_wmo;

layout(push_constant) uniform Push
{
  mat4 mvp;
  float time;
} pc;

void main()
{
  v_world = in_pos;
  v_normal = in_normal;
  v_uv = in_uv;
  v_depth_type = in_depth_type;
  v_anim = in_anim;
  v_tex_id = int(in_tex_id);
  v_wmo = in_wmo;

  // GL clip -> VK clip (z in [0, w]); y stays (VK y-down + un-flipped GL blit cancel)
  vec4 clip = pc.mvp * vec4(in_pos, 1.0);
  clip.z = (clip.z + clip.w) * 0.5;
  gl_Position = clip;
}
