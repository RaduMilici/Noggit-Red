// [VULKAN phase D slice 1, 2026-08-30] WMO group fragment stage -- GEOMETRY + baked MOCV only.
//
// Deliberately the first slice: this validates that the arena, the per-instance transforms and the
// replayed GL runs put WMO geometry in the right place on screen. It does NOT yet sample the group
// textures, so the parity NUMBER is expected to stay poor until slice 2 -- what this slice must move
// is the COVERAGE (vkMissing in the classification image), not the colour.
//
// STILL GL-ONLY (open TODO, phase D is not done while any remain): MOMT textures + the WMO combiner
// set, MOHD ambient / interior vs exterior lighting, MOLT per-room point lights, per-group fog,
// two-layer blend alphas, exterior liquid.
#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv0;
layout(location = 3) in vec2 v_uv1;
layout(location = 4) in vec4 v_color;
layout(location = 5) flat in uint v_batch_id;

layout(location = 0) out vec4 out_color;
layout(location = 1) out float out_z;   // depth-as-colour (the R32F attachment GL imports)

layout(push_constant) uniform Push
{
  mat4 mvp;
  float time;
  float terms;
  float slice;
  int xform_index;
  float wmo_interior_gain;
  float wmo_interior_floor;
  float wmo_debug_mode;
  float wmo_interior_mod2x;
} pc;

layout(set = 1, binding = 5) uniform sampler2D tilesets[];   // shared bindless array

// One record per arena batch: x = tex0 bindless id, y = tex1 bindless id, z = MOMT shader, w = flags.
// Indexed by the per-vertex batch id, exactly like GL indexes its batch record texture buffer.
layout(std430, set = 2, binding = 1) readonly buffer WmoBatches { ivec4 wmo_batches[]; };
// MOHD ambient per visible instance, indexed by the same push-constant xform_index
// 3 vec4 per instance: [0] = MOHD ambient rgb + fog mode (0 = zone fog, else 1 + start_frac),
// [1] = WMO fog colour rgb + fog end, [2].x = unified (MOHD 0x2) render path.
layout(std430, set = 2, binding = 2) readonly buffer WmoAmbients { vec4 wmo_ambients[]; };

// wmo_frag.glsl batch flags
#define eWMOBatch_ExteriorLit 0x1u
#define eWMOBatch_HasMOCV     0x2u
#define eWMOBatch_Unlit       0x4u
#define eWMOBatch_Unfogged    0x8u
#define eWMOBatch_Window      0x200u
#define eWMOBatch_NoEnvTexture 0x800u
#define eWMOBatch_ClampS      0x80u
#define eWMOBatch_ClampT      0x100u
#define eWMOBatch_PortalSpill 0x40u
#define eWMOBatch_Sidn        0x20u

layout(std140, set = 1, binding = 3) uniform Lighting
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
  vec4 EnvFogColor_On;
  vec4 EnvFogDist;
  mat4 ShadowMatrix;
  vec4 ShadowParams;
  vec4 ShadowCenterRange;
  mat4 ShadowMatrixEnv;
  vec4 ShadowEnvCenterRange;
  vec4 Camera_Pad;
  vec4 SunSpec_Pad;
  vec4 Toggles;
};

// WMO point lights use the LINEAR-RADIUS model over all active lights -- deliberately different from
// the M2 pass, which uses the client's 1/(0.7d + 0.03d^2) falloff over the 3 nearest only.
vec3 wmo_point_lights(vec3 world_pos, vec3 n)
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

layout(constant_id = 0) const uint ALPHA_TEST = 1u;

