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

// Per-instance opacity = CreatureDisplayInfo.CreatureModelAlpha (0..1). The client applies this to the
// WHOLE creature model -- mesh AND particles -- but Noggit only applied it to the mesh, so energy-
// elemental smoke rendered at full opacity (too solid). 1.0 for everything that isn't a faded creature.
uniform float particle_alpha_mod;

void main()
{
  vec4 t = texture(tex, vec3(f_uv, tex_index));

  if(t.a < alpha_test)
  {
    discard;
  }

  // Fix the "black fringe": these textures store an opaque shape over a TRANSPARENT BLACK background
  // (RGB 0, alpha 0). Bilinear filtering blends the shape with that black bg, darkening the edges, so an
  // alpha-blended (mode 2) particle gets a dark halo / the black background appears to bleed in. Dividing
  // the filtered RGB by its alpha undoes that darkening (equivalent to premultiplied compositing),
  // recovering the shape's true colour at the edges. Only for alpha-blend; additive (3/4) already
  // handles a black background correctly (black adds nothing).
  vec3 t_rgb = (particle_blend == 2 && t.a > 0.0039) ? clamp(t.rgb / t.a, 0.0, 1.0) : t.rgb;

  out_color = vec4(f_color.rgb * t_rgb, f_color.a * t.a * particle_alpha_mod);

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
