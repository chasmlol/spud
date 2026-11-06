#include <atomic>
#include <cstdlib>
#include "render/device.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/platform.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <string_view>

namespace spud::render {

std::string_view vk_result_name(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VK_ERROR_UNKNOWN";
    }
}

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        log::error("vulkan: {}", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        log::warn("vulkan: {}", data->pMessage);
    } else {
        log::debug("vulkan: {}", data->pMessage);
    }
    return VK_FALSE;
}

void VKAPI_PTR on_vma_allocate(VmaAllocator, uint32_t type, VkDeviceMemory, VkDeviceSize size, void* user) {
    auto* ev = static_cast<Device::MemoryEvents*>(user);
    ev->allocations.fetch_add(1, std::memory_order_relaxed);
    ev->allocated_bytes.fetch_add(size, std::memory_order_relaxed);
    log::debug("vma: vkAllocateMemory {} KiB (type {})", size >> 10, type);
}

void VKAPI_PTR on_vma_free(VmaAllocator, uint32_t, VkDeviceMemory, VkDeviceSize size, void* user) {
    auto* ev = static_cast<Device::MemoryEvents*>(user);
    ev->frees.fetch_add(1, std::memory_order_relaxed);
    ev->allocated_bytes.fetch_sub(size, std::memory_order_relaxed);
}

bool has_extension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    return std::any_of(exts.begin(), exts.end(),
                       [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

} // namespace

Device::Device(const DeviceOptions& options, const std::function<VkSurfaceKHR(VkInstance)>& make_surface) {
    create_instance(options);
    if (make_surface) {
        m_surface = make_surface(m_instance);
    }
    pick_physical_device(options.need_present && m_surface != VK_NULL_HANDLE);
    create_device();

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = m_queue_family;
    vk_check(vkCreateCommandPool(m_device, &pool_info, nullptr, &m_immediate_pool));
}

Device::~Device() {
    if (m_device) {
        vkDeviceWaitIdle(m_device);
        flush_all_garbage();
        vkDestroyCommandPool(m_device, m_immediate_pool, nullptr);
        vmaDestroyAllocator(m_allocator);
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_surface) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    }
    if (m_messenger) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) {
            destroy(m_instance, m_messenger, nullptr);
        }
    }
    if (m_instance) {
        vkDestroyInstance(m_instance, nullptr);
    }
}

void Device::create_instance(const DeviceOptions& options) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

    std::vector<const char*> extensions = options.instance_extensions;
    m_debug_utils = has_extension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (m_debug_utils) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    std::vector<const char*> layers;
    if (options.validation) {
        uint32_t layer_count = 0;
        vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
        std::vector<VkLayerProperties> props(layer_count);
        vkEnumerateInstanceLayerProperties(&layer_count, props.data());
        const char* validation = "VK_LAYER_KHRONOS_validation";
        if (std::any_of(props.begin(), props.end(),
                        [&](const VkLayerProperties& p) { return std::strcmp(p.layerName, validation) == 0; })) {
            layers.push_back(validation);
            log::info("vulkan: validation layers enabled");
        } else {
            log::warn("vulkan: validation requested but VK_LAYER_KHRONOS_validation is not installed");
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "spud";
    app.applicationVersion = VK_MAKE_VERSION(0, 0, 1);
    app.pEngineName = "spud";
    app.engineVersion = VK_MAKE_VERSION(0, 0, 1);
    app.apiVersion = VK_API_VERSION_1_3;

    VkDebugUtilsMessengerCreateInfoEXT messenger_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messenger_info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger_info.pfnUserCallback = debug_callback;

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    if (m_debug_utils && !layers.empty()) {
        info.pNext = &messenger_info;
    }
    vk_check(vkCreateInstance(&info, nullptr, &m_instance));

    if (m_debug_utils && !layers.empty()) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create) {
            vk_check(create(m_instance, &messenger_info, nullptr, &m_messenger));
        }
    }
    if (m_debug_utils) {
        m_set_name = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(m_instance, "vkSetDebugUtilsObjectNameEXT"));
    }
}

