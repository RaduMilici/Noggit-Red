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
  vec4 PointLightParams;     // .x = active point-light count
  vec4 PointLightPos[16];    // xyz = world pos, w = radius
  vec4 PointLightColor[16];  // xyz = colour * intensity
};

// Accumulated diffuse contribution of the emitter point lights (campfires etc.) at a world point.
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

layout (std140) uniform overlay_params
{
  int draw_shadows;
  int draw_lines;
  int draw_hole_lines;
  int draw_areaid_overlay;
  int draw_terrain_height_contour;
  int draw_wireframe;
  int wireframe_type;
  float wireframe_radius;
  float wireframe_width;
  int draw_impass_overlay;
  int draw_paintability_overlay;
  int draw_selection_overlay;
  vec4 wireframe_color;
  int draw_impassible_climb;
  int climb_use_output_angle;
  int climb_use_smooth_interpolation;
  float climb_value;
  int draw_vertex_color;
  int padding[3];
};

struct ChunkInstanceData
{
  ivec4 ChunkTextureSamplers;
  ivec4 ChunkTextureArrayIDs;
  ivec4 ChunkHoles_DrawImpass_TexLayerCount_CantPaint;
  ivec4 ChunkTexDoAnim;
  ivec4 ChunkTexAnimSpeed;
  ivec4 AreaIDColor_Pad2_DrawSelection;
  ivec4 ChunkXZ_TileXZ;
  ivec4 ChunkTexAnimDir;
};

layout (std140) uniform chunk_instances
{
  ChunkInstanceData instances[256];
};

uniform sampler2DArray shadowmap;
uniform sampler2DArray alphamap;
uniform sampler2D stamp_brush;
uniform sampler2DArray textures[11];
uniform vec3 camera;

// Terrain specular (client-exact): sun-band colour (LightIntBand band 9) + on/off from settings.
uniform int draw_terrain_specular;
uniform vec3 sun_spec_color;

uniform int draw_cursor_circle;
uniform vec3 cursor_position;
uniform float cursorRotation;
uniform float outer_cursor_radius;
uniform float inner_cursor_ratio;
uniform vec4 cursor_color;

in vec3 vary_position;
in vec2 vary_texcoord;
in vec2 vary_t0_uv;
in vec2 vary_t1_uv;
in vec2 vary_t2_uv;
in vec2 vary_t3_uv;
in vec3 vary_normal;
in vec3 vary_mccv;
flat in int instanceID;
flat in vec3 triangle_normal;

out vec4 out_color;

const float TILESIZE  = 533.33333;
const float CHUNKSIZE = TILESIZE / 16.0;
const float HOLESIZE  = CHUNKSIZE * 0.25;
const float UNITSIZE = HOLESIZE * 0.5;
const float PI = 3.14159265358979323846;

vec3 random_color(float areaID)
{
  float r = fract(sin(dot(vec2(areaID), vec2(12.9898, 78.233))) * 43758.5453);
  float g = fract(sin(dot(vec2(areaID), vec2(11.5591, 70.233))) * 43569.5451);
  float b = fract(sin(dot(vec2(areaID), vec2(13.1234, 76.234))) * 43765.5452);

  return vec3(r, g, b);
}

