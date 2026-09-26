// vk_common.h — private Vulkan/VMA include hub + VK_CHECK for the Vulkan backend.
#pragma once

// volk provides every Vulkan entry point (VK_NO_PROTOTYPES is a PUBLIC define of volk).
#include <volk.h>

// VMA fetches entry points dynamically through vkGetInstanceProcAddr/vkGetDeviceProcAddr
// (ADR-0001); these must be identical in every TU that includes vk_mem_alloc.h.
#ifndef VMA_STATIC_VULKAN_FUNCTIONS
#    define VMA_STATIC_VULKAN_FUNCTIONS 0
#endif
#ifndef VMA_DYNAMIC_VULKAN_FUNCTIONS
#    define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#endif
#include <vk_mem_alloc.h>

#include "aether/core/error.h"
#include "aether/core/log.h"

namespace aether::rhi::vk {

[[noreturn]] void vk_check_failed(VkResult result, const char* expr, const char* file, int line);
[[nodiscard]] const char* result_string(VkResult result);

} // namespace aether::rhi::vk

// For must-succeed calls only (ADR-0001): logs FATAL with the VkResult name and aborts.
// Recoverable results (OUT_OF_DATE, SUBOPTIMAL, ...) are handled explicitly instead.
#define VK_CHECK(expr)                                                                             \
    do {                                                                                           \
        const VkResult ae_vk_result_ = (expr);                                                     \
        if (ae_vk_result_ != VK_SUCCESS) [[unlikely]] {                                            \
            ::aether::rhi::vk::vk_check_failed(ae_vk_result_, #expr, __FILE__, __LINE__);          \
        }                                                                                          \
    } while (0)
