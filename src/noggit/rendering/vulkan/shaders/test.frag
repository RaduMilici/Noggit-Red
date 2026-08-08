// [VULKAN VK-1 skeleton] animated pattern -- visibly a SHADER (not a clear): proves the whole SPIR-V
// pipeline (glslang compile -> module -> render pass -> graphics pipeline -> draw) end to end.
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Push
{
  float time;
} pc;

void main()
{
  vec2 p = v_uv * 2.0 - 1.0;
  float r = length(p);
  float a = atan(p.y, p.x);
  // swirling rings + hue sweep
  float rings = 0.5 + 0.5 * sin(10.0 * r - pc.time * 3.0 + 3.0 * a);
  vec3 col = 0.5 + 0.5 * cos(pc.time + vec3(0.0, 2.1, 4.2) + rings * 3.0);
  col *= smoothstep(1.05, 0.85, r) * 0.9 + 0.1; // soft vignette
  out_color = vec4(col, 1.0);
}
