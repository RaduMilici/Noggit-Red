// This file is part of Noggit3, licensed under GNU General Public License (version 3).
// [VULKAN PHASE 0] See VulkanBackend.hpp for the architecture. Everything here fails SOFT: any error logs
// once and leaves the backend inert (ready()==false) so the GL renderer is never disturbed.
#ifdef _WIN32

#include <fstream>
#include <algorithm>   // [VKFLASH] std::sort for the median
#include <atomic>
#include <noggit/rendering/vulkan/VulkanBackend.hpp>
#include <noggit/Log.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan_win32.h> // win32 platform structs/PFN types (header keeps core-only; see hpp note)

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// [VKTRACE 2026-09-02] frame counter and the frame of the last terrain atlas rebuild.
static std::uint64_t g_vk_trace_frame = 0;
static std::uint64_t g_vk_trace_last_rebuild_frame = 0;

// [VKFLASH] Ring of recent frame state + collapse detector. See the note in scratchpad/vkflash.py.
struct VkTraceRow
{
  std::uint64_t f = 0;
  std::uint32_t vis = 0, m2 = 0, wmo = 0, widx = 0;
  float cam[3] = { 0.f, 0.f, 0.f };
  int mvp = 0;
  std::uint64_t rebuild = 0;
};
static VkTraceRow g_vk_ring[48];
static std::size_t g_vk_ring_n = 0;
static std::uint64_t g_vk_flash_last = 0;

static std::uint32_t vkTraceMedian(std::uint32_t VkTraceRow::*field)
{
  std::uint32_t v[48];
  std::size_t n = g_vk_ring_n < 48 ? g_vk_ring_n : 48;
  if (n < 16) { return 0; }
  for (std::size_t i = 0; i < n; ++i) { v[i] = g_vk_ring[i].*field; }
  std::sort(v, v + n);
  return v[n / 2];
}

namespace Noggit::Rendering::VK
{
  namespace
  {
    char const* vkres(VkResult r)
    {
      switch (r)
      {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "FEATURE_NOT_PRESENT";
        default: return "VK_ERROR_<other>";
      }
    }
  }

  // [spike hunt] Name the call that eats a frame. Batching the texture uploads only moved lost-ms
  // 8150 -> 7162 with p99 still ~470 ms, so the dominant stall is somewhere else entirely. Rather
  // than guess again, every heavy backend entry point reports itself when it exceeds the threshold.
  namespace
  {
    struct SlowCall
    {
      char const* name;
      std::chrono::steady_clock::time_point t0;
      explicit SlowCall(char const* n) : name(n), t0(std::chrono::steady_clock::now()) {}
      ~SlowCall()
      {
        double const ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();
        if (ms > 30.0)  // diagnosis used 8-10; restored so normal editing does not spam the log
          LogError << "[VK-SLOW] " << name << " = " << ms << " ms" << std::endl;
      }
    };
  }

  bool VulkanBackend::loadLoader()
  {
    _dll = ::LoadLibraryA("vulkan-1.dll");
    if (!_dll)
    {
      LogError << "[VK] vulkan-1.dll not found -- Vulkan backend disabled" << std::endl;
      return false;
    }
    _vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        ::GetProcAddress(static_cast<HMODULE>(_dll), "vkGetInstanceProcAddr"));
    if (!_vkGetInstanceProcAddr)
    {
      LogError << "[VK] vkGetInstanceProcAddr missing" << std::endl;
      return false;
    }
    vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(_vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    return vkCreateInstance != nullptr;
  }

