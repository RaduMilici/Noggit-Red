// [VULKAN phase F] Sky dome fragment: pure interpolated band colour, exactly like GL's dome shader.
// Alpha 0 is the bloom-mask opt-out (the sky must not read as emissive).
#version 450

layout(location = 0) in vec3 v_color;

layout(location = 0) out vec4 out_color;
layout(location = 1) out float out_z;   // depth-as-colour attachment GL imports

void main()
{
  out_color = vec4(v_color, 0.0);
  out_z = 1.0;   // backdrop: never occludes anything GL composes over it
}
