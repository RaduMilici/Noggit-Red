// [VULKAN phase G] M2 particle quads. GL keeps the emitter transform in a vertex attribute and adds
// the billboard offset in the shader; the VK feed bakes both on the CPU (VkParticleFeed), so the
// vertex here is already world space and carries only what the fragment stage needs.
//
// Layout (stride 36): pos xyz@0 | uv@12 | rgba@20
#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec3 v_world;

layout(push_constant) uniform Push
{
  mat4 mvp;
  vec4 tex_blend_alpha;   // x = bindless texture, y = blend mode, z = alpha test, w = alpha mod
} pc;

void main()
{
  v_uv = in_uv;
  v_color = in_color;
  v_world = in_pos;
  vec4 clip = pc.mvp * vec4(in_pos, 1.0);
  clip.z = (clip.z + clip.w) * 0.5;   // GL clip -> VK clip
  gl_Position = clip;
}
