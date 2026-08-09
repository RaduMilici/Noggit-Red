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
      // EXTERIOR water (client FUN_006b6630): zone day/night water light colour used DIRECTLY (client
      // draws liquid with LIGHTING=FALSE, so no ambient/diffuse multiply), plus the texture as an
      // additive shine for the ocean look. Sun band direction-corrected exactly like liquid_frag:
      // to_light in the NORMAL frame; to_view from camera_pos->world_pos converted position->normal
      // via (-z,y,-x). Neutral-warm shine tint (NOT the WMO DiffuseColor).
      water_rgb = liquid_color.rgb;
      float ripple = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
      vec3 to_light   = -normalize(vec3(LightDir_FogRate.x, LightDir_FogRate.z, LightDir_FogRate.y));
      vec3 to_view_ps = normalize(camera_pos.xyz - world_pos_);
      vec3 to_view    = vec3(-to_view_ps.z, to_view_ps.y, -to_view_ps.x);
      vec3 half_vec   = normalize(to_light + to_view);
      float sun_sheen = pow(clamp(dot(vec3(0.0, 1.0, 0.0), half_vec), 0.0, 1.0), 8.0);
      water_rgb += vec3(0.85, 0.88, 0.95) * ripple * (1.2 + 3.0 * sun_sheen);
    }
    out_color = vec4(clamp(water_rgb, 0.0, 1.0), water_alpha);
  }

  if (FogColor_FogOn.w != 0 && liquid_type != 2) // reference applies no fog to lava
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
  }

  // Only lava is emissive (writes alpha 1.0 so the bloom bright-pass makes it glow). Keep slime/water
  // below the reserved emissive range (>0.88) so they don't bloom.
  if (liquid_type != 2)
  {
    out_color.a = min(out_color.a, 0.85);
  }
}
