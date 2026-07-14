// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 410 core

// flags
#define eWMOBatch_ExteriorLit 0x1u
#define eWMOBatch_HasMOCV 0x2u
#define eWMOBatch_Unlit 0x4u
#define eWMOBatch_Unfogged 0x8u
#define eWMOBatch_Sidn 0x20u
#define eWMOBatch_PortalSpill 0x40u
#define eWMOBatch_ClampS 0x80u
#define eWMOBatch_ClampT 0x100u
#define eWMOBatch_Window 0x200u

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
  vec4 PointLightParams;     // .x = active point-light count
  vec4 PointLightPos[16];    // xyz = world pos, w = radius
  vec4 PointLightColor[16];  // xyz = colour * intensity
};

vec3 point_lights(vec3 world_pos, vec3 n)
{
  vec3 accum = vec3(0.0);
  int count = int(PointLightParams.x);
  for (int i = 0; i < count; ++i)
  {
    vec3 to_l = PointLightPos[i].xyz - world_pos;
    float dist = length(to_l);
    float radius = max(PointLightPos[i].w, 0.001);
    float atten = clamp(1.0 - dist / radius, 0.0, 1.0);
    atten *= atten;
    float ndotl = max(dot(n, to_l / max(dist, 0.0001)), 0.0);
    accum += PointLightColor[i].xyz * ndotl * atten;
  }
  return accum;
}

uniform vec3 camera;
uniform sampler2DArray texture_samplers[15];
uniform vec3 ambient_color;
// Per-region MFOG (client-canon): WMO geometry fogs with the group's authored MFOG (blended toward
// the zone fog by the volume falloff on the CPU) while terrain keeps the zone fog in the same frame.
// use_wmo_fog stays 0 in paths that never set it (previews) -> falls back to the UBO zone fog.
uniform int use_wmo_fog;
uniform vec3 wmo_fog_color;
uniform float wmo_fog_start; // FRACTION of wmo_fog_end (can be negative: mist scaler)
uniform float wmo_fog_end;
// Fixed-function fog trick (trace-verified): additive batches fog toward BLACK, modulate toward
// WHITE, so distance fog fades their contribution out instead of tinting it. 0 normal, 1 black, 2 white.
uniform int fog_color_mode;
// 1 when this WMO has exterior groups (an "open" WMO, e.g. a cave mouth). Lets outdoor zone light
// bleed into its interior groups; 0 for fully enclosed dungeons (keep their dark authored interior).
uniform int wmo_open;
// Debug: 1 = output the raw fixed-up MOCV vertex colour (magenta where a batch has NO MOCV flag), so we
// can see whether the black doorway-reveal faces actually carry the warm baked colour or lose it.
uniform int debug_mocv;

in vec3 f_position;
in vec3 f_normal;
in vec2 f_texcoord;
in vec2 f_texcoord_2;
in vec4 f_vertex_color;

flat in uint flags;
flat in uint shader;
flat in uint tex_array0;
flat in uint tex_array1;
flat in uint tex0;
flat in uint tex1;
flat in uint alpha_test_mode;

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
  else if (tex_sampler == 14)
  {
    return texture(texture_samplers[14], vec3(tex_coord, array_index)).rgba;
  }

  return vec4(0);
}

