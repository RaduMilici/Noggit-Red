// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

in vec4 position;

uniform mat4 model_view;
uniform mat4 projection;

void main()
{
  vec4 clip = projection * model_view * position;
  // [2026-09-08 WDL HORIZON] The 3.3.5a client draws the low-res WDL mesh (CMap::RenderLowDetail,
  // wow335a FUN_00795f80) with its own projection (near = far clip - 50, far = 4 x farclip) through a
  // viewport whose depth range is [511/512, 1023/1024] -- a slice BEHIND the world's depth range, so
  // the mesh is depth-tested against the real scene and can never win against real geometry: it only
  // fills the pixels nothing real covered. noggit's world uses the full [0,1] range and its depth
  // readers (water fade, decals, selection) linearise it, so the world range cannot be partitioned
  // here; the equivalent is to pin the mesh to the far end (depth exactly 1.0): LEQUAL passes only
  // where the depth buffer still holds the clear value (sky, clouds, celestials write no depth), and
  // anything real -- including the fully fogged 50 yd overlap band -- keeps the pixel.
  // A hair inside the far plane (a few 24-bit depth quanta) so no implementation can clip it away
  // or fail LEQUAL on a rounding difference against the cleared 1.0.
  clip.z = clip.w * 0.9999995;
  gl_Position = clip;
}
