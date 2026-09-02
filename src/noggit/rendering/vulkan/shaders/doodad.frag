// [VULKAN overnight stage 4] clay doodads until textures land: lambert with a normal-based tint heuristic
// (up-facing surfaces lean green like foliage, sides lean bark-brown) so trees read as trees.
#version 450

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 0) out vec4 out_color;
layout(location = 1) out float out_z; // [phase A] depth-as-colour (R32F attachment GL imports)

layout(push_constant) uniform Push
{
  mat4 mvp;
  float time;
} pc;

void main()
{
  vec3 n = normalize(v_normal);
  float ndl = clamp(dot(n, normalize(vec3(0.4, 1.0, 0.3))), 0.0, 1.0);
  float up = clamp(n.y, 0.0, 1.0);
  vec3 base = mix(vec3(0.32, 0.24, 0.16), vec3(0.22, 0.40, 0.16), up * up);
  out_color = vec4(base * (0.40 + 0.60 * ndl), 1.0);
  out_z = gl_FragCoord.z;
}
