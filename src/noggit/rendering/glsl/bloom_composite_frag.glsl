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
  out_color = vec4(c + b * intensity, 1.0);
}
