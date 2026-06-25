// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

layout(location = 0) in mat4 transform;
layout(location = 4) in vec3 position;
layout(location = 5) in vec3 offset;
layout(location = 6) in vec2 uv;
layout(location = 7) in vec4 color;

out vec2 f_uv;
out vec4 f_color;

uniform mat4 model_view_projection;
uniform int billboard;

void main()
{
  f_uv = uv;
  f_color = color;

  if(billboard == 1)
  { 
    vec4 pos = transform * vec4(position, 1.0);
    pos.xyz += offset;
    gl_Position = model_view_projection * pos;
  }
  else
  {
    gl_Position = model_view_projection * transform * vec4(position, 1.0);
  }
}
