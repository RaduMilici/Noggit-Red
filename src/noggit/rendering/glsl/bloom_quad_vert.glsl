// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Fullscreen triangle generated from gl_VertexID -- no vertex buffer needed. Draw with
// glDrawArrays(GL_TRIANGLES, 0, 3). Covers the whole screen; uv spans 0..1.
out vec2 uv;

void main()
{
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
