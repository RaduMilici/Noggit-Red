// [2026-09-08 WDL HORIZON] The low-res distant terrain (WDL MARE mesh), ported from the 3.3.5a client's
// CMapLowDetail. Position-only vertices in absolute world space, the same mvp as the terrain pass.
//
// The client draws this pass (CMap::RenderLowDetail, wow335a FUN_00795f80) with its own projection
// (near = far clip - 50, far = 4 x farclip, fed through setHorizon) through a viewport whose depth range
// is [511/512, 1023/1024] -- a slice BEHIND the world's depth range, so the mesh is depth-tested against
// everything real and can never win against it: it only fills the pixels nothing real covered. noggit's
// world uses the full [0,1] range (and linearises it in its depth readers), so the equivalent here is to
// pin the mesh to the far end, depth exactly 1.0: LEQUAL passes only where the depth buffer still holds
// the clear value (sky, clouds, celestials write no depth); anything real keeps the pixel.
#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 0) out vec3 v_color;

layout(push_constant) uniform Push
{
  mat4 mvp;     // 0..64  noggit projection * model_view (GL clip conventions)
  vec4 color;   // 64..80 flat fog colour
} pc;

void main()
{
  v_color = pc.color.rgb;
  vec4 clip = pc.mvp * vec4(in_pos, 1.0);
  clip.z = clip.w * 0.9999995;   // a hair inside the far plane (VK clip z in [0,w]); see horizon_vert.glsl
  gl_Position = clip;
}
