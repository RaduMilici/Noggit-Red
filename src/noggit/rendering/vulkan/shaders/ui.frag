// [2026-09-04 NATIVE UI COMPOSITE] Blend the Qt overlay image over the finished scene.
//
// STATUS: the pipeline this belongs to creates successfully and its draws are recorded into a
// valid, successfully-ended, executed secondary command buffer -- and rasterise NOTHING. Proven by
// reducing this shader to an unconditional constant colour (no push constants, no texture, no
// branch) and by substituting a different pipeline into the same slot. Not yet understood; see the
// note on createUiPipeline before building on this.
#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 v_uv;

layout(location = 0) out vec4 out_color;
layout(location = 1) out float out_z;

layout(set = 1, binding = 5) uniform sampler2D tilesets[];

// EXPLICIT OFFSET, deliberately: ui.vert declares no push block, so this declaration alone defines
// the layout, and glslang may eliminate an unused leading member and shift the offsets.
layout(push_constant) uniform Push
{
  layout(offset = 64) vec4 tex_pad;   // x = bindless texture index of the UI image
} pc;

void main()
{
  int id = int(pc.tex_pad.x);
  if (id < 0)
    discard;

  // The offscreen image is GL-oriented -- the present blit flips Y on its way to the swapchain --
  // so the UI has to be sampled flipped here or it lands upside down on screen.
  vec4 c = texture(tilesets[nonuniformEXT(id)], vec2(v_uv.x, 1.0 - v_uv.y));

  if (c.a <= 0.0039)
    discard;                 // fully transparent: leave the scene untouched

  out_color = c;             // straight alpha; the pipeline blends SRC_ALPHA/ONE_MINUS_SRC_ALPHA
  out_z = gl_FragCoord.z;
}
