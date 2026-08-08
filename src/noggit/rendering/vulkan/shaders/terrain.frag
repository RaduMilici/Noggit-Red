// [VULKAN VK-1b] terrain shading without textures yet: flat-shaded from screen-space derivatives (real
// surface normals) + a height tint. Enough to visually verify the GEOMETRY + camera track perfectly;
// the texture/alphamap descriptor work replaces this in the next step.
#version 450

layout(location = 0) in vec3 v_world;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Push
{
  mat4 mvp;
  float time;
} pc;

void main()
{
  // face normal from derivatives (world-space)
  vec3 n = normalize(cross(dFdx(v_world), dFdy(v_world)));
  float ndl = clamp(dot(n, normalize(vec3(0.4, 1.0, 0.3))), 0.0, 1.0);

  // height tint: low = green-brown, high = grey-white
  float h = clamp((v_world.y + 100.0) / 500.0, 0.0, 1.0);
  vec3 base = mix(vec3(0.28, 0.38, 0.18), vec3(0.75, 0.73, 0.70), h);

  out_color = vec4(base * (0.35 + 0.65 * ndl), 1.0);
}
