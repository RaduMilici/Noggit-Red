// [VULKAN phase F] SKY GRADIENT DOME -- the real one. Port of the inline program in Sky.cpp: a dome
// mesh whose per-vertex colours are the zone's interpolated sky bands (Skies::update_color_buffer),
// translated to sit on the camera. Replaces the hardcoded zenith/horizon gradient placeholder.
//
// Layout (stride 24): pos xyz@0 | colour rgb@12
#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_color;

layout(location = 0) out vec3 v_color;

layout(push_constant) uniform Push
{
  mat4 mvp;
  vec4 camera;   // xyz = camera position; the dome is authored around the origin
} pc;

void main()
{
  v_color = in_color;
  vec4 clip = pc.mvp * vec4(in_pos + pc.camera.xyz, 1.0);
  // GL clip -> VK clip (z in [0, w]); y stays (VK y-down + un-flipped GL blit cancel)
  clip.z = (clip.z + clip.w) * 0.5;
  gl_Position = clip;
}
