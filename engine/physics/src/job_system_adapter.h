// job_system_adapter.h (private) — runs Jolt's physics jobs on aether::JobSystem.
//
// JPH::JobSystemWithBarrier implements barriers; we only provide job storage and queueing.
// Queued jobs are handed to aether::JobSystem::run(). The thread waiting in
// PhysicsSystem::Update (WaitForJobs) also executes any not-yet-started job of its barrier, so
// progress never depends on engine workers being free (no deadlock when the pool is saturated,
// and even with zero workers). A Job executes at most once (Jolt's Job::Execute is atomic), so a
// worker that picks up an already-executed job just drops its reference.
#pragma once

#include "jolt_common.h"

#include "aether/core/job_system.h"

namespace aether::physics::detail {

class EngineJobSystemAdapter final : public JPH::JobSystemWithBarrier {
public:
    EngineJobSystemAdapter(JPH::uint max_jobs, JPH::uint max_barriers);
    ~EngineJobSystemAdapter() override;

    EngineJobSystemAdapter(const EngineJobSystemAdapter&) = delete;
    EngineJobSystemAdapter& operator=(const EngineJobSystemAdapter&) = delete;

    [[nodiscard]] int GetMaxConcurrency() const override;
    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                        JPH::uint32 num_dependencies = 0) override;

protected:
    void QueueJob(Job* job) override;
    void QueueJobs(Job** jobs, JPH::uint num_jobs) override;
    void FreeJob(Job* job) override;

private:
    using AvailableJobs = JPH::FixedSizeFreeList<Job>;
    AvailableJobs jobs_;
    aether::JobCounter in_flight_; // engine jobs still holding a reference to one of our Jobs
};

} // namespace aether::physics::detail
