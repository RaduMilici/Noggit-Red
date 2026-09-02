// [VULKAN phase B] textured terrain vertex stage. Binding 0 = the 36 B pos/normal/MCCV stream shared with
// the clay path; binding 1 = per-vertex chunk index (uint). UVs are derived from the chunk origin exactly
// like GL's per-vertex texcoord table (detail_size 8 -> 0..8 across a chunk; alphamap uv = tc / 8).
#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_mccv;
layout(location = 3) in uint in_chunk;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec3 v_mccv;
layout(location = 3) out vec2 v_tc;          // 0..8 across the chunk
layout(location = 4) flat out uint v_chunk;

layout(push_constant) uniform Push
{
  mat4 mvp;
  float time;
} pc;

struct TerrainChunk
{
  ivec4 tex;            // bindless texture index per layer (-1 = none)
  ivec4 misc;           // x = layer_count, y = holes
  ivec4 anim;           // per layer: bit0 uv-anim, bit1 overbright, bits 8-10 speed, bits 16-18 rotation
  vec4 origin;          // x, z = chunk world origin
};
layout(std430, set = 1, binding = 2) readonly buffer Chunks { TerrainChunk chunks[]; };

const float CHUNKSIZE = 533.33333 / 16.0;

void main()
{
  // GL clip (z in [-w, w]) -> VK clip (z in [0, w]); y stays (VK y-down + un-flipped GL blit cancel)
  vec4 clip = pc.mvp * vec4(in_pos, 1.0);
  clip.z = (clip.z + clip.w) * 0.5;
  gl_Position = clip;
  v_world = in_pos;
  v_normal = in_normal;
  v_mccv = in_mccv;
  v_chunk = in_chunk;
  vec2 o = chunks[in_chunk].origin.xy;
  v_tc = (in_pos.xz - o) / CHUNKSIZE * 8.0;
}
