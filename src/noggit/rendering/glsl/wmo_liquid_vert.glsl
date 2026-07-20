// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 410 core

layout (std140) uniform matrices
{
  mat4 model_view;
  mat4 projection;
  vec4 camera_pos;
};

uniform mat4 transform;

in vec3 position;
in vec2 tex_coord;
in float depth;

out vec2 tex_coord_;
out float depth_;
out float dist_from_camera_;
out vec3 world_pos_; // for the additive water shine (sun band) in the fragment shader

void main()
{
  vec4 world_position = transform * vec4(position, 1.0);
  world_pos_ = world_position.xyz;
  // Camera-relative (see terrain_vert.glsl): stops WMO water jittering against the world.
  mat4 view_rot = model_view;
  view_rot[3].xyz = vec3(0.0);
  vec4 view_position = view_rot * vec4(world_position.xyz - camera_pos.xyz, 1.0);

  tex_coord_ = tex_coord;
  depth_ = depth;
  dist_from_camera_ = length(view_position.xyz);

  gl_Position = projection * view_position;
}
