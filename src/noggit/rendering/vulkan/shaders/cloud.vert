// [VULKAN phase F] CLOUD DECK -- the client's cloud cap (pole + 11 rings x 17 columns, flattened by
// cos 45deg so its edge sits at eye level). Port of the inline cloud program in Sky.cpp: the mesh is
// authored around the origin and translated onto the camera, and each vertex carries the per-row
// alpha that fades the deck out at the horizon.
//
// Layout (stride 24): pos xyz@0 | uv@12 | row alpha@20
#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in float in_alpha;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out float v_alpha;

layout(push_constant) uniform Push
{
  mat4 mvp;
  vec4 camera_opacity;   // xyz = camera position, w = cloud opacity
  vec4 tex;              // x = bindless index of the ticked cloud texture
} pc;

void main()
{
  v_uv = in_uv;
  v_alpha = in_alpha;
  vec4 clip = pc.mvp * vec4(in_pos + pc.camera_opacity.xyz, 1.0);
  clip.z = (clip.z + clip.w) * 0.5;   // GL clip -> VK clip
  gl_Position = clip;
}
