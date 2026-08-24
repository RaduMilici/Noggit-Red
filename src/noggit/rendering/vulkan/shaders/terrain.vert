// [VULKAN VK-1] real terrain geometry: noggit chunk heightmap verts + SMOOTH per-vertex normals
// (MapChunk::getNormals -- same data the GL renderer shades with), camera MVP via push constants.
#version 450

layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec3 mccv; // painted vertex colour (default 1,1,1; straight multiply like GL)

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec3 v_mccv;

layout(push_constant) uniform Push
{
  mat4 mvp;      // 0..64   noggit projection * model_view (GL clip conventions)
  float time;    // 64..68
} pc;

void main()
{
  v_world = pos;
  v_normal = normal;
  v_mccv = mccv;
  vec4 clip = pc.mvp * vec4(pos, 1.0);
  // GL -> Vulkan: remap Z from [-w,w] to [0,w]. Y is deliberately NOT flipped here: VK renders y-down into
  // the image (scene-top at the LAST row), and the GL blit (row0 = window bottom) un-flips it -- flipping in
  // BOTH places rendered the world upside-down. Winding flips too, but the terrain pipeline culls NONE.
  clip.z = (clip.z + clip.w) * 0.5;
  gl_Position = clip;
}
