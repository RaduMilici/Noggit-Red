// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Reconstruct the world position of each covered pixel from the scene depth, map it into the
// circle's local disc space, and sample the client's UnitSelectTexture ring -- so the circle is
// painted onto the real terrain/WMO ground (wraps the mesh), not a separate 2D disc.

uniform sampler2DArray tex;      // UnitSelectTexture (ring/glow in ALPHA)
uniform float tex_index;
uniform sampler2D scene_depth;
uniform vec2 inv_viewport;
uniform mat4 inv_view_projection;
uniform vec3 center;
uniform vec3 camera;             // world camera position -- for the precision-safe (camera - center) disc offset
uniform float radius;
uniform float v_range;           // vertical range: reject ground too far above/below the foot plane
uniform vec4 color;
uniform float uv_rotation;       // orient the crescent's bright edge toward the camera
uniform int use_texture;         // 1 = sample UnitSelectTexture ring; 0 = procedural filled blob

out vec4 out_color;

void main()
{
  vec2 sc = gl_FragCoord.xy * inv_viewport;
  float d = texture(scene_depth, sc).r;
  if (d >= 1.0)
  {
    discard; // sky / void -- no ground here
  }

  // DISC MAPPING from the FLAT FOOT PLANE, not the scene depth (2026-07-20): reconstructing the disc coords
  // from the scene depth made the ring follow the floor's micro-geometry (recessed tile grout seams) AND
  // quantize at grazing angles -- warping the circle into the "streaks / serrated" ring. Intersect this
  // pixel's view ray with the flat plane y = center.y for a clean, precise circle regardless of floor detail.
  // (The scene depth is still read below, ONLY to occlude the ring behind raised bodies / lower ledges.)
  vec2 ndc = sc * 2.0 - 1.0;
  // inv_view_projection is the CAMERA-RELATIVE inverse (see WorldRender): np4/fp4 come out as small
  // camera-relative coords, not ~19000 world ones, so the ray direction is precise (no big-coord jitter).
  vec4 np4 = inv_view_projection * vec4(ndc, -1.0, 1.0);
  vec4 fp4 = inv_view_projection * vec4(ndc,  1.0, 1.0);
  vec3 dir = normalize(fp4.xyz / fp4.w - np4.xyz / np4.w); // pixel's world-space ray direction
  if (abs(dir.y) < 1e-5)
  {
    discard; // view ray parallel to the ground plane
  }
  // PRECISION (2026-07-20): compute the disc offset from cam_rel = (camera - center) -- a SMALL vector, the
  // same for every fragment this frame -- plus the ray, NOT from a reconstructed ~19000 world position minus
  // the ~19000 center. That big-minus-big cancellation lost precision at Karazhan's world coords, so the ring
  // DITHERED / shook side-to-side as the camera moved. Intersect the camera ray with the foot plane (rel.y=0):
  vec3 cam_rel = camera - center;
  float s = -cam_rel.y / dir.y;
  if (s < 0.0)
  {
    discard; // foot plane is behind the camera
  }
  vec3 rel = cam_rel + s * dir;
  vec2 local = vec2(rel.x, rel.z) / radius; // disc space, [-1,1] across the circle
  float r = length(local);
  if (r > 1.0)
  {
    discard; // outside the circle footprint
  }

  // OCCLUSION: reconstruct the ACTUAL surface at this pixel from the scene depth. A creature/doodad body
  // rises STEEPLY within the footprint (base 0.4 + 0.6 * horizontal dist above the foot plane); leave those
  // pixels to the model so it occludes the ring instead of the ring bleeding over it. A much lower floor
  // (ledge/pit) is likewise not painted.
  // inv_view_projection now reconstructs CAMERA-RELATIVE positions (world - camera), so the scene surface
  // and the foot plane are compared in that same small-coord space: foot plane sits at -cam_rel.y.
  vec4 sp4 = inv_view_projection * vec4(ndc, d * 2.0 - 1.0, 1.0);
  vec3 scene_rel = sp4.xyz / sp4.w;   // scene surface, camera-relative
  float foot_rel_y = -cam_rel.y;      // foot plane height in camera-relative space (= center.y - camera.y)
  float horiz = length(rel.xz);
  if (scene_rel.y > foot_rel_y + 0.4 + 0.6 * horiz)
  {
    discard; // raised object surface (creature/doodad body) -- let it draw over the ring
  }
  if (scene_rel.y < foot_rel_y - v_range)
  {
    discard; // a much lower floor (ledge/pit inside the quad) -- don't paint it
  }

  float a;
  if (use_texture == 1)
  {
    float cs = cos(uv_rotation);
    float sn = sin(uv_rotation);
    vec2 rot = vec2(local.x * cs - local.y * sn, local.x * sn + local.y * cs);
    vec2 uv = rot * 0.5 + 0.5;
    a = texture(tex, vec3(uv, tex_index)).a;
  }
  else
  {
    // Fallback (UnitSelectTexture missing): procedural filled blob with a soft rim, same as circle_frag.
    a = 0.85 * (1.0 - smoothstep(0.78, 1.0, r));
  }
  // Analytical edge anti-aliasing (2026-07-20): this decal is computed per-pixel from the resolved (1x,
  // non-MSAA) scene depth, so the hard r>1 disc cutoff above stair-steps. Fade alpha to 0 over a ~1.5px band
  // at the outer edge (fwidth(r) = screen-space change of r) so the ring resolves smoothly at any distance.
  float edge_aa = 1.0 - smoothstep(1.0 - 1.5 * fwidth(r), 1.0, r);
  out_color = vec4(color.rgb, a * color.a * edge_aa);
}
