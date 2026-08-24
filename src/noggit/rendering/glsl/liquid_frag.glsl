// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 410 core

layout (std140) uniform lighting
{
  vec4 DiffuseColor_FogStart;
  vec4 AmbientColor_FogEnd;
  vec4 FogColor_FogOn;
  vec4 LightDir_FogRate;
  vec4 OceanColorLight;
  vec4 OceanColorDark;
  vec4 RiverColorLight;
  vec4 RiverColorDark;
};

uniform float animtime;
uniform int draw_shadows;
uniform sampler2DArray shadowmap;
uniform sampler2DArray texture_samplers[14] ;
uniform float water_alpha_mult; // dev opacity lever (1 = unchanged)
uniform vec3 camera;                 // for the water specular view direction
uniform vec3 sun_spec_color;         // sun-band colour (LightIntBand band 9), same as terrain specular
uniform int draw_water_specular;     // toggle (render/water_specular)

in float depth_;
in vec2 tex_coord_;
in float dist_from_camera_;
flat in uint tex_array;
flat in uint type;
flat in vec2 anim_uv;
flat in int tex_frame;
flat in uint shadow_chunk_index;
in vec2 shadow_uv;
in vec3 world_pos_;

out vec4 out_color;

vec4 get_tex_color(vec2 tex_coord, uint tex_sampler, int array_index)
{
  if (tex_sampler == 0)
  {
    return texture(texture_samplers[0], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 1)
  {
    return texture(texture_samplers[1], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 2)
  {
    return texture(texture_samplers[2], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 3)
  {
    return texture(texture_samplers[3], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 4)
  {
    return texture(texture_samplers[4], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 5)
  {
    return texture(texture_samplers[5], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 6)
  {
    return texture(texture_samplers[6], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 7)
  {
    return texture(texture_samplers[7], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 8)
  {
    return texture(texture_samplers[8], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 9)
  {
    return texture(texture_samplers[9], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 10)
  {
    return texture(texture_samplers[10], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 11)
  {
    return texture(texture_samplers[11], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 12)
  {
    return texture(texture_samplers[12], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 13)
  {
    return texture(texture_samplers[13], vec3(tex_coord, array_index)).rgba;
  }

  return vec4(0);
}

vec4 get_tex_color_biased(vec2 tex_coord, uint tex_sampler, int array_index, float bias)
{
  if (tex_sampler == 0)
  {
    return texture(texture_samplers[0], vec3(tex_coord, array_index), bias).rgba;
  }
  else if (tex_sampler == 1)
  {
    return texture(texture_samplers[1], vec3(tex_coord, array_index), bias).rgba;
  }
  else if (tex_sampler == 2)
  {
    return texture(texture_samplers[2], vec3(tex_coord, array_index), bias).rgba;
  }
  else if (tex_sampler == 3)
  {
    return texture(texture_samplers[3], vec3(tex_coord, array_index), bias).rgba;
  }
  else if (tex_sampler == 4)
  {
    return texture(texture_samplers[4], vec3(tex_coord, array_index), bias).rgba;
  }
  else if (tex_sampler == 5)
  {
    return texture(texture_samplers[5], vec3(tex_coord, array_index), bias).rgba;
  }
  else if (tex_sampler == 6)
  {
    return texture(texture_samplers[6], vec3(tex_coord, array_index), bias).rgba;
  }
  else if (tex_sampler == 7)
  {
    return texture(texture_samplers[7], vec3(tex_coord, array_index), bias).rgba;
  }
  else if (tex_sampler == 8)
  {
    return texture(texture_samplers[8], vec3(tex_coord, array_index), bias).rgba;
  }
  else if (tex_sampler == 9)
  {
    return texture(texture_samplers[9], vec3(tex_coord, array_index), bias).rgba;
  }
  else if (tex_sampler == 10)
  {
    return texture(texture_samplers[10], vec3(tex_coord, array_index), bias).rgba;
  }
  else if (tex_sampler == 11)
  {
    return texture(texture_samplers[11], vec3(tex_coord, array_index), bias).rgba;
  }
  else if (tex_sampler == 12)
  {
    return texture(texture_samplers[12], vec3(tex_coord, array_index), bias).rgba;
  }
  else if (tex_sampler == 13)
  {
    return texture(texture_samplers[13], vec3(tex_coord, array_index), bias).rgba;
  }

  return vec4(0);
}

vec4 get_magma_color(vec2 tex_coord, uint tex_sampler, int array_index)
{
  // Match reference noggit3: use the per-vertex magma UV directly (no down-scale, no LOD
  // bias) so the lava shows its detailed crust pattern, and scroll by the LiquidType
  // animation direction so it flows. (Frame cycling via tex_frame handles the churn.)
  // Quarter-speed lava flow (matches WMO magma_flow_speed = 0.25/2880; divisor x4 = quarter rate).
  vec2 scroll = vec2(anim_uv.x * animtime / 11520.0,
                     anim_uv.y * animtime / 11520.0);
  return get_tex_color(tex_coord + scroll, tex_sampler, array_index);
}

vec2 rot2(vec2 p, float degree)
{
  float a = radians(degree);
  return mat2(cos(a), -sin(a), sin(a), cos(a))*p;
}

void main()
{
  if(type == 2)
  {
    out_color = get_magma_color(tex_coord_, tex_array, tex_frame);
    // Lava is opaque; liquid pass blends, so raw texture alpha darkened lava over the cave.
    out_color.a = 1.0;
  }
  // slime
  else if(type == 3)
  {
    // Slime scroll at quarter speed, matching lava (divisor x4: 2880 -> 11520).
    out_color = get_tex_color(tex_coord_ + vec2(anim_uv.x*animtime / 11520.0, anim_uv.y*animtime / 11520.0), tex_array, tex_frame);
    out_color.a = min(out_color.a, 0.85); // keep below the emissive bloom range (lava-only)
  }
  else
  {
    // Seamless in-game-style water (matches reference noggit3): depth-tinted ocean/river color
    // plus the water texture, additive. Deliberately NO per-chunk terrain shadow multiply (that
    // sampled a different shadowmap layer per chunk -> hard tile-boundary patches) and NO extra
    // ambient/diffuse lighting multiply (that muddied the color and mismatched the in-game look).
    // Per-vertex depth is smooth, so adjacent tiles now blend without seams.
    vec2 uv = rot2(tex_coord_ * anim_uv.x, anim_uv.y);

    vec4 texel = get_tex_color(uv, tex_array, tex_frame);

    // depth_ is now the RAW water depth (world units above the bottom, terrain-derived &
    // continuous). Map it two independent ways:
    //  - color_depth: a BROAD gradient so the water is light near the coast and only fades to
    //    the dark deep "fatigue" color far out (the shore lightening the in-game ocean has).
    //  - alpha_depth: a STEEPER ramp to opaque so the per-tile seafloor terrain stays hidden in
    //    deeper water (no "tile" seams) while shallow water near shore still shows the bottom.
    // color_depth: shallow (shore/edge, LIGHT colour) -> deep (DARK colour). The OCEAN is genuinely deep and
    // wants a BROAD ~80-unit gradient (light shore fading to the deep "fatigue" blue far out). RIVERS/canals
    // are SHALLOW (~5-20 units), so the 80-unit ramp never reaches their deep colour -> the whole river reads
    // shallow-GREEN (user report: "rivers all green, should be blue in the middle / green on the sides", and
    // Stormwind canal green up close). Give rivers a MUCH steeper ramp so the deep (blue) river colour shows
    // in the channel middle by ~20 units while the shallow edges stay on the green LIGHT colour.
    float ocean_color_depth = clamp(depth_ * 0.012, 0.0, 1.0); // ocean: light -> dark over ~80 units
    float river_color_depth = clamp(depth_ * 0.05,  0.0, 1.0); // river: reaches the zone's deep colour by ~20 units
    float alpha_depth = clamp(depth_ * 0.08,  0.0, 1.0); // ~opaque by ~12 units

    // Rivers/lakes colour -- CONDITIONAL on whether the zone authors real OCEAN water (2026-08-23,
    // Stormwind-canal green regression). Two facts settled it by runtime dump + DBC:
    //   * Western Plaguelands (inland murk): ocean bands UNAUTHORED = (0,0,0); its river band is the
    //     authored muddy-brown/olive -> the client shows brown. Pure river band is correct here.
    //   * Stormwind city (light 77, coastal): ocean bands AUTHORED blue (~17,74,88); its river band is
    //     GREEN (~78,92,20). The canals are river-typed ADT liquid, so a pure river band renders them
    //     grass-green -- but the in-game canals read a muted teal-blue (they share the harbour's blue
    //     water body). The 2026-08-07 tuning (blend the river colour 75% toward the zone ocean colour,
    //     darken x0.72) reproduces that; user-confirmed. Applying it GLOBALLY is what blued WPL, so it
    //     is now GATED on the ocean bands being authored: coastal zones get the teal blend, inland
    //     zones (ocean == 0) keep their pure river band. Ocean branch unchanged.
    // Does the zone author a REAL, visible OCEAN water colour? [WATERSNAP 2026-08-23, fly-through log]
    // Caer Darrow / WPL lake is a MIX of river-typed and OCEAN-typed ADT chunks, but WPL's ocean band is
    // near-black (measured oceanL=2,9,11 / oceanD=0,3,5). The ocean-typed lake chunks therefore rendered
    // BLACK and SNAPPED against the warm river-typed chunks as the camera crossed the type boundary. The
    // 1.12 client shows the whole lake warm -> an inland "ocean" chunk with no real ocean colour is just
    // LAKE water and must use the zone's river/lake band, not a near-black ocean band. Gate on ocean
    // BRIGHTNESS (max channel), not a tiny non-zero sum, so near-black inland ocean bands fall back to
    // river while a genuine blue ocean (e.g. oceanL max ~0.34) still uses the ocean colour.
    float ocean_lum = max(max(OceanColorLight.r, OceanColorLight.g), OceanColorLight.b);
    bool ocean_bright = ocean_lum > 0.1;
    bool use_ocean = (type == 1) && ocean_bright;

    float cd = use_ocean ? ocean_color_depth : river_color_depth;
    vec4 lerp;
    if (use_ocean)
    {
      lerp = mix (OceanColorLight, OceanColorDark, cd);
    }
    else
    {
      // River / lake water (includes inland "ocean"-typed chunks with no real ocean colour). COASTAL
      // rivers where the zone DOES author a blue ocean (Stormwind canals) keep the 2026-08-07 teal blend.
      vec3 river_shallow = ocean_bright
                         ? mix(RiverColorLight.rgb, OceanColorLight.rgb, 0.75) * 0.72
                         : RiverColorLight.rgb;
      vec3 river_deep    = ocean_bright
                         ? mix(RiverColorDark.rgb,  OceanColorDark.rgb,  0.75) * 0.72
                         : RiverColorDark.rgb;
      lerp = vec4(mix(river_shallow, river_deep, cd),
                  mix(RiverColorLight.a, RiverColorDark.a, cd));
    }

    // (A4 authored-alpha RESOLVED 2026-07-13: the earlier "too transparent" was the LightParams
    // OFF-BY-ONE, not zero endpoints -- the classic branch read river_shallow from the glow column
    // (~0.2). With the off-by-one fixed (Sky.cpp reads cols 5-8) and SkyParam defaulting the alphas to
    // 0.6 (never 0), the endpoints propagate correctly; consumed for the per-zone water alpha below.)
    // Water COLOR = the depth-tinted zone ocean/river color (light blue at the coast -> dark blue
    // deep), MODULATED by the water texture's brightness (its ripple/wave pattern) rather than the
    // old `texel + color` ADDITIVE -- the ocean texture is greenish, so adding it turned the blue
    // water teal/washed. Using the texture as a luminance ripple keeps the client's blue hue while
    // still animating the surface. (Reference: in-game Stormwind harbour = dark-blue deep, lighter-
    // blue coast, NOT green.)
    float ripple = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
    // Base water body = the depth-tinted zone ocean/river colour (lighter coast -> darker deep). The
    // surface texture is NOT modulated in here -- the client adds it additively (see below), so the
    // base stays a clean blue/teal and the texture shows up as the bright shine on top.
    vec3 water_rgb = lerp.rgb;

    // WATER SHINE -- reverse-engineered from wow_cap_westfall_ocean.trace. The 1.12 client draws the
    // ocean surface ADDITIVELY (SRCBLEND=SRCALPHA, DESTBLEND=ONE) with LIGHTING=FALSE, SPECULARENABLE
    // =FALSE and NO SetLight/SetMaterial, stage0 COLOROP=MODULATE(texture x per-vertex diffuse). So the
    // "reflection/sheen" is NOT a hardware specular at all -- it is the water TEXTURE's bright (white/
    // foam) texels ADDING light. We reproduce that: add sun-coloured light scaled by the texel
    // brightness so the white parts of the texture SHINE, everywhere as a broad sparkle and much
    // harder inside the sun-reflection band so the sun-facing water glints.
    if (draw_water_specular != 0)
    {
      // Sun-reflection band, DIRECTION-CORRECTED. to_light lives in the terrain NORMAL frame
      // (wow.x,wow.z,wow.y); to_view built from positions lives in noggit's POSITION frame
      // (-wow.y,wow.z,-wow.x). They MUST be in the same frame or the half-vector is ~180 deg off
      // (glint on the wrong side -- "back to the sun"). Convert to_view position->normal via (-z,y,-x),
      // EXACTLY like terrain_frag.glsl:301. camera uniform == terrain's (WorldRender 1162 vs 3803).
      vec3 to_light   = -normalize(vec3(LightDir_FogRate.x, LightDir_FogRate.z, LightDir_FogRate.y));
      vec3 to_view_ps = normalize(camera - world_pos_);
      vec3 to_view    = vec3(-to_view_ps.z, to_view_ps.y, -to_view_ps.x);
      vec3 half_vec   = normalize(to_light + to_view);
      float sun_sheen = pow(clamp(dot(vec3(0.0, 1.0, 0.0), half_vec), 0.0, 1.0), 8.0);

      // Additive shine, WATER-COLOR MODULATED [2026-08-22]. The client's additive pass is
      // MODULATE(texture x per-vertex diffuse) where the vertex diffuse IS the depth-lerped zone
      // water colour -- so in a murk zone (WPL: riverDeep RGB 62,72,56, dim orange light) the shine
      // self-dims with the water, while a bright ocean sparkles. The old raw sun-coloured broad term
      // (sun_spec x ripple x 1.2) added ~0.3 of warm light REGARDLESS of the zone, doubling the
      // brightness of authored-dark water ("WPL still feels bright" with a correct dark base).
      //   broad term  = texel x waterColor (the client's own additive formula)
      //   sun glint   = the validated sun-band sparkle, now also scaled by the water colour so
      //                 murk pools don't catch an ocean-strength glint.
      // Additive surface texture (trace-confirmed: the client draws the water surface with
      // DESTBLEND=ONE = ADDITIVE over the water body). The sliding ripple texture must be clearly
      // VISIBLE. It was too faint because texel*lerp self-dims to ~nothing on WPL's dark water, so add a
      // direct (non-modulated) texel component too, then the water-colour-modulated part on top, plus
      // the sun-band glint.
      water_rgb += texel.rgb * 0.8
                 + texel.rgb * lerp.rgb * 1.2
                 + sun_spec_color * ripple * lerp.rgb * 2.0 * sun_sheen;
    }
    water_rgb = clamp(water_rgb, 0.0, 1.0);

    // WATER OPACITY = the zone's AUTHORED shallow->deep alpha (LightParams cols 5-8), depth-ramped.
    // [2026-08-23 CORRECTION of the same day's mistake:] the "WPL water is 2-4% opacity" measurement
    // that briefly replaced this with `deep_a * alpha_depth` (no shallow floor) was taken from the
    // WRONG trace pass — the stride-24 draws at wow_cap_wpl_water.trace calls 3694+ run under
    // DESTBLEND=ONE, i.e. they are the ADDITIVE surface-texture pass; their 0..11 vertex alpha is the
    // additive fade, NOT the base water film. The base alpha-blend pass keeps the authored floor —
    // and LightParams authors waterShallow=0.5 for WPL exactly like everywhere else. Removing the
    // floor made every shallow river/canal in the world nearly INVISIBLE (user report). Authored
    // endpoints, depth-ramped, restored:
    float shallow_a = (type == 1) ? OceanColorLight.a : RiverColorLight.a;
    float deep_a    = (type == 1) ? OceanColorDark.a  : RiverColorDark.a;
    float water_alpha = mix(shallow_a, deep_a, alpha_depth);
    out_color = vec4(water_rgb, clamp(water_alpha * water_alpha_mult, 0.0, 1.0));
  }

  // [2026-08-22] Fog applies to ALL liquid types, lava included -- the old "reference applies no
  // fog to lava" exclusion was noggit3 behaviour, not the client's: the game's fixed-function fog
  // stays enabled for the magma draw, so distant lava fades into the haze like the terrain around
  // it (the exclusion made lava punch through fog at full saturation). Alpha is left alone: the
  // lava stays opaque and shows the fogged colour, and the dimmed rgb naturally stops the bloom.
  if (FogColor_FogOn.w != 0)
  {
    float start = AmbientColor_FogEnd.w * DiffuseColor_FogStart.w;

    vec3 fogParams;
    fogParams.x = -(1.0 / (AmbientColor_FogEnd.w - start));
    fogParams.y = (1.0 / (AmbientColor_FogEnd.w - start)) * AmbientColor_FogEnd.w;
    fogParams.z = LightDir_FogRate.w;

    float f1 = (dist_from_camera_ * fogParams.x) + fogParams.y;
    float f2 = max(f1, 0.0);
    float f3 = pow(f2, fogParams.z);
    float f4 = min(f3, 1.0);

    float fogFactor = 1.0 - f4;

    out_color.rgb = mix(out_color.rgb, FogColor_FogOn.rgb, fogFactor);
  }
}
