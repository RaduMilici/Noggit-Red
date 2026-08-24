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

    // Win32 HANDLEs for the GL import (opaque win32; ownership stays with VK).
    [[nodiscard]] void* imageMemoryHandle() const { return _image_mem_handle; }
    [[nodiscard]] std::uint64_t imageMemorySize() const { return _image_mem_size; }
    [[nodiscard]] void* depthMemoryHandle() const { return _depth_mem_handle; }   // null when export failed
    [[nodiscard]] std::uint64_t depthMemorySize() const { return _depth_mem_size; }
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
    bool createHostBuffer(VkBufferUsageFlags usage, std::size_t bytes,
                          VkBuffer& buf, VkDeviceMemory& mem, void const* data); // host-visible one-shot

    bool _ready = false;
    void* _dll = nullptr; // HMODULE

    VkInstance _instance = VK_NULL_HANDLE;
    VkPhysicalDevice _phys = VK_NULL_HANDLE;
    std::uint32_t _queue_family = 0;
    VkDevice _device = VK_NULL_HANDLE;
    VkQueue _queue = VK_NULL_HANDLE;
    VkCommandPool _pool = VK_NULL_HANDLE;
    VkCommandBuffer _cmd = VK_NULL_HANDLE;
    VkFence _fence = VK_NULL_HANDLE;

    std::uint32_t _width = 0, _height = 0;
    VkImage _image = VK_NULL_HANDLE;
    VkDeviceMemory _image_mem = VK_NULL_HANDLE;
    std::uint64_t _image_mem_size = 0;
    void* _image_mem_handle = nullptr;
    bool _image_initialized = false; // first barrier comes from UNDEFINED

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
    std::uint32_t _terrain_index_count = 0;

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

    // [overnight stage 4] doodads: instanced pipeline + geometry/instance buffers + per-model draw ranges
    bool createDoodadPipeline();
    VkPipeline _doodad_pipeline = VK_NULL_HANDLE;
    VkBuffer _doodad_vbo = VK_NULL_HANDLE;
    VkDeviceMemory _doodad_vbo_mem = VK_NULL_HANDLE;
    VkBuffer _doodad_ibo = VK_NULL_HANDLE;
    VkDeviceMemory _doodad_ibo_mem = VK_NULL_HANDLE;
    VkBuffer _doodad_inst = VK_NULL_HANDLE;
    VkDeviceMemory _doodad_inst_mem = VK_NULL_HANDLE;
    std::vector<DoodadDraw> _doodad_draws;

    // [overnight stage 3] water: terrain-pipeline clone with blending on / depth-write off + its own mesh
    VkPipeline _water_pipeline = VK_NULL_HANDLE;
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
