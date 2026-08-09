// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

uniform vec3 color;

out vec4 out_color;

void main()
{
  // Alpha 0 = opt OUT of bloom (the bloom bright-pass keeps only alpha > 0). This distant low-res
  // terrain is tinted toward the (bright) fog colour, so without this it blooms heavily under fog.
  out_color = vec4(color, 0.0);
}
