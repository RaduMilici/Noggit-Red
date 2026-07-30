// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 410 core

in vec4 position;
in vec3 normal;
in vec4 vertex_color;
in vec2 texcoord;
in vec2 texcoord_2;
in uint batch_mapping;

out vec3 f_position;
out vec3 f_normal;
out vec2 f_texcoord;
out vec2 f_texcoord_2;
out vec4 f_vertex_color;

flat out uint flags;
flat out uint shader;
flat out uint tex_array0;
flat out uint tex_array1;
flat out uint tex0;
flat out uint tex1;
flat out uint alpha_test_mode;

layout (std140) uniform matrices
{
  mat4 model_view;
  mat4 projection;
  vec4 camera_pos;
};

uniform mat4 transform;
uniform usamplerBuffer render_batches_tex;

float makeNaN(float nonneg)
{
  return sqrt(-nonneg-1.0);
}

void main()
{
  float NaN = makeNaN(1);

  if (!bool(batch_mapping)) // discard
  {
    gl_Position = vec4(NaN, NaN, NaN, NaN);

    f_position = vec3(0);
    f_normal = vec3(0);
    f_texcoord = vec2(0);
    f_texcoord_2 = vec2(0);
    f_vertex_color = vec4(0);

    flags = 0;
    shader = 0;
    tex_array0 = 0;
    tex_array1 = 0;
    tex0 = 0;
    tex1 = 0;
    alpha_test_mode = 0;
  }
  else
  {
    vec4 pos = transform * position;
    // Camera-relative (see terrain_vert.glsl): subtract camera before the rotation so world-scale
    // coords never cancel in float -> no PS1 jitter of WMO geometry against the models/terrain.
    mat4 view_rot = model_view;
    view_rot[3].xyz = vec3(0.0);
    vec4 view_space_pos = view_rot * vec4(pos.xyz - camera_pos.xyz, 1.0);
    gl_Position = projection * view_space_pos;

    f_position = pos.xyz;
    f_normal = mat3(transform) * normal;

    uvec4 batch_first_half = texelFetch(render_batches_tex, int((batch_mapping - 1) * 2));
    uvec4 batch_second_half = texelFetch(render_batches_tex, int((batch_mapping - 1) * 2 + 1));

    flags = batch_first_half.r;
    shader = batch_first_half.g;
    tex_array0 = batch_first_half.b;
    tex_array1 = batch_first_half.a;
    tex0 = batch_second_half.r;
    tex1 = batch_second_half.g;
    alpha_test_mode = batch_second_half.b;

    // Env and EnvMetal: the second coordinate is a generated SPHERE-MAP lookup, not a mesh UV.
    //
    // Two bugs fixed here:
    //  1. FRAME MISMATCH -- the incident vector is CAMERA space (view_space_pos) but f_normal is WORLD
    //     space (mat3(transform) * normal), so reflect() combined two different frames. Same class of bug
    //     as the terrain specular half-vector (see terrain_frag.glsl). Rotate the normal into view space
    //     first, using the same translation-stripped view matrix used for the position.
    //  2. NO REMAP -- a reflection vector's xy spans [-1,1], but a texture lookup needs [0,1]. The raw
    //     value sampled far outside the map and relied on GL_REPEAT wrapping, mirroring the env map
    //     across the surface. Remap with 0.5 + 0.5*r.
    if(shader == 3 || shader == 5)
    {
      f_texcoord = texcoord;

      vec3 normal_view = normalize(mat3(view_rot) * f_normal);
      vec3 incident_view = normalize(view_space_pos.xyz);
      vec3 reflected = reflect(incident_view, normal_view);
      f_texcoord_2 = 0.5 + 0.5 * reflected.xy;
    }
    else
    {
      f_texcoord = texcoord;
      f_texcoord_2 = texcoord_2;
    }

    f_vertex_color = vertex_color;
  }
}
