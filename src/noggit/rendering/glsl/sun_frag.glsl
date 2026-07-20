// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// The sky sun: a bright CORE disc + a soft round HALO + a radial STARBURST corona (the rays you see
// in-game radiating from the sun). All additive, so it saturates to white and the existing FFXGlow
// (box+gauss+glow, the client's real post chain) blooms it further. The trace proves the client has
// NO separate god-ray pass -- the rays live in the bright sun sprite + haze, then bloom. This bakes
// that corona into the sprite. Alpha carries the emissive bloom mask so FFXGlow scatters it hard.
//
// The quad is large (it holds the rays); the solid disc is only the inner `core_size` of it.

in vec2 uv;
uniform vec3 sun_color;   // sun band colour (near-white); core saturates
uniform float intensity;  // core brightness multiplier
uniform float halo_gain;  // strength of the round surrounding glow
uniform float core_size;  // radius (0..1 of the quad) of the solid disc
uniform float ray_gain;   // strength of the radial starburst rays
uniform float ray_reach;  // pow exponent for how far the rays extend (smaller = longer)

out vec4 out_color;

void main()
{
  float d = length(uv);
  if (d > 1.0)
  {
    discard;
  }

  // Solid bright disc with a feathered edge.
  float core = 1.0 - smoothstep(core_size, core_size + 0.05, d);

  // Round soft glow around the disc.
  float halo = pow(clamp(1.0 - d, 0.0, 1.0), 2.5);

  // Radial rays: the client sun has only ~5-6 rays (emergent from the FFXGlow streaking the bright
  // disc; there is no ray texture). Match with 6 clean rays, not the earlier busy 20-ray starburst.
  float theta = atan(uv.y, uv.x);
  float rays = pow(clamp(0.5 + 0.5 * cos(theta * 6.0), 0.0, 1.0), 3.5); // 6 rays
  // rays start just outside the core and fade out toward the quad edge
  float ray_body = smoothstep(core_size * 0.5, core_size, d) * pow(clamp(1.0 - d, 0.0, 1.0), ray_reach);
  rays *= ray_body;

  float lum = core + halo * halo_gain + rays * ray_gain;
  vec3 c = sun_color * intensity * lum;

  // alpha = emissive bloom mask; the core lands in the >0.88 reserved range so FFXGlow halos it.
  float a = clamp(lum, 0.0, 1.0);
  out_color = vec4(c, a);
}
