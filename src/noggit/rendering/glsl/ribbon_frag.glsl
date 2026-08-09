// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

in vec2 f_uv;
in float f_dist;

out vec4 out_color;

// Full shared lighting UBO layout (std140) -- the Env slots at the end need every preceding member
// declared so their offsets match.
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
  vec4 PointLightParams;
  vec4 PointLightPos[16];
  vec4 PointLightColor[16];
  vec4 EnvFogColor_On;       // rgb = entity fog colour (camera's fog context), w = 1 when active
  vec4 EnvFogDist;           // x = start FRACTION of end (can be negative), y = end
};

uniform sampler2DArray tex;
uniform int tex_index;
uniform vec4 color;
uniform int ribbon_blend; // M2 blend mode of the ribbon material (3/4 = additive -> fog to black)

void main()
{
  vec4 t = texture(tex, vec3(f_uv, tex_index));
  out_color = vec4(color.rgb * t.rgb, color.a * t.a);

  // Ribbons fog like every other entity (camera fog context; additive trails dissolve to black).
  if (FogColor_FogOn.w != 0.0)
  {
    bool use_env = EnvFogColor_On.w > 0.5;
    vec3 fog_color_r = use_env ? EnvFogColor_On.rgb : FogColor_FogOn.rgb;
    float fog_end_r = use_env ? EnvFogDist.y : AmbientColor_FogEnd.w;
    float fog_start_frac_r = use_env ? EnvFogDist.x : DiffuseColor_FogStart.w;

    float start = fog_end_r * fog_start_frac_r;
    float denom = fog_end_r - start;
    float f1 = (f_dist * (-(1.0 / denom))) + ((1.0 / denom) * fog_end_r);
    float fogFactor = 1.0 - min(pow(max(f1, 0.0), LightDir_FogRate.w), 1.0);

    if (ribbon_blend == 3 || ribbon_blend == 4)
    {
      out_color.rgb *= (1.0 - fogFactor);
    }
    else
    {
      out_color.rgb = mix(out_color.rgb, fog_color_r, fogFactor);
    }
  }
}
