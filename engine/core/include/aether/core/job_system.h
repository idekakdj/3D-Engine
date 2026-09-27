// aether/core/job_system.h — work-stealing task scheduler.
//
// FROZEN CONTRACT (ADR-0001). All parallelizable engine work (culling, animation
// sampling, asset decode) is expressed as jobs. The M0 backend is a thread pool with
// a wait-group counter; a fiber/task-graph upgrade can follow without API change.
// Thread count is clamped to max(1, hardware_concurrency()-1).
//
// Behaviour (documented per ADR-0009; the implementation in job_system.cpp is authoritative):
//   * Before initialize() / after shutdown() there is no pool: run() and parallel_for()
//     execute inline on the calling thread (a counter passed in never becomes pending), so
//     code written against jobs also works in tools and unit tests.
//   * parallel_for() WITHOUT a counter blocks until every index has run (the caller helps
//     execute jobs meanwhile). WITH a counter it returns immediately; wait(counter) joins.
//     `fn` is copied into the jobs, so passing a temporary lambda is safe.
//   * parallel_for() group_size == 0 selects an automatic grain (about 4 groups per worker).
//   * shutdown() drains every queued job before joining: no job is dropped and every
//     counter reaches zero.
// Thread-safety: run/parallel_for/wait may be called from any thread, including from inside
// a job; initialize/shutdown from the main thread only.
#pragma once

#include "aether/core/types.h"

#include <atomic>
#include <functional>

namespace aether {

// A counter you can wait on; incremented per dispatched job, decremented on completion.
// Non-copyable, non-movable (jobs hold a pointer to it until they finish).
class JobCounter {
public:
    JobCounter() = default;
    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;

    [[nodiscard]] i32 load() const noexcept { return pending_.load(std::memory_order_acquire); }

private:
    friend class JobSystem;
    alignas(64) std::atomic<i32> pending_{ 0 };
};

class JobSystem {
public:
    // Starts `worker_count` threads (0 => auto = max(1, hw-1)). Idempotent.
    static void initialize(u32 worker_count = 0);
    static void shutdown();
    static u32  worker_count();

    // Enqueue a single job. If `counter` is provided it is signalled on completion.
    static void run(std::function<void()> job, JobCounter* counter = nullptr);

    // Data-parallel for-loop: splits [0,count) into groups and runs `fn(i)`.
    static void parallel_for(u32 count, u32 group_size,
                             const std::function<void(u32 index)>& fn,
                             JobCounter* counter = nullptr);

    // Block the calling thread until the counter reaches zero, helping run jobs
    // meanwhile (so the main thread is never idle at a sync point).
    static void wait(JobCounter& counter);
};

} // namespace aether
