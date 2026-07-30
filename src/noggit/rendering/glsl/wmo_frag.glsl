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
// The vertex-colour ALPHA is a valid two-layer texture-blend factor (either from a real lighting MOCV or a
// dedicated texture-blend mocv2 whose RGB is 0). Set independently of HasMOCV, which gates LIGHTING.
#define eWMOBatch_HasMOCVBlend 0x400u

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
// [GREENDBG 2026-07-30] temporary bisect for the bright-green patches on EnvMetal WMO surfaces (icebreaker
// ship figurehead / Icecrown spikes). Set NOGGIT_WMO_DEBUG=1..6 to replace out_color with ONE term, so the
// green can be attributed without another rebuild:
//   1 = diffuse texture only      2 = second (env) texture sample only   3 = the env term only
//   4 = lighting only (on white)  5 = vertex colour only                 6 = tex_coord_2 as colour
uniform int wmo_debug_mode;
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
// 1 while the camera is INSIDE a WMO this frame (set from WorldRender::cameraInsideWmo). An exterior-lit or
// portal-spill face of an enclosed city WMO -- Ironforge's building fronts, the tiny gryphon flight tunnels
// -- must NOT be lit by the outdoor map light when viewed from inside: there is no Light.dbc row positioned
// inside a city WMO, so the outdoor fallback floods those faces with bright Dun Morogh daylight (the reported
// bug -- faces that should be dark "look lit from outside"). While inside, route them to the interior formula
// (MOHD ambient + baked MOCV) so they match the surrounding interior. Viewed from OUTSIDE they keep outdoor
// lighting. This ONLY changes the exterior-lit / portal-spill faces; the plain interior look is untouched.
uniform int camera_inside_wmo;
// Debug: 1 = output the raw fixed-up MOCV vertex colour (magenta where a batch has NO MOCV flag), so we
// can see whether the black doorway-reveal faces actually carry the warm baked colour or lose it.
uniform int debug_mocv;

