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

    // One colour attachment. initialLayout UNDEFINED + loadOp CLEAR every frame (previous contents replaced),
    // finalLayout GENERAL = the cross-API interop layout GL waits on. No explicit barriers needed.
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R8G8B8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkRenderPassCreateInfo rci{};
    rci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rci.attachmentCount = 1;
    rci.pAttachments = &att;
    rci.subpassCount = 1;
    rci.pSubpasses = &sub;
    if (vkCreateRenderPass(_device, &rci, nullptr, &_render_pass) != VK_SUCCESS)
      return false;

    VkFramebufferCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = _render_pass;
    fci.attachmentCount = 1;
    fci.pAttachments = &_view;
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
    pci.layout = _pipe_layout;
    pci.renderPass = _render_pass;
    VkResult const r = vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pci, nullptr, &_pipeline);
    vkDestroyShaderModule(_device, vs, nullptr);
    vkDestroyShaderModule(_device, fs, nullptr);
    if (r != VK_SUCCESS)
    {
      LogError << "[VK] graphics pipeline create failed: " << vkres(r) << std::endl;
      return false;
    }
    return true;
  }

  bool VulkanBackend::init(std::uint32_t width, std::uint32_t height)
  {
    _width = width;
    _height = height;
    if (!loadLoader() || !createInstance() || !pickDeviceAndQueue() || !createDevice()
        || !createSharedImage() || !createSemaphores() || !createRenderTarget() || !createPipeline())
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

  bool VulkanBackend::renderTestFrame(float t, bool wait_gl_done)
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

    // Full graphics pass: render pass handles UNDEFINED->attachment->GENERAL layouts; the fullscreen pipeline
    // draws the animated pattern with the frame clock in push constants. This is the same skeleton the real
    // scene passes (terrain, M2 batches) will extend with descriptors + geometry.
    VkClearValue clear{};
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = _render_pass;
    rbi.framebuffer = _framebuffer;
    rbi.renderArea.extent = { _width, _height };
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(_cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline);
    float push[4] = { t, 0.f, 0.f, 0.f };
    vkCmdPushConstants(_cmd, _pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(_cmd, 3, 1, 0, 0); // fullscreen triangle
    vkCmdEndRenderPass(_cmd);
    _image_initialized = true;

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
