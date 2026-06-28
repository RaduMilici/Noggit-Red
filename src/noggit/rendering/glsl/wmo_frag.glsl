// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 410 core

// flags
#define eWMOBatch_ExteriorLit 0x1u
#define eWMOBatch_HasMOCV 0x2u
#define eWMOBatch_Unlit 0x4u
#define eWMOBatch_Unfogged 0x8u

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
// 1 when this WMO has exterior groups (an "open" WMO, e.g. a cave mouth). Lets outdoor zone light
// bleed into its interior groups; 0 for fully enclosed dungeons (keep their dark authored interior).
uniform int wmo_open;

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

vec3 apply_lighting(vec3 material)
{
  // MOCV = baked per-vertex lighting (interior shadow/light). Already processed Blizzard-style on
  // load (CMapObjGroup::FixColorVertexAlpha). For groups without it the contribution is 0.
  vec3 vertex_color = bool(flags & eWMOBatch_HasMOCV) ? f_vertex_color.rgb : vec3(0.);

  vec3 light_color;

  if (bool(flags & eWMOBatch_Unlit))
  {
    // Self-lit: just the baked color + the relevant ambient.
    light_color = vertex_color
                + (bool(flags & eWMOBatch_ExteriorLit) ? AmbientColor_FogEnd.xyz : ambient_color);
  }
  else if (bool(flags & eWMOBatch_ExteriorLit))
  {
    // Exterior geometry: outdoor sun diffuse (N.L) + outdoor ambient (+ any baked color).
    float nDotL = clamp(dot(normalize(f_normal),
                            -normalize(vec3(-LightDir_FogRate.x, LightDir_FogRate.z, -LightDir_FogRate.y))),
                        0.0, 1.0);
    light_color = clamp(DiffuseColor_FogStart.xyz * nDotL, 0.0, 1.0)
                + AmbientColor_FogEnd.xyz
                + vertex_color;
  }
  else
  {
    // Interior geometry: lit purely by the WMO's own ambient + the baked MOCV. Matches the client
    // (and reference noggit3) -- NO outdoor-ambient floor / sun diffuse / sky-ground hemispheric
    // mix, all of which previously flooded out the baked shadows and made interiors washed-out.
    //
    // A near-white MOHD ambient is a "no extra ambient" sentinel (e.g. Timbermaw instance = (1,1,1)):
    // the baked MOCV already is the interior light, so adding literal white would flood the scene.
    // Treat near-white as a tiny floor; otherwise honour the artist-authored ambient (e.g.
    // md_timbermawhold's dim purple) as the dev-designed interior fill light.
    vec3 interior_ambient = all(greaterThan(ambient_color, vec3(0.95))) ? vec3(0.04) : ambient_color;
    // Open WMOs (cave mouths): approximate outdoor light spilling in by lifting the interior ambient
    // floor toward the outdoor zone ambient. max() keeps already-bright baked MOCV intact and only
    // fills the shadowed areas, so it warms/brightens the cave without flattening it. Enclosed
    // dungeons (wmo_open == 0) are untouched and stay dark/moody.
    vec3 interior_sun = vec3(0.0);
    if (wmo_open != 0)
    {
      interior_ambient = max(interior_ambient, AmbientColor_FogEnd.xyz * 0.6);
      // Open cave: the outdoor sun spills in, so add its (time-varying) diffuse on lit-facing surfaces.
      float nDotL = clamp(dot(normalize(f_normal),
                              -normalize(vec3(-LightDir_FogRate.x, LightDir_FogRate.z, -LightDir_FogRate.y))),
                          0.0, 1.0);
      interior_sun = DiffuseColor_FogStart.xyz * nDotL * 0.5;
    }
    light_color = interior_ambient + interior_sun + vertex_color;

    // Make the interior track TIME OF DAY. The wmo_open heuristic misses open caves like
    // timbermaw_instance (0 exterior groups), so do this for ALL interiors: scale the baked interior
    // light by the outdoor ambient brightness so the WMO darkens at night / brightens by day, and add
    // a small time-varying tint so it warms at dusk -- matching the doodads sitting in the same cave.
    float outdoor_lum = dot(AmbientColor_FogEnd.xyz, vec3(0.2126, 0.7152, 0.0722));
    light_color *= clamp(outdoor_lum * 2.2, 0.3, 1.15);
    light_color += AmbientColor_FogEnd.xyz * 0.15;
  }

  light_color += point_lights(f_position, normalize(f_normal));

  return clamp(material.rgb * light_color, 0.0, 1.0);
}

void main()
{

  float dist_from_camera = distance(camera, f_position);
  bool fog = FogColor_FogOn.w != 0 && !bool(flags & eWMOBatch_Unfogged);

  vec4 tex = get_tex_color(f_texcoord, tex_array0, int(tex0));
  vec4 tex_2 = get_tex_color(f_texcoord_2, tex_array1, int(tex1));

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
    float start = AmbientColor_FogEnd.w * DiffuseColor_FogStart.w;

    vec3 fogParams;
    fogParams.x = -(1.0 / (AmbientColor_FogEnd.w - start));
    fogParams.y = (1.0 / (AmbientColor_FogEnd.w - start)) * AmbientColor_FogEnd.w;
    fogParams.z = LightDir_FogRate.w;

    float f1 = (dist_from_camera * fogParams.x) + fogParams.y;
    float f2 = max(f1, 0.0);
    float f3 = pow(f2, fogParams.z);
    float f4 = min(f3, 1.0);

    float fogFactor = 1.0 - f4;

    out_color.rgb = mix(out_color.rgb, FogColor_FogOn.rgb, fogFactor);
    bloom_mask = 1.0 - fogFactor; // fog-brightened surfaces opt out of bloom (fully fogged -> 0)
  }

  if(out_color.a < alpha_test)
  {
    discard;
  }

  // Write the bloom mask into alpha (read by the bloom bright-pass) AFTER the alpha-test discard so it
  // can't punch holes in cutouts. Without this, geometry tinted toward a bright fog colour (e.g.
  // distant Stormwind buildings) crosses the low bloom threshold and blooms heavily under fog.
  out_color.a = bloom_mask;
}
