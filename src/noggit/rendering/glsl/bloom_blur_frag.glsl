// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Client FFXGlow blur, extracted from the live 1.12 + 3.3.5a clients (wow_cap_bloom-snow trace,
// pixel shader 4124 / 335a 13034): a SINGLE 4-tap gaussian per axis, weights [1/8, 3/8, 3/8, 1/8]
// (`def c0, 0.375, 0.125`), at texel offsets [-1.5, -0.5, +0.5, +1.5]. Run once horizontal, once
// vertical (2 passes total) on the quarter-res target.
//
// This REPLACES the old 6-pass 9-tap radius-4 gaussian, which spread the glow ~10x wider than the
// client and over-bloomed bright zones (snow in Dun Morogh/Icecrown smeared the halo across the
// whole frame). The client's tight 4-tap kernel keeps the glow to its measured footprint.
in vec2 uv;
out vec4 out_color;

uniform sampler2D image;
uniform int horizontal;
uniform vec2 texel; // 1/width, 1/height of this (quarter-res) target

void main()
{
  vec2 dir = (horizontal != 0) ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);

  vec3 result =
      texture(image, uv + dir * -1.5).rgb * 0.125
    + texture(image, uv + dir * -0.5).rgb * 0.375
    + texture(image, uv + dir *  0.5).rgb * 0.375
    + texture(image, uv + dir *  1.5).rgb * 0.125;

  out_color = vec4(result, 1.0);
}