void Device::pick_physical_device(bool need_present) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    int64_t best_score = -1;
    for (auto pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) {
            log::info("vulkan: skipping {} (Vulkan {}.{})", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
                      VK_API_VERSION_MINOR(props.apiVersion));
            continue;
        }
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        f12.pNext = &f13;
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f.pNext = &f12;
        vkGetPhysicalDeviceFeatures2(pd, &f);
        if (!f13.dynamicRendering || !f13.synchronization2 || !f12.bufferDeviceAddress ||
            !f12.descriptorBindingPartiallyBound || !f12.runtimeDescriptorArray ||
            !f12.shaderSampledImageArrayNonUniformIndexing || !f12.scalarBlockLayout ||
            !f.features.textureCompressionBC || !f.features.multiDrawIndirect) {
            log::info("vulkan: skipping {} (missing required features)", props.deviceName);
            continue;
        }

        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, queues.data());
        int family = -1;
        for (uint32_t i = 0; i < qcount; ++i) {
            if (!(queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                continue;
            }
            if (need_present) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, m_surface, &present);
                if (!present) {
                    continue;
                }
            }
            family = static_cast<int>(i);
            break;
        }
        if (family < 0) {
            log::info("vulkan: skipping {} (no suitable queue)", props.deviceName);
            continue;
        }

        VkPhysicalDeviceMemoryProperties mem;
        vkGetPhysicalDeviceMemoryProperties(pd, &mem);
        uint64_t local = 0;
        for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
            if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                local = std::max<uint64_t>(local, mem.memoryHeaps[i].size);
            }
        }
        int64_t score = static_cast<int64_t>(local >> 20);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += int64_t(1) << 40;
        } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score += int64_t(1) << 30;
        }
        log::info("vulkan: candidate {} ({} MiB device-local)", props.deviceName, local >> 20);
        if (score > best_score) {
            best_score = score;
            m_physical = pd;
            m_queue_family = static_cast<uint32_t>(family);
            m_properties = props;
        }
    }
    if (!m_physical) {
        throw Error("no Vulkan 1.3 GPU with the required features found");
    }
    m_name = m_properties.deviceName;
    log::info("vulkan: using {} (driver {:x}, API {}.{}.{})", m_name, m_properties.driverVersion,
              VK_API_VERSION_MAJOR(m_properties.apiVersion), VK_API_VERSION_MINOR(m_properties.apiVersion),
              VK_API_VERSION_PATCH(m_properties.apiVersion));
}

namespace {
Device* g_lost_device = nullptr;
} // namespace

