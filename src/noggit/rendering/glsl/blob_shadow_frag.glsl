// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

// The REAL 1.12 unit shadow decal, painted onto WHATEVER the ground is: sample the scene depth,
// reconstruct the world position of the pixel, map it into the unit's footprint rect and sample
// Textures\\ShadowBlob.blp (32x32 grayscale, white rim -> ~0.63 gray centre, 1-bit alpha oval).
// MODULATE blend (dst * src) like the client @006d7480. Ground pixels outside the footprint (or
// too far above/below the foot plane -- the client's vertical fade ramp) are discarded, so hills
// or walls between the camera and the unit never get painted.

uniform sampler2DArray shadow_tex;
uniform int tex_index;
uniform sampler2D scene_depth;
uniform vec2 inv_viewport;
uniform mat4 inv_view_projection;
uniform vec3 center;
uniform vec3 axis_x;
uniform vec3 axis_y;
uniform float v_range; // vertical fade range (world units)
uniform float fade;    // unit cull-fade: 1 = full shadow; toward 0 modulates toward white

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
  float u = dot(rel, axis_x) / dot(axis_x, axis_x) * 0.5 + 0.5;
  float v = dot(rel, axis_y) / dot(axis_y, axis_y) * 0.5 + 0.5;
  if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0)
  {
    discard; // outside the footprint rect
  }
  float vfade = clamp(1.0 - abs(rel.y) / v_range, 0.0, 1.0);
  if (vfade <= 0.0)
  {
    discard;
  }

  vec4 t = texture(shadow_tex, vec3(u, v, float(tex_index)));
  if (t.a < 0.5)
  {
    discard; // the BLP's 1-bit alpha: outside the oval
  }
  out_color = vec4(mix(vec3(1.0), t.rgb, fade * vfade), 1.0);
}
