// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_WMOGROUPRENDER_HPP
#define NOGGIT_WMOGROUPRENDER_HPP

#include <noggit/rendering/BaseRender.hpp>
#include <opengl/shader.hpp>
#include <opengl/scoped.hpp>
#include <math/frustum.hpp>

class WMOGroup;

namespace Noggit::Rendering
{
  struct WMORenderBatch
  {
    std::uint32_t flags;
    std::uint32_t shader;
    std::uint32_t tex_array0;
    std::uint32_t tex_array1;
    std::uint32_t tex0;
    std::uint32_t tex1;
    std::uint32_t alpha_test_mode;
    std::uint32_t _pad1;
  };

  enum WMORenderBatchFlags
  {
    eWMOBatch_ExteriorLit = 0x1,
    eWMOBatch_HasMOCV = 0x2,
    eWMOBatch_Unlit = 0x4,
    eWMOBatch_Unfogged = 0x8,
    eWMOBatch_Collision = 0x10,
    eWMOBatch_Sidn = 0x20, // material is Self-Illuminated Day/Night (windows glow at night)
    eWMOBatch_PortalSpill = 0x40, // vertex-colour alpha carries the portal-openness spill factor
    // MOMT F_CLAMP_S/F_CLAMP_T: material samples CLAMP instead of the REPEAT default on that axis.
    // Textures live in shared array textures, so the clamp is emulated in the fragment shader.
    eWMOBatch_ClampS = 0x80,
    eWMOBatch_ClampT = 0x100,
    eWMOBatch_Window = 0x200, // F_WINDOW: lit by the day-night WINDOW pair (client @006b5190, note 28)
    // The batch carries a two-layer texture-blend ALPHA in its vertex colour (either a real lighting MOCV's
    // alpha, or a dedicated texture-blend mocv2 whose RGB is 0). Distinct from HasMOCV, which means the
    // vertex colour is real per-vertex LIGHTING. A modern WMO can have the blend without the lighting.
    eWMOBatch_HasMOCVBlend = 0x400,
    // The material declares Env/EnvMetal but ships NO environment map (empty MOTX second entry). The env
    // term must not sample a texture -- noggit used to substitute the green shanecube placeholder there,
    // which the additive env term then painted over the surface (Stormwind harbour's docked ship).
    eWMOBatch_NoEnvTexture = 0x800,
    // group ships the dedicated mocv2 blend chunk -> two-layer blend reads the separate
    // f_blend_alpha stream. Absent on stock groups, which keep the legacy f_vertex_color.a.
    eWMOBatch_BlendStream = 0x1000
  };

  // One member batch of a combined draw call: its (contiguous) index span and its local-space AABB,
  // computed from the batch's own vertex range at build time (equivalent to the authored MOBA box,
  // but guaranteed to be in the same space as the VBO). Used for per-batch frustum culling (D4).
  struct WMOBatchSpan
  {
    std::uint32_t index_start = 0;
    std::uint32_t index_count = 0;
    glm::vec3 aabb_min = glm::vec3(0.0f);
    glm::vec3 aabb_max = glm::vec3(0.0f);
  };

  struct WMOCombinedDrawCall
  {
    std::vector<int> samplers;
    std::uint32_t index_start = 0;
    std::uint32_t index_count = 0;
    std::uint32_t n_used_samplers = 0;
    bool backface_cull = false;
    // WMO material blend mode (MOMT): 0 opaque, 1 alpha-key, 2 alpha, 3 additive, 4 mod, 5 mod2x...
    // Batches only merge into a draw call when this matches, so each draw call is one blend mode and
    // the draw can do opaque first, then the blended (additive/alpha) batches in a second pass.
    int blend_mode = 0;
    std::vector<WMOBatchSpan> spans;
  };

  class WMOGroupRender : public BaseRender
  {
  public:
    WMOGroupRender(WMOGroup* wmo_group);

    void upload() override;

    void unload() override;

    void draw( OpenGL::Scoped::use_program& wmo_shader
        , math::frustum const& frustum
        , glm::mat4x4 const& transform
        , const float& cull_distance
        , const glm::vec3& camera
        , bool draw_fog
        , bool world_has_skies
    );

    void initRenderBatches();

  private:

    void setupVao(OpenGL::Scoped::use_program& wmo_shader);

    WMOGroup* _wmo_group;

    std::vector<unsigned> _render_batch_mapping;
    std::vector<WMORenderBatch> _render_batches;
    std::vector<WMOCombinedDrawCall> _draw_calls;

    OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vertex_array;
    GLuint const& _vao = _vertex_array[0];
    OpenGL::Scoped::deferred_upload_buffers<9> _buffers;
    GLuint const& _vertices_buffer = _buffers[0];
    GLuint const& _normals_buffer = _buffers[1];
    GLuint const& _texcoords_buffer = _buffers[2];
    GLuint const& _texcoords_buffer_2 = _buffers[3];
    GLuint const& _vertex_colors_buffer = _buffers[4];
    GLuint const& _indices_buffer = _buffers[5];
    GLuint const& _render_batch_mapping_buffer = _buffers[6];
    GLuint const& _render_batch_tex_buffer = _buffers[7];
    GLuint const& _blend_alphas_buffer = _buffers[8];
    bool _has_blend_alphas = false;

    GLuint _render_batch_tex;

    bool _uploaded = false;
    bool _vao_is_setup = false;

  };
}

#endif //NOGGIT_WMOGROUPRENDER_HPP