vec2 get_tex_size(uint tex_sampler)
{
  if (tex_sampler == 0) { return vec2(textureSize(texture_samplers[0], 0).xy); }
  else if (tex_sampler == 1) { return vec2(textureSize(texture_samplers[1], 0).xy); }
  else if (tex_sampler == 2) { return vec2(textureSize(texture_samplers[2], 0).xy); }
  else if (tex_sampler == 3) { return vec2(textureSize(texture_samplers[3], 0).xy); }
  else if (tex_sampler == 4) { return vec2(textureSize(texture_samplers[4], 0).xy); }
  else if (tex_sampler == 5) { return vec2(textureSize(texture_samplers[5], 0).xy); }
  else if (tex_sampler == 6) { return vec2(textureSize(texture_samplers[6], 0).xy); }
  else if (tex_sampler == 7) { return vec2(textureSize(texture_samplers[7], 0).xy); }
  else if (tex_sampler == 8) { return vec2(textureSize(texture_samplers[8], 0).xy); }
  else if (tex_sampler == 9) { return vec2(textureSize(texture_samplers[9], 0).xy); }
  else if (tex_sampler == 10) { return vec2(textureSize(texture_samplers[10], 0).xy); }
  else if (tex_sampler == 11) { return vec2(textureSize(texture_samplers[11], 0).xy); }
  else if (tex_sampler == 12) { return vec2(textureSize(texture_samplers[12], 0).xy); }
  else if (tex_sampler == 13) { return vec2(textureSize(texture_samplers[13], 0).xy); }
  else if (tex_sampler == 14) { return vec2(textureSize(texture_samplers[14], 0).xy); }
  return vec2(1.0);
}

// MOMT F_CLAMP_S/F_CLAMP_T: the material addresses CLAMP instead of REPEAT on the flagged axis.
// The textures sit in shared array textures whose GL wrap state must stay REPEAT for everyone
// else, so clamp-to-edge is emulated by clamping the coordinate half a texel inside the border.
vec2 clamp_tex_coord(vec2 tex_coord, uint tex_sampler)
{
  vec2 half_texel = 0.5 / get_tex_size(tex_sampler);
  if (bool(flags & eWMOBatch_ClampS))
  {
    tex_coord.x = clamp(tex_coord.x, half_texel.x, 1.0 - half_texel.x);
  }
  if (bool(flags & eWMOBatch_ClampT))
  {
    tex_coord.y = clamp(tex_coord.y, half_texel.y, 1.0 - half_texel.y);
  }
  return tex_coord;
}

