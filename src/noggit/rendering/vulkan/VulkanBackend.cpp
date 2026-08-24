// This file is part of Noggit3, licensed under GNU General Public License (version 3).
// [VULKAN PHASE 0] See VulkanBackend.hpp for the architecture. Everything here fails SOFT: any error logs
// once and leaves the backend inert (ready()==false) so the GL renderer is never disturbed.
#ifdef _WIN32

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

    VkResult const r = vkCreateInstance(&ci, nullptr, &_instance);
    if (r != VK_SUCCESS)
    {
      LogError << "[VK] vkCreateInstance failed: " << vkres(r) << std::endl;
      return false;
    }

    auto ld = [&](char const* n) { return _vkGetInstanceProcAddr(_instance, n); };
    vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(ld("vkDestroyInstance"));
    vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(ld("vkEnumeratePhysicalDevices"));
    vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(ld("vkGetPhysicalDeviceProperties"));
    vkGetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(ld("vkGetPhysicalDeviceQueueFamilyProperties"));
    vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(ld("vkGetPhysicalDeviceMemoryProperties"));
    vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(ld("vkCreateDevice"));
    vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(ld("vkGetDeviceProcAddr"));
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
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    char const* exts[] = {
      "VK_KHR_external_memory",
      "VK_KHR_external_memory_win32",
      "VK_KHR_external_semaphore",
      "VK_KHR_external_semaphore_win32",
      "VK_KHR_dedicated_allocation",
      "VK_KHR_get_memory_requirements2",
    };

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = static_cast<std::uint32_t>(std::size(exts));
    ci.ppEnabledExtensionNames = exts;

    VkResult const r = vkCreateDevice(_phys, &ci, nullptr, &_device);
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

  bool VulkanBackend::createSharedImage()
  {
    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &ext;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = { _width, _height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(_device, &ici, nullptr, &_image) != VK_SUCCESS)
    {
      LogError << "[VK] shared image create failed" << std::endl;
      return false;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(_device, _image, &req);
    _image_mem_size = req.size;

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
    ded.image = _image;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &ded;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(_device, &mai, nullptr, &_image_mem) != VK_SUCCESS)
    {
      LogError << "[VK] shared image memory alloc failed" << std::endl;
      return false;
    }
    if (vkBindImageMemory(_device, _image, _image_mem, 0) != VK_SUCCESS)
      return false;

    VkMemoryGetWin32HandleInfoKHR gh{};
    gh.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    gh.memory = _image_mem;
    gh.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    HANDLE h = nullptr;
    if (reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(_pfn_vkGetMemoryWin32HandleKHR)(_device, &gh, &h) != VK_SUCCESS)
    {
      LogError << "[VK] vkGetMemoryWin32HandleKHR failed" << std::endl;
      return false;
    }
    _image_mem_handle = h;
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
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
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

    if (!createDepthTarget())
      return false;

    // Colour + depth. initialLayout UNDEFINED + loadOp CLEAR every frame (previous contents replaced);
    // colour finalLayout GENERAL = the cross-API interop layout GL waits on. No explicit barriers needed.
    VkAttachmentDescription atts[2]{};
    atts[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    atts[1].format = VK_FORMAT_D32_SFLOAT;
    atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference cref{};
    cref.attachment = 0;
    cref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference dref{};
    dref.attachment = 1;
    dref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;

    VkRenderPassCreateInfo rci{};
    rci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rci.attachmentCount = 2;
    rci.pAttachments = atts;
    rci.subpassCount = 1;
    rci.pSubpasses = &sub;
    if (vkCreateRenderPass(_device, &rci, nullptr, &_render_pass) != VK_SUCCESS)
      return false;

    VkImageView views[2] = { _view, _depth_view };
    VkFramebufferCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = _render_pass;
    fci.attachmentCount = 2;
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
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

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
    VkResult const r = vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pci, nullptr, &_pipeline);
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
      VkResult const rs2 = vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pci, nullptr, &_sky_pipeline);
      vkDestroyShaderModule(_device, sky_fs, nullptr);
      if (rs2 != VK_SUCCESS)
      {
        LogError << "[VK] sky pipeline create failed: " << vkres(rs2) << " (terrain draws over flat clear)" << std::endl;
        _sky_pipeline = VK_NULL_HANDLE; // soft: sky is optional
      }
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
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

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
    VkResult const r = vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pci, nullptr, &_terrain_pipeline);
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
      ds.depthWriteEnable = VK_FALSE;
      cba.blendEnable = VK_TRUE;
      cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      cba.colorBlendOp = VK_BLEND_OP_ADD;
      cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      cba.alphaBlendOp = VK_BLEND_OP_ADD;
      VkResult const rw = vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pci, nullptr, &_water_pipeline);
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
  bool VulkanBackend::createDoodadPipeline()
  {
    VkShaderModule vs = loadShaderModule("doodad.vert.spv");
    VkShaderModule fs = loadShaderModule("doodad.frag.spv");
    if (!vs || !fs)
    {
      if (vs) vkDestroyShaderModule(_device, vs, nullptr);
      if (fs) vkDestroyShaderModule(_device, fs, nullptr);
      LogError << "[VK] doodad shaders missing (doodads skipped)" << std::endl;
      return true; // soft-optional
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
    binds[0].binding = 0;
    binds[0].stride = 24;
    binds[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    binds[1].binding = 1;
    binds[1].stride = 64;
    binds[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    VkVertexInputAttributeDescription attrs[6]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 };
    for (std::uint32_t i = 0; i < 4; ++i)
      attrs[2 + i] = { 2 + i, 1, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16 };
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 2;
    vin.pVertexBindingDescriptions = binds;
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
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

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
    pci.layout = _terrain_layout; // same push block
    pci.renderPass = _render_pass;
    VkResult const r = vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pci, nullptr, &_doodad_pipeline);
    vkDestroyShaderModule(_device, vs, nullptr);
    vkDestroyShaderModule(_device, fs, nullptr);
    if (r != VK_SUCCESS)
    {
      LogError << "[VK] doodad pipeline create failed: " << vkres(r) << " (doodads skipped)" << std::endl;
      _doodad_pipeline = VK_NULL_HANDLE;
    }
    return true;
  }

  bool VulkanBackend::setDoodads(float const* pn_verts, std::size_t vertex_count,
                                 std::uint32_t const* indices, std::size_t index_count,
                                 float const* instance_mat4s, std::size_t instance_count,
                                 DoodadDraw const* draws, std::size_t draw_count)
  {
    if (!_ready)
      return false;

    vkQueueWaitIdle(_queue);
    auto kill = [&](VkBuffer& b, VkDeviceMemory& m)
    {
      if (b) { vkDestroyBuffer(_device, b, nullptr); b = VK_NULL_HANDLE; }
      if (m) { vkFreeMemory(_device, m, nullptr); m = VK_NULL_HANDLE; }
    };
    kill(_doodad_vbo, _doodad_vbo_mem);
    kill(_doodad_ibo, _doodad_ibo_mem);
    kill(_doodad_inst, _doodad_inst_mem);
    _doodad_draws.clear();

    if (!vertex_count || !index_count || !instance_count || !draw_count)
      return true; // nothing to draw here

    if (!createHostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_count * 24, _doodad_vbo, _doodad_vbo_mem, pn_verts)
        || !createHostBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_count * 4, _doodad_ibo, _doodad_ibo_mem, indices)
        || !createHostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, instance_count * 64, _doodad_inst, _doodad_inst_mem, instance_mat4s))
    {
      LogError << "[VK] doodad upload failed" << std::endl;
      kill(_doodad_vbo, _doodad_vbo_mem);
      kill(_doodad_ibo, _doodad_ibo_mem);
      kill(_doodad_inst, _doodad_inst_mem);
      return false;
    }
    _doodad_draws.assign(draws, draws + draw_count);
    LogError << "[VK] doodads: " << draw_count << " models / " << instance_count
             << " instances / " << vertex_count << " verts" << std::endl;
    return true;
  }

  bool VulkanBackend::setWaterMesh(float const* pos_normal_interleaved, std::size_t vertex_count,
                                   std::uint32_t const* indices, std::size_t index_count)
  {
    if (!_ready)
      return false;

    vkQueueWaitIdle(_queue);
    if (_water_vbo) { vkDestroyBuffer(_device, _water_vbo, nullptr); _water_vbo = VK_NULL_HANDLE; }
    if (_water_vbo_mem) { vkFreeMemory(_device, _water_vbo_mem, nullptr); _water_vbo_mem = VK_NULL_HANDLE; }
    if (_water_ibo) { vkDestroyBuffer(_device, _water_ibo, nullptr); _water_ibo = VK_NULL_HANDLE; }
    if (_water_ibo_mem) { vkFreeMemory(_device, _water_ibo_mem, nullptr); _water_ibo_mem = VK_NULL_HANDLE; }
    _water_index_count = 0;

    if (!vertex_count || !index_count)
      return true; // legitimately no water in this neighbourhood

    if (!createHostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_count * 36, _water_vbo, _water_vbo_mem, pos_normal_interleaved)
        || !createHostBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_count * 4, _water_ibo, _water_ibo_mem, indices))
    {
      LogError << "[VK] water mesh upload failed" << std::endl;
      return false;
    }
    _water_index_count = static_cast<std::uint32_t>(index_count);
    LogError << "[VK] water mesh: " << vertex_count << " verts / " << index_count << " indices" << std::endl;
    return true;
  }

  bool VulkanBackend::createHostBuffer(VkBufferUsageFlags usage, std::size_t bytes,
                                       VkBuffer& buf, VkDeviceMemory& mem, void const* data)
  {
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
    std::memcpy(mapped, data, bytes);
    vkUnmapMemory(_device, mem);
    return true;
  }

  bool VulkanBackend::setTerrainMesh(float const* pos_normal_interleaved, std::size_t vertex_count,
                                     std::uint32_t const* indices, std::size_t index_count)
  {
    if (!_ready || !vertex_count || !index_count)
      return false;

    // editor-cadence upload (tile change): safe to idle the queue and replace the buffers outright
    vkQueueWaitIdle(_queue);
    if (_terrain_vbo) { vkDestroyBuffer(_device, _terrain_vbo, nullptr); _terrain_vbo = VK_NULL_HANDLE; }
    if (_terrain_vbo_mem) { vkFreeMemory(_device, _terrain_vbo_mem, nullptr); _terrain_vbo_mem = VK_NULL_HANDLE; }
    if (_terrain_ibo) { vkDestroyBuffer(_device, _terrain_ibo, nullptr); _terrain_ibo = VK_NULL_HANDLE; }
    if (_terrain_ibo_mem) { vkFreeMemory(_device, _terrain_ibo_mem, nullptr); _terrain_ibo_mem = VK_NULL_HANDLE; }
    _terrain_index_count = 0;

    if (!createHostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_count * 36, _terrain_vbo, _terrain_vbo_mem, pos_normal_interleaved)
        || !createHostBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_count * 4, _terrain_ibo, _terrain_ibo_mem, indices))
    {
      LogError << "[VK] terrain mesh upload failed" << std::endl;
      return false;
    }
    _terrain_index_count = static_cast<std::uint32_t>(index_count);
    LogError << "[VK] terrain mesh: " << vertex_count << " verts / " << index_count << " indices" << std::endl;
    return true;
  }

  bool VulkanBackend::init(std::uint32_t width, std::uint32_t height)
  {
    _width = width;
    _height = height;
    if (!loadLoader() || !createInstance() || !pickDeviceAndQueue() || !createDevice()
        || !createSharedImage() || !createSemaphores() || !createRenderTarget() || !createPipeline()
        || !createTextureInfra() || !createTerrainPipeline() || !createDoodadPipeline())
    {
      LogError << "[VK] init failed -- staying on pure GL" << std::endl;
      return false;
    }
    LogError << "[VK] backend ready: " << _width << "x" << _height
             << " shared image (" << (_image_mem_size / 1024) << " KB) + semaphores + GRAPHICS PIPELINE (SPIR-V)"
             << std::endl;
    _ready = true;
    return true;
  }

  bool VulkanBackend::renderFrame(float t, bool wait_gl_done, float const* mvp16)
  {
    if (!_ready)
      return false;

    // CPU pacing: wait last submission's fence (also guarantees _cmd is reusable).
    vkWaitForFences(_device, 1, &_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(_device, 1, &_fence);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(_cmd, &bi);

    // Render pass handles UNDEFINED->attachment->GENERAL layouts (colour) + the depth clear. With a terrain
    // mesh uploaded this draws the REAL scene geometry depth-tested with the live camera MVP; before the
    // first mesh, the fullscreen ring pattern proves the skeleton.
    VkClearValue clears[2]{};
    clears[0].color.float32[0] = 0.35f; // sky-ish backdrop behind the terrain
    clears[0].color.float32[1] = 0.55f;
    clears[0].color.float32[2] = 0.80f;
    clears[0].color.float32[3] = 1.0f;
    clears[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = _render_pass;
    rbi.framebuffer = _framebuffer;
    rbi.renderArea.extent = { _width, _height };
    rbi.clearValueCount = 2;
    rbi.pClearValues = clears;
    vkCmdBeginRenderPass(_cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (_terrain_index_count && mvp16)
    {
      // sky gradient behind the terrain (no depth interaction; terrain depth-tests over it)
      if (_sky_pipeline)
      {
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _sky_pipeline);
        float sky_push[4] = { t, 0.f, 0.f, 0.f };
        vkCmdPushConstants(_cmd, _pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(sky_push), sky_push);
        vkCmdDraw(_cmd, 3, 1, 0, 0);
      }
      vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _terrain_pipeline);
      if (_dset)
      {
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _terrain_layout, 0, 1, &_dset, 0, nullptr);
      }
      float push[20]; // mat4 (16) + time + pad
      std::memcpy(push, mvp16, 16 * sizeof(float));
      push[16] = t;
      push[17] = push[18] = push[19] = 0.f;
      vkCmdPushConstants(_cmd, _terrain_layout,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
      VkDeviceSize const zero = 0;
      vkCmdBindVertexBuffers(_cmd, 0, 1, &_terrain_vbo, &zero);
      vkCmdBindIndexBuffer(_cmd, _terrain_ibo, 0, VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(_cmd, _terrain_index_count, 1, 0, 0, 0);

      // doodads (opaque, instanced) after the terrain
      if (_doodad_pipeline && !_doodad_draws.empty())
      {
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _doodad_pipeline);
        VkBuffer vbs[2] = { _doodad_vbo, _doodad_inst };
        VkDeviceSize offs[2] = { 0, 0 };
        vkCmdBindVertexBuffers(_cmd, 0, 2, vbs, offs);
        vkCmdBindIndexBuffer(_cmd, _doodad_ibo, 0, VK_INDEX_TYPE_UINT32);
        for (DoodadDraw const& d : _doodad_draws)
        {
          vkCmdDrawIndexed(_cmd, d.index_count, d.instance_count, d.first_index, d.base_vertex, d.first_instance);
        }
      }

      // water after the terrain (translucent; same layout so the push constants above still apply)
      if (_water_pipeline && _water_index_count)
      {
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _water_pipeline);
        vkCmdBindVertexBuffers(_cmd, 0, 1, &_water_vbo, &zero);
        vkCmdBindIndexBuffer(_cmd, _water_ibo, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(_cmd, _water_index_count, 1, 0, 0, 0);
      }
    }
    else
    {
      vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline);
      float push[4] = { t, 0.f, 0.f, 0.f };
      vkCmdPushConstants(_cmd, _pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
      vkCmdDraw(_cmd, 3, 1, 0, 0); // fullscreen triangle
    }
    vkCmdEndRenderPass(_cmd);
    _image_initialized = true;

    // [overnight 04:11] throttled content-stats line for morning diagnosis (every ~900 frames)
    {
      static unsigned s_stat_tick = 0;
      if ((s_stat_tick++ % 900u) == 0)
      {
        LogError << "[VK] frame content: terrainIdx=" << _terrain_index_count
                 << " waterIdx=" << _water_index_count
                 << " doodadDraws=" << _doodad_draws.size()
                 << " sky=" << (_sky_pipeline ? 1 : 0)
                 << " tex=" << (_dset ? 1 : 0) << std::endl;
      }
    }

    vkEndCommandBuffer(_cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &_cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &_vk_done;
    if (wait_gl_done)
    {
      si.waitSemaphoreCount = 1;
      si.pWaitSemaphores = &_gl_done;
      si.pWaitDstStageMask = &wait_stage;
    }
    VkResult const r = vkQueueSubmit(_queue, 1, &si, _fence);
    if (r != VK_SUCCESS)
    {
      LogError << "[VK] submit failed: " << vkres(r) << " -- backend going inert" << std::endl;
      _ready = false;
      return false;
    }
    return true;
  }

  void VulkanBackend::shutdown()
  {
    if (_device && vkDeviceWaitIdle)
      vkDeviceWaitIdle(_device);
    if (_device)
    {
      if (_ground_view) vkDestroyImageView(_device, _ground_view, nullptr);
      if (_ground_image) vkDestroyImage(_device, _ground_image, nullptr);
      if (_ground_mem) vkFreeMemory(_device, _ground_mem, nullptr);
      if (_dpool) vkDestroyDescriptorPool(_device, _dpool, nullptr); // frees _dset with it
      if (_dsl) vkDestroyDescriptorSetLayout(_device, _dsl, nullptr);
      if (_sampler) vkDestroySampler(_device, _sampler, nullptr);
      if (_doodad_pipeline) vkDestroyPipeline(_device, _doodad_pipeline, nullptr);
      if (_doodad_vbo) vkDestroyBuffer(_device, _doodad_vbo, nullptr);
      if (_doodad_vbo_mem) vkFreeMemory(_device, _doodad_vbo_mem, nullptr);
      if (_doodad_ibo) vkDestroyBuffer(_device, _doodad_ibo, nullptr);
      if (_doodad_ibo_mem) vkFreeMemory(_device, _doodad_ibo_mem, nullptr);
      if (_doodad_inst) vkDestroyBuffer(_device, _doodad_inst, nullptr);
      if (_doodad_inst_mem) vkFreeMemory(_device, _doodad_inst_mem, nullptr);
      if (_water_pipeline) vkDestroyPipeline(_device, _water_pipeline, nullptr);
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
      if (_sky_pipeline) vkDestroyPipeline(_device, _sky_pipeline, nullptr);
      if (_pipeline) vkDestroyPipeline(_device, _pipeline, nullptr);
      if (_pipe_layout) vkDestroyPipelineLayout(_device, _pipe_layout, nullptr);
      if (_framebuffer) vkDestroyFramebuffer(_device, _framebuffer, nullptr);
      if (_render_pass) vkDestroyRenderPass(_device, _render_pass, nullptr);
      if (_view) vkDestroyImageView(_device, _view, nullptr);
      if (_gl_done) vkDestroySemaphore(_device, _gl_done, nullptr);
      if (_vk_done) vkDestroySemaphore(_device, _vk_done, nullptr);
      if (_image) vkDestroyImage(_device, _image, nullptr);
      if (_image_mem) vkFreeMemory(_device, _image_mem, nullptr);
      if (_fence) vkDestroyFence(_device, _fence, nullptr);
      if (_pool) vkDestroyCommandPool(_device, _pool, nullptr);
      vkDestroyDevice(_device, nullptr);
      _device = VK_NULL_HANDLE;
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
