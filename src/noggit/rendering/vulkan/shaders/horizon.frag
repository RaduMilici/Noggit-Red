// [2026-09-08 WDL HORIZON] Flat fog colour: the client sets fog start 0 / end 1 for this pass, so every
// fragment lands on the fog colour. Alpha 0 = bloom-mask opt-out, like the GL horizon_frag.glsl.
#version 450

layout(location = 0) in vec3 v_color;
layout(location = 0) out vec4 out_color;
layout(location = 1) out float out_z;   // depth-as-colour attachment (R32F) the compose path imports

void main()
{
  out_color = vec4(v_color, 0.0);
  out_z = gl_FragCoord.z;
}