void main()
{
  ivec4 batch = wmo_batches[v_batch_id];
  uint flags = uint(batch.w) & 0xFFFFu;
  int  shader_id = batch.z;
  uint alpha_test_mode = uint(batch.w) >> 16;

  // ClampS/ClampT materials clamp to half a texel inside the edge (wmo_frag clamp_tex_coord);
  // without it these sample across the wrap and show the opposite edge of the texture.
  vec2 uv0 = v_uv0;
  vec2 uv1 = v_uv1;
  if ((flags & (eWMOBatch_ClampS | eWMOBatch_ClampT)) != 0u)
  {
    if (batch.x >= 0)
    {
      vec2 ht = 0.5 / vec2(textureSize(tilesets[nonuniformEXT(batch.x)], 0));
      if ((flags & eWMOBatch_ClampS) != 0u) uv0.x = clamp(uv0.x, ht.x, 1.0 - ht.x);
      if ((flags & eWMOBatch_ClampT) != 0u) uv0.y = clamp(uv0.y, ht.y, 1.0 - ht.y);
    }
    if (batch.y >= 0)
    {
      vec2 ht2 = 0.5 / vec2(textureSize(tilesets[nonuniformEXT(batch.y)], 0));
      if ((flags & eWMOBatch_ClampS) != 0u) uv1.x = clamp(uv1.x, ht2.x, 1.0 - ht2.x);
      if ((flags & eWMOBatch_ClampT) != 0u) uv1.y = clamp(uv1.y, ht2.y, 1.0 - ht2.y);
    }
  }

  vec4 tex = batch.x >= 0 ? texture(tilesets[nonuniformEXT(batch.x)], uv0) : vec4(1.0);
  vec4 tex_2 = batch.y >= 0 ? texture(tilesets[nonuniformEXT(batch.y)], uv1) : vec4(0.0);

  // No environment map authored -> the ADDITIVE env term must contribute NOTHING.
  if ((flags & eWMOBatch_NoEnvTexture) != 0u)
    tex_2 = vec4(0.0);

  // wmo_frag.glsl: mode 0 = off, 1 = 224/255, >=2 = 1/255
  float alpha_test = (alpha_test_mode == 0u) ? -1.0
                   : (alpha_test_mode < 2u ? 0.878431372 : 0.003921568);
  // A `discard` anywhere in a fragment shader disables EARLY-Z for the whole pipeline on
  // essentially all hardware -- so opaque WMO walls and floors were being shaded with full
  // overdraw and late-Z. Indoor WMOs are exactly where overdraw is worst, and this pass measured
  // 2.66 ms of VK's 5.0 ms GPU time. ALPHA_TEST is a specialization constant: the opaque variant
  // (blend_mode 0) compiles the discard out entirely and gets early-Z back. Alpha-KEY batches
  // (blend_mode 1) still need it and keep the discarding variant.
  if (ALPHA_TEST != 0u)
  {
    if (tex.a < alpha_test)
      discard;
  }

  vec3 color = tex.rgb;

  // ---- apply_lighting, ported from wmo_frag.glsl ----
  vec3 n = normalize(v_normal);
  vec3 light_color;

  if ((flags & eWMOBatch_Unlit) != 0u)
  {
    light_color = vec3(1.0);   // self-illuminated (F_UNLIT)
  }
  else if ((flags & eWMOBatch_ExteriorLit) != 0u)
  {
    // NOTE the NEGATED third light component: WMO normals load fixCoordSystem'd like M2's, so the
    // terrain swizzle (+y) mirrors the sun across north-south and lights buildings from the wrong side.
    float nDotL = clamp(dot(n, -normalize(vec3(LightDir_FogRate.x, LightDir_FogRate.z, -LightDir_FogRate.y))), 0.0, 1.0);
    vec3 lit_diffuse = DiffuseColor_FogStart.xyz;
    vec3 lit_ambient = AmbientColor_FogEnd.xyz;
    if ((flags & eWMOBatch_Window) != 0u)
    {
      // F_WINDOW: diffuse' = ambient' = midpoint, ambient lifted +16/255 -- glass reads flat and faint
      vec3 mid = mix(lit_diffuse, lit_ambient, 0.5);
      lit_diffuse = mid;
      lit_ambient = clamp(mid + vec3(16.0 / 255.0), 0.0, 1.0);
    }
    // UNIFIED (MOHD 0x2) exterior: the MapObjU shaders add the baked MOCV here too, and the
    // sun term is clamped as a whole before the add.
    if (wmo_ambients[pc.xform_index * 3 + 2].x != 0.0 && (flags & eWMOBatch_HasMOCV) != 0u)
      light_color = clamp(lit_diffuse * nDotL + lit_ambient, 0.0, 1.0) + v_color.rgb * 2.0;
    else
      light_color = clamp(lit_diffuse * nDotL, 0.0, 1.0) + lit_ambient;
  }
  else
  {
    // interior: MOHD ambient, or the baked MOCV mod2x when this batch carries vertex colours
    vec3 amb = wmo_ambients[pc.xform_index * 3].rgb;
    vec3 interior_ambient = all(greaterThan(amb, vec3(0.95))) ? vec3(0.04) : amb;
    // 3.3.5a interior combine is mod2x (tex*MOCV*2); 1.12 stays x1. GL gates this on the same flag.
    bool mod2x = pc.wmo_interior_mod2x != 0.0;
    vec3 mocv_light = mod2x ? (v_color.rgb * 2.0) : v_color.rgb;

    // Doorway/portal SEAM blend: blend the reveal toward the EXTERIOR lighting of the SAME face, so
    // at the opening (openness -> 1) a reveal face matches the abutting exterior face and the seam is
    // continuous, fading back to interior MOCV inward. Bounded mix -- never a brighten-only max().
    if ((flags & eWMOBatch_PortalSpill) != 0u)
    {
      float nDotL_ext = clamp(dot(n, -normalize(vec3(LightDir_FogRate.x, LightDir_FogRate.z, -LightDir_FogRate.y))), 0.0, 1.0);
      vec3 exterior_light = clamp(DiffuseColor_FogStart.xyz * nDotL_ext, 0.0, 1.0) + AmbientColor_FogEnd.xyz;
      mocv_light = mix(mocv_light, exterior_light, clamp(v_color.a, 0.0, 1.0));
    }

    light_color = ((flags & eWMOBatch_HasMOCV) != 0u) ? mocv_light : interior_ambient;
    bool unified = wmo_ambients[pc.xform_index * 3 + 2].x != 0.0;

    // FINAL interior tone: gain, then a HUE-PRESERVING shadow floor (scale the whole colour until
    // its brightest channel reaches the floor -- a per-channel max would shift hue and produce the
    // rainbow blotches in shadow). Identity when the brightest channel is already above the floor.
    if (mod2x && !unified)   // GL applies gain + floor only on this path
    {
      vec3 lc = light_color * pc.wmo_interior_gain;
      float mx = max(max(lc.r, lc.g), lc.b);
      light_color = (mx >= pc.wmo_interior_floor) ? lc
                  : ((mx > 1e-5) ? lc * (pc.wmo_interior_floor / mx)
                                 : vec3(pc.wmo_interior_floor));
    }
    if (unified)
    {
      // ADDITIVE interior, client-exact for the MOHD 0x2 WMOs: MOHD ambient + baked MOCV.
      light_color = amb + (((flags & eWMOBatch_HasMOCV) != 0u) ? mocv_light : vec3(0.0));
    }
  }

  // Point lights reach WMO surfaces too -- omitting this left every lit interior/exterior face darker
  // than GL (measured VK/GL ratio 0.60-0.87 on the failing pixels).
  light_color += wmo_point_lights(v_world, n);

  light_color = clamp(light_color, 0.0, 1.0);

  // MOMT combiners (wmo_frag.glsl): Env and EnvMetal ADD an environment term on top of the lit
  // material; TwoLayerDiffuse blends the two layers by the vertex-colour alpha before lighting.
  if (shader_id == 3)          // Env
  {
    color = clamp(tex.rgb * light_color, 0.0, 1.0) + tex_2.rgb * tex.rgb;
  }
  else if (shader_id == 5)     // EnvMetal
  {
    color = clamp(tex.rgb * light_color, 0.0, 1.0) + tex_2.rgb * tex.rgb * tex.a;
  }
  else if (shader_id == 6)     // TwoLayerDiffuse
  {
    float blend_alpha = v_color.a;
    vec3 layer2 = mix(tex.rgb, tex_2.rgb, tex_2.a);
    color = clamp(mix(layer2, tex.rgb, blend_alpha) * light_color, 0.0, 1.0);
  }
  else                          // 0,1,2,4 Diffuse / Specular / Metal / Opaque
  {
    color = clamp(tex.rgb * light_color, 0.0, 1.0);
  }
  // SIDN (Self-Illuminated Day/Night): window materials emit their own texture colour, ramping up as
  // the outdoor light fades, so they glow at night and stay neutral by day.
  if ((flags & eWMOBatch_Sidn) != 0u)
  {
    float outdoor_lum = dot(AmbientColor_FogEnd.xyz, vec3(0.2126, 0.7152, 0.0722));
    float night = clamp(1.0 - outdoor_lum * 2.0, 0.0, 1.0);
    color += tex.rgb * night;
  }

  color = clamp(color, 0.0, 1.0);

  if (Toggles.y != 0.0 && FogColor_FogOn.w != 0.0 && (flags & eWMOBatch_Unfogged) == 0u)
  {
    // Per-instance MFOG context when the camera is inside this WMO's fog sphere, else zone fog.
    vec4 fog_mode = wmo_ambients[pc.xform_index * 3];
    vec4 fog_ctx  = wmo_ambients[pc.xform_index * 3 + 1];
    bool use_wmo_fog = fog_mode.w != 0.0;

    float fog_end = use_wmo_fog ? fog_ctx.w : AmbientColor_FogEnd.w;
    float start = use_wmo_fog ? (fog_ctx.w * (fog_mode.w - 1.0))
                              : (DiffuseColor_FogStart.w * fog_end);
    vec3 fog_rgb = use_wmo_fog ? fog_ctx.rgb : FogColor_FogOn.rgb;
    float dist = distance(Camera_Pad.xyz, v_world);
    float f1 = (dist * (-(1.0 / (fog_end - start)))) + ((1.0 / (fog_end - start)) * fog_end);
    float f3 = pow(clamp(f1, 0.0, 1.0), LightDir_FogRate.w);
    color = mix(color, fog_rgb, 1.0 - clamp(f3, 0.0, 1.0));
  }

  // [term isolation] same modes as GL's wmo_debug_mode, so both backends can be asked for one term
  int dbg = int(pc.wmo_debug_mode);
  // 103 = branch visualiser, identical palette to GL's debug_mocv==3, so a colour mismatch means the
  // two backends chose DIFFERENT lighting branches for that fragment.
  if (dbg == 103)
  {
    vec3 c;
    if ((flags & eWMOBatch_Unlit) != 0u)             c = vec3(1.0, 1.0, 1.0);
    else if ((flags & eWMOBatch_ExteriorLit) != 0u)  c = vec3(1.0, 0.0, 0.0);   // camera outside
    else if ((flags & eWMOBatch_PortalSpill) != 0u)  c = vec3(0.0, 0.4, 1.0);
    else                                             c = vec3(0.0, 1.0, 0.0);
    out_color = vec4(c * 0.5, 1.0);   // GL dims by 0.5 when the camera is outside the WMO
    out_z = gl_FragCoord.z;
    return;
  }
  if (dbg == 1)      { color = tex.rgb; }
  else if (dbg == 2) { color = tex_2.rgb; }
  else if (dbg == 3) { color = tex_2.rgb * tex.rgb * tex.a; }
  else if (dbg == 4) { color = clamp(light_color, 0.0, 1.0); }
  else if (dbg == 5) { color = v_color.rgb; }
  // 8 = VK-only probe: the per-fragment BATCH INDEX as a colour. If a whole building comes out one
  // flat colour the per-vertex mapping never made it across; healthy output is patchwork per material.
  else if (dbg == 8)
  {
    uint b = uint(v_batch_id);
    color = vec3(float(b % 7u) / 6.0, float((b / 7u) % 5u) / 4.0, float((b / 35u) % 3u) / 2.0);
  }
  // 9 = the resolved tex0 bindless id, same encoding -- distinguishes "wrong batch" from "wrong texture"
  else if (dbg == 9)
  {
    uint t = uint(max(batch.x, 0));
    color = vec3(float(t % 7u) / 6.0, float((t / 7u) % 5u) / 4.0, float((t / 35u) % 3u) / 2.0);
  }

  out_color = vec4(color, 1.0);
  out_z = gl_FragCoord.z;
}
