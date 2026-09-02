// [VULKAN phase G] Particle fragment -- port of particle_frag.glsl.
#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec3 v_world;

layout(location = 0) out vec4 out_color;
layout(location = 1) out float out_z;

layout(set = 1, binding = 5) uniform sampler2D tilesets[];

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
  vec4 SheenDir_Pad;
  vec4 CamFwd_DetailDist;
};

layout(push_constant) uniform Push
{
  mat4 mvp;
  vec4 tex_blend_alpha;
} pc;

void main()
{
  int id = int(pc.tex_blend_alpha.x);
  // Ribbons ride this same pipeline family; +16 on the blend slot marks them. They skip the
  // particle-only black-fringe divide (their textures are not authored over transparent black).
  int blend_raw = int(pc.tex_blend_alpha.y);
  bool is_ribbon = blend_raw >= 16;
  int blend = blend_raw & 15;
  float alpha_test = pc.tex_blend_alpha.z;
  float alpha_mod = pc.tex_blend_alpha.w;

  vec4 t = id >= 0 ? texture(tilesets[nonuniformEXT(id)], v_uv) : vec4(1.0);
  if (t.a < alpha_test)
    discard;

  // Black-fringe fix: these textures store an opaque shape over TRANSPARENT BLACK, so bilinear
  // filtering darkens the edges. Dividing by alpha undoes it (premultiplied compositing). Additive
  // modes already handle a black background correctly.
  vec3 t_rgb = (!is_ribbon && blend == 2 && t.a > 0.0039) ? clamp(t.rgb / t.a, 0.0, 1.0) : t.rgb;

  out_color = vec4(v_color.rgb * t_rgb, v_color.a * t.a * alpha_mod);

  if (Toggles.y != 0.0 && FogColor_FogOn.w != 0.0)
  {
    bool use_env = EnvFogColor_On.w > 0.5;
    vec3 fog_color_p = use_env ? EnvFogColor_On.rgb : FogColor_FogOn.rgb;
    float fog_end_p = use_env ? EnvFogDist.y : AmbientColor_FogEnd.w;
    float fog_start_frac_p = use_env ? EnvFogDist.x : DiffuseColor_FogStart.w;
    float start = fog_end_p * fog_start_frac_p;
    float denom = fog_end_p - start;
    float dist = distance(Camera_Pad.xyz, v_world);
    float f1 = (dist * (-(1.0 / denom))) + ((1.0 / denom) * fog_end_p);
    float fogFactor = 1.0 - min(pow(max(f1, 0.0), LightDir_FogRate.w), 1.0);

    // Additive particles (fire/glow) dissolve to black; everything else fades to the fog colour.
    if (blend == 3 || blend == 4)
      out_color.rgb *= (1.0 - fogFactor);
    else
      out_color.rgb = mix(out_color.rgb, fog_color_p, fogFactor);
  }

  out_z = gl_FragCoord.z;
}
