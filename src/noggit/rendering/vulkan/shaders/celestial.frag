// [VULKAN phase F] Celestial billboard fragment -- port of moon_frag.glsl, both modes:
//   additive : PREMULTIPLIED glow (glare / sun disc), blended with ONE/ONE so opacity is a real
//              brightness multiplier. Background-haze floor subtracted and a hard radial mask, or the
//              glare's faint square backing shows as a glowing rectangle.
//   alpha    : the moon discs, whose shape lives in the texture alpha.
#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_dir;

layout(location = 0) out vec4 out_color;
layout(location = 1) out float out_z;

layout(set = 1, binding = 5) uniform sampler2D tilesets[];   // shared bindless array

layout(push_constant) uniform Push
{
  mat4 mvp_cam;
  vec4 center_half;
  vec4 right_opacity;
  vec4 up_tex;
  vec4 color_additive;
} pc;

void main()
{
  int id = int(pc.up_tex.w);
  vec4 t = id >= 0 ? texture(tilesets[nonuniformEXT(id)], v_uv) : vec4(0.0);
  vec3 color = pc.color_additive.rgb;
  float opacity = pc.right_opacity.w;

  // HORIZON CLIP: the below-horizon sky is a solid fog wall the celestial must set BEHIND.
  float horizon = smoothstep(0.0, 0.006, v_dir.y / length(v_dir));

  if (pc.color_additive.w > 0.5)
  {
    vec3 g = max(t.rgb * t.a - vec3(0.04), vec3(0.0));
    float d = length(v_uv - vec2(0.5)) * 2.0;
    float mask = 1.0 - smoothstep(0.62, 0.98, d);
    vec3 rgb = g * color * opacity * mask * horizon;
    out_color = vec4(rgb, clamp(max(rgb.r, max(rgb.g, rgb.b)), 0.0, 1.0));
  }
  else
  {
    out_color = vec4(t.rgb * color, t.a * opacity * horizon);
  }
  out_z = gl_FragCoord.z;
}
