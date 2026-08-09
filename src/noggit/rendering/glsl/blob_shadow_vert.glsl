// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Unit blob shadow as a SCREEN-SPACE PROJECTED DECAL (client parity: wow.exe paints the blob onto
// the terrain triangles under the unit @006d7480; we reconstruct the scene position from the depth
// buffer instead, which also handles WMO floors and doodads). The quad only provides screen
// coverage: it is the unit's bbox footprint rect EXPANDED so sloped ground above/below the foot
// plane still falls inside; the fragment shader re-derives the true decal UV from the scene depth.

uniform mat4 model_view_projection;
uniform vec3 center;   // footprint centre at ground height (world space)
uniform vec3 axis_x;   // world-space half-extent vector (unit facing, horizontal)
uniform vec3 axis_y;   // world-space half-extent vector (perpendicular, horizontal)
uniform vec2 expand;   // coverage expansion factor per axis (>= 1)

void main()
{
  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
  vec3 p = center + c.x * axis_x * expand.x + c.y * axis_y * expand.y;
  gl_Position = model_view_projection * vec4(p, 1.0);
}