vec3 apply_lighting(vec3 material)
{
  // MOCV = baked per-vertex lighting (interior shadow/light). Already processed Blizzard-style on
  // load (CMapObjGroup::FixColorVertexAlpha). For groups without it the contribution is 0.
  vec3 vertex_color = bool(flags & eWMOBatch_HasMOCV) ? f_vertex_color.rgb : vec3(0.);

  vec3 light_color;

  if (bool(flags & eWMOBatch_Unlit))
  {
    // Self-illuminated (F_UNLIT): render at full texture brightness, ignoring scene lighting -- e.g. the
    // glowing lava veins in Molten Core's Blackrock WMO (material 21, bm_brspire_smalllava01, flagged
    // unlit). Lighting these by the (near-black) cave ambient made them read as dull dark rock instead
    // of glowing. Fullbright matches the client; the emissive bloom write in main() adds the halo.
    light_color = vec3(1.0);
  }
  else if (bool(flags & eWMOBatch_ExteriorLit))
  {
    // Exterior geometry: outdoor sun diffuse (N.L) + outdoor ambient (+ any baked color).
    float nDotL = clamp(dot(normalize(f_normal),
                            -normalize(vec3(LightDir_FogRate.x, LightDir_FogRate.z, LightDir_FogRate.y))),
                        0.0, 1.0);
    vec3 lit_diffuse = DiffuseColor_FogStart.xyz;
    vec3 lit_ambient = AmbientColor_FogEnd.xyz;
    if (bool(flags & eWMOBatch_Window))
    {
      // F_WINDOW, CLIENT-EXACT (wow.exe @006d37e0 builds the pair, @006b5190 swaps it around the
      // batch): window materials are lit by diffuse' = ambient' = midpoint(diffuse, ambient), with
      // the ambient additionally lifted +16/255 (saturating byte add) -- glass reads flat (half the
      // directional contrast) and faintly luminous. RE note 28.
      vec3 mid = mix(lit_diffuse, lit_ambient, 0.5);
      lit_diffuse = mid;
      lit_ambient = clamp(mid + vec3(16.0 / 255.0), 0.0, 1.0);
    }
    light_color = clamp(lit_diffuse * nDotL, 0.0, 1.0)
                + lit_ambient
                + vertex_color;
  }
  else
  {
    // Interior geometry: the WMO's own MOHD ambient + the baked MOCV -- the client / reference noggit3
    // formula `ambient_color + vertex_color`. (The removed wmo_open spill, interior_sun and time-of-day
    // MOCV multiply were the real hand-tuned hacks that washed out / crushed Ironforge -- gone for good.)
    //
    // ONE required guard, NOT a look-tune: some WMOs (e.g. the Timbermaw instance) author a near-white
    // (~1,1,1) MOHD ambient as a sentinel meaning "the baked MOCV already carries the full interior
    // light -- add nothing extra". fix_vertex_color_alpha() detects that same near-white case and PRESERVES
    // the baked MOCV (it does NOT subtract the ambient the way it does for a normal WMO). So the shader
    // must match: for near-white ambient, add ~0 (a tiny floor so unlit vertices aren't pure black), not
    // literal white -- otherwise (1,1,1)+MOCV floods every baked shadow flat (bland Timbermaw). Normal
    // WMOs (Ironforge et al.) have a real dark authored ambient and take the plain path unchanged.
    // (Byte-matched client formula tex*MOCV*(1+4a) was tried and REVERTED -- looked worse on our
    // content; see WMO.cpp load_mocv note. Approved formula below.)
    vec3 interior_ambient = all(greaterThan(ambient_color, vec3(0.95))) ? vec3(0.04) : ambient_color;
    light_color = interior_ambient + vertex_color;

    if (bool(flags & eWMOBatch_PortalSpill))
    {
      float openness = clamp(f_vertex_color.a, 0.0, 1.0);
      float nDotL = clamp(dot(normalize(f_normal),
                              -normalize(vec3(LightDir_FogRate.x, LightDir_FogRate.z, LightDir_FogRate.y))),
                          0.0, 1.0);
      vec3 outdoor = clamp(DiffuseColor_FogStart.xyz * nDotL, 0.0, 1.0) + AmbientColor_FogEnd.xyz;
      light_color = mix(light_color, outdoor, openness);
    }
  }

  light_color += point_lights(f_position, normalize(f_normal));

  vec3 lit = material.rgb * light_color;

  // SIDN (Self-Illuminated Day/Night): windows and similar materials emit their own texture colour,
  // ramping up as the outdoor light fades, so they glow at night and stay neutral by day. Faithful to
  // the WMO material sidn flag (CMapObj night glow). Drive the ramp from the outdoor ambient luminance.
  if (bool(flags & eWMOBatch_Sidn))
  {
    float outdoor_lum = dot(AmbientColor_FogEnd.xyz, vec3(0.2126, 0.7152, 0.0722));
    float night = clamp(1.0 - outdoor_lum * 2.0, 0.0, 1.0);
    lit += material.rgb * night;
  }

  return clamp(lit, 0.0, 1.0);
}

