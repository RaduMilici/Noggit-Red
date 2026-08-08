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

#include <cstdint>

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

    // Phase-0 frame: clear the shared image to an animated colour, signal vk_done. wait_gl_done: pass false
    // on the very first frame (GL hasn't signalled yet), true afterwards.
    // Returns false on submission failure (backend goes inert).
    bool renderTestFrame(float time_seconds, bool wait_gl_done);

    // Win32 HANDLEs for the GL import (opaque win32; ownership stays with VK).
    [[nodiscard]] void* imageMemoryHandle() const { return _image_mem_handle; }
    [[nodiscard]] std::uint64_t imageMemorySize() const { return _image_mem_size; }
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
  };
}

#endif // _WIN32
