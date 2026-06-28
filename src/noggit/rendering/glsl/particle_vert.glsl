// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

layout(location = 0) in mat4 transform;
layout(location = 4) in vec3 position;
layout(location = 5) in vec3 offset;
layout(location = 6) in vec2 uv;
layout(location = 7) in vec4 color;

out vec2 f_uv;
out vec4 f_color;
out float f_dist; // distance from camera, for fog

uniform mat4 model_view_projection;
uniform int billboard;
uniform vec3 camera;

void main()
{
  f_uv = uv;
  f_color = color;

  vec4 pos = transform * vec4(position, 1.0);
  if(billboard == 1)
  {
    pos.xyz += offset;
  }

  f_dist = distance(camera, pos.xyz);
  gl_Position = model_view_projection * pos;
}
