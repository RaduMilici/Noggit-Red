// [VULKAN phase F] Camera-facing celestial billboard (sun disc + glare, moon discs + glare). Port of
// moon_vert.glsl. Corners come from gl_VertexIndex, no vertex buffer.
//
// The GL shader takes an absolute `center` plus `camera_pos` and derives the view ray from them.
// Push space is only 128 bytes guaranteed, and both would not fit, so the CPU sends the center
// RELATIVE to the camera and folds the camera translation into the matrix (mvp * translate(camera)).
// The view ray is then just the camera-relative position -- no camera uniform needed at all.
#version 450

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_dir;   // camera -> fragment, for the horizon clip

layout(push_constant) uniform Push
{
  mat4 mvp_cam;        // projection * view * translate(camera)
  vec4 center_half;    // xyz = centre relative to the camera, w = world half-extent
  vec4 right_opacity;  // xyz = camera right, w = opacity
  vec4 up_tex;         // xyz = camera up,    w = bindless texture index
  vec4 color_additive; // rgb = tint,         w = 1 for the premultiplied additive glow
} pc;

void main()
{
  vec2 c01 = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
  v_uv = c01;
  vec2 c = c01 * 2.0 - 1.0;
  vec3 p = pc.center_half.xyz + (c.x * pc.right_opacity.xyz + c.y * pc.up_tex.xyz) * pc.center_half.w;
  v_dir = p;   // already camera-relative
  vec4 clip = pc.mvp_cam * vec4(p, 1.0);
  clip.z = (clip.z + clip.w) * 0.5;   // GL clip -> VK clip
  gl_Position = clip;
}
