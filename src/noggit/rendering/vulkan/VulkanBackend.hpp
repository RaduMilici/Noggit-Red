// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// [VULKAN PHASE 0 -- foundation, 2026-08-07] The Vulkan backend that will progressively take over the heavy
// 3D scene rendering (the committed ceiling-breaker past single-thread GL submission: ~5-10x cheaper draws,
// multi-threaded command recording, descriptor-indexed textures). Architecture: VK renders the scene into an
// OFFSCREEN image exported via VK_KHR_external_memory_win32; the existing GL/Qt pipeline IMPORTS it
// (GL_EXT_memory_object_win32) and composites it in the viewport -- the whole Qt UI/tooling stack stays
// untouched, GL remains the default renderer, and content moves across pass by pass (terrain -> M2 MDI
// batches -> WMO -> water/sky) behind a toggle until parity.
//
// Phase 0 scope (this file): dynamic loader (vulkan-1.dll ships with the NVIDIA driver -- NO SDK dependency;
// headers vendored in src/external/vulkan-headers), instance/device/queue/command pool, one exportable RGBA8
// image + exportable binary semaphores, and a per-frame animated clear as the interop proof of life.
#pragma once

#ifdef _WIN32

// Core Vulkan only in the HEADER (VK_NO_PROTOTYPES; entry points loaded dynamically). The win32 platform
// header (vulkan_win32.h -- needs <windows.h> types) is included ONLY in the .cpp, so including this header
// never drags windows.h into other translation units. Win32-specific PFNs are stored type-erased.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace Noggit::Rendering::VK
{
  class VulkanBackend
  {
  public:
    // Loads vulkan-1.dll + creates instance/device/pool and the exportable image/semaphores.
    // Returns false (and stays inert) on ANY failure -- callers fall back to pure GL silently.
    bool init(std::uint32_t width, std::uint32_t height);
    void shutdown();
    [[nodiscard]] bool ready() const { return _ready; }

    // Per-frame render into the shared image, signalling vk_done. wait_gl_done: false on the very first
    // frame (GL hasn't signalled yet). mvp16: noggit's projection*model_view, column-major (GL clip
    // conventions -- the vertex shader converts). With a terrain mesh uploaded this draws the REAL terrain
    // (depth-tested); before any mesh it draws the ring-pattern skeleton test. False on submit failure.
    bool renderFrame(float time_seconds, bool wait_gl_done, float const* mvp16);

    // [VK-1] upload/replace the terrain mesh. Vertices are INTERLEAVED pos.xyz + smooth normal.xyz +
    // MCCV colour.rgb (9 floats / 36 bytes each); uint32 indices. Host-visible one-shot upload; called from
    // the GL side when the camera's tile neighbourhood changes. Waits the queue idle first (editor cadence).
    bool setTerrainMesh(float const* pos_normal_interleaved, std::size_t vertex_count,
                        std::uint32_t const* indices, std::size_t index_count);

    // [overnight stage 3] liquid surface mesh, SAME 36B format (colour = 1,1,1); translucent after terrain.
    bool setWaterMesh(float const* pos_normal_interleaved, std::size_t vertex_count,
                      std::uint32_t const* indices, std::size_t index_count);

    // [phase I] FRUSTUM-CULLED TERRAIN. The whole 3x3 tile neighbourhood was drawn every frame with
    // one draw over all ~8.8M indices, while GL draws only the chunks it can see. The mesh stays as
    // one buffer (it is tile-bound and rebuilt rarely); visibility is expressed as a per-frame
    // indirect command list holding just the visible chunks.
    struct ChunkDraw { std::uint32_t index_count, first_index; std::int32_t base_vertex; };
    bool setTerrainVisibleChunks(ChunkDraw const* draws, std::size_t count);
    // ADT water was never culled at all: 1.54M alpha-blended indices every frame, MORE than the
    // culled terrain, and blended pixels are the expensive kind. Same per-chunk indirect list.
    bool setWaterVisibleChunks(ChunkDraw const* draws, std::size_t count);

    // WMO liquid is VIEW-bound (it depends on which WMOs are visible) while ADT water is TILE-bound.
    // They used to share one mesh, which meant rebuilding, copying and comparing the ENTIRE water
    // vector every frame just to re-append a few thousand WMO vertices -- ~34 MB of memcpy+compare
    // per frame on a harbour view. They are now two buffers and two draws through the same pipeline.
    bool setWmoLiquidMesh(float const* verts, std::size_t vertex_count,
                          std::uint32_t const* indices, std::size_t index_count);

    // [phase F] the zone's sky gradient dome (Skies' own mesh + per-vertex band colours). Positions
    // are camera-relative; the shader adds the camera. Feeding it switches the sky pass off the
    // placeholder gradient. Pass a null/empty mesh to fall back to the placeholder.
    bool setSkyDome(float const* positions_xyz, float const* colors_rgb, std::size_t vertex_count,
                    std::uint16_t const* indices, std::size_t index_count);
    bool skyDomeReady() const { return _skydome_pipeline && _skydome_index_count; }

    // [phase F] the cloud deck: the client's cloud cap mesh (pos | uv | row alpha) plus the
    // bindless index of the CPU-ticked cloud texture and the live opacity.
    bool setCloudDome(float const* pos_uv_alpha, std::size_t vertex_count,
                      std::uint16_t const* indices, std::size_t index_count);
    void setCloudParams(std::int32_t texture_index, float opacity)
    { _cloud_tex_index = texture_index; _cloud_opacity = opacity; }
    bool cloudDomeReady() const { return _cloud_pipeline && _cloud_index_count; }

    // [phase F] celestial billboards (sun disc + glare, both moons + glare). One record per draw,
    // in the order GL issues them; positions are camera-relative (see celestial.vert).
    struct Celestial
    {
      float center_rel[3];
      float half_size;
      float right[3];
      float opacity;
      float up[3];
      float tex_index;
      float color[3];
      float additive;
    };
    void setCelestials(std::vector<Celestial> const& list) { _celestials = list; }
    bool celestialsReady() const { return _cel_pipeline_add && _cel_pipeline_alpha; }

    // [phase G] M2 particle quads, already in world space (see VkParticleFeed). One draw per emitter,
    // in GL's order -- particles are order-dependent.
    struct ParticleDraw
    {
      std::uint32_t first_index;
      std::uint32_t index_count;
      std::int32_t  base_vertex;
      std::int32_t  tex_index;
      float         blend;
      float         alpha_test;
      float         alpha_mod;
      float         ribbon;      // 1 = skip the particle black-fringe divide
    };
    bool setParticles(float const* verts, std::size_t vertex_count,
                      std::uint32_t const* indices, std::size_t index_count,
                      ParticleDraw const* draws, std::size_t draw_count);
    bool particlesReady() const { return _particle_pipelines[0] != VK_NULL_HANDLE; }

    // [overnight stage 4] instanced M2 doodads (clay). Geometry = concatenated per-model pos+normal verts +
    // uint32 indices; instances = one mat4 (16 floats) per instance, all models' instances in one stream;
    // draws = per-model ranges into all three. Replaced wholesale on neighbourhood change.
    struct DoodadDraw
    {
      std::uint32_t first_index = 0;
      std::uint32_t index_count = 0;
      std::int32_t base_vertex = 0;
      std::uint32_t first_instance = 0;
      std::uint32_t instance_count = 0;
    };
    bool setDoodads(float const* pn_verts, std::size_t vertex_count,
                    std::uint32_t const* indices, std::size_t index_count,
                    float const* instance_mat4s, std::size_t instance_count,
                    DoodadDraw const* draws, std::size_t draw_count);

    // [overnight 04:11, DORMANT until called] replace the sampled ground texture with caller-provided RGBA8
    // pixels (e.g. a decoded BLP). Creates a fresh image via the same staging path, repoints the descriptor,
    // destroys the old image after a queue idle. Daylight BLP wiring becomes one call.
    bool setGroundTexture(std::uint32_t const* rgba, std::uint32_t width, std::uint32_t height);

    // ---- [VULKAN phase B, 2026-08-29] TEXTURED TERRAIN (the first pass VK owns) ----
    // Bindless tileset textures: RGBA8 level-0 pixels, mips generated here; returns the index into the
    // descriptor-indexed sampler2D array (or -1). Uploads are one-shot (queue idle).
    std::int32_t addTexture(std::uint32_t const* rgba, std::uint32_t width, std::uint32_t height);
    // Re-upload an EXISTING bindless slot in place. addTexture() allocates a new slot every call,
    // which cannot serve a texture that is regenerated on a timer (the cloud deck, 10 Hz).
    bool updateTexture(std::int32_t index, std::uint32_t const* rgba,
                       std::uint32_t width, std::uint32_t height);
    // RGBA8 with the source's OWN mip chain (level 0 first) -- GL uploads the BLP's mips, so generating
    // our own box mips made distant/steep terrain sample differently.
    std::int32_t addTextureMips(std::vector<std::vector<std::uint8_t>> const& rgba_mips,
                                std::uint32_t width, std::uint32_t height);
    // Block-compressed BLPs (DXT1/3/5 = BC1/BC2/BC3) with their own mip chain, uploaded as-is.
    std::int32_t addTextureCompressed(VkFormat format, std::uint32_t width, std::uint32_t height,
                                      std::vector<std::vector<std::uint8_t>> const& mips);
    // Per-chunk data (std430 mirror of GL's ChunkInstanceData subset). 64 bytes.
    struct TerrainChunk
    {
      std::int32_t tex[4];        // bindless texture index per layer, -1 = none
      std::int32_t layer_count;   // 0..4
      std::int32_t holes;         // MCNK hole bits (already applied to the index buffer; kept for the shader)
      std::int32_t pad0, pad1;
      std::int32_t anim[4];       // bit0 uv-anim enabled, bit1 overbright, bits 8-10 speed, bits 16-18 rotation
      float origin_x, origin_z;   // chunk world origin (uv = (pos - origin) / CHUNKSIZE * 8)
      float pad2, pad3;
    };
    // Whole textured neighbourhood: vertices = pos3+normal3+mccv3 (36 B, as setTerrainMesh) + a parallel
    // per-vertex chunk index stream; alphamaps = chunk_count x 64x64 RGBA8 (r,g,b = layers 1..3 alpha);
    // shadows = chunk_count x 64x64 R8. Chunk images are packed into 2D atlases (48 chunks per row).
    // alpha_rgba8/shadow_r8 arrive ALREADY IN ATLAS LAYOUT (atlas_w x atlas_h), so there is no
    // repack here at all; only atlas rows [dirty_row_lo, dirty_row_hi] (in 64px chunk rows) changed.
    bool setTerrainTextured(float const* pn_verts, std::size_t vertex_count,
                            std::uint32_t const* chunk_index_per_vertex,
                            std::uint32_t const* indices, std::size_t index_count,
                            TerrainChunk const* chunks, std::size_t chunk_count,
                            std::uint8_t const* alpha_rgba8, std::uint8_t const* shadow_r8,
                            std::uint32_t atlas_w, std::uint32_t atlas_h,
                            std::uint32_t dirty_row_lo, std::uint32_t dirty_row_hi);
    [[nodiscard]] bool terrainTextured() const { return _tt_ready; }

    // ---- [VULKAN phase C, 2026-08-29] M2 / DOODAD BATCHES (the Vulkan twin of drawDynamicBatched) ----
    // Arena: the SAME ModelVertex blob + uint16 indices GL uploads into its MDI arena; uploaded once per
    // batch-set change and addressed by per-draw first_index/vertex_offset.
    // ---- [VULKAN phase D] WMO pass ----
    struct WmoDraw
    {
      std::uint32_t index_count;
      std::uint32_t first_index;
      std::int32_t  base_vertex;
      std::uint32_t xform_index;
      std::int32_t  blend_mode;
      std::int32_t  backface_cull;
    };
    bool setWmoArena(void const* vertices, std::size_t vertex_bytes,
                     std::uint16_t const* indices, std::size_t index_count);
    bool setWmoFrame(float const* transforms, float const* ambients, std::size_t transform_count,
                     WmoDraw const* draws, std::size_t draw_count);
    // one ivec4 per arena batch: (tex0 bindless id, tex1 bindless id, shader, flags)
    bool setWmoBatches(std::int32_t const* records, std::size_t batch_count);
    bool wmoAvailable() const { return _wmo_pipeline != VK_NULL_HANDLE; }

    bool setM2Arena(void const* model_vertices, std::size_t vertex_bytes,
                    std::uint16_t const* indices, std::size_t index_count);
    // Matches VkDrawIndexedIndirectCommand exactly (fed straight into vkCmdDrawIndexedIndirect).
    struct M2Draw
    {
      std::uint32_t index_count = 0;
      std::uint32_t instance_count = 0;
      std::uint32_t first_index = 0;
      std::int32_t vertex_offset = 0;
      std::uint32_t first_instance = 0;
    };
    // Per-frame streams: instance transforms (mat4 each), interiors (vec4 each), tex/bone info (ivec4:
    // x,y = bindless texture ids, z = bone base, w = bone count), the shared bone block, and the draws.
    bool setM2Frame(float const* transforms, float const* interiors, std::int32_t const* tex_info,
                    std::int32_t const* state_info,
                    std::int32_t const* group_info, std::size_t group_count,
                    float slice_dist,
                    std::size_t instance_count,
                    float const* bone_mat4s, std::size_t bone_count,
                    M2Draw const* draws, std::size_t draw_count);
    [[nodiscard]] bool m2Ready() const { return _m2_pipeline != VK_NULL_HANDLE && _m2_draw_count > 0; }
    [[nodiscard]] bool m2Available() const { return _m2_pipeline != VK_NULL_HANDLE; }
    // [finding 161] The water equivalent of m2Ready(). VK draws water only when BOTH of these hold
    // (see the draw: `_water_pipeline && _water_index_count`), so anything less means VK renders no
    // water at all -- and GL must not be gated off in that case.
    [[nodiscard]] bool waterReady() const { return _water_pipeline != VK_NULL_HANDLE && _water_index_count != 0; }
    [[nodiscard]] bool terrainTexturedAvailable() const { return _tt_pipeline != VK_NULL_HANDLE && _tt_dset != VK_NULL_HANDLE; }
    // Per-frame lighting/fog block (byte copy of OpenGL::LightingUniformBlock) followed by 3 extra vec4s
    // the shader appends: camera xyz, sun-spec rgb + spec-on flag, toggles (draw_shadows, draw_fog).
    // extra tail = 4 vec4: Camera_Pad, SunSpec_Pad, Toggles, SheenDir_Pad (liquid sun sheen)
    // extra = 5 trailing vec4: Camera_Pad, SunSpec_Pad, Toggles, SheenDir_Pad, CamFwd_DetailDist.
    void setLighting(void const* block, std::size_t bytes, float const* extra_4vec4);

    // Win32 HANDLEs for the GL import (opaque win32; ownership stays with VK).
    [[nodiscard]] void* imageMemoryHandle() const { return _image_mem_handle; }
    [[nodiscard]] std::uint64_t imageMemorySize() const { return _image_mem_size; }
    [[nodiscard]] void* depthMemoryHandle() const { return _depth_mem_handle; }   // null when export failed
    [[nodiscard]] std::uint64_t depthMemorySize() const { return _depth_mem_size; }
    // [phase A] the R32F "depth as colour" attachment (GL imports it as GL_R32F; .r = window depth)
    [[nodiscard]] void* zMemoryHandle() const { return _z_mem_handle; }
    [[nodiscard]] std::uint64_t zMemorySize() const { return _z_mem_size; }
    [[nodiscard]] void* vkDoneSemaphoreHandle() const { return _vk_done_handle; }
    [[nodiscard]] void* glDoneSemaphoreHandle() const { return _gl_done_handle; }
    [[nodiscard]] std::uint32_t width() const { return _width; }
    [[nodiscard]] std::uint32_t height() const { return _height; }

    ~VulkanBackend() { shutdown(); }

  private:
    bool loadLoader();
    bool createInstance();
    bool pickDeviceAndQueue();
    bool createDevice();
    bool createSharedImage();
    bool createSemaphores();
    // [VK-1 skeleton] real GRAPHICS PIPELINE over the shared image: image view + render pass + framebuffer,
    // SPIR-V modules loaded from <exe dir>/vk_shaders (compiled at build by the vendored glslang), fullscreen
    // pipeline with a push-constant clock. renderTestFrame draws through it (no more bare clears).
    bool createRenderTarget();
    bool createPipeline();
    VkShaderModule loadShaderModule(char const* filename); // from <exe>/vk_shaders/
    // [VK-1b]
    bool createDepthTarget();   // D32 depth image/view for the terrain pass
    bool createTerrainPipeline();
    // copy_bytes defaults to the allocation size; pass it explicitly when the buffer is
    // deliberately larger than the payload (see writeHostBuffer's growth headroom).
    bool createHostBuffer(VkBufferUsageFlags usage, std::size_t bytes,
                          VkBuffer& buf, VkDeviceMemory& mem, void const* data,
                          std::size_t copy_bytes = 0); // host-visible one-shot

    bool _ready = false;
    void* _dll = nullptr; // HMODULE

    VkInstance _instance = VK_NULL_HANDLE;
    VkPhysicalDevice _phys = VK_NULL_HANDLE;
    std::uint32_t _queue_family = 0;
    std::uint32_t _queue_family_count = 1;   // queues this family offers (a 2nd = the copy queue)
    VkDevice _device = VK_NULL_HANDLE;
    VkQueue _queue = VK_NULL_HANDLE;
    VkCommandPool _pool = VK_NULL_HANDLE;
    VkCommandBuffer _cmd = VK_NULL_HANDLE;

    // [phase H] MULTITHREADED COMMAND RECORDING.
    //
    // The render pass body is recorded into SECONDARY command buffers instead of inline, so the
    // work can be split across threads: a Vulkan command pool is not thread-safe, so each recorder
    // owns its own pool and its own secondary buffer, and the primary just executes them in a fixed
    // order (vkCmdExecuteCommands preserves the order given, which is what keeps the alpha-blended
    // passes compositing correctly even though they were recorded concurrently).
    struct Recorder
    {
      VkCommandPool pool = VK_NULL_HANDLE;
      VkCommandBuffer cmd = VK_NULL_HANDLE;
    };
    // One per pass group -- see RecordGroup in the .cpp.
    std::vector<Recorder> _recorders;
    // The buffer the pass-recording helpers write into. Each helper is handed its own recorder, so
    // this is only the single-threaded fallback path.
    VkCommandBuffer _rec = VK_NULL_HANDLE;
    bool createRecorders(std::size_t count);
    void destroyRecorders();
    // Pass groups, executed by the primary in this order (see recordGroup).
    enum RecordGroup { kGroupSky = 0, kGroupTerrain, kGroupWmo, kGroupModels, kGroupOverlays,
                       kGroupCount };
    void recordGroup(int group, VkCommandBuffer rec, float t, float const* mvp16);
    void recordGroups(float t, float const* mvp16);

    // Worker threads that record the pass groups. They are parked on a condition variable between
    // frames; NOGGIT_VK_RECORD_THREADS=0 records inline instead (kept as the A/B for the port).
    std::vector<std::thread> _record_threads;
    std::mutex _record_mutex;
    std::condition_variable _record_cv;
    std::condition_variable _record_done_cv;
    unsigned _record_epoch = 0;
    unsigned _record_outstanding = 0;
    float _record_t = 0.f;
    float const* _record_mvp = nullptr;
    bool _record_quit = false;
    bool _record_threads_started = false;
    void startRecordThreads();
    void stopRecordThreads();

    // [phase H] SUBMIT THREAD. vkQueueSubmit plus the fence wait are the two calls that block the
    // caller inside the driver; moving them onto their own thread lets the GL thread carry on with
    // its own frame work and only pay the wait at the point it actually needs the rendered images
    // (waitFrameComplete, called immediately before the compose). The queue is single-slot: this
    // renderer submits exactly one command buffer per frame.
    std::thread _submit_thread;
    std::mutex _submit_mutex;
    std::condition_variable _submit_cv;
    std::condition_variable _submit_done_cv;
    bool _submit_pending = false;      // work handed over, not yet submitted+completed
    bool _submit_quit = false;
    bool _submit_started = false;
    bool _submit_wait_gl = false;
    VkResult _submit_result = VK_SUCCESS;
    void startSubmitThread();
    void stopSubmitThread();
    void submitFrameAsync(bool wait_gl_done);

  public:
    // Blocks until the frame handed to the submit thread has finished on the GPU. Safe to call when
    // no frame is in flight. Returns false if the submission itself failed.
    bool waitFrameComplete();

  private:
    // Where the VK frame actually goes: recording (CPU) vs waiting on the fence (GPU).
    double _stat_record_ms = 0.0;
    double _stat_gpu_wait_ms = 0.0;
    unsigned _stat_frames = 0;
  public:

  private:
    VkFence _fence = VK_NULL_HANDLE;

    std::uint32_t _width = 0, _height = 0;
    VkImage _image = VK_NULL_HANDLE;
    VkDeviceMemory _image_mem = VK_NULL_HANDLE;
    std::uint64_t _image_mem_size = 0;
    void* _image_mem_handle = nullptr;
    bool _image_initialized = false; // first barrier comes from UNDEFINED
    // [VULKAN phase A, 2026-08-29] DEPTH AS COLOUR: a second exportable R32F colour attachment that every
    // fragment shader writes gl_FragCoord.z into. The D32 depth image import into GL read as garbage
    // (probe: -inf/3e38/NaN -- depth tiling/compression metadata is not importable), while the RGBA8
    // colour import is proven; so GL gets depth through the same proven colour route.
    VkImage _z_image = VK_NULL_HANDLE;
    VkDeviceMemory _z_mem = VK_NULL_HANDLE;
    VkImageView _z_view = VK_NULL_HANDLE;
    std::uint64_t _z_mem_size = 0;
    void* _z_mem_handle = nullptr;
    bool createExportableImage(VkFormat format, VkImageUsageFlags usage,
                               VkImage& image, VkDeviceMemory& mem, std::uint64_t& size, void*& handle);

    VkSemaphore _vk_done = VK_NULL_HANDLE;
    VkSemaphore _gl_done = VK_NULL_HANDLE;
    void* _vk_done_handle = nullptr;
    void* _gl_done_handle = nullptr;

    // graphics pipeline over the shared image
    VkImageView _view = VK_NULL_HANDLE;
    VkRenderPass _render_pass = VK_NULL_HANDLE;
    VkFramebuffer _framebuffer = VK_NULL_HANDLE;
    VkPipelineLayout _pipe_layout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkPipeline _sky_pipeline = VK_NULL_HANDLE; // fullscreen zenith->horizon gradient behind the terrain
    // [phase F] the REAL sky: the zone's gradient dome mesh (Skies), drawn instead of the placeholder
    // whenever a dome has been fed. Own layout: push only, no descriptor sets.
    VkPipeline _skydome_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout _skydome_layout = VK_NULL_HANDLE;
    VkBuffer _skydome_vbo = VK_NULL_HANDLE;
    VkDeviceMemory _skydome_vbo_mem = VK_NULL_HANDLE;
    VkBuffer _skydome_ibo = VK_NULL_HANDLE;
    VkDeviceMemory _skydome_ibo_mem = VK_NULL_HANDLE;
    // grow-only capacities: the dome re-tints every frame as the clock advances, so it must
    // update IN PLACE rather than reallocate (see setSkyDome)
    std::size_t _skydome_vbo_cap = 0;
    std::size_t _skydome_ibo_cap = 0;
    // what the WMO descriptor set currently points at, so it is only rewritten when it changes
    VkBuffer _wmo_xforms_bound = VK_NULL_HANDLE;
    VkBuffer _wmo_amb_bound = VK_NULL_HANDLE;
    // Host-visible upload memory stays mapped for its lifetime. writeHostBuffer used to
    // vkMapMemory + memcpy + vkUnmapMemory on EVERY call, and it is called ~10x per frame (sky
    // dome x2, WMO transforms x2, M2 bones, terrain/water indirect, particles, WMO liquid...).
    // The memory is HOST_VISIBLE|HOST_COHERENT, so a persistent mapping is valid and needs no
    // flush; only the grow path has to unmap before it frees.
    std::unordered_map<VkDeviceMemory, void*> _persistent_maps;
    void* mapPersistent(VkDeviceMemory mem);
    void unmapPersistent(VkDeviceMemory mem);
    // Re-upload pixels INTO an existing single-mip image (same extent/format) instead of building a
    // whole new VkImage. updateTexture used to allocate an image + memory + view, full-stall the
    // graphics queue, then free the old one -- ten times a second for the cloud deck.
    bool updateTexturePixelsInPlace(std::int32_t index, void const* rgba,
                                    std::uint32_t width, std::uint32_t height);
    VkBuffer _texup_staging = VK_NULL_HANDLE;
    VkDeviceMemory _texup_staging_mem = VK_NULL_HANDLE;
    std::size_t _texup_staging_cap = 0;
    std::uint32_t _skydome_index_count = 0;
    float _sky_camera[3] = { 0.f, 0.f, 0.f };   // cached from the lighting extra, for the dome push
    VkPipeline _cloud_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout _cloud_layout = VK_NULL_HANDLE;
    VkBuffer _cloud_vbo = VK_NULL_HANDLE;
    VkDeviceMemory _cloud_vbo_mem = VK_NULL_HANDLE;
    VkBuffer _cloud_ibo = VK_NULL_HANDLE;
    VkDeviceMemory _cloud_ibo_mem = VK_NULL_HANDLE;
    std::uint32_t _cloud_index_count = 0;
    std::int32_t _cloud_tex_index = -1;
    VkPipeline _cel_pipeline_add = VK_NULL_HANDLE;    // premultiplied glow (ONE / ONE)
    VkPipeline _cel_pipeline_alpha = VK_NULL_HANDLE;  // moon discs (SRC_ALPHA / 1-SRC_ALPHA)
    VkPipelineLayout _cel_layout = VK_NULL_HANDLE;
    std::vector<Celestial> _celestials;
    // One pipeline per M2 particle blend mode (0..7): blend state is baked into a VK pipeline.
    // 0..7 = M2 particle blend modes; 8 = the RIBBON variant of blend 3 (SRC_COLOR/ONE, which
    // differs from the particle table's ONE/ONE).
    VkPipeline _particle_pipelines[9] = {};
    VkPipelineLayout _particle_layout = VK_NULL_HANDLE;
    VkBuffer _particle_vbo = VK_NULL_HANDLE;
    VkDeviceMemory _particle_vbo_mem = VK_NULL_HANDLE;
    VkBuffer _particle_ibo = VK_NULL_HANDLE;
    VkDeviceMemory _particle_ibo_mem = VK_NULL_HANDLE;
    std::vector<ParticleDraw> _particle_draws;
    std::size_t _particle_vbo_cap = 0;
    std::size_t _particle_ibo_cap = 0;
    float _cloud_opacity = 0.f;

    // [VK-1b] depth target + terrain mesh + terrain pipeline
    VkImage _depth_image = VK_NULL_HANDLE;
    VkDeviceMemory _depth_mem = VK_NULL_HANDLE;
    VkImageView _depth_view = VK_NULL_HANDLE;
    std::uint64_t _depth_mem_size = 0;   // exported for the GL depth-compose route (optional)
    void* _depth_mem_handle = nullptr;
    VkPipelineLayout _terrain_layout = VK_NULL_HANDLE; // mat4 mvp (vert) + time (frag) push block
    VkPipeline _terrain_pipeline = VK_NULL_HANDLE;
    VkBuffer _terrain_vbo = VK_NULL_HANDLE;
    VkDeviceMemory _terrain_vbo_mem = VK_NULL_HANDLE;
    VkBuffer _terrain_ibo = VK_NULL_HANDLE;
    VkDeviceMemory _terrain_ibo_mem = VK_NULL_HANDLE;
    std::size_t _terrain_vbo_cap = 0;
    std::size_t _terrain_ibo_cap = 0;
    // [spike fix] Buffers retired by a GROW are freed a few frames later instead of stalling the
    // queue to make the free safe. One frame is in flight, so three frames of latency is ample.
    struct RetiredBuffer { VkBuffer buf; VkDeviceMemory mem; int frames_left; };
    std::vector<RetiredBuffer> _retired;
    void retireBuffer(VkBuffer buf, VkDeviceMemory mem);
    void tickRetired();

    // [SPIKE FIX -- the 600 ms frames] Every texture upload used to be its own vkCreateBuffer +
    // vkAllocateMemory + vkQueueSubmit + BLOCKING vkWaitForFences + destroy + free. A tile load
    // streams dozens of textures at once, so a single frame paid dozens of full GPU round trips:
    // measured p99 534 ms / worst 607 ms while flying, against GL's p99 39 ms. Uploads now record
    // into ONE batch command buffer against a persistent staging arena and are submitted ONCE per
    // frame, so the count of textures no longer multiplies the number of stalls.
    VkBuffer _tex_arena = VK_NULL_HANDLE;
    VkDeviceMemory _tex_arena_mem = VK_NULL_HANDLE;
    std::size_t _tex_arena_cap = 0;
    std::size_t _tex_arena_used = 0;
    bool _tex_batch_open = false;
    // The copy queue is a DEDICATED second queue, so submission order does not order completion --
    // the upload batch had to be waited on by the CPU before the frame could sample it. That is a
    // per-frame stall whenever anything streams. Signal a semaphore instead and let the GRAPHICS
    // submit wait on it, so the CPU never blocks.
    VkSemaphore _copy_done = VK_NULL_HANDLE;
    bool _copy_signalled = false;
    static bool textureBatchEnabled();
    bool beginTextureBatch();
    void flushTextureUploads();

    // [SPIKE FIX] setTerrainTextured rebuilt the whole alpha+shadow atlas on EVERY tile change:
    // at 5120x9472 that is 194 MB (RGBA8) + 48 MB (R8) allocated, zeroed, refilled and re-uploaded
    // to change a handful of chunks. Measured 144 ms per call, several per flight -- the half-second
    // freezes. The CPU staging vectors and the GPU images are now kept and reused whenever the atlas
    // dimensions are unchanged, which is every case except the neighbourhood actually resizing.
    std::uint32_t _atlas_w = 0, _atlas_h = 0;
    bool uploadIntoImage(VkImage image, std::uint32_t width, std::uint32_t height,
                         void const* data, std::size_t bytes);
    // Same, but writing a horizontal BAND at a y offset -- the atlas only ever changes in the rows
    // belonging to a newly resident tile.
    bool uploadIntoImageRows(VkImage image, std::uint32_t width, std::uint32_t y0,
                             std::uint32_t rows, void const* data, std::size_t bytes);
    std::uint32_t _terrain_index_count = 0;
    // Dirty byte ranges for the next terrain upload, set by setTerrainDirtyRanges() and consumed
    // (and cleared) by the next setTerrainMesh/setTerrainTextured. Empty = upload everything.
    std::vector<std::pair<std::size_t, std::size_t>> _terrain_dirty_v, _terrain_dirty_i, _terrain_dirty_c;
    // Atlas rows to re-upload, as SEPARATE bands (one per repacked tile). A single min..max span
    // covering tiles at slot 0 and slot 63 spans the whole 205-row atlas and uploads ~268 MB for two
    // tiles' worth of change -- that was the 354 ms rebuild left after the pack budget.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> _atlas_dirty_bands;
    std::vector<std::pair<std::size_t, std::size_t>> _water_dirty_v, _water_dirty_i;
  public:
    void setAtlasDirtyBands(std::vector<std::pair<std::uint32_t, std::uint32_t>> b)
    { _atlas_dirty_bands = std::move(b); }
    // ADT water lives in a persistent, slot-allocated buffer; only the tiles repacked this frame
    // need copying. Empty = copy everything (first build, or a caller that does not track ranges).
    void setWaterDirtyRanges(std::vector<std::pair<std::size_t, std::size_t>> v,
                             std::vector<std::pair<std::size_t, std::size_t>> i)
    { _water_dirty_v = std::move(v); _water_dirty_i = std::move(i); }
    void setTerrainDirtyRanges(std::vector<std::pair<std::size_t, std::size_t>> v,
                               std::vector<std::pair<std::size_t, std::size_t>> i,
                               std::vector<std::pair<std::size_t, std::size_t>> c)
    { _terrain_dirty_v = std::move(v); _terrain_dirty_i = std::move(i); _terrain_dirty_c = std::move(c); }
  private:

    // [overnight stage 6] DESCRIPTOR infrastructure (the gateway to all texturing): sampler + set layout +
    // pool + one set pointing at a generated ground texture (staging-uploaded OPTIMAL image). The terrain
    // pipeline layout includes the set; terrain.frag samples by world XZ. Real BLPs replace the image later.
    bool createTextureInfra();
    VkSampler _sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout _dsl = VK_NULL_HANDLE;
    VkDescriptorPool _dpool = VK_NULL_HANDLE;
    VkDescriptorSet _dset = VK_NULL_HANDLE;
    VkImage _ground_image = VK_NULL_HANDLE;
    VkDeviceMemory _ground_mem = VK_NULL_HANDLE;
    VkImageView _ground_view = VK_NULL_HANDLE;

    // [VULKAN phase B] textured terrain: descriptor-indexed tileset array (set 1) + chunk atlases + SSBO
    bool createTerrainTexInfra();      // set layout/pool/set for the textured path (needs descriptor indexing)
    bool createTerrainTexPipeline();   // terrain_tex.vert/frag, 2 vertex bindings (36 B verts + u32 chunk index)
    bool createM2Pipeline();           // m2.vert/frag: arena verts + 3 per-instance streams, indirect draws
    // M2 arena + per-frame streams (host-visible, grown on demand)
    VkBuffer _m2_vbo = VK_NULL_HANDLE;      VkDeviceMemory _m2_vbo_mem = VK_NULL_HANDLE;  std::size_t _m2_vbo_cap = 0;
    std::size_t _m2_vbo_written = 0, _m2_ibo_written = 0;   // finding 105: append-only arena
    VkBuffer _m2_ibo = VK_NULL_HANDLE;      VkDeviceMemory _m2_ibo_mem = VK_NULL_HANDLE;  std::size_t _m2_ibo_cap = 0;
    VkBuffer _m2_inst_tf = VK_NULL_HANDLE;  VkDeviceMemory _m2_inst_tf_mem = VK_NULL_HANDLE; std::size_t _m2_inst_tf_cap = 0;
    VkBuffer _m2_inst_in = VK_NULL_HANDLE;  VkDeviceMemory _m2_inst_in_mem = VK_NULL_HANDLE; std::size_t _m2_inst_in_cap = 0;
    VkBuffer _m2_inst_tx = VK_NULL_HANDLE;  VkDeviceMemory _m2_inst_tx_mem = VK_NULL_HANDLE; std::size_t _m2_inst_tx_cap = 0;
    VkBuffer _m2_inst_st = VK_NULL_HANDLE;  VkDeviceMemory _m2_inst_st_mem = VK_NULL_HANDLE; std::size_t _m2_inst_st_cap = 0;
    VkBuffer _m2_bones = VK_NULL_HANDLE;
    // What the bone SSBO descriptor currently points at. The buffer is grow-only, so this
    // only changes on a growth frame -- rewriting the descriptor otherwise cost a full stall.
    VkBuffer _m2_bones_bound = VK_NULL_HANDLE;    VkDeviceMemory _m2_bones_mem = VK_NULL_HANDLE;   std::size_t _m2_bones_cap = 0;
    VkBuffer _m2_indirect = VK_NULL_HANDLE; VkDeviceMemory _m2_indirect_mem = VK_NULL_HANDLE; std::size_t _m2_indirect_cap = 0;
    VkPipelineLayout _m2_layout = VK_NULL_HANDLE;
    VkPipeline _m2_pipeline = VK_NULL_HANDLE;   // == _m2_pipelines[0] (opaque, no cull)
    // [blend class][cull]: class 0 = no blend, 1 = SRC_ALPHA/1-SRC_ALPHA, 2 = ONE/ONE
    VkPipeline _m2_pipelines[6]{};

    // WMO pass: same 6-variant (blend class, cull) scheme as the M2 batches
    VkPipeline _wmo_pipeline = VK_NULL_HANDLE;   // == _wmo_pipelines[0]
    VkPipeline _wmo_pipelines[6]{};
    // blend_mode 0 only (true opaque): same state as _wmo_pipelines[0..1] but built with
    // ALPHA_TEST=0, so the fragment shader has no `discard` and early-Z stays enabled.
    // blend_mode 1 is alpha-KEY -- it shares blend class 0 but genuinely needs the discard,
    // which is why this is keyed on blend_mode and not on the class.
    VkPipeline _wmo_pipelines_earlyz[2]{};
    VkPipelineLayout _wmo_layout = VK_NULL_HANDLE;
    VkBuffer _wmo_vbo = VK_NULL_HANDLE;      VkDeviceMemory _wmo_vbo_mem = VK_NULL_HANDLE;      std::size_t _wmo_vbo_cap = 0;
    std::size_t _wmo_vbo_written = 0, _wmo_ibo_written = 0;   // finding 103: append-only arena
    VkBuffer _wmo_ibo = VK_NULL_HANDLE;      VkDeviceMemory _wmo_ibo_mem = VK_NULL_HANDLE;      std::size_t _wmo_ibo_cap = 0;
    VkBuffer _wmo_xforms = VK_NULL_HANDLE;   VkDeviceMemory _wmo_xforms_mem = VK_NULL_HANDLE;   std::size_t _wmo_xforms_cap = 0;
    VkBuffer _wmo_batches = VK_NULL_HANDLE;  VkDeviceMemory _wmo_batches_mem = VK_NULL_HANDLE;  std::size_t _wmo_batches_cap = 0;
    VkBuffer _wmo_amb = VK_NULL_HANDLE;      VkDeviceMemory _wmo_amb_mem = VK_NULL_HANDLE;      std::size_t _wmo_amb_cap = 0;
    std::uint32_t _wmo_index_count = 0;
    std::vector<WmoDraw> _wmo_draws;
    VkDescriptorSetLayout _wmo_dsl = VK_NULL_HANDLE;
    VkDescriptorPool _wmo_pool = VK_NULL_HANDLE;
    VkDescriptorSet _wmo_dset = VK_NULL_HANDLE;
    // Interior tone knobs mirrored from WMORender (NOGGIT_335A_INTERIOR_GAIN / _FLOOR): gain
    // brightens the whole interior, the floor guarantees no interior face renders pure black.
    float _wmo_interior_gain = 1.30f;
    float _wmo_interior_floor = 0.10f;
    // mirrors GL's NOGGIT_WMO_DEBUG (1 tex, 2 tex2, 3 env, 4 light-only, 5 MOCV)
    int _wmo_debug_mode = 0;
    bool createWmoPipeline();
    std::vector<std::int32_t> _m2_groups;   // flat (blend_mode, cull, first_cmd, cmd_count)
    float _m2_slice_dist = 0.f;             // per-pixel object cull distance (0 = off)
    // vkCmdDrawIndexedIndirect needs these ENABLED: drawCount > 1 requires multiDrawIndirect,
    // and a non-zero firstInstance requires drawIndirectFirstInstance. Without them the M2
    // batches collapsed onto instance 0 and only the first command of each group drew.
    bool _multi_draw_indirect = false;
    bool _draw_indirect_first_instance = false;
    std::uint32_t _m2_draw_count = 0;
    std::uint32_t _m2_index_count = 0;
    // replace a host-visible buffer's contents, growing (and re-creating) it when the data does not fit
    bool writeHostBuffer(VkBufferUsageFlags usage, void const* data, std::size_t bytes,
                         VkBuffer& buf, VkDeviceMemory& mem, std::size_t& cap);
    // [STABLE SLOTS -- geometry] Same, but copying only the given byte ranges of `data` instead of
    // all of it. The terrain streams live in a fixed slot space (~145 MB); on a tile crossing only
    // the handful of tiles that actually changed need to move, one contiguous range each.
    // A buffer that had to grow is filled completely, since its contents are otherwise undefined.
    bool writeHostBufferRanges(VkBufferUsageFlags usage, void const* data, std::size_t total_bytes,
                               std::pair<std::size_t, std::size_t> const* ranges, std::size_t range_count,
                               VkBuffer& buf, VkDeviceMemory& mem, std::size_t& cap);
    bool uploadImage2D(std::uint32_t width, std::uint32_t height, VkFormat format, std::uint32_t bytes_per_pixel,
                       std::vector<std::vector<std::uint8_t>> const& mips, VkImage& image, VkDeviceMemory& mem,
                       VkImageView& view);
    bool _descriptor_indexing = false;
    static constexpr std::uint32_t kMaxTextures = 2048;
    VkSampler _tile_sampler = VK_NULL_HANDLE;      // repeat + trilinear (tilesets)
    VkSampler _atlas_sampler = VK_NULL_HANDLE;     // clamp + linear, no mips (alpha/shadow atlases)
    VkDescriptorSetLayout _tt_dsl = VK_NULL_HANDLE;
    VkDescriptorPool _tt_dpool = VK_NULL_HANDLE;
    VkDescriptorSet _tt_dset = VK_NULL_HANDLE;
    struct TexEntry { VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
                      std::uint32_t w = 0, h = 0, levels = 0; };
    std::vector<TexEntry> _textures;
    VkImage _alpha_image = VK_NULL_HANDLE;  VkDeviceMemory _alpha_mem = VK_NULL_HANDLE;  VkImageView _alpha_view = VK_NULL_HANDLE;
    VkImage _shadow_image = VK_NULL_HANDLE; VkDeviceMemory _shadow_mem = VK_NULL_HANDLE; VkImageView _shadow_view = VK_NULL_HANDLE;
    VkBuffer _chunk_ssbo = VK_NULL_HANDLE;  VkDeviceMemory _chunk_ssbo_mem = VK_NULL_HANDLE;
    VkBuffer _light_ubo = VK_NULL_HANDLE;   VkDeviceMemory _light_ubo_mem = VK_NULL_HANDLE; void* _light_ubo_map = nullptr;
    VkBuffer _tt_cidx = VK_NULL_HANDLE;     VkDeviceMemory _tt_cidx_mem = VK_NULL_HANDLE;   // per-vertex chunk index stream
    std::size_t _tt_cidx_cap = 0;           // grow-only capacity: cidx is slot-addressed and persistent
    VkPipelineLayout _tt_layout = VK_NULL_HANDLE;
    VkPipeline _tt_pipeline = VK_NULL_HANDLE;
    bool _tt_ready = false;
    std::uint32_t _tt_atlas_w = 0, _tt_atlas_h = 0, _tt_chunk_count = 0;
    float _tt_params[4] = { 0.f, 0.f, 0.f, 0.f }; // x = draw_shadows, y = draw_fog

    // [overnight stage 4] doodads: instanced pipeline + geometry/instance buffers + per-model draw ranges
    bool createDoodadPipeline();
    VkPipeline _doodad_pipeline = VK_NULL_HANDLE;
    VkBuffer _doodad_vbo = VK_NULL_HANDLE;
    VkDeviceMemory _doodad_vbo_mem = VK_NULL_HANDLE;
    VkBuffer _doodad_ibo = VK_NULL_HANDLE;
    VkDeviceMemory _doodad_ibo_mem = VK_NULL_HANDLE;
    VkBuffer _doodad_inst = VK_NULL_HANDLE;
    VkDeviceMemory _doodad_inst_mem = VK_NULL_HANDLE;
    // finding 104: persistent doodad buffers -- no destroy/create, no queue drain per rebuild
    std::size_t _doodad_vbo_cap = 0, _doodad_ibo_cap = 0, _doodad_inst_cap = 0;
    std::size_t _doodad_vbo_written = 0, _doodad_ibo_written = 0;
    std::vector<DoodadDraw> _doodad_draws;

    // [overnight stage 3] water: terrain-pipeline clone with blending on / depth-write off + its own mesh
    VkPipeline _water_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout _water_layout = VK_NULL_HANDLE;   // {_dsl, _tt_dsl}: liquid needs set 1
    std::size_t _water_vbo_cap = 0;
    std::size_t _water_ibo_cap = 0;
    VkBuffer _wliq_vbo = VK_NULL_HANDLE;
    VkDeviceMemory _wliq_vbo_mem = VK_NULL_HANDLE;
    VkBuffer _wliq_ibo = VK_NULL_HANDLE;
    VkDeviceMemory _wliq_ibo_mem = VK_NULL_HANDLE;
    std::size_t _wliq_vbo_cap = 0;
    std::size_t _wliq_ibo_cap = 0;
    VkBuffer _terrain_indirect = VK_NULL_HANDLE;
    VkDeviceMemory _terrain_indirect_mem = VK_NULL_HANDLE;
    std::size_t _terrain_indirect_cap = 0;
    std::uint32_t _terrain_visible_chunks = 0;
    VkBuffer _water_indirect = VK_NULL_HANDLE;
    VkDeviceMemory _water_indirect_mem = VK_NULL_HANDLE;
    std::size_t _water_indirect_cap = 0;
    std::uint32_t _water_visible_chunks = 0;

    // [phase I] PIPELINE CACHE -- the closest real analogue to the client's
    // GxCompatAsyncShaderCompilation. noggit has no runtime shader compilation to make asynchronous
    // (every pipeline is created during init), so the equivalent win is not paying the driver's
    // compile cost again on the next launch: the cache is seeded from disk and written back.
    VkPipelineCache _pipeline_cache = VK_NULL_HANDLE;
    std::string _pipeline_cache_path;
    // [phase I] COPY QUEUE (the client's GxCompatCopyQueue). Uploads used to record into _cmd -- the
    // FRAME's own primary command buffer -- and wait on _fence, the frame's own fence. That both
    // stalled the frame and shared state with it. Transfers now have their own queue (a second one
    // from the graphics family when the device offers it), pool, buffer and fence.
    VkQueue _copy_queue = VK_NULL_HANDLE;
    VkCommandPool _copy_pool = VK_NULL_HANDLE;
    VkCommandBuffer _copy_cmd = VK_NULL_HANDLE;
    VkFence _copy_fence = VK_NULL_HANDLE;
    bool createCopyQueue();

    void createPipelineCache();
    void savePipelineCache();
    std::uint32_t _wliq_index_count = 0;
    VkBuffer _water_vbo = VK_NULL_HANDLE;
    VkDeviceMemory _water_vbo_mem = VK_NULL_HANDLE;
    VkBuffer _water_ibo = VK_NULL_HANDLE;
    VkDeviceMemory _water_ibo_mem = VK_NULL_HANDLE;
    std::uint32_t _water_index_count = 0;

    // --- dynamically loaded entry points (driver's vulkan-1.dll; VK_NO_PROTOTYPES) ---
    PFN_vkGetInstanceProcAddr _vkGetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance vkCreateInstance = nullptr;
    PFN_vkDestroyInstance vkDestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateDevice vkCreateDevice = nullptr;
    PFN_vkDestroyDevice vkDestroyDevice = nullptr;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
    PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
    PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit = nullptr;
    PFN_vkCreateFence vkCreateFence = nullptr;
    PFN_vkDestroyFence vkDestroyFence = nullptr;
    PFN_vkWaitForFences vkWaitForFences = nullptr;
    PFN_vkResetFences vkResetFences = nullptr;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
    PFN_vkCreateImage vkCreateImage = nullptr;
    PFN_vkDestroyImage vkDestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
    PFN_vkBindImageMemory vkBindImageMemory = nullptr;
    PFN_vkVoidFunction _pfn_vkGetMemoryWin32HandleKHR = nullptr;    // win32 PFNs type-erased (cast in .cpp)
    PFN_vkCreateSemaphore vkCreateSemaphore = nullptr;
    PFN_vkDestroySemaphore vkDestroySemaphore = nullptr;
    PFN_vkVoidFunction _pfn_vkGetSemaphoreWin32HandleKHR = nullptr;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
    PFN_vkCmdClearColorImage vkCmdClearColorImage = nullptr;
    PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
    PFN_vkCreateImageView vkCreateImageView = nullptr;
    PFN_vkDestroyImageView vkDestroyImageView = nullptr;
    PFN_vkCreateRenderPass vkCreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass vkDestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer vkCreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer vkDestroyFramebuffer = nullptr;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
    PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass vkCmdEndRenderPass = nullptr;
    // [GPU timing] The port has never measured its own GPU time. gpuWait read ~0.035 ms only because
    // the submit thread overlapped it with GL's traversal, and the inline-submit A/B also waits on the
    // GL-done semaphore, so it measures serialisation, not GPU work. Timestamps settle it.
    PFN_vkCreateQueryPool vkCreateQueryPool = nullptr;
    PFN_vkDestroyQueryPool vkDestroyQueryPool = nullptr;
    PFN_vkCmdResetQueryPool vkCmdResetQueryPool = nullptr;
    PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp = nullptr;
    PFN_vkGetQueryPoolResults vkGetQueryPoolResults = nullptr;
    VkQueryPool _ts_pool = VK_NULL_HANDLE;
    float _ts_period_ns = 0.0f;
    double _stat_gpu_ms = 0.0;
    PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
    PFN_vkCmdPushConstants vkCmdPushConstants = nullptr;
    PFN_vkCmdDraw vkCmdDraw = nullptr;
    PFN_vkCreateBuffer vkCreateBuffer = nullptr;
    PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
    PFN_vkMapMemory vkMapMemory = nullptr;
    PFN_vkUnmapMemory vkUnmapMemory = nullptr;
    PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers = nullptr;
    PFN_vkCmdBindIndexBuffer vkCmdBindIndexBuffer = nullptr;
    PFN_vkCmdDrawIndexed vkCmdDrawIndexed = nullptr;
    PFN_vkCmdExecuteCommands vkCmdExecuteCommands = nullptr;   // [phase H] secondary buffers
    PFN_vkResetCommandPool vkResetCommandPool = nullptr;       // [phase H] per-frame pool reset
    PFN_vkCreatePipelineCache vkCreatePipelineCache = nullptr;   // [phase I] async-resource analogue
    PFN_vkGetPipelineCacheData vkGetPipelineCacheData = nullptr;
    PFN_vkDestroyPipelineCache vkDestroyPipelineCache = nullptr;
    PFN_vkCmdDrawIndexedIndirect vkCmdDrawIndexedIndirect = nullptr; // [phase C] M2 batches
    PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;
    PFN_vkCreateSampler vkCreateSampler = nullptr;
    PFN_vkDestroySampler vkDestroySampler = nullptr;
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
    PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = nullptr;
  };
}

#endif // _WIN32
