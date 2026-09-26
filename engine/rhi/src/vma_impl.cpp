// vma_impl.cpp — the single translation unit that compiles VulkanMemoryAllocator.
//
// Leaks are detected and reported by VulkanDevice teardown (vmaCalculateStatistics ->
// rhi::last_leak_report()); VMA's own leak asserts are routed to the log instead of
// aborting, so a leaking app still shuts down and reports cleanly.
#include "aether/core/log.h"

#define VMA_ASSERT_LEAK(expr)                                                                      \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ::aether::log_message(::aether::LogLevel::Error, "VMA", "leak check failed: " #expr);  \
        }                                                                                          \
    } while (0)

#define VMA_IMPLEMENTATION
#include "vk_common.h"
