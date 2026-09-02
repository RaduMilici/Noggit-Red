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
  vec4 PointLightParams;
  vec4 PointLightPos[16];
  vec4 PointLightColor[16];
  vec4 EnvFogColor_On;       // rgb = entity fog colour (camera's fog context), w = 1 when active
  vec4 EnvFogDist;           // x = start FRACTION of end (can be negative), y = end
};

// camera_pos (world/position frame) for the water shine's view direction. Same UBO the WMO-liquid
// vertex uses; bound program-wide (WorldRender bind_uniform_block("matrices", 0)).
layout (std140) uniform matrices
{
  mat4 model_view;
  mat4 projection;
  vec4 camera_pos;
};

uniform float animtime;
uniform sampler2DArray texture_samplers[14];
uniform int tex_frame;
uniform int liquid_type;
uniform vec2 anim_uv;
uniform vec2 magma_flow_dir;   // lava scroll direction (UV space)
uniform float magma_flow_speed; // lava scroll rate (UV units per ms)
uniform float water_alpha_mult; // dev water-opacity lever (1 = unchanged)
uniform vec4 debug_liquid_color;
// INTERIOR WMO water: the client (FUN_006b6420) paints indoor liquid with the WMO material's own
// MOMT.diffColor -- a flat, baked colour -- NOT the zone day/night water light. use_material_color=1
// selects that path; material_color is that diffColor (linear 0..1 RGB). Chosen per group by the
// EXTERIOR/exterior-lit flags (MOGP & 0x48) in WMO.cpp.
uniform int use_material_color;
uniform vec3 material_color;
// Exterior WMO water flat colour: the WATER param's RIVER_COLOR_DARK band (SW noon #234A69 --
// the canal dark blue). The CLEAR param's band 17 is the muddy teal; the client's canal hue
// matches the WATER param family, so Skies feeds this per frame from CLEAR_WATER.
uniform vec3 wmo_water_river_dark;
// 1 = city water channel (MOGP indoor 0x2000 + exterior_lit: SW canals/harbor, Booty Bay) -> the
// ocean-dark opaque look; 0 = open-air WMO pool (Northshire abbeygate stream) -> river blend.
uniform int wmo_indoor_channel;
uniform vec3 camera; // for the user-directed sun/moon glitter lobe on exterior WMO water
uniform vec3 sheen_dir; // TO the drawn sun/moon disc (WorldRender celestial_dir)

in vec2 tex_coord_;
in float depth_;
in float dist_from_camera_;
in vec3 world_pos_;

out vec4 out_color;

vec4 get_tex_color(vec2 tex_coord, int frame)
{
  return texture(texture_samplers[0], vec3(tex_coord, frame)).rgba;
}

vec4 get_magma_color(vec2 tex_coord, int frame)
{
  // Lava flow: scroll the authored per-vertex UVs along magma_flow_dir at magma_flow_speed.
  // Direction and speed are driven from wmo_liquid::draw so they can be tuned in one place.
  vec2 scroll = magma_flow_dir * (animtime * magma_flow_speed);
  return texture(texture_samplers[0], vec3(tex_coord + scroll, frame)).rgba;
}

vec2 rot2(vec2 p, float degree)
{
  float a = radians(degree);
  return mat2(cos(a), -sin(a), sin(a), cos(a)) * p;
}

