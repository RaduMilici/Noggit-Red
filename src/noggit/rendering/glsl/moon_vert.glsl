// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Camera-facing textured billboard for the moon (Environments\Stars\Moon.blp). Corners from
// gl_VertexID (TRIANGLE_STRIP, 4 verts); uv spans 0..1 for the texture.
uniform mat4 model_view_projection;
uniform vec3 center;     // world position of the moon
uniform vec3 camera_pos; // for the per-fragment horizon clip
uniform vec3 cam_right;  // camera right (world)
uniform vec3 cam_up;     // camera up (world)
uniform float half_size; // world half-extent of the quad

out vec2 uv;
out vec3 f_dir; // camera -> fragment (world), for the horizon clip

void main()
{
  vec2 c01 = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  uv = c01;
  vec2 c = c01 * 2.0 - 1.0;
  vec3 p = center + (c.x * cam_right + c.y * cam_up) * half_size;
  f_dir = p - camera_pos;
  gl_Position = model_view_projection * vec4(p, 1.0);
}
