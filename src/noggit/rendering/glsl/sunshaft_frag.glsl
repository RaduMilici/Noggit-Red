// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Screen-space SUNSHAFT (radial light scatter / "god rays"). For each pixel we march a series of
// samples along the line toward the sun's screen position, accumulating the BRIGHT (near-white sun)
// pixels with a decay -- this smears the sun radially into rays. The rays reach farther across the
// frame the closer the sun is to screen centre, so they visibly grow as the camera looks straight
// at the sun (exactly the client behaviour). Drawn ADDITIVE over the composited scene.

in vec2 uv;
out vec4 out_color;

uniform sampler2D scene;   // full composited scene colour (the bright sun lives in here)
uniform vec2 sun_uv;       // sun position in screen space [0,1]
uniform float density;     // fraction of the sun->pixel vector covered by the march (ray reach)
uniform float weight;      // per-sample weight
uniform float decay;       // per-sample falloff
uniform float strength;    // overall additive strength (view-alignment faded on the CPU side)

const int SAMPLES = 40;

// Only the very bright sun feeds the shafts (not the whole scene) -> clean rays instead of a wash.
float sun_mask(vec3 c)
{
  float m = max(c.r, max(c.g, c.b));
  return smoothstep(0.72, 1.0, m);
}

void main()
{
  vec2 delta = (uv - sun_uv) * (density / float(SAMPLES));
  vec2 coord = uv;
  float illum = 1.0;
  vec3 shaft = vec3(0.0);

  for (int i = 0; i < SAMPLES; ++i)
  {
    coord -= delta;
    vec3 s = texture(scene, coord).rgb;
    shaft += s * sun_mask(s) * illum * weight;
    illum *= decay;
  }

  out_color = vec4(shaft * strength, 1.0);
}
