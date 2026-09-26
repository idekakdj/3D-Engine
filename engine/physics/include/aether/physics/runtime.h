// aether/physics/runtime.h — process-wide Jolt runtime lifetime (refcounted).
//
// Jolt needs RegisterDefaultAllocator / Factory / RegisterTypes exactly once per process before
// any shape or body is created, and the reverse on shutdown. PhysicsSubsystem and every
// PhysicsWorld hold a reference, so standalone PhysicsWorld use (tools, tests) works too.
// Thread-safe.
#pragma once

#include "aether/core/types.h"

namespace aether::physics {

// Takes a reference; the first call initialises Jolt (and routes Jolt trace/asserts to the
// engine log). Always succeeds.
void runtime_acquire();

// Drops a reference; the last one unregisters Jolt types and destroys the factory.
void runtime_release();

// Current reference count (0 = Jolt not initialised).
[[nodiscard]] u32 runtime_ref_count();

// Diagnostics: number of Jolt-internal assertion failures reported since process start
// (only non-zero in builds with JPH_ENABLE_ASSERTS, i.e. Debug). Tests assert this stays 0.
[[nodiscard]] u32 jolt_assert_failure_count();

// RAII helper around runtime_acquire/runtime_release.
class RuntimeRef {
public:
    RuntimeRef() { runtime_acquire(); }
    ~RuntimeRef() { runtime_release(); }
    RuntimeRef(const RuntimeRef&) = delete;
    RuntimeRef& operator=(const RuntimeRef&) = delete;
};

} // namespace aether::physics
