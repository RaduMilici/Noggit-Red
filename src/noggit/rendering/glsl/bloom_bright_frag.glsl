// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Bright pass feeding the bloom blur. Two additive contributions:
//   (1) authored EMISSIVE surfaces (alpha >0.88 reserved range) -- lava, unlit WMO, additive M2 glows,
//   (2) the CANON client FFXGlow: the full scene scaled by the per-zone `glow` strength (no threshold).
// The blur + composite then turn this into the soft glow halo.
in vec2 uv;
out vec4 out_color;

uniform sampler2D scene;
uniform float threshold;
// FFXGlow strength for this frame = per-zone glow (LightParams.glow, 0..1), floored at the client's
// outdoor minimum 84/255=0.329 when outdoors, raw (~0) inside an enclosed WMO. Drives the CANON
// full-screen additive glow: the client downsamples+blurs the WHOLE scene and adds it back scaled by
// this, with NO luminance threshold -- bright pixels bloom purely because additive blending saturates
// them. (RE'd from wow.exe FFXEffects.cpp, see RE_notes/13_ffx_fullscreen_glow.md.)
uniform float glow;

void main()
{
  // MEASURED (wow_cap_upstairs.trace, RE_notes/16): the client has NO bright pass and NO emissive
  // mask -- blur input = plain downsampled scene, composite = scene + zoneGlow * blur^2.
  // DELIBERATE DEVIATION (user, Anomalus A/B 2026-07-08): energy-creature UNLIT passes still stamp
  // the reserved emissive alpha (>0.88); boost those texels into the blur input so bright energy
  // bodies bloom hot like the live client's saturated core. Ordinary lit surfaces (a <= 0.85) are
  // untouched -- for them this remains the measured passthrough.
  vec4 s = texture(scene, uv);
  float emissive = smoothstep(0.88, 0.97, clamp(s.a, 0.0, 1.0));
  out_color = vec4(s.rgb * (1.0 + 0.25 * emissive), 1.0); // 0.25: dialed by A/B (2x too hot, 1.5x still too much)
}

void main_old_masked()
{
  vec4 s = texture(scene, uv);
  vec3 c = s.rgb;
  float a = clamp(s.a, 0.0, 1.0);

  // The scene FBO alpha is a bloom mask with a RESERVED EMISSIVE range (>0.88). ONLY surfaces that are
  // emissive by their authored flags write into it:
  //   - lava liquid          (LiquidType == magma, liquid_frag)
  //   - WMO unlit material    (self-illum / F_UNLIT flag, wmo_frag)
  //   - additive M2 glow mesh (blendingMode additive)
  // Everything else -- lit WMO walls, lit M2 doodads, terrain, sky, and particles -- writes <= 0.85 (or
  // 0) and therefore NEVER blooms, no matter how bright the zone lighting makes it.
  //
  // Why this matters (the Molten-Core-fine / Ironforge-overbearing puzzle): the old pass ALSO had a
  // LUMINANCE gate that bloomed any pixel brighter than `threshold`. That is a heuristic, not canon, and
  // it is zone-dependent -- the dark Molten Core cave has almost no bright lit surface so it barely fired,
  // while the bright Ironforge Great Forge is full of warm-lit metal/stone that all crossed the gate and
  // washed the room (and bled through the 38%-alpha forge smoke, which only SHOWS that bright background).
  // The smoke renders byte-identical in both zones (particles are unlit); the ONLY difference was this
  // brightness heuristic. Tying bloom to the emissive flag removes the wash without touching the lava glow.
  float maxc = max(c.r, max(c.g, c.b));
  vec3 bloom = vec3(0.0);

  // (1) EMISSIVE-flag bloom (authored): alpha in the reserved >0.88 range -- lava liquid, WMO unlit
  // materials, additive M2 glows. Gated by MAX CHANNEL so saturated orange lava/fire passes while dark
  // lava cracks don't (threshold*0.55 = the original emissive knee; Molten Core unchanged).
  float emissive = smoothstep(0.88, 0.97, a);
  if (emissive > 0.0)
  {
    float thr  = threshold * 0.55;
    float knee = clamp((maxc - thr) / max(1.0 - thr, 0.0001), 0.0, 1.0);
    bloom += c * knee * emissive;
  }

  // (2) CANON FFXGlow: full-screen additive glow, exactly as the 1.12 client does it. NO threshold --
  // the whole scene colour is scaled by the per-zone glow strength and added back (after the blur +
  // composite). final = scene + blur(scene)*glow, since blur is linear (bright=scene*glow, composite
  // add=1). Bright/near-white pixels bloom because the additive push saturates them past 1.0 while dark
  // pixels gain glow*dark ~= 0. `glow` = max(zone LightParams.glow, 0.329 client floor), so bright
  // near-white surfaces (sky-ceiling domes, snow, forge fire) bloom while dark interiors stay dark.
  bloom += c * glow;

  if (bloom == vec3(0.0))
  {
    out_color = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }

  out_color = vec4(bloom, 1.0);
}
