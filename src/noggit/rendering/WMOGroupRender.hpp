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

  // [VULKAN phase D] One interleaved vertex for the VK WMO pipeline. upload() frees the separate
  // CPU streams, so this mirror is built there and kept.
  struct VkWmoVertex
  {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv0;
    glm::vec2 uv1;
    glm::vec4 color;
    std::uint32_t batch_id;   // == GL's per-vertex batch_mapping attribute
    std::uint32_t _pad;       // keep the stride at 64 so the VK attribute offsets stay aligned
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

    // [VULKAN phase D] geometry mirror for the VK backend -- captured during upload(), before the
    // CPU streams are freed. _vk_dirty is cleared once the backend has taken a copy.
    std::vector<VkWmoVertex> const& vkVertices() const { return _vk_verts; }
    std::vector<std::uint16_t> const& vkIndices() const { return _vk_indices; }
    std::vector<WMORenderBatch> const& vkBatches() const { return _vk_batches; }
    // BLP file names per batch, parallel to vkBatches(); VK addresses textures by name -> bindless id
    std::vector<std::pair<std::string, std::string>> const& vkBatchBlps() const { return _vk_batch_blps; }
    std::vector<unsigned> const& vkBatchMapping() const { return _vk_batch_mapping; }
    std::vector<WMOCombinedDrawCall> const& vkDrawCalls() const { return _draw_calls; }
    bool vkMirrorDirty() const { return _vk_dirty; }
    void clearVkMirrorDirty() { _vk_dirty = false; }
    // Called once the VK arena has copied this group; the arena is append-only and keyed per group,
    // so holding the mirror afterwards just duplicates ~65 MB of geometry in RAM.
    void releaseVkMirror()
    {
      _vk_verts.clear(); _vk_verts.shrink_to_fit();
      _vk_indices.clear(); _vk_indices.shrink_to_fit();
      _vk_batches.clear(); _vk_batches.shrink_to_fit();
      _vk_batch_mapping.clear(); _vk_batch_mapping.shrink_to_fit();
      _vk_batch_blps.clear(); _vk_batch_blps.shrink_to_fit();
      _vk_mirror_taken = true;
      _vk_dirty = false;
    }
    // true once the arena has taken this group -- distinguishes "already copied" from "no geometry"
    bool vkMirrorTaken() const { return _vk_mirror_taken; }

    // The runs GL actually emitted for this group in the most recent draw(), with the owning draw
    // call's pipeline state. Overwritten every draw, so read it straight after the instance drew.
    struct VkRun
    {
      std::uint32_t index_start = 0;
      std::uint32_t index_count = 0;
      std::int32_t blend_mode = 0;
      std::int32_t backface_cull = 0;
    };
    std::vector<VkRun> const& vkLastRuns() const { return _vk_last_runs; }
    // frame the runs above were captured in; stale records must not be replayed
    unsigned vkLastRunFrame() const { return _vk_last_run_frame; }

  private:

    void setupVao(OpenGL::Scoped::use_program& wmo_shader);

    WMOGroup* _wmo_group;

    std::vector<unsigned> _render_batch_mapping;
    std::vector<WMORenderBatch> _render_batches;
    std::vector<WMOCombinedDrawCall> _draw_calls;

    std::vector<VkWmoVertex> _vk_verts;
    std::vector<std::uint16_t> _vk_indices;
    std::vector<WMORenderBatch> _vk_batches;
    std::vector<unsigned> _vk_batch_mapping;
    std::vector<std::pair<std::string, std::string>> _render_batch_blps;
    std::vector<std::pair<std::string, std::string>> _vk_batch_blps;
    bool _vk_dirty = false;
    bool _vk_mirror_taken = false;
    std::vector<VkRun> _vk_last_runs;
    unsigned _vk_last_run_frame = 0;

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
