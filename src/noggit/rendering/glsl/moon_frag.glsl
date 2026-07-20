// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Textured celestial billboard (sun/moon disc + glare). Two modes:
//   additive == 1 : PREMULTIPLIED additive glow (glare / sun disc). Output = tex*color*opacity, blended
//                   with GL_ONE/GL_ONE so `opacity` is a REAL brightness multiplier (not clamped like a
//                   SRC_ALPHA factor). Matches the client, which builds sun brightness by stacking the
//                   additive glare many times. Uses tex.rgb*tex.a so it works whether the glow lives in
//                   the texture's RGB (sunGlare: grey glow, alpha=255) or its alpha (a masked glow).
//   additive == 0 : alpha-blended disc (moon.blp / moon02.blp carry the disc shape in alpha).
in vec2 uv;
in vec3 f_dir;
uniform sampler2DArray moon_tex;
uniform int tex_index;
uniform vec3 moon_color;
uniform float opacity;
uniform int additive;

out vec4 out_color;

void main()
{
  vec4 t = texture(moon_tex, vec3(uv, float(tex_index)));
  // HORIZON CLIP: the below-horizon sky is a solid fog wall the celestial must set BEHIND. Kill
  // fragments whose view direction dips below the horizon (soft ~0.3 deg edge so the disc melts
  // into the band instead of being razor-cut).
  float horizon = smoothstep(0.0, 0.006, f_dir.y / length(f_dir));
  if (additive == 1)
  {
    // The glare texture has a faint background haze (sunGlare median lum ~6/255) that, added and
    // brightened, shows as a glowing SQUARE. Two guards: (1) subtract that floor, (2) HARD radial mask
    // forcing the glow to 0 before the quad edge so the square can never appear -- the glow is radial
    // so this only trims empty corners.
    vec3 g = max(t.rgb * t.a - vec3(0.04), vec3(0.0));
    float d = length(uv - vec2(0.5)) * 2.0;        // 0 centre .. 1 edge midpoint .. 1.414 corner
    float mask = 1.0 - smoothstep(0.62, 0.98, d);  // circle mask -> 0 well before the edge
    vec3 rgb = g * moon_color * opacity * mask * horizon;
    // ALPHA must follow the circular glow, NOT a flat 1.0. The scene alpha channel is the bloom
    // emissive mask; with GL_ONE additive a flat alpha=1 flags the WHOLE square quad emissive and the
    // FFXGlow blooms it as a glowing square. Circular alpha -> circular bloom, no square.
    out_color = vec4(rgb, clamp(max(rgb.r, max(rgb.g, rgb.b)), 0.0, 1.0));
  }
  else
  {
    out_color = vec4(t.rgb * moon_color, t.a * opacity * horizon);
  }
}
