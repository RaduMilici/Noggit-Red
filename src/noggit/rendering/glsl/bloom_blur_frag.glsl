// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Separable 9-tap gaussian blur. Run alternately horizontal/vertical (ping-pong) for a wide blur.
in vec2 uv;
out vec4 out_color;

uniform sampler2D image;
uniform int horizontal;
uniform vec2 texel; // 1/width, 1/height of this (half-res) target

void main()
{
  float weight[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
  vec2 dir = (horizontal != 0) ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);

  vec3 result = texture(image, uv).rgb * weight[0];
  for (int i = 1; i < 5; ++i)
  {
    result += texture(image, uv + dir * float(i)).rgb * weight[i];
    result += texture(image, uv - dir * float(i)).rgb * weight[i];
  }
  out_color = vec4(result, 1.0);
}