void main()
{
  if (debug_liquid_color.a > 0.0)
  {
    out_color = debug_liquid_color;
    return;
  }

  if (liquid_type == 2)
  {
    out_color = get_magma_color(tex_coord_, tex_frame);
    // Lava is opaque; the liquid pass blends, so raw texture alpha darkened it over the cave.
    out_color.a = 1.0;
  }
  else if (liquid_type == 3)
  {
    // Slime scroll at quarter speed, matching lava (divisor x4: 2880 -> 11520).
    out_color = get_tex_color(tex_coord_ + vec2(anim_uv.x * animtime / 11520.0,
                                                anim_uv.y * animtime / 11520.0),
                              tex_frame);
  }
  else
  {
    vec2 uv = rot2(tex_coord_ * anim_uv.x, anim_uv.y);
    vec4 texel = get_tex_color(uv, tex_frame);
    vec4 liquid_color = (liquid_type == 1)
      ? mix(OceanColorLight, OceanColorDark, depth_)
      : mix(RiverColorLight, RiverColorDark, depth_);

    // The alpha is depth-based from the zone shallow/deep WATER alpha for BOTH indoor and outdoor water
    // (client depth LUT from lightStruct +0x114/+0x118) -- only the RGB source differs by group.
    float water_alpha = liquid_color.a * water_alpha_mult;

    vec3 water_rgb;
    if (use_material_color != 0)
    {
      // INTERIOR water (client FUN_006b6420 + D3D9 trace of the actual Timbermaw frame): the fixed-
      // function stage0 is COLOROP=ADD(TEXTURE, DIFFUSE) with LIGHTING=FALSE, i.e. water texture PLUS
      // the flat per-vertex diffuse = the WMO material's MOMT.diffColor. No zone-light tint, no sun
      // shine. This is why Timbermaw's cave pool is NOT green in-game even though the EXTERIOR Felwood
      // zone water light is green: indoor liquid never consults the zone light for its hue -- it reads
      // the baked material colour straight from the WMO (near-black in the deep pools, dark blue in the
      // shallower ones). The old code used the green zone RiverColor here; this reproduces the client.
      water_rgb = texel.rgb + material_color;
    }
    else
    {
      // EXTERIOR water (client FUN_006b6630, PINNED 2026-08-24): the vertex colour is ONE FLAT zone
      // colour -- light-manager bytes +0xec, which the band evaluator (FUN_006d64d0 slot map) proves
      // is Light band 17 = RIVER DEEP. No shallow/deep depth mix, no ocean-band branch (the legacy
      // dispatcher has no ocean case). Stormwind canals: #27372F from above / #234A69 (dark blue)
      // when the CLEAR_WATER param is active -- the user's expected canal colour. Depth still drives
      // the ALPHA (client depth LUT, kept above). Texture rides as the additive shine below.
      // MEASURED (wow_cap_fountain.trace state distribution): stage-0 COLOROP is NEVER ADD in the
      // whole capture -- exterior WMO water is the SAME TWO-PASS model as ADT water:
      //   base pass    = MODULATE(texture x flat diffuse)  -> tex * dark colour, much darker than
      //                  the flat colour alone (the "not dark enough" gap), alpha-blended;
      //   additive pass = the texture again with DESTBLEND=ONE -> full-brightness sparkles on top.
      // Emulated in one pass with the ADT-water additive strengths the user already validated
      // (liquid_frag.glsl shine block), minus the sun-glint term (no sun uniform here).
      // ROUND 6 -- the sparkle was NEVER visible because lake_a.*.blp stores the water pattern in
      // ALPHA (measured: RGB mean 4/255 near-black, alpha mean 54 with caustic lines to 255 -- the
      // classic 1.12 alpha-replicate water texture). Every texel.rgb term multiplied by ~zero.
      // Pattern = texel.a; base = the WATER param's river-deep band (#234A69 SW noon).
      // The sliding-texture pattern lives in ALPHA on the 1.12 water textures (RGB is near-black).
      // UNIFIED surface-layer strength across ALL water bodies (user round 10): pat*0.55 raw +
      // pat*body*1.0 -- between the old canal punch and the old ocean shine. liquid_frag (ADT)
      // uses the same law.
      float pattern = texel.a;
      if (wmo_indoor_channel == 1)
      {
        // CITY CHANNEL (canals/harbor/Booty Bay): ocean-deep navy body, fully opaque
        // (user-directed rounds 8-9; round 19: "a tad darker" -> x0.8). ROUND 21 (2026-08-28,
        // "canal water is too dark in color and shade so lower the dark blue a bit"): the x0.8
        // darkening is dropped and the body is lifted 15% toward the shallow ocean band, which
        // takes depth out of the navy without changing its hue family. USER-DIRECTED, not a
        // client law -- the exterior client law is flat river-deep (see above).
        water_rgb = mix(OceanColorDark.rgb, OceanColorLight.rgb, 0.12) * 0.85;
        water_alpha = RiverColorDark.a * water_alpha_mult;
      }
      else
      {
        // OPEN-AIR WMO POOL (Northshire abbeygate stream): the SAME depth-mixed shallow->deep
        // colour ADT rivers use (flat river-deep still stuck out darker than the river beside
        // it -- user round 11). liquid_color is already that mix. x0.85 = the round-20 surface
        // darkening, matching liquid_frag so pools stay flush with the rivers they feed.
        water_rgb = liquid_color.rgb * 0.85;
      }
      // Unified surface layer, lowered (user round 11: 0.55 raw was too speckly).
      // USER-DIRECTED GLITTER (2026-08-25, same law as liquid_frag): exterior WMO water dims the
      // pattern at baseline and blooms it toward the sun/moon. INDOOR CHANNELS (canals) keep the
      // constant user-validated strength -- their look was tuned over 9 rounds; no roof-borne sun.
      // User 2026-08-26 (canal screenshot: "same shine all over, too bright"): the canal exemption
      // is GONE -- city channels take the directional law like every other water.
      float sheen = 1.0;
      if (camera.y >= world_pos_.y) // from below: full pattern (see liquid_frag)
      {
        vec3 sheen_V = normalize(camera - world_pos_);
        // ROUND 3: keyed on the DRAWN sun/moon disc (see liquid_frag) -- the dayDir frame mirrored
        // the azimuth ("back to the sun" report).
        vec3 sheen_L = sheen_dir;
        float L_len = length(sheen_L);
        if (L_len > 0.001)
        {
          sheen_L /= L_len;
          sheen_L.y = max(sheen_L.y, 0.14); // disc lifted >= ~8deg above horizon (night moon band)
          sheen_L = normalize(sheen_L);
          float sheen_align = clamp(dot(reflect(-sheen_V, vec3(0.0, 1.0, 0.0)), sheen_L), 0.0, 1.0);
          sheen = 0.25 + 1.25 * pow(sheen_align, 8.0);
        }
      }
      water_rgb += (vec3(pattern) * 0.30 + pattern * water_rgb * 1.0) * sheen;
    }
    out_color = vec4(clamp(water_rgb, 0.0, 1.0), water_alpha);
  }

  if (FogColor_FogOn.w != 0) // fog applies to lava too (see liquid_frag.glsl -- client behaviour)
  {
    // WMO liquid sits inside the WMO: fog with the camera's fog context (Env slots) when active.
    bool use_env = EnvFogColor_On.w > 0.5;
    vec3 fog_color_l = use_env ? EnvFogColor_On.rgb : FogColor_FogOn.rgb;
    float fog_end_l = use_env ? EnvFogDist.y : AmbientColor_FogEnd.w;
    float fog_start_frac_l = use_env ? EnvFogDist.x : DiffuseColor_FogStart.w;

    float start = fog_end_l * fog_start_frac_l;

    vec3 fogParams;
    fogParams.x = -(1.0 / (fog_end_l - start));
    fogParams.y = (1.0 / (fog_end_l - start)) * fog_end_l;
    fogParams.z = LightDir_FogRate.w;

    float f1 = (dist_from_camera_ * fogParams.x) + fogParams.y;
    float f2 = max(f1, 0.0);
    float f3 = pow(f2, fogParams.z);
    float f4 = min(f3, 1.0);

    out_color.rgb = mix(out_color.rgb, fog_color_l, 1.0 - f4);

    // Same as liquid_frag (round 28): a fully-fogged emissive liquid must not bloom back through
    // the fog (alpha > 0.88 is the bright pass's emissive mask). Ordinary water alpha is untouched.
    out_color.a = min(out_color.a, mix(0.85, 1.0, f4));
  }

  // No alpha cap: water/slime draws mask the framebuffer alpha channel (wmo_liquid::draw), so the
  // authored blend alpha can reach the client's 1.0 (opaque deep canal water) without polluting
  // the bloom's emissive mask. Lava still writes alpha 1.0 as its emissive flag.
}
