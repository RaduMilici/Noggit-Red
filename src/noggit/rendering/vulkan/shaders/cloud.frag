// [VULKAN phase F] Cloud deck fragment -- byte-for-byte the GL cloud shader:
//   out = vec4(tex.rgb, tex.a * vertex_alpha * cloud_opacity)
// The framebuffer ALPHA is left alone by the blend state (the sky wrote 0 there as the bloom mask).
#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_alpha;

layout(location = 0) out vec4 out_color;
layout(location = 1) out float out_z;

layout(set = 1, binding = 5) uniform sampler2D tilesets[];   // shared bindless array

layout(push_constant) uniform Push
{
  mat4 mvp;
  vec4 camera_opacity;
  vec4 tex;
} pc;

void main()
{
  int id = int(pc.tex.x);
  vec4 t = id >= 0 ? texture(tilesets[nonuniformEXT(id)], v_uv) : vec4(0.0);
  out_color = vec4(t.rgb, t.a * v_alpha * pc.camera_opacity.w);
  out_z = 1.0;   // backdrop layer: never occludes what GL composes over it
}
