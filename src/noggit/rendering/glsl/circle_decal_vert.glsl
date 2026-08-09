// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Selection circle as a SCREEN-SPACE PROJECTED DECAL (matches the blob shadow): the quad only
// provides screen coverage; the fragment shader reconstructs each pixel's world position from the
// scene depth and paints the UnitSelectTexture ring onto the actual terrain / WMO ground under the
// unit -- wrapping the existing mesh instead of a flat draped disc.

uniform mat4 model_view_projection;
uniform vec3 center;   // unit ground position (world space)
uniform float radius;  // circle radius (world units)
uniform float expand;  // coverage expansion so sloped ground stays inside the quad on screen

void main()
{
  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
  vec3 p = center + vec3(c.x * radius * expand, 0.0, c.y * radius * expand);
  gl_Position = model_view_projection * vec4(p, 1.0);
}
