// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Composite: original scene + blurred bloom. The add makes bright areas bleed/glow onto their
// surroundings and blow out toward white -- the overbearing golden-light look of cave openings etc.
in vec2 uv;
out vec4 out_color;

uniform sampler2D scene;
uniform sampler2D bloom;
uniform float intensity;

void main()
{
  vec3 c = texture(scene, uv).rgb;
  vec3 b = texture(bloom, uv).rgb;
  // MEASURED composite (wow_cap_upstairs.trace, RE_notes/16): oC0 = scene + weight * blur^2 with
  // weight = the zone glow (0.647 in the inn frame, carried in the quad vertex alpha). blur is
  // SQUARED -- dark blur contributes ~nothing, bright areas glow warm -- preserving contrast
  // instead of washing the frame.
  out_color = vec4(c + b * b * intensity, 1.0);
}
