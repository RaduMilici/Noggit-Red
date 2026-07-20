// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// The 1.12 sky sun: a camera-facing billboard at the sun direction, far out on the sky. Corners
// from gl_VertexID (TRIANGLE_STRIP, 4 verts) -- no vertex buffer.

uniform mat4 model_view_projection;
uniform vec3 sun_center;   // world position of the sun (camera + dirToSun * distance)
uniform vec3 cam_right;    // camera right axis (world space)
uniform vec3 cam_up;       // camera up axis (world space)
uniform float half_size;   // world half-extent of the sun quad

out vec2 uv;

void main()
{
  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
  uv = c;
  vec3 p = sun_center + (c.x * cam_right + c.y * cam_up) * half_size;
  gl_Position = model_view_projection * vec4(p, 1.0);
}
