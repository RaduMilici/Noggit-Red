// [VULKAN VK-1 skeleton] fullscreen triangle from gl_VertexIndex -- no vertex buffers needed.
#version 450

layout(location = 0) out vec2 v_uv;

void main()
{
  // 3 vertices covering the screen: (-1,-1) (3,-1) (-1,3)
  vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_uv = pos;
  gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
