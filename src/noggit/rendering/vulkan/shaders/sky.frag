// [VULKAN overnight stage 2] sky backdrop: vertical zenith->horizon gradient drawn as a fullscreen triangle
// BEHIND the terrain (depth test/write off, drawn first). Colours are a plausible WoW daytime set for now;
// wiring the zone's live _skies->color_set through push constants comes with the later lighting pass.
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(location = 1) out float out_z; // [phase A] depth-as-colour (R32F attachment GL imports)

layout(push_constant) uniform Push
{
  float time;
} pc;

void main()
{
  // v_uv.y: 0 at one edge, ~2 at the other (fullscreen-triangle overshoot); the image is y-down and the GL
  // blit flips rows, so SMALL v_uv.y lands at the TOP of the window.
  float t = clamp(v_uv.y, 0.0, 1.0);
  vec3 zenith  = vec3(0.18, 0.42, 0.78);
  vec3 horizon = vec3(0.68, 0.82, 0.94);
  vec3 col = mix(zenith, horizon, smoothstep(0.25, 0.95, t));
  out_color = vec4(col, 1.0);
  out_z = 1.0;
}