void Device::report_lost() {
    log::error("gpu: device lost");
    if (auto get = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(vkGetDeviceProcAddr(m_device, "vkGetDeviceFaultInfoEXT"))) {
        VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        if (get(m_device, &counts, nullptr) == VK_SUCCESS || counts.addressInfoCount || counts.vendorInfoCount) {
            std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
            std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);
            VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
            info.pAddressInfos = addresses.data();
            info.pVendorInfos = vendor.data();
            counts.vendorBinarySize = 0;
            const VkResult r = get(m_device, &counts, &info);
            log::error("gpu: fault report ({}): {}", int(r), info.description);
            for (const auto& a : addresses) {
                log::error("gpu: address type {} at {:#x} precision {}", int(a.addressType), a.reportedAddress, a.addressPrecision);
            }
            for (const auto& v : vendor) {
                log::error("gpu: vendor {} code {:#x} data {:#x}", v.description, v.vendorFaultCode, v.vendorFaultData);
            }
        } else {
            log::error("gpu: no fault report");
        }
    }
    if (m_checkpoints) {
        auto get = reinterpret_cast<PFN_vkGetQueueCheckpointDataNV>(vkGetDeviceProcAddr(m_device, "vkGetQueueCheckpointDataNV"));
        uint32_t count = 0;
        get(m_queue, &count, nullptr);
        std::vector<VkCheckpointDataNV> points(count, VkCheckpointDataNV{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
        get(m_queue, &count, points.data());
        for (const auto& p : points) {
            log::error("gpu: checkpoint {} reached stage {:#x}", reinterpret_cast<uintptr_t>(p.pCheckpointMarker), uint32_t(p.stage));
        }
    }
}

void Device::create_device() {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &count, available.data());

    std::vector<const char*> extensions;
    if (m_surface) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    const bool memory_budget = has_extension(available, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (memory_budget) {
        extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    }
    // When other processes oversubscribe VRAM, Windows moves allocations to system memory, and a frame
    // that reads geometry over PCIe runs 5-10x slower. Priorities make it move cached textures first.
    const bool memory_priority = has_extension(available, VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
    const bool pageable = memory_priority && has_extension(available, VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
    if (memory_priority) {
        extensions.push_back(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
    }
    if (pageable) {
        extensions.push_back(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
    }
    // SPUD_PIPELINE_STATS=1: the driver's statistics per pipeline (registers, instructions), logged at startup for
    // shader optimisation. Off by default: capturing them changes how pipelines are created.
    m_pipeline_stats = platform::env("SPUD_PIPELINE_STATS") == "1" &&
                       has_extension(available, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    if (m_pipeline_stats) {
        extensions.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    }

    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(m_physical, &supported);

    VkPhysicalDeviceMemoryPriorityFeaturesEXT priority_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT};
    priority_features.memoryPriority = VK_TRUE;
    VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT pageable_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT};
    pageable_features.pageableDeviceLocalMemory = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.maintenance4 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    f12.bufferDeviceAddress = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;
    f12.drawIndirectCount = VK_TRUE;
    f12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f12;
    f11.shaderDrawParameters = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f11;
    if (memory_priority) {
        priority_features.pNext = features.pNext;
        features.pNext = &priority_features;
    }
    if (pageable) {
        pageable_features.pNext = features.pNext;
        features.pNext = &pageable_features;
    }
    // What the driver can say when the GPU is lost: the fault, and how far each pass got. Always on: losses are too rare to
    // catch on purpose.
    const bool device_fault = has_extension(available, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    static VkPhysicalDeviceFaultFeaturesEXT fault_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    if (device_fault) {
        extensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        fault_features.deviceFault = VK_TRUE;
        fault_features.pNext = features.pNext;
        features.pNext = &fault_features;
    }
    m_checkpoints = has_extension(available, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    if (m_checkpoints) {
        extensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    }
    // SPUD_VRS=2x1: the user's side-by-side of coarse foliage shading. Off in normal play.
    static VkPhysicalDeviceFragmentShadingRateFeaturesKHR rate_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    const char* vrs = std::getenv("SPUD_VRS");
    m_coarse_foliage = vrs && std::string_view(vrs) == "2x1" &&
                       has_extension(available, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    if (m_coarse_foliage) {
        extensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
        rate_features.pipelineFragmentShadingRate = VK_TRUE;
        rate_features.pNext = features.pNext;
        features.pNext = &rate_features;
        log::info("foliage shading: 2x1");
    }
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executable_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    executable_features.pipelineExecutableInfo = VK_TRUE;
    if (m_pipeline_stats) {
        executable_features.pNext = features.pNext;
        features.pNext = &executable_features;
    }
    features.features.samplerAnisotropy = supported.features.samplerAnisotropy;
    features.features.sampleRateShading = VK_TRUE; // the composite's fog, per sample
    features.features.shaderStorageImageExtendedFormats = VK_TRUE; // the volumetric lighting's R16F volume
    features.features.fillModeNonSolid = supported.features.fillModeNonSolid;
    features.features.shaderClipDistance = VK_TRUE; // a view through a door stops at the far doorway
    features.features.depthClamp = VK_TRUE;         // and the doorway itself holds at the near plane
    features.features.depthBiasClamp = supported.features.depthBiasClamp; // decals' and membranes' bias
    features.features.independentBlend = VK_TRUE;
    features.features.multiDrawIndirect = VK_TRUE;
    features.features.drawIndirectFirstInstance = VK_TRUE;
    features.features.textureCompressionBC = VK_TRUE;
    features.features.shaderInt64 = supported.features.shaderInt64;

    // Async uploads: prefer a transfer-only family (DMA engine), else a second graphics queue.
    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> families(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qcount, families.data());
    m_transfer_family = m_queue_family;
    m_transfer_index = 0;
    bool dedicated = false;
    for (uint32_t i = 0; i < qcount; ++i) {
        const auto flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_TRANSFER_BIT) && !(flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
            m_transfer_family = i;
            dedicated = true;
            break;
        }
    }
    if (!dedicated && families[m_queue_family].queueCount > 1) {
        m_transfer_index = 1;
    }

    const float priorities[2] = {1.0f, 0.5f};
    VkDeviceQueueCreateInfo queue_infos[2]{};
    uint32_t queue_info_count = 1;
    queue_infos[0] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_infos[0].queueFamilyIndex = m_queue_family;
    queue_infos[0].queueCount = (!dedicated && m_transfer_index == 1) ? 2 : 1;
    queue_infos[0].pQueuePriorities = priorities;
    if (dedicated) {
        queue_infos[1] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_infos[1].queueFamilyIndex = m_transfer_family;
        queue_infos[1].queueCount = 1;
        queue_infos[1].pQueuePriorities = &priorities[1];
        queue_info_count = 2;
    }

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &features;
    info.queueCreateInfoCount = queue_info_count;
    info.pQueueCreateInfos = queue_infos;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    vk_check(vkCreateDevice(m_physical, &info, nullptr, &m_device));
    if (m_checkpoints) {
        m_set_checkpoint = reinterpret_cast<PFN_vkCmdSetCheckpointNV>(vkGetDeviceProcAddr(m_device, "vkCmdSetCheckpointNV"));
    }
    g_lost_device = this;
    g_device_lost_report = [] {
        static std::atomic_flag once;
        if (!once.test_and_set() && g_lost_device) {
            g_lost_device->report_lost();
        }
    };
    vkGetDeviceQueue(m_device, m_queue_family, 0, &m_queue);
    vkGetDeviceQueue(m_device, m_transfer_family, m_transfer_index, &m_transfer_queue);
    m_families[0] = m_queue_family;
    m_families[1] = m_transfer_family;
    log::info("vulkan: uploads on {}", dedicated              ? std::format("transfer family {}", m_transfer_family)
                                       : m_transfer_index == 1 ? std::string("a second graphics queue")
                                                               : std::string("the graphics queue"));

    static VmaDeviceMemoryCallbacks callbacks{};
    callbacks.pfnAllocate = on_vma_allocate;
    callbacks.pfnFree = on_vma_free;
    callbacks.pUserData = &m_memory_events;
    VmaAllocatorCreateInfo alloc_info{};
    alloc_info.pDeviceMemoryCallbacks = &callbacks;
    alloc_info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (memory_budget) {
        alloc_info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }
    if (memory_priority) {
        alloc_info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
    }
    alloc_info.vulkanApiVersion = VK_API_VERSION_1_3;
    alloc_info.instance = m_instance;
    alloc_info.physicalDevice = m_physical;
    alloc_info.device = m_device;
    vk_check(vmaCreateAllocator(&alloc_info, &m_allocator));
}

void Device::set_shared(VkImageCreateInfo& info) const {
    if (m_transfer_family != m_queue_family) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = m_families;
    } else {
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
}

namespace {

// GPU resources that carry state from one frame to the next: effect-local simulation or accumulation
// (water, volumetrics, ...), eye adaptation, caches. Add entries freely, each with a line in the table in
// docs/RENDERING.md. The final image itself must never be one of them: it is rebuilt every frame.
// "local map": the Map Menu's local view, redrawn every frame it is shown; the movie may show the last one while
// it fades out.
constexpr std::array<std::string_view, 8> k_cross_frame_allowlist{"local map", "eye adaptation",
                                                                   "precipitation occlusion", "water history",
                                                                   "water reflection", "water flow", "water ripples",
                                                                   "skin decals"};

void check_lifetime(Lifetime lifetime, const char* debug_name) {
    if (lifetime != Lifetime::simulation && lifetime != Lifetime::cache) {
        return;
    }
    const std::string_view name = debug_name ? debug_name : "";
    for (auto allowed : k_cross_frame_allowlist) {
        if (name == allowed) {
            return;
        }
    }
    throw Error(std::format("GPU resource '{}' is declared as {} state but is not in the cross-frame allowlist. "
                            "Rendered results must not carry over between frames (docs/RENDERING.md).",
                            name, to_string(lifetime)));
}

} // namespace

const char* to_string(Lifetime lifetime) {
    switch (lifetime) {
    case Lifetime::asset:
        return "asset";
    case Lifetime::frame:
        return "frame";
    case Lifetime::transfer:
        return "transfer";
    case Lifetime::simulation:
        return "simulation";
    case Lifetime::cache:
        return "cache";
    }
    return "?";
}

VkResult Device::create_image(const VkImageCreateInfo& info, const VmaAllocationCreateInfo& alloc,
                              Lifetime lifetime, const char* debug_name, VkImage& image, VmaAllocation& allocation,
                              VmaAllocationInfo* alloc_info) {
    check_lifetime(lifetime, debug_name);
    VmaAllocationCreateInfo prioritised = alloc;
    if (prioritised.priority == 0.0f) {
        // Render targets are touched by every frame; cached textures can wait in system memory first.
        prioritised.priority = lifetime == Lifetime::frame ? 1.0f : 0.5f;
    }
    const VkResult result = vmaCreateImage(m_allocator, &info, &prioritised, &image, &allocation, alloc_info);
    if (result == VK_SUCCESS && debug_name) {
        set_name(reinterpret_cast<uint64_t>(image), VK_OBJECT_TYPE_IMAGE, debug_name);
    }
    return result;
}

Buffer Device::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage memory, Lifetime lifetime,
                             const char* debug_name, bool shared) {
    check_lifetime(lifetime, debug_name);
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<VkDeviceSize>(size, 16);
    info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (shared && m_transfer_family != m_queue_family) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = m_families;
    }

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.priority = 0.5f;
    switch (memory) {
    case MemoryUsage::gpu_only:
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        alloc.priority = 1.0f; // geometry and tables every frame reads
        break;
    case MemoryUsage::upload:
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemoryUsage::dynamic:
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        alloc.priority = 1.0f; // per-frame data
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemoryUsage::readback:
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    }

    Buffer buffer;
    VmaAllocationInfo result{};
    vk_check(vmaCreateBuffer(m_allocator, &info, &alloc, &buffer.buffer, &buffer.allocation, &result));
    buffer.mapped = result.pMappedData;
    buffer.size = size;
    VkBufferDeviceAddressInfo addr{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addr.buffer = buffer.buffer;
    buffer.address = vkGetBufferDeviceAddress(m_device, &addr);
    if (debug_name) {
        set_name(reinterpret_cast<uint64_t>(buffer.buffer), VK_OBJECT_TYPE_BUFFER, debug_name);
    }
    return buffer;
}

void Device::destroy_buffer(Buffer& buffer) {
    if (buffer.buffer) {
        vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
    }
    buffer = {};
}

void Device::defer(std::function<void()> fn) { m_deferred.push_back({m_recording_serial, std::move(fn)}); }

void Device::defer_destroy(Buffer buffer) {
    if (!buffer) {
        return;
    }
    defer([this, buffer]() mutable { destroy_buffer(buffer); });
}

void Device::set_frame_serials(uint64_t recording, uint64_t completed) {
    m_recording_serial = recording;
    m_completed_serial = completed;
}

void Device::collect_garbage() {
    size_t keep = 0;
    for (size_t i = 0; i < m_deferred.size(); ++i) {
        if (m_deferred[i].serial <= m_completed_serial) {
            m_deferred[i].fn();
        } else {
            if (keep != i) {
                m_deferred[keep] = std::move(m_deferred[i]);
            }
            ++keep;
        }
    }
    m_deferred.resize(keep);
}

void Device::flush_all_garbage() {
    // Destructors may defer more work; loop until empty.
    while (!m_deferred.empty()) {
        auto items = std::move(m_deferred);
        m_deferred.clear();
        for (auto& d : items) {
            d.fn();
        }
    }
}

void Device::immediate(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = m_immediate_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vk_check(vkAllocateCommandBuffers(m_device, &alloc, &cmd));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin));
    record(cmd);
    vk_check(vkEndCommandBuffer(cmd));
    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    std::lock_guard lock(m_queue_mutex);
    vk_check(vkQueueSubmit2(m_queue, 1, &submit, VK_NULL_HANDLE));
    vk_check(vkQueueWaitIdle(m_queue));
    vkFreeCommandBuffers(m_device, m_immediate_pool, 1, &cmd);
}

void Device::set_name(uint64_t handle, VkObjectType type, const char* name) const {
    if (!m_set_name || !name) {
        return;
    }
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    m_set_name(m_device, &info);
}

void Device::memory_budget(uint64_t& used, uint64_t& budget) const {
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(m_allocator, budgets);
    const VkPhysicalDeviceMemoryProperties* mem = nullptr;
    vmaGetMemoryProperties(m_allocator, &mem);
    used = 0;
    budget = 0;
    for (uint32_t i = 0; i < mem->memoryHeapCount; ++i) {
        if (mem->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            used += budgets[i].usage;
            budget += budgets[i].budget;
        }
    }
}

} // namespace spud::render
