// [VULKAN VK-1b] real terrain geometry: noggit chunk heightmap verts, camera MVP via push constants.
#version 450

layout(location = 0) in vec3 pos;

layout(location = 0) out vec3 v_world;

layout(push_constant) uniform Push
{
  mat4 mvp;      // 0..64   noggit projection * model_view (GL clip conventions)
  float time;    // 64..68
} pc;

void main()
{
  v_world = pos;
  vec4 clip = pc.mvp * vec4(pos, 1.0);
  // GL -> Vulkan clip space: flip Y, remap Z from [-w,w] to [0,w]
  clip.y = -clip.y;
  clip.z = (clip.z + clip.w) * 0.5;
  gl_Position = clip;
}
