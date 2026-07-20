// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

in vec2 uv1;
in vec2 uv2;
in float camera_dist;
in vec3 norm;
in vec3 m2_world_pos;
flat in vec4 v_interior;        // rgb = interior room ambient; a in [0.5,1.0] indoors (carries GAP B doorway spill), 0 outdoors

out vec4 out_color;

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
    vec4 EnvFogColor_On;       // rgb = entity fog colour (camera's fog context), w = 1 when active
    vec4 EnvFogDist;           // x = start FRACTION of end (can be negative), y = end
};

// Client point-light model for M2s (RE_notes/15):
//  - falloff = 1/(0.7d + 0.03d^2), HARDCODED (@0x695C92 / GxuLight defaults @0x591260); MOLT
//    attenStart/attenEnd never shape it (PointLightPos.w is only a cull bound).
//  - AT MOST 3 point lights reach any model (CM2Lighting keeps a 4-slot nearest heap, <=3 to
//    hardware). Summing all 16 UBO lights stacked chandeliers into gold-plated NPCs; we mirror
//    the cap by accumulating only the 3 NEAREST lights to the fragment.
//  - skip_molt: the client never lights UNITS with MOLT point lights (converted to linear-falloff
//    directionals, hard-skipped at d >= attenEnd -- always skipped in classic WMOs where
//    attenEnd=0). Units pass true; doodads keep MOLT (their group light list includes them).
//    MOLT lights are flagged with PointLightColor.w == 1.
vec3 point_lights_impl(vec3 world_pos, vec3 n, bool skip_molt)
{
  int count = int(PointLightParams.x);
  int   best_i[3] = int[3](-1, -1, -1);
  float best_d[3] = float[3](1e30, 1e30, 1e30);
  for (int i = 0; i < count; ++i)
  {
    if (skip_molt && PointLightColor[i].w > 0.5)
    {
      continue;
    }
    float dist = length(PointLightPos[i].xyz - world_pos);
    if (dist < best_d[0])      { best_d[2]=best_d[1]; best_i[2]=best_i[1]; best_d[1]=best_d[0]; best_i[1]=best_i[0]; best_d[0]=dist; best_i[0]=i; }
    else if (dist < best_d[1]) { best_d[2]=best_d[1]; best_i[2]=best_i[1]; best_d[1]=dist; best_i[1]=i; }
    else if (dist < best_d[2]) { best_d[2]=dist; best_i[2]=i; }
  }
  vec3 accum = vec3(0.0);
  for (int k = 0; k < 3; ++k)
  {
    if (best_i[k] < 0)
    {
      break;
    }
    vec3 to_l = PointLightPos[best_i[k]].xyz - world_pos;
    float dist = best_d[k];
    float atten = 1.0 / max(0.7 * dist + 0.03 * dist * dist, 0.05);
    float ndotl = max(dot(n, to_l / max(dist, 0.0001)), 0.0);
    accum += PointLightColor[best_i[k]].xyz * ndotl * atten;
  }
  return accum;
}

vec3 point_lights(vec3 world_pos, vec3 n)
{
  return point_lights_impl(world_pos, n, false);
}

uniform vec4 mesh_color;
uniform int blend_mode;
uniform int water_surface_effect; // 1 = fishing-pool wake geoset: grey out + luminance-alpha (see below)

uniform sampler2DArray tex1;
uniform sampler2DArray tex2;
uniform int tex1_index;
uniform int tex2_index;
// M2 texture wrap flags (0x1 wrap X, 0x2 wrap Y): an UNSET bit means the texture addresses CLAMP
// on that axis (trace-verified: 1.12 sets D3DTADDRESS_CLAMP on most in-world M2 draws). Encoded
// here inverted as a clamp mask (bit0 = clamp U, bit1 = clamp V) so an unset uniform (0) keeps the
// wrap default for callers that never bind it. Textures share array textures, so clamp-to-edge is
// emulated by pulling the coordinate half a texel inside the border.
uniform int tex1_clamp;
uniform int tex2_clamp;

vec2 cuv1;
vec2 cuv2;

