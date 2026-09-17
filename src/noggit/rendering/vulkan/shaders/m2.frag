// [VULKAN phase C slice 3, 2026-08-29] M2 / doodad BATCHED fragment stage -- a FAITHFUL port of
// the GL m2_frag.glsl batched path, not a slice any more:
//   * per-blend-mode alpha test + fog mode (the pipeline supplies the matching blend state)
//   * the full Combiners_* pixel-shader table (0..22), verbatim from m2_frag
//   * the client M2 sun light (FLAT ambient + single directional; note the NEGATED third light
//     component -- M2 normals load fixCoordSystem'd, terrain MCNR normals do not)
//   * the client M2 point-light model: 1/(0.7d + 0.03d^2), 3 NEAREST lights only
//   * interior lighting from the baked MOCV floor colour + doorway spill
//   * fog to black for additive passes, bloom-mask alpha, Add premultiply
// STILL GL-ONLY (open TODO, tracked): tu_lookup UV selection, animated UV
// matrices, ground clutter (detail_doodad), water_surface_effect / creature_bloom geosets,
// dynamic editor shadows on doodads, billboards, particles, ribbons.
#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv0;
layout(location = 3) in vec2 v_uv1;
layout(location = 4) flat in ivec4 v_tex;
layout(location = 5) in vec4 v_interior;
layout(location = 6) flat in ivec4 v_state;   // blend_mode, flags, pixel_shader, tu lookups

layout(location = 0) out vec4 out_color;
layout(location = 1) out float out_z;   // depth-as-colour (the R32F attachment GL imports)

layout(push_constant) uniform Push
{
  mat4 mvp;
  float time;
  float terms;
  float slice;   // object cull distance; fragments beyond it are discarded (0 = off)
} pc;

layout(set = 1, binding = 5) uniform sampler2D tilesets[];   // shared bindless array (terrain + M2)
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
  vec4 SheenDir_Pad;       // celestial disc direction (unused here, keeps the block aligned)
  vec4 CamFwd_DetailDist;  // xyz = camera forward, w = groundEffectDist (ground clutter fade)
};

// CLIENT point-light model for M2s (RE_notes/15, ported verbatim from m2_frag.glsl):
// falloff 1/(0.7d + 0.03d^2) hardcoded, and AT MOST the 3 NEAREST lights reach a model.
// (The MOLT skip is for UNITS only; batched doodads keep MOLT, so skip_molt == false here.)
vec3 point_lights(vec3 world_pos, vec3 n)
{
  int count = int(PointLightParams.x);
  int   best_i[3] = int[3](-1, -1, -1);
  float best_d[3] = float[3](1e30, 1e30, 1e30);
  for (int i = 0; i < count; ++i)
  {
    float dist = length(PointLightPos[i].xyz - world_pos);
    if (dist < best_d[0])      { best_d[2]=best_d[1]; best_i[2]=best_i[1]; best_d[1]=best_d[0]; best_i[1]=best_i[0]; best_d[0]=dist; best_i[0]=i; }
    else if (dist < best_d[1]) { best_d[2]=best_d[1]; best_i[2]=best_i[1]; best_d[1]=dist; best_i[1]=i; }
    else if (dist < best_d[2]) { best_d[2]=dist; best_i[2]=i; }
  }
  vec3 accum = vec3(0.0);
  for (int k = 0; k < 3; ++k)
  {
    if (best_i[k] < 0) break;
    vec3 to_l = PointLightPos[best_i[k]].xyz - world_pos;
    float dist = best_d[k];
    float atten = 1.0 / max(0.7 * dist + 0.03 * dist * dist, 0.05);
    float ndotl = max(dot(n, to_l / max(dist, 0.0001)), 0.0);
    accum += PointLightColor[best_i[k]].xyz * ndotl * atten;
  }
  return accum;
}

// [parity bisect] see the header: bit0 lighting, bit2 fog, bit4 force-exterior, bit5 kill point lights
bool term_on(int bit) { return (int(pc.terms) & (1 << bit)) != 0; }

vec4 fetch(int id, vec2 uv)
{
  return id >= 0 ? texture(tilesets[nonuniformEXT(id)], uv) : vec4(1.0);
}

