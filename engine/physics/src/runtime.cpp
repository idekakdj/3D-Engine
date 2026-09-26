// runtime.cpp — refcounted process-wide Jolt initialisation (see aether/physics/runtime.h).
#include "jolt_common.h"

#include "aether/core/log.h"
#include "aether/physics/runtime.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace aether::physics {
namespace {

std::mutex          g_runtime_mutex;
u32                 g_ref_count = 0; // guarded by g_runtime_mutex
std::atomic<u32>    g_ref_count_mirror{ 0 };
std::atomic<u32>    g_assert_failures{ 0 };

void jolt_trace(const char* fmt, ...) {
    char    buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    AE_LOG_INFO("Physics", "Jolt: {}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool jolt_assert_failed(const char* expression, const char* message, const char* file,
                        JPH::uint line) {
    g_assert_failures.fetch_add(1, std::memory_order_relaxed);
    AE_LOG_ERROR("Physics", "Jolt assertion failed: {} ({}) at {}:{}", expression,
                 message != nullptr ? message : "", file, line);
    return false; // log and continue; the counter lets tests fail loudly instead of crashing
}
#endif

} // namespace

void runtime_acquire() {
    std::lock_guard lock(g_runtime_mutex);
    if (g_ref_count++ == 0) {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = &jolt_trace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = &jolt_assert_failed;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        AE_LOG_INFO("Physics", "Jolt runtime initialised");
    }
    g_ref_count_mirror.store(g_ref_count, std::memory_order_release);
}

void runtime_release() {
    std::lock_guard lock(g_runtime_mutex);
    if (g_ref_count == 0) {
        AE_LOG_ERROR("Physics", "runtime_release() without matching runtime_acquire()");
        return;
    }
    if (--g_ref_count == 0) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
        AE_LOG_INFO("Physics", "Jolt runtime shut down");
    }
    g_ref_count_mirror.store(g_ref_count, std::memory_order_release);
}

u32 runtime_ref_count() { return g_ref_count_mirror.load(std::memory_order_acquire); }

u32 jolt_assert_failure_count() { return g_assert_failures.load(std::memory_order_relaxed); }

} // namespace aether::physics
