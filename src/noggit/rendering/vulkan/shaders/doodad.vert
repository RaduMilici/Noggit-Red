// [VULKAN overnight stage 4] instanced M2 doodads: per-vertex pos+normal (extracted from ModelVertex),
// per-INSTANCE world matrix as 4 divisor-1 vec4 attributes (locations 2-5), camera MVP via push constants.
#version 450

layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 im0;
layout(location = 3) in vec4 im1;
layout(location = 4) in vec4 im2;
layout(location = 5) in vec4 im3;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;

layout(push_constant) uniform Push
{
  mat4 mvp;
  float time;
} pc;

void main()
{
  mat4 inst = mat4(im0, im1, im2, im3);
  vec4 world = inst * vec4(pos, 1.0);
  v_world = world.xyz;
  v_normal = mat3(inst) * normal;
  vec4 clip = pc.mvp * world;
  clip.z = (clip.z + clip.w) * 0.5; // GL->VK depth remap (Y handled by the blit, see terrain.vert)
  gl_Position = clip;
}
