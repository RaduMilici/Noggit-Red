// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Bright-pass: keep only the pixels brighter than a threshold, with a soft knee, so the bloom blur
// only spreads the bright stuff (sky openings, additive glows, light shafts) and not the whole image.
in vec2 uv;
out vec4 out_color;

uniform sampler2D scene;
uniform float threshold;

void main()
{
  vec4 s = texture(scene, uv);
  vec3 c = s.rgb;
  // Bloom mask (GRADED, not binary): terrain/horizon write alpha 0 to opt out, and WMO/M2 write
  // alpha = 1-fogFactor so fog-brightened surfaces bloom proportionally LESS (fully fogged = none).
  // A binary step would let any partially-fogged pixel bloom at full strength (Stormwind white-out).
  float bloom_mask = clamp(s.a, 0.0, 1.0);
  // Perceptual LUMINANCE, not max-channel: a saturated red/orange (R=1) is bright in one channel but
  // is not "white" and should NOT bloom. Luminance keeps bloom on genuinely bright/white pixels only,
  // so e.g. a sky disc blooms at its white top and fades out over its red gradient instead of glowing
  // uniformly.
  float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
  float knee = clamp((lum - threshold) / max(1.0 - threshold, 0.0001), 0.0, 1.0);
  out_color = vec4(c * knee * bloom_mask, 1.0);
}
