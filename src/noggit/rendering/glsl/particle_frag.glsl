// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

in vec2 f_uv;
in vec4 f_color;
in float f_dist;

out vec4 out_color;

// Only the first members are used for fog; std140 prefix matches the shared lighting UBO.
layout (std140) uniform lighting
{
  vec4 DiffuseColor_FogStart;
  vec4 AmbientColor_FogEnd;
  vec4 FogColor_FogOn;
  vec4 LightDir_FogRate;
};

uniform sampler2DArray tex;
uniform int tex_index;

uniform float alpha_test;
uniform int particle_blend; // particle blend mode (3,4 = additive -> fade to black in fog)

void main()
{
  vec4 t = texture(tex, vec3(f_uv, tex_index));

  if(t.a < alpha_test)
  {
    discard;
  }

  out_color = vec4(f_color.rgb * t.rgb, f_color.a * t.a);

  // Fog: smoke/dust particles should fade into the haze like the rest of the scene. Additive
  // particles (blend 3/4 -- fire/glow) fade to black so they dissolve; everything else fades toward
  // the fog colour.
  if (FogColor_FogOn.w != 0.0)
  {
    float start = AmbientColor_FogEnd.w * DiffuseColor_FogStart.w;
    float denom = AmbientColor_FogEnd.w - start;
    float f1 = (f_dist * (-(1.0 / denom))) + ((1.0 / denom) * AmbientColor_FogEnd.w);
    float fogFactor = 1.0 - min(pow(max(f1, 0.0), LightDir_FogRate.w), 1.0);

    if (particle_blend == 3 || particle_blend == 4)
    {
      out_color.rgb *= (1.0 - fogFactor);
    }
    else
    {
      out_color.rgb = mix(out_color.rgb, FogColor_FogOn.rgb, fogFactor);
    }
  }
}
