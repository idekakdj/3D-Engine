// vk_debug.cpp — VK_EXT_debug_utils messenger -> AE_LOG_*, validation counters, object
// names, and the process-wide diagnostics API (aether/rhi/diagnostics.h).
#include "aether/rhi/diagnostics.h"
#include "vk_device.h"

#include <atomic>
#include <cstring>

namespace aether::rhi {
namespace {

std::atomic<u32>  g_errors{ 0 };
std::atomic<u32>  g_warnings{ 0 };
std::atomic<bool> g_validation_active{ false };
std::atomic<u32>  g_leaked_resources{ 0 };
std::atomic<u32>  g_leaked_allocations{ 0 };

VKAPI_ATTR VkBool32 VKAPI_CALL messenger_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT        types,
                                                  const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                  void* /*user*/) {
    const char* id  = (data && data->pMessageIdName) ? data->pMessageIdName : "";
    const char* msg = (data && data->pMessage) ? data->pMessage : "";

    // Only layer diagnostics (validation / performance) count; GENERAL messages come from
    // the loader or layer status reports and are logged without failing the run.
    const bool counted =
        (types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) != 0;

    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (counted) {
            g_errors.fetch_add(1, std::memory_order_relaxed);
        }
        AE_LOG_ERROR("Vulkan", "[{}] {}", id, msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if (counted) {
            g_warnings.fetch_add(1, std::memory_order_relaxed);
        }
        AE_LOG_WARN("Vulkan", "[{}] {}", id, msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        AE_LOG_DEBUG("Vulkan", "[{}] {}", id, msg);
    } else {
        AE_LOG_TRACE("Vulkan", "[{}] {}", id, msg);
    }
    return VK_FALSE; // never abort the call
}

} // namespace

ValidationCounts validation_counts() {
    return { g_errors.load(std::memory_order_relaxed), g_warnings.load(std::memory_order_relaxed) };
}

void reset_validation_counts() {
    g_errors.store(0, std::memory_order_relaxed);
    g_warnings.store(0, std::memory_order_relaxed);
}

bool validation_layer_active() {
    return g_validation_active.load(std::memory_order_relaxed);
}

LeakReport last_leak_report() {
    return { g_leaked_resources.load(std::memory_order_relaxed), g_leaked_allocations.load(std::memory_order_relaxed) };
}

namespace vk {

VkDebugUtilsMessengerCreateInfoEXT make_messenger_info() {
    VkDebugUtilsMessengerCreateInfoEXT info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = &messenger_callback;
    return info;
}

void mark_validation_active(bool active) {
    g_validation_active.store(active, std::memory_order_relaxed);
}

void record_leaks(u32 resources, u32 allocations) {
    g_leaked_resources.store(resources, std::memory_order_relaxed);
    g_leaked_allocations.store(allocations, std::memory_order_relaxed);
}

void VulkanDevice::set_object_name(VkObjectType type, u64 handle, const std::string& name) const {
    if (!debug_utils_ || name.empty() || handle == 0) {
        return;
    }
    VkDebugUtilsObjectNameInfoEXT info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
    info.objectType   = type;
    info.objectHandle = handle;
    info.pObjectName  = name.c_str();
    vkSetDebugUtilsObjectNameEXT(device_, &info);
}

} // namespace vk
} // namespace aether::rhi
