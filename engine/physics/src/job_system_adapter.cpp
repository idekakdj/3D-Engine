// job_system_adapter.cpp — see job_system_adapter.h.
#include "job_system_adapter.h"

#include "aether/core/log.h"

#include <thread>

namespace aether::physics::detail {

EngineJobSystemAdapter::EngineJobSystemAdapter(JPH::uint max_jobs, JPH::uint max_barriers)
    : JPH::JobSystemWithBarrier(max_barriers) {
    jobs_.Init(max_jobs, max_jobs);
}

EngineJobSystemAdapter::~EngineJobSystemAdapter() {
    // Engine workers may still hold references to jobs that the barrier thread already executed;
    // they must drop them (FreeJob -> jobs_) before our storage goes away.
    if (in_flight_.load() > 0)
        aether::JobSystem::wait(in_flight_);
}

int EngineJobSystemAdapter::GetMaxConcurrency() const {
    // Engine workers plus the thread blocked in PhysicsSystem::Update, which helps out.
    return static_cast<int>(aether::JobSystem::worker_count()) + 1;
}

JPH::JobSystem::JobHandle EngineJobSystemAdapter::CreateJob(const char* name, JPH::ColorArg color,
                                                            const JobFunction& function,
                                                            JPH::uint32 num_dependencies) {
    JPH::uint32 index;
    for (;;) {
        index = jobs_.ConstructObject(name, color, this, function, num_dependencies);
        if (index != AvailableJobs::cInvalidObjectIndex)
            break;
        // Pool exhausted: jobs are freed as workers finish, so this resolves itself. Size
        // PhysicsWorldSettings::max_jobs so it never happens in practice.
        AE_LOG_WARN("Physics", "physics job pool exhausted; waiting (raise max_jobs)");
        std::this_thread::yield();
    }
    Job* job = &jobs_.Get(index);

    // The handle keeps a reference: the job may be queued below and complete immediately.
    JobHandle handle(job);
    if (num_dependencies == 0)
        QueueJob(job);
    return handle;
}

void EngineJobSystemAdapter::QueueJob(Job* job) {
    job->AddRef(); // released by the engine job below
    aether::JobSystem::run(
        [job] {
            job->Execute(); // no-op if the barrier thread already ran it
            job->Release();
        },
        &in_flight_);
}

void EngineJobSystemAdapter::QueueJobs(Job** jobs, JPH::uint num_jobs) {
    for (JPH::uint i = 0; i < num_jobs; ++i)
        QueueJob(jobs[i]);
}

void EngineJobSystemAdapter::FreeJob(Job* job) { jobs_.DestructObject(job); }

} // namespace aether::physics::detail
