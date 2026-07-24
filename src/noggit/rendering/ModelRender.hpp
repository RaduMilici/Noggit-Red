// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_MODELRENDER_HPP
#define NOGGIT_MODELRENDER_HPP

#include <noggit/rendering/BaseRender.hpp>
#include <noggit/ModelHeaders.h>
#include <noggit/tool_enums.hpp>
#include <opengl/scoped.hpp>
#include <opengl/shader.hpp>
#include <math/frustum.hpp>

class Model;
class ModelInstance;

namespace Noggit::Rendering
{
  enum class M2Blend : uint16_t
  {
    Opaque,
    Alpha_Key,
    Alpha,
    No_Add_Alpha,
    Add,
    Mod,
    Mod2x
  };

  enum class ModelPixelShader : uint16_t
  {
    Combiners_Opaque,
    Combiners_Decal,
    Combiners_Add,
    Combiners_Mod2x,
    Combiners_Fade,
    Combiners_Mod,
    Combiners_Opaque_Opaque,
    Combiners_Opaque_Add,
    Combiners_Opaque_Mod2x,
    Combiners_Opaque_Mod2xNA,
    Combiners_Opaque_AddNA,
    Combiners_Opaque_Mod,
    Combiners_Mod_Opaque,
    Combiners_Mod_Add,
    Combiners_Mod_Mod2x,
    Combiners_Mod_Mod2xNA,
    Combiners_Mod_AddNA,
    Combiners_Mod_Mod,
    Combiners_Add_Mod,
    Combiners_Mod2x_Mod2x,
    Combiners_Opaque_Mod2xNA_Alpha,
    Combiners_Opaque_AddAlpha,
    Combiners_Opaque_AddAlpha_Alpha,
  };

  enum class texture_unit_lookup : int
  {
    environment,
    t1,
    t2,
    none
  };


  struct ModelRenderPass : ModelTexUnit
  {
    ModelRenderPass() = delete;
    ModelRenderPass(ModelTexUnit const& tex_unit, Model* m);

    float ordering_thingy = 0.f;
    // Model-space submesh sort-centre (classic: geoset.center; wotlk: SkinSection CenterPosition -- both
    // land in BoundingBox[0], the point the client keys its transparency distance-sort on). Consumed by the
    // per-frame per-instance back-to-front transparency sort in ModelRender::draw (single-instance overload).
    glm::vec3 sort_center = glm::vec3(0.f);
    uint16_t index_start = 0, index_count = 0, vertex_start = 0, vertex_end = 0;
    uint16_t geoset_id = 0;
    uint16_t blend_mode = 0;
    texture_unit_lookup tu_lookups[2];
    uint16_t textures[2];
    uint16_t uv_animations[2];
    std::optional<ModelPixelShader> pixel_shader;

    // Fishing-pool water-effect geoset (foam/bubble/sparkle) on a _water_surface_effect model: promote
    // the authored-Opaque blend to alpha + drop depth-write so it reads translucent and sits flush on
    // the water instead of z-fighting. Tri-state cache: -1 = not yet resolved, 0 = no, 1 = yes.
    int _water_effect_translucent = -1;


    // extra_alpha: distance-fade factor for instanced draws -- multiplies into the instance alpha so
    // opaque passes promote to alpha-blend and fade out instead of popping at the render distance.
    bool prepareDraw(OpenGL::Scoped::use_program& m2_shader, Model *m, ModelInstance const* instance, OpenGL::M2RenderState& model_render_state, float extra_alpha = 1.0f);
    void afterDraw();
    bool bindTexture(size_t index, Model* m, ModelInstance const* instance, OpenGL::M2RenderState& model_render_state, OpenGL::Scoped::use_program& m2_shader);
    void initUVTypes(Model* m);

    bool operator< (const ModelRenderPass &m) const
    {
      if (priority_plane < m.priority_plane)
      {
        return true;
      }
      else if (priority_plane > m.priority_plane)
      {
        return false;
      }
      else
      {
        return blend_mode == m.blend_mode ? (ordering_thingy < m.ordering_thingy) : blend_mode < m.blend_mode;
      }
    }
  };

  class ModelRender : public BaseRender
  {
    friend struct ModelRenderPass;

  public:
    ModelRender(Model* model);
    // NOT override: BaseRender has no virtual destructor. ModelRender is only ever destroyed as a concrete
    // value member of Model (never through a BaseRender*), so a non-virtual dtor is safe. Frees the raw
    // _bone_matrices_buf_tex on model eviction (which never calls unload()) -- see ModelRender.cpp.
    ~ModelRender();

    void upload() override;
    void unload() override;

    void draw(glm::mat4x4 const& model_view
        , ModelInstance& instance
        , OpenGL::Scoped::use_program& m2_shader
        , OpenGL::M2RenderState& model_render_state
        , math::frustum const& frustum
        , const float& cull_distance
        , const glm::vec3& camera
        , int animtime
        , display_mode display
        , bool no_cull = false
        , bool bloom_mask_only = false // re-stamp only the emissive bloom mask (alpha channel); no colour
        // Interior light for this object: rgb = the MOCV floor colour sampled under it, a = 1 when
        // indoors; (0,0,0,0) = outdoor (default). The shader splits the colour into ambient/diffuse per
        // the client's unit interior lighting (RE_notes/15) instead of the outdoor sun.
        , glm::vec4 const& interior_light = glm::vec4(0.f)
        // Cull-range fade alpha for this instance (client 2000 ms fade; 1.0 = fully shown).
        , float dist_fade = 1.0f
        // skip_animate: bones were pre-computed for this instance on a worker thread (creature parallel-
        // animate pre-pass) and already restored into _model->bone_matrices -- upload them, don't recompute.
        , bool skip_animate = false
    );

