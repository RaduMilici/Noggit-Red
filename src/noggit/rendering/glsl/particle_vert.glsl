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
uniform int scale_with_instance;
uniform vec3 camera;

void main()
{
  f_uv = uv;
  f_color = color;

  vec4 pos = transform * vec4(position, 1.0);
  if(billboard == 1)
  {
    // Emitter flag 0x8 = particle size scales with the model's scale (trace-verified on Anomalus,
    // scale 5: aura flare x5, feet smoke / rising stars x1). Offsets are authored in model units.
    float inst_scale = (scale_with_instance == 1) ? length(transform[0].xyz) : 1.0;
    pos.xyz += offset * inst_scale;
  }

  f_dist = distance(camera, pos.xyz);
  // Camera-relative: model_view_projection is projection*view_rot (rotation only), so subtract the
  // camera before it -- keeps particles locked to the (also camera-relative) models/terrain, no jitter.
  gl_Position = model_view_projection * vec4(pos.xyz - camera, 1.0);
}
