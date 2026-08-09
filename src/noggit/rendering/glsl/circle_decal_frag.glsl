// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Reconstruct the world position of each covered pixel from the scene depth, map it into the
// circle's local disc space, and sample the client's UnitSelectTexture ring -- so the circle is
// painted onto the real terrain/WMO ground (wraps the mesh), not a separate 2D disc.

uniform sampler2DArray tex;      // UnitSelectTexture (ring/glow in ALPHA)
uniform float tex_index;
uniform sampler2D scene_depth; // everything drawn so far -- what is VISIBLE at this pixel
uniform sampler2D world_depth; // terrain + WMO only -- where the GROUND is at this pixel
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
  float dw = texture(world_depth, sc).r;
  vec2 ndc = sc * 2.0 - 1.0;

  // The world surface at this pixel and WHICH WAY IT FACES. Both are computed up here, before any
  // discard: screen-space derivatives read the neighbouring lanes of the 2x2 quad, and if those have
  // already discarded (which they do all round the rim of the disc) the result is undefined.
  vec4 wp4 = inv_view_projection * vec4(ndc, dw * 2.0 - 1.0, 1.0);
  vec3 ground_rel = wp4.xyz / wp4.w;
  vec3 gcr = cross(dFdx(ground_rel), dFdy(ground_rel));
  float gcl = length(gcr);
  float ground_up = 1.0; // degenerate derivatives -> treat as floor rather than reject
  if (gcl > 1e-12)
  {
    ground_up = gcr.y / gcl;
    if (dot(gcr, -ground_rel) < 0.0)
    {
      ground_up = -ground_up; // orient toward the camera so the sign means "faces upward"
    }
  }

  if (d >= 1.0)
  {
    discard; // sky / void -- no ground here
  }

  // DISC MAPPING from the FLAT FOOT PLANE, not the scene depth (2026-07-20): reconstructing the disc coords
  // from the scene depth made the ring follow the floor's micro-geometry (recessed tile grout seams) AND
  // quantize at grazing angles -- warping the circle into the "streaks / serrated" ring. Intersect this
  // pixel's view ray with the flat plane y = center.y for a clean, precise circle regardless of floor detail.
  // (The scene depth is still read below, ONLY to occlude the ring behind raised bodies / lower ledges.)
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

  // OCCLUSION. The ring is painted on the GROUND, so reconstruct the ground from the world-only depth
  // (terrain + WMO, captured before any model was drawn) and compare it against what is actually
  // VISIBLE here. Anything visible in front of the ground is a doodad, creature or gameobject: the
  // model owns that pixel and draws over the ring, never the other way round.
  //
  // This replaces a height heuristic ("reject surfaces rising steeply within the footprint"), which
  // could not tell a creature's boots from the floor they stand on and so let the ring paint over the
  // bottom of every model. A depth comparison has no such blind spot.
  // inv_view_projection reconstructs CAMERA-RELATIVE positions (world - camera), so both surfaces and
  // the foot plane live in that same small-coord space: the foot plane sits at -cam_rel.y.
  if (dw >= 1.0)
  {
    discard; // no ground here at all (sky seen past the edge of the world)
  }

  // Compare the two depths RAW rather than reconstructing positions and comparing distances. Both
  // textures are blits of the same depth buffer, so a pixel no model covered holds a bit-identical
  // value in each and the difference is exactly zero -- which lets the threshold be a couple of
  // depth LSBs instead of a world-space slop. That slop was the visible bug: a hand or a foot lying
  // within a few centimetres of the floor fell inside it, and the ring painted over the limb.
  if (d < dw - 1e-7)
  {
    discard; // something visible is nearer than the ground -- the model owns this pixel
  }

  float foot_rel_y = -cam_rel.y;      // foot plane height in camera-relative space (= center.y - camera.y)
  float horiz = length(rel.xz);

  // The depth test above only settles MODELS. Static world geometry rising out of the disc -- a
  // crate modelled as part of a WMO, a ledge, a step -- is legitimately "ground" to a depth test,
  // and without this the ring wraps up and over it. Reject world surfaces that climb steeply away
  // from the unit's foot plane; a gentle floor slope stays within the allowance and still gets the
  // ring, which is the whole point of draping it in the first place.
  if (ground_rel.y > foot_rel_y + 0.4 + 0.6 * horiz)
  {
    discard;
  }

  // ...and reject by FACING as well as by height. The height cone above allows more rise the further
  // out in the disc you go, so a vertical face -- a crate side, a barrel, a wall -- crossing the disc
  // at roughly the right height slips through it, which is the thin sliver of ring that still showed
  // on props at a low, grazing camera angle. A floor faces up; a crate side does not.
  if (ground_up < 0.4)
  {
    discard;
  }
  if (ground_rel.y < foot_rel_y - v_range)
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
  else if (use_texture == 2)
  {
    // Plain outline ring: a UI cursor, not a client selection circle -- no texture, just an annulus
    // from r = 1-thickness to the rim (the edge AA below fades the outer edge).
    // NOTE: `const float`, not `float const` -- GLSL requires the qualifier first, and the wrong
    // order compiles as C++ but throws at shader build time (crashed on entering the creature tool).
    const float thickness = 0.1167; // 1/3 of the original 0.35
    a = smoothstep(1.0 - thickness - fwidth(r), 1.0 - thickness, r);
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