  bool VulkanBackend::createInstance()
  {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "noggit-red";
    app.apiVersion = VK_MAKE_VERSION(1, 1, 0); // 1.1: external memory/semaphore capabilities are core

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    // [NATIVE PRESENT, 2026-09-03] surface extensions so the backend can own a swapchain and
    // present without GL. Universal on Windows drivers; falls back to a surfaceless instance
    // (compose mode) if a driver ever lacks them.
    char const* surf_exts[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = surf_exts;

    VkResult r = vkCreateInstance(&ci, nullptr, &_instance);
    _present_capable = (r == VK_SUCCESS);
    if (r == VK_ERROR_EXTENSION_NOT_PRESENT)
    {
      LogError << "[VK] VK_KHR_win32_surface unavailable -- native presentation disabled" << std::endl;
      ci.enabledExtensionCount = 0;
      ci.ppEnabledExtensionNames = nullptr;
      r = vkCreateInstance(&ci, nullptr, &_instance);
    }
    if (r != VK_SUCCESS)
    {
      LogError << "[VK] vkCreateInstance failed: " << vkres(r) << std::endl;
      return false;
    }

    auto ld = [&](char const* n) { return _vkGetInstanceProcAddr(_instance, n); };
    vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(ld("vkDestroyInstance"));
    vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(ld("vkEnumeratePhysicalDevices"));
    vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(ld("vkGetPhysicalDeviceProperties"));
    vkGetPhysicalDeviceFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(ld("vkGetPhysicalDeviceFeatures"));
    vkGetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(ld("vkGetPhysicalDeviceQueueFamilyProperties"));
    vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(ld("vkGetPhysicalDeviceMemoryProperties"));
    vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(ld("vkCreateDevice"));
    vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(ld("vkGetDeviceProcAddr"));
    // [NATIVE PRESENT] surface entry points (instance level)
    if (_present_capable)
    {
      _pfn_vkCreateWin32SurfaceKHR = ld("vkCreateWin32SurfaceKHR");
      vkDestroySurfaceKHR = reinterpret_cast<PFN_vkDestroySurfaceKHR>(ld("vkDestroySurfaceKHR"));
      vkGetPhysicalDeviceSurfaceSupportKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(ld("vkGetPhysicalDeviceSurfaceSupportKHR"));
      vkGetPhysicalDeviceSurfaceCapabilitiesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(ld("vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
      vkGetPhysicalDeviceSurfaceFormatsKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(ld("vkGetPhysicalDeviceSurfaceFormatsKHR"));
      vkGetPhysicalDeviceSurfacePresentModesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(ld("vkGetPhysicalDeviceSurfacePresentModesKHR"));
      if (!_pfn_vkCreateWin32SurfaceKHR || !vkDestroySurfaceKHR
          || !vkGetPhysicalDeviceSurfaceSupportKHR || !vkGetPhysicalDeviceSurfaceCapabilitiesKHR
          || !vkGetPhysicalDeviceSurfaceFormatsKHR || !vkGetPhysicalDeviceSurfacePresentModesKHR)
      {
        LogError << "[VK] surface entry points missing -- native presentation disabled" << std::endl;
        _present_capable = false;
      }
    }
    return vkEnumeratePhysicalDevices && vkCreateDevice && vkGetDeviceProcAddr;
  }

  bool VulkanBackend::pickDeviceAndQueue()
  {
    std::uint32_t n = 0;
    vkEnumeratePhysicalDevices(_instance, &n, nullptr);
    if (!n)
    {
      LogError << "[VK] no physical devices" << std::endl;
      return false;
    }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(_instance, &n, devs.data());

    // prefer a DISCRETE gpu (the 3090), else first
    _phys = devs[0];
    for (auto d : devs)
    {
      VkPhysicalDeviceProperties p{};
      vkGetPhysicalDeviceProperties(d, &p);
      if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
      {
        _phys = d;
        break;
      }
    }
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(_phys, &p);
    LogError << "[VK] device: " << p.deviceName << " api "
             << VK_VERSION_MAJOR(p.apiVersion) << "." << VK_VERSION_MINOR(p.apiVersion) << std::endl;

    std::uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(_phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(_phys, &qn, qf.data());
    for (std::uint32_t i = 0; i < qn; ++i)
    {
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
      {
        _queue_family = i;
        _queue_family_count = qf[i].queueCount;
        return true;
      }
    }
    LogError << "[VK] no graphics queue family" << std::endl;
    return false;
  }

  bool VulkanBackend::createDevice()
  {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = _queue_family;
    // Two queues when the family allows it: the second is the COPY queue, so uploads never queue up
    // behind (or share a fence with) the frame's own submission.
    float const prios[2] = { 1.0f, 0.9f };
    std::uint32_t const wanted_queues = (_queue_family_count > 1) ? 2u : 1u;
    qci.queueCount = wanted_queues;
    qci.pQueuePriorities = (wanted_queues > 1) ? prios : &prio;

    // [NATIVE PRESENT] VK_KHR_swapchain joins the list when the instance has surface support.
    // The retry ladder below keeps descriptor indexing and the swapchain INDEPENDENT: losing one
    // must never cost the other.
    std::vector<char const*> exts = {
      "VK_KHR_external_memory",
      "VK_KHR_external_memory_win32",
      "VK_KHR_external_semaphore",
      "VK_KHR_external_semaphore_win32",
      "VK_KHR_dedicated_allocation",
      "VK_KHR_get_memory_requirements2",
      // [phase B] descriptor indexing = one sampler2D[] for every tileset (bindless terrain)
      "VK_KHR_maintenance3",
      "VK_EXT_descriptor_indexing",
    };
    if (_present_capable)
      exts.push_back("VK_KHR_swapchain");
    auto const drop_ext = [&](char const* name)
    {
      for (std::size_t i = 0; i < exts.size(); ++i)
        if (std::strcmp(exts[i], name) == 0) { exts.erase(exts.begin() + i); return; }
    };
    VkPhysicalDeviceDescriptorIndexingFeaturesEXT dif{};
    dif.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
    dif.runtimeDescriptorArray = VK_TRUE;
    dif.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    dif.descriptorBindingPartiallyBound = VK_TRUE;
    dif.descriptorBindingVariableDescriptorCount = VK_TRUE;
    // [spike fix] Without UPDATE_AFTER_BIND, writing a new texture into the bindless array while any
    // command buffer that uses the set is in flight is illegal -- which is why every addTexture had
    // to vkQueueWaitIdle (a FULL GPU stall) first. Texture streaming therefore stalled the GPU on
    // every new texture, which is what produces the periodic hitching while flying. With this the
    // descriptor can be written safely and the stall is unnecessary.
    dif.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    dif.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;

    VkPhysicalDeviceFeatures feats{};
    feats.samplerAnisotropy = VK_TRUE; // tilesets: GL samples with 16x AF (render/anisotropic_filtering)
    // INDIRECT DRAW features -- the M2/doodad batches are useless without them (see header note).
    VkPhysicalDeviceFeatures avail{};
    if (vkGetPhysicalDeviceFeatures)
      vkGetPhysicalDeviceFeatures(_phys, &avail);
    feats.multiDrawIndirect = avail.multiDrawIndirect;
    feats.drawIndirectFirstInstance = avail.drawIndirectFirstInstance;
    _multi_draw_indirect = avail.multiDrawIndirect == VK_TRUE;
    _draw_indirect_first_instance = avail.drawIndirectFirstInstance == VK_TRUE;
    LogError << "[VK] indirect features: multiDrawIndirect=" << _multi_draw_indirect
             << " drawIndirectFirstInstance=" << _draw_indirect_first_instance << std::endl;
    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext = &dif;
    ci.pEnabledFeatures = &feats;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();

    VkResult r = vkCreateDevice(_phys, &ci, nullptr, &_device);
    if (r != VK_SUCCESS && _present_capable)
    {
      // EXTENSION_NOT_PRESENT does not say WHICH extension. Before blaming descriptor indexing,
      // try the same config without the swapchain -- so a hypothetical no-present device still
      // gets its bindless terrain.
      std::vector<char const*> no_sc(exts.begin(), exts.end() - 1); // swapchain was pushed last
      ci.enabledExtensionCount = static_cast<std::uint32_t>(no_sc.size());
      ci.ppEnabledExtensionNames = no_sc.data();
      VkResult const r2 = vkCreateDevice(_phys, &ci, nullptr, &_device);
      if (r2 == VK_SUCCESS)
      {
        LogError << "[VK] VK_KHR_swapchain unavailable -- native presentation disabled" << std::endl;
        _present_capable = false;
        exts = no_sc;
        r = r2;
      }
      else
      {
        ci.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
      }
    }
    _descriptor_indexing = (r == VK_SUCCESS);
    if (r != VK_SUCCESS)
    {
      // fall back to the phase-0 device (no bindless terrain; clay path only)
      LogError << "[VK] descriptor indexing unavailable (" << vkres(r) << ") -- textured terrain disabled" << std::endl;
      ci.pNext = nullptr;
      drop_ext("VK_KHR_maintenance3");
      drop_ext("VK_EXT_descriptor_indexing");
      ci.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
      ci.ppEnabledExtensionNames = exts.data();
      r = vkCreateDevice(_phys, &ci, nullptr, &_device);
      if (r != VK_SUCCESS && _present_capable)
      {
        LogError << "[VK] still failing (" << vkres(r) << ") -- retrying without VK_KHR_swapchain too" << std::endl;
        _present_capable = false;
        drop_ext("VK_KHR_swapchain");
        ci.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
        r = vkCreateDevice(_phys, &ci, nullptr, &_device);
      }
    }
    if (r != VK_SUCCESS)
    {
      LogError << "[VK] vkCreateDevice failed: " << vkres(r) << " (external-memory extensions unsupported?)" << std::endl;
      return false;
    }

    auto ld = [&](char const* n) { return vkGetDeviceProcAddr(_device, n); };
    vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(ld("vkDestroyDevice"));
    vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(ld("vkGetDeviceQueue"));
    vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(ld("vkCreateCommandPool"));
    vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(ld("vkDestroyCommandPool"));
    vkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(ld("vkAllocateCommandBuffers"));
    vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(ld("vkBeginCommandBuffer"));
    vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(ld("vkEndCommandBuffer"));
    vkResetCommandBuffer = reinterpret_cast<PFN_vkResetCommandBuffer>(ld("vkResetCommandBuffer"));
    vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(ld("vkQueueSubmit"));
    vkCreateFence = reinterpret_cast<PFN_vkCreateFence>(ld("vkCreateFence"));
    vkDestroyFence = reinterpret_cast<PFN_vkDestroyFence>(ld("vkDestroyFence"));
    vkWaitForFences = reinterpret_cast<PFN_vkWaitForFences>(ld("vkWaitForFences"));
    vkResetFences = reinterpret_cast<PFN_vkResetFences>(ld("vkResetFences"));
    vkDeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(ld("vkDeviceWaitIdle"));
    vkCreateImage = reinterpret_cast<PFN_vkCreateImage>(ld("vkCreateImage"));
    vkDestroyImage = reinterpret_cast<PFN_vkDestroyImage>(ld("vkDestroyImage"));
    vkGetImageMemoryRequirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(ld("vkGetImageMemoryRequirements"));
    vkAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(ld("vkAllocateMemory"));
    vkFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(ld("vkFreeMemory"));
    vkBindImageMemory = reinterpret_cast<PFN_vkBindImageMemory>(ld("vkBindImageMemory"));
    _pfn_vkGetMemoryWin32HandleKHR = ld("vkGetMemoryWin32HandleKHR");
    vkCreateSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(ld("vkCreateSemaphore"));
    vkDestroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(ld("vkDestroySemaphore"));
    _pfn_vkGetSemaphoreWin32HandleKHR = ld("vkGetSemaphoreWin32HandleKHR");
    vkCmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(ld("vkCmdPipelineBarrier"));
    vkCmdClearColorImage = reinterpret_cast<PFN_vkCmdClearColorImage>(ld("vkCmdClearColorImage"));
    vkCreateShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(ld("vkCreateShaderModule"));
    vkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(ld("vkDestroyShaderModule"));
    vkCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(ld("vkCreateImageView"));
    vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(ld("vkDestroyImageView"));
    vkCreateRenderPass = reinterpret_cast<PFN_vkCreateRenderPass>(ld("vkCreateRenderPass"));
    vkDestroyRenderPass = reinterpret_cast<PFN_vkDestroyRenderPass>(ld("vkDestroyRenderPass"));
    vkCreateFramebuffer = reinterpret_cast<PFN_vkCreateFramebuffer>(ld("vkCreateFramebuffer"));
    vkDestroyFramebuffer = reinterpret_cast<PFN_vkDestroyFramebuffer>(ld("vkDestroyFramebuffer"));
    vkCreatePipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(ld("vkCreatePipelineLayout"));
    vkDestroyPipelineLayout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(ld("vkDestroyPipelineLayout"));
    vkCreateGraphicsPipelines = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(ld("vkCreateGraphicsPipelines"));
    vkDestroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(ld("vkDestroyPipeline"));
    vkCmdBeginRenderPass = reinterpret_cast<PFN_vkCmdBeginRenderPass>(ld("vkCmdBeginRenderPass"));
    vkCmdEndRenderPass = reinterpret_cast<PFN_vkCmdEndRenderPass>(ld("vkCmdEndRenderPass"));
    vkCreateQueryPool = reinterpret_cast<PFN_vkCreateQueryPool>(ld("vkCreateQueryPool"));
    vkDestroyQueryPool = reinterpret_cast<PFN_vkDestroyQueryPool>(ld("vkDestroyQueryPool"));
    vkCmdResetQueryPool = reinterpret_cast<PFN_vkCmdResetQueryPool>(ld("vkCmdResetQueryPool"));
    vkCmdWriteTimestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(ld("vkCmdWriteTimestamp"));
    vkGetQueryPoolResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(ld("vkGetQueryPoolResults"));
    vkCmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(ld("vkCmdBindPipeline"));
    vkCmdPushConstants = reinterpret_cast<PFN_vkCmdPushConstants>(ld("vkCmdPushConstants"));
    vkCmdDraw = reinterpret_cast<PFN_vkCmdDraw>(ld("vkCmdDraw"));
    vkCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(ld("vkCreateBuffer"));
    vkDestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(ld("vkDestroyBuffer"));
    vkGetBufferMemoryRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(ld("vkGetBufferMemoryRequirements"));
    vkBindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(ld("vkBindBufferMemory"));
    vkMapMemory = reinterpret_cast<PFN_vkMapMemory>(ld("vkMapMemory"));
    vkUnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(ld("vkUnmapMemory"));
    vkCmdBindVertexBuffers = reinterpret_cast<PFN_vkCmdBindVertexBuffers>(ld("vkCmdBindVertexBuffers"));
    vkCmdBindIndexBuffer = reinterpret_cast<PFN_vkCmdBindIndexBuffer>(ld("vkCmdBindIndexBuffer"));
    vkCmdDrawIndexed = reinterpret_cast<PFN_vkCmdDrawIndexed>(ld("vkCmdDrawIndexed"));
    vkCmdExecuteCommands = reinterpret_cast<PFN_vkCmdExecuteCommands>(ld("vkCmdExecuteCommands"));
    vkResetCommandPool = reinterpret_cast<PFN_vkResetCommandPool>(ld("vkResetCommandPool"));
    vkCreatePipelineCache = reinterpret_cast<PFN_vkCreatePipelineCache>(ld("vkCreatePipelineCache"));
    vkGetPipelineCacheData = reinterpret_cast<PFN_vkGetPipelineCacheData>(ld("vkGetPipelineCacheData"));
    vkDestroyPipelineCache = reinterpret_cast<PFN_vkDestroyPipelineCache>(ld("vkDestroyPipelineCache"));
    vkCmdDrawIndexedIndirect = reinterpret_cast<PFN_vkCmdDrawIndexedIndirect>(ld("vkCmdDrawIndexedIndirect"));
    vkQueueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(ld("vkQueueWaitIdle"));
    vkCreateSampler = reinterpret_cast<PFN_vkCreateSampler>(ld("vkCreateSampler"));
    vkDestroySampler = reinterpret_cast<PFN_vkDestroySampler>(ld("vkDestroySampler"));
    vkCreateDescriptorSetLayout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(ld("vkCreateDescriptorSetLayout"));
    vkDestroyDescriptorSetLayout = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(ld("vkDestroyDescriptorSetLayout"));
    vkCreateDescriptorPool = reinterpret_cast<PFN_vkCreateDescriptorPool>(ld("vkCreateDescriptorPool"));
    vkDestroyDescriptorPool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(ld("vkDestroyDescriptorPool"));
    vkAllocateDescriptorSets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(ld("vkAllocateDescriptorSets"));
    vkUpdateDescriptorSets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(ld("vkUpdateDescriptorSets"));
    vkCmdBindDescriptorSets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(ld("vkCmdBindDescriptorSets"));
    vkCmdCopyBufferToImage = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(ld("vkCmdCopyBufferToImage"));
    vkCmdBlitImage = reinterpret_cast<PFN_vkCmdBlitImage>(ld("vkCmdBlitImage"));
    vkCmdCopyImageToBuffer = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(ld("vkCmdCopyImageToBuffer"));
    vkFreeCommandBuffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(ld("vkFreeCommandBuffers"));
    // [NATIVE PRESENT] swapchain entry points (device level)
    if (_present_capable)
    {
      vkCreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(ld("vkCreateSwapchainKHR"));
      vkDestroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(ld("vkDestroySwapchainKHR"));
      vkGetSwapchainImagesKHR = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(ld("vkGetSwapchainImagesKHR"));
      vkAcquireNextImageKHR = reinterpret_cast<PFN_vkAcquireNextImageKHR>(ld("vkAcquireNextImageKHR"));
      vkQueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(ld("vkQueuePresentKHR"));
      if (!vkCreateSwapchainKHR || !vkDestroySwapchainKHR || !vkGetSwapchainImagesKHR
          || !vkAcquireNextImageKHR || !vkQueuePresentKHR || !vkCmdBlitImage)
      {
        LogError << "[VK] swapchain entry points missing -- native presentation disabled" << std::endl;
        _present_capable = false;
      }
    }

    if (!_pfn_vkGetMemoryWin32HandleKHR || !_pfn_vkGetSemaphoreWin32HandleKHR)
    {
      LogError << "[VK] win32 external handle entry points missing" << std::endl;
      return false;
    }

    vkGetDeviceQueue(_device, _queue_family, 0, &_queue);

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = _queue_family;
    if (vkCreateCommandPool(_device, &pci, nullptr, &_pool) != VK_SUCCESS)
      return false;

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = _pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(_device, &ai, &_cmd) != VK_SUCCESS)
      return false;

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return vkCreateFence(_device, &fci, nullptr, &_fence) == VK_SUCCESS;
  }

  // [phase A] one exportable OPTIMAL-tiled image + dedicated exported memory + win32 handle (colour route)
  bool VulkanBackend::createExportableImage(VkFormat format, VkImageUsageFlags usage,
                                            VkImage& image, VkDeviceMemory& mem, std::uint64_t& size, void*& handle)
  {
    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &ext;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = { _width, _height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(_device, &ici, nullptr, &image) != VK_SUCCESS)
    {
      LogError << "[VK] shared image create failed" << std::endl;
      return false;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(_device, image, &req);
    size = req.size;

    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(_phys, &mp);
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    {
      if ((req.memoryTypeBits & (1u << i))
          && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      {
        type = i;
        break;
      }
    }
    if (type == UINT32_MAX)
      return false;

    VkExportMemoryAllocateInfo exp{};
    exp.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkMemoryDedicatedAllocateInfo ded{};
    ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.pNext = &exp;
    ded.image = image;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &ded;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(_device, &mai, nullptr, &mem) != VK_SUCCESS)
    {
      LogError << "[VK] shared image memory alloc failed" << std::endl;
      return false;
    }
    if (vkBindImageMemory(_device, image, mem, 0) != VK_SUCCESS)
      return false;

    VkMemoryGetWin32HandleInfoKHR gh{};
    gh.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    gh.memory = mem;
    gh.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    HANDLE h = nullptr;
    if (reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(_pfn_vkGetMemoryWin32HandleKHR)(_device, &gh, &h) != VK_SUCCESS)
    {
      LogError << "[VK] vkGetMemoryWin32HandleKHR failed" << std::endl;
      return false;
    }
    handle = h;
    return true;
  }

  bool VulkanBackend::createSharedImage()
  {
    // [NATIVE PRESENT] TRANSFER_SRC: the per-frame present path blits this image into the swapchain.
    VkImageUsageFlags const usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                  | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!createExportableImage(VK_FORMAT_R8G8B8A8_UNORM, usage, _image, _image_mem, _image_mem_size, _image_mem_handle))
      return false;
    // depth-as-colour: R32F, written by every fragment shader (location 1), imported by GL as GL_R32F
    if (!createExportableImage(VK_FORMAT_R32_SFLOAT, usage, _z_image, _z_mem, _z_mem_size, _z_mem_handle))
    {
      LogError << "[VK] depth-as-colour image failed (compose route unavailable)" << std::endl;
      _z_image = VK_NULL_HANDLE; _z_mem_handle = nullptr;
    }
    return true;
  }

  bool VulkanBackend::createSemaphores()
  {
    VkExportSemaphoreCreateInfo exp{};
    exp.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exp.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &exp;

    if (vkCreateSemaphore(_device, &sci, nullptr, &_vk_done) != VK_SUCCESS
        || vkCreateSemaphore(_device, &sci, nullptr, &_gl_done) != VK_SUCCESS)
    {
      LogError << "[VK] exportable semaphore create failed" << std::endl;
      return false;
    }

    auto get = [&](VkSemaphore s, void** out) -> bool
    {
      VkSemaphoreGetWin32HandleInfoKHR gh{};
      gh.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
      gh.semaphore = s;
      gh.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
      HANDLE h = nullptr;
      if (reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(_pfn_vkGetSemaphoreWin32HandleKHR)(_device, &gh, &h) != VK_SUCCESS)
        return false;
      *out = h;
      return true;
    };
    return get(_vk_done, &_vk_done_handle) && get(_gl_done, &_gl_done_handle);
  }

  VkShaderModule VulkanBackend::loadShaderModule(char const* filename)
  {
    // <exe dir>/vk_shaders/<filename> -- compiled at build time by the vendored glslang (CMake POST_BUILD).
    char exe[MAX_PATH]{};
    ::GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string path(exe);
    auto const slash = path.find_last_of("\\/");
    path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "vk_shaders\\" + filename;

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
    {
      LogError << "[VK] shader not found: " << path << std::endl;
      return VK_NULL_HANDLE;
    }
    std::fseek(f, 0, SEEK_END);
    long const size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> data(static_cast<std::size_t>(size));
    std::size_t const rd = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (rd != data.size() || data.size() < 4)
    {
      LogError << "[VK] shader read failed: " << path << std::endl;
      return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = data.size();
    ci.pCode = reinterpret_cast<std::uint32_t const*>(data.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(_device, &ci, nullptr, &mod) != VK_SUCCESS)
    {
      LogError << "[VK] vkCreateShaderModule failed: " << path << std::endl;
      return VK_NULL_HANDLE;
    }
    return mod;
  }

  bool VulkanBackend::createDepthTarget()
  {
    // exportable like the colour image: the GL side imports it for the depth-compose route (VK scene as the
    // base layer UNDER the normal editor, gated NOGGIT_VK_COMPOSE). SAMPLED so the compose quad can read it.
    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &ext;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_D32_SFLOAT;
    ici.extent = { _width, _height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = _samples; // [MSAA] the depth target renders at the scene sample count
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(_device, &ici, nullptr, &_depth_image) != VK_SUCCESS)
      return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(_device, _depth_image, &req);
    _depth_mem_size = req.size;
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(_phys, &mp);
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      { type = i; break; }
    if (type == UINT32_MAX)
      return false;
    VkExportMemoryAllocateInfo exp{};
    exp.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkMemoryDedicatedAllocateInfo ded{};
    ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.pNext = &exp;
    ded.image = _depth_image;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &ded;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(_device, &mai, nullptr, &_depth_mem) != VK_SUCCESS
        || vkBindImageMemory(_device, _depth_image, _depth_mem, 0) != VK_SUCCESS)
      return false;
    {
      VkMemoryGetWin32HandleInfoKHR gh{};
      gh.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
      gh.memory = _depth_mem;
      gh.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
      HANDLE h = nullptr;
      if (reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(_pfn_vkGetMemoryWin32HandleKHR)(_device, &gh, &h) == VK_SUCCESS)
        _depth_mem_handle = h; // optional: compose route only
    }

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = _depth_image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_D32_SFLOAT;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    return vkCreateImageView(_device, &vci, nullptr, &_depth_view) == VK_SUCCESS;
  }

  bool VulkanBackend::createRenderTarget()
  {
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = _image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(_device, &vci, nullptr, &_view) != VK_SUCCESS)
      return false;
    if (!_z_image)
      return false; // phase A requires the z attachment (render pass layout below is fixed at 3)
    {
      VkImageViewCreateInfo zci = vci;
      zci.image = _z_image;
      zci.format = VK_FORMAT_R32_SFLOAT;
      if (vkCreateImageView(_device, &zci, nullptr, &_z_view) != VK_SUCCESS)
        return false;
    }

    if (!createDepthTarget())
      return false;

    // [MSAA 2026-09-03] native-present MSAA: render into multisampled colour+z+depth and let the
    // render pass RESOLVE into the existing single-sample exportable images (_image / _z_image),
    // so the blit/present/readback/parity plumbing is untouched. Sample-1 keeps the original
    // 3-attachment layout below, byte for byte.
    if (_samples != VK_SAMPLE_COUNT_1_BIT)
    {
      auto const make_ms = [&](VkFormat fmt, VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool
      {
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = fmt;
        ici.extent = { _width, _height, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = _samples;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(_device, &ici, nullptr, &img) != VK_SUCCESS)
          return false;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(_device, img, &req);
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(_phys, &mp);
        std::uint32_t type = UINT32_MAX;
        for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
          if ((req.memoryTypeBits & (1u << i))
              && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
          {
            type = i;
            break;
          }
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        if (type == UINT32_MAX || vkAllocateMemory(_device, &mai, nullptr, &mem) != VK_SUCCESS
            || vkBindImageMemory(_device, img, mem, 0) != VK_SUCCESS)
          return false;
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = fmt;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        return vkCreateImageView(_device, &vci, nullptr, &view) == VK_SUCCESS;
      };
      if (!make_ms(VK_FORMAT_R8G8B8A8_UNORM, _ms_color_image, _ms_color_mem, _ms_color_view)
          || !make_ms(VK_FORMAT_R32_SFLOAT, _ms_z_image, _ms_z_mem, _ms_z_view))
      {
        LogError << "[VK] MSAA target creation failed -- falling back to 1 sample" << std::endl;
        _samples = VK_SAMPLE_COUNT_1_BIT;
        // NOTE: _depth_image was already created at the higher count; recreate it single-sample.
        if (_depth_view) { vkDestroyImageView(_device, _depth_view, nullptr); _depth_view = VK_NULL_HANDLE; }
        if (_depth_image) { vkDestroyImage(_device, _depth_image, nullptr); _depth_image = VK_NULL_HANDLE; }
        if (_depth_mem) { vkFreeMemory(_device, _depth_mem, nullptr); _depth_mem = VK_NULL_HANDLE; }
        if (!createDepthTarget())
          return false;
      }
    }
    if (_samples != VK_SAMPLE_COUNT_1_BIT)
    {
      VkAttachmentDescription atts[5]{};
      atts[0].format = VK_FORMAT_R8G8B8A8_UNORM;          // MS colour
      atts[0].samples = _samples;
      atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      atts[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; // resolved, never read back
      atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      atts[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      atts[1] = atts[0];
      atts[1].format = VK_FORMAT_R32_SFLOAT;              // MS depth-as-colour
      atts[2].format = VK_FORMAT_D32_SFLOAT;              // MS depth
      atts[2].samples = _samples;
      atts[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      atts[2].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      atts[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      atts[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      atts[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      atts[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      atts[3].format = VK_FORMAT_R8G8B8A8_UNORM;          // resolve -> exportable colour
      atts[3].samples = VK_SAMPLE_COUNT_1_BIT;
      atts[3].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // fully overwritten by the resolve
      atts[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      atts[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      atts[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      atts[3].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      atts[3].finalLayout = VK_IMAGE_LAYOUT_GENERAL;      // blit/present/GL-import layout
      atts[4] = atts[3];
      atts[4].format = VK_FORMAT_R32_SFLOAT;              // resolve -> exportable z
      VkAttachmentReference crefs[2]{};
      crefs[0] = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
      crefs[1] = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
      VkAttachmentReference rrefs[2]{};
      rrefs[0] = { 3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
      rrefs[1] = { 4, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
      VkAttachmentReference dref{ 2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
      VkSubpassDescription sub{};
      sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      sub.colorAttachmentCount = 2;
      sub.pColorAttachments = crefs;
      sub.pResolveAttachments = rrefs;
      sub.pDepthStencilAttachment = &dref;
      VkRenderPassCreateInfo rci{};
      rci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      rci.attachmentCount = 5;
      rci.pAttachments = atts;
      rci.subpassCount = 1;
      rci.pSubpasses = &sub;
      if (vkCreateRenderPass(_device, &rci, nullptr, &_render_pass) != VK_SUCCESS)
        return false;
      VkImageView views[5] = { _ms_color_view, _ms_z_view, _depth_view, _view, _z_view };
      VkFramebufferCreateInfo fci{};
      fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      fci.renderPass = _render_pass;
      fci.attachmentCount = 5;
      fci.pAttachments = views;
      fci.width = _width;
      fci.height = _height;
      fci.layers = 1;
      LogError << "[VK] MSAA x" << static_cast<int>(_samples)
               << " targets ready (resolve -> exportable images)" << std::endl;
      return vkCreateFramebuffer(_device, &fci, nullptr, &_framebuffer) == VK_SUCCESS;
    }

    // Colour + depth. initialLayout UNDEFINED + loadOp CLEAR every frame (previous contents replaced);
    // colour finalLayout GENERAL = the cross-API interop layout GL waits on. No explicit barriers needed.
    VkAttachmentDescription atts[3]{};
    atts[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    // [phase A] attachment 1 = depth-as-colour R32F (cleared to 1.0 = background; finalLayout GENERAL for GL)
    atts[1] = atts[0];
    atts[1].format = VK_FORMAT_R32_SFLOAT;
    atts[2].format = VK_FORMAT_D32_SFLOAT;
    atts[2].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[2].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference crefs[2]{};
    crefs[0].attachment = 0;
    crefs[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    crefs[1].attachment = 1;
    crefs[1].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference dref{};
    dref.attachment = 2;
    dref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 2;
    sub.pColorAttachments = crefs;
    sub.pDepthStencilAttachment = &dref;

    VkRenderPassCreateInfo rci{};
    rci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rci.attachmentCount = 3;
    rci.pAttachments = atts;
    rci.subpassCount = 1;
    rci.pSubpasses = &sub;
    if (vkCreateRenderPass(_device, &rci, nullptr, &_render_pass) != VK_SUCCESS)
      return false;

    VkImageView views[3] = { _view, _z_view, _depth_view };
    VkFramebufferCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = _render_pass;
    fci.attachmentCount = 3;
    fci.pAttachments = views;
    fci.width = _width;
    fci.height = _height;
    fci.layers = 1;
    return vkCreateFramebuffer(_device, &fci, nullptr, &_framebuffer) == VK_SUCCESS;
  }

  bool VulkanBackend::createPipeline()
  {
    VkShaderModule vs = loadShaderModule("test.vert.spv");
    VkShaderModule fs = loadShaderModule("test.frag.spv");
    if (!vs || !fs)
    {
      if (vs) vkDestroyShaderModule(_device, vs, nullptr);
      if (fs) vkDestroyShaderModule(_device, fs, nullptr);
      return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO; // no vertex buffers (gl_VertexIndex)

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{};
    vp.width = static_cast<float>(_width);
    vp.height = static_cast<float>(_height);
    vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.extent = { _width, _height };
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = _samples; // [MSAA] every pipeline renders at the scene sample count

    VkPipelineColorBlendAttachmentState cba[2]{};
    cba[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT; // [phase A] depth-as-colour attachment
    cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 2;
    cb.pAttachments = cba;

    // the render pass now has a depth attachment -> a depth-stencil state is mandatory (disabled for the
    // fullscreen test pattern)
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = 16; // float time (+ pad, room for resolution later)

    VkPipelineLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(_device, &lci, nullptr, &_pipe_layout) != VK_SUCCESS)
    {
      vkDestroyShaderModule(_device, vs, nullptr);
      vkDestroyShaderModule(_device, fs, nullptr);
      return false;
    }

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vin;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vps;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDepthStencilState = &ds;
    pci.layout = _pipe_layout;
    pci.renderPass = _render_pass;
    VkResult const r = vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci, nullptr, &_pipeline);
    vkDestroyShaderModule(_device, fs, nullptr);
    if (r != VK_SUCCESS)
    {
      vkDestroyShaderModule(_device, vs, nullptr);
      LogError << "[VK] graphics pipeline create failed: " << vkres(r) << std::endl;
      return false;
    }

    // [overnight stage 2] SKY pipeline: same fullscreen vert + layout, gradient fragment, no depth.
    VkShaderModule sky_fs = loadShaderModule("sky.frag.spv");
    if (sky_fs)
    {
      stages[1].module = sky_fs;
      VkResult const rs2 = vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci, nullptr, &_sky_pipeline);
      vkDestroyShaderModule(_device, sky_fs, nullptr);
      if (rs2 != VK_SUCCESS)
      {
        LogError << "[VK] sky pipeline create failed: " << vkres(rs2) << " (terrain draws over flat clear)" << std::endl;
        _sky_pipeline = VK_NULL_HANDLE; // soft: sky is optional
      }
    }
    // [phase F] REAL SKY DOME pipeline: own vertex layout (pos + band colour) and its own push-only
    // layout. Depth test AND write stay off -- it is a backdrop, exactly like GL's dome draw.
    {
      VkShaderModule dome_vs = loadShaderModule("skydome.vert.spv");
      VkShaderModule dome_fs = loadShaderModule("skydome.frag.spv");
      if (dome_vs && dome_fs)
      {
        if (!_skydome_layout)
        {
          VkPushConstantRange range{};
          range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
          range.size = 80;   // mat4 + vec4 camera
          VkPipelineLayoutCreateInfo lci{};
          lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
          lci.pushConstantRangeCount = 1;
          lci.pPushConstantRanges = &range;
          if (vkCreatePipelineLayout(_device, &lci, nullptr, &_skydome_layout) != VK_SUCCESS)
            _skydome_layout = VK_NULL_HANDLE;
        }

        VkVertexInputBindingDescription dbind{ 0, 24, VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription dattrs[2]{};
        dattrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
        dattrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 };
        VkPipelineVertexInputStateCreateInfo dvin{};
        dvin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        dvin.vertexBindingDescriptionCount = 1;
        dvin.pVertexBindingDescriptions = &dbind;
        dvin.vertexAttributeDescriptionCount = 2;
        dvin.pVertexAttributeDescriptions = dattrs;

        VkPipelineDepthStencilStateCreateInfo dds = ds;
        dds.depthTestEnable = VK_FALSE;
        dds.depthWriteEnable = VK_FALSE;

        stages[0].module = dome_vs;
        stages[1].module = dome_fs;
        pci.pVertexInputState = &dvin;
        pci.pDepthStencilState = &dds;
        VkPipelineLayout const prev_dome_layout = pci.layout;
        if (_skydome_layout)
          pci.layout = _skydome_layout;
        VkResult const rd = vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci, nullptr, &_skydome_pipeline);
        pci.pVertexInputState = &vin;
        pci.pDepthStencilState = &ds;
        pci.layout = prev_dome_layout;
        if (rd != VK_SUCCESS)
        {
          LogError << "[VK] sky dome pipeline create failed: " << vkres(rd)
                   << " (placeholder gradient stays)" << std::endl;
          _skydome_pipeline = VK_NULL_HANDLE;
        }
      }
      if (dome_vs) vkDestroyShaderModule(_device, dome_vs, nullptr);
      if (dome_fs) vkDestroyShaderModule(_device, dome_fs, nullptr);
    }
    // [2026-09-08 WDL HORIZON] low-res distant terrain (client CMapLowDetail): position-only vertices,
    // push-only layout (mvp + flat colour), depth test LEQUAL. Two pipelines: solid cells write depth,
    // MAHO hole cells do not (CMapLowDetail::RenderTile draws them as a second range with the
    // depth-write state flipped). The client's [511/512, 1023/1024] viewport depth slice lives in
    // horizon.vert, so the viewport state stays the shared one.
    {
      VkShaderModule hz_vs = loadShaderModule("horizon.vert.spv");
      VkShaderModule hz_fs = loadShaderModule("horizon.frag.spv");
      if (hz_vs && hz_fs)
      {
        if (!_horizon_layout)
        {
          VkPushConstantRange range{};
          range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
          range.size = 80;   // mat4 + vec4 colour
          VkPipelineLayoutCreateInfo hlci{};
          hlci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
          hlci.pushConstantRangeCount = 1;
          hlci.pPushConstantRanges = &range;
          if (vkCreatePipelineLayout(_device, &hlci, nullptr, &_horizon_layout) != VK_SUCCESS)
            _horizon_layout = VK_NULL_HANDLE;
        }

        VkVertexInputBindingDescription hbind{ 0, 12, VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription hattr{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
        VkPipelineVertexInputStateCreateInfo hvin{};
        hvin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        hvin.vertexBindingDescriptionCount = 1;
        hvin.pVertexBindingDescriptions = &hbind;
        hvin.vertexAttributeDescriptionCount = 1;
        hvin.pVertexAttributeDescriptions = &hattr;

        VkPipelineDepthStencilStateCreateInfo hds = ds;
        hds.depthTestEnable = VK_TRUE;
        hds.depthWriteEnable = VK_TRUE;
        hds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        stages[0].module = hz_vs;
        stages[1].module = hz_fs;
        pci.pVertexInputState = &hvin;
        pci.pDepthStencilState = &hds;
        VkPipelineLayout const prev_hz_layout = pci.layout;
        if (_horizon_layout)
          pci.layout = _horizon_layout;
        VkResult const rh = vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci, nullptr, &_horizon_pipeline);
        if (rh != VK_SUCCESS)
        {
          LogError << "[VK] horizon pipeline create failed: " << vkres(rh) << " (no distant backdrop)" << std::endl;
          _horizon_pipeline = VK_NULL_HANDLE;
        }
        hds.depthWriteEnable = VK_FALSE;   // MAHO hole cells
        VkResult const rh2 = vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci, nullptr, &_horizon_pipeline_nowrite);
        if (rh2 != VK_SUCCESS)
        {
          LogError << "[VK] horizon (hole cells) pipeline create failed: " << vkres(rh2) << std::endl;
          _horizon_pipeline_nowrite = VK_NULL_HANDLE;
        }
        pci.pVertexInputState = &vin;
        pci.pDepthStencilState = &ds;
        pci.layout = prev_hz_layout;
      }
      else
      {
        LogError << "[VK] horizon shaders missing (horizon.vert.spv / horizon.frag.spv): no distant backdrop" << std::endl;
      }
      if (hz_vs) vkDestroyShaderModule(_device, hz_vs, nullptr);
      if (hz_fs) vkDestroyShaderModule(_device, hz_fs, nullptr);
    }
    vkDestroyShaderModule(_device, vs, nullptr);
    return true;
  }

  // [overnight stage 6] Descriptor infrastructure + a generated ground texture. Proves the full texturing
  // path (sampler -> set layout -> pool/set -> staging upload -> OPTIMAL image -> sampled in terrain.frag);
  // real BLP layers replace the generated image next session through this exact machinery.
  bool VulkanBackend::createTextureInfra()
  {
    // sampler (repeat, linear, mips off for the generated texture)
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 0.0f;
    if (vkCreateSampler(_device, &sci, nullptr, &_sampler) != VK_SUCCESS)
      return false;

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 1;
    dlci.pBindings = &b;
    if (vkCreateDescriptorSetLayout(_device, &dlci, nullptr, &_dsl) != VK_SUCCESS)
      return false;

    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = 8;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 8;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(_device, &dpci, nullptr, &_dpool) != VK_SUCCESS)
      return false;

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = _dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &_dsl;
    if (vkAllocateDescriptorSets(_device, &dsai, &_dset) != VK_SUCCESS)
      return false;

    // generated 256x256 grass/dirt value-noise texture (multi-octave hash noise, deliberately organic)
    constexpr std::uint32_t TW = 256;
    std::vector<std::uint32_t> pixels(TW * TW);
    auto hash01 = [](std::uint32_t x, std::uint32_t y) -> float
    {
      std::uint32_t h = x * 374761393u + y * 668265263u;
      h = (h ^ (h >> 13)) * 1274126177u;
      return static_cast<float>((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
    };
    auto smooth = [&](float x, float y) -> float
    {
      std::uint32_t const xi = static_cast<std::uint32_t>(x), yi = static_cast<std::uint32_t>(y);
      float const fx = x - xi, fy = y - yi;
      float const a = hash01(xi & 255u, yi & 255u), b2 = hash01((xi + 1u) & 255u, yi & 255u);
      float const c = hash01(xi & 255u, (yi + 1u) & 255u), d = hash01((xi + 1u) & 255u, (yi + 1u) & 255u);
      float const ux = fx * fx * (3.f - 2.f * fx), uy = fy * fy * (3.f - 2.f * fy);
      return a + (b2 - a) * ux + (c - a) * uy + (a - b2 - c + d) * ux * uy;
    };
    for (std::uint32_t y = 0; y < TW; ++y)
    {
      for (std::uint32_t x = 0; x < TW; ++x)
      {
        float n = 0.55f * smooth(x * 0.06f, y * 0.06f)
                + 0.30f * smooth(x * 0.19f, y * 0.19f)
                + 0.15f * smooth(x * 0.47f, y * 0.47f);
        float const g = 0.55f + 0.45f * n; // brightness variation
        std::uint8_t const r = static_cast<std::uint8_t>(90.f * g + 25.f * n);
        std::uint8_t const gg = static_cast<std::uint8_t>(120.f * g + 20.f * n);
        std::uint8_t const bb = static_cast<std::uint8_t>(60.f * g);
        pixels[y * TW + x] = 0xFF000000u | (static_cast<std::uint32_t>(bb) << 16)
                           | (static_cast<std::uint32_t>(gg) << 8) | r;
      }
    }

    // image (OPTIMAL) + staging upload through a one-time command buffer
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = { TW, TW, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(_device, &ici, nullptr, &_ground_image) != VK_SUCCESS)
      return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(_device, _ground_image, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(_phys, &mp);
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      { type = i; break; }
    if (type == UINT32_MAX)
      return false;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(_device, &mai, nullptr, &_ground_mem) != VK_SUCCESS
        || vkBindImageMemory(_device, _ground_image, _ground_mem, 0) != VK_SUCCESS)
      return false;

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    if (!createHostBuffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, pixels.size() * 4, staging, staging_mem, pixels.data()))
      return false;

    // one-time upload: fence starts SIGNALED -> wait+reset, record copy, submit, wait
    vkWaitForFences(_device, 1, &_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(_device, 1, &_fence);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(_cmd, &bi);
    VkImageMemoryBarrier ib{};
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = _ground_image;
    ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ib.subresourceRange.levelCount = 1;
    ib.subresourceRange.layerCount = 1;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &ib);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { TW, TW, 1 };
    vkCmdCopyBufferToImage(_cmd, staging, _ground_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &ib);
    vkEndCommandBuffer(_cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &_cmd;
    if (vkQueueSubmit(_queue, 1, &si, _fence) != VK_SUCCESS)
      return false;
    vkWaitForFences(_device, 1, &_fence, VK_TRUE, UINT64_MAX);
    // leave the fence SIGNALED (renderFrame's wait/reset cycle expects it)
    vkDestroyBuffer(_device, staging, nullptr);
    vkFreeMemory(_device, staging_mem, nullptr);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = _ground_image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(_device, &vci, nullptr, &_ground_view) != VK_SUCCESS)
      return false;

    VkDescriptorImageInfo dii{};
    dii.sampler = _sampler;
    dii.imageView = _ground_view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr{};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = _dset;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &dii;
    vkUpdateDescriptorSets(_device, 1, &wr, 0, nullptr);

    LogError << "[VK] descriptor infra ready (sampler + set + 256x256 generated ground texture)" << std::endl;
    return true;
  }

  // [overnight 04:11, DORMANT] swap the ground texture for caller pixels (decoded BLP later). Self-contained
  // duplicate of the init upload sequence on purpose -- no refactor of the proven init path.
  bool VulkanBackend::setGroundTexture(std::uint32_t const* rgba, std::uint32_t width, std::uint32_t height)
  {
    if (!_ready || !rgba || !width || !height || !_dset)
      return false;

    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = { width, height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(_device, &ici, nullptr, &img) != VK_SUCCESS)
      return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(_device, img, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(_phys, &mp);
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      { type = i; break; }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (type == UINT32_MAX
        || vkAllocateMemory(_device, &mai, nullptr, &mem) != VK_SUCCESS
        || vkBindImageMemory(_device, img, mem, 0) != VK_SUCCESS)
    {
      if (mem) vkFreeMemory(_device, mem, nullptr);
      vkDestroyImage(_device, img, nullptr);
      return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    if (!createHostBuffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          static_cast<std::size_t>(width) * height * 4, staging, staging_mem, rgba))
    {
      vkFreeMemory(_device, mem, nullptr);
      vkDestroyImage(_device, img, nullptr);
      return false;
    }

    vkWaitForFences(_device, 1, &_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(_device, 1, &_fence);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(_cmd, &bi);
    VkImageMemoryBarrier ib{};
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img;
    ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ib.subresourceRange.levelCount = 1;
    ib.subresourceRange.layerCount = 1;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &ib);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(_cmd, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &ib);
    vkEndCommandBuffer(_cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &_cmd;
    if (vkQueueSubmit(_queue, 1, &si, _fence) != VK_SUCCESS)
    {
      vkDestroyBuffer(_device, staging, nullptr);
      vkFreeMemory(_device, staging_mem, nullptr);
      vkFreeMemory(_device, mem, nullptr);
      vkDestroyImage(_device, img, nullptr);
      return false;
    }
    vkWaitForFences(_device, 1, &_fence, VK_TRUE, UINT64_MAX); // fence stays SIGNALED for renderFrame
    vkDestroyBuffer(_device, staging, nullptr);
    vkFreeMemory(_device, staging_mem, nullptr);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(_device, &vci, nullptr, &view) != VK_SUCCESS)
    {
      vkFreeMemory(_device, mem, nullptr);
      vkDestroyImage(_device, img, nullptr);
      return false;
    }

    VkDescriptorImageInfo dii{};
    dii.sampler = _sampler;
    dii.imageView = view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr{};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = _dset;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &dii;
    vkQueueWaitIdle(_queue); // no in-flight frame may still sample the old image / read the old descriptor
    vkUpdateDescriptorSets(_device, 1, &wr, 0, nullptr);

    // retire the old image
    if (_ground_view) vkDestroyImageView(_device, _ground_view, nullptr);
    if (_ground_image) vkDestroyImage(_device, _ground_image, nullptr);
    if (_ground_mem) vkFreeMemory(_device, _ground_mem, nullptr);
    _ground_image = img;
    _ground_mem = mem;
    _ground_view = view;
    LogError << "[VK] ground texture replaced: " << width << "x" << height << std::endl;
    return true;
  }

  // [VK-1b] terrain pipeline: position-only vertex stream, depth-tested, MVP + clock in push constants.
  bool VulkanBackend::createTerrainPipeline()
  {
    VkShaderModule vs = loadShaderModule("terrain.vert.spv");
    VkShaderModule fs = loadShaderModule("terrain.frag.spv");
    if (!vs || !fs)
    {
      if (vs) vkDestroyShaderModule(_device, vs, nullptr);
      if (fs) vkDestroyShaderModule(_device, fs, nullptr);
      return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = 36; // vec3 position + vec3 smooth normal + vec3 MCCV colour, interleaved
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 };
    attrs[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, 24 };
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 3;
    vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{};
    vp.width = static_cast<float>(_width);
    vp.height = static_cast<float>(_height);
    vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.extent = { _width, _height };
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; // orientation-agnostic while the mesh winding settles
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = _samples; // [MSAA] every pipeline renders at the scene sample count

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cba[2]{};
    cba[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT; // [phase A] depth-as-colour attachment
    cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 2;
    cb.pAttachments = cba;

    // one push block visible to BOTH stages: mat4 mvp (0..64) + float time (64..68), padded to 80
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = 80;
    VkPipelineLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &push;
    lci.setLayoutCount = _dsl ? 1 : 0; // stage 6: set 0 = ground sampler (createTextureInfra runs first)
    lci.pSetLayouts = _dsl ? &_dsl : nullptr;
    if (vkCreatePipelineLayout(_device, &lci, nullptr, &_terrain_layout) != VK_SUCCESS)
    {
      vkDestroyShaderModule(_device, vs, nullptr);
      vkDestroyShaderModule(_device, fs, nullptr);
      return false;
    }

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vin;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vps;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDepthStencilState = &ds;
    pci.layout = _terrain_layout;
    pci.renderPass = _render_pass;
    VkResult const r = vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci, nullptr, &_terrain_pipeline);
    vkDestroyShaderModule(_device, fs, nullptr);
    if (r != VK_SUCCESS)
    {
      vkDestroyShaderModule(_device, vs, nullptr);
      LogError << "[VK] terrain pipeline create failed: " << vkres(r) << std::endl;
      return false;
    }

    // [overnight stage 3] WATER pipeline: same vertex layout/stages, alpha blending on, depth write OFF
    // (test stays on so water sits correctly against the terrain). Soft-optional like the sky.
    VkShaderModule water_fs = loadShaderModule("water.frag.spv");
    if (water_fs)
    {
      stages[1].module = water_fs;
      VkShaderModule water_vs = loadShaderModule("water.vert.spv");
      VkShaderModule const prev_vs = stages[0].module;
      if (water_vs)
        stages[0].module = water_vs;
      ds.depthWriteEnable = VK_FALSE;
      cba[0].blendEnable = VK_TRUE;
      cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      cba[0].colorBlendOp = VK_BLEND_OP_ADD;
      cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
      cba[1].colorWriteMask = 0; // water writes NO depth (translucent; depth write is off in GL too)

      // Water has its OWN vertex layout -- it shared the terrain's (stride 36) until the feed grew
      // the data GL's liquid shader needs:
      //   pos xyz | normal xyz | uv xy | depth | type | anim_u | anim_v   (48 bytes)
      // Own pipeline layout: the liquid shader reads the lighting UBO and the bindless textures,
      // both in SET 1, which _terrain_layout does not declare.
      if (!_water_layout && _dsl && _tt_dsl)
      {
        VkPushConstantRange wpush_range{};
        wpush_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        wpush_range.size = 80;
        VkDescriptorSetLayout wlayouts[2] = { _dsl, _tt_dsl };
        VkPipelineLayoutCreateInfo wlci{};
        wlci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        wlci.pushConstantRangeCount = 1;
        wlci.pPushConstantRanges = &wpush_range;
        wlci.setLayoutCount = 2;
        wlci.pSetLayouts = wlayouts;
        if (vkCreatePipelineLayout(_device, &wlci, nullptr, &_water_layout) != VK_SUCCESS)
          _water_layout = VK_NULL_HANDLE;
      }
      VkPipelineLayout const prev_layout = pci.layout;
      if (_water_layout)
        pci.layout = _water_layout;

      VkVertexInputBindingDescription wbind{ 0, 68, VK_VERTEX_INPUT_RATE_VERTEX };
      VkVertexInputAttributeDescription wattrs[7]{};
      wattrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };   // pos
      wattrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 };  // normal
      wattrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,    24 };  // uv
      wattrs[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT,    32 };  // depth, type
      wattrs[4] = { 4, 0, VK_FORMAT_R32G32_SFLOAT,    40 };  // anim_u, anim_v
      wattrs[5] = { 5, 0, VK_FORMAT_R32_SFLOAT,       48 };  // bindless liquid texture id
      // WMO liquid rides the same mesh but obeys wmo_liquid_frag's colour law, which needs the
      // group's flags and its baked MOMT.diffColor. ADT water leaves this zeroed.
      wattrs[6] = { 6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 52 }; // wmo flags | material colour rgb
      VkPipelineVertexInputStateCreateInfo wvin{};
      wvin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
      wvin.vertexBindingDescriptionCount = 1;
      wvin.pVertexBindingDescriptions = &wbind;
      wvin.vertexAttributeDescriptionCount = 7;
      wvin.pVertexAttributeDescriptions = wattrs;
      pci.pVertexInputState = &wvin;

      // [phase F] CLOUD DECK pipeline. Same two sets as water (it samples the bindless array), its
      // own 96-byte push, alpha blend over the sky, and -- like GL's blendFuncSeparate(ZERO, ONE) --
      // the framebuffer ALPHA is left untouched because it is the bloom's emissive mask.
      {
        VkShaderModule cloud_vs = loadShaderModule("cloud.vert.spv");
        VkShaderModule cloud_fs = loadShaderModule("cloud.frag.spv");
        if (cloud_vs && cloud_fs && _dsl && _tt_dsl)
        {
          if (!_cloud_layout)
          {
            VkPushConstantRange range{};
            range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            range.size = 96;   // mat4 + vec4 camera/opacity + vec4 texture index
            VkDescriptorSetLayout sets[2] = { _dsl, _tt_dsl };
            VkPipelineLayoutCreateInfo lci{};
            lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            lci.pushConstantRangeCount = 1;
            lci.pPushConstantRanges = &range;
            lci.setLayoutCount = 2;
            lci.pSetLayouts = sets;
            if (vkCreatePipelineLayout(_device, &lci, nullptr, &_cloud_layout) != VK_SUCCESS)
              _cloud_layout = VK_NULL_HANDLE;
          }

          VkVertexInputBindingDescription cbind{ 0, 24, VK_VERTEX_INPUT_RATE_VERTEX };
          VkVertexInputAttributeDescription cattrs[3]{};
          cattrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };   // position
          cattrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT,    12 };  // uv
          cattrs[2] = { 2, 0, VK_FORMAT_R32_SFLOAT,       20 };  // per-row alpha
          VkPipelineVertexInputStateCreateInfo cvin{};
          cvin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
          cvin.vertexBindingDescriptionCount = 1;
          cvin.pVertexBindingDescriptions = &cbind;
          cvin.vertexAttributeDescriptionCount = 3;
          cvin.pVertexAttributeDescriptions = cattrs;

          VkPipelineDepthStencilStateCreateInfo cds = ds;
          cds.depthTestEnable = VK_FALSE;
          cds.depthWriteEnable = VK_FALSE;
          VkPipelineRasterizationStateCreateInfo crs = rs;
          crs.cullMode = VK_CULL_MODE_NONE;

          // Save and restore EVERYTHING borrowed from the in-progress water pipeline state: this
          // block sits between the water layout assignment and its create call, so hardcoding the
          // terrain layout back handed the water pipeline the wrong vertex stride.
          auto const* const prev_cloud_vin = pci.pVertexInputState;
          auto const* const prev_cloud_ds = pci.pDepthStencilState;
          auto const* const prev_cloud_rs = pci.pRasterizationState;
          VkShaderModule const prev_cloud_vs_mod = stages[0].module;
          VkShaderModule const prev_cloud_fs_mod = stages[1].module;
          stages[0].module = cloud_vs;
          stages[1].module = cloud_fs;
          pci.pVertexInputState = &cvin;
          pci.pDepthStencilState = &cds;
          pci.pRasterizationState = &crs;
          VkPipelineLayout const prev_cloud_layout = pci.layout;
          if (_cloud_layout)
            pci.layout = _cloud_layout;
          // cba[0] is already SRC_ALPHA / ONE_MINUS_SRC_ALPHA from the water setup above; keep the
          // colour blend but stop the deck writing framebuffer alpha.
          VkColorComponentFlags const prev_mask = cba[0].colorWriteMask;
          cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                | VK_COLOR_COMPONENT_B_BIT;
          VkResult const rc = vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci, nullptr, &_cloud_pipeline);
          cba[0].colorWriteMask = prev_mask;
          pci.pVertexInputState = prev_cloud_vin;
          pci.pDepthStencilState = prev_cloud_ds;
          pci.pRasterizationState = prev_cloud_rs;
          pci.layout = prev_cloud_layout;
          stages[0].module = prev_cloud_vs_mod;
          stages[1].module = prev_cloud_fs_mod;
          if (rc != VK_SUCCESS)
          {
            LogError << "[VK] cloud pipeline create failed: " << vkres(rc) << " (clouds skipped)" << std::endl;
            _cloud_pipeline = VK_NULL_HANDLE;
          }
        }
        if (cloud_vs) vkDestroyShaderModule(_device, cloud_vs, nullptr);
        if (cloud_fs) vkDestroyShaderModule(_device, cloud_fs, nullptr);
      }

      // [phase F] CELESTIAL billboards: no vertex buffer (corners from gl_VertexIndex), depth TEST
      // on / write off so terrain occludes the disc but the disc never occludes the world. Two blend
      // variants because blend state is baked into a Vulkan pipeline.
      {
        VkShaderModule cel_vs = loadShaderModule("celestial.vert.spv");
        VkShaderModule cel_fs = loadShaderModule("celestial.frag.spv");
        if (cel_vs && cel_fs && _dsl && _tt_dsl)
        {
          if (!_cel_layout)
          {
            VkPushConstantRange range{};
            range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            range.size = 128;   // exactly the guaranteed minimum -- see celestial.vert
            VkDescriptorSetLayout sets[2] = { _dsl, _tt_dsl };
            VkPipelineLayoutCreateInfo lci{};
            lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            lci.pushConstantRangeCount = 1;
            lci.pPushConstantRanges = &range;
            lci.setLayoutCount = 2;
            lci.pSetLayouts = sets;
            if (vkCreatePipelineLayout(_device, &lci, nullptr, &_cel_layout) != VK_SUCCESS)
              _cel_layout = VK_NULL_HANDLE;
          }

          VkPipelineVertexInputStateCreateInfo cel_vin{};
          cel_vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
          VkPipelineInputAssemblyStateCreateInfo cel_ia = ia;
          cel_ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
          VkPipelineDepthStencilStateCreateInfo cel_ds = ds;
          cel_ds.depthTestEnable = VK_TRUE;
          cel_ds.depthWriteEnable = VK_FALSE;
          cel_ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
          VkPipelineRasterizationStateCreateInfo cel_rs = rs;
          cel_rs.cullMode = VK_CULL_MODE_NONE;

          auto const* const prev_cel_vin = pci.pVertexInputState;
          auto const* const prev_cel_ia = pci.pInputAssemblyState;
          auto const* const prev_cel_ds = pci.pDepthStencilState;
          auto const* const prev_cel_rs = pci.pRasterizationState;
          VkShaderModule const prev_cel_vs = stages[0].module;
          VkShaderModule const prev_cel_fs = stages[1].module;
          VkPipelineLayout const prev_cel_layout = pci.layout;
          VkBlendFactor const prev_src = cba[0].srcColorBlendFactor;
          VkBlendFactor const prev_dst = cba[0].dstColorBlendFactor;

          stages[0].module = cel_vs;
          stages[1].module = cel_fs;
          pci.pVertexInputState = &cel_vin;
          pci.pInputAssemblyState = &cel_ia;
          pci.pDepthStencilState = &cel_ds;
          pci.pRasterizationState = &cel_rs;
          if (_cel_layout)
            pci.layout = _cel_layout;

          cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
          cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
          VkResult const ra = vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci, nullptr, &_cel_pipeline_alpha);
          cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
          cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
          VkResult const rg = vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci, nullptr, &_cel_pipeline_add);

          cba[0].srcColorBlendFactor = prev_src;
          cba[0].dstColorBlendFactor = prev_dst;
          pci.pVertexInputState = prev_cel_vin;
          pci.pInputAssemblyState = prev_cel_ia;
          pci.pDepthStencilState = prev_cel_ds;
          pci.pRasterizationState = prev_cel_rs;
          pci.layout = prev_cel_layout;
          stages[0].module = prev_cel_vs;
          stages[1].module = prev_cel_fs;

          if (ra != VK_SUCCESS || rg != VK_SUCCESS)
          {
            LogError << "[VK] celestial pipeline create failed: alpha=" << vkres(ra)
                     << " add=" << vkres(rg) << " (sun/moon skipped)" << std::endl;
            if (_cel_pipeline_alpha) { vkDestroyPipeline(_device, _cel_pipeline_alpha, nullptr); _cel_pipeline_alpha = VK_NULL_HANDLE; }
            if (_cel_pipeline_add) { vkDestroyPipeline(_device, _cel_pipeline_add, nullptr); _cel_pipeline_add = VK_NULL_HANDLE; }
          }
        }
        if (cel_vs) vkDestroyShaderModule(_device, cel_vs, nullptr);
        if (cel_fs) vkDestroyShaderModule(_device, cel_fs, nullptr);
      }

      // [phase G] PARTICLE pipelines -- one per M2 particle blend mode, because blend state is baked
      // into a Vulkan pipeline. Factors mirror ParticleSystem::draw's blendFuncSeparate table.
      {
        VkShaderModule part_vs = loadShaderModule("particle.vert.spv");
        VkShaderModule part_fs = loadShaderModule("particle.frag.spv");
        if (part_vs && part_fs && _dsl && _tt_dsl)
        {
          if (!_particle_layout)
          {
            VkPushConstantRange range{};
            range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            range.size = 80;   // mat4 + vec4 (texture, blend, alpha test, alpha mod)
            VkDescriptorSetLayout sets[2] = { _dsl, _tt_dsl };
            VkPipelineLayoutCreateInfo lci{};
            lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            lci.pushConstantRangeCount = 1;
            lci.pPushConstantRanges = &range;
            lci.setLayoutCount = 2;
            lci.pSetLayouts = sets;
            if (vkCreatePipelineLayout(_device, &lci, nullptr, &_particle_layout) != VK_SUCCESS)
              _particle_layout = VK_NULL_HANDLE;
          }

          VkVertexInputBindingDescription pbind{ 0, 36, VK_VERTEX_INPUT_RATE_VERTEX };
          VkVertexInputAttributeDescription pattrs[3]{};
          pattrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0 };   // world position
          pattrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT,      12 };   // uv
          pattrs[2] = { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20 };  // colour
          VkPipelineVertexInputStateCreateInfo pvin{};
          pvin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
          pvin.vertexBindingDescriptionCount = 1;
          pvin.pVertexBindingDescriptions = &pbind;
          pvin.vertexAttributeDescriptionCount = 3;
          pvin.pVertexAttributeDescriptions = pattrs;

          VkPipelineDepthStencilStateCreateInfo pds = ds;
          pds.depthTestEnable = VK_TRUE;
          pds.depthWriteEnable = VK_FALSE;   // particles never occlude the world
          pds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
          VkPipelineRasterizationStateCreateInfo prs = rs;
          prs.cullMode = VK_CULL_MODE_NONE;

          auto const* const prev_p_vin = pci.pVertexInputState;
          auto const* const prev_p_ds = pci.pDepthStencilState;
          auto const* const prev_p_rs = pci.pRasterizationState;
          VkShaderModule const prev_p_vs = stages[0].module;
          VkShaderModule const prev_p_fs = stages[1].module;
          VkPipelineLayout const prev_p_layout = pci.layout;
          VkBlendFactor const prev_p_src = cba[0].srcColorBlendFactor;
          VkBlendFactor const prev_p_dst = cba[0].dstColorBlendFactor;

          stages[0].module = part_vs;
          stages[1].module = part_fs;
          pci.pVertexInputState = &pvin;
          pci.pDepthStencilState = &pds;
          pci.pRasterizationState = &prs;
          if (_particle_layout)
            pci.layout = _particle_layout;

          // ParticleSystem::draw: 0/1 opaque, 2 alpha, 3 additive, 4 src-alpha additive,
          // 5 modulate, 6 mod2x, 7 premultiplied alpha.
          struct { VkBlendFactor src, dst; } const factors[8] = {
            { VK_BLEND_FACTOR_ONE,             VK_BLEND_FACTOR_ZERO },
            { VK_BLEND_FACTOR_ONE,             VK_BLEND_FACTOR_ZERO },
            { VK_BLEND_FACTOR_SRC_ALPHA,       VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA },
            { VK_BLEND_FACTOR_ONE,             VK_BLEND_FACTOR_ONE },
            { VK_BLEND_FACTOR_SRC_ALPHA,       VK_BLEND_FACTOR_ONE },
            { VK_BLEND_FACTOR_DST_COLOR,       VK_BLEND_FACTOR_ZERO },
            { VK_BLEND_FACTOR_DST_COLOR,       VK_BLEND_FACTOR_SRC_COLOR },
            { VK_BLEND_FACTOR_ONE,             VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA },
          };
          for (int bi = 0; bi < 9; ++bi)
          {
            cba[0].blendEnable = (bi == 0 || bi == 1) ? VK_FALSE : VK_TRUE;
            // index 8 = ribbon blend 3 (SRC_COLOR/ONE)
            cba[0].srcColorBlendFactor = (bi == 8) ? VK_BLEND_FACTOR_SRC_COLOR : factors[bi].src;
            cba[0].dstColorBlendFactor = (bi == 8) ? VK_BLEND_FACTOR_ONE : factors[bi].dst;
            cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            VkResult const rp = vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci, nullptr,
                                                          &_particle_pipelines[bi]);
            if (rp != VK_SUCCESS)
            {
              LogError << "[VK] particle pipeline " << bi << " create failed: " << vkres(rp) << std::endl;
              _particle_pipelines[bi] = VK_NULL_HANDLE;
            }
          }

          cba[0].blendEnable = VK_TRUE;
          cba[0].srcColorBlendFactor = prev_p_src;
          cba[0].dstColorBlendFactor = prev_p_dst;
          cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
          cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
          pci.pVertexInputState = prev_p_vin;
          pci.pDepthStencilState = prev_p_ds;
          pci.pRasterizationState = prev_p_rs;
          pci.layout = prev_p_layout;
          stages[0].module = prev_p_vs;
          stages[1].module = prev_p_fs;
        }
        if (part_vs) vkDestroyShaderModule(_device, part_vs, nullptr);
        if (part_fs) vkDestroyShaderModule(_device, part_fs, nullptr);
      }

      VkResult const rw = vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci, nullptr, &_water_pipeline);
      pci.pVertexInputState = &vin;   // restore for any pipeline created after this one
      pci.layout = prev_layout;
      stages[0].module = prev_vs;
      if (water_vs) vkDestroyShaderModule(_device, water_vs, nullptr);
      vkDestroyShaderModule(_device, water_fs, nullptr);
      if (rw != VK_SUCCESS)
      {
        LogError << "[VK] water pipeline create failed: " << vkres(rw) << " (water skipped)" << std::endl;
        _water_pipeline = VK_NULL_HANDLE;
      }
    }
    vkDestroyShaderModule(_device, vs, nullptr);
    return true;
  }

  // [overnight stage 4] instanced doodad pipeline: binding 0 = per-vertex pos+normal (24B), binding 1 =
  // per-INSTANCE mat4 (64B, divisor 1, locations 2-5). Depth on, opaque. Same push layout as terrain.

  // =====================================================================================================
  // [VULKAN phase B, 2026-08-29] TEXTURED TERRAIN -- the first pass Vulkan owns.
  // =====================================================================================================

  // [2026-09-04 NATIVE UI COMPOSITE] Self-contained on purpose: the shared `pci` scaffolding in
  // createTerrainPipeline is a state machine several pipelines borrow from and restore, and it has
  // already caused one wrong-vertex-stride bug. This builds its own create-info from scratch.
  bool VulkanBackend::createUiPipeline()
  {
    if (_ui_pipeline)
      return true;
    if (!_particle_layout || !_render_pass)
      return false;   // shares the particle layout (same mat4+vec4 push, same two sets)

    VkShaderModule vs = loadShaderModule("ui.vert.spv");
    VkShaderModule fs = loadShaderModule("ui.frag.spv");
    if (!vs || !fs)
    {
      if (vs) vkDestroyShaderModule(_device, vs, nullptr);
      if (fs) vkDestroyShaderModule(_device, fs, nullptr);
      LogError << "[VK] UI overlay: shader modules missing (ui.vert.spv/ui.frag.spv)" << std::endl;
      return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin{};   // no buffers: 3 generated vertices
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{};
    vp.width = static_cast<float>(_width);
    vp.height = static_cast<float>(_height);
    vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.extent = { _width, _height };
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = _samples;

    VkPipelineDepthStencilStateCreateInfo ds{};   // an overlay: never tested, never written
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    VkPipelineColorBlendAttachmentState cba[2]{};
    cba[0].blendEnable = VK_TRUE;
    cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba[0].colorBlendOp = VK_BLEND_OP_ADD;
    cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
    cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                          | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba[1].colorWriteMask = 0;   // never disturb the depth-as-colour attachment
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 2;
    cb.pAttachments = cba;

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vin;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vps;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.layout = _particle_layout;
    pci.renderPass = _render_pass;
    pci.subpass = 0;

    VkResult const r = vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci, nullptr, &_ui_pipeline);
    vkDestroyShaderModule(_device, vs, nullptr);
    vkDestroyShaderModule(_device, fs, nullptr);
    if (r != VK_SUCCESS)
    {
      _ui_pipeline = VK_NULL_HANDLE;
      LogError << "[VK] UI overlay pipeline create failed: " << vkres(r) << std::endl;
      return false;
    }
    LogError << "[VK] UI overlay pipeline ready" << std::endl;
    return true;
  }

  bool VulkanBackend::setUiOverlay(void const* rgba, std::uint32_t width, std::uint32_t height)
  {
    if (!_ready || !rgba || !width || !height)
    {
      _ui_visible = false;
      return true;               // nothing to draw is not an error
    }
    if (!_ui_pipeline && !createUiPipeline())
    {
      _ui_visible = false;
      return false;
    }

    if (_ui_tex < 0 || width != _ui_w || height != _ui_h)
    {
      // First use, or the window was resized: a new slot at the new extent.
      _ui_tex = addTexture(static_cast<std::uint32_t const*>(rgba), width, height);
      if (_ui_tex < 0)
      {
        _ui_visible = false;
        return false;
      }
      _ui_w = width;
      _ui_h = height;
      LogError << "[VK] UI overlay texture: slot " << _ui_tex << " " << width << "x" << height << std::endl;
    }
    else if (!updateTexturePixelsInPlace(_ui_tex, rgba, width, height))
    {
      _ui_visible = false;
      return false;
    }
    _ui_visible = true;
    return true;
  }

  bool VulkanBackend::uploadImage2D(std::uint32_t width, std::uint32_t height, VkFormat format, std::uint32_t bpp,
                                    std::vector<std::vector<std::uint8_t>> const& mips, VkImage& image,
                                    VkDeviceMemory& mem, VkImageView& view)
  {
    SlowCall _sc_uploadImage2D("uploadImage2D");
    // [phase I] runs on the COPY QUEUE with its own pool/buffer/fence. This used to record into the
    // FRAME's primary buffer and wait on the FRAME's fence, which serialised every texture upload
    // against rendering and shared mutable state with the in-flight frame.
    std::uint32_t const levels = static_cast<std::uint32_t>(mips.size());
    if (!levels) return false;
    if (!_copy_cmd || !_copy_fence) return false;
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = { width, height, 1 };
    ici.mipLevels = levels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(_device, &ici, nullptr, &image) != VK_SUCCESS)
      return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(_device, image, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(_phys, &mp);
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      { type = i; break; }
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(_device, &mai, nullptr, &mem) != VK_SUCCESS || vkBindImageMemory(_device, image, mem, 0) != VK_SUCCESS)
      return false;

    std::size_t total = 0;
    for (auto const& m : mips) total += m.size();
    std::vector<std::uint8_t> all;
    all.reserve(total);
    std::vector<VkBufferImageCopy> regions(levels);
    std::uint32_t w = width, h = height;
    for (std::uint32_t l = 0; l < levels; ++l)
    {
      regions[l].bufferOffset = all.size();
      regions[l].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      regions[l].imageSubresource.mipLevel = l;
      regions[l].imageSubresource.layerCount = 1;
      regions[l].imageExtent = { w, h, 1 };
      all.insert(all.end(), mips[l].begin(), mips[l].end());
      w = std::max(1u, w >> 1); h = std::max(1u, h >> 1);
    }
    // Stage into the SHARED arena at an offset. Growing it means the batch already recorded against
    // the old arena has to go out first, or its copies would read freed memory.
    std::size_t const need = (_tex_batch_open ? _tex_arena_used : 0u) + all.size();
    if (need > _tex_arena_cap)
    {
      flushTextureUploads();
      std::size_t const want = std::max<std::size_t>(need + need / 2 + 4096, 16u << 20);
      retireBuffer(_tex_arena, _tex_arena_mem);
      _tex_arena = VK_NULL_HANDLE;
      _tex_arena_mem = VK_NULL_HANDLE;
      _tex_arena_cap = 0;
      _tex_arena_used = 0;
      if (!createHostBuffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, want, _tex_arena, _tex_arena_mem, nullptr))
        return false;
      _tex_arena_cap = want;
    }
    if (!beginTextureBatch())
      return false;
    void* const arena_ptr = mapPersistent(_tex_arena_mem);
    if (!arena_ptr)
      return false;
    std::size_t const arena_offset = _tex_arena_used;
    std::memcpy(static_cast<std::uint8_t*>(arena_ptr) + arena_offset, all.data(), all.size());
    _tex_arena_used += all.size();
    for (auto& r : regions)
      r.bufferOffset += arena_offset;      // regions were built relative to THIS upload
    VkBuffer const staging = _tex_arena;
    VkImageMemoryBarrier ib{};
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = image;
    ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ib.subresourceRange.levelCount = levels;
    ib.subresourceRange.layerCount = 1;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(_copy_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
    vkCmdCopyBufferToImage(_copy_cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, regions.data());
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(_copy_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
    // This upload is one entry in a batch that goes out ONCE per frame (flushTextureUploads, called
    // at the top of the frame). Streaming 50 textures for a tile crossing therefore costs ONE GPU
    // round trip instead of fifty -- that per-texture round trip is what produced 500+ ms frames.
    // NOGGIT_VK_TEXBATCH=0 forces the old per-upload flush for comparison.
    if (!textureBatchEnabled())
      flushTextureUploads();

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = levels;
    vci.subresourceRange.layerCount = 1;
    (void)bpp;
    return vkCreateImageView(_device, &vci, nullptr, &view) == VK_SUCCESS;
  }

  bool VulkanBackend::createTerrainTexInfra()
  {
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 16.0f;
    bool const no_af = std::getenv("NOGGIT_PARITY_NO_AF") != nullptr; // parity diagnostic
    sci.anisotropyEnable = no_af ? VK_FALSE : VK_TRUE;
    sci.maxAnisotropy = no_af ? 1.0f : 16.0f;
    if (vkCreateSampler(_device, &sci, nullptr, &_tile_sampler) != VK_SUCCESS) return false;
    sci.anisotropyEnable = VK_FALSE;
    sci.maxAnisotropy = 1.0f;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.maxLod = 0.0f;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(_device, &sci, nullptr, &_atlas_sampler) != VK_SUCCESS) return false;

    // binding numbers: 0 alpha atlas, 1 shadow atlas, 2 chunk SSBO, 3 lighting UBO, 4 tilesets[] --
    // the variable-count array MUST carry the HIGHEST binding number in the set (Vulkan rule).
    VkDescriptorSetLayoutBinding ordered[6]{};
    ordered[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    ordered[1] = { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    ordered[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    // VERTEX too: m2.vert reads the camera basis from this block to build the SPHERE MAP uv.
    ordered[3] = { 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    ordered[4] = { 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr }; // M2 bones
    ordered[5] = { 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    VkDescriptorBindingFlagsEXT oflags[6] = { 0, 0, 0, 0, 0,
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT
      | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT
      | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT_EXT };
    VkDescriptorSetLayoutBindingFlagsCreateInfoEXT bfci{};
    bfci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT;
    bfci.bindingCount = 6;
    bfci.pBindingFlags = oflags;
    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.pNext = &bfci;
    dlci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT_EXT;
    dlci.bindingCount = 6;
    dlci.pBindings = ordered;
    if (vkCreateDescriptorSetLayout(_device, &dlci, nullptr, &_tt_dsl) != VK_SUCCESS) return false;

    VkDescriptorPoolSize ps[3]{};
    ps[0] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures + 2 };
    ps[1] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 }; // chunk SSBO + M2 bones
    ps[2] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT;   // must match the layout
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(_device, &dpci, nullptr, &_tt_dpool) != VK_SUCCESS) return false;

    std::uint32_t const var_count = kMaxTextures;
    VkDescriptorSetVariableDescriptorCountAllocateInfoEXT vdc{};
    vdc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT;
    vdc.descriptorSetCount = 1;
    vdc.pDescriptorCounts = &var_count;
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.pNext = &vdc;
    dsai.descriptorPool = _tt_dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &_tt_dsl;
    if (vkAllocateDescriptorSets(_device, &dsai, &_tt_dset) != VK_SUCCESS) return false;

    // lighting UBO: persistently mapped host-visible (rewritten every frame)
    std::vector<std::uint8_t> zeros(4096, 0);
    if (!createHostBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, zeros.size(), _light_ubo, _light_ubo_mem, zeros.data()))
      return false;
    if (vkMapMemory(_device, _light_ubo_mem, 0, zeros.size(), 0, &_light_ubo_map) != VK_SUCCESS)
      return false;
    VkDescriptorBufferInfo lbi{ _light_ubo, 0, zeros.size() };
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = _tt_dset;
    w.dstBinding = 3;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo = &lbi;
    vkUpdateDescriptorSets(_device, 1, &w, 0, nullptr);
    LogError << "[VK] textured-terrain descriptor infra ready (bindless " << kMaxTextures << " slots)" << std::endl;
    return true;
  }

  std::int32_t VulkanBackend::addTexture(std::uint32_t const* rgba, std::uint32_t width, std::uint32_t height)
  {
    if (!_ready || !_tt_dset || !rgba || !width || !height) return -1;
    if (_textures.size() >= kMaxTextures) return -1;
    // CPU mip chain (box filter) -- tilesets shimmer badly without mips
    std::vector<std::vector<std::uint8_t>> mips;
    std::vector<std::uint32_t> cur(rgba, rgba + static_cast<std::size_t>(width) * height);
    std::uint32_t w = width, h = height;
    for (;;)
    {
      mips.emplace_back(reinterpret_cast<std::uint8_t const*>(cur.data()),
                        reinterpret_cast<std::uint8_t const*>(cur.data()) + cur.size() * 4u);
      if (w == 1 && h == 1) break;
      std::uint32_t const nw = std::max(1u, w >> 1), nh = std::max(1u, h >> 1);
      std::vector<std::uint32_t> next(static_cast<std::size_t>(nw) * nh);
      for (std::uint32_t y = 0; y < nh; ++y)
        for (std::uint32_t x = 0; x < nw; ++x)
        {
          std::uint32_t const x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
          std::uint32_t const y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
          std::uint32_t const p[4] = { cur[y0 * w + x0], cur[y0 * w + x1], cur[y1 * w + x0], cur[y1 * w + x1] };
          std::uint32_t out = 0;
          for (int c = 0; c < 4; ++c)
          {
            std::uint32_t sum = 0;
            for (int k = 0; k < 4; ++k) sum += (p[k] >> (8 * c)) & 0xFFu;
            out |= ((sum + 2u) / 4u) << (8 * c);
          }
          next[y * nw + x] = out;
        }
      cur.swap(next); w = nw; h = nh;
    }
    TexEntry e;
    e.w = width; e.h = height; e.levels = static_cast<std::uint32_t>(mips.size());
    if (!uploadImage2D(width, height, VK_FORMAT_R8G8B8A8_UNORM, 4, mips, e.image, e.mem, e.view))
    {
      if (e.view) vkDestroyImageView(_device, e.view, nullptr);
      if (e.image) vkDestroyImage(_device, e.image, nullptr);
      if (e.mem) vkFreeMemory(_device, e.mem, nullptr);
      return -1;
    }
    std::int32_t const idx = static_cast<std::int32_t>(_textures.size());
    _textures.push_back(e);
    VkDescriptorImageInfo dii{ _tile_sampler, e.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wd{};
    wd.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wd.dstSet = _tt_dset;
    wd.dstBinding = 5;
    wd.dstArrayElement = static_cast<std::uint32_t>(idx);
    wd.descriptorCount = 1;
    wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wd.pImageInfo = &dii;
    // [spike fix] No stall: the bindless array is UPDATE_AFTER_BIND, so this descriptor write is
    // legal while in-flight command buffers still reference the set. This used to be a FULL GPU
    // stall on every streamed texture -- the periodic hitch while flying.
    vkUpdateDescriptorSets(_device, 1, &wd, 0, nullptr);
    return idx;
  }

  bool VulkanBackend::updateTexturePixelsInPlace(std::int32_t index, void const* rgba,
                                                 std::uint32_t width, std::uint32_t height)
  {
    if (index < 0 || static_cast<std::size_t>(index) >= _textures.size())
      return false;
    TexEntry const& slot = _textures[static_cast<std::size_t>(index)];
    // only valid when the destination is exactly the shape we are about to write
    if (!slot.image || slot.levels != 1u || slot.w != width || slot.h != height)
      return false;
    if (!_copy_cmd || !_copy_fence)
      return false;

    // This path drives _copy_cmd / _copy_fence itself. An OPEN upload batch owns both -- the fence
    // is reset and unsubmitted, and the command buffer is mid-recording -- so waiting on the fence
    // here would hang forever. Close the batch before touching either. (Found by hanging the
    // renderer: CPU time frozen while the cloud texture, which lands here ~10x a second, waited on
    // a fence that nothing was ever going to signal.)
    flushTextureUploads();

    std::size_t const bytes = static_cast<std::size_t>(width) * height * 4u;
    if (!writeHostBuffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rgba, bytes,
                         _texup_staging, _texup_staging_mem, _texup_staging_cap))
      return false;

    vkWaitForFences(_device, 1, &_copy_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(_device, 1, &_copy_fence);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(_copy_cmd, &bi) != VK_SUCCESS)
      return false;

    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = slot.image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(_copy_cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    VkBufferImageCopy r{};
    r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageExtent = { width, height, 1u };
    vkCmdCopyBufferToImage(_copy_cmd, _texup_staging, slot.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);

    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(_copy_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkEndCommandBuffer(_copy_cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &_copy_cmd;
    if (vkQueueSubmit(_copy_queue, 1, &si, _copy_fence) != VK_SUCCESS)
      return false;
    vkWaitForFences(_device, 1, &_copy_fence, VK_TRUE, UINT64_MAX);   // leave SIGNALED
    // the view and descriptor are unchanged -- no vkUpdateDescriptorSets, no queue-wide stall
    return true;
  }

  bool VulkanBackend::updateTexture(std::int32_t index, std::uint32_t const* rgba,
                                   std::uint32_t width, std::uint32_t height)
  {
    if (!_ready || !_tt_dset || !rgba || !width || !height) return false;
    if (index < 0 || static_cast<std::size_t>(index) >= _textures.size()) return false;

    // Fast path: same extent, single mip -> write into the image we already have.
    if (updateTexturePixelsInPlace(index, rgba, width, height))
      return true;

    // One mip is enough here: the cloud deck is sampled at roughly texel scale and never minified
    // hard enough to shimmer, and a full chain would be rebuilt ten times a second for nothing.
    std::vector<std::vector<std::uint8_t>> mips;
    mips.emplace_back(reinterpret_cast<std::uint8_t const*>(rgba),
                      reinterpret_cast<std::uint8_t const*>(rgba)
                        + static_cast<std::size_t>(width) * height * 4u);

    TexEntry e;
    if (!uploadImage2D(width, height, VK_FORMAT_R8G8B8A8_UNORM, 4, mips, e.image, e.mem, e.view))
    {
      if (e.view) vkDestroyImageView(_device, e.view, nullptr);
      if (e.image) vkDestroyImage(_device, e.image, nullptr);
      if (e.mem) vkFreeMemory(_device, e.mem, nullptr);
      return false;
    }

    e.w = width; e.h = height; e.levels = 1u;
    // [spike fix] No stall: the bindless array is UPDATE_AFTER_BIND, so this descriptor write is
    // legal while in-flight command buffers still reference the set. This used to be a FULL GPU
    // stall on every streamed texture -- the periodic hitch while flying.
    TexEntry& slot = _textures[static_cast<std::size_t>(index)];
    if (slot.view) vkDestroyImageView(_device, slot.view, nullptr);
    if (slot.image) vkDestroyImage(_device, slot.image, nullptr);
    if (slot.mem) vkFreeMemory(_device, slot.mem, nullptr);
    slot = e;

    VkDescriptorImageInfo dii{ _tile_sampler, slot.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wd{};
    wd.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wd.dstSet = _tt_dset;
    wd.dstBinding = 5;
    wd.dstArrayElement = static_cast<std::uint32_t>(index);
    wd.descriptorCount = 1;
    wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wd.pImageInfo = &dii;
    vkUpdateDescriptorSets(_device, 1, &wd, 0, nullptr);
    return true;
  }

  std::int32_t VulkanBackend::addTextureMips(std::vector<std::vector<std::uint8_t>> const& rgba_mips,
                                             std::uint32_t width, std::uint32_t height)
  {
    if (!_ready || !_tt_dset || rgba_mips.empty() || !width || !height) return -1;
    if (_textures.size() >= kMaxTextures) return -1;
    TexEntry e;
    e.w = width; e.h = height; e.levels = static_cast<std::uint32_t>(rgba_mips.size());
    if (!uploadImage2D(width, height, VK_FORMAT_R8G8B8A8_UNORM, 4, rgba_mips, e.image, e.mem, e.view))
    {
      if (e.view) vkDestroyImageView(_device, e.view, nullptr);
      if (e.image) vkDestroyImage(_device, e.image, nullptr);
      if (e.mem) vkFreeMemory(_device, e.mem, nullptr);
      return -1;
    }
    std::int32_t const idx = static_cast<std::int32_t>(_textures.size());
    _textures.push_back(e);
    VkDescriptorImageInfo dii{ _tile_sampler, e.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wd{};
    wd.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wd.dstSet = _tt_dset;
    wd.dstBinding = 5;
    wd.dstArrayElement = static_cast<std::uint32_t>(idx);
    wd.descriptorCount = 1;
    wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wd.pImageInfo = &dii;
    // [spike fix] No stall: the bindless array is UPDATE_AFTER_BIND, so this descriptor write is
    // legal while in-flight command buffers still reference the set. This used to be a FULL GPU
    // stall on every streamed texture -- the periodic hitch while flying.
    vkUpdateDescriptorSets(_device, 1, &wd, 0, nullptr);
    return idx;
  }

  std::int32_t VulkanBackend::addTextureCompressed(VkFormat format, std::uint32_t width, std::uint32_t height,
                                                   std::vector<std::vector<std::uint8_t>> const& mips)
  {
    if (!_ready || !_tt_dset || mips.empty() || !width || !height) return -1;
    if (_textures.size() >= kMaxTextures) return -1;
    TexEntry e;
    e.w = width; e.h = height; e.levels = static_cast<std::uint32_t>(mips.size());
    if (!uploadImage2D(width, height, format, 0, mips, e.image, e.mem, e.view))
    {
      if (e.view) vkDestroyImageView(_device, e.view, nullptr);
      if (e.image) vkDestroyImage(_device, e.image, nullptr);
      if (e.mem) vkFreeMemory(_device, e.mem, nullptr);
      return -1;
    }
    std::int32_t const idx = static_cast<std::int32_t>(_textures.size());
    _textures.push_back(e);
    VkDescriptorImageInfo dii{ _tile_sampler, e.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wd{};
    wd.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wd.dstSet = _tt_dset;
    wd.dstBinding = 5;
    wd.dstArrayElement = static_cast<std::uint32_t>(idx);
    wd.descriptorCount = 1;
    wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wd.pImageInfo = &dii;
    // [spike fix] No stall: the bindless array is UPDATE_AFTER_BIND, so this descriptor write is
    // legal while in-flight command buffers still reference the set. This used to be a FULL GPU
    // stall on every streamed texture -- the periodic hitch while flying.
    vkUpdateDescriptorSets(_device, 1, &wd, 0, nullptr);
    return idx;
  }

  bool VulkanBackend::setTerrainTextured(float const* pn_verts, std::size_t vertex_count,
                                         std::uint32_t const* chunk_index_per_vertex,
                                         std::uint32_t const* indices, std::size_t index_count,
                                         TerrainChunk const* chunks, std::size_t chunk_count,
                                         std::uint8_t const* alpha_rgba8, std::uint8_t const* shadow_r8,
                                         std::uint32_t atlas_w, std::uint32_t atlas_h,
                                         std::uint32_t dirty_row_lo, std::uint32_t dirty_row_hi)
  {
    SlowCall _sc_setTerrainTextured("setTerrainTextured");
    if (!_ready || !_tt_dset || !_tt_pipeline || !vertex_count || !index_count || !chunk_count)
      return false;
    _tt_ready = false;
    if (!setTerrainMesh(pn_verts, vertex_count, indices, index_count))
      return false;
    // [spike fix] No stall: the bindless array is UPDATE_AFTER_BIND, so this descriptor write is
    // legal while in-flight command buffers still reference the set. This used to be a FULL GPU
    // stall on every streamed texture -- the periodic hitch while flying.
    if (_chunk_ssbo) { vkDestroyBuffer(_device, _chunk_ssbo, nullptr); _chunk_ssbo = VK_NULL_HANDLE; }
    if (_chunk_ssbo_mem) { vkFreeMemory(_device, _chunk_ssbo_mem, nullptr); _chunk_ssbo_mem = VK_NULL_HANDLE; }
    // NOTE: the atlas images are deliberately NOT destroyed here any more -- see the reuse check
    // further down. Recreating them meant a 194 MB + 48 MB vkAllocateMemory pair on every tile
    // change, which is most of the 144 ms this function used to cost.


    // cidx is slot-addressed like the vertices, so it is persistent and range-written too.
    if (!writeHostBufferRanges(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, chunk_index_per_vertex,
                               vertex_count * 4, _terrain_dirty_c.data(), _terrain_dirty_c.size(),
                               _tt_cidx, _tt_cidx_mem, _tt_cidx_cap)
        || !createHostBuffer(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, chunk_count * sizeof(TerrainChunk), _chunk_ssbo, _chunk_ssbo_mem, chunks))
    {
      LogError << "[VK] textured terrain buffers failed" << std::endl;
      return false;
    }
    // [SPIKE FIX -- STABLE SLOTS] The caller hands us the atlas ALREADY LAID OUT, so the per-chunk
    // repack that used to happen here (a ~193 MB memcpy on every tile crossing, for the ~4% of the
    // neighbourhood that actually changed) is gone. Only the rows a newly resident tile touched are
    // copied to the GPU.
    std::uint32_t const aw = atlas_w, ah = atlas_h;
    bool const atlas_same = (aw == _atlas_w && ah == _atlas_h && _alpha_image && _shadow_image);
    if (atlas_same)
    {
      // Upload each repacked tile's OWN band. Falling back to the min..max span when no bands were
      // given keeps the old behaviour for callers that do not set them.
      std::vector<std::pair<std::uint32_t, std::uint32_t>> bands = _atlas_dirty_bands;
      if (bands.empty() && dirty_row_hi >= dirty_row_lo)
        bands.emplace_back(dirty_row_lo, dirty_row_hi);
      for (auto const& band : bands)
      {
        if (band.second < band.first) continue;
        std::uint32_t const y0 = band.first * 64u;
        if (y0 >= ah) continue;
        std::uint32_t const rows_px = std::min(ah - y0, (band.second - band.first + 1u) * 64u);
        if (!uploadIntoImageRows(_alpha_image, aw, y0, rows_px,
                                 alpha_rgba8 + static_cast<std::size_t>(y0) * aw * 4u,
                                 static_cast<std::size_t>(rows_px) * aw * 4u)
            || !uploadIntoImageRows(_shadow_image, aw, y0, rows_px,
                                    shadow_r8 + static_cast<std::size_t>(y0) * aw,
                                    static_cast<std::size_t>(rows_px) * aw))
        {
          LogError << "[VK] textured terrain atlas row upload failed" << std::endl;
          return false;
        }
      }
    }
    else
    {
      static std::vector<std::vector<std::uint8_t>> amip(1), smip(1);
      amip[0].assign(alpha_rgba8, alpha_rgba8 + static_cast<std::size_t>(aw) * ah * 4u);
      smip[0].assign(shadow_r8, shadow_r8 + static_cast<std::size_t>(aw) * ah);
      // [2026-09-02] These images and their memory are referenced by _tt_dset, which command
      // buffers ALREADY SUBMITTED are bound to. Destroying and freeing them below without waiting is
      // a GPU use-after-free -- this function had no synchronisation of any kind. It is driven by
      // tileset streaming (log: 13 -> 187 -> ... -> 410 tilesets, 23 full rebuilds in one session),
      // so it fires exactly when the camera moves: "the moment I start moving it flashes and all my
      // doodads disappear, anti aliasing breaks".
      //
      // Drain the submit thread first -- it hands work to the queue independently of this thread, so
      // vkDeviceWaitIdle alone would still race a submission already in flight.
      {
        std::unique_lock<std::mutex> lk(_submit_mutex);
        _submit_done_cv.wait(lk, [this] { return !_submit_pending; });
      }
      if (_device && vkDeviceWaitIdle)
        vkDeviceWaitIdle(_device);

      if (_alpha_view) vkDestroyImageView(_device, _alpha_view, nullptr);
      if (_alpha_image) vkDestroyImage(_device, _alpha_image, nullptr);
      if (_alpha_mem) vkFreeMemory(_device, _alpha_mem, nullptr);
      if (_shadow_view) vkDestroyImageView(_device, _shadow_view, nullptr);
      if (_shadow_image) vkDestroyImage(_device, _shadow_image, nullptr);
      if (_shadow_mem) vkFreeMemory(_device, _shadow_mem, nullptr);
      _alpha_view = VK_NULL_HANDLE; _alpha_image = VK_NULL_HANDLE; _alpha_mem = VK_NULL_HANDLE;
      _shadow_view = VK_NULL_HANDLE; _shadow_image = VK_NULL_HANDLE; _shadow_mem = VK_NULL_HANDLE;
      if (!uploadImage2D(aw, ah, VK_FORMAT_R8G8B8A8_UNORM, 4, amip, _alpha_image, _alpha_mem, _alpha_view)
          || !uploadImage2D(aw, ah, VK_FORMAT_R8_UNORM, 1, smip, _shadow_image, _shadow_mem, _shadow_view))
      {
        LogError << "[VK] textured terrain atlases failed" << std::endl;
        return false;
      }
      _atlas_w = aw;
      _atlas_h = ah;
    }

    VkDescriptorImageInfo aii{ _atlas_sampler, _alpha_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo sii{ _atlas_sampler, _shadow_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorBufferInfo cbi{ _chunk_ssbo, 0, chunk_count * sizeof(TerrainChunk) };
    VkWriteDescriptorSet w[3]{};
    for (auto& x : w) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.dstSet = _tt_dset; x.descriptorCount = 1; }
    w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &aii;
    w[1].dstBinding = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &sii;
    w[2].dstBinding = 2; w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &cbi;
    vkUpdateDescriptorSets(_device, 3, w, 0, nullptr);
    _tt_atlas_w = aw; _tt_atlas_h = ah; _tt_chunk_count = static_cast<std::uint32_t>(chunk_count);
    _terrain_dirty_v.clear(); _terrain_dirty_i.clear(); _terrain_dirty_c.clear();
    _atlas_dirty_bands.clear();
    _tt_ready = true;
    g_vk_trace_last_rebuild_frame = g_vk_trace_frame;   // [VKTRACE]
    LogError << "[VK] TEXTURED terrain: " << chunk_count << " chunks, " << _textures.size() << " tilesets, atlas "
             << aw << "x" << ah << std::endl;
    return true;
  }

  void VulkanBackend::setLighting(void const* block, std::size_t bytes, float const* extra_4vec4)
  {
    if (!_light_ubo_map || !block) return;
    std::size_t const n = std::min<std::size_t>(bytes, 4096 - 48);
    std::memcpy(_light_ubo_map, block, n);
    if (extra_4vec4)
      std::memcpy(static_cast<std::uint8_t*>(_light_ubo_map) + n, extra_4vec4, 112);
    // The sky dome is authored around the origin and translated onto the camera in its vertex stage
    // (GL does the same with a `camera_pos` uniform); extra_4vec4[0..2] is that position.
    if (extra_4vec4)
    {
      _sky_camera[0] = extra_4vec4[0];
      _sky_camera[1] = extra_4vec4[1];
      _sky_camera[2] = extra_4vec4[2];
    }
    static unsigned s_tick = 0;
    if ((s_tick++ % 120u) == 0 && extra_4vec4)
    {
      float const* f = static_cast<float const*>(block);
      LogError << "[VK] lighting: bytes=" << bytes << " fogStartFrac=" << f[3] << " fogEnd=" << f[7]
               << " fogOn=" << f[11] << " fogRate=" << f[15] << " cam=(" << extra_4vec4[0] << "," << extra_4vec4[1]
               << "," << extra_4vec4[2] << ") toggles=(" << extra_4vec4[8] << "," << extra_4vec4[9] << ")"
               // ENTITY fog: M2s fog with these, terrain uses the zone slots above -- so terrain parity
               // never exercised them. f[164..167] EnvFogColor_On, f[168..171] EnvFogDist.
               // water bands: OceanColorLight/Dark and RiverColorLight/Dark are f[16..31]; their .a
               // is the water opacity the liquid shader uses -- zero here means invisible water.
               << " oceanLa=" << f[19] << " oceanDa=" << f[23]
               << " riverLa=" << f[27] << " riverDa=" << f[31]
               << " envOn=" << f[167] << " envRGB=(" << f[164] << "," << f[165] << "," << f[166]
               << ") envStartFrac=" << f[168] << " envEnd=" << f[169] << std::endl;
    }
  }

  bool VulkanBackend::createTerrainTexPipeline()
  {
    VkShaderModule vs = loadShaderModule("terrain_tex.vert.spv");
    VkShaderModule fs = loadShaderModule("terrain_tex.frag.spv");
    if (!vs || !fs)
    {
      if (vs) vkDestroyShaderModule(_device, vs, nullptr);
      if (fs) vkDestroyShaderModule(_device, fs, nullptr);
      return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkVertexInputBindingDescription binds[2]{};
    binds[0] = { 0, 36, VK_VERTEX_INPUT_RATE_VERTEX };
    binds[1] = { 1, 4, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 };
    attrs[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, 24 };
    attrs[3] = { 3, 1, VK_FORMAT_R32_UINT, 0 };
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 2;
    vin.pVertexBindingDescriptions = binds;
    vin.vertexAttributeDescriptionCount = 4;
    vin.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{};
    vp.width = static_cast<float>(_width);
    vp.height = static_cast<float>(_height);
    vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.extent = { _width, _height };
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = _samples; // [MSAA] every pipeline renders at the scene sample count
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba[2]{};
    cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 2;
    cb.pAttachments = cba;
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = 80;
    VkDescriptorSetLayout layouts[2] = { _dsl, _tt_dsl };
    VkPipelineLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &push;
    lci.setLayoutCount = 2;
    lci.pSetLayouts = layouts;
    if (vkCreatePipelineLayout(_device, &lci, nullptr, &_tt_layout) != VK_SUCCESS)
    {
      vkDestroyShaderModule(_device, vs, nullptr);
      vkDestroyShaderModule(_device, fs, nullptr);
      return false;
    }
    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vin;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vps;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDepthStencilState = &ds;
    pci.layout = _tt_layout;
    pci.renderPass = _render_pass;
    VkResult const r = vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci, nullptr, &_tt_pipeline);
    vkDestroyShaderModule(_device, vs, nullptr);
    vkDestroyShaderModule(_device, fs, nullptr);
    if (r != VK_SUCCESS)
    {
      LogError << "[VK] textured terrain pipeline create failed: " << vkres(r) << std::endl;
      _tt_pipeline = VK_NULL_HANDLE;
      return false;
    }
    LogError << "[VK] textured terrain pipeline ready" << std::endl;
    return true;
  }


  // =====================================================================================================
  // [VULKAN phase C, 2026-08-29] M2 / DOODAD BATCHES -- the Vulkan twin of WorldRender::drawDynamicBatched.
  // Same arena data, same per-instance streams, same bone addressing; drawn with vkCmdDrawIndexedIndirect.
  // =====================================================================================================

  bool VulkanBackend::textureBatchEnabled()
  {
    // Batching is OPT-IN until the first-frame hang it causes is understood. With it off, each
    // upload still uses the shared staging arena (so the per-texture vkCreateBuffer/vkAllocateMemory
    // /destroy churn is gone) but is submitted immediately, which is the known-good ordering.
    static bool const on = []() {
      char const* v = std::getenv("NOGGIT_VK_TEXBATCH");
      return !(v && *v == '0');   // ON unless explicitly disabled
    }();
    return on;
  }

  bool VulkanBackend::beginTextureBatch()
  {
    if (_tex_batch_open)
      return true;
    if (!_copy_cmd || !_copy_fence)
      return false;
    vkWaitForFences(_device, 1, &_copy_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(_device, 1, &_copy_fence);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(_copy_cmd, &bi) != VK_SUCCESS)
      return false;
    _tex_batch_open = true;
    _tex_arena_used = 0;
    return true;
  }

  bool VulkanBackend::uploadIntoImageRows(VkImage image, std::uint32_t width, std::uint32_t y0,
                                          std::uint32_t rows, void const* data, std::size_t bytes)
  {
    if (!image || !data || !bytes || !rows)
      return false;
    std::size_t const need = (_tex_batch_open ? _tex_arena_used : 0u) + bytes;
    if (need > _tex_arena_cap)
    {
      flushTextureUploads();
      std::size_t const want = need + need / 4 + 4096;
      retireBuffer(_tex_arena, _tex_arena_mem);
      _tex_arena = VK_NULL_HANDLE;
      _tex_arena_mem = VK_NULL_HANDLE;
      _tex_arena_cap = 0;
      _tex_arena_used = 0;
      if (!createHostBuffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, want, _tex_arena, _tex_arena_mem, nullptr))
        return false;
      _tex_arena_cap = want;
    }
    if (!beginTextureBatch())
      return false;
    void* const arena_ptr = mapPersistent(_tex_arena_mem);
    if (!arena_ptr)
      return false;
    std::size_t const off = _tex_arena_used;
    std::memcpy(static_cast<std::uint8_t*>(arena_ptr) + off, data, bytes);
    _tex_arena_used += bytes;

    VkImageMemoryBarrier ib{};
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = image;
    ib.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    ib.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(_copy_cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);

    VkBufferImageCopy r{};
    r.bufferOffset = off;
    r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageOffset = { 0, static_cast<std::int32_t>(y0), 0 };
    r.imageExtent = { width, rows, 1u };
    vkCmdCopyBufferToImage(_copy_cmd, _tex_arena, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);

    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(_copy_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
    return true;
  }

  bool VulkanBackend::uploadIntoImage(VkImage image, std::uint32_t width, std::uint32_t height,
                                      void const* data, std::size_t bytes)
  {
    // Copy straight into an image we already own -- no vkCreateImage, no vkAllocateMemory, no view
    // rebuild, no descriptor rewrite. Rides the same per-frame batch as the texture uploads.
    if (!image || !data || !bytes)
      return false;
    std::size_t const need = (_tex_batch_open ? _tex_arena_used : 0u) + bytes;
    if (need > _tex_arena_cap)
    {
      flushTextureUploads();
      std::size_t const want = need + need / 4 + 4096;
      retireBuffer(_tex_arena, _tex_arena_mem);
      _tex_arena = VK_NULL_HANDLE;
      _tex_arena_mem = VK_NULL_HANDLE;
      _tex_arena_cap = 0;
      _tex_arena_used = 0;
      if (!createHostBuffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, want, _tex_arena, _tex_arena_mem, nullptr))
        return false;
      _tex_arena_cap = want;
    }
    if (!beginTextureBatch())
      return false;
    void* const arena_ptr = mapPersistent(_tex_arena_mem);
    if (!arena_ptr)
      return false;
    std::size_t const off = _tex_arena_used;
    std::memcpy(static_cast<std::uint8_t*>(arena_ptr) + off, data, bytes);
    _tex_arena_used += bytes;

    VkImageMemoryBarrier ib{};
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = image;
    ib.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    ib.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(_copy_cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);

    VkBufferImageCopy r{};
    r.bufferOffset = off;
    r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageExtent = { width, height, 1u };
    vkCmdCopyBufferToImage(_copy_cmd, _tex_arena, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);

    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(_copy_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
    return true;
  }

  void VulkanBackend::flushTextureUploads()
  {
    SlowCall _sc_flushTextureUploads("flushTextureUploads");
    if (!_tex_batch_open)
      return;
    _tex_batch_open = false;
    vkEndCommandBuffer(_copy_cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &_copy_cmd;
    // Signal the graphics submit rather than blocking the CPU here. If a previous signal has not
    // been consumed yet (no frame submitted in between) fall back to the CPU wait, because
    // signalling an already-signalled binary semaphore is illegal.
    static bool const s_copy_sem = std::getenv("NOGGIT_VK_COPY_SEM") != nullptr;
    if (s_copy_sem && _copy_done && !_copy_signalled)
    {
      si.signalSemaphoreCount = 1;
      si.pSignalSemaphores = &_copy_done;
      if (vkQueueSubmit(_copy_queue, 1, &si, _copy_fence) != VK_SUCCESS)
        return;
      _copy_signalled = true;
      return;   // no CPU wait: the frame's submit waits on _copy_done
    }
    if (vkQueueSubmit(_copy_queue, 1, &si, _copy_fence) != VK_SUCCESS)
      return;
    vkWaitForFences(_device, 1, &_copy_fence, VK_TRUE, UINT64_MAX);
  }

  void VulkanBackend::retireBuffer(VkBuffer buf, VkDeviceMemory mem)
  {
    if (!buf && !mem)
      return;
    unmapPersistent(mem);
    _retired.push_back({ buf, mem, 3 });
  }

  void VulkanBackend::tickRetired()
  {
    for (std::size_t i = 0; i < _retired.size();)
    {
      if (--_retired[i].frames_left <= 0)
      {
        if (_retired[i].buf) vkDestroyBuffer(_device, _retired[i].buf, nullptr);
        if (_retired[i].mem) vkFreeMemory(_device, _retired[i].mem, nullptr);
        _retired[i] = _retired.back();
        _retired.pop_back();
      }
      else
      {
        ++i;
      }
    }
  }

  void* VulkanBackend::mapPersistent(VkDeviceMemory mem)
  {
    if (!mem)
      return nullptr;
    auto const it = _persistent_maps.find(mem);
    if (it != _persistent_maps.end())
      return it->second;
    void* p = nullptr;
    // VK_WHOLE_SIZE: the mapping must stay valid for any write up to the allocation's capacity
    if (vkMapMemory(_device, mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
      return nullptr;
    _persistent_maps.emplace(mem, p);
    return p;
  }

  void VulkanBackend::unmapPersistent(VkDeviceMemory mem)
  {
    auto const it = _persistent_maps.find(mem);
    if (it == _persistent_maps.end())
      return;
    vkUnmapMemory(_device, mem);
    _persistent_maps.erase(it);
  }

  bool VulkanBackend::writeHostBuffer(VkBufferUsageFlags usage, void const* data, std::size_t bytes,
                                      VkBuffer& buf, VkDeviceMemory& mem, std::size_t& cap)
  {
    if (!bytes)
      return true;
    if (bytes > cap)
    {
      // No stall: the old allocation is retired and freed a few frames on, once nothing in flight
      // can still be reading it. This used to vkQueueWaitIdle on every growth.
      retireBuffer(buf, mem);
      buf = VK_NULL_HANDLE;
      mem = VK_NULL_HANDLE;
      std::size_t const want = bytes + bytes / 2 + 4096; // headroom so streams stop reallocating
      // allocate `want` (growth headroom) but copy only the `bytes` that are actually valid
      if (!createHostBuffer(usage, want, buf, mem, data, bytes))
      {
        cap = 0;
        return false;
      }
      cap = want;
      return true;
    }
    void* const mapped = mapPersistent(mem);
    if (!mapped)
      return false;
    std::memcpy(mapped, data, bytes);
    return true;
  }

  bool VulkanBackend::writeHostBufferRanges(VkBufferUsageFlags usage, void const* data,
                                            std::size_t total_bytes,
                                            std::pair<std::size_t, std::size_t> const* ranges,
                                            std::size_t range_count,
                                            VkBuffer& buf, VkDeviceMemory& mem, std::size_t& cap)
  {
    if (!total_bytes)
      return true;
    // No ranges given, or the buffer has to grow (its contents would be undefined): full write.
    if (!ranges || !range_count || total_bytes > cap)
      return writeHostBuffer(usage, data, total_bytes, buf, mem, cap);

    void* const mapped = mapPersistent(mem);
    if (!mapped)
      return false;
    auto const* src = static_cast<std::uint8_t const*>(data);
    auto* dst = static_cast<std::uint8_t*>(mapped);
    for (std::size_t r = 0; r < range_count; ++r)
    {
      std::size_t const off = ranges[r].first;
      std::size_t const len = ranges[r].second;
      if (!len || off >= total_bytes)
        continue;
      std::memcpy(dst + off, src + off, std::min(len, total_bytes - off));
    }
    return true;
  }

  bool VulkanBackend::setM2Arena(void const* model_vertices, std::size_t vertex_bytes,
                                 std::uint16_t const* indices, std::size_t index_count)
  {
    SlowCall _sc_setM2Arena("setM2Arena");
    if (!_ready || !_m2_pipeline || !model_vertices || !vertex_bytes || !indices || !index_count)
      return false;
    // [finding 105] append-only arena: upload the tail, not all of it (same as 103/104)
    std::size_t const m2_index_bytes = index_count * sizeof(std::uint16_t);
    std::pair<std::size_t, std::size_t> m2vr{ _m2_vbo_written,
                                              vertex_bytes > _m2_vbo_written ? vertex_bytes - _m2_vbo_written : 0 };
    std::pair<std::size_t, std::size_t> m2ir{ _m2_ibo_written,
                                              m2_index_bytes > _m2_ibo_written ? m2_index_bytes - _m2_ibo_written : 0 };
    bool const m2v_app = vertex_bytes >= _m2_vbo_written && m2vr.second;
    bool const m2i_app = m2_index_bytes >= _m2_ibo_written && m2ir.second;
    if (!writeHostBufferRanges(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, model_vertices, vertex_bytes,
                               m2v_app ? &m2vr : nullptr, m2v_app ? 1u : 0u,
                               _m2_vbo, _m2_vbo_mem, _m2_vbo_cap)
        || !writeHostBufferRanges(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices, m2_index_bytes,
                                  m2i_app ? &m2ir : nullptr, m2i_app ? 1u : 0u,
                                  _m2_ibo, _m2_ibo_mem, _m2_ibo_cap))
    {
      LogError << "[VK] M2 arena upload failed" << std::endl;
      _m2_vbo_written = _m2_ibo_written = 0;   // force a full write next time
      return false;
    }
    _m2_vbo_written = vertex_bytes;
    _m2_ibo_written = m2_index_bytes;
    _m2_index_count = static_cast<std::uint32_t>(index_count);
    LogError << "[VK] M2 arena: " << (vertex_bytes / 48) << " verts / " << index_count << " indices" << std::endl;
    return true;
  }

  bool VulkanBackend::setM2Frame(float const* transforms, float const* interiors, std::int32_t const* tex_info,
                                 std::int32_t const* state_info,
                                 std::int32_t const* group_info, std::size_t group_count,
                                 float slice_dist,
                                 std::size_t instance_count,
                                 float const* bone_mat4s, std::size_t bone_count,
                                 M2Draw const* draws, std::size_t draw_count)
  {
    SlowCall _sc_setM2Frame("setM2Frame");
    _m2_draw_count = 0;
    if (!_ready || !_m2_pipeline || !instance_count || !draw_count || !state_info)
      return false;
    // one identity matrix keeps the SSBO valid when every batch is static (bone_count == 0)
    static float const s_identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    float const* bones = bone_count ? bone_mat4s : s_identity;
    std::size_t const bone_bytes = (bone_count ? bone_count : 1) * 64;

    if (!writeHostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, transforms, instance_count * 64,
                         _m2_inst_tf, _m2_inst_tf_mem, _m2_inst_tf_cap)
        || !writeHostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, interiors, instance_count * 16,
                            _m2_inst_in, _m2_inst_in_mem, _m2_inst_in_cap)
        || !writeHostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, tex_info, instance_count * 16,
                            _m2_inst_tx, _m2_inst_tx_mem, _m2_inst_tx_cap)
        || !writeHostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, state_info, instance_count * 16,
                            _m2_inst_st, _m2_inst_st_mem, _m2_inst_st_cap)
        || !writeHostBuffer(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, bones, bone_bytes,
                            _m2_bones, _m2_bones_mem, _m2_bones_cap)
        || !writeHostBuffer(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, draws, draw_count * sizeof(M2Draw),
                            _m2_indirect, _m2_indirect_mem, _m2_indirect_cap))
    {
      LogError << "[VK] M2 frame streams failed" << std::endl;
      return false;
    }
    // Point the bone SSBO descriptor at the buffer -- but ONLY when the buffer actually changed.
    // writeHostBuffer is grow-only, so the handle is stable across almost every frame; rewriting the
    // descriptor unconditionally meant a vkQueueWaitIdle (a FULL GPU STALL) on every single frame,
    // which is most of why the Vulkan path measured slower than GL. VK_WHOLE_SIZE keeps the binding
    // valid as the bone block grows within the same allocation.
    if (_m2_bones != _m2_bones_bound)
    {
      VkDescriptorBufferInfo bbi{ _m2_bones, 0, VK_WHOLE_SIZE };
      VkWriteDescriptorSet w{};
      w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w.dstSet = _tt_dset;
      w.dstBinding = 4;
      w.descriptorCount = 1;
      w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w.pBufferInfo = &bbi;
      vkQueueWaitIdle(_queue);   // the set may still be read by the in-flight frame
      vkUpdateDescriptorSets(_device, 1, &w, 0, nullptr);
      _m2_bones_bound = _m2_bones;
    }
    _m2_draw_count = static_cast<std::uint32_t>(draw_count);
    _m2_slice_dist = slice_dist;
    _m2_groups.clear();
    if (group_info && group_count)
      _m2_groups.assign(group_info, group_info + group_count * 4);
    return true;
  }


  // ---- [VULKAN CLUTTER PERSISTENT, 2026-09-03] --------------------------------------------------

  std::int32_t VulkanBackend::clutterRegisterChunk(float const* tf, float const* interior,
                                                   std::int32_t const* tex, std::int32_t const* state,
                                                   std::size_t n)
  {
    if (!_ready || !tf || !interior || !tex || !state || !n)
      return -1;
    ClutterChunkBuf c;
    c.count = static_cast<std::uint32_t>(n);
    c.off_in = static_cast<VkDeviceSize>(n) * 64u;
    c.off_tx = c.off_in + static_cast<VkDeviceSize>(n) * 16u;
    c.off_st = c.off_tx + static_cast<VkDeviceSize>(n) * 16u;
    VkDeviceSize const bytes = c.off_st + static_cast<VkDeviceSize>(n) * 16u;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(_device, &bci, nullptr, &c.buf) != VK_SUCCESS)
      return -1;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(_device, c.buf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(_phys, &mp);
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((req.memoryTypeBits & (1u << i))
          && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
          && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
      {
        type = i;
        break;
      }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    void* p = nullptr;
    if (type == UINT32_MAX || vkAllocateMemory(_device, &mai, nullptr, &c.mem) != VK_SUCCESS
        || vkBindBufferMemory(_device, c.buf, c.mem, 0) != VK_SUCCESS
        || vkMapMemory(_device, c.mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS || !p)
    {
      if (c.mem) vkFreeMemory(_device, c.mem, nullptr);
      vkDestroyBuffer(_device, c.buf, nullptr);
      return -1;
    }
    std::memcpy(static_cast<char*>(p),             tf,       n * 64u);
    std::memcpy(static_cast<char*>(p) + c.off_in,  interior, n * 16u);
    std::memcpy(static_cast<char*>(p) + c.off_tx,  tex,      n * 16u);
    std::memcpy(static_cast<char*>(p) + c.off_st,  state,    n * 16u);
    vkUnmapMemory(_device, c.mem);
    c.used = true;

    for (std::size_t i = 0; i < _clutter_chunks.size(); ++i)
      if (!_clutter_chunks[i].used)
      {
        _clutter_chunks[i] = c;
        return static_cast<std::int32_t>(i);
      }
    _clutter_chunks.push_back(c);
    return static_cast<std::int32_t>(_clutter_chunks.size() - 1u);
  }

  void VulkanBackend::clutterReleaseChunk(std::int32_t slot)
  {
    if (slot < 0 || static_cast<std::size_t>(slot) >= _clutter_chunks.size())
      return;
    ClutterChunkBuf& c = _clutter_chunks[static_cast<std::size_t>(slot)];
    if (!c.used)
      return;
    retireBuffer(c.buf, c.mem);   // frame-in-flight safe (same rule as every grow)
    c = ClutterChunkBuf{};
  }

  bool VulkanBackend::setClutterFrame(ClutterDraw const* draws, std::size_t count)
  {
    _clutter_draws.clear();
    if (!_ready || !draws || !count)
      return true;   // nothing visible is not an error
    _clutter_draws.assign(draws, draws + count);
    // one indirect record per draw, in the given order -- runs are issued as contiguous spans
    static std::vector<M2Draw> cmds;   // scratch; single-threaded feed path
    cmds.clear();
    cmds.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
      M2Draw d;
      d.index_count = draws[i].index_count;
      d.instance_count = draws[i].instance_count;
      d.first_index = draws[i].first_index;
      d.vertex_offset = draws[i].base_vertex;
      d.first_instance = draws[i].first_instance;
      cmds.push_back(d);
    }
    if (!writeHostBuffer(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, cmds.data(), cmds.size() * sizeof(M2Draw),
                         _clutter_indirect, _clutter_indirect_mem, _clutter_indirect_cap))
    {
      _clutter_draws.clear();
      return false;
    }
    return true;
  }

  // ================= [VULKAN phase D] WMO pass =================
  //
  // Geometry is one append-only arena (stride 56) mirrored out of WMOGroupRender::upload(); the
  // per-frame draw list replays exactly the runs GL emitted, so per-batch culling is already done.
  // Transforms live in their own descriptor set because set 1's binding 5 is the variable-count
  // bindless array, which Vulkan requires to be the highest binding in its set.

  bool VulkanBackend::setWmoArena(void const* vertices, std::size_t vertex_bytes,
                                  std::uint16_t const* indices, std::size_t index_count)
  {
    SlowCall _sc_setWmoArena("setWmoArena");
    if (!_ready || !_wmo_pipeline || !vertices || !vertex_bytes || !indices || !index_count)
      return false;
    // [finding 103] The arena only ever grows at the tail, so upload the appended bytes, not all
    // ~40 MB of it. A shrink (or any non-append change) falls back to a full write, as does a
    // capacity grow inside writeHostBufferRanges.
    std::size_t const index_bytes = index_count * sizeof(std::uint16_t);
    std::pair<std::size_t, std::size_t> vrange{ _wmo_vbo_written,
                                                vertex_bytes > _wmo_vbo_written
                                                  ? vertex_bytes - _wmo_vbo_written : 0 };
    std::pair<std::size_t, std::size_t> irange{ _wmo_ibo_written,
                                                index_bytes > _wmo_ibo_written
                                                  ? index_bytes - _wmo_ibo_written : 0 };
    bool const v_append = vertex_bytes >= _wmo_vbo_written && vrange.second;
    bool const i_append = index_bytes  >= _wmo_ibo_written && irange.second;
    if (!writeHostBufferRanges(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, vertex_bytes,
                               v_append ? &vrange : nullptr, v_append ? 1u : 0u,
                               _wmo_vbo, _wmo_vbo_mem, _wmo_vbo_cap)
        || !writeHostBufferRanges(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices, index_bytes,
                                  i_append ? &irange : nullptr, i_append ? 1u : 0u,
                                  _wmo_ibo, _wmo_ibo_mem, _wmo_ibo_cap))
    {
      LogError << "[VK] WMO arena upload failed" << std::endl;
      _wmo_vbo_written = _wmo_ibo_written = 0;   // force a full write next time
      return false;
    }
    _wmo_vbo_written = vertex_bytes;
    _wmo_ibo_written = index_bytes;
    _wmo_index_count = static_cast<std::uint32_t>(index_count);
    return true;
  }

  bool VulkanBackend::setWmoFrame(float const* transforms, float const* ambients, std::size_t transform_count,
                                  WmoDraw const* draws, std::size_t draw_count)
  {
    _wmo_draws.clear();
    if (!_ready || !_wmo_pipeline || !transforms || !transform_count || !draws || !draw_count)
      return true;   // nothing to draw this frame is not an error

    if (!writeHostBuffer(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, transforms, transform_count * 64,
                         _wmo_xforms, _wmo_xforms_mem, _wmo_xforms_cap))
    {
      LogError << "[VK] WMO transform upload failed" << std::endl;
      return false;
    }

    // 3 vec4 per instance: [0] ambient.rgb + fog mode, [1] fog colour + fog end, [2].x unified path
    if (ambients && !writeHostBuffer(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ambients, transform_count * 48,
                                     _wmo_amb, _wmo_amb_mem, _wmo_amb_cap))
    {
      LogError << "[VK] WMO ambient upload failed" << std::endl;
      return false;
    }

    if (_wmo_xforms != _wmo_xforms_bound || _wmo_amb != _wmo_amb_bound)
    {
      VkDescriptorBufferInfo bi{ _wmo_xforms, 0, VK_WHOLE_SIZE };
      VkDescriptorBufferInfo ai{ _wmo_amb, 0, VK_WHOLE_SIZE };
      VkWriteDescriptorSet w[2]{};
      w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[0].dstSet = _wmo_dset;
      w[0].dstBinding = 0;
      w[0].descriptorCount = 1;
      w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w[0].pBufferInfo = &bi;
      w[1] = w[0];
      w[1].dstBinding = 2;
      w[1].pBufferInfo = &ai;
      vkUpdateDescriptorSets(_device, _wmo_amb ? 2 : 1, w, 0, nullptr);
      _wmo_xforms_bound = _wmo_xforms;
      _wmo_amb_bound = _wmo_amb;
    }

    _wmo_draws.assign(draws, draws + draw_count);
    return true;
  }

  bool VulkanBackend::setWmoBatches(std::int32_t const* records, std::size_t batch_count)
  {
    SlowCall _sc_setWmoBatches("setWmoBatches");
    if (!_ready || !_wmo_pipeline || !records || !batch_count)
      return false;
    if (!writeHostBuffer(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, records, batch_count * 16,
                         _wmo_batches, _wmo_batches_mem, _wmo_batches_cap))
    {
      LogError << "[VK] WMO batch table upload failed" << std::endl;
      return false;
    }
    VkDescriptorBufferInfo bi{ _wmo_batches, 0, batch_count * 16 };
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = _wmo_dset;
    w.dstBinding = 1;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(_device, 1, &w, 0, nullptr);
    return true;
  }

  bool VulkanBackend::createWmoPipeline()
  {
    VkShaderModule vs = loadShaderModule("wmo.vert.spv");
    VkShaderModule fs = loadShaderModule("wmo.frag.spv");
    if (!vs || !fs)
    {
      if (vs) vkDestroyShaderModule(_device, vs, nullptr);
      if (fs) vkDestroyShaderModule(_device, fs, nullptr);
      return false;
    }

    // own set: one storage buffer of per-instance transforms
    VkDescriptorSetLayoutBinding xb[3]{};
    xb[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr };
    xb[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    xb[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr }; // MOHD ambient
    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 3;
    dlci.pBindings = xb;
    if (vkCreateDescriptorSetLayout(_device, &dlci, nullptr, &_wmo_dsl) != VK_SUCCESS)
      return false;
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 };
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(_device, &dpci, nullptr, &_wmo_pool) != VK_SUCCESS)
      return false;
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = _wmo_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &_wmo_dsl;
    if (vkAllocateDescriptorSets(_device, &dsai, &_wmo_dset) != VK_SUCCESS)
      return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{ 0, 64, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[6]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0 };  // pos
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,   12 };  // normal
    attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,      24 };  // uv0
    attrs[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT,      32 };  // uv1
    attrs[4] = { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 40 }; // MOCV colour
    attrs[5] = { 5, 0, VK_FORMAT_R32_UINT,            56 }; // batch id (GL's batch_mapping)
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 6;
    vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{};
    vp.width = static_cast<float>(_width);
    vp.height = static_cast<float>(_height);
    vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.extent = { _width, _height };
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = _samples; // [MSAA] every pipeline renders at the scene sample count
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba[2]{};
    cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;   // depth-as-colour never blends
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 2;
    cb.pAttachments = cba;

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = 96;   // mat4 + time + terms + slice + xform_index
    VkDescriptorSetLayout layouts[3] = { _dsl, _tt_dsl, _wmo_dsl };
    VkPipelineLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &push;
    lci.setLayoutCount = 3;
    lci.pSetLayouts = layouts;
    if (vkCreatePipelineLayout(_device, &lci, nullptr, &_wmo_layout) != VK_SUCCESS)
    {
      vkDestroyShaderModule(_device, vs, nullptr);
      vkDestroyShaderModule(_device, fs, nullptr);
      return false;
    }

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vin;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vps;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDepthStencilState = &ds;
    pci.layout = _wmo_layout;
    pci.renderPass = _render_pass;

    // Same 6 variants as the M2 pass. Front face CLOCKWISE: the clip conversion leaves y un-flipped,
    // so a GL front (CCW) face is CW in VK framebuffer space (proven on M2 -- cull-off made the
    // Elwynn cameras measurably worse).
    bool ok = true;
    for (int cls = 0; cls < 3 && ok; ++cls)
    {
      for (int cull = 0; cull < 2 && ok; ++cull)
      {
        cba[0].blendEnable = (cls == 0) ? VK_FALSE : VK_TRUE;
        cba[0].colorBlendOp = VK_BLEND_OP_ADD;
        cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
        if (cls == 1)
        {
          cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
          cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
          cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
          cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        }
        else
        {
          cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
          cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
          cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
          cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        }
        rs.cullMode = cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;

        VkPipeline pl = VK_NULL_HANDLE;
        VkResult const r = vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci, nullptr, &pl);
        if (r != VK_SUCCESS)
        {
          LogError << "[VK] WMO pipeline variant (blend " << cls << ", cull " << cull
                   << ") failed: " << vkres(r) << std::endl;
          ok = false;
          break;
        }
        _wmo_pipelines[cls * 2 + cull] = pl;

        // For TRUE OPAQUE (blend class 0) also build an ALPHA_TEST=0 variant. Identical state --
        // only the fragment shader's specialization constant differs, which removes the `discard`
        // and lets the hardware keep early-Z. blend_mode 1 (alpha key) lives in this class too and
        // must NOT use it, so the selection at record time keys on blend_mode == 0.
        if (cls == 0)
        {
          std::uint32_t const atest_off = 0u;
          VkSpecializationMapEntry sme{};
          sme.constantID = 0;
          sme.offset = 0;
          sme.size = sizeof(std::uint32_t);
          VkSpecializationInfo spec{};
          spec.mapEntryCount = 1;
          spec.pMapEntries = &sme;
          spec.dataSize = sizeof(std::uint32_t);
          spec.pData = &atest_off;

          VkPipelineShaderStageCreateInfo stages_ez[2] = { stages[0], stages[1] };
          stages_ez[1].pSpecializationInfo = &spec;   // stage 1 is the fragment stage
          VkGraphicsPipelineCreateInfo pci_ez = pci;
          pci_ez.pStages = stages_ez;

          VkPipeline pl_ez = VK_NULL_HANDLE;
          if (vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci_ez, nullptr, &pl_ez) == VK_SUCCESS)
          {
            _wmo_pipelines_earlyz[cull] = pl_ez;
          }
          else
          {
            LogError << "[VK] WMO early-Z variant (cull " << cull << ") failed -- falling back to the"
                     << " discarding pipeline for opaque batches" << std::endl;
          }
        }
      }
    }
    vkDestroyShaderModule(_device, vs, nullptr);
    vkDestroyShaderModule(_device, fs, nullptr);
    if (!ok)
    {
      for (VkPipeline& pl : _wmo_pipelines)
      {
        if (pl) vkDestroyPipeline(_device, pl, nullptr);
        pl = VK_NULL_HANDLE;
      }
      _wmo_pipeline = VK_NULL_HANDLE;
      return false;
    }
    _wmo_pipeline = _wmo_pipelines[0];
    LogError << "[VK] WMO pipelines ready (6 blend/cull variants)" << std::endl;
    return true;
  }

  bool VulkanBackend::createM2Pipeline()
  {
    VkShaderModule vs = loadShaderModule("m2.vert.spv");
    VkShaderModule fs = loadShaderModule("m2.frag.spv");
    if (!vs || !fs)
    {
      if (vs) vkDestroyShaderModule(_device, vs, nullptr);
      if (fs) vkDestroyShaderModule(_device, fs, nullptr);
      return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    // binding 0 = ModelVertex (stride 48, per-vertex); 1..3 = per-instance transform / interior / tex
    VkVertexInputBindingDescription binds[5]{};
    binds[0] = { 0, 48, VK_VERTEX_INPUT_RATE_VERTEX };
    binds[1] = { 1, 64, VK_VERTEX_INPUT_RATE_INSTANCE };
    binds[2] = { 2, 16, VK_VERTEX_INPUT_RATE_INSTANCE };
    binds[3] = { 3, 16, VK_VERTEX_INPUT_RATE_INSTANCE };
    binds[4] = { 4, 16, VK_VERTEX_INPUT_RATE_INSTANCE };   // per-batch state
    VkVertexInputAttributeDescription attrs[13]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };    // pos
    attrs[1] = { 1, 0, VK_FORMAT_R8G8B8A8_UINT,    12 };   // bone weights
    attrs[2] = { 2, 0, VK_FORMAT_R8G8B8A8_UINT,    16 };   // bone indices
    attrs[3] = { 3, 0, VK_FORMAT_R32G32B32_SFLOAT, 20 };   // normal
    attrs[4] = { 4, 0, VK_FORMAT_R32G32_SFLOAT,    32 };   // uv0
    attrs[5] = { 5, 0, VK_FORMAT_R32G32_SFLOAT,    40 };   // uv1
    for (std::uint32_t i = 0; i < 4; ++i)                  // mat4 = 4 consecutive vec4 locations
      attrs[6 + i] = { 6 + i, 1, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16 };
    attrs[10] = { 10, 2, VK_FORMAT_R32G32B32A32_SFLOAT, 0 }; // interior
    attrs[11] = { 11, 3, VK_FORMAT_R32G32B32A32_SINT,   0 }; // tex/bone info
    attrs[12] = { 12, 4, VK_FORMAT_R32G32B32A32_SINT,   0 }; // batch state
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 5;
    vin.pVertexBindingDescriptions = binds;
    vin.vertexAttributeDescriptionCount = 13;
    vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{};
    vp.width = static_cast<float>(_width);
    vp.height = static_cast<float>(_height);
    vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.extent = { _width, _height };
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;   // set per variant below
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = _samples; // [MSAA] every pipeline renders at the scene sample count
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba[2]{};
    cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 2;
    cb.pAttachments = cba;
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = 80;
    VkDescriptorSetLayout layouts[2] = { _dsl, _tt_dsl };
    VkPipelineLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &push;
    lci.setLayoutCount = 2;
    lci.pSetLayouts = layouts;
    if (vkCreatePipelineLayout(_device, &lci, nullptr, &_m2_layout) != VK_SUCCESS)
    {
      vkDestroyShaderModule(_device, vs, nullptr);
      vkDestroyShaderModule(_device, fs, nullptr);
      return false;
    }
    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vin;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vps;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDepthStencilState = &ds;
    pci.layout = _m2_layout;
    pci.renderPass = _render_pass;
    // One variant per (blend class, backface cull) -- the state GL switches between draw groups.
    // WINDING: our clip conversion leaves y un-flipped, so VK rasterises the image y-DOWN relative to
    // GL window space and a GL front (CCW) face is CLOCKWISE here. Front face is therefore CW so that
    // "cull back" removes the same triangles GL removes.
    bool ok = true;
    for (int cls = 0; cls < 3 && ok; ++cls)
    {
      for (int cull = 0; cull < 2 && ok; ++cull)
      {
        cba[0].blendEnable = (cls == 0) ? VK_FALSE : VK_TRUE;
        cba[0].colorBlendOp = VK_BLEND_OP_ADD;
        cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
        if (cls == 1)   // Alpha
        {
          cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
          cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
          cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
          cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        }
        else            // No_Add_Alpha / Add -- premultiplied, ONE/ONE
        {
          cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
          cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
          cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
          cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        }
        // [dev diagnosis] NOGGIT_VK_M2_CULL=0 forces cull off; NOGGIT_VK_M2_FF=ccw flips the winding.
        // Our clip conversion leaves y un-flipped, so a GL front (CCW) face should be CW here -- these
        // knobs exist to PROVE that rather than assume it. Not user-facing.
        static int const s_cull_on = []() { char const* v = std::getenv("NOGGIT_VK_M2_CULL"); return (v && *v == '0') ? 0 : 1; }();
        static bool const s_ff_ccw = []() { char const* v = std::getenv("NOGGIT_VK_M2_FF"); return v && v[0] == 'c' && v[1] == 'c'; }();
        rs.cullMode = (cull && s_cull_on) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        rs.frontFace = s_ff_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;

        VkPipeline p = VK_NULL_HANDLE;
        VkResult const r = vkCreateGraphicsPipelines(_device, _pipeline_cache, 1, &pci, nullptr, &p);
        if (r != VK_SUCCESS)
        {
          LogError << "[VK] M2 pipeline variant (blend " << cls << ", cull " << cull
                   << ") create failed: " << vkres(r) << std::endl;
          ok = false;
          break;
        }
        _m2_pipelines[cls * 2 + cull] = p;
      }
    }
    vkDestroyShaderModule(_device, vs, nullptr);
    vkDestroyShaderModule(_device, fs, nullptr);
    if (!ok)
    {
      for (VkPipeline& p : _m2_pipelines)
      {
        if (p) vkDestroyPipeline(_device, p, nullptr);
        p = VK_NULL_HANDLE;
      }
      _m2_pipeline = VK_NULL_HANDLE;
      return false;
    }
    _m2_pipeline = _m2_pipelines[0];
    LogError << "[VK] M2 batched pipelines ready (6 blend/cull variants)" << std::endl;
    return true;
  }
  // [2026-09-03] createDoodadPipeline / setDoodads (the untextured clay path) removed --
  // the M2 batches carry every doodad; nothing fed or recorded this since 2026-09-02.

  bool VulkanBackend::setWaterMesh(float const* pos_normal_interleaved, std::size_t vertex_count,
                                   std::uint32_t const* indices, std::size_t index_count)
  {
    SlowCall _sc_setWaterMesh("setWaterMesh");
    if (!pos_normal_interleaved || !vertex_count || !indices || !index_count)
    {
      _water_index_count = 0;   // water toggled off: draw nothing, keep the buffers
      return true;
    }

    if (!_ready)
      return false;

    _water_index_count = 0;
    if (!vertex_count || !index_count)
      return true; // legitimately no water in this neighbourhood

    // Grow-only, same reasoning as the particle streams: the WMO-liquid half of this mesh changes
    // whenever the camera moves, so a destroy/create pair here was a frequent stall.
    // Ranged: the ADT water buffer is persistent and slot-allocated, so a crossing only moves the
    // bytes of the tiles that changed instead of re-copying the whole ~70 MB neighbourhood.
    if (!writeHostBufferRanges(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, pos_normal_interleaved,
                               vertex_count * 68, _water_dirty_v.data(), _water_dirty_v.size(),
                               _water_vbo, _water_vbo_mem, _water_vbo_cap)
        || !writeHostBufferRanges(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices, index_count * 4,
                                  _water_dirty_i.data(), _water_dirty_i.size(),
                                  _water_ibo, _water_ibo_mem, _water_ibo_cap))
    {
      LogError << "[VK] water mesh upload failed" << std::endl;
      return false;
    }
    _water_dirty_v.clear(); _water_dirty_i.clear();
    _water_index_count = static_cast<std::uint32_t>(index_count);
    LogError << "[VK] water mesh: " << vertex_count << " verts / " << index_count << " indices" << std::endl;
    return true;
  }

  bool VulkanBackend::setParticles(float const* verts, std::size_t vertex_count,
                                   std::uint32_t const* indices, std::size_t index_count,
                                   ParticleDraw const* draws, std::size_t draw_count)
  {
    if (!_ready)
      return false;
    _particle_draws.clear();
    if (!verts || !vertex_count || !indices || !index_count || !draws || !draw_count)
      return true;   // nothing alive this frame

    // Particles change EVERY frame. Destroying and re-creating both buffers here (with a full
    // vkQueueWaitIdle in front) was a guaranteed per-frame GPU stall plus device-memory churn --
    // the churn is also what degraded the frame rate the longer a session ran. Grow-only host
    // buffers write in place instead, and only stall on the rare frame that has to grow.
    if (!writeHostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, verts, vertex_count * 36,
                         _particle_vbo, _particle_vbo_mem, _particle_vbo_cap)
        || !writeHostBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices, index_count * 4,
                            _particle_ibo, _particle_ibo_mem, _particle_ibo_cap))
    {
      LogError << "[VK] particle upload failed" << std::endl;
      return false;
    }
    _particle_draws.assign(draws, draws + draw_count);
    return true;
  }

  bool VulkanBackend::setTerrainVisibleChunks(ChunkDraw const* draws, std::size_t count)
  {
    if (!_ready)
      return false;
    _terrain_visible_chunks = 0;
    if (!draws || !count)
      return true;   // nothing visible -> the draw below is skipped entirely

    // VkDrawIndexedIndirectCommand: indexCount, instanceCount, firstIndex, vertexOffset, firstInstance
    static std::vector<std::uint32_t> cmds;   // reused: this runs every frame
    cmds.resize(count * 5);
    for (std::size_t i = 0; i < count; ++i)
    {
      cmds[i * 5 + 0] = draws[i].index_count;
      cmds[i * 5 + 1] = 1;
      cmds[i * 5 + 2] = draws[i].first_index;
      cmds[i * 5 + 3] = static_cast<std::uint32_t>(draws[i].base_vertex);
      cmds[i * 5 + 4] = 0;
    }
    if (!writeHostBuffer(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, cmds.data(), cmds.size() * 4,
                         _terrain_indirect, _terrain_indirect_mem, _terrain_indirect_cap))
      return false;
    _terrain_visible_chunks = static_cast<std::uint32_t>(count);
    return true;
  }

  bool VulkanBackend::setWaterVisibleChunks(ChunkDraw const* draws, std::size_t count)
  {
    if (!_ready)
      return false;
    _water_visible_chunks = 0;
    if (!draws || !count)
      return true;

    static std::vector<std::uint32_t> cmds;   // reused: this runs every frame
    cmds.resize(count * 5);
    for (std::size_t i = 0; i < count; ++i)
    {
      cmds[i * 5 + 0] = draws[i].index_count;
      cmds[i * 5 + 1] = 1;
      cmds[i * 5 + 2] = draws[i].first_index;
      cmds[i * 5 + 3] = static_cast<std::uint32_t>(draws[i].base_vertex);
      cmds[i * 5 + 4] = 0;
    }
    if (!writeHostBuffer(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, cmds.data(), cmds.size() * 4,
                         _water_indirect, _water_indirect_mem, _water_indirect_cap))
      return false;
    _water_visible_chunks = static_cast<std::uint32_t>(count);
    return true;
  }

  bool VulkanBackend::setWmoLiquidMesh(float const* verts, std::size_t vertex_count,
                                       std::uint32_t const* indices, std::size_t index_count)
  {
    SlowCall _sc_setWmoLiquidMesh("setWmoLiquidMesh");
    if (!_ready)
      return false;
    _wliq_index_count = 0;
    if (!verts || !vertex_count || !indices || !index_count)
      return true;   // no WMO liquid in view

    if (!writeHostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, verts, vertex_count * 68,
                         _wliq_vbo, _wliq_vbo_mem, _wliq_vbo_cap)
        || !writeHostBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices, index_count * 4,
                            _wliq_ibo, _wliq_ibo_mem, _wliq_ibo_cap))
    {
      LogError << "[VK] WMO liquid upload failed" << std::endl;
      return false;
    }
    _wliq_index_count = static_cast<std::uint32_t>(index_count);
    return true;
  }

  bool VulkanBackend::setSkyDome(float const* positions_xyz, float const* colors_rgb,
                                 std::size_t vertex_count, std::uint16_t const* indices,
                                 std::size_t index_count)
  {
    if (!_ready)
      return false;
    if (!positions_xyz || !colors_rgb || !vertex_count || !indices || !index_count)
    {
      _skydome_index_count = 0;   // fall back to the placeholder gradient
      return true;
    }

    // The dome's COLOURS change as the clock advances, so vkDomeDirty() is set on essentially
    // every frame -- this setter runs per frame. It used to vkQueueWaitIdle (a FULL GPU stall:
    // the CPU blocks until the GPU has drained everything) and then destroy + re-create both
    // buffers, i.e. a stall plus two vkAllocateMemory per frame. Same fix as the particle path:
    // grow-only host buffers written in place, stalling only on a frame that actually has to grow.
    _skydome_index_count = 0;

    // GL keeps positions and colours in two separate VBOs; interleave them for one binding.
    // Reused across frames: this runs per frame and a fresh vector each time is a pointless malloc.
    static std::vector<float> interleaved;
    interleaved.resize(vertex_count * 6);
    for (std::size_t i = 0; i < vertex_count; ++i)
    {
      interleaved[i * 6 + 0] = positions_xyz[i * 3 + 0];
      interleaved[i * 6 + 1] = positions_xyz[i * 3 + 1];
      interleaved[i * 6 + 2] = positions_xyz[i * 3 + 2];
      interleaved[i * 6 + 3] = colors_rgb[i * 3 + 0];
      interleaved[i * 6 + 4] = colors_rgb[i * 3 + 1];
      interleaved[i * 6 + 5] = colors_rgb[i * 3 + 2];
    }

    if (!writeHostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, interleaved.data(),
                         interleaved.size() * sizeof(float),
                         _skydome_vbo, _skydome_vbo_mem, _skydome_vbo_cap)
        || !writeHostBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices,
                            index_count * sizeof(std::uint16_t),
                            _skydome_ibo, _skydome_ibo_mem, _skydome_ibo_cap))
    {
      LogError << "[VK] sky dome upload failed" << std::endl;
      return false;
    }
    _skydome_index_count = static_cast<std::uint32_t>(index_count);
    {
      // [SKY BLACK] what colours went in? A dome uploaded while the sky was still uninitialised
      // would draw black forever, because dome_changed latches once the signature stops moving.
      float mn = 1e9f, mx = -1e9f, sum = 0.f;
      for (std::size_t i = 0; i < vertex_count * 3; ++i)
      { float const c = colors_rgb[i]; mn = std::min(mn, c); mx = std::max(mx, c); sum += c; }
      LogError << "[VK] skydome upload: verts=" << vertex_count << " idx=" << index_count
               << " colour min=" << mn << " max=" << mx
               << " mean=" << (vertex_count ? sum / (vertex_count * 3) : 0.f) << std::endl;
    }
    // NO per-frame log here: this runs every frame, and LogError writes to log.txt (file I/O in
    // the middle of the frame). It had produced 1100 lines in a 600-frame benchmark.
    return true;
  }

  bool VulkanBackend::setHorizon(float const* xyz, std::size_t vertex_count, std::uint32_t vertex_generation,
                                 std::uint32_t const* solid, std::size_t solid_count,
                                 std::uint32_t const* holes, std::size_t hole_count, float const* rgb,
                                 float const* mvp16)
  {
    if (!_ready)
      return false;
    _horizon_solid_count = 0;
    _horizon_hole_count = 0;
    if (!xyz || !vertex_count || (!solid_count && !hole_count) || !rgb || !mvp16)
      return true;   // nothing to draw this frame
    std::memcpy(_horizon_mvp, mvp16, sizeof(_horizon_mvp));

    // the whole map's WDL mesh: uploaded once, again only when the generation changes
    if (vertex_generation != _horizon_vertex_generation || !_horizon_vbo)
    {
      if (!writeHostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, xyz, vertex_count * 3 * sizeof(float),
                           _horizon_vbo, _horizon_vbo_mem, _horizon_vbo_cap))
      {
        LogError << "[VK] horizon vertex upload failed (" << vertex_count << " verts)" << std::endl;
        return false;
      }
      _horizon_vertex_generation = vertex_generation;
      LogError << "[VK] horizon mesh uploaded: verts=" << vertex_count << " gen=" << vertex_generation << std::endl;
    }

    // solid range first, hole range after it: one index buffer, two draws
    static std::vector<std::uint32_t> combined;
    combined.resize(solid_count + hole_count);
    if (solid_count) std::memcpy(combined.data(), solid, solid_count * sizeof(std::uint32_t));
    if (hole_count) std::memcpy(combined.data() + solid_count, holes, hole_count * sizeof(std::uint32_t));
    if (!writeHostBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, combined.data(), combined.size() * sizeof(std::uint32_t),
                         _horizon_ibo, _horizon_ibo_mem, _horizon_ibo_cap))
    {
      LogError << "[VK] horizon index upload failed" << std::endl;
      return false;
    }
    _horizon_solid_count = static_cast<std::uint32_t>(solid_count);
    _horizon_hole_count = static_cast<std::uint32_t>(hole_count);
    _horizon_color[0] = rgb[0];
    _horizon_color[1] = rgb[1];
    _horizon_color[2] = rgb[2];
    return true;
  }

