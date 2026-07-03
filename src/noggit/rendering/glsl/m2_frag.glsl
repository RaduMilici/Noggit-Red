// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

in vec2 uv1;
in vec2 uv2;
in float camera_dist;
in vec3 norm;
in vec3 m2_world_pos;

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

uniform vec4 mesh_color;
uniform int blend_mode;

uniform sampler2DArray tex1;
uniform sampler2DArray tex2;
uniform int tex1_index;
uniform int tex2_index;

uniform int unfogged;
uniform int unlit;
uniform int masked_additive;

uniform int pixel_shader;

void main()
{

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

  if(mesh_color.a < alpha_test)
  {
    discard;
  }
  
  // code from Deamon87 and https://wowdev.wiki/M2/Rendering#Pixel_Shaders
  if (pixel_shader == 0) //Combiners_Opaque
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      color.rgb = texture1.rgb * mesh_color.rgb;
      color.a = mesh_color.a;
  } 
  else if (pixel_shader == 1) // Combiners_Decal
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      color.rgb = mix(mesh_color.rgb, texture1.rgb, mesh_color.a);
      color.a = mesh_color.a;
  } 
  else if (pixel_shader == 2) // Combiners_Add
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      color.rgba = texture1.rgba + mesh_color.rgba;
  } 
  else if (pixel_shader == 3) // Combiners_Mod2x
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      color.rgb = texture1.rgb * mesh_color.rgb * vec3(2.0);
      color.a = texture1.a * mesh_color.a * 2.0;
  } 
  else if (pixel_shader == 4) // Combiners_Fade
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      color.rgb = mix(texture1.rgb, mesh_color.rgb, mesh_color.a);
      color.a = mesh_color.a;
  } 
  else if (pixel_shader == 5) // Combiners_Mod
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      color.rgba = texture1.rgba * mesh_color.rgba;
  } 
  else if (pixel_shader == 6) // Combiners_Opaque_Opaque
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
      color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb;
      color.a = mesh_color.a;
  } 
  else if (pixel_shader == 7) // Combiners_Opaque_Add
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
      color.rgb = texture2.rgb + texture1.rgb * mesh_color.rgb;
      color.a = mesh_color.a + texture1.a;
  } 
  else if (pixel_shader == 8) // Combiners_Opaque_Mod2x
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
      color.rgb = texture1.rgb * mesh_color.rgb * texture2.rgb * vec3(2.0);
      color.a  = texture2.a * mesh_color.a * 2.0;
  } 
  else if (pixel_shader == 9)  // Combiners_Opaque_Mod2xNA
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
      color.rgb = texture1.rgb * mesh_color.rgb * texture2.rgb * vec3(2.0);
      color.a  = mesh_color.a;
  } 
  else if (pixel_shader == 10) // Combiners_Opaque_AddNA
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
      color.rgb = texture2.rgb + texture1.rgb * mesh_color.rgb;
      color.a = mesh_color.a;
  } 
  else if (pixel_shader == 11) // Combiners_Opaque_Mod
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
      color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb;
      color.a = texture2.a * mesh_color.a;
  } 
  else if (pixel_shader == 12) // Combiners_Mod_Opaque
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
      color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb;
      color.a = texture1.a;
  } 
  else if (pixel_shader == 13) // Combiners_Mod_Add
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
      color.rgba = texture2.rgba + texture1.rgba * mesh_color.rgba;
  } 
  else if (pixel_shader == 14) // Combiners_Mod_Mod2x
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
      color.rgba = texture1.rgba * texture2.rgba * mesh_color.rgba * vec4(2.0);
  } 
  else if (pixel_shader == 15) // Combiners_Mod_Mod2xNA
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
      color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb * vec3(2.0);
      color.a = texture1.a * mesh_color.a;
  } 
  else if (pixel_shader == 16) // Combiners_Mod_AddNA
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
      color.rgb = texture2.rgb + texture1.rgb * mesh_color.rgb;
      color.a = texture1.a * mesh_color.a;
  } 
  else if (pixel_shader == 17) // Combiners_Mod_Mod
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
      color.rgba = texture1.rgba * texture2.rgba * mesh_color.rgba;
  } 
  else if (pixel_shader == 18) // Combiners_Add_Mod
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
      color.rgb = (texture1.rgb + mesh_color.rgb) * texture2.a;
      color.a = (texture1.a + mesh_color.a) * texture2.a;
  } 
  else if (pixel_shader == 19) // Combiners_Mod2x_Mod2x
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
      color.rgba = texture1.rgba * texture2.rgba * mesh_color.rgba * vec4(4.0);
  }
  else if (pixel_shader == 20)  // Combiners_Opaque_Mod2xNA_Alpha
  {
    vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
    vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
    color.rgb = (mesh_color.rgb * texture1.rgb) * mix(texture2.rgb * 2.0, vec3(1.0), texture1.a);
    color.a = mesh_color.a;
  }
  else if (pixel_shader == 21)   //Combiners_Opaque_AddAlpha
  {
    vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
    vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
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
    vec4 texture1 = texture(tex1, vec3(uv1, tex1_index));
    vec4 texture2 = texture(tex2, vec3(uv2, tex2_index));
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

  if(color.a < alpha_test)
  {
    discard;
  }

  // apply world lighting
  vec3 currColor;
  vec3 lDiffuse = vec3(0.0, 0.0, 0.0);
  vec3 accumlatedLight = vec3(1.0, 1.0, 1.0);

  if(unlit == 0)
  {
      float nDotL = clamp(dot(normalize(norm), -normalize(vec3(-LightDir_FogRate.x, LightDir_FogRate.z, -LightDir_FogRate.y))), 0.0, 1.0);

      vec3 ambientColor = AmbientColor_FogEnd.xyz;

      vec3 skyColor = (ambientColor * 1.10000002);
      vec3 groundColor = (ambientColor * 0.699999988);

      currColor = mix(groundColor, skyColor, 0.5 + (0.5 * nDotL));
      lDiffuse = DiffuseColor_FogStart.xyz * nDotL + point_lights(m2_world_pos, normalize(norm));
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

  color.rgb = clamp(color.rgb * (currColor + lDiffuse), 0.0, 1.0);

  if(FogColor_FogOn.w != 0 && unfogged == 0)
  {
    float start = AmbientColor_FogEnd.w * DiffuseColor_FogStart.w;

    vec3 fogParams;
    fogParams.x = -(1.0 / (AmbientColor_FogEnd.w - start));
    fogParams.y = (1.0 / (AmbientColor_FogEnd.w - start)) * AmbientColor_FogEnd.w;
    fogParams.z = LightDir_FogRate.w;

    float f1 = (camera_dist * fogParams.x) + fogParams.y;
    float f2 = max(f1, 0.0);
    float f3 = pow(f2, fogParams.z);
    float f4 = min(f3, 1.0);

    float fogFactor = 1.0 - f4;

    // Additive passes (Add=4, No_Add_Alpha=3) add light, so in fog they must fade to NOTHING (black)
    // rather than toward the fog colour -- otherwise a distant light shaft / glow stays as a bright
    // cutout floating in the haze instead of dissolving into it. Everything else fades to fog colour.
    vec3 fog_target = (blend_mode == 4 || blend_mode == 3) ? vec3(0.0) : FogColor_FogOn.rgb;
    color.rgb = mix(color.rgb, fog_target, fogFactor);

    // Opaque / alpha-key doodads: write the bloom mask into alpha so fog-brightened doodads stop
    // blooming (the bloom bright-pass keeps only alpha > 0). Transparent passes keep their blend
    // alpha; additive ones already fade to black above so they don't bloom under fog anyway.
    if (blend_mode == 0 || blend_mode == 1)
    {
      color.a = 1.0 - fogFactor;
    }
  }

  // Bloom-mask encoding for the bright-pass. CRITICAL: additive passes (No_Add_Alpha=3 / Add=4) use
  // out_color.a as the BLEND source factor (GL_SRC_ALPHA), so we must NOT overwrite it -- forcing it to
  // 1.0 made masked low-opacity additive layers (the volumetric light-shaft's scrolling dust layer)
  // render at full intensity. Leave additive alpha exactly as the material set it: a bright additive
  // glow (brazier/flame) naturally accumulates a high alpha and trips the emissive bloom bypass, while a
  // faint masked additive layer keeps its low alpha and does not. Only non-additive surfaces are capped
  // below the reserved emissive range (lava's emissive bloom comes from the liquid shader, not here).
  if (blend_mode != 3 && blend_mode != 4)
  {
    color.a = min(color.a, 0.85);
  }

  out_color = color;
}
