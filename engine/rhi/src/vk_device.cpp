// vk_device.cpp — instance / adapter / logical device / VMA bring-up and teardown.
#include "vk_device.h"

#include "aether/core/window.h"
#include "aether/rhi/diagnostics.h"
#include "aether/rhi/shader_compiler.h"
#include "vk_command_list.h"
#include "vk_convert.h"

#ifndef _WIN32
// Non-Windows surfaces go through GLFW (volk.h above defines VK_VERSION_1_0, which makes
// glfw3.h declare glfwCreateWindowSurface). Windows keeps the explicit Win32 path.
#    define GLFW_INCLUDE_NONE
#    include <GLFW/glfw3.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string_view>

namespace aether::rhi {

Result<std::unique_ptr<Device>> create_device(const DeviceDesc& desc) {
    return vk::VulkanDevice::create(desc);
}

namespace vk {
namespace {

std::string env_string(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(),
                       [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice gpu) {
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, exts.data());
    return exts;
}

std::string version_string(u32 v) {
    return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

std::string decode_driver_version(const VkPhysicalDeviceProperties& p) {
    const u32 v = p.driverVersion;
    switch (p.vendorID) {
    case 0x10DE: // NVIDIA
        return std::format("{}.{}.{}.{}", (v >> 22) & 0x3ff, (v >> 14) & 0xff, (v >> 6) & 0xff, v & 0x3f);
    case 0x8086: // Intel (Windows)
        return std::format("{}.{}", v >> 14, v & 0x3fff);
    default: return version_string(v);
    }
}

const char* device_type_name(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

// Every feature the engine needs or may use, queried and enabled as one pNext chain.
struct FeatureChain {
    VkPhysicalDeviceFeatures2                        core{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan11Features                 v11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features                 v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan13Features                 v13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceMeshShaderFeaturesEXT            mesh{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accel{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR    rtp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR              rq{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };

    bool has_mesh = false;
    bool has_rt   = false;

    // Links the structs; extension structs only when the extension exists.
    void link() {
        core.pNext = &v11;
        v11.pNext  = &v12;
        v12.pNext  = &v13;
        void** tail = &v13.pNext;
        *tail       = nullptr;
        if (has_mesh) {
            *tail      = &mesh;
            tail       = &mesh.pNext;
            mesh.pNext = nullptr;
        }
        if (has_rt) {
            *tail       = &accel;
            accel.pNext = &rtp;
            rtp.pNext   = &rq;
            rq.pNext    = nullptr;
        }
    }
};

struct Candidate {
    VkPhysicalDevice gpu    = VK_NULL_HANDLE;
    u32              family = 0;
    i64              score  = -1;
    std::string      name;
    std::string      reject_reason;
};

std::string missing_required(const FeatureChain& f) {
    std::string missing;
    auto        need = [&](VkBool32 v, const char* name) {
        if (!v) {
            missing += missing.empty() ? "" : ", ";
            missing += name;
        }
    };
    need(f.v13.dynamicRendering, "dynamicRendering");
    need(f.v13.synchronization2, "synchronization2");
    need(f.v12.timelineSemaphore, "timelineSemaphore");
    need(f.v12.bufferDeviceAddress, "bufferDeviceAddress");
    need(f.v12.scalarBlockLayout, "scalarBlockLayout");
    need(f.v12.descriptorIndexing, "descriptorIndexing");
    need(f.v12.runtimeDescriptorArray, "runtimeDescriptorArray");
    need(f.v12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");
    need(f.v12.descriptorBindingSampledImageUpdateAfterBind, "descriptorBindingSampledImageUpdateAfterBind");
    need(f.v12.descriptorBindingStorageImageUpdateAfterBind, "descriptorBindingStorageImageUpdateAfterBind");
    need(f.v12.shaderSampledImageArrayNonUniformIndexing, "shaderSampledImageArrayNonUniformIndexing");
    need(f.v12.shaderStorageImageArrayNonUniformIndexing, "shaderStorageImageArrayNonUniformIndexing");
    return missing;
}

} // namespace

// ---------------------------------------------------------------------------------------
// creation
// ---------------------------------------------------------------------------------------

Result<std::unique_ptr<Device>> VulkanDevice::create(const DeviceDesc& desc) {
    if (!desc.window) {
        return Error{ ErrorCode::InvalidArgument, "DeviceDesc::window is required" };
    }
    std::unique_ptr<VulkanDevice> device(new VulkanDevice());
    if (auto r = device->init(desc); !r) {
        return r.error(); // destructor tears down whatever was created
    }
    return std::unique_ptr<Device>(std::move(device));
}

Result<void> VulkanDevice::init(const DeviceDesc& desc) {
    desc_              = desc;
    desc_.frames_in_flight = std::clamp(desc.frames_in_flight, 1u, 4u);
    window_            = desc.window;
    vsync_             = desc.vsync;

    if (auto r = create_instance(desc_); !r) return r;
    if (auto r = create_surface(); !r) return r;
    if (auto r = pick_physical_device(); !r) return r;
    if (auto r = create_logical_device(); !r) return r;
    if (auto r = create_allocator(); !r) return r;

    load_pipeline_cache();
    create_frames();
    create_bindless(); // also the universal pipeline layout + fallback descriptors
    swapchain_dirty_ = true;
    if (!window_->minimized()) {
        recreate_swapchain();
    }
    return {};
}

Result<void> VulkanDevice::create_instance(const DeviceDesc& desc) {
    if (volkInitialize() != VK_SUCCESS) {
        return Error{ ErrorCode::Unsupported, "Vulkan loader (vulkan-1.dll) not found" };
    }
    const u32 loader_version = volkGetInstanceVersion();
    if (loader_version < VK_API_VERSION_1_3) {
        return Error{ ErrorCode::Unsupported,
                      std::format("Vulkan 1.3 loader required (found {})", version_string(loader_version)) };
    }

    // ---- layers: validation only if enumerated AND (desc OR env AE_VALIDATION=1) ----
    u32 layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
    constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
    const bool            layer_present    = std::any_of(layers.begin(), layers.end(), [&](const VkLayerProperties& l) {
        return std::strcmp(l.layerName, kValidationLayer) == 0;
    });
    const std::string env_validation = env_string("AE_VALIDATION");
    bool              want           = desc.enable_validation || env_validation == "1";
    if (env_validation == "0") {
        want = false;
    }
    validation_ = want && layer_present;
    if (want && !layer_present) {
        AE_LOG_WARN("RHI", "Vulkan validation requested but {} is not installed; continuing without it",
                    kValidationLayer);
    }

    // ---- extensions ----
    u32 ext_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> available(ext_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, available.data());

    std::vector<const char*> extensions;
    for (const char* e : window_->required_instance_extensions()) {
        extensions.push_back(e);
    }
    if (extensions.empty()) {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef _WIN32
        extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif
    }
    debug_utils_ = has_extension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debug_utils_) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    // Optional synchronization validation (env AE_VALIDATION_SYNC=1).
    bool                      sync_validation = false;
    VkValidationFeatureEnableEXT sync_enable  = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    if (validation_ && env_string("AE_VALIDATION_SYNC") == "1") {
        u32 layer_ext_count = 0;
        vkEnumerateInstanceExtensionProperties(kValidationLayer, &layer_ext_count, nullptr);
        std::vector<VkExtensionProperties> layer_exts(layer_ext_count);
        vkEnumerateInstanceExtensionProperties(kValidationLayer, &layer_ext_count, layer_exts.data());
        if (has_extension(layer_exts, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            sync_validation = true;
        }
    }

    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName   = "Aether";
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.pEngineName        = "Aether Engine";
    app.engineVersion      = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.apiVersion         = VK_API_VERSION_1_3;

    VkDebugUtilsMessengerCreateInfoEXT messenger_info = make_messenger_info();
    VkValidationFeaturesEXT            features{ VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT };
    features.enabledValidationFeatureCount = 1;
    features.pEnabledValidationFeatures    = &sync_enable;

    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo        = &app;
    ci.enabledExtensionCount   = static_cast<u32>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount       = validation_ ? 1u : 0u;
    ci.ppEnabledLayerNames     = validation_ ? &kValidationLayer : nullptr;
    // Messenger in pNext also captures vkCreateInstance / vkDestroyInstance messages.
    const void* chain = debug_utils_ ? static_cast<const void*>(&messenger_info) : nullptr;
    if (sync_validation) {
        features.pNext = chain;
        chain          = &features;
    }
    ci.pNext = chain;

    const VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        return Error{ ErrorCode::Unsupported, std::format("vkCreateInstance failed: {}", result_string(r)) };
    }
    volkLoadInstance(instance_);

    if (debug_utils_) {
        VK_CHECK(vkCreateDebugUtilsMessengerEXT(instance_, &messenger_info, nullptr, &messenger_));
    }
    mark_validation_active(validation_);
    AE_LOG_INFO("RHI", "Vulkan instance: loader {}, validation {}{}", version_string(loader_version),
                validation_ ? "ON" : "off", sync_validation ? " (+sync)" : "");
    return {};
}

Result<void> VulkanDevice::create_surface() {
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR ci{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    ci.hinstance    = static_cast<HINSTANCE>(window_->native_instance_handle());
    ci.hwnd         = static_cast<HWND>(window_->native_handle());
    const VkResult r = vkCreateWin32SurfaceKHR(instance_, &ci, nullptr, &surface_);
    if (r != VK_SUCCESS) {
        return Error{ ErrorCode::Unsupported, std::format("vkCreateWin32SurfaceKHR failed: {}", result_string(r)) };
    }
    return {};
#else
    auto* glfw_window = static_cast<GLFWwindow*>(window_->glfw_handle());
    if (glfw_window == nullptr) {
        return Error{ ErrorCode::Unsupported, "window has no GLFW handle" };
    }
    const VkResult r = glfwCreateWindowSurface(instance_, glfw_window, nullptr, &surface_);
    if (r != VK_SUCCESS) {
        return Error{ ErrorCode::Unsupported, std::format("glfwCreateWindowSurface failed: {}", result_string(r)) };
    }
    return {};
#endif
}

Result<void> VulkanDevice::pick_physical_device() {
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance_, &count, gpus.data());
    if (gpus.empty()) {
        return Error{ ErrorCode::Unsupported, "no Vulkan physical devices" };
    }
    const std::string preferred = env_string("AE_GPU"); // optional adapter-name substring

    Candidate best;
    std::string rejects;
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu, &props);
        Candidate c{ gpu, 0, -1, props.deviceName, {} };

        const auto exts = device_extensions(gpu);
        FeatureChain f;
        f.link();
        vkGetPhysicalDeviceFeatures2(gpu, &f.core);

        u32 family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families.data());
        bool found_family = false;
        for (u32 i = 0; i < family_count; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface_, &present);
            const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((families[i].queueFlags & need) == need && present) {
                c.family     = i;
                found_family = true;
                break;
            }
        }

        if (props.apiVersion < VK_API_VERSION_1_3) {
            c.reject_reason = "Vulkan 1.3 not supported";
        } else if (!has_extension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            c.reject_reason = "no VK_KHR_swapchain";
        } else if (!found_family) {
            c.reject_reason = "no graphics+compute+present queue family";
        } else if (std::string m = missing_required(f); !m.empty()) {
            c.reject_reason = "missing features: " + m;
        } else {
            c.score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3000
                      : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2000
                                                                                   : 1000;
            VkPhysicalDeviceMemoryProperties mem{};
            vkGetPhysicalDeviceMemoryProperties(gpu, &mem);
            for (u32 h = 0; h < mem.memoryHeapCount; ++h) {
                if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                    c.score += static_cast<i64>(mem.memoryHeaps[h].size >> 30); // + GiB of VRAM
                }
            }
            if (!preferred.empty() && c.name.find(preferred) != std::string::npos) {
                c.score += 1'000'000;
            }
        }
        if (c.score < 0) {
            rejects += std::format("\n  {}: {}", c.name, c.reject_reason);
        }
        if (c.score > best.score) {
            best = c;
        }
    }
    if (best.score < 0) {
        return Error{ ErrorCode::Unsupported, "no suitable GPU:" + rejects };
    }

