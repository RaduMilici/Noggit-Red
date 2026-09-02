// [VULKAN phase E] Liquid surface. Port of the water branch of GL's liquid_frag.glsl: depth-tinted
// zone colour (ocean vs river ramps), with the magma/slime branches taking their own paths.
// STILL GL-ONLY (open TODO): the animated liquid TEXTURE frames, water specular / sun sheen, and the
// authored-ocean gate that decides whether an inland "ocean" chunk uses the river band.
#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec2 v_depth_type;        // x = raw water depth, y = liquid id
layout(location = 4) flat in vec2 v_anim;
layout(location = 5) flat in int v_tex_id;
layout(location = 6) flat in vec4 v_wmo;   // x = flags, yzw = WMO material colour

layout(location = 0) out vec4 out_color;
layout(location = 1) out float out_z;

layout(push_constant) uniform Push
{
  mat4 mvp;
  float time;
} pc;

layout(set = 1, binding = 5) uniform sampler2D tilesets[];   // shared bindless array

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
  vec4 SheenDir_Pad;   // celestial disc direction (GL: sheen_dir)
};

void main()
{
  float depth = v_depth_type.x;
  int type = int(v_depth_type.y + 0.5);

  // GL scrolls magma/slime UVs with animtime and rotates the water UV by anim_uv; the frame itself is
  // already chosen on the feed side.
  vec2 uv = v_uv;
  vec4 texel = v_tex_id >= 0 ? texture(tilesets[nonuniformEXT(v_tex_id)], uv) : vec4(1.0);

  int wmo_flags = int(v_wmo.x + 0.5);
  bool is_wmo = (wmo_flags & 1) != 0;

  // Sun/moon glitter lobe -- identical law in liquid_frag and wmo_liquid_frag.
  float sheen = 1.0;
  if (Camera_Pad.y >= v_world.y)
  {
    vec3 sheen_V = normalize(Camera_Pad.xyz - v_world);
    vec3 sheen_L = SheenDir_Pad.xyz;
    float L_len = length(sheen_L);
    if (L_len > 0.001)
    {
      sheen_L /= L_len;
      sheen_L.y = max(sheen_L.y, 0.14);
      sheen_L = normalize(sheen_L);
      float sheen_align = clamp(dot(reflect(-sheen_V, vec3(0.0, 1.0, 0.0)), sheen_L), 0.0, 1.0);
      sheen = 0.25 + 1.25 * pow(sheen_align, 8.0);
    }
  }

  vec4 col;
  if (is_wmo)
  {
    // [phase E] WMO liquid (harbour/canal/cave pools). Port of wmo_liquid_frag.glsl: its colour law
    // is NOT the ADT one -- no ocean-pull on the river band, a flat city-channel body, and an
    // interior branch that ADDS the WMO material's baked diffColor to the texture.
    if (type == 2)
    {
      col = vec4(texel.rgb, 1.0);
    }
    else if (type == 3)
    {
      col = texel;
    }
    else
    {
      vec4 liquid_color = (type == 1)
        ? mix(OceanColorLight, OceanColorDark, depth)
        : mix(RiverColorLight, RiverColorDark, depth);
      float water_alpha = liquid_color.a;
      vec3 water_rgb;
      if ((wmo_flags & 2) != 0)
      {
        // INTERIOR: stage0 is ADD(TEXTURE, DIFFUSE) with lighting off -- no zone tint, no sheen.
        water_rgb = texel.rgb + v_wmo.yzw;
      }
      else
      {
        float pattern = texel.a;
        if ((wmo_flags & 4) != 0)
        {
          // City channel (SW canals/harbour, Booty Bay): ocean-deep navy body, opaque.
          water_rgb = mix(OceanColorDark.rgb, OceanColorLight.rgb, 0.12) * 0.85;
          water_alpha = RiverColorDark.a;
        }
        else
        {
          water_rgb = liquid_color.rgb * 0.85;
        }
        water_rgb += (vec3(pattern) * 0.30 + pattern * water_rgb) * sheen;
      }
      col = vec4(clamp(water_rgb, 0.0, 1.0), water_alpha);
    }
  }
  else if (type == 2)        // magma: opaque and emissive
  {
    col = vec4(texel.rgb, 1.0);
  }
  else if (type == 3)   // slime -- kept below the emissive bloom range (lava only)
  {
    col = vec4(texel.rgb, min(texel.a, 0.85));
  }
  else
  {
    // Depth ramps (liquid_frag): ocean fades light -> dark over ~80 units, rivers reach the deep band
    // by ~20, alpha goes ~opaque by ~12 so the seafloor stops showing through.
    float ocean_color_depth = clamp(depth * 0.012, 0.0, 1.0);
    float river_color_depth = clamp(depth * 0.05,  0.0, 1.0);
    float alpha_depth       = clamp(depth * 0.08,  0.0, 1.0);

    float ocean_lum = max(max(OceanColorLight.r, OceanColorLight.g), OceanColorLight.b);
    bool ocean_bright = ocean_lum > 0.1;
    bool use_ocean = (type == 1) && ocean_bright;

    float cd = use_ocean ? ocean_color_depth : river_color_depth;
    vec4 lerp;
    if (use_ocean)
    {
      lerp = mix(OceanColorLight, OceanColorDark, cd);
    }
    else
    {
      // A coastal zone (ocean band authored) pulls the river band 75% toward the ocean band and
      // darkens it x0.72 -- keeps city canals teal instead of grass-green. Inland zones, whose ocean
      // band is near-black, keep their pure river band.
      vec3 river_shallow = ocean_bright
                         ? mix(RiverColorLight.rgb, OceanColorLight.rgb, 0.75) * 0.72
                         : RiverColorLight.rgb;
      vec3 river_deep    = ocean_bright
                         ? mix(RiverColorDark.rgb,  OceanColorDark.rgb,  0.75) * 0.72
                         : RiverColorDark.rgb;
      lerp = vec4(mix(river_shallow, river_deep, cd),
                  mix(RiverColorLight.a, RiverColorDark.a, cd));
    }

    vec3 water_rgb = lerp.rgb * 0.85;

    // The liquid texture enters ONLY here, as the ripple pattern in its ALPHA -- never as a colour
    // modulate. TODO: GL scales this by a sun-sheen term built from the celestial disc direction
    // (`sheen_dir`), which the VK lighting block does not carry yet; held at 1.0 for now.
    // Sun sheen, from liquid_frag: the glitter tracks the drawn sun/moon disc, lifted to at least
    // ~8 degrees above the horizon so the lobe can actually align.
    float pat = texel.a;
    float sheen = 1.0;
    if (Camera_Pad.y >= v_world.y)
    {
      vec3 sheen_V = normalize(Camera_Pad.xyz - v_world);
      vec3 sheen_L = SheenDir_Pad.xyz;
      float L_len = length(sheen_L);
      if (L_len > 0.001)
      {
        sheen_L /= L_len;
        sheen_L.y = max(sheen_L.y, 0.14);
        sheen_L = normalize(sheen_L);
        float sheen_align = clamp(dot(reflect(-sheen_V, vec3(0.0, 1.0, 0.0)), sheen_L), 0.0, 1.0);
        sheen = 0.25 + 1.25 * pow(sheen_align, 8.0);
      }
    }
    water_rgb += (vec3(pat) * 0.30 + pat * lerp.rgb) * sheen;
    water_rgb = clamp(water_rgb, 0.0, 1.0);

    float shallow_a = (type == 1) ? OceanColorLight.a : RiverColorLight.a;
    float deep_a    = (type == 1) ? OceanColorDark.a  : RiverColorDark.a;
    col = vec4(water_rgb, clamp(mix(shallow_a, deep_a, alpha_depth), 0.0, 1.0));
  }

  if (Toggles.y != 0.0 && FogColor_FogOn.w != 0.0)
  {
    // WMO liquid fogs with the CAMERA's fog context (Env slots) when one is active -- it sits
    // inside a building; ADT water always uses the outdoor fog.
    bool use_env = is_wmo && EnvFogColor_On.w > 0.5;
    vec3 fog_rgb  = use_env ? EnvFogColor_On.rgb : FogColor_FogOn.rgb;
    float fog_end = use_env ? EnvFogDist.y : AmbientColor_FogEnd.w;
    float start   = (use_env ? EnvFogDist.x : DiffuseColor_FogStart.w) * fog_end;
    float dist = distance(Camera_Pad.xyz, v_world);
    float f1 = (dist * (-(1.0 / (fog_end - start)))) + ((1.0 / (fog_end - start)) * fog_end);
    float f4 = clamp(pow(max(f1, 0.0), LightDir_FogRate.w), 0.0, 1.0);
    col.rgb = mix(col.rgb, fog_rgb, 1.0 - f4);
    // A fully-fogged emissive liquid must stop blooming: the bright pass treats alpha > 0.88 as
    // emissive. Ordinary water alpha is untouched by the min(). Omitting this made VK water
    // uniformly ~2 LSB darker than GL wherever fog was partial (it stayed more opaque).
    col.a = min(col.a, mix(0.85, 1.0, f4));
  }

  // Seen from BELOW, water must never enter the emissive range (the distant surface otherwise
  // glowed as a bright band through the water fog).
  if (type != 2 && Camera_Pad.y < v_world.y)
    col.a = min(col.a, 0.85);

  out_color = col;
  out_z = gl_FragCoord.z;
}