vec4 get_tex_color(vec2 tex_coord, int tex_sampler, int array_index)
{
  if (tex_sampler == 0)
  {
    return texture(textures[0], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 1)
  {
    return texture(textures[1], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 2)
  {
    return texture(textures[2], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 3)
  {
    return texture(textures[3], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 4)
  {
    return texture(textures[4], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 5)
  {
    return texture(textures[5], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 6)
  {
    return texture(textures[6], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 7)
  {
    return texture(textures[7], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 8)
  {
    return texture(textures[8], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 9)
  {
    return texture(textures[9], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 10)
  {
    return texture(textures[10], vec3(tex_coord, array_index)).rgba;
  }

  return vec4(0);


  /*
  // This should be more compliant to the GLSL standard, but seems to be slower :(
  vec2 uvDx = dFdx(tex_coord);
  vec2 uvDy = dFdy(tex_coord);
  vec3 uv = vec3(tex_coord, array_index);
  
  switch(tex_sampler) 
  {
    case 0:
      return textureGrad(textures[0], uv, uvDx, uvDy);
    case 1:
      return textureGrad(textures[1], uv, uvDx, uvDy);
    case 2:
      return textureGrad(textures[2], uv, uvDx, uvDy);
    case 3:
      return textureGrad(textures[3], uv, uvDx, uvDy);
    case 4:
      return textureGrad(textures[4], uv, uvDx, uvDy);
    case 5:
      return textureGrad(textures[5], uv, uvDx, uvDy);
    case 6:
      return textureGrad(textures[6], uv, uvDx, uvDy);
    case 7:
      return textureGrad(textures[7], uv, uvDx, uvDy);
    case 8:
      return textureGrad(textures[8], uv, uvDx, uvDy);
    case 9:
      return textureGrad(textures[9], uv, uvDx, uvDy);
    case 10:
      return textureGrad(textures[10], uv, uvDx, uvDy);
    default:
      return vec4(0.0);
  }

  */

}

vec4 texture_blend()
{
  vec3 alpha = texture(alphamap, vec3(vary_texcoord / 8.0, instanceID)).rgb;

  int layer_count = instances[instanceID].ChunkHoles_DrawImpass_TexLayerCount_CantPaint.b;

  alpha.r = mix(alpha.r, 0.0, float(layer_count < 2));
  alpha.g = mix(alpha.g, 0.0, float(layer_count < 3));
  alpha.b = mix(alpha.b, 0.0, float(layer_count < 4));

  float a0 = alpha.r;
  float a1 = alpha.g;
  float a2 = alpha.b;

  vec4 t0 = get_tex_color(vary_t0_uv, instances[instanceID].ChunkTextureSamplers.x, abs(instances[instanceID].ChunkTextureArrayIDs.x));
  t0.a = mix(t0.a, 0.f, int(instances[instanceID].ChunkTextureArrayIDs.x < 0));

  vec4 t1 = get_tex_color(vary_t1_uv, instances[instanceID].ChunkTextureSamplers.y, abs(instances[instanceID].ChunkTextureArrayIDs.y));
  t1.a = mix(t1.a, 0.f, int(instances[instanceID].ChunkTextureArrayIDs.y < 0));

  vec4 t2 = get_tex_color(vary_t2_uv, instances[instanceID].ChunkTextureSamplers.z, abs(instances[instanceID].ChunkTextureArrayIDs.z));
  t2.a = mix(t2.a, 0.f, int(instances[instanceID].ChunkTextureArrayIDs.z < 0));

  vec4 t3 = get_tex_color(vary_t3_uv, instances[instanceID].ChunkTextureSamplers.w, abs(instances[instanceID].ChunkTextureArrayIDs.w));
  t3.a = mix(t3.a, 0.f, int(instances[instanceID].ChunkTextureArrayIDs.w < 0));

  // MCLY 0x80 overbright (bit 1 of ChunkTexDoAnim, RE_notes/21 A1): the layer renders ~2x brighter
  // (client MOD2X) -- authored on lava-crack layers (MC / Searing Gorge / custom zones) to glow.
  t0.rgb *= 1.0 + float((instances[instanceID].ChunkTexDoAnim.x >> 1) & 1);
  t1.rgb *= 1.0 + float((instances[instanceID].ChunkTexDoAnim.y >> 1) & 1);
  t2.rgb *= 1.0 + float((instances[instanceID].ChunkTexDoAnim.z >> 1) & 1);
  t3.rgb *= 1.0 + float((instances[instanceID].ChunkTexDoAnim.w >> 1) & 1);

  return vec4 (t0 * (1.0 - (a0 + a1 + a2)) + t1 * a0 + t2 * a1 + t3 * a2);
}

float contour_alpha(float unit_size, float pos, float line_width)
{
  float f = abs(fract((pos + unit_size*0.5) / unit_size) - 0.5);
  float df = abs(line_width / unit_size);
  return smoothstep(0.0, df, f);
}

float contour_alpha(float unit_size, vec2 pos, vec2 line_width)
{
  return 1.0 - min( contour_alpha(unit_size, pos.x, line_width.x)
                  , contour_alpha(unit_size, pos.y, line_width.y)
                  );
}

void main()
{
  float dist_from_camera = distance(camera, vary_position);

  vec3 fw = fwidth(vary_position.xyz);

  // calc world lighting
  vec3 currColor;
  vec3 lDiffuse = vec3(0.0, 0.0, 0.0);
  vec3 accumlatedLight = vec3(1.0, 1.0, 1.0);

  vec3 normalized_normal = normalize(vary_normal);
  // Sun direction. The UBO carries dayDir in WOW z-up space. N.L is evaluated in RENDER space, so
  // the light must be converted with the SAME frame the NORMALS use, not the world/position frame.
  // MCNR normals are loaded as (x, z, y) with NO sign flip (MapChunk.cpp), whereas world POSITIONS
  // flip the horizontal axes ((-y, z, -x), MapView.cpp wowToNoggit). Converting the light with the
  // position frame (the old -normalize((-x,z,-y))) therefore lit slopes from the wrong compass
  // direction -- 180 deg opposite the true sun in azimuth (elevation was still right, since "up"
  // is unaffected, which is why the sun DISC looked correctly placed while the ground was lit from
  // the opposite side). Matching the normal frame (x, z, y) fixes it.
  vec3 to_light = -normalize(vec3(LightDir_FogRate.x, LightDir_FogRate.z, LightDir_FogRate.y));
  float nDotL = clamp(dot(normalized_normal, to_light), 0.0, 1.0);

  // Client terrain lighting = fixed-function directional with a SINGLE FLAT ambient (LightIntBand[1]),
  // NOT a sky/ground hemisphere. RE 2026-07-26: the 1.12 FF T&L path writes one D3DLIGHT ambient, and
  // the 335a terrain vertex shader is `min(1, ambient + saturate(N.L)*diffuse)`. The old ambient*1.1
  // sky / *0.7 ground mix (keyed on nDotL) was a noggit invention -- it tinted slopes brighter/darker
  // than the client. See RE_notes/lighting/00_lighting_pipeline_RE.md 7.2.
  currColor = AmbientColor_FogEnd.xyz;
  lDiffuse = DiffuseColor_FogStart.xyz * nDotL;

  // TERRAIN SPECULAR, CLIENT-EXACT (Westfall trace, 25.6k terrain draws + the terrain PS disasm):
  // the client's FF vertex pipeline (SPECULARENABLE=1, LOCALVIEWER=1) computes Blinn specular with
  // material Power = 20 and white material spec; the light's specular colour is the SUN band
  // (LightIntBand band 9). The PS then ADDS v1 * litFactor * blendedLayerAlpha AFTER the diffuse
  // modulate -- tileset ALPHA is the gloss mask, and the shadow map's lit factor gates it.
  // Sun direction: to_light computed once above (shared with the diffuse -- audit complete).
  // to_view comes from POSITIONS, which live in noggit's position frame (-wow.y, wow.z, -wow.x from
  // wowToNoggit), whereas vary_normal and to_light live in the NORMAL frame (wow.x, wow.z, wow.y from
  // the MCNR load). N.L was fine (both normal frame), but the Blinn half-vector mixes to_view's
  // position frame with to_light's normal frame -> the specular highlight landed ~180 deg off. Convert
  // to_view into the normal frame first (position frame -> wow -> normal frame): (-z, y, -x).
  vec3 to_view_pos = normalize(camera - vary_position);
  vec3 to_view = vec3(-to_view_pos.z, to_view_pos.y, -to_view_pos.x);
  vec3 half_vec = normalize(to_light + to_view);
  float spec_scalar = pow(clamp(dot(normalized_normal, half_vec), 0.0, 1.0), 20.0);

  // one shadow sample, shared by the specular lit-mask here and the MCSH darkening below
  float shadow_sample = draw_shadows != 0
    ? texture(shadowmap, vec3(vary_texcoord / 8.0, instanceID)).r
    : 0.0;
  float lit_factor = clamp(1.0 - 3.0 * shadow_sample, 0.0, 1.0); // sample is 85/255 when shadowed

  // blend textures
  out_color = mix(vec4(1.0, 1.0, 1.0, 0.0), texture_blend(), int(instances[instanceID].ChunkHoles_DrawImpass_TexLayerCount_CantPaint.b > 0));

  float blended_layer_alpha = out_color.a; // tileset alpha channels blended like the colours = gloss mask
  out_color.a = 1.0;

  // apply vertex color
  if (draw_vertex_color != 0)
  {
    out_color.rgb *= vary_mccv;
  }

  // apply world lighting (+ emitter point lights). CLIENT-EXACT combine order: clamp the LIGHT to [0,1]
  // FIRST, then modulate the texture -- `tex * min(1, light)`, NOT `min(1, tex*light)`. The old outer-only
  // clamp let bright sunlit surfaces clamp UP past the client (a white sunlit wall read hotter than in-game,
  // exaggerating the city-vs-terrain contrast at e.g. the Stormwind harbor). RE 2026-07-26 (§7.2/§12b).
  out_color.rgb = out_color.rgb * clamp(currColor + lDiffuse + point_lights(vary_position, normalized_normal), 0.0, 1.0);

  // apply overlays
  if(draw_paintability_overlay != 0 && instances[instanceID].ChunkHoles_DrawImpass_TexLayerCount_CantPaint.a != 0)
  {
    out_color *= vec4(1.0, 0.0, 0.0, 1.0);
  }

  if(draw_areaid_overlay != 0)
  {
    out_color.rgb = out_color.rgb * 0.3 + random_color(instances[instanceID].AreaIDColor_Pad2_DrawSelection.r);
  }

  if(draw_impass_overlay != 0 && instances[instanceID].ChunkHoles_DrawImpass_TexLayerCount_CantPaint.g != 0)
  {
    out_color.rgb = mix(vec3(1.0), out_color.rgb, 0.5);
  }

  if(draw_selection_overlay != 0 && instances[instanceID].AreaIDColor_Pad2_DrawSelection.a != 0)
  {
   out_color.rgb = mix(vec3(1.0), out_color.rgb, 0.5);
  }

  if (draw_shadows != 0)
  {
    // MCSH blend, CLIENT-EXACT (disassembled from the 1.12 terrain pixel shaders, wow_cap_upstairs_day:
    // every Terrain*.bls variant ends with `mad w, shadow, 0.3, 0.7; mul color, layers, w` -- a fully
    // shadowed texel renders at exactly 70% brightness, hardcoded, NOT data-driven). Our shadowmap
    // texture stores 85 for a set shadow bit (85/255 = 1/3), so scale by 0.9: 1 - 0.9*(85/255) = 0.7.
    out_color = vec4 (out_color.rgb * (1.0 - 0.9 * shadow_sample), 1.0);
  }

  // Add the terrain specular LAST like the client PS (`mad final, diffusePart, v0, spec` -- spec is
  // NOT texture-modulated and NOT darkened by the 0.7 shadow term; the lit factor gates it instead).
  if (draw_terrain_specular != 0)
  {
    out_color.rgb = clamp(out_color.rgb + sun_spec_color * (spec_scalar * lit_factor * blended_layer_alpha), 0.0, 1.0);
  }

  if (draw_terrain_height_contour != 0)
  {
    out_color = vec4(out_color.rgb * contour_alpha(4.0, vary_position.y+0.1, fw.y), 1.0);
  }

  bool lines_drawn = false;
  if(draw_lines != 0)
  {
    vec4 color = vec4(0.0, 0.0, 0.0, 0.0);

    color.a = contour_alpha(TILESIZE, vary_position.xz, fw.xz * 1.5);
    color.g = color.a > 0.0 ? 0.8 : 0.0;

    if(color.a == 0.0)
    {
      color.a = contour_alpha(CHUNKSIZE, vary_position.xz, fw.xz);
      color.r = color.a > 0.0 ? 0.8 : 0.0;
    }
    if(draw_hole_lines != 0 && color.a == 0.0)
    {
      color.a = contour_alpha(HOLESIZE, vary_position.xz, fw.xz * 0.75);
      color.b = 0.8;
    }

    lines_drawn = color.a > 0.0;
    out_color.rgb = mix(out_color.rgb, color.rgb, color.a);
  }


  if(FogColor_FogOn.w != 0)
  {
    float start = AmbientColor_FogEnd.w * DiffuseColor_FogStart.w; // 0

    vec3 fogParams;
    fogParams.x = -(1.0 / (AmbientColor_FogEnd.w - start)); // - 1 / 338
    fogParams.y = (1.0 / (AmbientColor_FogEnd.w - start)) * AmbientColor_FogEnd.w; // 1 / 338 * 338
    fogParams.z = LightDir_FogRate.w; // 2.7

    float f1 = (dist_from_camera * fogParams.x) + fogParams.y; // 1.0029
    float f2 = max(f1, 0.0);
    float f3 = pow(f2, fogParams.z);
    float f4 = min(f3, 1.0);

    float fogFactor = 1.0 - f4;

    float alpha = clamp((dist_from_camera - start) / (AmbientColor_FogEnd.w - start), 0.0, 1.0);

    out_color.rgb = mix(out_color.rgb, FogColor_FogOn.rgb, fogFactor);
  }

  if(draw_wireframe != 0 && !lines_drawn)
  {
    // true by default => type 0
	  bool draw_wire = true;
      float real_wireframe_radius = max(outer_cursor_radius * wireframe_radius, 2.0 * UNITSIZE);

	  if(wireframe_type == 1)
	  {
		  draw_wire = (length(vary_position.xz - cursor_position.xz) < real_wireframe_radius);
	  }

	  if(draw_wire)
	  {
		  float alpha = contour_alpha(UNITSIZE, vary_position.xz, fw.xz * wireframe_width);
		  float xmod = mod(vary_position.x, UNITSIZE);
		  float zmod = mod(vary_position.z, UNITSIZE);
		  float d = length(fw.xz) * wireframe_width;
		  float diff = min( min(abs(xmod - zmod), abs(xmod - UNITSIZE + zmod))
                      , min(abs(zmod - xmod), abs(zmod + UNITSIZE - zmod))
                      );

		  alpha = max(alpha, 1.0 - smoothstep(0.0, d, diff));
          out_color.rgb = mix(out_color.rgb, wireframe_color.rgb, wireframe_color.a *alpha);
	  }
  }

  if (draw_impassible_climb != 0)
  {
      vec4 color = vec4(out_color.r, out_color.g, out_color.b, 0.5);
      vec3 use_normal;

      if (climb_use_smooth_interpolation != 0)
      {
          use_normal = vary_normal;
      }
      else
      {
          use_normal = triangle_normal;
      }

      float d1 = use_normal.y;
      float d2 = sqrt(use_normal.x * use_normal.x +
                      use_normal.y * use_normal.y +
                      use_normal.z * use_normal.z);

      if (d2 > 0.0)
      {
          float normal_angle = acos(d1/d2);

          if (climb_use_output_angle != 0)
          {
              color.r = normal_angle;
          }
          else
          {
              if (normal_angle > climb_value)
              {
                  color.r = 1.0;
              }
          }
      }


      out_color.rgb = mix(out_color.rgb, color.rgb, color.a);
  }

  if(draw_cursor_circle == 1)
  {
    float diff = length(vary_position.xz - cursor_position.xz);
    diff = min(abs(diff - outer_cursor_radius), abs(diff - outer_cursor_radius * inner_cursor_ratio));
    float alpha = smoothstep(0.0, length(fw.xz), diff);

    out_color.rgb = mix(cursor_color.rgb, out_color.rgb, alpha);
  }
  else if(draw_cursor_circle == 2)
  {
    float angle = cursorRotation * 2.0 * PI;
    vec2 topleft = cursor_position.xz;
    topleft.x -= outer_cursor_radius;
    topleft.y -= outer_cursor_radius;
    vec2 texcoord = (vary_position.xz - topleft) / outer_cursor_radius * 0.5 - 0.5;
    vec2 rotatedTexcoord;
    rotatedTexcoord.x = texcoord.x * cos(angle) + texcoord.y * sin(angle) + 0.5;
    rotatedTexcoord.y = texcoord.y * cos(angle) - texcoord.x * sin(angle) + 0.5;
    /*out_color.rgb = mix(out_color.rgb, texture(stampBrush, rotatedTexcoord).rgb
    , 1.0 * (int(length(vary_position.xz - cursor_position.xz) / outer_cursor_radius < 1.0))
    * (1.0 - length(vary_position.xz - cursor_position.xz) / outer_cursor_radius));*/
    out_color.rgb = mix(out_color.rgb, cursor_color.rgb, texture(stamp_brush, rotatedTexcoord).r
    * int(abs(vary_position.x - cursor_position.x) <= outer_cursor_radius
    && abs(vary_position.z - cursor_position.z) <= outer_cursor_radius));
    /*vec2 posRel = vary_position.xz - cursor_position.xz;
    float pos_x = posRel.x * sin(angle) - posRel.y * cos(angle);
    float pos_z = posRel.y * sin(angle) + posRel.x * cos(angle);
    float diff_x = abs(pos_x);
    float diff_z = abs(pos_z);
    float inner_radius = outer_cursor_radius * inner_cursor_ratio;
    float d = length(fw);
    float alpha = 1.0 * (1 - int((diff_x < outer_cursor_radius && diff_z < outer_cursor_radius
    && (outer_cursor_radius - diff_x <= d || outer_cursor_radius - diff_z <= d)) || (diff_x < inner_radius
    && diff_z < inner_radius && (inner_radius - diff_x <= d || inner_radius - diff_z <= d))));
    out_color.rgb = mix(cursor_color.rgb, out_color.rgb, alpha);*/
  }

  // Terrain opts OUT of bloom: write alpha 0 as the bloom mask (the bright-pass keeps only alpha > 0).
  // Bright/white ground then never blooms, while sky, models and light shafts (alpha > 0) still do.
  out_color.a = 0.0;
}