    gpu_          = best.gpu;
    queue_family_ = best.family;

    VkPhysicalDeviceVulkan12Properties v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceProperties2        p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    p2.pNext = &v12;
    vkGetPhysicalDeviceProperties2(gpu_, &p2);
    props_       = p2.properties;
    driver_name_ = v12.driverName;
    driver_info_ = v12.driverInfo[0] ? std::string(v12.driverInfo) : decode_driver_version(props_);
    api_version_ = std::min<u32>(props_.apiVersion, VK_API_VERSION_1_3);

    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(gpu_, &mem);
    for (u32 h = 0; h < mem.memoryHeapCount; ++h) {
        if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            device_local_bytes_ = std::max<u64>(device_local_bytes_, mem.memoryHeaps[h].size);
        }
    }

    AE_LOG_INFO("RHI", "GPU: {} ({}), driver {} {}, Vulkan {}, {} MiB device-local", props_.deviceName,
                device_type_name(props_.deviceType), driver_name_, driver_info_, version_string(props_.apiVersion),
                device_local_bytes_ >> 20);
    return {};
}

Result<void> VulkanDevice::create_logical_device() {
    const auto   exts = device_extensions(gpu_);
    FeatureChain supported;
    supported.has_mesh = has_extension(exts, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    supported.has_rt   = has_extension(exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                       has_extension(exts, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) &&
                       has_extension(exts, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                       has_extension(exts, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    supported.link();
    vkGetPhysicalDeviceFeatures2(gpu_, &supported.core);
    const VkPhysicalDeviceFeatures& s = supported.core.features;

    FeatureChain enable;
    enable.has_mesh = supported.has_mesh && supported.mesh.meshShader && supported.mesh.taskShader;
    enable.has_rt   = supported.has_rt && supported.accel.accelerationStructure &&
                    supported.rtp.rayTracingPipeline && supported.rq.rayQuery;
    enable.link();
    VkPhysicalDeviceFeatures& e = enable.core.features;

    // Core 1.0 (optional; enabled when present).
    e.samplerAnisotropy                       = s.samplerAnisotropy;
    e.depthClamp                              = s.depthClamp;
    e.depthBiasClamp                          = s.depthBiasClamp;
    e.fillModeNonSolid                        = s.fillModeNonSolid;
    e.wideLines                               = s.wideLines;
    e.multiDrawIndirect                       = s.multiDrawIndirect;
    e.drawIndirectFirstInstance               = s.drawIndirectFirstInstance;
    e.imageCubeArray                          = s.imageCubeArray;
    e.independentBlend                        = s.independentBlend;
    e.geometryShader                          = s.geometryShader;
    e.sampleRateShading                       = s.sampleRateShading;
    e.fullDrawIndexUint32                     = s.fullDrawIndexUint32;
    e.textureCompressionBC                    = s.textureCompressionBC;
    e.shaderInt64                             = s.shaderInt64;
    e.shaderInt16                             = s.shaderInt16;
    e.fragmentStoresAndAtomics                = s.fragmentStoresAndAtomics;
    e.vertexPipelineStoresAndAtomics          = s.vertexPipelineStoresAndAtomics;
    e.shaderImageGatherExtended               = s.shaderImageGatherExtended;
    e.shaderStorageImageExtendedFormats       = s.shaderStorageImageExtendedFormats;
    e.shaderStorageImageReadWithoutFormat     = s.shaderStorageImageReadWithoutFormat;
    e.shaderStorageImageWriteWithoutFormat    = s.shaderStorageImageWriteWithoutFormat;
    e.shaderClipDistance                      = s.shaderClipDistance;
    e.shaderSampledImageArrayDynamicIndexing  = s.shaderSampledImageArrayDynamicIndexing;
    e.shaderStorageImageArrayDynamicIndexing  = s.shaderStorageImageArrayDynamicIndexing;

    // 1.1
    enable.v11.shaderDrawParameters     = supported.v11.shaderDrawParameters;
    enable.v11.storageBuffer16BitAccess = supported.v11.storageBuffer16BitAccess;
    enable.v11.multiview                = supported.v11.multiview;

    // 1.2: required set + useful optional ones.
    VkPhysicalDeviceVulkan12Features& e12 = enable.v12;
    const VkPhysicalDeviceVulkan12Features& s12 = supported.v12;
    e12.timelineSemaphore                            = VK_TRUE;
    e12.bufferDeviceAddress                          = VK_TRUE;
    e12.scalarBlockLayout                            = VK_TRUE;
    e12.descriptorIndexing                           = VK_TRUE;
    e12.runtimeDescriptorArray                       = VK_TRUE;
    e12.descriptorBindingPartiallyBound              = VK_TRUE;
    e12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    e12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    e12.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;
    e12.shaderStorageImageArrayNonUniformIndexing    = VK_TRUE;
    e12.descriptorBindingUpdateUnusedWhilePending    = s12.descriptorBindingUpdateUnusedWhilePending;
    e12.descriptorBindingVariableDescriptorCount     = s12.descriptorBindingVariableDescriptorCount;
    e12.drawIndirectCount                            = s12.drawIndirectCount;
    e12.hostQueryReset                               = s12.hostQueryReset;
    e12.shaderFloat16                                = s12.shaderFloat16;
    e12.shaderInt8                                   = s12.shaderInt8;
    e12.storageBuffer8BitAccess                      = s12.storageBuffer8BitAccess;
    e12.uniformAndStorageBuffer8BitAccess            = s12.uniformAndStorageBuffer8BitAccess;
    e12.samplerFilterMinmax                          = s12.samplerFilterMinmax;
    e12.samplerMirrorClampToEdge                     = s12.samplerMirrorClampToEdge;
    e12.separateDepthStencilLayouts                  = s12.separateDepthStencilLayouts;
    e12.shaderOutputViewportIndex                    = s12.shaderOutputViewportIndex;
    e12.shaderOutputLayer                            = s12.shaderOutputLayer;
    e12.vulkanMemoryModel                            = s12.vulkanMemoryModel;
    e12.vulkanMemoryModelDeviceScope                 = s12.vulkanMemoryModelDeviceScope;
    e12.shaderBufferInt64Atomics                     = s12.shaderBufferInt64Atomics;
    e12.imagelessFramebuffer                         = VK_FALSE;

    // 1.3
    enable.v13.dynamicRendering                   = VK_TRUE;
    enable.v13.synchronization2                   = VK_TRUE;
    enable.v13.maintenance4                       = supported.v13.maintenance4;
    enable.v13.shaderDemoteToHelperInvocation     = supported.v13.shaderDemoteToHelperInvocation;
    enable.v13.subgroupSizeControl                = supported.v13.subgroupSizeControl;
    enable.v13.computeFullSubgroups               = supported.v13.computeFullSubgroups;
    enable.v13.shaderTerminateInvocation          = supported.v13.shaderTerminateInvocation;
    enable.v13.shaderZeroInitializeWorkgroupMemory = supported.v13.shaderZeroInitializeWorkgroupMemory;

    std::vector<const char*> extensions{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    memory_budget_ = has_extension(exts, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (memory_budget_) {
        extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    }
    if (enable.has_mesh) {
        enable.mesh.taskShader = VK_TRUE;
        enable.mesh.meshShader = VK_TRUE;
        extensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    }
    if (enable.has_rt) {
        enable.accel.accelerationStructure = VK_TRUE;
        enable.rtp.rayTracingPipeline      = VK_TRUE;
        enable.rq.rayQuery                 = VK_TRUE;
        extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    }

    const float             priority = 1.0f;
    VkDeviceQueueCreateInfo queue_ci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queue_ci.queueFamilyIndex = queue_family_;
    queue_ci.queueCount       = 1;
    queue_ci.pQueuePriorities = &priority;

    VkDeviceCreateInfo ci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    ci.pNext                   = &enable.core;
    ci.queueCreateInfoCount    = 1;
    ci.pQueueCreateInfos       = &queue_ci;
    ci.enabledExtensionCount   = static_cast<u32>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    const VkResult r           = vkCreateDevice(gpu_, &ci, nullptr, &device_);
    if (r != VK_SUCCESS) {
        return Error{ ErrorCode::Unsupported, std::format("vkCreateDevice failed: {}", result_string(r)) };
    }
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    // Timestamps (GPU frame time).
    u32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu_, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu_, &family_count, families.data());
    timestamps_supported_ = families[queue_family_].timestampValidBits > 0 && props_.limits.timestampPeriod > 0.0f;
    timestamp_period_     = props_.limits.timestampPeriod;
    depth_bias_clamp_     = e.depthBiasClamp == VK_TRUE;

    features_.dynamic_rendering       = true;
    features_.timeline_semaphores     = true;
    features_.synchronization2        = true;
    features_.descriptor_indexing     = true;
    features_.buffer_device_address   = true;
    features_.draw_indirect_count     = e12.drawIndirectCount == VK_TRUE;
    features_.draw_indirect_first_instance = e.drawIndirectFirstInstance == VK_TRUE;
    features_.mesh_shaders            = enable.has_mesh;
    features_.ray_tracing             = enable.has_rt;
    features_.wide_lines              = e.wideLines == VK_TRUE;
    features_.fill_mode_non_solid     = e.fillModeNonSolid == VK_TRUE;
    features_.depth_clamp             = e.depthClamp == VK_TRUE;
    features_.sampler_anisotropy      = e.samplerAnisotropy == VK_TRUE;
    features_.texture_compression_bc  = e.textureCompressionBC == VK_TRUE;
    features_.max_push_constant_bytes = 128;
    features_.adapter_name            = props_.deviceName;
    features_.driver_version          = driver_info_;

    AE_LOG_INFO("RHI",
                "features: drawIndirectCount={} drawIndirectFirstInstance={} bc={} mesh={} rt={} wideLines={} fillModeNonSolid={} depthClamp={} "
                "anisotropy={} storageWriteWithoutFormat={} memoryBudget={}",
                features_.draw_indirect_count, features_.draw_indirect_first_instance,
                features_.texture_compression_bc, features_.mesh_shaders, features_.ray_tracing, features_.wide_lines,
                features_.fill_mode_non_solid, features_.depth_clamp, features_.sampler_anisotropy,
                e.shaderStorageImageWriteWithoutFormat == VK_TRUE, memory_budget_);
    return {};
}

Result<void> VulkanDevice::create_allocator() {
    VmaVulkanFunctions funcs{};
    funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    funcs.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo ci{};
    ci.flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT |
               (memory_budget_ ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0u);
    ci.physicalDevice   = gpu_;
    ci.device           = device_;
    ci.instance         = instance_;
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.pVulkanFunctions = &funcs;
    const VkResult r    = vmaCreateAllocator(&ci, &allocator_);
    if (r != VK_SUCCESS) {
        return Error{ ErrorCode::OutOfMemory, std::format("vmaCreateAllocator failed: {}", result_string(r)) };
    }
    return {};
}

void VulkanDevice::create_frames() {
    VkSemaphoreTypeCreateInfo type_ci{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type_ci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type_ci.initialValue  = 0;
    VkSemaphoreCreateInfo timeline_ci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    timeline_ci.pNext = &type_ci;
    VK_CHECK(vkCreateSemaphore(device_, &timeline_ci, nullptr, &timeline_));
    set_object_name(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<u64>(timeline_), "frame timeline");

    frames_.resize(desc_.frames_in_flight);
    for (usize i = 0; i < frames_.size(); ++i) {
        FrameSlot& f = frames_[i];
        VkCommandPoolCreateInfo pool_ci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pool_ci.queueFamilyIndex = queue_family_;
        pool_ci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VK_CHECK(vkCreateCommandPool(device_, &pool_ci, nullptr, &f.pool));

        VkCommandBufferAllocateInfo alloc{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        alloc.commandPool        = f.pool;
        alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device_, &alloc, &f.cmd));

        VkSemaphoreCreateInfo sem_ci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(device_, &sem_ci, nullptr, &f.acquire));

        if (timestamps_supported_) {
            VkQueryPoolCreateInfo q{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            q.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            q.queryCount = 2;
            VK_CHECK(vkCreateQueryPool(device_, &q, nullptr, &f.timestamps));
        }
        f.staging.init(allocator_, device_, 8ull << 20);
        f.list = std::make_unique<VulkanCommandList>(*this);

        const std::string tag = std::format("frame slot {}", i);
        set_object_name(VK_OBJECT_TYPE_COMMAND_BUFFER, reinterpret_cast<u64>(f.cmd), tag + " cmd");
        set_object_name(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<u64>(f.acquire), tag + " acquire");
    }

    VkCommandPoolCreateInfo pool_ci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool_ci.queueFamilyIndex = queue_family_;
    pool_ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(device_, &pool_ci, nullptr, &immediate_pool_));
    VkCommandBufferAllocateInfo alloc{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    alloc.commandPool        = immediate_pool_;
    alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device_, &alloc, &immediate_cmd_));
    VkFenceCreateInfo fence_ci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_CHECK(vkCreateFence(device_, &fence_ci, nullptr, &immediate_fence_));
    immediate_list_ = std::make_unique<VulkanCommandList>(*this);

    prepare_slot(frames_[0]); // uploads before the first begin_frame land in slot 0
}

// ---------------------------------------------------------------------------------------
// teardown
// ---------------------------------------------------------------------------------------

VulkanDevice::~VulkanDevice() {
    teardown();
}

void VulkanDevice::teardown() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        if (post_present_hook_) {
            AE_LOG_ERROR("RHI", "Device destroyed before rhi::imgui_shutdown(); ImGui Vulkan objects leak");
            post_present_hook_ = nullptr;
        }
        // Close any upload command buffers that were begun but never submitted.
        for (FrameSlot& f : frames_) {
            if (f.upload_open) {
                vkEndCommandBuffer(f.upload_open);
                f.upload_open = VK_NULL_HANDLE;
            }
        }
        process_deferred(0, true);
        destroy_swapchain(false);

        // Internal resources first, so everything still alive afterwards is an app leak.
        if (fallback_texture_) destroy(fallback_texture_);
        if (fallback_storage_) destroy(fallback_storage_);
        if (fallback_sampler_) destroy(fallback_sampler_);
        process_deferred(0, true);

        u32 leaked = 0;
        buffers_.for_each([&](BufferHandle, BufferRecord& b) {
            AE_LOG_WARN("RHI", "leaked buffer '{}' ({} bytes)", b.desc.debug_name, b.desc.size);
            vmaDestroyBuffer(allocator_, b.buffer, b.allocation);
            ++leaked;
        });
        textures_.for_each([&](TextureHandle, TextureRecord& t) {
            AE_LOG_WARN("RHI", "leaked texture '{}' ({}x{})", t.desc.debug_name, t.desc.width, t.desc.height);
            destroy_texture_record(t);
            ++leaked;
        });
        samplers_.for_each([&](SamplerHandle, SamplerRecord& s) {
            AE_LOG_WARN("RHI", "leaked sampler");
            vkDestroySampler(device_, s.sampler, nullptr);
            ++leaked;
        });
        shaders_.for_each([&](ShaderHandle, ShaderRecord& s) {
            AE_LOG_WARN("RHI", "leaked shader '{}'", s.debug_name);
            vkDestroyShaderModule(device_, s.module, nullptr);
            ++leaked;
        });
        pipelines_.for_each([&](PipelineHandle, PipelineRecord& p) {
            AE_LOG_WARN("RHI", "leaked pipeline '{}'", p.debug_name);
            vkDestroyPipeline(device_, p.pipeline, nullptr);
            ++leaked;
        });

        destroy_bindless();
        save_pipeline_cache();
        if (pipeline_cache_) {
            vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
        }

        for (FrameSlot& f : frames_) {
            f.staging.destroy();
            if (f.timestamps) vkDestroyQueryPool(device_, f.timestamps, nullptr);
            if (f.acquire) vkDestroySemaphore(device_, f.acquire, nullptr);
            if (f.pool) vkDestroyCommandPool(device_, f.pool, nullptr);
        }
        frames_.clear();
        if (immediate_fence_) vkDestroyFence(device_, immediate_fence_, nullptr);
        if (immediate_pool_) vkDestroyCommandPool(device_, immediate_pool_, nullptr);
        if (timeline_) vkDestroySemaphore(device_, timeline_, nullptr);

        u32 leaked_allocations = 0;
        if (allocator_) {
            VmaTotalStatistics stats{};
            vmaCalculateStatistics(allocator_, &stats);
            leaked_allocations = stats.total.statistics.allocationCount;
            if (leaked_allocations > 0) {
                AE_LOG_ERROR("RHI", "VMA: {} allocation(s) still alive at shutdown", leaked_allocations);
            }
            vmaDestroyAllocator(allocator_);
            allocator_ = nullptr;
        }
        record_leaks(leaked, leaked_allocations);
        if (leaked == 0 && leaked_allocations == 0) {
            AE_LOG_INFO("RHI", "device shutdown: no leaked resources, 0 VMA allocations");
        }

        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (messenger_) {
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------------------
// misc
// ---------------------------------------------------------------------------------------

Result<std::vector<u32>> VulkanDevice::compile_glsl(ShaderStage stage, StringView source, StringView name,
                                                    const ShaderCompileOptions& options) {
    return rhi::compile_glsl(stage, source, name, options);
}

Result<std::vector<u32>> VulkanDevice::compile_glsl_file(ShaderStage stage, const std::filesystem::path& file,
                                                         const ShaderCompileOptions& options) {
    return rhi::compile_glsl_file(stage, file, options);
}

DeviceInfo VulkanDevice::info() const {
    DeviceInfo i;
    i.adapter_name       = props_.deviceName;
    i.driver_name        = driver_name_;
    i.driver_version     = driver_info_;
    i.api_version        = version_string(props_.apiVersion);
    i.present_mode       = present_mode_name(present_mode_);
    i.swapchain_images   = static_cast<u32>(swapchain_images_.size());
    i.device_local_bytes = device_local_bytes_;
    return i;
}

} // namespace vk

// ---- device_ext.h ----------------------------------------------------------------------

void set_vsync(Device& device, bool enabled) {
    vk::as_vulkan(device).set_vsync(enabled);
}

bool vsync(const Device& device) {
    return vk::as_vulkan(device).vsync();
}

DeviceInfo device_info(const Device& device) {
    return vk::as_vulkan(device).info();
}

} // namespace aether::rhi