void main()
{
  if (debug_mocv == 1)
  {
    vec3 vc = bool(flags & eWMOBatch_HasMOCV) ? f_vertex_color.rgb : vec3(1.0, 0.0, 1.0);
    out_color = vec4(vc, 1.0);
    return;
  }
  if (debug_mocv == 2) // show the portal-openness factor (white=at portal, black=deep/no spill)
  {
    float o = bool(flags & eWMOBatch_PortalSpill) ? clamp(f_vertex_color.a, 0.0, 1.0) : 0.0;
    out_color = vec4(vec3(o), 1.0);
    return;
  }

  float dist_from_camera = distance(camera, f_position);
  bool fog = FogColor_FogOn.w != 0 && !bool(flags & eWMOBatch_Unfogged);

  vec2 tex_coord = f_texcoord;
  vec2 tex_coord_2 = f_texcoord_2;

  if (bool(flags & (eWMOBatch_ClampS | eWMOBatch_ClampT)))
  {
    tex_coord = clamp_tex_coord(tex_coord, tex_array0);
    tex_coord_2 = clamp_tex_coord(tex_coord_2, tex_array1);
  }

  vec4 tex = get_tex_color(tex_coord, tex_array0, int(tex0));
  vec4 tex_2 = get_tex_color(tex_coord_2, tex_array1, int(tex1));

  float alpha_test = !bool(alpha_test_mode) ? -1.f : (alpha_test_mode < 2 ? 0.878431372 : 0.003921568);

  if(tex.a < alpha_test)
  {
    discard;
  }

  vec4 vertex_color = vec4(0., 0., 0., 1.f);
  vec3 light_color = vec3(1.);

  if(bool(flags & eWMOBatch_HasMOCV))
  {
    vertex_color = f_vertex_color;
  }


  // see: https://github.com/Deamon87/WebWowViewerCpp/blob/master/wowViewerLib/src/glsl/wmoShader.glsl
  if(shader == 3) // Env
  {
    vec3 env = tex_2.rgb * tex.rgb;
    out_color = vec4(apply_lighting(tex.rgb) + env, 1.);
  }
  else if(shader == 5) // EnvMetal
  {
    vec3 env = tex_2.rgb * tex.rgb * tex.a;
    out_color = vec4(apply_lighting(tex.rgb) + env, 1.);
  }
  else if(shader == 6) // TwoLayerDiffuse
  {
    vec3 layer2 = mix(tex.rgb, tex_2.rgb, tex_2.a);
    out_color = vec4(apply_lighting(mix(layer2, tex.rgb, vertex_color.a)), 1.);
  }
  else // default shader, used for shader 0,1,2,4 (Diffuse, Specular, Metal, Opaque)
  {
    out_color = vec4(apply_lighting(tex.rgb), 1.);
  }

  float bloom_mask = 1.0; // bloom eligibility written into alpha; reduced by fog (see below)
  if(fog)
  {
    float fog_end_eff = (use_wmo_fog == 1) ? wmo_fog_end : AmbientColor_FogEnd.w;
    float fog_start_frac = (use_wmo_fog == 1) ? wmo_fog_start : DiffuseColor_FogStart.w;
    vec3 fog_color_eff = (use_wmo_fog == 1) ? wmo_fog_color : FogColor_FogOn.rgb;
    if (fog_color_mode == 1)
    {
      fog_color_eff = vec3(0.0);
    }
    else if (fog_color_mode == 2)
    {
      fog_color_eff = vec3(1.0);
    }

    float start = fog_end_eff * fog_start_frac;

    vec3 fogParams;
    fogParams.x = -(1.0 / (fog_end_eff - start));
    fogParams.y = (1.0 / (fog_end_eff - start)) * fog_end_eff;
    fogParams.z = LightDir_FogRate.w;

    float f1 = (dist_from_camera * fogParams.x) + fogParams.y;
    float f2 = max(f1, 0.0);
    float f3 = pow(f2, fogParams.z);
    float f4 = min(f3, 1.0);

    float fogFactor = 1.0 - f4;

    out_color.rgb = mix(out_color.rgb, fog_color_eff, fogFactor);
    bloom_mask = 1.0 - fogFactor; // fog-brightened surfaces opt out of bloom (fully fogged -> 0)
  }

  if(out_color.a < alpha_test)
  {
    discard;
  }

  // Write the bloom mask into alpha (read by the bloom bright-pass) AFTER the alpha-test discard so it
  // can't punch holes in cutouts. Without this, geometry tinted toward a bright fog colour (e.g.
  // distant Stormwind buildings) crosses the low bloom threshold and blooms heavily under fog.
  //
  // Unlit (self-illuminated) WMO materials -- glowing lava veins, runes, light fixtures -- are emissive,
  // so write them into the reserved emissive range (>0.88) so their bright pixels bloom, exactly like
  // lava liquid and M2 unlit-additive glows. The bright-pass's emissive branch still keeps only
  // genuinely bright pixels (lowered threshold on max-channel), so dark parts of an unlit material don't
  // bloom. Normal (lit) WMO surfaces stay capped to 0.85, below the emissive range.
  out_color.a = bool(flags & eWMOBatch_Unlit) ? bloom_mask : (bloom_mask * 0.85);
}
