// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// Paint one patrol-path segment onto the surface that is actually visible at each covered pixel.
// Everything is done in CAMERA-RELATIVE space (see circle_decal_frag): reconstructing absolute world
// positions at Karazhan's ~19000 coords loses float precision and makes the result shimmer.

uniform sampler2D scene_depth; // everything drawn so far -- what is actually VISIBLE at this pixel
uniform sampler2D world_depth; // terrain + WMO only -- where the GROUND is at this pixel
uniform vec2 inv_viewport;
uniform mat4 inv_view_projection; // inverse of the camera-relative view-projection
uniform vec3 view_axis;           // camera forward, world space -- turns a position into view depth
uniform vec4 color;
uniform float world_width;        // authored ribbon width, world units
uniform float min_pixels;         // never thinner than this on screen
uniform float max_pixels;         // never fatter than this on screen
uniform float px_scale;           // world units per pixel, per unit of view depth

flat in vec3 v_a;
flat in vec3 v_b;
flat in float v_up;
flat in float v_down;

out vec4 out_color;

void main()
{
  vec2 sc = gl_FragCoord.xy * inv_viewport;
  float d = texture(world_depth, sc).r; // the ribbon lives on the GROUND, so reconstruct from that
  float ds = texture(scene_depth, sc).r;

  vec2 ndc = sc * 2.0 - 1.0;
  vec4 sp4 = inv_view_projection * vec4(ndc, d * 2.0 - 1.0, 1.0);
  vec3 p = sp4.xyz / sp4.w; // the GROUND at this pixel, camera-relative

  // MODELS OWN THEIR PIXELS. If something visible sits in front of the ground here, it is a doodad,
  // a creature or a gameobject, and the route runs behind it -- so the model draws over the line,
  // never the other way round. This is a real depth comparison, not a height heuristic: whatever the
  // model is and however tall it stands, if it is nearer than the ground the ribbon is occluded.
  //
  // Compared RAW: both textures are blits of the same depth buffer, so a pixel no model covered is
  // bit-identical in each and the threshold can be a couple of depth LSBs rather than world-space
  // slop -- slop lets a limb resting on the floor slip through and get painted over.
  bool occluded = ds < d - 1e-7;

  // Screen-space derivatives must be taken BEFORE any discard, or the neighbouring lanes in the
  // 2x2 quad may already have exited and the result is undefined.
  //
  // The cross product's orientation depends on the screen-derivative winding, which is not worth
  // assuming: flip it to face the camera (-p points from the surface back to the eye, since p is
  // camera-relative) and the sign becomes meaningful. After the flip, ny > 0 is an upward-facing
  // surface (ground) and ny < 0 is an underside (ceiling, arch, overhang). Using abs() here was
  // what let the ribbon paint across tunnel ceilings.
  vec3 cr = cross(dFdx(p), dFdy(p));
  float cl = length(cr);
  float ny = 1.0; // degenerate derivatives (flat, depth-quantized) -> treat as ground, don't reject
  if (cl > 1e-12)
  {
    ny = cr.y / cl;
    if (dot(cr, -p) < 0.0)
    {
      ny = -ny;
    }
  }

  if (d >= 1.0 || occluded)
  {
    discard; // sky / void, or a model standing in front of the ground here
  }

  // Closest point on the segment measured on the ground plane: the ribbon follows the route's XZ
  // footprint and takes its height from whatever mesh happens to be under it. Clamping t gives
  // round end caps and round joins where consecutive segments overlap.
  vec2 ab = v_b.xz - v_a.xz;
  float ab2 = dot(ab, ab);
  float t = ab2 > 1e-8 ? clamp(dot(p.xz - v_a.xz, ab) / ab2, 0.0, 1.0) : 0.0;
  float dist = distance(p.xz, v_a.xz + t * ab);

  // The route's ground sits at or below the line between two waypoints. Anything appreciably above
  // it is a roof, a ceiling, a tree canopy or the body of a creature standing on the path -- not
  // ground. Below, allow much more: stairs and hollows under a long chord are all walkable.
  float rel_y = p.y - mix(v_a.y, v_b.y, t);
  if (rel_y > v_up || rel_y < -v_down)
  {
    discard;
  }

  // Undersides and steep faces are ceilings, walls and creature bodies, never ground the NPC walks
  // across. This is what keeps the ribbon off an arch overhead and off a cliff face beside it.
  if (ny < 0.4)
  {
    discard;
  }

  // Width: a fixed world width read out in pixels, then clamped so the route never goes hair-thin
  // in the distance nor absurdly fat underfoot. Between the two clamps it thins with distance
  // exactly like a real world-space ribbon.
  float view_depth = max(dot(p, view_axis), 0.05);
  float wpp = max(px_scale * view_depth, 1e-6); // world units per pixel at this depth
  float half_w = 0.5 * clamp(world_width / wpp, min_pixels, max_pixels) * wpp;

  // ~1px analytical edge fade: this decal is evaluated against the resolved (non-MSAA) depth, so a
  // hard cutoff would stair-step.
  float a = 1.0 - smoothstep(half_w - wpp, half_w + wpp, dist);
  if (a <= 0.0)
  {
    discard;
  }
  out_color = vec4(color.rgb, a * color.a);
}
