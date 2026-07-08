// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

in mat4 transform;
in vec4 position;
in vec2 uv;

out vec2 f_uv;

uniform mat4 model_view_projection; // camera-relative: projection * view_rot (rotation only)
uniform vec3 camera;

void main()
{
  f_uv = uv;
  // Camera-relative (see particle_vert / terrain_vert): subtract camera before the rotation-only MVP.
  vec4 world_pos = transform * position;
  gl_Position = model_view_projection * vec4(world_pos.xyz - camera, 1.0);
}
