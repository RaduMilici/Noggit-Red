// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Composite: original scene + blurred bloom. The add makes bright areas bleed/glow onto their
// surroundings and blow out toward white -- the overbearing golden-light look of cave openings etc.
in vec2 uv;
out vec4 out_color;

uniform sampler2D scene;
uniform sampler2D bloom;
uniform float intensity;

// Underwater wave warp -- the 1.12 FFXGlowWave composite (RE doc 36). The client's glow composite
// samples screen+blur through a DuDv warp: waveTex = 128x128 procedural (sin(2*pi*x/128),
// sin(2*pi*y/128)) V8U8 (generator FUN_006cbea0), sampled with a texture matrix =
// rotate(10 deg) * scale(1.0/128, 0.88/128 per PIXEL) + scroll(t/3174ms, t/2805ms)
// (FUN_006cb310 + setters FUN_006cbc40/006cbca0), and the ps (FFXGlowWave.bls, all variants) offsets
// both samples by wave * bumpMtx where bumpMtx = diag(3.0 * texel) (_DAT_0081147c = 3.0) -- i.e.
// +/-3 screen pixels. The ps law: oC0 = mix(screen', blur', v0.z) + blur'^2 * v0.w with
// v0.w = zone glow (LightParams field 4; the CLEAR_WATER param sets author 1.0) and v0.z = the
// scene-glow byte, floored at 84/255 = 0.329 in-game.
// EDITOR DELTA (deliberate): the client runs this warp on EVERY in-game frame (ffxGlow on); noggit
// gates it to a submerged camera so the editing viewport stays stable above water. Underwater the
// full client law runs (warp + 0.329 base blur mix + blur^2 * glow); above water the validated
// noggit composite (scene + blur^2 * zone glow, no warp, no base mix) is unchanged.
uniform int wave_on;          // 1 = camera submerged
uniform float wave_time;      // seconds
uniform vec2 viewport_px;     // composite target size in pixels

uniform float wave_strength; // coupled wobble scale, 1.0 = client-exact 3px/0.329

void main()
{
  vec2 suv = uv;
  float base_mix = 0.0;
  if (wave_on == 1)
  {
    // wave UV in tiles: one 128px tile horizontally, 128/0.88 ~ 145px vertically, rotated 10 deg,
    // scrolling one tile per 3.174s (u) / 2.805s (v) -- incommensurate periods so it never loops.
    vec2 wuv = uv * viewport_px * vec2(1.0 / 128.0, 0.88 / 128.0);
    float c = cos(radians(10.0));
    float s = sin(radians(10.0));
    wuv = vec2(c * wuv.x - s * wuv.y, s * wuv.x + c * wuv.y);
    wuv += vec2(wave_time * 1000.0 / 3174.0, wave_time * 1000.0 / 2805.0);
    vec2 dudv = vec2(sin(6.2831853 * wuv.x), sin(6.2831853 * wuv.y));
    // CLIENT-EXACT RESTORED (2026-08-26). History: +/-3px + 0.329 base mix are the verified
    // client constants; two earlier tune-downs (to 1.0px/0.12) chased "too strong" -- but the
    // user then observed the in-game wobble feels WEAKER than ours. Root cause of the inversion:
    // the client hides its 3px warp under the 0.329 BLUR VEIL; cutting the veil to 0.12 left the
    // scene nearly crisp, and a warp on crisp edges reads harsher per pixel than a larger warp
    // under softness. The two constants must move TOGETHER; back to the client's exact pair.
    // (If the veil now reads softer than in-game, the real fix is matching the client's
    // Box4+Gauss4 blur profile -- not detuning this mix again.)
    // wave_strength: COUPLED live lever (Settings render/underwater_wobble_strength, 1.0 =
    // client-exact) -- amplitude and veil scale TOGETHER (doc 37: scaling them apart caused the
    // perceived-strength inversion). Exists because noggit's blur profile differs from the
    // client's Box4+Gauss4, so the exact pair can read differently here.
    suv = uv + dudv * (3.0 * wave_strength) / viewport_px;
    base_mix = 0.329 * wave_strength;
  }
  vec3 c3 = texture(scene, suv).rgb;
  vec3 b = texture(bloom, suv).rgb;
  // MEASURED composite (wow_cap_upstairs.trace, RE_notes/16): oC0 = scene + weight * blur^2 with
  // weight = the zone glow (0.647 in the inn frame, carried in the quad vertex alpha). blur is
  // SQUARED -- dark blur contributes ~nothing, bright areas glow warm -- preserving contrast
  // instead of washing the frame.
  vec3 base = mix(c3, b, base_mix);
  out_color = vec4(base + b * b * intensity, 1.0);
}