// 3.3.5a WMO interior material shader is mod2x (tex*MOCV*2); 1.12 is x1. Set for non-CLASSIC projects when
// the NOGGIT_335A_WMO_MOD2X toggle is on; pairs with the load-time WotLK FixColorVertexAlpha. 0 = x1 (1.12).
uniform int wmo_interior_mod2x;
// 3.3.5a interior tone knobs (applied to the FINAL interior light when wmo_interior_mod2x is set):
//   wmo_interior_floor = shadow minimum -- no interior face darker than this (kills pure-black voids).
//   wmo_interior_gain  = overall interior brightness multiplier (1.0 = neutral).
// Live-tunable via NOGGIT_335A_INTERIOR_FLOOR / NOGGIT_335A_INTERIOR_GAIN so contrast can be dialled to client.
uniform float wmo_interior_floor;
uniform float wmo_interior_gain;

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
    // CANON (wow.exe FUN_006a7300 exterior branch; LIGHT_FOG_SELECTION_RE.md §9.4): exterior/exterior_lit
    // groups (MOGP flags & 0x48) take the OUTDOOR directional light -- diffuse*max(0,N.L) + ambient -- PER
    // GROUP, with NO camera test. Exterior faces seen from INSIDE Ironforge are dark for free because the
    // zone selection picks the dim Ironforge-interior Light.dbc zone (ID 72/73, param 77 #98461d) at that
    // eye -- not from any camera gate. (If they stay bright, the bug is noggit's zone selection wrongly
    // holding the bright surface ID=16 (#546f84) inside IF -- a light-selection fix, not a shader one.)
    // FRAME FIX (RE 2026-07-26): WMO normals load fixCoordSystem'd = (wow.x, wow.z, -wow.y) (WMO.cpp,
    // same as M2), so the light must NEGATE its 3rd component like m2_frag does -- NOT the terrain
    // swizzle (+y). With +y the West/East term had the wrong sign, mirroring the sun's azimuth across
    // North-South: WMO faces lit from the NE while terrain/M2/client light from the NW. That is why
    // Stormwind's WMO keep/walls lit from the opposite side of the (correct) terrain mountain. Proven
    // byte-exact: a west-facing wall gives N.L = +0.646 (client) only with -y; +y gave -0.646 (dark).
    float nDotL = clamp(dot(normalize(f_normal),
                            -normalize(vec3(LightDir_FogRate.x, LightDir_FogRate.z, -LightDir_FogRate.y))),
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
    // CANON (RE_notes/17 §1b, apitrace): exterior / exterior_lit batches are drawn with MATERIAL
    // diffuse/ambient = WHITE and DIFFUSE/AMBIENTMATERIALSOURCE=MATERIAL, so the vertex colour (MOCV) is
    // IGNORED for lighting (those batches carry a white placeholder MOCV). Exterior faces = pure outdoor
    // sun, tex * saturate(ambient + diffuse*N.L), exactly like terrain -- NO baked-colour add. (noggit's
    // old `+ vertex_color` stacked the buildings' baked colour ON TOP of the sun, which is why Stormwind
    // read too bright and warm vs in-game.)
    light_color = clamp(lit_diffuse * nDotL, 0.0, 1.0)
                + lit_ambient;
  }
  else
  {
    // Interior geometry = tex * MOCV (x1). The byte-exact client combine is tex*MOCV.rgb*(1+4*MOCV.a) HDR
    // (docs/client_re/17), but the (1+4a) overbright BLOWS the doorway reveal + candle/window verts to solid
    // white on our content: noggit renders `tex*MOCV` brighter than the client, so the same >1 factor clamps
    // to white here where it stays sub-1 on the client (a texture-decode/render divergence, the true root --
    // NOT fixable in this shader). Kept x1 * MOCV, stable. No-MOCV faces -> MOHD ambient, floored to 0.04 for
    // the near-white (~1,1,1) "MOCV carries the light" sentinel (Timbermaw) so no-MOCV skywrap doesn't flood.
    // NO doorway spill. GROUND TRUTH (apitrace wow_cap_doorway_portal.trace, 2026-07-27): the client does NOT
    // brighten the reveal at all -- its reveal MOCV alpha is ~0 (92.9% exactly 0) so the `(1+4*a)` term is
    // dormant (x1), the reveal is just `tex * MOCV` (~0.35, measured (108,92,93)), and it is NATURALLY darker
    // than the sunlit exterior because it's a recessed opening. ZERO pixels clip to white in the whole client
    // frame. Every runtime spill I layered on here (toward white, then outdoor, then extended into the room)
    // was ADDING light the client never adds -- the "overtuned/seam" the user saw. Interior = plain `tex*MOCV`,
    // which at alpha~0 is byte-exact to the client. (The (1+4a) overbright only lifts candle/emissive hotspots
    // -- a separate enhancement that needs the raw MOCV alpha plumbed, NOT the portal-openness in .w.)
    vec3 interior_ambient = all(greaterThan(ambient_color, vec3(0.95))) ? vec3(0.04) : ambient_color;
    // 3.3.5a interior combine is mod2x: tex*MOCV*2 (paired with the load-time WotLK FixColorVertexAlpha, which
    // pre-halves so alpha~0 verts net x1 and only higher-alpha verts brighten). 1.12 stays x1. Clamp below
    // caps hotspots. A/B via wmo_interior_mod2x.
    vec3 mocv_light = (wmo_interior_mod2x != 0) ? (vertex_color * 2.0) : vertex_color;
    // Doorway/portal SEAM blend: blend the reveal toward the EXTERIOR lighting of THIS SAME face (same normal):
    // clamp(diffuse*N.L)+ambient, identical to the ExteriorLit branch, so at the opening (openness a->1) a reveal
    // face equals the abutting exterior face of the same orientation -> continuous seam, fading to interior MOCV
    // inward (a->0). Applies to BOTH 1.12 and 3.3.5a (portal-spill faces only) -- the doorway hard-seam is the
    // same in both. This is the `mix` version (bounded), NOT the brighten-only max() (which over-lit whole multi-
    // window rooms with directional streaks -- reverted 2026-07-29, do NOT reintroduce max here). The 335a
    // branch is UNCHANGED by ungating (it already ran this); only 1.12 (mod2x==0) newly gains it.
    if (bool(flags & eWMOBatch_PortalSpill))
    {
      float nDotL_ext = clamp(dot(normalize(f_normal),
                                  -normalize(vec3(LightDir_FogRate.x, LightDir_FogRate.z, -LightDir_FogRate.y))),
                              0.0, 1.0);
      vec3 exterior_light = clamp(DiffuseColor_FogStart.xyz * nDotL_ext, 0.0, 1.0) + AmbientColor_FogEnd.xyz;
      mocv_light = mix(mocv_light, exterior_light, clamp(f_vertex_color.a, 0.0, 1.0));
    }
    light_color = bool(flags & eWMOBatch_HasMOCV) ? mocv_light : interior_ambient;
    // FINAL interior tone (335a): gain brightens the whole interior, then a hard shadow floor guarantees no
    // interior face renders pure BLACK (catches near-0/absent MOCV AND any seam-darkened deep face -- it clamps
    // the FINISHED light). Both flat + brighten-friendly, applied LAST so nothing upstream can leave black.
    if (wmo_interior_mod2x != 0)
    {
      light_color = max(light_color * wmo_interior_gain, vec3(wmo_interior_floor));
    }
  }

  light_color += point_lights(f_position, normalize(f_normal));

  // Clamp the light to [0,1] before the texture modulate (client/WMO clamp-before-texture, §12b harbor fix).
  light_color = clamp(light_color, 0.0, 1.0);

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
  if (debug_mocv == 3) // BRANCH visualiser: which lighting path does each WMO face take? (dimmed = camera outside)
  {
    vec3 c;
    if (bool(flags & eWMOBatch_Unlit))                                      c = vec3(1.0, 1.0, 1.0); // WHITE  = unlit/fullbright
    else if (bool(flags & eWMOBatch_ExteriorLit) && camera_inside_wmo == 0) c = vec3(1.0, 0.0, 0.0); // RED    = ExteriorLit outdoor branch (the bug when inside)
    else if (bool(flags & eWMOBatch_ExteriorLit))                          c = vec3(1.0, 0.5, 0.0); // ORANGE = ExteriorLit face but INSIDE -> now routed to interior (fixed)
    else if (bool(flags & eWMOBatch_PortalSpill))                          c = vec3(0.0, 0.4, 1.0); // BLUE   = interior + portal spill
    else                                                                   c = vec3(0.0, 1.0, 0.0); // GREEN  = plain interior (ambient + MOCV)
    out_color = vec4(c * (camera_inside_wmo != 0 ? 1.0 : 0.5), 1.0);
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

  // Two-layer (shader 6) blend factor rides the vertex-colour ALPHA. That alpha is valid whenever ANY
  // vertex-colour chunk was uploaded -- including modern WMOs that carry ONLY a texture-blend mocv2 (RGB=0)
  // and are therefore NOT flagged HasMOCV for lighting. Gate the blend on its own flag so those groups
  // still blend their two layers instead of collapsing to layer 1; default 1.0 = pure layer 1.
  float blend_alpha = bool(flags & eWMOBatch_HasMOCVBlend) ? f_vertex_color.a : 1.0;

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
    out_color = vec4(apply_lighting(mix(layer2, tex.rgb, blend_alpha)), 1.);
  }
  else // default shader, used for shader 0,1,2,4 (Diffuse, Specular, Metal, Opaque)
  {
    out_color = vec4(apply_lighting(tex.rgb), 1.);
  }

  // [GREENDBG 2026-07-30] temporary: isolate one shading term. Placed BEFORE fog/bloom so the term is
  // shown raw. Alpha test above still applies, so cutouts stay cutouts.
  if (wmo_debug_mode != 0)
  {
    if (wmo_debug_mode == 1) { out_color = vec4(tex.rgb, 1.0); }
    else if (wmo_debug_mode == 2) { out_color = vec4(tex_2.rgb, 1.0); }
    else if (wmo_debug_mode == 3) { out_color = vec4(tex_2.rgb * tex.rgb * tex.a, 1.0); }
    else if (wmo_debug_mode == 4) { out_color = vec4(apply_lighting(vec3(1.0)), 1.0); }
    else if (wmo_debug_mode == 5) { out_color = vec4(f_vertex_color.rgb, 1.0); }
    else if (wmo_debug_mode == 6) { out_color = vec4(fract(abs(tex_coord_2)), 0.0, 1.0); }
    // 7 = IDENTIFY the texture actually bound to the env slot, by its dimensions. The env map for these
    // materials is wr_env.blp at 128x128, so a correct binding is BLUE. Any other colour means slot 1 holds
    // a different texture than the one the batch recorded -> a recycled/stale GL texture name.
    else if (wmo_debug_mode == 7)
    {
      float s = get_tex_size(tex_array1).x;
      if (s == 128.0)      { out_color = vec4(0.0, 0.0, 1.0, 1.0); } // BLUE  = 128 (expected wr_env)
      else if (s == 256.0) { out_color = vec4(1.0, 1.0, 0.0, 1.0); } // YELLOW= 256
      else if (s == 512.0) { out_color = vec4(1.0, 0.0, 0.0, 1.0); } // RED   = 512
      else if (s == 64.0)  { out_color = vec4(0.0, 1.0, 1.0, 1.0); } // CYAN  = 64
      else                 { out_color = vec4(1.0, 1.0, 1.0, 1.0); } // WHITE = anything else / unbound
    }
    // 8 = same identification for the DIFFUSE slot, as a control (ship figurehead is 512 -> RED).
    else if (wmo_debug_mode == 8)
    {
      float s = get_tex_size(tex_array0).x;
      if (s == 128.0)      { out_color = vec4(0.0, 0.0, 1.0, 1.0); }
      else if (s == 256.0) { out_color = vec4(1.0, 1.0, 0.0, 1.0); }
      else if (s == 512.0) { out_color = vec4(1.0, 0.0, 0.0, 1.0); }
      else if (s == 64.0)  { out_color = vec4(0.0, 1.0, 1.0, 1.0); }
      else                 { out_color = vec4(1.0, 1.0, 1.0, 1.0); }
    }
    // 9 = ONLY EnvMetal/Env batches, coloured by the env slot's texture size; everything else BLACK.
    // (Mode 7 was useless here: the ship's ~11 non-env materials read their own 256px diffuse and
    // drowned out the 4 env batches.) BLUE = 128 = the expected wr_env.
    else if (wmo_debug_mode == 9)
    {
      if (shader != 3u && shader != 5u) { out_color = vec4(0.0, 0.0, 0.0, 1.0); }
      else
      {
        float s = get_tex_size(tex_array1).x;
        if (s == 128.0)      { out_color = vec4(0.0, 0.0, 1.0, 1.0); } // BLUE  = correct wr_env
        else if (s == 256.0) { out_color = vec4(1.0, 1.0, 0.0, 1.0); } // YELLOW= wrong (256)
        else if (s == 512.0) { out_color = vec4(1.0, 0.0, 0.0, 1.0); } // RED   = wrong (512)
        else                 { out_color = vec4(1.0, 1.0, 1.0, 1.0); } // WHITE = other/unbound
      }
    }
    // 10 = THE DECIDER: sample the env texture at a FIXED (0.5,0.5) instead of the computed coordinate,
    // on env batches only (others black). wr_env's centre is grey/teal, so:
    //   grey/teal  -> the bound texture is CORRECT and the COORDINATE is what's broken
    //   still green-> the bound texture itself is wrong (a different texture is in that slot)
    else if (wmo_debug_mode == 10)
    {
      if (shader != 3u && shader != 5u) { out_color = vec4(0.0, 0.0, 0.0, 1.0); }
      else { out_color = vec4(get_tex_color(vec2(0.5, 0.5), tex_array1, int(tex1)).rgb, 1.0); }
    }
    // 11 = what SHADER id reaches the fragment stage?  DARKGREY=0 Diffuse, CYAN=3 Env, BLUE=5 EnvMetal,
    // MAGENTA=6 TwoLayer, RED=anything else. If the whole ship is one colour here, the batch data is wrong.
    else if (wmo_debug_mode == 11)
    {
      if (shader == 0u)      { out_color = vec4(0.25, 0.25, 0.25, 1.0); }
      else if (shader == 3u) { out_color = vec4(0.0, 1.0, 1.0, 1.0); }
      else if (shader == 5u) { out_color = vec4(0.0, 0.0, 1.0, 1.0); }
      else if (shader == 6u) { out_color = vec4(1.0, 0.0, 1.0, 1.0); }
      else                   { out_color = vec4(1.0, 0.0, 0.0, 1.0); }
    }
    // 12 = what SLOT index does the batch ask for on the second texture?  BLACK=0, GREEN=1, YELLOW=2,
    // RED=anything else (a slot >= n_used would sample an unbound unit).
    else if (wmo_debug_mode == 12)
    {
      if (tex_array1 == 0u)      { out_color = vec4(0.0, 0.0, 0.0, 1.0); }
      else if (tex_array1 == 1u) { out_color = vec4(0.0, 1.0, 0.0, 1.0); }
      else if (tex_array1 == 2u) { out_color = vec4(1.0, 1.0, 0.0, 1.0); }
      else                       { out_color = vec4(1.0, 0.0, 0.0, 1.0); }
    }
    // 13/14 = sample a sampler DIRECTLY, bypassing get_tex_color's if-chain, at a fixed UV. Alpha 0 so the
    // bloom/emissive post-pass cannot touch the result. 13 = env slot's unit, 14 = diffuse slot's unit.
    else if (wmo_debug_mode == 13)
    {
      out_color = vec4(texture(texture_samplers[1], vec3(0.5, 0.5, 0.0)).rgb, 0.0);
      return;
    }
    else if (wmo_debug_mode == 14)
    {
      out_color = vec4(texture(texture_samplers[0], vec3(0.5, 0.5, 0.0)).rgb, 0.0);
      return;
    }
    // 17/18/19 = direct samples of the other sampler slots at a fixed UV (alpha 0). Sweeping the slots
    // shows WHICH unit actually holds the grey env map, which exposes any off-by-one between the binding
    // loop (slot i -> unit 1+i) and the uniform mapping.
    else if (wmo_debug_mode == 17)
    {
      out_color = vec4(texture(texture_samplers[2], vec3(0.5, 0.5, 0.0)).rgb, 0.0);
      return;
    }
    else if (wmo_debug_mode == 18)
    {
      out_color = vec4(texture(texture_samplers[3], vec3(0.5, 0.5, 0.0)).rgb, 0.0);
      return;
    }
    // 19 = sample the env slot but force the LOWEST mip explicitly. If mip 0 is grey here while mode 13 is
    // green, the higher mip levels are missing/undefined and mip selection is what produces the colour.
    else if (wmo_debug_mode == 19)
    {
      out_color = vec4(textureLod(texture_samplers[1], vec3(0.5, 0.5, 0.0), 0.0).rgb, 0.0);
      return;
    }
    // 15 = FLAT MID-GREY with alpha 1.0 (the emissive/bloom range every other debug mode wrote). If this
    // comes out GREEN, the post-process is producing the colour and every earlier debug reading was
    // contaminated by it -- including the "green" that pointed at the env sample.
    else if (wmo_debug_mode == 15)
    {
      out_color = vec4(0.5, 0.5, 0.5, 1.0);
      return;
    }
    // 16 = the same flat mid-grey but alpha 0.0 (bloom-excluded), as the control for 15.
    else if (wmo_debug_mode == 16)
    {
      out_color = vec4(0.5, 0.5, 0.5, 0.0);
      return;
    }
    out_color.a = 1.0;
    return;
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
