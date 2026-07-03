// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// EMISSIVE-ONLY bright pass. Bloom is driven by the CANON MATERIAL/EMISSIVE flags in the assets, never
// by raw screen brightness.
in vec2 uv;
out vec4 out_color;

uniform sampler2D scene;
uniform float threshold;

void main()
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
  float emissive = smoothstep(0.88, 0.97, a);
  if (emissive <= 0.0)
  {
    out_color = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }

  // Among emissive pixels, gate by MAX CHANNEL so a saturated orange lava/fire passes while dark lava
  // cracks (low brightness) don't glow. threshold*0.55 is the same emissive knee the pass used before,
  // so genuinely-emissive surfaces bloom exactly as they did (Molten Core is unchanged).
  float maxc = max(c.r, max(c.g, c.b));
  float thr  = threshold * 0.55;
  float knee = clamp((maxc - thr) / max(1.0 - thr, 0.0001), 0.0, 1.0);

  out_color = vec4(c * knee * emissive, 1.0);
}
