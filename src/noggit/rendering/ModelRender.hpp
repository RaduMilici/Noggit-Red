// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_MODELRENDER_HPP
#define NOGGIT_MODELRENDER_HPP

#include <noggit/rendering/BaseRender.hpp>
#include <noggit/ModelHeaders.h>
#include <noggit/tool_enums.hpp>
#include <opengl/scoped.hpp>
#include <opengl/shader.hpp>
#include <math/frustum.hpp>
#include <tuple>

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
    // Modern (Legion+ s_modelShaderEffect) combiners the MD21 path can select; expressions in m2_frag.glsl
    // follow the client-derived WoWViewerCpp commonM2Material (docs/client_re/42 sec 3).
    Combiners_Mod_AddAlpha,          // 23
    Combiners_Mod_AddAlpha_Alpha,    // 24
    Combiners_Opaque_Alpha_Alpha,    // 25
    Combiners_Opaque_ModNA_Alpha,    // 26
    Combiners_Mod_Add_Alpha,         // 27
    Combiners_Opaque_Alpha,          // 28
    Combiners_Opaque_Mod_Add_Wgt,    // 29
  };

  enum class texture_unit_lookup : int
  {
    environment,
    t1,
    t2,
    none
  };

  // [perf 2026-08-05] MDI batching group-key + per-instance texture layers for ONE static doodad render pass.
  // Passes with an identical identity() can be collapsed into a single glMultiDrawElementsIndirect call. The
  // texture-array LAYER (layer0/layer1) is deliberately NOT part of the identity -- it rides the per-instance
  // inst_tex stream, so passes sampling different layers of the SAME array object still batch together.
  struct StaticBatchKey
  {
    GLuint tex_array0 = 0;   // GL_TEXTURE_2D_ARRAY object on unit 1 (0 => pass not resolved)
    GLuint tex_array1 = 0;   // unit 2 (0 => single-texture pass)
    int    tu_lookup0 = 0;
    int    tu_lookup1 = 0;
    int    pixel_shader = 0;
    int    tex_clamp0 = 0;
    int    tex_clamp1 = 0;
    uint16_t blend_mode = 0;
    // Era bit for the TWO-ERA alpha-key law (doc 40 sec 10): classic v256 models alpha-key at
    // 128/255 WITH src-alpha blending (1.12 table 0x8120D4 + turtle capture); wotlk models at
    // 224/255 with blending OFF (3.3.5a FUN_0081fe90 + table 0xa453b0 row 0). Only set for
    // blend 1 so groups of other blends never split on era.
    bool   classic_alpha = false;
    // Third era: MD21 models alpha-key at 128/255 (Legion+ client; WoWViewerCpp `discardAlpha < 0.50196`),
    // docs/client_re/42 sec 3. Only set for blend 1, like classic_alpha.
    bool   modern_alpha = false;
    bool   backface_cull = true;
    // [pib-MDI 2026-08-07] carried for the billboard-doodad batch only (glow cards are commonly unlit and/or
    // unfogged; the tile-doodad batch always resolves these to false, so its grouping is unchanged).
    bool   unfogged = false;
    bool   unlit = false;
    // per-instance (NOT identity)
    int    layer0 = 0;
    int    layer1 = 0;
    // [VULKAN phase C] the BLPs behind tex_array0/1 -- the Vulkan backend addresses textures by a bindless
    // index keyed on the file name, not by (GL array, layer). Excluded from the identity tuple below, like
    // layer0/layer1: two batches differing only in texture NAME already differ in tex_array/layer.
    std::string blp0;
    std::string blp1;

    auto identity() const
    {
      return std::tie(tex_array0, tex_array1, tu_lookup0, tu_lookup1, pixel_shader,
                      tex_clamp0, tex_clamp1, blend_mode, classic_alpha, modern_alpha, backface_cull, unfogged, unlit);
    }
    bool operator<(StaticBatchKey const& o) const { return identity() < o.identity(); }
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
    // 32-bit: Legion+ skins address up to (Level << 16) | start -- the Forever Beta HD character skins
    // hold 147,966 triangle indices. As uint16 the shifted start truncated straight back to the raw value,
    // so the section-Level fix in initRenderPasses never reached the draw call (docs/client_re/42 sec 22).
    uint32_t index_start = 0, index_count = 0, vertex_start = 0, vertex_end = 0;
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

    // [perf 2026-08-05] MDI batching: fill `out` with this pass's group-key + per-instance texture layers,
    // resolving only the model's BASE textures (no instance/special-texture path). Returns false when the pass
    // is NOT batchable -> the caller must draw it the classic per-model way. Mirrors the batchable subset of
    // prepareDraw/bindTexture: rejects special/replaceable textures, animated UV, animated bones, non-default
    // colour/opacity/flags, non-Opaque/Alpha_Key blend, creature/character/lightray/water-effect models, and
    // any texture not yet loaded+uploaded (deferred this frame). See StaticBatchKey.
    // for_pib widens the gate for the billboard-doodad (per-instance-animation) batch: additive/alpha blends
    // (2..4) and unfogged/unlit passes become batchable (carried per-group in the key). Default keeps the
    // strict tile-doodad subset.
    // [creature MDI 2026-08-18] `rep` = the representative instance for a creature batch. When non-null the
    // classifier resolves the group's REPLACEABLE skin (array,layer) from rep->replaceTextures() instead of
    // rejecting special textures, filters passes by rep's geoset visibility instead of m->showGeosets, and
    // skips the "creature model" reject -- so shared-pose creature groups can fold into the pib-style MDI.
    // rep == nullptr keeps the exact doodad behaviour (special tex -> reject, m->showGeosets, creature reject).
    [[nodiscard]] bool resolveStaticBatch(Model* m, StaticBatchKey& out, bool for_pib = false,
                                          ModelInstance const* rep = nullptr) const;

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
    // [2026-09-04 NATIVE VK DEADLOCK FIX] upload() is what fills Model::_textures (from
    // _textureFilenames), and it was only ever reached lazily from GL's own draw paths. With VK
    // owning the M2 pass, GL's draw is gated off -> the model never uploads -> _textures stays
    // empty -> resolveStaticBatch rejects texture unit 0 (rej12/tex3) -> VK refuses the model ->
    // it is never drawn -> it never uploads. A closed loop: every doodad first seen AFTER VK took
    // ownership stayed permanently invisible (Westfall trees/grass/rocks, 136k instances).
    // The VK feed calls this so a model uploads because VULKAN wants it, not only because GL drew it.
    // MUST stay gated on finishedLoading(). upload() builds Model::_textures from
    // _textureFilenames and then CLEARS the filenames, and sets _uploaded -- it is strictly
    // one-shot. Called while the async load is still filling those filenames it produces a SHORT
    // _textures array, destroys the data needed to ever repair it, and never runs again; the model
    // is then permanently rejected (tex >= _textures.size() = rej12/tex3) and invisible forever.
    // GL only ever reached upload() from its draw path, which never runs on an unloaded model --
    // this restores that protection for the VK feed.
    void ensureUploaded();   // defined in the .cpp: needs the complete Model type
    [[nodiscard]] bool uploaded() const { return _uploaded; }
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

    // [perf 2026-08-05] Draw a whole persistent doodad bucket from an EXTERNAL, tile-owned instance buffer
    // (TileRender::DoodadInstanceBuffer) -- no per-frame gather, no transform upload, no per-instance cull.
    // The caller frustum-culls at TILE granularity and the fragment shader distance-clips (slice_dist); this
    // just binds the external transform + interior buffers to the shared VAO's instance attributes and draws
    // every pass once at full alpha (fade=1). For static tile M2 doodads only (outdoor, no per-instance
    // bones / no emitters). transform_vbo carries each instance's full world matrix; interior_vbo the
    // per-instance room light (all-zero for tile doodads). representative=nullptr (doodads).
    void drawPersistent(glm::mat4x4 const& model_view
        , GLuint transform_vbo
        , GLuint interior_vbo
        , int instance_count
        , OpenGL::Scoped::use_program& m2_shader
        , OpenGL::M2RenderState& model_render_state
        , int animtime
        , float extra_alpha = 1.0f // <1 => distance fade: alpha-blend the whole bucket (still lit)
    );

    // True iff drawPersistent() would actually render this model. The persistent buffer build MUST gate on
    // this exact predicate: a model put in the buffer but skipped by drawPersistent gets skipped in the
    // dynamic gather too (it's "persistent") and then renders NOWHERE -> missing pieces of multi-model
    // structures (the fragmented-building bug). Mirrors drawPersistent's early-returns + the emitter exclusion.
    [[nodiscard]] bool eligibleForPersistentDraw() const;

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
    void hideEmitterPlaceholderCards();

  private:

    void setupVAO(OpenGL::Scoped::use_program& m2_shader);
    void fixShaderIdBlendOverride();
    void fixShaderIDLayer();
    void computePixelShaderIDs();


    Model* _model;

    // buffers
    OpenGL::Scoped::deferred_upload_buffers<7> _buffers;
    OpenGL::Scoped::deferred_upload_vertex_arrays<2> _vertex_arrays;

    std::vector<uint16_t> const _box_indices = {5, 7, 3, 2, 0, 1, 3, 1, 5, 4, 0, 4, 6, 2, 6, 7};

    GLuint const& _vao = _vertex_arrays[0];
    GLuint const& _transform_buffer = _buffers[0];
    GLuint const& _vertices_buffer = _buffers[1];
    GLuint const& _indices_buffer = _buffers[3];
    GLuint const& _box_indices_buffer = _buffers[4];
    GLuint const& _bone_matrices_buffer = _buffers[5];
    // [perf 2026-08-05] per-instance INTERIOR light attribute (divisor 1), parallel to _transform_buffer.
    // Moving interior off the per-draw uniform onto a vertex attribute lets instances with DIFFERENT room
    // colours batch in ONE instanced draw -- previously each distinct interior value forced its own sub-draw
    // group (WMO doodads in a city split ~6 ways -> groups/model=6.1, the dominant SubmitInst cost).
    GLuint const& _interior_buffer = _buffers[6];

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
