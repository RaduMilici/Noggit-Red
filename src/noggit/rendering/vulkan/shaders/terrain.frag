// [VULKAN VK-1] terrain shading without textures yet: SMOOTH per-vertex normals (the same MapChunk normals
// GL shades with -- no more per-face PS1 faceting) + a height tint. The texture/alphamap descriptor work
// replaces the tint in the next stage.
#version 450

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec3 v_mccv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Push
{
  mat4 mvp;
  float time;
} pc;

// [overnight stage 6] ground texture via the first descriptor set (generated noise now; real BLP layers ride
// this exact binding next). Sampled by world XZ, tiled.
layout(set = 0, binding = 0) uniform sampler2D ground;

void main()
{
  vec3 n = normalize(v_normal);
  float ndl = clamp(dot(n, normalize(vec3(0.4, 1.0, 0.3))), 0.0, 1.0);

  vec3 tex = texture(ground, v_world.xz * 0.075).rgb;

  // height tint keeps distant relief readable; texture carries the near detail
  float h = clamp((v_world.y + 100.0) / 500.0, 0.0, 1.0);
  vec3 base = mix(tex, vec3(0.75, 0.73, 0.70), h * h * 0.6);

  out_color = vec4(base * (0.40 + 0.60 * ndl) * v_mccv, 1.0);
}
