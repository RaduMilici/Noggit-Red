// [VULKAN phase B] textured terrain fragment stage = a port of the GL terrain_frag.glsl core:
// 4-layer alphamap blend, MCCV, client-exact lighting (min(1, ambient + sat(N.L)*diffuse)), MCSH shadow
// (0.7 floor), tileset-alpha gloss specular (pow 20, sun band), and the client fog curve. Editor overlays
// (cursor, wireframe, contours, impass/area colours) stay in GL for now.
#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec3 v_mccv;
layout(location = 3) in vec2 v_tc;
layout(location = 4) flat in uint v_chunk;

layout(location = 0) out vec4 out_color;
layout(location = 1) out float out_z; // depth-as-colour (R32F attachment GL imports)

layout(push_constant) uniform Push
{
  mat4 mvp;
  float time;
  float terms;   // [parity bisect] bit0 light, bit1 MCSH shadow, bit2 fog, bit3 MCCV
} pc;

bool term_on(int bit) { return (int(pc.terms) & (1 << bit)) != 0; }

// GL terrain_frag point_lights() verbatim (emitter lights: lamps, fires...). PointLightParams.x = count.
vec3 point_lights(vec3 world_pos, vec3 n);

struct TerrainChunk
{
  ivec4 tex;
  ivec4 misc;
  ivec4 anim;
  vec4 origin;
};

layout(set = 1, binding = 5) uniform sampler2D tilesets[];          // descriptor-indexed
layout(set = 1, binding = 0) uniform sampler2D alpha_atlas;         // 64x64 per chunk, 48 per row
layout(set = 1, binding = 1) uniform sampler2D shadow_atlas;        // 64x64 per chunk, R8
layout(std430, set = 1, binding = 2) readonly buffer Chunks { TerrainChunk chunks[]; };
layout(std140, set = 1, binding = 3) uniform Lighting
{
  vec4 DiffuseColor_FogStart;
  vec4 AmbientColor_FogEnd;
  vec4 FogColor_FogOn;
  vec4 LightDir_FogRate;
  vec4 OceanColorLight;
  vec4 OceanColorDark;
  vec4 RiverColorLight;
  vec4 RiverColorDark;
  vec4 PointLightParams;
  vec4 PointLightPos[16];
  vec4 PointLightColor[16];
  vec4 EnvFogColor_On;
  vec4 EnvFogDist;
  mat4 ShadowMatrix;
  vec4 ShadowParams;
  vec4 ShadowCenterRange;
  mat4 ShadowMatrixEnv;
  vec4 ShadowEnvCenterRange;
  vec4 Camera_Pad;          // xyz = camera world position (appended by the backend)
  vec4 SunSpec_Pad;         // rgb = SUN band colour (terrain specular), w = draw_terrain_specular
  vec4 Toggles;             // x = draw_shadows, y = draw_fog, z = draw_vertex_color
};

vec3 point_lights(vec3 world_pos, vec3 n)
{
  vec3 accum = vec3(0.0);
  int count = int(PointLightParams.x);
  for (int i = 0; i < count; ++i)
  {
    vec3 to_l = PointLightPos[i].xyz - world_pos;
    float dist = length(to_l);
    float radius = max(PointLightPos[i].w, 0.001);
    float atten = clamp(1.0 - dist / radius, 0.0, 1.0);
    atten *= atten;
    float ndotl = max(dot(n, to_l / max(dist, 0.0001)), 0.0);
    accum += PointLightColor[i].xyz * ndotl * atten;
  }
  return accum;
}

const float ATLAS_CHUNKS_PER_ROW = 80.0;

vec2 atlas_uv(uint chunk, vec2 tc01)
{
  // clamp inside the 64x64 cell by half a texel (GL used CLAMP_TO_EDGE per array layer)
  vec2 t = clamp(tc01, 0.5 / 64.0, 63.5 / 64.0);
  float cx = float(chunk % 80u);
  float cy = float(chunk / 80u);
  vec2 atlas_size = vec2(textureSize(alpha_atlas, 0));
  return ((vec2(cx, cy) + t) * 64.0) / atlas_size;
}

vec2 anim_uv(vec2 tc, int a)
{
  // GL animUVOffset(enabled, speed, rotation) port: 8 directions x 45deg, speed ticks
  if ((a & 1) == 0) return tc;
  int spd = (a >> 8) & 7;
  int rot = (a >> 16) & 7;
  float angle = float(rot) * 0.78539816; // 45 deg
  vec2 dir = vec2(cos(angle), sin(angle));
  float f = fract(pc.time * (float(spd) / 7.0) * 0.2);
  return tc + dir * f;
}

vec4 layer_color(int tex_index, vec2 tc)
{
  if (tex_index < 0) return vec4(0.0);
  return texture(tilesets[nonuniformEXT(tex_index)], tc);
}

