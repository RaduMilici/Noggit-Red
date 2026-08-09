// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Patrol path as a SCREEN-SPACE PROJECTED DECAL -- the same machinery as the spawn selection circle
// (circle_decal_*). The geometry drawn here is NOT the path: it is only a coverage volume, one box
// per polyline segment, sized to contain every pixel the ribbon could possibly touch. The fragment
// shader reconstructs each covered pixel's surface position from the scene depth and paints the
// ribbon onto whatever mesh is actually visible there, so the route drapes over terrain and WMO
// floors instead of a straight world-space line clipping through them.

uniform mat4 model_view_projection; // CAMERA-RELATIVE (rotation-only view), see WorldRender
uniform float px_scale;   // world units per pixel, per unit of view depth
uniform float min_pixels; // pixel-width floor -- distant routes stay visible, so the box must fit it
uniform float world_width;

in vec3 seg_a; // segment start, camera-relative
in vec3 seg_b; // segment end, camera-relative

flat out vec3 v_a;
flat out vec3 v_b;
flat out float v_up;   // how far ABOVE the route a surface may be and still count as its ground
flat out float v_down; // how far BELOW

void main()
{
  // Once the pixel-width floor kicks in, the ribbon's WORLD width grows with distance, so the box
  // has to be sized from the far end of the segment or the ribbon would be clipped by its own
  // coverage volume. 1.25 + 0.5 is slack for the anti-aliased edge and the round joins.
  float far_d = max(length(seg_a), length(seg_b));
  float half_w = 0.5 * max(world_width, min_pixels * px_scale * far_d) * 1.25 + 0.5;

  // Vertical tolerance around the straight chord between two authored waypoints. It has to be loose
  // enough upward to clear a bridge deck or a hill crest bowing above that chord, so it scales with
  // the segment length and caps out well below roof height.
  //
  // This used to have to be tight, because a loose band also swallowed the body of an NPC standing
  // on the route. That is no longer this test's job: the fragment shader compares the world depth
  // against the scene depth, so any MODEL in front of the ground occludes the ribbon outright, at any
  // height. The band now only has to separate the route's own ground from a different world surface
  // above it, and the facing test handles ceilings and undersides regardless of height.
  float seg_len = length(seg_b - seg_a);
  v_up = min(4.0, 0.75 + 0.15 * seg_len);
  v_down = max(5.0, 0.3 * seg_len);

  vec3 lo = min(seg_a, seg_b) - vec3(half_w, v_down + 1.0, half_w);
  vec3 hi = max(seg_a, seg_b) + vec3(half_w, v_up + 1.0, half_w);

  // 12 triangles of the coverage box; corner bits are 1 = +x, 2 = +y, 4 = +z. Depth test and face
  // culling are both off for this pass, so only the silhouette coverage matters, not the winding.
  const int CORNERS[36] = int[36]
    ( 0, 2, 3,  0, 3, 1    // -z
    , 4, 5, 7,  4, 7, 6    // +z
    , 0, 1, 5,  0, 5, 4    // -y
    , 2, 6, 7,  2, 7, 3    // +y
    , 0, 4, 6,  0, 6, 2    // -x
    , 1, 3, 7,  1, 7, 5    // +x
    );
  int c = CORNERS[gl_VertexID];
  vec3 p = vec3((c & 1) != 0 ? hi.x : lo.x
               ,(c & 2) != 0 ? hi.y : lo.y
               ,(c & 4) != 0 ? hi.z : lo.z);

  v_a = seg_a;
  v_b = seg_b;
  gl_Position = model_view_projection * vec4(p, 1.0);
}
