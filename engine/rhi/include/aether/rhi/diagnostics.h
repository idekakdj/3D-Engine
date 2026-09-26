// aether/rhi/diagnostics.h — process-wide RHI health counters (additive, not frozen).
//
// The Vulkan debug messenger counts every validation/performance message of WARNING or
// ERROR severity (messages are also routed to AE_LOG_*). Device teardown records GPU
// memory / handle leaks. Apps and tests use this to fail on validation noise, e.g.:
//     if (rhi::validation_counts().total() > 0) return 1;
// Counters survive device destruction so they can be checked at the very end of main().
// Thread-safe.
#pragma once

#include "aether/core/types.h"

namespace aether::rhi {

struct ValidationCounts {
    u32 errors   = 0;
    u32 warnings = 0;
    [[nodiscard]] u32 total() const { return errors + warnings; }
};

// Messages reported by VK_LAYER_KHRONOS_validation (and other layers) so far.
[[nodiscard]] ValidationCounts validation_counts();
void                           reset_validation_counts();

// True once a device was created with the validation layer enabled.
[[nodiscard]] bool validation_layer_active();

struct LeakReport {
    u32 resources   = 0; // RHI handles never destroyed by the app (auto-freed at teardown)
    u32 allocations = 0; // VMA allocations still alive after all RHI objects were freed
};
// Result of the most recent Device destruction.
[[nodiscard]] LeakReport last_leak_report();

} // namespace aether::rhi