vec2 clamp_uv(vec2 uv, int clamp_mask, vec2 tex_size)
{
  vec2 half_texel = 0.5 / tex_size;
  if (bool(clamp_mask & 1))
  {
    uv.x = clamp(uv.x, half_texel.x, 1.0 - half_texel.x);
  }
  if (bool(clamp_mask & 2))
  {
    uv.y = clamp(uv.y, half_texel.y, 1.0 - half_texel.y);
  }
  return uv;
}

uniform int unfogged;
uniform int unlit;
uniform int detail_doodad; // 1 = ground clutter: grayscale day/night dim, no colour tint
uniform int masked_additive;
uniform int creature_bloom; // 1 = creature/character M2: bright body pixels feed the emissive bloom mask

uniform int pixel_shader;

// Per-pixel distance slice (terrain-parity object boundary): fragments beyond the object cull
// distance are discarded, so a model straddling the boundary loses its far pixels first and
// slices in/out of view exactly like terrain at the far plane -- no whole-model pops. 0 = off
// (creatures/near paths keep their own fade).
uniform float slice_dist;

void main()
{
  if (slice_dist > 0.0 && camera_dist > slice_dist)
  {
    discard;
  }

  cuv1 = (tex1_clamp != 0) ? clamp_uv(uv1, tex1_clamp, vec2(textureSize(tex1, 0).xy)) : uv1;
  cuv2 = (tex2_clamp != 0) ? clamp_uv(uv2, tex2_clamp, vec2(textureSize(tex2, 0).xy)) : uv2;

  float alpha_test;
  int fog_mode;

  switch (blend_mode)
  {
      default:
      case 0: // Opaque
      {
          alpha_test = -1.0;
          fog_mode = 1;
          break;
      }
      case 1: // Alpha_key
      {
          alpha_test = (224.f / 255.f) * mesh_color.w;
          fog_mode = 1;
          break;
      }
      case 2: // Alpha
      {
          alpha_test = (1.f / 255.f) * mesh_color.w;
          fog_mode = 1;
          break;
      }
      case 3: // No_Add_Alpha
      case 4: // Add
      {
          alpha_test = (1.f / 255.f) * mesh_color.w;
          fog_mode = 2; // Warning: wiki is unsure on that for No_Add_Alpha
          break;
      }
      case 5: // Mod
      {
          alpha_test = (1.f / 255.f) * mesh_color.w;
          fog_mode = 3;
          break;
      }
      case 6: // Mod2X
      {
          alpha_test = (1.f / 255.f) * mesh_color.w;
          fog_mode = 4;
          break;
      }
  }

  vec4 color = vec4(0.0);

  float grass_coverage = 1.0;
  if (detail_doodad == 1 && blend_mode == 1)
  {
    // ALPHA-TO-COVERAGE for ground-clutter grass. Distant blades cover < 1 pixel; a hard alpha-key
    // makes each a full-bright green DOT (or nothing), so the field "dissolves into pixels" far
    // away. Compute the blade's sub-pixel coverage from the mip-softened alpha via its screen-space
    // derivative and output it as the fragment alpha; with GL_SAMPLE_ALPHA_TO_COVERAGE enabled on
    // this draw (needs MSAA), a partly-covered blade contributes partial MSAA samples -> it blends
    // smoothly into the ground instead of dotting. Keeps near grass crisp (coverage saturates to 1).
    float w = max(fwidth(mesh_color.a), 1e-4);
    grass_coverage = clamp((mesh_color.a - alpha_test) / w + 0.5, 0.0, 1.0);
    if (grass_coverage <= 0.003)
    {
      discard;
    }
  }
  else if(mesh_color.a < alpha_test)
  {
    discard;
  }
  
  // code from Deamon87 and https://wowdev.wiki/M2/Rendering#Pixel_Shaders
  if (pixel_shader == 0) //Combiners_Opaque
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      color.rgb = texture1.rgb * mesh_color.rgb;
      color.a = mesh_color.a;
  } 
  else if (pixel_shader == 1) // Combiners_Decal
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      color.rgb = mix(mesh_color.rgb, texture1.rgb, mesh_color.a);
      color.a = mesh_color.a;
  } 
  else if (pixel_shader == 2) // Combiners_Add
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      color.rgba = texture1.rgba + mesh_color.rgba;
  } 
  else if (pixel_shader == 3) // Combiners_Mod2x
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      color.rgb = texture1.rgb * mesh_color.rgb * vec3(2.0);
      color.a = texture1.a * mesh_color.a * 2.0;
  } 
  else if (pixel_shader == 4) // Combiners_Fade
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      color.rgb = mix(texture1.rgb, mesh_color.rgb, mesh_color.a);
      color.a = mesh_color.a;
  } 
  else if (pixel_shader == 5) // Combiners_Mod
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      color.rgba = texture1.rgba * mesh_color.rgba;
  } 
  else if (pixel_shader == 6) // Combiners_Opaque_Opaque
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
      color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb;
      color.a = mesh_color.a;
  } 
  else if (pixel_shader == 7) // Combiners_Opaque_Add
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
      color.rgb = texture2.rgb + texture1.rgb * mesh_color.rgb;
      color.a = mesh_color.a + texture1.a;
  } 
  else if (pixel_shader == 8) // Combiners_Opaque_Mod2x
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
      color.rgb = texture1.rgb * mesh_color.rgb * texture2.rgb * vec3(2.0);
      color.a  = texture2.a * mesh_color.a * 2.0;
  } 
  else if (pixel_shader == 9)  // Combiners_Opaque_Mod2xNA
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
      color.rgb = texture1.rgb * mesh_color.rgb * texture2.rgb * vec3(2.0);
      color.a  = mesh_color.a;
  } 
  else if (pixel_shader == 10) // Combiners_Opaque_AddNA
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
      color.rgb = texture2.rgb + texture1.rgb * mesh_color.rgb;
      color.a = mesh_color.a;
  } 
  else if (pixel_shader == 11) // Combiners_Opaque_Mod
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
      color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb;
      color.a = texture2.a * mesh_color.a;
  } 
  else if (pixel_shader == 12) // Combiners_Mod_Opaque
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
      color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb;
      color.a = texture1.a;
  } 
  else if (pixel_shader == 13) // Combiners_Mod_Add
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
      color.rgba = texture2.rgba + texture1.rgba * mesh_color.rgba;
  } 
  else if (pixel_shader == 14) // Combiners_Mod_Mod2x
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
      color.rgba = texture1.rgba * texture2.rgba * mesh_color.rgba * vec4(2.0);
  } 
  else if (pixel_shader == 15) // Combiners_Mod_Mod2xNA
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
      color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb * vec3(2.0);
      color.a = texture1.a * mesh_color.a;
  } 
  else if (pixel_shader == 16) // Combiners_Mod_AddNA
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
      color.rgb = texture2.rgb + texture1.rgb * mesh_color.rgb;
      color.a = texture1.a * mesh_color.a;
  } 
  else if (pixel_shader == 17) // Combiners_Mod_Mod
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
      color.rgba = texture1.rgba * texture2.rgba * mesh_color.rgba;
  } 
  else if (pixel_shader == 18) // Combiners_Add_Mod
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
      color.rgb = (texture1.rgb + mesh_color.rgb) * texture2.a;
      color.a = (texture1.a + mesh_color.a) * texture2.a;
  } 
  else if (pixel_shader == 19) // Combiners_Mod2x_Mod2x
  {
      vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
      color.rgba = texture1.rgba * texture2.rgba * mesh_color.rgba * vec4(4.0);
  }
  else if (pixel_shader == 20)  // Combiners_Opaque_Mod2xNA_Alpha
  {
    vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
    vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
    color.rgb = (mesh_color.rgb * texture1.rgb) * mix(texture2.rgb * 2.0, vec3(1.0), texture1.a);
    color.a = mesh_color.a;
  }
  else if (pixel_shader == 21)   //Combiners_Opaque_AddAlpha
  {
    vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
    vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
        if (masked_additive != 0)
        {
            // Keep the original dust mask so low-dust regions stay TRANSPARENT (showing the cave
            // behind, not black sheets) -- only brighten the visible additive beam so the shaft reads
            // brighter like in-game.
            // This batch's 2nd texture is just a *_Mask (Lightray_Dusty_02_Mask) -- WMV ignores it and
            // renders the beam (Lightray_Dusty_02). That beam texture has a DARK background with bright
            // sparkles, meant to be drawn additively. Our pass alpha-blends, so the dark background
            // showed as an opaque black "space" sheet. Use the beam's LUMINANCE as the alpha: dark
            // background -> transparent, bright sparkles/beam -> visible. Reads as a translucent beam
            // with bright white sparkles, no black sheet, under additive OR alpha blend.
            // Transparent dusty beam: render with the beam's LUMINANCE as alpha so the dark background
            // stays transparent (only the bright dust/beam texels show), drawn additively. The trailing
            // factor is the lightray dust "texture opacity" lever -- lower it to make the dust fainter.
            color.rgb = mesh_color.rgb * texture1.rgb;
            color.a = mesh_color.a * max(texture1.r, max(texture1.g, texture1.b)) * 0.10;
        }
        else
        {
            color.rgb = (mesh_color.rgb * texture1.rgb) + (texture2.rgb * texture2.a);
            color.a = mesh_color.a;
        }
  }
  else if (pixel_shader == 22)   // Combiners_Opaque_AddAlpha_Alpha
  {
    vec4 texture1 = texture(tex1, vec3(cuv1, tex1_index));
    vec4 texture2 = texture(tex2, vec3(cuv2, tex2_index));
        if (masked_additive != 0)
        {
            // This batch's 2nd texture is just a *_Mask (Lightray_Dusty_02_Mask) -- WMV ignores it and
            // renders the beam (Lightray_Dusty_02). That beam texture has a DARK background with bright
            // sparkles, meant to be drawn additively. Our pass alpha-blends, so the dark background
            // showed as an opaque black "space" sheet. Use the beam's LUMINANCE as the alpha: dark
            // background -> transparent, bright sparkles/beam -> visible. Reads as a translucent beam
            // with bright white sparkles, no black sheet, under additive OR alpha blend.
            // Transparent dusty beam: render with the beam's LUMINANCE as alpha so the dark background
            // stays transparent (only the bright dust/beam texels show), drawn additively. The trailing
            // factor is the lightray dust "texture opacity" lever -- lower it to make the dust fainter.
            color.rgb = mesh_color.rgb * texture1.rgb;
            color.a = mesh_color.a * max(texture1.r, max(texture1.g, texture1.b)) * 0.10;
        }
        else
        {
            color.rgb = (mesh_color.rgb * texture1.rgb) + (texture2.rgb * texture2.a * texture1.a);
            color.a = mesh_color.a;
        }
  }

  // FISHING-POOL WAKE (foam ring / bubbles / sparkles): the effect texture is a near-black field with
  // bright wisps and no usable alpha. Drawn opaque it bloomed white; alpha-blended it painted a solid
  // blue veil; additive glowed too bright. Make it read like the murky water it floats on: desaturate
  // the wisps to grey and use the texel BRIGHTNESS as the alpha (same trick as the dusty-lightray
  // combiners above) so the dark field goes fully transparent -- the water colour shows straight
  // through -- and only a faint grey wake remains. It stays lit below, so it warms/cools with the
  // scene like the surrounding water. The 0.7 lever is the wake opacity (lower = fainter).
  if (water_surface_effect == 1)
  {
    // Luminance (NOT max-channel, which read near-white on the cyan wisps). Map it into a DARK grey
    // band so the wake reads like the murky water -- never white, never a black hole -- and use it as
    // the alpha so the near-black texture field goes transparent (water shows through). Lit below, so
    // it takes the scene's warmth. 0.15 = trough grey, 0.55 = brightest wake, 1.4 = wake opacity lever.
    float lum = dot(color.rgb, vec3(0.299, 0.587, 0.114));
    color.rgb = vec3(mix(0.15, 0.55, lum));
    color.a = clamp(lum * 1.4, 0.0, 1.0);
  }

  if(color.a < alpha_test)
  {
    discard;
  }

  // apply world lighting
  vec3 currColor;
  vec3 lDiffuse = vec3(0.0, 0.0, 0.0);
  vec3 accumlatedLight = vec3(1.0, 1.0, 1.0);

  if(detail_doodad == 1)
  {
      // GROUND CLUTTER: the client bakes the Light-DBC scene colours straight into grass vertices
      // (D3D lighting OFF in the trace; night verts avg ~(200,132,56) = a dim muted warm, and it
      // darkens/cools with the scene). So light grass with the SAME colored hemisphere+diffuse as
      // terrain -- it then matches the ground it grows from at every hour. Grass normals point up,
      // so use a fixed upward normal for a stable, non-flickery N.L (tufts have no meaningful face).
      vec3 upN = vec3(0.0, 1.0, 0.0);
      float nDotL = clamp(dot(upN, -normalize(vec3(LightDir_FogRate.x, LightDir_FogRate.z, -LightDir_FogRate.y))), 0.0, 1.0);
      vec3 amb = AmbientColor_FogEnd.xyz;
      vec3 skyC = amb * 1.10000002;
      vec3 grndC = amb * 0.699999988;
      currColor = mix(grndC, skyC, 0.5 + 0.5 * nDotL);
      lDiffuse = DiffuseColor_FogStart.xyz * nDotL;
      accumlatedLight = vec3(0.0, 0.0, 0.0);
  }
  else if(unlit == 0)
  {
      if (v_interior.a > 0.25)
      {
          // Interior lighting from the baked MOCV floor colour C (raycast-sampled under the object):
          // ambient = C capped to max-channel 96/255, diffuse = C boosted to >= 168/255 with N.L along
          // the client's FIXED interior light direction (client formula, RE_notes/15 @0x69E4C0/0x6A77E0).
          // v_interior.a in [0.5,1.0] carries the GAP B (checklist 8.7) DOORWAY SPILL = the baked MOCV
          // floor ALPHA sampled under the object: 0.5 = deep interior (no change), 1.0 = at a portal/
          // window. Below, the interior light is lerped toward the OUTDOOR day/night light by that spill
          // (client FUN_0069e4c0 @0x69e4c0 / LIGHT_FOG_SELECTION_RE.md 9.3: interior = mix(interior,
          // outdoor, mocv_alpha/255)); spill 0 is a no-op mix, so deep interiors are byte-identical. Any
          // a > 0.25 is "interior" (the exact magnitude only encodes spill). The outdoor sun and MOHD
          // ambient otherwise play no part.
          // MEASURED from wow_cap_upstairs.trace (RE_notes/16): every in-world M2 -- NPC and doodad
          // alike -- decodes to ambient(C capped) + ONE warm directional(C boosted) along the fixed
          // interior vector, and ZERO point lights reach any M2 (c17-c27 all zero all frames). The
          // candle look on models is entirely this baked pair; do NOT add point lights here.
          vec3 C = v_interior.rgb;
          float mc = max(C.r, max(C.g, C.b));
          vec3 interior_ambient = C * min(1.0, 0.376471 / max(mc, 0.003922)); // cap 96/255
          vec3 interior_diffuse = C * max(1.0, 0.658824 / max(mc, 0.003922)); // boost 168/255
          vec3 L = normalize(vec3(0.30822, 0.9, -0.30822)); // to-light, noggit y-up
          // GAP B doorway spill: lerp the interior light toward the outdoor day/night light by the baked
          // MOCV floor alpha (v_interior.a in [0.5,1.0] -> spill in [0,1]). spill 0 => mix is a no-op.
          float spill = clamp((v_interior.a - 0.5) * 2.0, 0.0, 1.0);
          interior_ambient = mix(interior_ambient, AmbientColor_FogEnd.xyz, spill);
          interior_diffuse = mix(interior_diffuse, DiffuseColor_FogStart.xyz, spill);
          currColor = interior_ambient;
          lDiffuse = interior_diffuse * clamp(dot(normalize(norm), L), 0.0, 1.0);
      }
      else
      {
          // OUTDOOR sun lighting -- IDENTICAL PIPELINE to terrain_frag.glsl (same Light-DBC diffuse/
          // ambient, same ground/sky hemisphere, same N.L), so objects and the ground they sit on
          // catch the sun from the same compass direction.
          // FRAME: M2 normals load via fixCoordSystem = (wow.x, wow.z, -wow.y), but terrain (MCNR)
          // normals are (wow.x, wow.z, +wow.y). Terrain lights with (LightDir.x, LightDir.z,
          // LightDir.y); to compute the SAME physical wow-space N.L against the M2's negated third
          // component, the light's third component must be negated too -- else the north-south
          // half of the sun azimuth is mirrored and objects light from the wrong side vs terrain.
          float nDotL = clamp(dot(normalize(norm), -normalize(vec3(LightDir_FogRate.x, LightDir_FogRate.z, -LightDir_FogRate.y))), 0.0, 1.0);

          vec3 ambientColor = AmbientColor_FogEnd.xyz;

          vec3 skyColor = (ambientColor * 1.10000002);
          vec3 groundColor = (ambientColor * 0.699999988);

          currColor = mix(groundColor, skyColor, 0.5 + (0.5 * nDotL));
          lDiffuse = DiffuseColor_FogStart.xyz * nDotL + point_lights(m2_world_pos, normalize(norm));

          // Per-OBJECT zone light (trace: wow_cap_timbermaw -- at one instant, different M2s carry the
          // zone SH scaled by different per-channel tints): the client evaluates the zone light at each
          // ENTITY's position, not the camera's. v_interior.rgb carries light_at(object)/light_at(camera)
          // when a==0; (0,0,0) is the legacy no-tint sentinel.
          if (v_interior.r + v_interior.g + v_interior.b > 0.0)
          {
            currColor *= v_interior.rgb;
            lDiffuse *= v_interior.rgb;
          }
      }
  }
  else
  {
      // Unlit materials ignore scene lighting entirely (fullbright). Multiplying the
      // texture by ambient (as before) turned emissive models such as lava waterfalls
      // and fire black inside dark caves, where ambient is near zero.
      currColor = vec3(1.0f, 1.0f, 1.0f);
      accumlatedLight = vec3(0.0f, 0.0f, 0.0f);

      // ...but unlit OPAQUE / alpha-key props (totems, barrels, mushrooms) should still catch a light
      // shaft's point-light glow. Limited to blend 0/1: widening to alpha/additive blends made other
      // doodads render worse.
      if (blend_mode == 0 || blend_mode == 1)
      {
        lDiffuse += point_lights(m2_world_pos, normalize(norm));
      }
  }

  // D3D fixed-function semantics (client clamp-pack @0x71CA80): the accumulated vertex LIGHT saturates
  // to 1.0 BEFORE the texture modulate, so a surface can never render brighter than its own texture.
  // Clamping after the multiply instead let >1 light (e.g. candle falloff right at a chandelier)
  // white-out the texture -- blown chandeliers, glowing blouses, and a washed low-contrast room vs
  // the client's warm orange.
  color.rgb = color.rgb * clamp(currColor + lDiffuse, 0.0, 1.0);

  if(FogColor_FogOn.w != 0 && unfogged == 0)
  {
    // ENTITY fog: the camera's fog context (zone fog blended toward the containing WMO fog volume)
    // -- client-canon (note 16: inside the inn, M2 fog constants equal the inn MFOG). Terrain keeps
    // the zone slots; EnvFogColor_On.w == 0 falls back to them.
    bool use_env = EnvFogColor_On.w > 0.5;
    vec3 fog_color_m2 = use_env ? EnvFogColor_On.rgb : FogColor_FogOn.rgb;
    float fog_end_m2 = use_env ? EnvFogDist.y : AmbientColor_FogEnd.w;
    float fog_start_frac_m2 = use_env ? EnvFogDist.x : DiffuseColor_FogStart.w;

    float start = fog_end_m2 * fog_start_frac_m2;

    vec3 fogParams;
    fogParams.x = -(1.0 / (fog_end_m2 - start));
    fogParams.y = (1.0 / (fog_end_m2 - start)) * fog_end_m2;
    fogParams.z = LightDir_FogRate.w;

    float f1 = (camera_dist * fogParams.x) + fogParams.y;
    float f2 = max(f1, 0.0);
    float f3 = pow(f2, fogParams.z);
    float f4 = min(f3, 1.0);

    float fogFactor = 1.0 - f4;

    // Additive passes (Add=4, No_Add_Alpha=3) add light, so in fog they must fade to NOTHING (black)
    // rather than toward the fog colour -- otherwise a distant light shaft / glow stays as a bright
    // cutout floating in the haze instead of dissolving into it. Everything else fades to fog colour.
    vec3 fog_target = (blend_mode == 4 || blend_mode == 3) ? vec3(0.0) : fog_color_m2;
    color.rgb = mix(color.rgb, fog_target, fogFactor);

    // Opaque / alpha-key doodads: write the bloom mask into alpha so fog-brightened doodads stop
    // blooming (the bloom bright-pass keeps only alpha > 0). Transparent passes keep their blend
    // alpha; additive ones already fade to black above so they don't bloom under fog anyway.
    // EXCEPTION creature_bloom == 2 (translucent creature body): its alpha is the BLEND FACTOR --
    // this overwrite made Anomalus/ghosts render OPAQUE on any fogged map (the bug only spared
    // fog-off interiors). Its bloom mask comes from the separate alpha-only pass (mode 3).
    if ((blend_mode == 0 || blend_mode == 1) && creature_bloom != 2)
    {
      color.a = 1.0 - fogFactor;
    }
  }

  // Bloom-mask encoding for the bright-pass. Additive passes (No_Add_Alpha=3 / Add=4) render
  // PREMULTIPLIED: the blender runs ONE/ONE (see ModelRenderPass::prepareDraw) and we fold the
  // material alpha into RGB here for Add -- byte-identical output to the old SRC_ALPHA/ONE -- which
  // frees the alpha channel to carry the pass's own EMITTED brightness as the bloom-mask
  // contribution. The old path wrote the material's COVERAGE alpha into the mask: a big glow card
  // marked its whole quad emissive no matter how faint its texels were, and the bright-pass then
  // bloomed whatever was BEHIND the quad -- a bright sky lit up as a hard white BOX (dark terrain
  // behind stayed below the brightness knee, hiding the bug). Emitted brightness only reaches the
  // reserved emissive range (>0.88) where the glow itself is genuinely bright (brazier cores, flame
  // hearts), which is the canon signal; faint card edges write ~0 and can never light the background.
  if (blend_mode == 3 || blend_mode == 4)
  {
    if (blend_mode == 4)
    {
      color.rgb *= clamp(color.a, 0.0, 1.0);
    }
    color.a = max(color.r, max(color.g, color.b));
  }
  else
  {
    float masked = min(color.a, 0.85);
    // Energy creatures (Anomalus, arcane elementals, ghosts): a bright, near-white-hot body pixel
    // is emitted light -- push it into the reserved emissive mask range (>0.88) so the bright pass
    // blooms it, matching the client's screen-luminance FFXGlow. The knee (0.72..0.96 on the max
    // channel) fires ONLY on genuinely bright energy, so dim/dark body pixels and ordinary creatures
    // keep the capped mask (no bloom) and their coverage alpha. On a translucent energy creature the
    // bright core consequently also reads solid (alpha ->~0.99), which is how the client draws it.
    if (creature_bloom == 3)
    {
      // ALPHA-ONLY BLOOM-MASK PASS over a translucent energy creature (Anomalus): the body just drew
      // alpha-blended with its authored translucency (you can see through it), which leaves the FBO
      // bloom mask far below the emissive range. This re-draw writes ONLY the alpha channel (color
      // writes are off, blending off): bright body texels stamp an emissive mask so they bloom like
      // the client's screen-luminance FFXGlow; dim texels discard and leave the mask untouched.
      float luma = max(color.r, max(color.g, color.b));
      float e = smoothstep(0.65, 0.92, luma);
      if (e <= 0.0)
      {
        discard;
      }
      color.a = mix(0.885, 0.99, e);
    }
    else if (creature_bloom == 1)
    {
      // OPAQUE creature (arcane elementals, alpha 255; no blending on this pass, alpha writes raw):
      // near-white-hot texels feed the emissive mask directly; everything else keeps the capped
      // mask (no bloom).
      float luma = max(color.r, max(color.g, color.b));
      float e = smoothstep(0.65, 0.92, luma);
      color.a = (e > 0.0) ? max(masked, mix(0.885, 0.99, e)) : masked;
    }
    else
    {
      // Translucent creatures keep their authored per-pixel blend alpha here (mode 2 = body pass of
      // a translucent creature; its bloom comes from the mode-3 alpha-only pass afterwards).
      color.a = masked;
    }
  }

  out_color = color;

  // Grass: output the sub-pixel blade coverage as alpha for GL_SAMPLE_ALPHA_TO_COVERAGE (overrides
  // the bloom-mask alpha above; grass isn't emissive so it never needed to bloom).
  if (detail_doodad == 1)
  {
    out_color.a = grass_coverage;
  }
}