  bool VulkanBackend::setCloudDome(float const* pos_uv_alpha, std::size_t vertex_count,
                                   std::uint16_t const* indices, std::size_t index_count)
  {
    if (!_ready)
      return false;
    if (!pos_uv_alpha || !vertex_count || !indices || !index_count)
    {
      _cloud_index_count = 0;
      return true;
    }

    vkQueueWaitIdle(_queue);
    if (_cloud_vbo) { vkDestroyBuffer(_device, _cloud_vbo, nullptr); _cloud_vbo = VK_NULL_HANDLE; }
    if (_cloud_vbo_mem) { vkFreeMemory(_device, _cloud_vbo_mem, nullptr); _cloud_vbo_mem = VK_NULL_HANDLE; }
    if (_cloud_ibo) { vkDestroyBuffer(_device, _cloud_ibo, nullptr); _cloud_ibo = VK_NULL_HANDLE; }
    if (_cloud_ibo_mem) { vkFreeMemory(_device, _cloud_ibo_mem, nullptr); _cloud_ibo_mem = VK_NULL_HANDLE; }
    _cloud_index_count = 0;

    if (!createHostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_count * 24,
                          _cloud_vbo, _cloud_vbo_mem, pos_uv_alpha)
        || !createHostBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_count * sizeof(std::uint16_t),
                             _cloud_ibo, _cloud_ibo_mem, indices))
    {
      LogError << "[VK] cloud dome upload failed" << std::endl;
      return false;
    }
    _cloud_index_count = static_cast<std::uint32_t>(index_count);
    LogError << "[VK] cloud dome: " << vertex_count << " verts / " << index_count << " indices" << std::endl;
    return true;
  }

  bool VulkanBackend::createHostBuffer(VkBufferUsageFlags usage, std::size_t bytes,
                                       VkBuffer& buf, VkDeviceMemory& mem, void const* data,
                                       std::size_t copy_bytes)
  {
    // Only `copy_bytes` of `data` is valid -- the buffer itself may be deliberately bigger.
    if (copy_bytes == 0 || copy_bytes > bytes)
      copy_bytes = bytes;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(_device, &bci, nullptr, &buf) != VK_SUCCESS)
      return false;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(_device, buf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(_phys, &mp);
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((req.memoryTypeBits & (1u << i))
          && (mp.memoryTypes[i].propertyFlags
              & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
             == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
      { type = i; break; }
    if (type == UINT32_MAX)
      return false;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(_device, &mai, nullptr, &mem) != VK_SUCCESS
        || vkBindBufferMemory(_device, buf, mem, 0) != VK_SUCCESS)
      return false;

    void* mapped = nullptr;
    if (vkMapMemory(_device, mem, 0, bytes, 0, &mapped) != VK_SUCCESS)
      return false;
    if (data && copy_bytes)
      std::memcpy(mapped, data, copy_bytes);
    vkUnmapMemory(_device, mem);
    return true;
  }

  bool VulkanBackend::setTerrainMesh(float const* pos_normal_interleaved, std::size_t vertex_count,
                                     std::uint32_t const* indices, std::size_t index_count)
  {
    SlowCall _sc_setTerrainMesh("setTerrainMesh");
    if (!_ready || !vertex_count || !index_count)
      return false;

    // [spike fix] This is NOT editor cadence -- it fires every time the camera crosses a tile
    // boundary, i.e. constantly while flying, and it used to idle the whole queue and rebuild both
    // buffers. Grow-only in-place writes instead: a stall only on the rare frame that must grow,
    // and even that no longer waits (the old allocation is retired, not freed under a barrier).
    _terrain_index_count = 0;

    // [STABLE SLOTS -- geometry] Copy only the tiles that changed. With a fixed slot space these
    // buffers are ~85 MB (verts) + ~50 MB (indices); re-copying all of it on every tile crossing was
    // most of what remained of the rebuild hitch.
    if (!writeHostBufferRanges(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, pos_normal_interleaved,
                               vertex_count * 36, _terrain_dirty_v.data(), _terrain_dirty_v.size(),
                               _terrain_vbo, _terrain_vbo_mem, _terrain_vbo_cap)
        || !writeHostBufferRanges(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices, index_count * 4,
                                  _terrain_dirty_i.data(), _terrain_dirty_i.size(),
                                  _terrain_ibo, _terrain_ibo_mem, _terrain_ibo_cap))
    {
      LogError << "[VK] terrain mesh upload failed" << std::endl;
      return false;
    }
    _terrain_index_count = static_cast<std::uint32_t>(index_count);
    // no per-tile-change log line: this fires whenever the camera crosses a tile boundary and
    // LogError writes to log.txt in the middle of the frame
    return true;
  }

  bool VulkanBackend::init(std::uint32_t width, std::uint32_t height, std::uint32_t msaa_samples)
  {
    _width = width;
    _height = height;
    // [MSAA] requested count now; clamped to what the device's framebuffers support below.
    _samples = static_cast<VkSampleCountFlagBits>(
        msaa_samples >= 8u ? 8u : msaa_samples >= 4u ? 4u : msaa_samples >= 2u ? 2u : 1u);
    auto const clamp_samples = [this]()
    {
      if (_samples != VK_SAMPLE_COUNT_1_BIT)
      {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(_phys, &p);
        VkSampleCountFlags const ok = p.limits.framebufferColorSampleCounts
                                    & p.limits.framebufferDepthSampleCounts;
        while (_samples != VK_SAMPLE_COUNT_1_BIT && !(ok & _samples))
          _samples = static_cast<VkSampleCountFlagBits>(_samples >> 1);
        LogError << "[VK] MSAA sample count: " << static_cast<int>(_samples) << std::endl;
      }
      return true;
    };
    // The pipeline cache must exist BEFORE the first pipeline is built -- everything below passes it.
    auto const seed_cache = [this]() { createPipelineCache(); return true; };
    if (!loadLoader() || !createInstance() || !pickDeviceAndQueue() || !createDevice()
        || !clamp_samples()
        || !seed_cache() || !createCopyQueue()
        || !createSharedImage() || !createSemaphores() || !createRenderTarget() || !createPipeline()
        || !createTextureInfra()
        // createTerrainTexInfra FIRST: it owns _tt_dsl (set 1 = lighting UBO + bindless textures),
        // and the water pipeline built inside createTerrainPipeline needs that set in its layout.
        // With the old order _tt_dsl was still null, water silently fell back to the terrain layout
        // (set 0 only) and its lighting reads returned zeros -> fully transparent water.
        || (_descriptor_indexing && !createTerrainTexInfra())
        || !createTerrainPipeline())
    {
      LogError << "[VK] init failed -- staying on pure GL" << std::endl;
      return false;
    }
    // [2026-09-04] Build the UI overlay pipeline EAGERLY, here, with every other pipeline. It was
    // created lazily from setUiOverlay() on the main thread while the recorder threads were already
    // running -- a needless variable when its draws were producing no fragments. Non-fatal: the
    // editor must still run if only the overlay fails.
    if (!createUiPipeline())
      LogError << "[VK] UI overlay pipeline unavailable -- overlay will not composite" << std::endl;

    // Persist the cache HERE, not only at shutdown: a hard exit (the harness uses std::_Exit) would
    // otherwise never write it, and the next launch would recompile every pipeline again.
    savePipelineCache();
    LogError << "[VK] backend ready: " << _width << "x" << _height
             << " shared image (" << (_image_mem_size / 1024) << " KB) + semaphores + GRAPHICS PIPELINE (SPIR-V)"
             << std::endl;
    _ready = true;
    // [phase B] textured terrain path -- soft-optional (falls back to the clay path if anything fails)
    if (_descriptor_indexing && !(createTerrainTexPipeline() && createM2Pipeline()
                                  && createWmoPipeline()))
    {
      LogError << "[VK] textured terrain infra failed -- clay terrain only" << std::endl;
      _tt_pipeline = VK_NULL_HANDLE;
    }
    return true;
  }

  // [phase H] One command pool + one secondary buffer per recorder. Vulkan command pools are NOT
  // thread-safe, so a thread that records must own its pool outright -- sharing one pool across
  // recording threads is the classic way to get sporadic corruption instead of a clean error.
  // Seed the pipeline cache from disk. Every vkCreateGraphicsPipelines call passes this, so a second
  // launch reuses the driver's compiled form instead of rebuilding ~20 pipelines from SPIR-V.
  bool VulkanBackend::createCopyQueue()
  {
    // [GPU timing] one 2-slot timestamp pool, written either side of the render pass. timestampPeriod
    // converts ticks to ns; a device that reports 0 has no usable timestamps, so the pool stays null
    // and the reporting is simply skipped.
    {
      {
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(_device, &sci, nullptr, &_copy_done) != VK_SUCCESS)
          _copy_done = VK_NULL_HANDLE;
      }
      VkPhysicalDeviceProperties props{};
      vkGetPhysicalDeviceProperties(_phys, &props);
      _ts_period_ns = props.limits.timestampPeriod;
      if (_ts_period_ns > 0.f && vkCreateQueryPool)
      {
        VkQueryPoolCreateInfo qpi{};
        qpi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2;
        if (vkCreateQueryPool(_device, &qpi, nullptr, &_ts_pool) != VK_SUCCESS)
          _ts_pool = VK_NULL_HANDLE;
      }
    }

    // Second queue from the same family when the device offers one; otherwise share the graphics
    // queue. Either way the pool/buffer/fence are the transfer's OWN, which is what removes the
    // frame-state sharing.
    if (_queue_family_count > 1)
      vkGetDeviceQueue(_device, _queue_family, 1, &_copy_queue);
    if (!_copy_queue)
      _copy_queue = _queue;

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = _queue_family;
    if (vkCreateCommandPool(_device, &pci, nullptr, &_copy_pool) != VK_SUCCESS)
      return false;

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = _copy_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(_device, &ai, &_copy_cmd) != VK_SUCCESS)
      return false;

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // SIGNALED, like the frame fence: uploadImage2D waits on this fence BEFORE it records, so an
    // unsignalled fence deadlocks on the very first texture upload.
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(_device, &fci, nullptr, &_copy_fence) != VK_SUCCESS)
      return false;

    LogError << "[VK] copy queue: " << ((_copy_queue == _queue) ? "shared with graphics"
                                                                : "dedicated second queue")
             << " (own pool + fence)" << std::endl;
    return true;
  }

  void VulkanBackend::createPipelineCache()
  {
    if (_pipeline_cache || !vkCreatePipelineCache)
      return;
    _pipeline_cache_path = "vk_pipeline_cache.bin";
    std::vector<char> blob;
    if (std::ifstream in(_pipeline_cache_path, std::ios::binary); in)
      blob.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

    VkPipelineCacheCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.empty() ? nullptr : blob.data();
    if (vkCreatePipelineCache(_device, &ci, nullptr, &_pipeline_cache) != VK_SUCCESS)
    {
      _pipeline_cache = VK_NULL_HANDLE;   // soft: pipelines still build, just without the cache
      return;
    }
    LogError << "[VK] pipeline cache: seeded with " << blob.size() << " bytes" << std::endl;
  }

  void VulkanBackend::savePipelineCache()
  {
    if (!_pipeline_cache || !vkGetPipelineCacheData)
      return;
    std::size_t bytes = 0;
    if (vkGetPipelineCacheData(_device, _pipeline_cache, &bytes, nullptr) != VK_SUCCESS || !bytes)
      return;
    std::vector<char> blob(bytes);
    if (vkGetPipelineCacheData(_device, _pipeline_cache, &bytes, blob.data()) != VK_SUCCESS)
      return;
    if (std::ofstream out(_pipeline_cache_path, std::ios::binary); out)
      out.write(blob.data(), static_cast<std::streamsize>(bytes));
  }

  bool VulkanBackend::createRecorders(std::size_t count)
  {
    destroyRecorders();
    _recorders.resize(count);
    for (auto& r : _recorders)
    {
      VkCommandPoolCreateInfo pci{};
      pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pci.queueFamilyIndex = _queue_family;
      if (vkCreateCommandPool(_device, &pci, nullptr, &r.pool) != VK_SUCCESS)
      {
        LogError << "[VK] recorder command pool create failed" << std::endl;
        destroyRecorders();
        return false;
      }
      VkCommandBufferAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      ai.commandPool = r.pool;
      ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
      ai.commandBufferCount = 1;
      if (vkAllocateCommandBuffers(_device, &ai, &r.cmd) != VK_SUCCESS)
      {
        LogError << "[VK] recorder command buffer alloc failed" << std::endl;
        destroyRecorders();
        return false;
      }
    }
    return true;
  }

  void VulkanBackend::destroyRecorders()
  {
    for (auto& r : _recorders)
    {
      if (r.pool)
        vkDestroyCommandPool(_device, r.pool, nullptr);   // frees its buffers with it
      r = Recorder{};
    }
    _recorders.clear();
  }

  // [phase H] Record ONE pass group into its own secondary command buffer. Every group reads
  // shared state but writes only its own buffer, which is what makes it safe to run these
  // concurrently; the primary then executes them in group order, so the alpha-blended passes
  // still composite in exactly the order the single-threaded path used.
  //   0 sky dome + clouds | 1 terrain | 2 WMO | 3 doodads + M2 | 4 water + celestials + particles
  // recordGroup runs on the RECORDING THREADS, so its diagnostics need serialising: plain
  // `LogError << a << b` from five threads interleaves into unreadable lines (that is what produced
  // "celestial draws issued: 10082 tex=" -- two passes' numbers spliced together), and the throttle
  // counters were a data race besides.
  namespace
  {
    std::mutex& recordLogMutex()
    {
      static std::mutex m;
      return m;
    }
  }

  void VulkanBackend::recordGroup(int group, VkCommandBuffer rec, float t, float const* mvp16)
  {
    // [GPU bisect] NOGGIT_VK_SKIP is a bitmask of PASSES to skip while recording, so the GPU cost of
    // each one can be measured directly (run with NOGGIT_VK_SUBMIT_THREAD=0, which submits inline and
    // therefore charges the frame for the real GPU time instead of hiding it behind GL's traversal).
    //   1=sky dome  2=clouds  4=WMO  8=doodads  16=M2  32=water  64=celestials  128=particles
    static int const s_skip = []() { char const* v = std::getenv("NOGGIT_VK_SKIP"); return v ? std::atoi(v) : 0; }();
    auto const pass_on = [](int bit) { return (s_skip & bit) == 0; };
    auto const want = [group](int g) { return group < 0 || group == g; };
    // WMO-ONLY MAPS: this gate used to be `_terrain_index_count && mvp16`, and it wraps EVERYTHING --
    // sky, terrain, WMO, M2, water, doodads (to line ~4220). A map with no ADT terrain (every dungeon
    // and instance: Molten Core, and by extension all of them) therefore drew NOTHING in Vulkan while
    // GL drew the scene normally. The parity suite could not see it: every camera in it is on map 0,
    // which always has terrain. Each pass below already carries its own readiness guard, so the only
    // thing this gate legitimately protected is the terrain section itself -- now guarded there.
    if (mvp16)
    {
      // Sky behind the terrain (no depth interaction). The REAL zone dome when one has been fed,
      // otherwise the placeholder gradient.
      if (pass_on(1) && want(0) && _skydome_pipeline && _skydome_index_count && _skydome_layout && mvp16)
      {
        vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _skydome_pipeline);
        float dome_push[20];
        std::memcpy(dome_push, mvp16, 16 * sizeof(float));
        dome_push[16] = _sky_camera[0];
        dome_push[17] = _sky_camera[1];
        dome_push[18] = _sky_camera[2];
        dome_push[19] = 0.f;
        vkCmdPushConstants(rec, _skydome_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(dome_push), dome_push);
        VkDeviceSize const dome_zero = 0;
        vkCmdBindVertexBuffers(rec, 0, 1, &_skydome_vbo, &dome_zero);
        vkCmdBindIndexBuffer(rec, _skydome_ibo, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(rec, _skydome_index_count, 1, 0, 0, 0);
        {
          // [SKY BLACK] draw-issued log: never trust a pass without one.
          static int s_dd = 0;
          if ((s_dd++ % 120) == 0)
            LogError << "[VK] skydome draw issued: idx=" << _skydome_index_count << std::endl;
        }
      }
      else if (want(0) && _sky_pipeline)
      {
        vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _sky_pipeline);
        float sky_push[4] = { t, 0.f, 0.f, 0.f };
        vkCmdPushConstants(rec, _pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(sky_push), sky_push);
        vkCmdDraw(rec, 3, 1, 0, 0);
      }

      // Cloud deck: over the dome, still behind the terrain -- GL draws it in exactly this slot.
      if (pass_on(2) && want(0) && _cloud_pipeline && _cloud_index_count && _cloud_layout && mvp16
          && _cloud_tex_index >= 0 && _cloud_opacity > 0.001f && _dset && _tt_dset)
      {
        vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _cloud_pipeline);
        VkDescriptorSet cloud_sets[2] = { _dset, _tt_dset };
        vkCmdBindDescriptorSets(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _cloud_layout, 0, 2,
                                cloud_sets, 0, nullptr);
        float cloud_push[24];
        std::memcpy(cloud_push, mvp16, 16 * sizeof(float));
        cloud_push[16] = _sky_camera[0];
        cloud_push[17] = _sky_camera[1];
        cloud_push[18] = _sky_camera[2];
        cloud_push[19] = _cloud_opacity;
        cloud_push[20] = static_cast<float>(_cloud_tex_index);
        cloud_push[21] = cloud_push[22] = cloud_push[23] = 0.f;
        vkCmdPushConstants(rec, _cloud_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(cloud_push), cloud_push);
        VkDeviceSize const cloud_zero = 0;
        vkCmdBindVertexBuffers(rec, 0, 1, &_cloud_vbo, &cloud_zero);
        vkCmdBindIndexBuffer(rec, _cloud_ibo, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(rec, _cloud_index_count, 1, 0, 0, 0);
        // A pass that is silently skipped also "passes" the harness when the reference is faint --
        // so say out loud that the deck was actually recorded.
        static std::atomic<unsigned> s_cloud_draws{0};
        if ((s_cloud_draws++ % 300u) == 0)
        {
          std::lock_guard<std::mutex> lk(recordLogMutex());
          LogError << "[VK] cloud draw issued: idx=" << _cloud_index_count
                   << " tex=" << _cloud_tex_index << " opacity=" << _cloud_opacity << std::endl;
        }
      }

      float push[20]; // mat4 (16) + time + pad
      std::memcpy(push, mvp16, 16 * sizeof(float));
      push[16] = t;
      static float const s_term_mask = []() -> float {
        char const* v = std::getenv("NOGGIT_VK_TERM");
        return (v && *v) ? static_cast<float>(std::atoi(v)) : 15.f; // all terms on
      }();
      push[17] = s_term_mask;   // [VULKAN parity bisect] bit0 light, bit1 MCSH, bit2 fog, bit3 MCCV
      push[18] = push[19] = 0.f;
      VkDeviceSize const zero = 0;
      // The plain-terrain path is the ELSE of the textured one, so the group gate has to wrap BOTH:
      // gating only the `if` sent every other group down the else and drew the untextured terrain
      // once per secondary buffer (a flat green plane over the whole scene).
      // `!_terrain_index_count` covers the WMO-only maps: with no terrain there is nothing to draw
      // here, and the plain-terrain ELSE below would bind a null _terrain_vbo.
      if (!_terrain_index_count || !pass_on(256) || !want(1)) { /* skipped, or another group's buffer */ }
      else if (_tt_ready && _tt_pipeline && _tt_cidx)
      {
        // [phase B] TEXTURED terrain: bindless tilesets + chunk atlases + chunk SSBO + lighting UBO
        vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _tt_pipeline);
        VkDescriptorSet sets[2] = { _dset, _tt_dset };
        vkCmdBindDescriptorSets(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _tt_layout, 0, 2, sets, 0, nullptr);
        vkCmdPushConstants(rec, _tt_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
        VkBuffer vbs[2] = { _terrain_vbo, _tt_cidx };
        VkDeviceSize offs[2] = { 0, 0 };
        vkCmdBindVertexBuffers(rec, 0, 2, vbs, offs);
        vkCmdBindIndexBuffer(rec, _terrain_ibo, 0, VK_INDEX_TYPE_UINT32);
        if (_terrain_visible_chunks && _terrain_indirect)
        {
          // One indirect command per VISIBLE chunk. GL culls per chunk; drawing the whole
          // neighbourhood unconditionally was most of the VK GPU time.
          if (_multi_draw_indirect)
            vkCmdDrawIndexedIndirect(rec, _terrain_indirect, 0, _terrain_visible_chunks, 20);
          else
            for (std::uint32_t i = 0; i < _terrain_visible_chunks; ++i)
              vkCmdDrawIndexedIndirect(rec, _terrain_indirect, i * 20, 1, 20);
        }
        else
        {
          vkCmdDrawIndexed(rec, _terrain_index_count, 1, 0, 0, 0);
        }
      }
      else
      {
        vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _terrain_pipeline);
        if (_dset)
        {
          vkCmdBindDescriptorSets(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _terrain_layout, 0, 1, &_dset, 0, nullptr);
        }
        vkCmdPushConstants(rec, _terrain_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
        vkCmdBindVertexBuffers(rec, 0, 1, &_terrain_vbo, &zero);
        vkCmdBindIndexBuffer(rec, _terrain_ibo, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(rec, _terrain_index_count, 1, 0, 0, 0);
      }
      // The doodad pipeline below shares _terrain_layout's push block, and a secondary command buffer
      // inherits NO state, so this has to be re-issued in every group that draws with that layout.
      vkCmdPushConstants(rec, _terrain_layout,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);

      // [phase D] WMO groups: replayed GL runs, one draw each, pipeline keyed by blend/cull
      if (pass_on(4) && want(2) && _wmo_pipeline && !_wmo_draws.empty() && _wmo_vbo && _wmo_ibo && _wmo_xforms && _wmo_batches && _wmo_amb)
      {
        VkDescriptorSet wsets[3] = { _dset, _tt_dset, _wmo_dset };
        vkCmdBindDescriptorSets(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _wmo_layout, 0, 3, wsets, 0, nullptr);
        VkDeviceSize const wzero = 0;
        vkCmdBindVertexBuffers(rec, 0, 1, &_wmo_vbo, &wzero);
        vkCmdBindIndexBuffer(rec, _wmo_ibo, 0, VK_INDEX_TYPE_UINT16);
        VkPipeline last = VK_NULL_HANDLE;
            float wpush[24]{};
        std::memcpy(wpush, push, 20 * sizeof(float));
        for (WmoDraw const& d : _wmo_draws)
        {
          int const cls = (d.blend_mode == 2) ? 1 : ((d.blend_mode == 3 || d.blend_mode == 4) ? 2 : 0);
          int const cull_i = d.backface_cull ? 1 : 0;
          // blend_mode 0 is the only mode with no alpha test, so it is the only one that can take
          // the early-Z pipeline (no `discard` compiled in).
          VkPipeline pl = (d.blend_mode == 0 && _wmo_pipelines_earlyz[cull_i])
                        ? _wmo_pipelines_earlyz[cull_i]
                        : _wmo_pipelines[cls * 2 + cull_i];
          if (!pl) pl = _wmo_pipeline;
          if (pl != last)
          {
            vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
            last = pl;
          }
          // PUSH LAYOUT must match wmo.frag EXACTLY -- the struct has no padding:
          //   mat4 mvp [0..15] | time [16] | terms [17] | slice [18] | xform_index [19]
          //   | gain [20] | floor [21] | debug [22]
          // Writing xform_index at [20] (the M2 layout, which pads to 20 floats) made EVERY WMO
          // instance read transform 0 and pushed wmo_debug_mode past the end of the struct.
          std::memcpy(&wpush[19], &d.xform_index, sizeof(std::uint32_t));
          wpush[20] = _wmo_interior_gain;
          wpush[21] = _wmo_interior_floor;
          {
            extern int g_wmo_interior_mod2x;
            wpush[23] = static_cast<float>(g_wmo_interior_mod2x);
          }
          {
            static int const s_dbg = []() { char const* v = std::getenv("NOGGIT_WMO_DEBUG");
                                            return (v && *v) ? std::atoi(v) : 0; }();
            // NOGGIT_WMO_DEBUG_MOCV mirrors GL's branch visualiser; encoded as 100 + mode so it
            // shares the one debug slot. Mode 3 colours each fragment by which lighting branch it
            // took, which compares BRANCH SELECTION between the backends directly.
            static int const s_dbg_mocv = []() { char const* v = std::getenv("NOGGIT_WMO_DEBUG_MOCV");
                                                 return (v && *v) ? std::atoi(v) : 0; }();
            wpush[22] = s_dbg_mocv ? static_cast<float>(100 + s_dbg_mocv) : static_cast<float>(s_dbg);
          }
          vkCmdPushConstants(rec, _wmo_layout,
                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96, wpush);
          vkCmdDrawIndexed(rec, d.index_count, 1, d.first_index, d.base_vertex, 0);
        }
      }

      // [2026-09-08 WDL HORIZON] the low-res distant terrain, in the client's slot: after terrain and
      // WMOs, before M2s (CMap::RenderLowDetail). Solid cells write depth, MAHO hole cells do not; the
      // far depth slice is in horizon.vert. Its OWN projection (near = far clip - 50, far = 4 x
      // farclip, the client's FUN_00791170) comes in through setHorizon, not the scene mvp.
      if (pass_on(1024) && want(2) && _horizon_pipeline && _horizon_layout && _horizon_vbo && _horizon_ibo
          && (_horizon_solid_count || _horizon_hole_count) && mvp16)
      {
        float hz_push[20];
        std::memcpy(hz_push, _horizon_mvp, 16 * sizeof(float));
        hz_push[16] = _horizon_color[0];
        hz_push[17] = _horizon_color[1];
        hz_push[18] = _horizon_color[2];
        hz_push[19] = 0.f;
        VkDeviceSize const hz_zero = 0;
        vkCmdBindVertexBuffers(rec, 0, 1, &_horizon_vbo, &hz_zero);
        vkCmdBindIndexBuffer(rec, _horizon_ibo, 0, VK_INDEX_TYPE_UINT32);
        if (_horizon_solid_count)
        {
          vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _horizon_pipeline);
          vkCmdPushConstants(rec, _horizon_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(hz_push), hz_push);
          vkCmdDrawIndexed(rec, _horizon_solid_count, 1, 0, 0, 0);
        }
        if (_horizon_hole_count && _horizon_pipeline_nowrite)
        {
          vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _horizon_pipeline_nowrite);
          vkCmdPushConstants(rec, _horizon_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(hz_push), hz_push);
          vkCmdDrawIndexed(rec, _horizon_hole_count, 1, _horizon_solid_count, 0, 0);
        }
        {
          static int s_hzd = 0;
          if ((s_hzd++ % 300) == 0)
            LogError << "[VK] horizon draw issued: solid=" << _horizon_solid_count
                     << " holes=" << _horizon_hole_count << std::endl;
        }
      }

      // doodads (opaque, instanced) after the terrain
      // [2026-09-02 NATIVE VK] The CLAY doodad pass was removed here. doodad.frag had no sampler,
      // no UVs and no alpha test -- lambert mud, "clay doodads until textures land" -- so compose
      // mode never fed it (MapView skipped setDoodads when compose_ok) and _doodad_draws was empty
      // on all 1383 traced frames. It was also redundant: vk_feeding is set independently of
      // compose_ok, so vkFeedClassicBucket routes the same tile doodads into the TEXTURED _m2_*
      // arena in both modes. Doodads render through the M2 pipeline like everything else.

      // [phase C] M2 / doodad batches: ONE vkCmdDrawIndexedIndirect for every batch VK owns
      // every buffer bound below must exist -- binding a VK_NULL_HANDLE vertex buffer faults in the driver
      if (pass_on(16) && want(3) && _m2_pipeline && _m2_draw_count && _m2_vbo && _m2_ibo && _m2_indirect
          && _m2_inst_tf && _m2_inst_in && _m2_inst_tx && _m2_inst_st && _m2_bones)
      {
        // (pipeline is bound per draw group below -- it carries the blend/cull state)
        VkDescriptorSet m2_sets[2] = { _dset, _tt_dset };
        vkCmdBindDescriptorSets(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _m2_layout, 0, 2, m2_sets, 0, nullptr);
        push[18] = _m2_slice_dist;   // m2.frag: per-pixel object-cull slice
        if (_m2_footprint_probe.load(std::memory_order_relaxed))
          push[17] = static_cast<float>(static_cast<int>(push[17]) | 64);   // term bit 6 = FOOTPRINT
        // [2026-09-01 DITHER FIX] m2.vert now builds a CAMERA-RELATIVE clip position (see the note
        // there), so this pass needs mvp * translate(camera) rather than the absolute mvp -- the same
        // matrix the celestial pass already builds. Written into a copy so the passes after M2 keep
        // the absolute mvp they expect.
        float m2_push[20];
        std::memcpy(m2_push, push, sizeof(push));
        for (int r = 0; r < 4; ++r)
        {
          m2_push[12 + r] = mvp16[0 + r] * _sky_camera[0]
                          + mvp16[4 + r] * _sky_camera[1]
                          + mvp16[8 + r] * _sky_camera[2]
                          + mvp16[12 + r];
        }
        vkCmdPushConstants(rec, _m2_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(m2_push), m2_push);
        VkBuffer m2_vbs[5] = { _m2_vbo, _m2_inst_tf, _m2_inst_in, _m2_inst_tx, _m2_inst_st };
        VkDeviceSize m2_offs[5] = { 0, 0, 0, 0, 0 };
        vkCmdBindVertexBuffers(rec, 0, 5, m2_vbs, m2_offs);
        vkCmdBindIndexBuffer(rec, _m2_ibo, 0, VK_INDEX_TYPE_UINT16);
        if (_m2_groups.size() >= 4)
        {
          // GL order: opaque/alpha-key groups first, then blended (the feed hands them over already
          // ordered). One pipeline bind + one indirect call per group, exactly like the GL loop.
          VkPipeline last = VK_NULL_HANDLE;
          for (std::size_t g = 0; g + 3 < _m2_groups.size(); g += 4)
          {
            std::int32_t const blend_mode = _m2_groups[g];
            std::int32_t const cull = (_m2_groups[g + 1] & 1) ? 1 : 0;
            bool const classic_alpha_key = (_m2_groups[g + 1] & 2) != 0;
            std::uint32_t const first = static_cast<std::uint32_t>(_m2_groups[g + 2]);
            std::uint32_t const count = static_cast<std::uint32_t>(_m2_groups[g + 3]);
            if (!count || first + count > _m2_draw_count)
              continue;
            // CLASSIC (v256) ALPHA-KEY blends with SRC_ALPHA in GL even though it is blend mode 1
            // -- the two-era alpha-key law. Selecting the variant from blend_mode alone rendered
            // every classic leaf/blade cutout with blending OFF, so their fringe texels differed
            // against GL in both directions.
            int cls = (blend_mode == 2 || (blend_mode == 1 && classic_alpha_key))
                        ? 1
                        : ((blend_mode == 3 || blend_mode == 4) ? 2 : 0);
            // [dev] FOOTPRINT probe: force the non-blending, non-culling variant so the magenta
            // fragments land opaquely and the measured coverage is the true rasterised footprint.
            static bool const s_footprint = std::getenv("NOGGIT_VK_M2_FOOTPRINT") != nullptr;
            int const cull_eff = s_footprint ? 0 : cull;
            if (s_footprint) cls = 0;
            VkPipeline p = _m2_pipelines[cls * 2 + cull_eff];
            if (!p) p = _m2_pipeline;
            if (p != last)
            {
              vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
              last = p;
            }
            if (_multi_draw_indirect)
            {
              vkCmdDrawIndexedIndirect(rec, _m2_indirect,
                                       static_cast<VkDeviceSize>(first) * sizeof(M2Draw),
                                       count, sizeof(M2Draw));
            }
            else
            {
              // drawCount MUST be 1 without the feature; one call per command is the legal equivalent
              for (std::uint32_t c = 0; c < count; ++c)
                vkCmdDrawIndexedIndirect(rec, _m2_indirect,
                                         static_cast<VkDeviceSize>(first + c) * sizeof(M2Draw),
                                         1, sizeof(M2Draw));
            }
          }
        }
        else
        {
          vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _m2_pipeline);
          if (_multi_draw_indirect)
          {
            vkCmdDrawIndexedIndirect(rec, _m2_indirect, 0, _m2_draw_count, sizeof(M2Draw));
          }
          else
          {
            for (std::uint32_t c = 0; c < _m2_draw_count; ++c)
              vkCmdDrawIndexedIndirect(rec, _m2_indirect,
                                       static_cast<VkDeviceSize>(c) * sizeof(M2Draw), 1, sizeof(M2Draw));
          }
        }
      }

      // [CLUTTER PERSISTENT] draws over per-chunk instance buffers, same M2 pipeline family. Skip
      // bit 8 (freed by the clay removal). Self-contained: rebinds sets/pushes/buffers, so it works
      // whether or not the main M2 block ran this frame.
      if (pass_on(8) && want(3) && _m2_pipeline && !_clutter_draws.empty() && _clutter_indirect
          && _m2_vbo && _m2_ibo && _dset && _tt_dset)
      {
        VkDescriptorSet cl_sets[2] = { _dset, _tt_dset };
        vkCmdBindDescriptorSets(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _m2_layout, 0, 2, cl_sets, 0, nullptr);
        float cl_push[20];
        std::memcpy(cl_push, push, sizeof(cl_push));
        cl_push[18] = _m2_slice_dist;
        for (int r = 0; r < 4; ++r)   // camera-relative mvp, exactly like the M2 pass
        {
          cl_push[12 + r] = mvp16[0 + r] * _sky_camera[0]
                          + mvp16[4 + r] * _sky_camera[1]
                          + mvp16[8 + r] * _sky_camera[2]
                          + mvp16[12 + r];
        }
        vkCmdPushConstants(rec, _m2_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(cl_push), cl_push);
        vkCmdBindIndexBuffer(rec, _m2_ibo, 0, VK_INDEX_TYPE_UINT16);
        VkPipeline last_p = VK_NULL_HANDLE;
        std::int32_t last_slot = -2;
        std::size_t i = 0;
        while (i < _clutter_draws.size())
        {
          ClutterDraw const& d0 = _clutter_draws[i];
          std::size_t run = 1;
          while (i + run < _clutter_draws.size()
                 && _clutter_draws[i + run].state_key == d0.state_key
                 && _clutter_draws[i + run].chunk_slot == d0.chunk_slot)
            ++run;
          bool ok = d0.chunk_slot >= 0
                 && static_cast<std::size_t>(d0.chunk_slot) < _clutter_chunks.size()
                 && _clutter_chunks[static_cast<std::size_t>(d0.chunk_slot)].used;
          if (ok)
          {
            std::int32_t const blend = d0.state_key & 0xF;
            std::int32_t const cull = (d0.state_key >> 4) & 1;
            bool const classic_alpha = (d0.state_key >> 5) & 1;
            int const cls = (blend == 2 || (blend == 1 && classic_alpha))
                              ? 1 : ((blend == 3 || blend == 4) ? 2 : 0);
            VkPipeline p = _m2_pipelines[cls * 2 + cull];
            if (!p) p = _m2_pipeline;
            if (p != last_p)
            {
              vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
              last_p = p;
            }
            if (d0.chunk_slot != last_slot)
            {
              ClutterChunkBuf const& cb = _clutter_chunks[static_cast<std::size_t>(d0.chunk_slot)];
              VkBuffer cl_vbs[5] = { _m2_vbo, cb.buf, cb.buf, cb.buf, cb.buf };
              VkDeviceSize cl_offs[5] = { 0, 0, cb.off_in, cb.off_tx, cb.off_st };
              vkCmdBindVertexBuffers(rec, 0, 5, cl_vbs, cl_offs);
              last_slot = d0.chunk_slot;
            }
            if (_multi_draw_indirect)
            {
              vkCmdDrawIndexedIndirect(rec, _clutter_indirect,
                                       static_cast<VkDeviceSize>(i) * sizeof(M2Draw),
                                       static_cast<std::uint32_t>(run), sizeof(M2Draw));
            }
            else
            {
              for (std::size_t c = 0; c < run; ++c)
                vkCmdDrawIndexedIndirect(rec, _clutter_indirect,
                                         static_cast<VkDeviceSize>(i + c) * sizeof(M2Draw),
                                         1, sizeof(M2Draw));
            }
          }
          i += run;
        }
        {
          static std::atomic<unsigned> s_cl_log{0};
          if ((s_cl_log++ % 300u) == 0)
          {
            std::lock_guard<std::mutex> lk(recordLogMutex());
            LogError << "[VK] clutter persistent draw issued: draws=" << _clutter_draws.size()
                     << " chunks(buffers)=" << _clutter_chunks.size() << std::endl;
          }
        }
      }

      // water after the terrain (translucent; same layout so the push constants above still apply)
      if (pass_on(32) && want(4) && _water_pipeline && _water_index_count)
      {
        vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _water_pipeline);
        // The water pass bound NO descriptor sets and inherited whatever the previous pass left. The
        // M2/WMO passes use different layouts, which invalidates the bindings for this one, so the
        // liquid shader read a ZERO lighting block -- the zone band alphas came out 0 and the water
        // rendered fully transparent (it rasterised correctly; a magenta probe showed 39,972 px).
        if (_water_layout && _tt_dset)
        {
          VkDescriptorSet wsets[2] = { _dset, _tt_dset };
          vkCmdBindDescriptorSets(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _water_layout, 0, 2,
                                  wsets, 0, nullptr);
        }
        vkCmdBindVertexBuffers(rec, 0, 1, &_water_vbo, &zero);
        vkCmdBindIndexBuffer(rec, _water_ibo, 0, VK_INDEX_TYPE_UINT32);
        if (_water_visible_chunks && _water_indirect)
        {
          if (_multi_draw_indirect)
            vkCmdDrawIndexedIndirect(rec, _water_indirect, 0, _water_visible_chunks, 20);
          else
            for (std::uint32_t i = 0; i < _water_visible_chunks; ++i)
              vkCmdDrawIndexedIndirect(rec, _water_indirect, i * 20, 1, 20);
        }
        else
        {
          vkCmdDrawIndexed(rec, _water_index_count, 1, 0, 0, 0);
        }

        // WMO liquid: same pipeline and descriptors, its own (small, per-frame) buffers.
        if (_wliq_index_count && _wliq_vbo && _wliq_ibo)
        {
          vkCmdBindVertexBuffers(rec, 0, 1, &_wliq_vbo, &zero);
          vkCmdBindIndexBuffer(rec, _wliq_ibo, 0, VK_INDEX_TYPE_UINT32);
          vkCmdDrawIndexed(rec, _wliq_index_count, 1, 0, 0, 0);
        }
      }

      // [phase F] CELESTIAL billboards last: depth-TESTED against the finished scene (terrain and
      // buildings occlude the disc) but never depth-writing. Camera-relative positions, so the
      // camera translation is folded into the matrix here (see celestial.vert).
      if (pass_on(64) && want(4) && !_celestials.empty() && _cel_layout && mvp16 && _dset && _tt_dset
          && _cel_pipeline_add && _cel_pipeline_alpha)
      {
        // mvp * translate(camera): columns 0..2 unchanged, column 3 shifted by the camera.
        float mvp_cam[16];
        std::memcpy(mvp_cam, mvp16, 16 * sizeof(float));
        for (int r = 0; r < 4; ++r)
        {
          mvp_cam[12 + r] = mvp16[0 + r] * _sky_camera[0]
                          + mvp16[4 + r] * _sky_camera[1]
                          + mvp16[8 + r] * _sky_camera[2]
                          + mvp16[12 + r];
        }

        VkDescriptorSet cel_sets[2] = { _dset, _tt_dset };
        VkPipeline bound = VK_NULL_HANDLE;
        for (Celestial const& c : _celestials)
        {
          VkPipeline const want = (c.additive > 0.5f) ? _cel_pipeline_add : _cel_pipeline_alpha;
          if (want != bound)
          {
            vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
            vkCmdBindDescriptorSets(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _cel_layout, 0, 2,
                                    cel_sets, 0, nullptr);
            bound = want;
          }
          float push[32];
          std::memcpy(push, mvp_cam, 16 * sizeof(float));
          push[16] = c.center_rel[0]; push[17] = c.center_rel[1]; push[18] = c.center_rel[2];
          push[19] = c.half_size;
          push[20] = c.right[0]; push[21] = c.right[1]; push[22] = c.right[2];
          push[23] = c.opacity;
          push[24] = c.up[0]; push[25] = c.up[1]; push[26] = c.up[2];
          push[27] = c.tex_index;
          push[28] = c.color[0]; push[29] = c.color[1]; push[30] = c.color[2];
          push[31] = c.additive;
          vkCmdPushConstants(rec, _cel_layout,
                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, sizeof(push), push);
          vkCmdDraw(rec, 4, 1, 0, 0);
        }
        static std::atomic<unsigned> s_cel_log{0};
        if ((s_cel_log++ % 300u) == 0)
        {
          std::lock_guard<std::mutex> lk(recordLogMutex());
          LogError << "[VK] celestial draws issued: " << _celestials.size() << std::endl;
        }
      }

      // [phase G] PARTICLES last: depth-tested against the finished scene, never depth-writing, and
      // in the emitter order GL issued -- particle blending is order-dependent.
      if (pass_on(128) && want(4) && !_particle_draws.empty() && _particle_layout && mvp16 && _dset && _tt_dset
          && _particle_vbo && _particle_ibo)
      {
        VkDescriptorSet part_sets[2] = { _dset, _tt_dset };
        VkDeviceSize const part_zero = 0;
        vkCmdBindVertexBuffers(rec, 0, 1, &_particle_vbo, &part_zero);
        vkCmdBindIndexBuffer(rec, _particle_ibo, 0, VK_INDEX_TYPE_UINT32);
        VkPipeline bound_p = VK_NULL_HANDLE;
        std::size_t issued = 0;
        for (ParticleDraw const& d : _particle_draws)
        {
          int bi = (d.blend >= 0.f && d.blend < 8.f) ? static_cast<int>(d.blend) : 2;
          if (d.ribbon > 0.5f && bi == 3) bi = 8;   // ribbons blend 3 as SRC_COLOR/ONE
          VkPipeline const want = _particle_pipelines[bi];
          if (!want)
            continue;
          if (want != bound_p)
          {
            vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
            vkCmdBindDescriptorSets(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _particle_layout, 0, 2,
                                    part_sets, 0, nullptr);
            bound_p = want;
          }
          float push[20];
          std::memcpy(push, mvp16, 16 * sizeof(float));
          push[16] = static_cast<float>(d.tex_index);
          push[17] = d.blend;
          push[18] = d.alpha_test;
          push[19] = d.alpha_mod;
          // Ribbon marker: +16 on the blend slot. The shader masks it off for the blend branch and
          // uses it to skip the particle-only black-fringe divide. Keeps the push at 80 bytes.
          if (d.ribbon > 0.5f) push[17] += 16.0f;
          vkCmdPushConstants(rec, _particle_layout,
                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, sizeof(push), push);
          vkCmdDrawIndexed(rec, d.index_count, 1, d.first_index, d.base_vertex, 0);
          ++issued;
        }
        static std::atomic<unsigned> s_part_log{0};
        if ((s_part_log++ % 300u) == 0)
        {
          std::lock_guard<std::mutex> lk(recordLogMutex());
          LogError << "[VK] particle draws issued: " << issued << " of " << _particle_draws.size() << std::endl;
        }
      }

      // [2026-09-04 NATIVE UI COMPOSITE] The editor overlay, LAST: after every world pass so it sits
      // on top of the finished frame. Blended, no depth, three generated vertices, and it shares the
      // particle pipeline layout so the sets bound here are exactly the ones it needs.
      // [2026-09-06 DIAG] The M2 slice discard measures from the LIGHTING-UBO camera (Camera_Pad);
    // print what this process actually uploaded there, next to the slice, once per ~5 s.
    {
      static std::atomic<int> s_cam_log{0};
      if (want(4) && (s_cam_log.fetch_add(1) % 300) == 0)
      {
        std::lock_guard<std::mutex> lk(recordLogMutex());
        LogError << "[VK] M2 GPU-side: slice=" << _m2_slice_dist
                 << " uboCam=(" << _sky_camera[0] << "," << _sky_camera[1] << "," << _sky_camera[2]
                 << ") draws=" << _m2_draw_count << std::endl;
      }
    }
    if (pass_on(512) && want(4) && _ui_pipeline && _ui_visible && _ui_tex >= 0
          && _particle_layout && _dset && _tt_dset)
      {
        vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _ui_pipeline);
        VkDescriptorSet usets[2] = { _dset, _tt_dset };
        vkCmdBindDescriptorSets(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, _particle_layout, 0, 2,
                                usets, 0, nullptr);
        float upush[20]{};
        upush[16] = static_cast<float>(_ui_tex);   // vec4.x = bindless index (the mat4 is unused)
        vkCmdPushConstants(rec, _particle_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(upush), upush);
        vkCmdDraw(rec, 3, 1, 0, 0);
      }
    }
    else
    {
      // [2026-09-02] Was: bind _pipeline (the VK-1 skeleton, test.vert/test.frag) and draw a
      // FULLSCREEN triangle. test.frag is an animated hue sweep --
      //   col = 0.5 + 0.5 * cos(pc.time + vec3(0.0, 2.1, 4.2) + rings * 3.0)
      // -- so every frame that landed here repainted the whole screen with a colour-cycling pattern
      // and skipped all the real passes with it: "everything flashes blue and all the doodads go
      // missing". It was bring-up scaffolding to prove the SPIR-V path worked end to end, and it had
      // no business running once there was a scene to draw.
      //
      // A frame with no camera matrix has nothing to contribute, so record nothing at all.
      static std::atomic<int> s_no_mvp{0};
      int const n = s_no_mvp.fetch_add(1, std::memory_order_relaxed);
      if (n == 0 || (n % 600) == 0)
      {
        std::lock_guard<std::mutex> lk(recordLogMutex());
        LogError << "[VK] recordInto with no mvp16 -- nothing recorded (occurrence " << (n + 1)
                 << "). Previously this painted the test pattern over the frame." << std::endl;
      }
    }
  }

  // [phase H] Fan the pass groups out across the recording threads and wait for them. Each worker
  // owns its command pool and writes only its own secondary buffer, so no locking is needed around
  // the recording itself -- only around the handoff.
  void VulkanBackend::recordGroups(float t, float const* mvp16)
  {
    // [VKTRACE] What each pass guard actually sees this frame. Draw guards, not timers.
    {
      static int const s_trace = []() -> int {
        char const* v = std::getenv("NOGGIT_VK_TRACE");
        return (v && *v) ? std::atoi(v) : 60;  // [VKTRACE] heartbeat; VKFLASH carries the detail
      }();
      std::uint64_t const f = g_vk_trace_frame++;
      // [VKFLASH] The reported flash happens at the GL->VK HANDOVER, in VK's first frames --
      // exactly where a median-based detector cannot arm yet. So trace the first 150 frames of
      // VK activity unconditionally, then fall back to the heartbeat.
      if (f < 150 || (s_trace > 0 && (f % static_cast<std::uint64_t>(s_trace)) == 0))
      {
        std::lock_guard<std::mutex> lk(recordLogMutex());
        LogError << "[VKTRACE] f=" << f
                 << " mvp=" << (mvp16 ? 1 : 0)
                 << " | terrain idx=" << _terrain_index_count
                 << " vis=" << _terrain_visible_chunks
                 << " ttReady=" << (_tt_ready ? 1 : 0)
                 << " ttPipe=" << (_tt_pipeline != VK_NULL_HANDLE)
                 << " ttCidx=" << (_tt_cidx != VK_NULL_HANDLE)
                 << " | m2 pipe=" << (_m2_pipeline != VK_NULL_HANDLE)
                 << " draws=" << _m2_draw_count
                 << " | wmo=" << _wmo_draws.size()
                 << " | water pipe=" << (_water_pipeline != VK_NULL_HANDLE)
                 << " idx=" << _water_index_count
                 << " | cam=" << _sky_camera[0] << "," << _sky_camera[1] << "," << _sky_camera[2]
                 << " mvpT=" << (mvp16 ? mvp16[12] : 0.f)
                 << "," << (mvp16 ? mvp16[13] : 0.f)
                 << "," << (mvp16 ? mvp16[14] : 0.f)
                 << " | lastRebuild=" << g_vk_trace_last_rebuild_frame
                 << std::endl;
      }

      // [VKFLASH] ring + collapse detection
      {
        VkTraceRow row;
        row.f = f;
        row.vis = _terrain_visible_chunks;
        row.m2 = static_cast<std::uint32_t>(_m2_draw_count);
        row.wmo = static_cast<std::uint32_t>(_wmo_draws.size());
        row.widx = _water_index_count;
        row.cam[0] = _sky_camera[0]; row.cam[1] = _sky_camera[1]; row.cam[2] = _sky_camera[2];
        row.mvp = mvp16 ? 1 : 0;
        row.rebuild = g_vk_trace_last_rebuild_frame;

        std::uint32_t const mv = vkTraceMedian(&VkTraceRow::vis);
        std::uint32_t const mm = vkTraceMedian(&VkTraceRow::m2);
        bool const collapsed = mv > 200 && mm > 50
                            && (row.vis * 4u < mv || row.m2 * 4u < mm);
        if (collapsed && f > g_vk_flash_last + 120)
        {
          g_vk_flash_last = f;
          std::lock_guard<std::mutex> lk2(recordLogMutex());
          LogError << "[VKFLASH] collapse at f=" << f
                   << " (vis " << row.vis << " vs median " << mv
                   << ", m2 " << row.m2 << " vs median " << mm
                   << "). Preceding frames:" << std::endl;
          std::size_t const n = g_vk_ring_n < 48 ? g_vk_ring_n : 48;
          std::size_t const base = g_vk_ring_n < 48 ? 0 : (g_vk_ring_n % 48);
          for (std::size_t i = 0; i < n; ++i)
          {
            VkTraceRow const& r = g_vk_ring[(base + i) % 48];
            LogError << "[VKFLASH]   f=" << r.f
                     << " vis=" << r.vis << " m2=" << r.m2
                     << " wmo=" << r.wmo << " widx=" << r.widx
                     << " mvp=" << r.mvp
                     << " cam=" << r.cam[0] << "," << r.cam[1]
                     << "," << r.cam[2]
                     << " rebuild=" << r.rebuild << std::endl;
          }
        }
        g_vk_ring[g_vk_ring_n % 48] = row;
        ++g_vk_ring_n;
      }
    }

    static int const s_threads = []() -> int {
      char const* v = std::getenv("NOGGIT_VK_RECORD_THREADS");
      return (v && *v) ? std::atoi(v) : 1;   // 0 = record inline (the single-threaded A/B)
    }();

    if (s_threads <= 0 || _recorders.size() < kGroupCount)
    {
      for (int g = 0; g < kGroupCount && static_cast<std::size_t>(g) < _recorders.size(); ++g)
        recordGroup(g, _recorders[g].cmd, t, mvp16);
      return;
    }

    startRecordThreads();
    {
      std::unique_lock<std::mutex> lock(_record_mutex);
      _record_t = t;
      _record_mvp = mvp16;
      _record_outstanding = kGroupCount;
      ++_record_epoch;
      _record_cv.notify_all();
      _record_done_cv.wait(lock, [this] { return _record_outstanding == 0; });
    }
  }

  void VulkanBackend::startRecordThreads()
  {
    if (_record_threads_started)
      return;
    _record_threads_started = true;
    _record_threads.reserve(kGroupCount);
    for (int g = 0; g < kGroupCount; ++g)
    {
      _record_threads.emplace_back([this, g]()
      {
        unsigned seen = 0;
        for (;;)
        {
          float t = 0.f;
          float const* mvp = nullptr;
          {
            std::unique_lock<std::mutex> lock(_record_mutex);
            _record_cv.wait(lock, [this, &seen] { return _record_quit || _record_epoch != seen; });
            if (_record_quit)
              return;
            seen = _record_epoch;
            t = _record_t;
            mvp = _record_mvp;
          }

          recordGroup(g, _recorders[g].cmd, t, mvp);

          {
            std::lock_guard<std::mutex> lock(_record_mutex);
            if (--_record_outstanding == 0)
              _record_done_cv.notify_one();
          }
        }
      });
    }
  }

  void VulkanBackend::stopRecordThreads()
  {
    if (!_record_threads_started)
      return;
    {
      std::lock_guard<std::mutex> lock(_record_mutex);
      _record_quit = true;
      _record_cv.notify_all();
    }
    for (auto& th : _record_threads)
      if (th.joinable())
        th.join();
    _record_threads.clear();
    _record_threads_started = false;
  }

  // The post-submit CPU fence wait is OFF by default. It was added when the cross-API semaphores were
  // believed not to order the GPU work on this driver, but with everything else fixed the semaphore
  // path measures 10/10 PARITY and the stall costs 4.7 ms/frame (18.1 -> 13.3). NOGGIT_VK_HARD_SYNC=1
  // brings it back if a driver ever needs it.
  static bool const s_no_hard_sync = []() {
    char const* v = std::getenv("NOGGIT_VK_HARD_SYNC");
    return !(v && *v && *v != '0');
  }();

  void VulkanBackend::startSubmitThread()
  {
    if (_submit_started)
      return;
    _submit_started = true;
    _submit_thread = std::thread([this]()
    {
      for (;;)
      {
        bool wait_gl = false;
        bool wait_acquire = false;
        bool signal_sem = true;
        {
          std::unique_lock<std::mutex> lock(_submit_mutex);
          _submit_cv.wait(lock, [this] { return _submit_quit || _submit_pending; });
          if (_submit_quit)
            return;
          wait_gl = _submit_wait_gl;
          wait_acquire = _submit_wait_acquire;    // [NATIVE PRESENT]
          signal_sem = _submit_signal_sem;
        }

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &_cmd;
        // [NATIVE PRESENT] binary semaphore: signal only when GL or the present will wait it
        si.signalSemaphoreCount = signal_sem ? 1u : 0u;
        si.pSignalSemaphores = signal_sem ? &_vk_done : nullptr;
        // same copy-batch wait as the inline path: the uploads run on a separate queue, so this is
        // what orders them before the draws that sample them (instead of a CPU fence wait)
        VkSemaphore waits[3];
        VkPipelineStageFlags wait_stages[3];
        std::uint32_t wait_count = 0;
        if (wait_gl) { waits[wait_count] = _gl_done; wait_stages[wait_count] = wait_stage; ++wait_count; }
        if (_copy_signalled) { waits[wait_count] = _copy_done; wait_stages[wait_count] = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; ++wait_count; _copy_signalled = false; }
        // [NATIVE PRESENT] the swapchain blit waits its acquire at the transfer stage
        if (wait_acquire) { waits[wait_count] = _sc_acquire; wait_stages[wait_count] = VK_PIPELINE_STAGE_TRANSFER_BIT; ++wait_count; }
        if (wait_count)
        {
          si.waitSemaphoreCount = wait_count;
          si.pWaitSemaphores = waits;
          si.pWaitDstStageMask = wait_stages;
        }
        VkResult const r = vkQueueSubmit(_queue, 1, &si, _fence);
        // [NATIVE PRESENT] present as soon as the work is queued -- before any optional CPU sync
        if (r == VK_SUCCESS)
          presentAcquiredImage();
        else
          _sc_acquired = false;
        if (r == VK_SUCCESS && !s_no_hard_sync)
        {
          // [phase A HARD SYNC] the cross-API semaphores were found not to order the GPU work on this
          // driver, so the frame is completed on the CPU before GL reads the images. This CPU stall is
          // the single largest remaining cost (~7 ms/frame): it drains the GPU pipeline twice per
          // frame and stops VK and GL overlapping at all. NOGGIT_VK_NO_HARD_SYNC=1 drops it and
          // relies on the _vk_done semaphore alone -- the A/B that says whether this driver really
          // needs the stall.
          vkWaitForFences(_device, 1, &_fence, VK_TRUE, UINT64_MAX);
        }

        {
          std::lock_guard<std::mutex> lock(_submit_mutex);
          _submit_result = r;
          _submit_pending = false;
          _submit_done_cv.notify_all();
        }
      }
    });
  }

  void VulkanBackend::stopSubmitThread()
  {
    if (!_submit_started)
      return;
    waitFrameComplete();
    {
      std::lock_guard<std::mutex> lock(_submit_mutex);
      _submit_quit = true;
      _submit_cv.notify_all();
    }
    if (_submit_thread.joinable())
      _submit_thread.join();
    _submit_started = false;
  }

  void VulkanBackend::submitFrameAsync(bool wait_gl_done)
  {
    startSubmitThread();
    std::lock_guard<std::mutex> lock(_submit_mutex);
    _submit_wait_gl = wait_gl_done;
    _submit_wait_acquire = _sc_acquired;                              // [NATIVE PRESENT]
    _submit_signal_sem = _frame_sem_external_wait || _sc_acquired;    // [NATIVE PRESENT]
    _submit_result = VK_SUCCESS;
    _submit_pending = true;
    _submit_cv.notify_one();
  }

  bool VulkanBackend::waitFrameComplete()
  {
    SlowCall _sc_waitFrameComplete("waitFrameComplete");
    if (!_submit_started)
      return true;
    // Time spent HERE is GPU time: the submit thread has already handed the work over, so this is the
    // caller blocking on the GPU finishing. Recording time vs this is what says CPU-bound or GPU-bound.
    auto const t0 = std::chrono::steady_clock::now();
    {
      std::unique_lock<std::mutex> lock(_submit_mutex);
      _submit_done_cv.wait(lock, [this] { return !_submit_pending; });
    }
    _stat_gpu_wait_ms += std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - t0).count();

    tickRetired();   // free anything a grow retired, now that it can no longer be in flight

    if (_ts_pool && _ts_period_ns > 0.f)
    {
      std::uint64_t ts[2] = { 0, 0 };
      if (vkGetQueryPoolResults(_device, _ts_pool, 0, 2, sizeof(ts), ts, sizeof(std::uint64_t),
                                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0])
      {
        _stat_gpu_ms += static_cast<double>(ts[1] - ts[0]) * _ts_period_ns / 1.0e6;
      }
    }

    if (++_stat_frames >= 300)
    {
      LogError << "[VK] GPU TIME (timestamps): " << (_stat_gpu_ms / _stat_frames)
               << " ms/frame" << std::endl;
      _stat_gpu_ms = 0.0;
      LogError << "[VK] cost/frame: record=" << (_stat_record_ms / _stat_frames)
               << " ms  gpuWait=" << (_stat_gpu_wait_ms / _stat_frames)
               // terrainIdx/waterIdx are the BUFFER sizes, not what is drawn -- the draw goes
               // through the indirect path and only touches the visible chunks. Reading them as a
               // draw count reads "VK draws the whole neighbourhood every frame", which is false;
               // print what is actually issued next to them.
               << " ms  | terrainVis=" << _terrain_visible_chunks
               << "/" << (_terrain_index_count / 768u) << " chunks"
               << " waterVis=" << _water_visible_chunks
               << " (bufIdx terrain=" << _terrain_index_count
               << " water=" << _water_index_count << ")"
               << " m2Draws=" << _m2_draw_count
               << " wmoDraws=" << _wmo_draws.size() << std::endl;
      _stat_frames = 0;
      _stat_record_ms = 0.0;
      _stat_gpu_wait_ms = 0.0;
    }
    return _submit_result == VK_SUCCESS;
  }

  // ---- [VULKAN NATIVE PRESENT, 2026-09-03] --------------------------------------------------------
  // The backend owns presentation: a surface on a native child window, a swapchain, a per-frame
  // blit recorded into the frame's own command buffer, and vkQueuePresentKHR right after the
  // submit. The offscreen render target stays exactly as it was (exportable; the parity harness's
  // GL import keeps working) -- presentation is a pure consumer of it.

  bool VulkanBackend::initPresent(void* hwnd)
  {
    if (_present_active)
      return true;
    if (!_present_capable || !_device || !hwnd)
      return false;
    _present_hwnd = hwnd;
    if (!_surface)
    {
      VkWin32SurfaceCreateInfoKHR sci{};
      sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
      sci.hinstance = ::GetModuleHandleA(nullptr);
      sci.hwnd = static_cast<HWND>(hwnd);
      if (reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(_pfn_vkCreateWin32SurfaceKHR)(_instance, &sci, nullptr, &_surface) != VK_SUCCESS)
      {
        LogError << "[VK] vkCreateWin32SurfaceKHR failed -- native present unavailable" << std::endl;
        _present_capable = false;
        return false;
      }
      VkBool32 supported = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(_phys, _queue_family, _surface, &supported);
      if (!supported)
      {
        LogError << "[VK] graphics queue family cannot present -- native present unavailable" << std::endl;
        vkDestroySurfaceKHR(_instance, _surface, nullptr);
        _surface = VK_NULL_HANDLE;
        _present_capable = false;
        return false;
      }
    }
    if (!_sc_acquire)
    {
      VkSemaphoreCreateInfo semci{};
      semci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      if (vkCreateSemaphore(_device, &semci, nullptr, &_sc_acquire) != VK_SUCCESS)
      {
        LogError << "[VK] acquire semaphore create failed" << std::endl;
        return false;
      }
    }
    if (!createSwapchainInternal())
      return false;
    _present_active = true;
    LogError << "[VK] NATIVE PRESENT active: swapchain " << _sc_extent.width << "x" << _sc_extent.height
             << " format=" << _sc_format << " images=" << _sc_images.size() << std::endl;
    return true;
  }

  bool VulkanBackend::createSwapchainInternal()
  {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_phys, _surface, &caps) != VK_SUCCESS)
    {
      LogError << "[VK] surface capabilities query failed" << std::endl;
      return false;
    }
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu)   // surface lets the application choose
      ext = { _width, _height };
    if (ext.width == 0 || ext.height == 0)
    {
      _sc_needs_recreate = true;    // minimised: retry when the window has a size again
      return false;
    }
    auto const clampv = [](std::uint32_t v, std::uint32_t lo, std::uint32_t hi)
    { return v < lo ? lo : (hi && v > hi ? hi : v); };
    ext.width = clampv(ext.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    ext.height = clampv(ext.height, caps.minImageExtent.height, caps.maxImageExtent.height);

    // format: prefer 8-bit UNORM -- the frame is already display-referred, an SRGB target would
    // re-encode it and shift every colour
    std::uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(_phys, _surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fn);
    if (fn)
      vkGetPhysicalDeviceSurfaceFormatsKHR(_phys, _surface, &fn, fmts.data());
    VkSurfaceFormatKHR pick{};
    pick.format = VK_FORMAT_B8G8R8A8_UNORM;
    pick.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (!fmts.empty())
    {
      pick = fmts[0];
      for (auto const& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)
        {
          pick = f;
          break;
        }
    }

    // present mode: MAILBOX (uncapped, tear-free) > IMMEDIATE (uncapped) > FIFO (always there).
    // FIFO would cap the editor AND the benches at vsync -- last resort only.
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    {
      std::uint32_t pn = 0;
      vkGetPhysicalDeviceSurfacePresentModesKHR(_phys, _surface, &pn, nullptr);
      std::vector<VkPresentModeKHR> modes(pn);
      if (pn)
        vkGetPhysicalDeviceSurfacePresentModesKHR(_phys, _surface, &pn, modes.data());
      for (auto m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) { mode = m; break; }
      if (mode == VK_PRESENT_MODE_FIFO_KHR)
        for (auto m : modes)
          if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) { mode = m; break; }
    }

    std::uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount)
      count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = _surface;
    sci.minImageCount = count;
    sci.imageFormat = pick.format;
    sci.imageColorSpace = pick.colorSpace;
    sci.imageExtent = ext;
    sci.imageArrayLayers = 1;
    // [DIAG] TRANSFER_SRC when the surface allows it, so readbackPresented() can capture exactly
    // what the window shows. Universal on desktop; absent -> present-readback simply unavailable.
    _sc_transfer_src = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                   | (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                   | (_sc_transfer_src ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                           ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                           : static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1u));
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = _swapchain;

    VkSwapchainKHR ns = VK_NULL_HANDLE;
    VkResult const r = vkCreateSwapchainKHR(_device, &sci, nullptr, &ns);
    if (r != VK_SUCCESS)
    {
      LogError << "[VK] vkCreateSwapchainKHR failed: " << vkres(r) << std::endl;
      return false;
    }
    if (_swapchain)
      vkDestroySwapchainKHR(_device, _swapchain, nullptr); // retired via oldSwapchain; queue idled by callers on recreate
    _swapchain = ns;
    _sc_format = pick.format;
    _sc_extent = ext;
    std::uint32_t n = 0;
    vkGetSwapchainImagesKHR(_device, _swapchain, &n, nullptr);
    _sc_images.resize(n);
    if (n)
      vkGetSwapchainImagesKHR(_device, _swapchain, &n, _sc_images.data());
    _sc_needs_recreate = false;
    _sc_acquired = false;
    LogError << "[VK] swapchain: " << ext.width << "x" << ext.height << " x" << n
             << " mode=" << (mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX"
                             : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "FIFO")
             << std::endl;
    return true;
  }

  // Called right after vkQueueSubmit on whichever thread submitted (queue access is externally
  // synchronised there). No-op unless renderFrame acquired an image this frame.
  void VulkanBackend::presentAcquiredImage()
  {
    if (!_present_active || !_sc_acquired)
      return;
    _sc_acquired = false;
    _sc_presented_once = true;   // [DIAG] a readback of _sc_index is now meaningful
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &_vk_done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &_swapchain;
    pi.pImageIndices = &_sc_index;
    VkResult const r = vkQueuePresentKHR(_queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    {
      _sc_needs_recreate = true;
    }
    else if (r != VK_SUCCESS)
    {
      static int s_err = 0;
      if (s_err++ < 8)
        LogError << "[VK] vkQueuePresentKHR failed: " << vkres(r) << std::endl;
    }
  }

  bool VulkanBackend::readbackImage(std::vector<std::uint8_t>& out)
  {
    if (!_ready || !vkCmdCopyImageToBuffer || !vkFreeCommandBuffers)
      return false;
    waitFrameComplete();
    vkQueueWaitIdle(_queue); // capture cadence: a full sync is fine

    std::size_t const bytes = std::size_t(_width) * std::size_t(_height) * 4u;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    {
      VkBufferCreateInfo bci{};
      bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bci.size = bytes;
      bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (vkCreateBuffer(_device, &bci, nullptr, &buf) != VK_SUCCESS)
        return false;
      VkMemoryRequirements req{};
      vkGetBufferMemoryRequirements(_device, buf, &req);
      VkPhysicalDeviceMemoryProperties mp{};
      vkGetPhysicalDeviceMemoryProperties(_phys, &mp);
      std::uint32_t type = UINT32_MAX;
      for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i))
            && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
          type = i;
          break;
        }
      VkMemoryAllocateInfo mai{};
      mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      mai.allocationSize = req.size;
      mai.memoryTypeIndex = type;
      if (type == UINT32_MAX || vkAllocateMemory(_device, &mai, nullptr, &mem) != VK_SUCCESS
          || vkBindBufferMemory(_device, buf, mem, 0) != VK_SUCCESS)
      {
        if (mem) vkFreeMemory(_device, mem, nullptr);
        vkDestroyBuffer(_device, buf, nullptr);
        return false;
      }
    }

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = _pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer c = VK_NULL_HANDLE;
    bool ok = vkAllocateCommandBuffers(_device, &ai, &c) == VK_SUCCESS;
    if (ok)
    {
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      vkBeginCommandBuffer(c, &bi);
      VkBufferImageCopy rc{};
      rc.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      rc.imageExtent = { _width, _height, 1 };
      vkCmdCopyImageToBuffer(c, _image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &rc);
      vkEndCommandBuffer(c);
      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &c;
      ok = vkQueueSubmit(_queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS
        && vkQueueWaitIdle(_queue) == VK_SUCCESS;
    }
    if (ok)
    {
      void* p = nullptr;
      ok = vkMapMemory(_device, mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS && p;
      if (ok)
      {
        out.resize(bytes);
        std::memcpy(out.data(), p, bytes);
        vkUnmapMemory(_device, mem);
      }
    }
    if (c)
      vkFreeCommandBuffers(_device, _pool, 1, &c);
    vkDestroyBuffer(_device, buf, nullptr);
    vkFreeMemory(_device, mem, nullptr);
    return ok;
  }

  bool VulkanBackend::readbackPresented(std::vector<std::uint8_t>& out)
  {
    if (!_ready || !_present_active || !_sc_transfer_src || !_sc_presented_once
        || _sc_index >= _sc_images.size() || !vkCmdCopyImageToBuffer || !vkFreeCommandBuffers)
      return false;
    waitFrameComplete();
    vkQueueWaitIdle(_queue);
    VkImage const src = _sc_images[_sc_index];   // last presented, layout PRESENT_SRC_KHR
    std::size_t const bytes = std::size_t(_sc_extent.width) * _sc_extent.height * 4u;

    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(_device, &bci, nullptr, &buf) != VK_SUCCESS)
      return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(_device, buf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(_phys, &mp);
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((req.memoryTypeBits & (1u << i))
          && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
          && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
      { type = i; break; }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(_device, &mai, nullptr, &mem) != VK_SUCCESS
        || vkBindBufferMemory(_device, buf, mem, 0) != VK_SUCCESS)
    {
      if (mem) vkFreeMemory(_device, mem, nullptr);
      vkDestroyBuffer(_device, buf, nullptr);
      return false;
    }

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = _pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer c = VK_NULL_HANDLE;
    bool ok = vkAllocateCommandBuffers(_device, &ai, &c) == VK_SUCCESS;
    if (ok)
    {
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      vkBeginCommandBuffer(c, &bi);
      VkImageMemoryBarrier b{};
      b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      b.srcAccessMask = 0;
      b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = src;
      b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &b);
      VkBufferImageCopy rc{};
      rc.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      rc.imageExtent = { _sc_extent.width, _sc_extent.height, 1 };
      vkCmdCopyImageToBuffer(c, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rc);
      VkImageMemoryBarrier back = b;
      back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      back.dstAccessMask = 0;
      back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &back);
      vkEndCommandBuffer(c);
      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &c;
      ok = vkQueueSubmit(_queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS
        && vkQueueWaitIdle(_queue) == VK_SUCCESS;
    }
    if (ok)
    {
      void* p = nullptr;
      ok = vkMapMemory(_device, mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS && p;
      if (ok)
      {
        // swapchain is B8G8R8A8; emit RGBA8 BOTTOM-UP (savePng mirrors vertically like GL rows)
        std::uint32_t const w = _sc_extent.width, h = _sc_extent.height;
        out.resize(bytes);
        auto const* srcpx = static_cast<std::uint8_t const*>(p);
        bool const bgra = (_sc_format == VK_FORMAT_B8G8R8A8_UNORM
                        || _sc_format == VK_FORMAT_B8G8R8A8_SRGB);
        for (std::uint32_t y = 0; y < h; ++y)
        {
          std::uint8_t const* srow = srcpx + std::size_t(y) * w * 4u;
          std::uint8_t* drow = out.data() + std::size_t(h - 1u - y) * w * 4u;
          for (std::uint32_t x = 0; x < w; ++x)
          {
            std::uint8_t const b0 = srow[x * 4 + 0], g0 = srow[x * 4 + 1],
                               r0 = srow[x * 4 + 2], a0 = srow[x * 4 + 3];
            drow[x * 4 + 0] = bgra ? r0 : b0;
            drow[x * 4 + 1] = g0;
            drow[x * 4 + 2] = bgra ? b0 : r0;
            drow[x * 4 + 3] = a0;
          }
        }
        vkUnmapMemory(_device, mem);
      }
    }
    if (c)
      vkFreeCommandBuffers(_device, _pool, 1, &c);
    vkDestroyBuffer(_device, buf, nullptr);
    vkFreeMemory(_device, mem, nullptr);
    return ok;
  }

  bool VulkanBackend::renderFrame(float t, bool wait_gl_done, float const* mvp16)
  {
    SlowCall _sc_renderFrame("renderFrame");
    if (!_ready)
      return false;

    // Flush pending texture uploads BEFORE recording. Their descriptors were written as soon as the
    // texture was added (the array is UPDATE_AFTER_BIND), so if the copies were still unsubmitted
    // this frame could sample an image that is still in UNDEFINED layout. Flushing here closes that
    // window: everything recorded below is resident. One submit for the whole batch.
    flushTextureUploads();

    // CPU pacing: the previous frame must be fully done before _cmd and the fence are reused. With
    // the submit thread that means joining it first -- it owns both the submit and the fence wait.
    waitFrameComplete();
    // [NATIVE PRESENT] with no GL wait anywhere in the native frame, a submit-thread failure would
    // otherwise spin silently forever -- surface it here (the thread has finished: pending==false).
    if (_submit_started && _submit_result != VK_SUCCESS)
    {
      LogError << "[VK] previous submit failed: " << vkres(_submit_result)
               << " -- backend going inert" << std::endl;
      _ready = false;
      return false;
    }
    vkWaitForFences(_device, 1, &_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(_device, 1, &_fence);

    // [NATIVE PRESENT] Acquire the swapchain image this frame blits into. After the fence wait
    // above exactly ONE frame is in flight, so the single acquire semaphore is free again.
    bool present_this_frame = false;
    if (_present_active)
    {
      if (_sc_needs_recreate)
      {
        vkQueueWaitIdle(_queue);       // resize is rare; make the old chain safe to retire
        createSwapchainInternal();     // a zero-sized window leaves the flag set -- retried later
      }
      if (!_sc_needs_recreate && _swapchain)
      {
        VkResult ar = vkAcquireNextImageKHR(_device, _swapchain, UINT64_MAX, _sc_acquire,
                                            VK_NULL_HANDLE, &_sc_index);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR)
        {
          vkQueueWaitIdle(_queue);
          if (createSwapchainInternal())
            ar = vkAcquireNextImageKHR(_device, _swapchain, UINT64_MAX, _sc_acquire,
                                       VK_NULL_HANDLE, &_sc_index);
        }
        if (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR)
        {
          if (ar == VK_SUBOPTIMAL_KHR)
            _sc_needs_recreate = true; // present this frame, rebuild before the next
          present_this_frame = true;
        }
        else
        {
          static int s_aq_err = 0;
          if (s_aq_err++ < 8)
            LogError << "[VK] vkAcquireNextImageKHR failed: " << vkres(ar) << std::endl;
        }
      }
    }
    _sc_acquired = present_this_frame;

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(_cmd, &bi);

    // Render pass handles UNDEFINED->attachment->GENERAL layouts (colour) + the depth clear. With a terrain
    // mesh uploaded this draws the REAL scene geometry depth-tested with the live camera MVP; before the
    // first mesh, the fullscreen ring pattern proves the skeleton.
    VkClearValue clears[3]{};
    // [2026-09-02] Was an OPAQUE SKY-BLUE (0.35, 0.55, 0.80, 1.0) -- "sky-ish backdrop behind the
    // terrain", scaffolding from before there was a real skydome/cloud pass. VK clears its colour
    // attachment to this EVERY FRAME and composes it into GL, so anywhere VK contributes no geometry
    // the result is solid blue. At the GL->VK handover VK has almost nothing yet (traced: vis=0 and
    // wmo=0 for its first frames while ownership has already switched), which is the reported
    // "everything flashes blue the moment the map loads". It also keeps doodads blue-holed for the
    // whole session, because compose mode never feeds them (setDoodads is skipped when compose_ok,
    // MapView.cpp; traced doodad=0 on all 1383 frames) while GL is gated off from drawing them.
    //
    // Clear to TRANSPARENT BLACK so a texel VK never wrote composes as nothing instead of as sky.
    clears[0].color.float32[0] = 0.0f;
    clears[0].color.float32[1] = 0.0f;
    clears[0].color.float32[2] = 0.0f;
    clears[0].color.float32[3] = 0.0f;
    clears[1].color.float32[0] = 1.0f; // depth-as-colour background = far (GL compose discards >= 1)
    clears[2].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = _render_pass;
    rbi.framebuffer = _framebuffer;
    rbi.renderArea.extent = { _width, _height };
    rbi.clearValueCount = 3;
    rbi.pClearValues = clears;
    // [phase H] the pass body lives in a SECONDARY command buffer so recording can be split across
    // threads; the primary only executes them, in order.
    if (_recorders.size() < kGroupCount && !createRecorders(kGroupCount))
      return false;

    // Begin every group's secondary buffer up front: vkBeginCommandBuffer touches the owning POOL,
    // so doing it here (one thread) keeps the workers to pure vkCmd* recording on their own buffers.
    VkCommandBufferInheritanceInfo inh{};
    inh.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inh.renderPass = _render_pass;
    inh.subpass = 0;
    inh.framebuffer = _framebuffer;
    VkCommandBufferBeginInfo sbi{};
    sbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    sbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
              | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    sbi.pInheritanceInfo = &inh;
    for (int g = 0; g < kGroupCount; ++g)
    {
      // Reset the POOL, not just the buffer: a per-buffer reset returns the memory to its pool but
      // lets the pool keep growing its block list across a long session.
      vkResetCommandPool(_device, _recorders[g].pool, 0);
      vkBeginCommandBuffer(_recorders[g].cmd, &sbi);
    }

    if (_ts_pool)
    {
      vkCmdResetQueryPool(_cmd, _ts_pool, 0, 2);
      vkCmdWriteTimestamp(_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, _ts_pool, 0);
    }
    vkCmdBeginRenderPass(_cmd, &rbi, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    // [phase H] record the pass groups -- in parallel when the pool is up, otherwise inline.
    auto const t_rec0 = std::chrono::steady_clock::now();
    recordGroups(t, mvp16);
    _stat_record_ms += std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t_rec0).count();
    VkCommandBuffer secs[kGroupCount];
    for (int g = 0; g < kGroupCount; ++g)
    {
      // [2026-09-04 DIAG] A secondary that fails to END is INVALID, and executing it silently drops
      // everything recorded in it -- the whole group. Group 4 carries water, celestials, particles
      // and the UI overlay, and none of them were verifiably rendering. Never ignore this result.
      VkResult const er = vkEndCommandBuffer(_recorders[g].cmd);
      if (er != VK_SUCCESS)
      {
        static std::atomic<int> s_end_err{0};
        if (s_end_err.fetch_add(1) < 10)
          LogError << "[VK] SECONDARY END FAILED group=" << g << " res=" << vkres(er)
                   << " -- every draw in this group is discarded" << std::endl;
      }
      secs[g] = _recorders[g].cmd;
    }
    // Executed in GROUP ORDER, which is the order the single-threaded path recorded them in --
    // that is what preserves the blending of the overlay passes.
    vkCmdExecuteCommands(_cmd, static_cast<std::uint32_t>(kGroupCount), secs);

    vkCmdEndRenderPass(_cmd);
    if (_ts_pool)
      vkCmdWriteTimestamp(_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _ts_pool, 1);

    // [NATIVE PRESENT] blit the finished frame into the acquired swapchain image, inside the SAME
    // command buffer: one submit per frame, and the present just waits the frame semaphore.
    if (present_this_frame)
    {
      VkImage const dst = _sc_images[_sc_index];
      VkImageMemoryBarrier bar[2]{};
      bar[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      bar[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      bar[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      bar[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;   // render pass finalLayout
      bar[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
      bar[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      bar[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      bar[0].image = _image;
      bar[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      bar[1] = bar[0];
      bar[1].srcAccessMask = 0;
      bar[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      bar[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // previous contents replaced wholesale
      bar[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      bar[1].image = dst;
      vkCmdPipelineBarrier(_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, bar);
      VkImageBlit blit{};
      blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      blit.dstSubresource = blit.srcSubresource;
      // our image is GL-oriented (row 0 = BOTTOM row); the swapchain shows row 0 at the TOP -> flip
      blit.srcOffsets[0] = { 0, static_cast<std::int32_t>(_height), 0 };
      blit.srcOffsets[1] = { static_cast<std::int32_t>(_width), 0, 1 };
      blit.dstOffsets[0] = { 0, 0, 0 };
      blit.dstOffsets[1] = { static_cast<std::int32_t>(_sc_extent.width),
                             static_cast<std::int32_t>(_sc_extent.height), 1 };
      vkCmdBlitImage(_cmd, _image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &blit, VK_FILTER_LINEAR);
      VkImageMemoryBarrier to_present = bar[1];
      to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_present.dstAccessMask = 0;
      to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      vkCmdPipelineBarrier(_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &to_present);
    }
    _image_initialized = true;

    // [overnight 04:11] throttled content-stats line for morning diagnosis (every ~900 frames)
    {
      static unsigned s_stat_tick = 0;
      if ((s_stat_tick++ % 900u) == 0)
      {
        LogError << "[VK] frame content: terrainIdx=" << _terrain_index_count
                 << " waterIdx=" << _water_index_count
                 << " sky=" << (_sky_pipeline ? 1 : 0)
                 << " tex=" << (_dset ? 1 : 0) << std::endl;
      }
    }

    vkEndCommandBuffer(_cmd);

    // [phase H] Hand the finished primary buffer to the SUBMIT THREAD. vkQueueSubmit and the fence
    // wait both block inside the driver; doing them here made the GL thread pay for them before it
    // could do anything else. The caller now waits at waitFrameComplete(), immediately before it
    // reads the images -- everything it does in between overlaps with the GPU.
    static bool const s_submit_thread = []() {
      char const* v = std::getenv("NOGGIT_VK_SUBMIT_THREAD");
      return !(v && *v == '0');   // 0 = submit inline (the A/B for this change)
    }();

    if (!s_submit_thread)
    {
      VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &_cmd;
      // [NATIVE PRESENT] _vk_done is BINARY: signal only when someone will consume it (GL in
      // compose mode, the present when an image was acquired). Signalling with no waiter would
      // leave it signalled and make the NEXT signal invalid.
      bool const signal_sem = _frame_sem_external_wait || _sc_acquired;
      si.signalSemaphoreCount = signal_sem ? 1u : 0u;
      si.pSignalSemaphores = signal_sem ? &_vk_done : nullptr;
      // Wait on the texture-copy batch too when one was submitted this frame: the copy runs on a
      // SEPARATE queue, so nothing else orders it before the draws that sample those textures.
      VkSemaphore waits[3];
      VkPipelineStageFlags wait_stages[3];
      std::uint32_t wait_count = 0;
      if (wait_gl_done) { waits[wait_count] = _gl_done; wait_stages[wait_count] = wait_stage; ++wait_count; }
      if (_copy_signalled) { waits[wait_count] = _copy_done; wait_stages[wait_count] = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; ++wait_count; _copy_signalled = false; }
      // [NATIVE PRESENT] the blit into the swapchain image must wait for its acquire
      if (_sc_acquired) { waits[wait_count] = _sc_acquire; wait_stages[wait_count] = VK_PIPELINE_STAGE_TRANSFER_BIT; ++wait_count; }
      if (wait_count)
      {
        si.waitSemaphoreCount = wait_count;
        si.pWaitSemaphores = waits;
        si.pWaitDstStageMask = wait_stages;
      }
      VkResult const r = vkQueueSubmit(_queue, 1, &si, _fence);
      if (r != VK_SUCCESS)
      {
        _sc_acquired = false;   // nothing to present; the backend is going inert anyway
        LogError << "[VK] submit failed: " << vkres(r) << " -- backend going inert" << std::endl;
        _ready = false;
        return false;
      }
      presentAcquiredImage();   // [NATIVE PRESENT] no-op unless an image was acquired this frame
      vkWaitForFences(_device, 1, &_fence, VK_TRUE, UINT64_MAX);
      return true;
    }

    submitFrameAsync(wait_gl_done);
    return true;
  }

  void VulkanBackend::shutdown()
  {
    if (_device && vkDeviceWaitIdle)
      vkDeviceWaitIdle(_device);
    if (_device)
    {
      // [phase C]
      for (VkPipeline& p : _m2_pipelines)
      {
        if (p) vkDestroyPipeline(_device, p, nullptr);
        p = VK_NULL_HANDLE;
      }
      for (VkPipeline& pl : _wmo_pipelines)
      {
        if (pl) vkDestroyPipeline(_device, pl, nullptr);
        pl = VK_NULL_HANDLE;
      }
      _wmo_pipeline = VK_NULL_HANDLE;
      if (_wmo_layout) { vkDestroyPipelineLayout(_device, _wmo_layout, nullptr); _wmo_layout = VK_NULL_HANDLE; }
      if (_wmo_pool) { vkDestroyDescriptorPool(_device, _wmo_pool, nullptr); _wmo_pool = VK_NULL_HANDLE; }
      if (_wmo_dsl) { vkDestroyDescriptorSetLayout(_device, _wmo_dsl, nullptr); _wmo_dsl = VK_NULL_HANDLE; }
      for (auto* pair : { &_wmo_vbo, &_wmo_ibo, &_wmo_xforms, &_wmo_batches, &_wmo_amb })
        if (*pair) { vkDestroyBuffer(_device, *pair, nullptr); *pair = VK_NULL_HANDLE; }
      for (auto* m : { &_wmo_vbo_mem, &_wmo_ibo_mem, &_wmo_xforms_mem, &_wmo_batches_mem, &_wmo_amb_mem })
        if (*m) { vkFreeMemory(_device, *m, nullptr); *m = VK_NULL_HANDLE; }

      // [CLUTTER PERSISTENT]
      for (ClutterChunkBuf& cc : _clutter_chunks)
        if (cc.used)
        {
          vkDestroyBuffer(_device, cc.buf, nullptr);
          vkFreeMemory(_device, cc.mem, nullptr);
          cc = ClutterChunkBuf{};
        }
      if (_clutter_indirect) vkDestroyBuffer(_device, _clutter_indirect, nullptr);
      if (_clutter_indirect_mem) vkFreeMemory(_device, _clutter_indirect_mem, nullptr);

      _m2_pipeline = VK_NULL_HANDLE;
      if (_m2_layout) vkDestroyPipelineLayout(_device, _m2_layout, nullptr);
      for (auto* pair : { &_m2_vbo, &_m2_ibo, &_m2_inst_tf, &_m2_inst_in, &_m2_inst_tx, &_m2_inst_st, &_m2_bones, &_m2_indirect })
        if (*pair) vkDestroyBuffer(_device, *pair, nullptr);
      for (auto* m : { &_m2_vbo_mem, &_m2_ibo_mem, &_m2_inst_tf_mem, &_m2_inst_in_mem, &_m2_inst_tx_mem,
                       &_m2_inst_st_mem, &_m2_bones_mem, &_m2_indirect_mem })
        if (*m) vkFreeMemory(_device, *m, nullptr);
      // [phase B]
      if (_tt_pipeline) vkDestroyPipeline(_device, _tt_pipeline, nullptr);
      if (_tt_layout) vkDestroyPipelineLayout(_device, _tt_layout, nullptr);
      if (_tt_cidx) vkDestroyBuffer(_device, _tt_cidx, nullptr);
      if (_tt_cidx_mem) vkFreeMemory(_device, _tt_cidx_mem, nullptr);
      if (_light_ubo_map) vkUnmapMemory(_device, _light_ubo_mem);
      if (_light_ubo) vkDestroyBuffer(_device, _light_ubo, nullptr);
      if (_light_ubo_mem) vkFreeMemory(_device, _light_ubo_mem, nullptr);
      if (_chunk_ssbo) vkDestroyBuffer(_device, _chunk_ssbo, nullptr);
      if (_chunk_ssbo_mem) vkFreeMemory(_device, _chunk_ssbo_mem, nullptr);
      if (_alpha_view) vkDestroyImageView(_device, _alpha_view, nullptr);
      if (_alpha_image) vkDestroyImage(_device, _alpha_image, nullptr);
      if (_alpha_mem) vkFreeMemory(_device, _alpha_mem, nullptr);
      if (_shadow_view) vkDestroyImageView(_device, _shadow_view, nullptr);
      if (_shadow_image) vkDestroyImage(_device, _shadow_image, nullptr);
      if (_shadow_mem) vkFreeMemory(_device, _shadow_mem, nullptr);
      for (TexEntry& e : _textures)
      {
        if (e.view) vkDestroyImageView(_device, e.view, nullptr);
        if (e.image) vkDestroyImage(_device, e.image, nullptr);
        if (e.mem) vkFreeMemory(_device, e.mem, nullptr);
      }
      _textures.clear();
      if (_tt_dpool) vkDestroyDescriptorPool(_device, _tt_dpool, nullptr);
      if (_tt_dsl) vkDestroyDescriptorSetLayout(_device, _tt_dsl, nullptr);
      if (_tile_sampler) vkDestroySampler(_device, _tile_sampler, nullptr);
      if (_atlas_sampler) vkDestroySampler(_device, _atlas_sampler, nullptr);
      if (_ground_view) vkDestroyImageView(_device, _ground_view, nullptr);
      if (_ground_image) vkDestroyImage(_device, _ground_image, nullptr);
      if (_ground_mem) vkFreeMemory(_device, _ground_mem, nullptr);
      if (_dpool) vkDestroyDescriptorPool(_device, _dpool, nullptr); // frees _dset with it
      if (_dsl) vkDestroyDescriptorSetLayout(_device, _dsl, nullptr);
      if (_sampler) vkDestroySampler(_device, _sampler, nullptr);
      if (_water_pipeline) vkDestroyPipeline(_device, _water_pipeline, nullptr);
      if (_water_layout) { vkDestroyPipelineLayout(_device, _water_layout, nullptr); _water_layout = VK_NULL_HANDLE; }
      if (_water_indirect) vkDestroyBuffer(_device, _water_indirect, nullptr);
      if (_water_indirect_mem) vkFreeMemory(_device, _water_indirect_mem, nullptr);
      if (_terrain_indirect) vkDestroyBuffer(_device, _terrain_indirect, nullptr);
      if (_terrain_indirect_mem) vkFreeMemory(_device, _terrain_indirect_mem, nullptr);
      if (_wliq_vbo) vkDestroyBuffer(_device, _wliq_vbo, nullptr);
      if (_wliq_vbo_mem) vkFreeMemory(_device, _wliq_vbo_mem, nullptr);
      if (_wliq_ibo) vkDestroyBuffer(_device, _wliq_ibo, nullptr);
      if (_wliq_ibo_mem) vkFreeMemory(_device, _wliq_ibo_mem, nullptr);
      if (_water_vbo) vkDestroyBuffer(_device, _water_vbo, nullptr);
      if (_water_vbo_mem) vkFreeMemory(_device, _water_vbo_mem, nullptr);
      if (_water_ibo) vkDestroyBuffer(_device, _water_ibo, nullptr);
      if (_water_ibo_mem) vkFreeMemory(_device, _water_ibo_mem, nullptr);
      if (_terrain_vbo) vkDestroyBuffer(_device, _terrain_vbo, nullptr);
      if (_terrain_vbo_mem) vkFreeMemory(_device, _terrain_vbo_mem, nullptr);
      if (_terrain_ibo) vkDestroyBuffer(_device, _terrain_ibo, nullptr);
      if (_terrain_ibo_mem) vkFreeMemory(_device, _terrain_ibo_mem, nullptr);
      if (_terrain_pipeline) vkDestroyPipeline(_device, _terrain_pipeline, nullptr);
      if (_terrain_layout) vkDestroyPipelineLayout(_device, _terrain_layout, nullptr);
      if (_depth_view) vkDestroyImageView(_device, _depth_view, nullptr);
      if (_depth_image) vkDestroyImage(_device, _depth_image, nullptr);
      if (_depth_mem) vkFreeMemory(_device, _depth_mem, nullptr);
      // [MSAA]
      if (_ms_color_view) vkDestroyImageView(_device, _ms_color_view, nullptr);
      if (_ms_color_image) vkDestroyImage(_device, _ms_color_image, nullptr);
      if (_ms_color_mem) vkFreeMemory(_device, _ms_color_mem, nullptr);
      if (_ms_z_view) vkDestroyImageView(_device, _ms_z_view, nullptr);
      if (_ms_z_image) vkDestroyImage(_device, _ms_z_image, nullptr);
      if (_ms_z_mem) vkFreeMemory(_device, _ms_z_mem, nullptr);
      if (_sky_pipeline) vkDestroyPipeline(_device, _sky_pipeline, nullptr);
      if (_skydome_pipeline) vkDestroyPipeline(_device, _skydome_pipeline, nullptr);
      if (_horizon_pipeline) vkDestroyPipeline(_device, _horizon_pipeline, nullptr);
      if (_horizon_pipeline_nowrite) vkDestroyPipeline(_device, _horizon_pipeline_nowrite, nullptr);
      if (_cloud_pipeline) vkDestroyPipeline(_device, _cloud_pipeline, nullptr);
      for (VkPipeline& pp : _particle_pipelines)
        if (pp) { vkDestroyPipeline(_device, pp, nullptr); pp = VK_NULL_HANDLE; }
      if (_copy_fence) vkDestroyFence(_device, _copy_fence, nullptr);
      if (_copy_pool) vkDestroyCommandPool(_device, _copy_pool, nullptr);
      savePipelineCache();
      if (_pipeline_cache && vkDestroyPipelineCache)
        vkDestroyPipelineCache(_device, _pipeline_cache, nullptr);
      stopSubmitThread();
      stopRecordThreads();
      destroyRecorders();
      if (_particle_layout) vkDestroyPipelineLayout(_device, _particle_layout, nullptr);
      if (_particle_vbo) vkDestroyBuffer(_device, _particle_vbo, nullptr);
      if (_particle_vbo_mem) vkFreeMemory(_device, _particle_vbo_mem, nullptr);
      if (_particle_ibo) vkDestroyBuffer(_device, _particle_ibo, nullptr);
      if (_particle_ibo_mem) vkFreeMemory(_device, _particle_ibo_mem, nullptr);
      if (_cel_pipeline_add) vkDestroyPipeline(_device, _cel_pipeline_add, nullptr);
      if (_cel_pipeline_alpha) vkDestroyPipeline(_device, _cel_pipeline_alpha, nullptr);
      if (_cel_layout) vkDestroyPipelineLayout(_device, _cel_layout, nullptr);
      if (_cloud_layout) vkDestroyPipelineLayout(_device, _cloud_layout, nullptr);
      if (_cloud_vbo) vkDestroyBuffer(_device, _cloud_vbo, nullptr);
      if (_cloud_vbo_mem) vkFreeMemory(_device, _cloud_vbo_mem, nullptr);
      if (_cloud_ibo) vkDestroyBuffer(_device, _cloud_ibo, nullptr);
      if (_cloud_ibo_mem) vkFreeMemory(_device, _cloud_ibo_mem, nullptr);
      if (_skydome_layout) vkDestroyPipelineLayout(_device, _skydome_layout, nullptr);
      if (_skydome_vbo) vkDestroyBuffer(_device, _skydome_vbo, nullptr);
      if (_skydome_vbo_mem) vkFreeMemory(_device, _skydome_vbo_mem, nullptr);
      if (_skydome_ibo) vkDestroyBuffer(_device, _skydome_ibo, nullptr);
      if (_skydome_ibo_mem) vkFreeMemory(_device, _skydome_ibo_mem, nullptr);
      if (_horizon_layout) vkDestroyPipelineLayout(_device, _horizon_layout, nullptr);
      if (_horizon_vbo) vkDestroyBuffer(_device, _horizon_vbo, nullptr);
      if (_horizon_vbo_mem) vkFreeMemory(_device, _horizon_vbo_mem, nullptr);
      if (_horizon_ibo) vkDestroyBuffer(_device, _horizon_ibo, nullptr);
      if (_horizon_ibo_mem) vkFreeMemory(_device, _horizon_ibo_mem, nullptr);
      if (_pipeline) vkDestroyPipeline(_device, _pipeline, nullptr);
      if (_pipe_layout) vkDestroyPipelineLayout(_device, _pipe_layout, nullptr);
      if (_framebuffer) vkDestroyFramebuffer(_device, _framebuffer, nullptr);
      if (_render_pass) vkDestroyRenderPass(_device, _render_pass, nullptr);
      if (_z_view) vkDestroyImageView(_device, _z_view, nullptr);
      if (_z_image) vkDestroyImage(_device, _z_image, nullptr);
      if (_z_mem) vkFreeMemory(_device, _z_mem, nullptr);
      if (_view) vkDestroyImageView(_device, _view, nullptr);
      if (_gl_done) vkDestroySemaphore(_device, _gl_done, nullptr);
      if (_vk_done) vkDestroySemaphore(_device, _vk_done, nullptr);
      if (_image) vkDestroyImage(_device, _image, nullptr);
      if (_image_mem) vkFreeMemory(_device, _image_mem, nullptr);
      if (_fence) vkDestroyFence(_device, _fence, nullptr);
      if (_pool) vkDestroyCommandPool(_device, _pool, nullptr);
      // [NATIVE PRESENT] swapchain objects go before the device (surface after, it is instance-level)
      if (_swapchain && vkDestroySwapchainKHR)
        vkDestroySwapchainKHR(_device, _swapchain, nullptr);
      _swapchain = VK_NULL_HANDLE;
      if (_sc_acquire)
        vkDestroySemaphore(_device, _sc_acquire, nullptr);
      _sc_acquire = VK_NULL_HANDLE;
      _present_active = false;
      vkDestroyDevice(_device, nullptr);
      _device = VK_NULL_HANDLE;
    }
    if (_surface && _instance && vkDestroySurfaceKHR)
    {
      vkDestroySurfaceKHR(_instance, _surface, nullptr);
      _surface = VK_NULL_HANDLE;
    }
    if (_instance && vkDestroyInstance)
    {
      vkDestroyInstance(_instance, nullptr);
      _instance = VK_NULL_HANDLE;
    }
    if (_dll)
    {
      ::FreeLibrary(static_cast<HMODULE>(_dll));
      _dll = nullptr;
    }
    _ready = false;
  }
}

#endif // _WIN32