void main()
{
  // [parity bisect] bit6 (64): FOOTPRINT probe -- paint every rasterised M2 fragment magenta with no
  // discards, so "VK drew nothing" and "VK drew it wrong" can be told apart.
  if (term_on(6))
  {
    out_color = vec4(1.0, 0.0, 1.0, 1.0);
    out_z = gl_FragCoord.z;
    return;
  }

  // Per-pixel distance slice (m2_frag: the object-cull boundary) -- a model straddling the boundary
  // loses its far pixels first, so it slices out exactly like terrain at the far plane. Without this
  // VK painted far doodads over pixels GL leaves empty.
  if (pc.slice > 0.0 && distance(Camera_Pad.xyz, v_world) > pc.slice)
    discard;

  int blend_mode   = v_state.x;
  int flags        = v_state.y;   // 1 unlit | 2 unfogged | 4 classic_alpha | 8 backface_cull
  int pixel_shader = v_state.z;
  bool unlit    = (flags & 1) != 0;
  bool detail_doodad = (flags & 16) != 0;   // ground clutter: its own shading and cutout law
  bool unfogged = (flags & 2) != 0;

  // The batched GL path sets mesh_color and the anim tex matrices to identity, so keep the literal
  // here: the combiners below then read EXACTLY like m2_frag.glsl and stay diffable against it.
  const vec4 mesh_color = vec4(1.0);

  // ---- per-blend-mode alpha test (m2_frag switch(blend_mode)); the pipeline carries the blend state
  float alpha_test;
  if (blend_mode == 0)      { alpha_test = -1.0; }                                  // Opaque
  else if (blend_mode == 1) { alpha_test = (224.0 / 255.0) * mesh_color.w; }        // Alpha_Key
  else                      { alpha_test = (1.0 / 255.0) * mesh_color.w; }          // Alpha/Add/Mod...

  if (mesh_color.a < alpha_test) discard;

  // Per-unit UV CLAMP (m2_frag clamp_uv): alpha-tested cards -- grass blades, tree leaves -- are
  // authored to clamp at the texture edge. Sampling them unclamped differs on precisely the edge
  // texels of every cutout, in both directions, which is invisible in the mean and loud in a
  // per-pixel count. Masks ride bits 16-19 of the state word.
  int clamp0 = (v_state.w >> 16) & 0x3;
  int clamp1 = (v_state.w >> 18) & 0x3;
  // tu_lookup selects WHICH authored UV set feeds each texture unit (m2_vert get_texture_uv):
  //   1 = texcoord1, 2 = texcoord2, 3+ = none. 0 is the SPHERE MAP (env shine), which needs the view
  //   basis and is counted rather than approximated -- see the sphere-map note in the port doc.
  int tu0 = v_state.w & 0xFF;
  int tu1 = (v_state.w >> 8) & 0xFF;
  vec2 cuv1 = (tu0 == 2) ? v_uv1 : ((tu0 >= 3) ? vec2(0.0) : v_uv0);
  vec2 cuv2 = (tu1 == 1) ? v_uv0 : ((tu1 >= 3) ? vec2(0.0) : v_uv1);
  if (clamp0 != 0 && v_tex.x >= 0)
  {
    vec2 half_texel = 0.5 / vec2(textureSize(tilesets[nonuniformEXT(v_tex.x)], 0));
    if ((clamp0 & 1) != 0) cuv1.x = clamp(cuv1.x, half_texel.x, 1.0 - half_texel.x);
    if ((clamp0 & 2) != 0) cuv1.y = clamp(cuv1.y, half_texel.y, 1.0 - half_texel.y);
  }
  if (clamp1 != 0 && v_tex.y >= 0)
  {
    vec2 half_texel = 0.5 / vec2(textureSize(tilesets[nonuniformEXT(v_tex.y)], 0));
    if ((clamp1 & 1) != 0) cuv2.x = clamp(cuv2.x, half_texel.x, 1.0 - half_texel.x);
    if ((clamp1 & 2) != 0) cuv2.y = clamp(cuv2.y, half_texel.y, 1.0 - half_texel.y);
  }
  vec4 color = vec4(0.0);

  // ---- Combiners_* (wowdev.wiki M2/Rendering#Pixel_Shaders), verbatim from m2_frag.glsl ----
  vec4 texture1 = fetch(v_tex.x, cuv1);
  vec4 texture2 = fetch(v_tex.y, cuv2);

  if (pixel_shader == 0) // Combiners_Opaque
  {
    color.rgb = texture1.rgb * mesh_color.rgb;
    color.a = mesh_color.a;
  }
  else if (pixel_shader == 1) // Combiners_Decal
  {
    color.rgb = mix(mesh_color.rgb, texture1.rgb, mesh_color.a);
    color.a = mesh_color.a;
  }
  else if (pixel_shader == 2) // Combiners_Add
  {
    color.rgba = texture1.rgba + mesh_color.rgba;
  }
  else if (pixel_shader == 3) // Combiners_Mod2x
  {
    color.rgb = texture1.rgb * mesh_color.rgb * vec3(2.0);
    color.a = texture1.a * mesh_color.a * 2.0;
  }
  else if (pixel_shader == 4) // Combiners_Fade
  {
    color.rgb = mix(texture1.rgb, mesh_color.rgb, mesh_color.a);
    color.a = mesh_color.a;
  }
  else if (pixel_shader == 5) // Combiners_Mod
  {
    color.rgba = texture1.rgba * mesh_color.rgba;
  }
  else if (pixel_shader == 6) // Combiners_Opaque_Opaque
  {
    color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb;
    color.a = mesh_color.a;
  }
  else if (pixel_shader == 7) // Combiners_Opaque_Add
  {
    color.rgb = texture2.rgb + texture1.rgb * mesh_color.rgb;
    color.a = mesh_color.a + texture1.a;
  }
  else if (pixel_shader == 8) // Combiners_Opaque_Mod2x
  {
    color.rgb = texture1.rgb * mesh_color.rgb * texture2.rgb * vec3(2.0);
    color.a  = texture2.a * mesh_color.a * 2.0;
  }
  else if (pixel_shader == 9) // Combiners_Opaque_Mod2xNA
  {
    color.rgb = texture1.rgb * mesh_color.rgb * texture2.rgb * vec3(2.0);
    color.a  = mesh_color.a;
  }
  else if (pixel_shader == 10) // Combiners_Opaque_AddNA
  {
    color.rgb = texture2.rgb + texture1.rgb * mesh_color.rgb;
    color.a = mesh_color.a;
  }
  else if (pixel_shader == 11) // Combiners_Opaque_Mod
  {
    color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb;
    color.a = texture2.a * mesh_color.a;
  }
  else if (pixel_shader == 12) // Combiners_Mod_Opaque
  {
    color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb;
    color.a = texture1.a;
  }
  else if (pixel_shader == 13) // Combiners_Mod_Add
  {
    color.rgba = texture2.rgba + texture1.rgba * mesh_color.rgba;
  }
  else if (pixel_shader == 14) // Combiners_Mod_Mod2x
  {
    color.rgba = texture1.rgba * texture2.rgba * mesh_color.rgba * vec4(2.0);
  }
  else if (pixel_shader == 15) // Combiners_Mod_Mod2xNA
  {
    color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb * vec3(2.0);
    color.a = texture1.a * mesh_color.a;
  }
  else if (pixel_shader == 16) // Combiners_Mod_AddNA
  {
    color.rgb = texture2.rgb + texture1.rgb * mesh_color.rgb;
    color.a = texture1.a * mesh_color.a;
  }
  else if (pixel_shader == 17) // Combiners_Mod_Mod
  {
    color.rgba = texture1.rgba * texture2.rgba * mesh_color.rgba;
  }
  else if (pixel_shader == 18) // Combiners_Add_Mod
  {
    color.rgb = (texture1.rgb + mesh_color.rgb) * texture2.a;
    color.a = (texture1.a + mesh_color.a) * texture2.a;
  }
  else if (pixel_shader == 19) // Combiners_Mod2x_Mod2x
  {
    color.rgba = texture1.rgba * texture2.rgba * mesh_color.rgba * vec4(4.0);
  }
  else if (pixel_shader == 20) // Combiners_Opaque_Mod2xNA_Alpha
  {
    color.rgb = (mesh_color.rgb * texture1.rgb) * mix(texture2.rgb * 2.0, vec3(1.0), texture1.a);
    color.a = mesh_color.a;
  }
  else if (pixel_shader == 21) // Combiners_Opaque_AddAlpha (masked_additive == 0 in the batched path)
  {
    color.rgb = (mesh_color.rgb * texture1.rgb) + (texture2.rgb * texture2.a);
    color.a = mesh_color.a;
  }
  else if (pixel_shader == 22) // Combiners_Opaque_AddAlpha_Alpha (masked_additive == 0)
  {
    color.rgb = (mesh_color.rgb * texture1.rgb) + (texture2.rgb * texture2.a * texture1.a);
    color.a = mesh_color.a;
  }
  else
  {
    color.rgb = texture1.rgb * mesh_color.rgb;
    color.a = texture1.a * mesh_color.a;
  }

  // GROUND CLUTTER cutout -- its own law (m2_frag detail_doodad branch), NOT the generic alpha test.
  // The 3.3.5a grass key is the detailDoodadAlpha CVar (default 128/255), CONSTANT with distance,
  // softened by mip level so far mips do not dissolve into sparse dots. The final alpha is
  // tex.a * fade, the fade being the client's per-vertex view-depth ramp over the last 15% of
  // groundEffectDist (DetailDoodad.bls c9); coverage carries the true alpha so half-alpha blade
  // fringes stay translucent instead of saturating to full coverage.
  float grass_coverage = 1.0;
  if (detail_doodad)
  {
    float detail_dist = CamFwd_DetailDist.w;
    float view_z = dot(v_world - Camera_Pad.xyz, CamFwd_DetailDist.xyz);
    float fade = (detail_dist > 0.0)
      ? clamp((detail_dist - view_z) / (0.15 * detail_dist), 0.0, 1.0)
      : 1.0;
    float mesh_fade = clamp(mesh_color.a, 0.0, 1.0);
    float blade = color.a / max(mesh_fade, 1e-4);
    // GL's own isotropic LOD, NOT textureQueryLod: the hardware query folds in the sampler's
    // anisotropic filtering and mip bias and returns a LOWER level, which shifts the cutoff and
    // shuffles which fringe texels survive.
    vec2 tex_dim = v_tex.x >= 0 ? vec2(textureSize(tilesets[nonuniformEXT(v_tex.x)], 0)) : vec2(1.0);
    vec2 lod_dx = dFdx(cuv1 * tex_dim);
    vec2 lod_dy = dFdy(cuv1 * tex_dim);
    float lod = 0.5 * log2(max(max(dot(lod_dx, lod_dx), dot(lod_dy, lod_dy)), 1e-8));
    float far_ref = 0.501961 * clamp(1.0 - lod * 0.22, 0.15, 1.0);
    float a_final = blade * fade;
    if (a_final < far_ref) discard;
    float w = max(fwidth(a_final), 1e-4);
    grass_coverage = min(clamp((a_final - far_ref) / w + 0.5, 0.0, 1.0), a_final);
  }
  // the real cutout: THIS one tests the TEXTURE alpha (m2_frag: else if (color.a < alpha_test) discard)
  else if (color.a < alpha_test) discard;

  // ---- world lighting ----
  vec3 norm = normalize(v_normal);
  vec3 currColor;
  vec3 lDiffuse = vec3(0.0);

  if (detail_doodad)
  {
    // GROUND CLUTTER -- the 1.12 client law (m2_frag's detail_doodad branch). The blade never runs
    // the M2 sun: its colour is the zone ambient + directional scaled into a fixed [0.25, 0.75]
    // shade band, times the RAW MCCV tint baked per blade. That band is what keeps client grass sunk
    // into shadowed terrain instead of glowing over it.
    //
    // Packed per-instance attribute (MapChunk::computeDetailDoodads):
    //   x = mccv_r*256 + mccv_g,  y = mccv_b*2 + shadowBit + 1024 (the "packed" marker),
    //   z,w = ground normal x,z. Unpacked paths send 0 -> neutral white, unshadowed.
    vec3 blade_tint = vec3(1.0);
    float blade_shadow = 1.0;
    if (v_interior.y >= 1024.0)
    {
      float pr = floor(v_interior.x / 256.0);
      float pg = v_interior.x - pr * 256.0;
      float py = v_interior.y - 1024.0;
      float pb = floor(py / 2.0);
      blade_shadow = py - pb * 2.0;
      blade_tint = vec3(pr, pg, pb) / 255.0;
    }
    // [2026-09-02] SUNLIT brightness lowered 0.75 -> 0.55 on request; shadowed blades keep 0.25.
    // Look tweak, not a client-matched value. Must stay in step with the GL copy in m2_frag.glsl.
    float shade = mix(0.25, 0.55, blade_shadow);
    currColor = min(AmbientColor_FogEnd.xyz + DiffuseColor_FogStart.xyz * shade, vec3(1.0)) * blade_tint;
    lDiffuse = vec3(0.0);
  }
  else if (!unlit)
  {
    if (v_interior.a > 0.25 && !term_on(4))
    {
      // interior: baked MOCV floor colour C -- ambient capped at 96/255, diffuse boosted to >= 168/255
      // along the fixed interior light vector, lerped toward the outdoor light by the doorway spill.
      vec3 C = v_interior.rgb;
      float mc = max(C.r, max(C.g, C.b));
      vec3 interior_ambient = C * min(1.0, 0.376471 / max(mc, 0.003922));
      vec3 interior_diffuse = C * max(1.0, 0.658824 / max(mc, 0.003922));
      vec3 L = normalize(vec3(0.30822, 0.9, -0.30822));
      float spill = clamp((v_interior.a - 0.5) * 2.0, 0.0, 1.0);
      interior_ambient = mix(interior_ambient, AmbientColor_FogEnd.xyz, spill);
      interior_diffuse = mix(interior_diffuse, DiffuseColor_FogStart.xyz, spill);
      currColor = interior_ambient;
      lDiffuse = interior_diffuse * clamp(dot(norm, L), 0.0, 1.0);
    }
    else
    {
      // OUTDOOR: client-exact M2 sun (Diffuse_T1.bls vs_2_0 perm 1) -- FLAT ambient + ONE directional.
      // The third light component is NEGATED vs terrain because M2 normals load fixCoordSystem'd.
      float nDotL = clamp(dot(norm, -normalize(vec3(LightDir_FogRate.x, LightDir_FogRate.z, -LightDir_FogRate.y))), 0.0, 1.0);
      currColor = AmbientColor_FogEnd.xyz;
      lDiffuse = DiffuseColor_FogStart.xyz * nDotL;
      if (!term_on(5)) lDiffuse += point_lights(v_world, norm);
      // per-OBJECT zone light tint (v_interior.rgb when a == 0; (0,0,0) is the no-tint sentinel)
      if (v_interior.r + v_interior.g + v_interior.b > 0.0)
      {
        currColor *= v_interior.rgb;
        lDiffuse *= v_interior.rgb;
      }
    }
  }
  else
  {
    currColor = vec3(1.0);
    // unlit OPAQUE / alpha-key props still catch a light shaft point-light glow (blend 0/1 only)
    if ((blend_mode == 0 || blend_mode == 1) && !term_on(5))
      lDiffuse = point_lights(v_world, norm);
  }

  // D3D fixed-function: the accumulated LIGHT saturates BEFORE the texture modulate
  if (term_on(0))
    color.rgb = color.rgb * clamp(currColor + lDiffuse, 0.0, 1.0);

  if (term_on(2) && Toggles.y != 0.0 && FogColor_FogOn.w != 0.0 && !unfogged)
  {
    bool use_env = EnvFogColor_On.w > 0.5;
    vec3 fog_color_m2 = use_env ? EnvFogColor_On.rgb : FogColor_FogOn.rgb;
    float fog_end_m2 = use_env ? EnvFogDist.y : AmbientColor_FogEnd.w;
    float fog_start_frac_m2 = use_env ? EnvFogDist.x : DiffuseColor_FogStart.w;
    float start = fog_end_m2 * fog_start_frac_m2;
    float camera_dist = distance(Camera_Pad.xyz, v_world);
    float fx = -(1.0 / (fog_end_m2 - start));
    float fy = (1.0 / (fog_end_m2 - start)) * fog_end_m2;
    float f4 = min(pow(max(camera_dist * fx + fy, 0.0), LightDir_FogRate.w), 1.0);
    float fogFactor = 1.0 - f4;

    // additive passes fade to NOTHING, everything else toward the fog colour
    vec3 fog_target = (blend_mode == 4 || blend_mode == 3) ? vec3(0.0) : fog_color_m2;
    color.rgb = mix(color.rgb, fog_target, fogFactor);
    if (blend_mode == 0 || blend_mode == 1)
      color.a = 1.0 - fogFactor;   // bloom mask (these modes never blend)
  }

  // Add renders PREMULTIPLIED (the blender is ONE/ONE); alpha then carries the emitted brightness.
  if (blend_mode == 3 || blend_mode == 4)
  {
    if (blend_mode == 4)
      color.rgb *= clamp(color.a, 0.0, 1.0);
    color.a = max(color.r, max(color.g, color.b));
  }

  // Foliage carries its coverage in alpha (the fade is baked in above); it never blooms.
  if (detail_doodad && blend_mode == 1)
    color.a = grass_coverage;

  out_color = color;
  out_z = gl_FragCoord.z;
}
