// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <QtGui/QOpenGLContext>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <cstdint>
#include <array>

namespace OpenGL
{
  typedef GLuint light;

  enum ubo_targets
  {
    MVP,
    LIGHTING,
    TERRAIN_OVERLAYS,
    CHUNK_INSTANCE_DATA,
    CHUNK_LIQUID_INSTANCE_INDEX
  };

  struct MVPUniformBlock
  {
  	glm::mat4x4 model_view;
    glm::mat4x4 projection;
    // World-space camera position (xyz; w unused). Used by terrain/WMO vertex shaders to render
    // camera-relative -- subtract this (Sterbenz-exact for near geometry) before the rotation so the
    // float pipeline never cancels world-scale (~17000 on a real map) coordinates = no PS1 jitter.
    glm::vec4 camera_pos;
  };

  // Emitter point lights (campfires, braziers, ...) carried to the terrain/WMO/M2 shaders.
  static constexpr int MAX_POINT_LIGHTS = 16;

  struct LightingUniformBlock
  {
    glm::vec4 DiffuseColor_FogStart;
    glm::vec4 AmbientColor_FogEnd;
    glm::vec4 FogColor_FogOn;
    glm::vec4 LightDir_FogRate;
    glm::vec4 OceanColorLight;
    glm::vec4 OceanColorDark;
    glm::vec4 RiverColorLight;
    glm::vec4 RiverColorDark;
    // point lights: .x of PointLightParams = active count. Per light: world pos.xyz + radius in .w
    // (PointLightPos), colour already scaled by intensity in .xyz (PointLightColor).
    glm::vec4 PointLightParams;
    glm::vec4 PointLightPos[MAX_POINT_LIGHTS];
    glm::vec4 PointLightColor[MAX_POINT_LIGHTS];
    // ENTITY fog: the fog context at the CAMERA (zone fog blended toward the containing WMO fog
    // volume). The client fogs M2s/doodads/particles/WMO-liquid with the CAMERA's fog, not the zone
    // fog (note 16: inside the inn, the M2 fog constants equal the inn MFOG). Terrain keeps the zone
    // slots above; WMO geometry has its own per-group fog. .w of EnvFogColor_On = 1 when active
    // (else the shaders fall back to the zone slots). EnvFogDist = (start FRACTION, end, 0, 0).
    glm::vec4 EnvFogColor_On;
    glm::vec4 EnvFogDist;
  };

  struct TerrainParamsUniformBlock
  {
    int draw_shadows = true;
    int draw_lines = false;
    int draw_hole_lines = false;
    int draw_areaid_overlay = false;
    int draw_terrain_height_contour = false;
    int draw_wireframe = false;
    int wireframe_type;
    float wireframe_radius;
    float wireframe_width;
    int draw_impass_overlay = false;
    int draw_paintability_overlay = false;
    int draw_selection_overlay = false;
    glm::vec4 wireframe_color;
    int draw_impassible_climb = false;
    int climb_use_output_angle = false;
    int climb_use_smooth_interpolation = false;
    float climb_value;
    int draw_vertex_color = true;
    int padding[3];
  };

  struct ChunkInstanceDataUniformBlock
  {
    int ChunkTextureSamplers[4];
    int ChunkTextureArrayIDs[4];
    int ChunkHoles_DrawImpass_TexLayerCount_CantPaint[4];
    int ChunkTexDoAnim[4];
    int ChunkTexAnimSpeed[4];
    int AreaIDColor_Pad2_DrawSelection[4];
    int ChunkXZ_TileXZ[4];
    int ChunkTexAnimDir[4];
  };

  struct LiquidChunkInstanceDataUniformBlock
  {
    unsigned texture_array;
    unsigned type;
    float xbase;
    float zbase;
    float anim_u;
    float anim_v;
    unsigned subchunks_1;
    unsigned subchunks_2;
    unsigned n_texture_frames;
    unsigned _pad1;
    unsigned _pad2;
    unsigned _pad3;
    unsigned _pad4;
    unsigned _pad5;
    unsigned _pad6;
    unsigned _pad7;
  };

  struct M2RenderState
  {
    std::uint16_t blend = 0;
    bool backface_cull = true;
    bool z_buffered = false;
    bool unfogged = false;
    bool unlit = false;
    int detail_doodad = -1; // -1 unset; 1 = ground-clutter (day/night grayscale dim, keeps texture hue)
    bool masked_additive = false;
    int water_surface_effect = -1; // -1 unset; 1 = fishing-pool wake geoset (grey luminance-alpha, blends into water)
    int creature_bloom = -1; // -1 = unset; 1 = opaque energy creature (mask direct); 3 = alpha-only mask pass
    bool bloom_mask_pass = false; // drawing the alpha-only bloom-mask re-draw of a translucent creature
    int discard_invisible = 0; // promoted creature pass: discard texels that add nothing (no depth write)
    bool allow_lightray_model = true;
    // [perf 2026-07-23] texture-bind + per-texture uniform caches (check-before-set, per-draw-call).
    // Consumed by ModelRenderPass::bindTexture so an unchanged array/index/clamp on a unit skips the GL
    // call. Sentinels matter: 0 is never a valid GL texture name (tex_arrays); tex_index 0 IS valid so
    // its cache starts at 0xFFFFFFFF; clamp mask is 0..3 so -1 forces the first upload.
    std::array<GLuint, 2> tex_arrays = {0, 0};
    std::array<GLuint, 2> tex_indices = {0xFFFFFFFFu, 0xFFFFFFFFu};
    std::array<int, 2> tex_clamp = {-1, -1};
    std::array<GLint, 2> tex_unit_lookups;
    GLint pixel_shader = 0;
    // tex_matrix upload state per unit: -1 unset, 0 last upload was animated (re-upload), 1 last upload was
    // identity (skip). Most world M2s have no UV scroll -> identity every pass -> uploaded once per draw.
    int tex_matrix_state[2] = {-1, -1};
    glm::vec4 mesh_color = glm::vec4(-1e30f); // sentinel; skip re-uploading an unchanged mesh_color

  };

  // [perf 2026-08-05] glMultiDrawElementsIndirect command. Layout is fixed by the GL spec (must match exactly,
  // 20 bytes, no padding): one per (model render-pass) in a batch. baseVertex is SIGNED per spec. baseInstance
  // offsets the divisor-1 per-instance attribute fetches (transform loc6-9, interior loc10, inst_tex loc11) --
  // that is how each command selects its own model's instances + texture layers without gl_DrawID (4.6).
  struct DrawElementsIndirectCommand
  {
    GLuint count = 0;         // index count for this render pass
    GLuint instanceCount = 0; // number of instances of this model
    GLuint firstIndex = 0;    // start index in the shared arena IBO (= arena base + pass index_start)
    GLint  baseVertex = 0;    // added to every index (= this model's base vertex in the shared arena VBO)
    GLuint baseInstance = 0;  // first per-instance record for this command in the batch instance buffer
  };
  static_assert(sizeof(DrawElementsIndirectCommand) == 20, "DrawElementsIndirectCommand must be tightly packed for MDI");


}
