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
    discard; // sky
  }
  vec4 wp4 = inv_view_projection * vec4(sc * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
  vec3 wp = wp4.xyz / wp4.w;

  vec3 rel = wp - center;
  vec2 local = vec2(rel.x, rel.z) / radius; // disc space, [-1,1] across the circle
  float r = length(local);
  if (r > 1.0)
  {
    discard; // outside the circle footprint
  }
  // DEPTH CORRECTNESS: the circle is a GROUND decal, so creatures/objects standing in it must occlude
  // it (draw OVER it), not the other way round. This pass reconstructs whatever surface is at each
  // pixel from the scene depth, so a creature's BODY pixels land inside the disc footprint and would
  // get painted -- the ring bleeding over the model. Ground rises GRADUALLY with horizontal distance
  // (a slope), whereas a body rises STEEPLY within a small footprint. Reject any surface that climbs
  // faster than plausible ground (base 0.4 + 0.6 * horizontal dist): the model is left untouched so it
  // occludes the ring, while real terrain slopes inside the circle still take the paint.
  float horiz = length(rel.xz);
  if (rel.y > 0.4 + 0.6 * horiz)
  {
    discard; // raised object surface (creature/doodad body) -- let it draw over the ring
  }
  if (rel.y < -v_range)
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
  out_color = vec4(color.rgb, a * color.a);
}