void main()
{
  TerrainChunk c = chunks[v_chunk];
  vec2 tc01 = v_tc / 8.0;
  vec3 alpha = texture(alpha_atlas, atlas_uv(v_chunk, tc01)).rgb;
  int layer_count = c.misc.x;
  alpha.r = layer_count < 2 ? 0.0 : alpha.r;
  alpha.g = layer_count < 3 ? 0.0 : alpha.g;
  alpha.b = layer_count < 4 ? 0.0 : alpha.b;

  vec4 t0 = layer_color(c.tex.x, anim_uv(v_tc, c.anim.x));
  vec4 t1 = layer_color(c.tex.y, anim_uv(v_tc, c.anim.y));
  vec4 t2 = layer_color(c.tex.z, anim_uv(v_tc, c.anim.z));
  vec4 t3 = layer_color(c.tex.w, anim_uv(v_tc, c.anim.w));
  t0.rgb *= 1.0 + float((c.anim.x >> 1) & 1);
  t1.rgb *= 1.0 + float((c.anim.y >> 1) & 1);
  t2.rgb *= 1.0 + float((c.anim.z >> 1) & 1);
  t3.rgb *= 1.0 + float((c.anim.w >> 1) & 1);
  vec4 blend = t0 * (1.0 - (alpha.r + alpha.g + alpha.b)) + t1 * alpha.r + t2 * alpha.g + t3 * alpha.b;
  vec4 color = layer_count > 0 ? blend : vec4(1.0, 1.0, 1.0, 0.0);
  float gloss = color.a;
  color.a = 1.0;

  // MCCV (vertex paint) only when the editor toggle says so (GL: draw_vertex_color)
  if (Toggles.z != 0.0 && term_on(3))
    color.rgb *= v_mccv;

  // lighting: client FF -- one flat ambient + saturate(N.L) * diffuse, clamped, then modulate
  vec3 n = normalize(v_normal);
  // GL terrain_frag l.328: to_light = -normalize(vec3(L.x, L.z, L.y)) -- the y/z SWIZZLE is deliberate
  vec3 to_light = -normalize(vec3(LightDir_FogRate.x, LightDir_FogRate.z, LightDir_FogRate.y));
  float ndl = clamp(dot(n, to_light), 0.0, 1.0);
  float shadow_sample = Toggles.x != 0.0 ? texture(shadow_atlas, atlas_uv(v_chunk, tc01)).r : 0.0;
  float lit_factor = clamp(1.0 - 3.0 * shadow_sample, 0.0, 1.0);
  vec3 ambient = AmbientColor_FogEnd.xyz;
  vec3 diffuse = DiffuseColor_FogStart.xyz * ndl;
  if (term_on(0))
    color.rgb = color.rgb * clamp(ambient + diffuse + point_lights(v_world, n), 0.0, 1.0);

  // MCSH darkening: shadowed texel -> 70% (client hardcoded mad 0.3/0.7; our map stores 85/255)
  if (Toggles.x != 0.0 && term_on(1))
    color.rgb *= (1.0 - 0.9 * shadow_sample);

  // terrain specular -- GL terrain_frag l.352-356 + l.417 verbatim: the view vector is converted into
  // the light's component frame ((-z, y, -x)) before the half-vector, the gloss mask is the blended
  // tileset ALPHA, and the whole term is gated by lit_factor (MCSH) and clamped after the add. It is
  // NOT texture-modulated and NOT darkened by the 0.7 MCSH term.
  if (SunSpec_Pad.w != 0.0)
  {
    vec3 to_view_pos = normalize(Camera_Pad.xyz - v_world);
    vec3 to_view = vec3(-to_view_pos.z, to_view_pos.y, -to_view_pos.x);
    vec3 half_vec = normalize(to_light + to_view);
    float spec_scalar = pow(clamp(dot(n, half_vec), 0.0, 1.0), 20.0);
    color.rgb = clamp(color.rgb + SunSpec_Pad.rgb * (spec_scalar * lit_factor * gloss), 0.0, 1.0);
  }

  // fog (GL terrain_frag curve): start = fogStart * fogEnd, rate = LightDir_FogRate.w
  if (Toggles.y != 0.0 && FogColor_FogOn.w != 0.0 && term_on(2))
  {
    float dist = distance(Camera_Pad.xyz, v_world);
    float fog_end = AmbientColor_FogEnd.w;
    float start = DiffuseColor_FogStart.w * fog_end;
    float f1 = (dist * (-(1.0 / (fog_end - start)))) + ((1.0 / (fog_end - start)) * fog_end);
    float f2 = clamp(f1, 0.0, 1.0);
    float f3 = pow(f2, LightDir_FogRate.w);
    float fogFactor = 1.0 - clamp(f3, 0.0, 1.0);
    color.rgb = mix(color.rgb, FogColor_FogOn.rgb, fogFactor);
  }

  out_color = vec4(color.rgb, 1.0);
  out_z = gl_FragCoord.z;
}