    void draw (glm::mat4x4 const& model_view
        , std::vector<glm::mat4x4> const& instances
        , OpenGL::Scoped::use_program& m2_shader
        , OpenGL::M2RenderState& model_render_state
        , math::frustum const& frustum
        , const float& cull_distance
        , const glm::vec3& camera
        , int animtime
        , bool all_boxes
        , std::unordered_map<Model*, std::size_t>& model_boxes_to_draw
        , display_mode display
        , bool no_cull = false
        // Representative instance supplying the per-instance resolves the instanced draw can't do per-copy
        // (replaceable creature skin texture, geoset selection). All instances in ONE call must share it --
        // callers group creatures by (model, display) so each batch is a single skin. nullptr for doodads.
        , ModelInstance const* representative = nullptr
        // Per-instance interior light (parallel to `instances`): rgb = sampled MOCV floor colour,
        // a = 1 indoors. Empty = all outdoor. The draw partitions instances by this value into sub-draws.
        , std::vector<glm::vec4> const& instance_interior = {}
        // Per-instance distance fade 0..1 (parallel to `instances`): alpha ramp over the last stretch
        // before the render distance so doodads dissolve instead of popping. Empty = no fade (1.0).
        , std::vector<float> const& instance_fades = {}
        // Per-instance bone matrices (perf 2026-07-20): instances.size() * bone_matrix_count matrices,
        // instance i's slice at [i*count, (i+1)*count). Each billboard doodad's own CPU-baked bones so
        // they draw INSTANCED (one drawElementsInstanced per interior group) instead of one draw each.
        // Non-empty => per-instance bone slices + skip the single shared animate(). Empty = unchanged.
        , std::vector<glm::mat4x4> const& per_instance_bones = {}
    );

    void drawParticles(glm::mat4x4 const& model_view
        , OpenGL::Scoped::use_program& particles_shader
        , std::size_t instance_count
    );

    // Draw particles/ribbons for a FILTERED set of instance transforms (uploads them to the
    // instance buffer first). Used for the client-faithful particle draw range: the batched world
    // pass culls doodad instances beyond the range instead of stamping every placement's identical
    // cloud (IF Great Forge: stacked LavaSteam columns multiplied additively into a white core).
    void drawParticlesFiltered(glm::mat4x4 const& model_view
        , OpenGL::Scoped::use_program& particles_shader
        , std::vector<glm::mat4x4> const& transforms
    );

    void drawRibbonsFiltered(OpenGL::Scoped::use_program& ribbons_shader
        , std::vector<glm::mat4x4> const& transforms
    );

    // Draw this model's particles for ONE instance with the given world transform. Creature spawns
    // render through a per-instance path (not the batched model_with_particles set), so their emitters
    // are drawn here -- uploads the single transform to the instance buffer then reuses drawParticles.
    void drawParticlesForInstance(glm::mat4x4 const& model_view
        , OpenGL::Scoped::use_program& particles_shader
        , glm::mat4x4 const& transform
        , float model_alpha = 1.0f
    );

    void drawRibbons(OpenGL::Scoped::use_program& ribbons_shader
        , std::size_t instance_count
    );

    void drawBox(OpenGL::Scoped::use_program& m2_box_shader, std::size_t box_count);

    [[nodiscard]]
    std::vector<ModelRenderPass> const& renderPasses() const { return _render_passes; };

    void updateBoneMatrices();

    void initRenderPasses(ModelView const* view, ModelTexUnit const* tex_unit, ModelGeoset const* model_geosets);

  private:

    void setupVAO(OpenGL::Scoped::use_program& m2_shader);
    void fixShaderIdBlendOverride();
    void fixShaderIDLayer();
    void computePixelShaderIDs();


    Model* _model;

    // buffers
    OpenGL::Scoped::deferred_upload_buffers<6> _buffers;
    OpenGL::Scoped::deferred_upload_vertex_arrays<2> _vertex_arrays;

    std::vector<uint16_t> const _box_indices = {5, 7, 3, 2, 0, 1, 3, 1, 5, 4, 0, 4, 6, 2, 6, 7};

    GLuint const& _vao = _vertex_arrays[0];
    GLuint const& _transform_buffer = _buffers[0];
    GLuint const& _vertices_buffer = _buffers[1];
    GLuint const& _indices_buffer = _buffers[3];
    GLuint const& _box_indices_buffer = _buffers[4];
    GLuint const& _bone_matrices_buffer = _buffers[5];

    GLuint const& _box_vao = _vertex_arrays[1];
    GLuint const& _box_vbo = _buffers[2];

    // 0 = not generated. This raw GLuint is the sentinel the guarded deletes in unload() and ~ModelRender
    // key off, so it MUST start at 0: a model that loads but is never drawn never runs upload() (which
    // otherwise zeroes it), and the destructor would then read an uninitialized name and delete garbage.
    GLuint _bone_matrices_buf_tex = 0;
    std::size_t _bone_matrices_buffer_size = 0;
    std::vector<glm::vec3> _vertex_box_points;
    std::vector<ModelRenderPass> _render_passes;

    bool _uploaded = false;
    bool _vao_setup = false;
  };
}

#endif //NOGGIT_MODELRENDER_HPP
