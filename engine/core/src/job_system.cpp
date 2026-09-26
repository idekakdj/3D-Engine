// job_system.cpp — work-stealing thread pool behind aether::JobSystem.
//
// Structure
//   * One deque per worker (owner pushes/pops at the back = LIFO for cache locality,
//     thieves pop at the front = FIFO), plus one global injection queue used by
//     non-worker threads. Each deque has its own small mutex (lock-based stealing: simple,
//     obviously correct, and uncontended in the common case).
//   * `queued_` counts jobs sitting in queues. Idle workers spin briefly, then sleep on a
//     condition variable whose predicate reads `queued_`.
//   * No lost wakeups: a sleeper increments `sleepers_` and evaluates its predicate while
//     holding `sleep_mutex_`; a producer publishes (queued_++ / counter--) with seq_cst,
//     then reads `sleepers_` (seq_cst) and, if non-zero, notifies under the same mutex.
//     By the seq_cst total order either the sleeper sees the new state, or the producer
//     sees the sleeper and its notify cannot race ahead of the sleeper's wait.
//   * wait(counter) never just sleeps: it keeps executing queued jobs (own queue first,
//     then global, then stealing) and only blocks when there is nothing to run, waking on
//     either new work or the counter reaching zero.
//
// Semantics beyond the frozen header (documented here):
//   * Before initialize() / after shutdown(), run() and parallel_for() execute inline on
//     the calling thread (so code using jobs also works in tools/tests without a pool).
//   * parallel_for(..., counter == nullptr) blocks (helping) until every index ran.
//     With a counter it is asynchronous; `fn` is copied, so temporaries are safe.
//   * group_size == 0 picks an automatic grain (~4 groups per thread).
//   * shutdown() lets workers drain every queued job, joins them, then runs anything left
//     on the calling thread: no job is ever dropped and every counter reaches zero.
#include "aether/core/job_system.h"
#include "aether/core/log.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#    include <intrin.h>
#    define AE_CPU_RELAX() _mm_pause()
#else
#    define AE_CPU_RELAX() std::this_thread::yield()
#endif

#ifdef _WIN32
#    include <windows.h>
#endif

namespace aether {
namespace {

struct Job {
    std::function<void()> fn;
    std::atomic<i32>*     counter = nullptr; // JobCounter::pending_ (resolved by JobSystem)
};

class JobQueue {
public:
    void push(Job&& job) {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
        size_.store(jobs_.size(), std::memory_order_release);
    }
    void push_batch(std::vector<Job>& batch) {
        std::lock_guard lock(mutex_);
        for (Job& j : batch) {
            jobs_.push_back(std::move(j));
        }
        size_.store(jobs_.size(), std::memory_order_release);
    }
    bool pop_back(Job& out) { return pop(out, true); }
    bool pop_front(Job& out) { return pop(out, false); }

private:
    bool pop(Job& out, bool back) {
        // Lock-free emptiness hint: idle thieves scanning many queues never touch the
        // mutex of an empty queue (a stale hint only costs one extra lock / retry).
        if (size_.load(std::memory_order_acquire) == 0) {
            return false;
        }
        std::lock_guard lock(mutex_);
        if (jobs_.empty()) {
            return false;
        }
        if (back) {
            out = std::move(jobs_.back());
            jobs_.pop_back();
        } else {
            out = std::move(jobs_.front());
            jobs_.pop_front();
        }
        size_.store(jobs_.size(), std::memory_order_release);
        return true;
    }

    std::mutex          mutex_;
    std::deque<Job>     jobs_;
    std::atomic<usize>  size_{ 0 };
};

class Scheduler;
thread_local Scheduler* t_scheduler    = nullptr; // set on worker threads only
thread_local i32        t_worker_index = -1;

constexpr u32 kSpinIterations = 256;

class Scheduler {
public:
    explicit Scheduler(u32 worker_count) {
        locals_.reserve(worker_count);
        for (u32 i = 0; i < worker_count; ++i) {
            locals_.push_back(std::make_unique<JobQueue>());
        }
        threads_.reserve(worker_count);
        for (u32 i = 0; i < worker_count; ++i) {
            threads_.emplace_back([this, i] { worker_main(i); });
        }
    }

    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    [[nodiscard]] u32 worker_count() const { return static_cast<u32>(locals_.size()); }

    void submit(Job&& job) {
        target_queue().push(std::move(job));
        queued_.fetch_add(1, std::memory_order_seq_cst);
        if (sleepers_.load(std::memory_order_seq_cst) > 0) {
            std::lock_guard lock(sleep_mutex_);
            sleep_cv_.notify_one();
        }
    }

    // One lock + one wake-up for many jobs (parallel_for).
    void submit_batch(std::vector<Job>& batch) {
        if (batch.empty()) {
            return;
        }
        const i64 n = static_cast<i64>(batch.size());
        target_queue().push_batch(batch);
        queued_.fetch_add(n, std::memory_order_seq_cst);
        if (sleepers_.load(std::memory_order_seq_cst) > 0) {
            std::lock_guard lock(sleep_mutex_);
            if (n == 1) {
                sleep_cv_.notify_one();
            } else {
                sleep_cv_.notify_all();
            }
        }
    }

    // Help until `pending` drops to zero.
    void wait(std::atomic<i32>& pending) {
        const i32 self  = (t_scheduler == this) ? t_worker_index : -1;
        u32       spins = 0;
        while (pending.load(std::memory_order_acquire) > 0) {
            Job job;
            if (find_job(self, job)) {
                execute(job);
                spins = 0;
                continue;
            }
            if (++spins < kSpinIterations) {
                AE_CPU_RELAX();
                continue;
            }
            std::unique_lock lock(sleep_mutex_);
            sleepers_.fetch_add(1, std::memory_order_seq_cst);
            sleep_cv_.wait(lock, [&] {
                return pending.load(std::memory_order_seq_cst) <= 0 ||
                       queued_.load(std::memory_order_seq_cst) > 0 ||
                       stop_.load(std::memory_order_seq_cst);
            });
            sleepers_.fetch_sub(1, std::memory_order_seq_cst);
            spins = 0;
        }
    }

    // Stop workers after they drain all queues, join them, run any stragglers inline.
    void shutdown() {
        {
            std::lock_guard lock(sleep_mutex_);
            stop_.store(true, std::memory_order_seq_cst);
            sleep_cv_.notify_all();
        }
        for (std::thread& t : threads_) {
            t.join();
        }
        threads_.clear();
        Job job;
        while (find_job(-1, job)) {
            execute(job);
        }
    }

private:
    JobQueue& target_queue() {
        if (t_scheduler == this && t_worker_index >= 0) {
            return *locals_[static_cast<usize>(t_worker_index)];
        }
        return global_;
    }

    bool find_job(i32 self, Job& out) {
        bool found = false;
        if (self >= 0) {
            found = locals_[static_cast<usize>(self)]->pop_back(out);
        }
        if (!found) {
            found = global_.pop_front(out);
        }
        if (!found) {
            const usize n     = locals_.size();
            const usize start = self >= 0 ? static_cast<usize>(self) + 1 : steal_cursor_++ % std::max<usize>(n, 1);
            for (usize i = 0; i < n && !found; ++i) {
                const usize victim = (start + i) % n;
                if (static_cast<i32>(victim) == self) {
                    continue;
                }
                found = locals_[victim]->pop_front(out);
            }
        }
        if (found) {
            queued_.fetch_sub(1, std::memory_order_seq_cst);
        }
        return found;
    }

    void execute(Job& job) {
        job.fn();
        job.fn = nullptr; // release captures before signalling completion
        if (job.counter && job.counter->fetch_sub(1, std::memory_order_seq_cst) == 1) {
            // Counter hit zero: wake blocked waiters (they re-check their predicate).
            if (sleepers_.load(std::memory_order_seq_cst) > 0) {
                std::lock_guard lock(sleep_mutex_);
                sleep_cv_.notify_all();
            }
        }
    }

    void worker_main(u32 index) {
        t_scheduler    = this;
        t_worker_index = static_cast<i32>(index);
#ifdef _WIN32
        const std::wstring name = L"Aether Worker " + std::to_wstring(index);
        SetThreadDescription(GetCurrentThread(), name.c_str());
#endif
        for (;;) {
            Job job;
            if (find_job(static_cast<i32>(index), job)) {
                execute(job);
                continue;
            }
            if (stop_.load(std::memory_order_acquire)) {
                break; // queues observed empty after stop: done
            }
            bool work_appeared = false;
            for (u32 i = 0; i < kSpinIterations; ++i) {
                if (queued_.load(std::memory_order_relaxed) > 0) {
                    work_appeared = true;
                    break;
                }
                AE_CPU_RELAX();
            }
            if (work_appeared) {
                continue;
            }
            std::unique_lock lock(sleep_mutex_);
            sleepers_.fetch_add(1, std::memory_order_seq_cst);
            sleep_cv_.wait(lock, [&] {
                return stop_.load(std::memory_order_seq_cst) || queued_.load(std::memory_order_seq_cst) > 0;
            });
            sleepers_.fetch_sub(1, std::memory_order_seq_cst);
        }
        t_scheduler    = nullptr;
        t_worker_index = -1;
    }

    std::vector<std::unique_ptr<JobQueue>> locals_;
    JobQueue                               global_;
    std::vector<std::thread>               threads_;
    std::atomic<i64>                       queued_{ 0 };
    std::atomic<u32>                       sleepers_{ 0 };
    std::atomic<usize>                     steal_cursor_{ 0 };
    std::atomic<bool>                      stop_{ false };
    std::mutex                             sleep_mutex_;
    std::condition_variable                sleep_cv_;
};

std::mutex              g_lifecycle_mutex; // serializes initialize()/shutdown()
std::atomic<Scheduler*> g_scheduler{ nullptr };

// The scheduler a waiting thread should help: its own pool if it is a worker (valid for
// the thread's lifetime even mid-shutdown), else the live global pool.
Scheduler* scheduler_for_wait() {
    return t_scheduler ? t_scheduler : g_scheduler.load(std::memory_order_acquire);
}

} // namespace

void JobSystem::initialize(u32 worker_count) {
    std::lock_guard lock(g_lifecycle_mutex);
    if (Scheduler* existing = g_scheduler.load(std::memory_order_acquire)) {
        if (worker_count != 0 && worker_count != existing->worker_count()) {
            AE_LOG_DEBUG("Jobs", "initialize({}) ignored: already running with {} workers", worker_count,
                         existing->worker_count());
        }
        return;
    }
    u32 count = worker_count;
    if (count == 0) {
        const u32 hw = std::thread::hardware_concurrency();
        count        = std::max<u32>(1u, hw > 1 ? hw - 1 : 1u);
    }
    g_scheduler.store(new Scheduler(count), std::memory_order_release);
    AE_LOG_INFO("Jobs", "job system started: {} worker thread(s)", count);
}

void JobSystem::shutdown() {
    std::lock_guard lock(g_lifecycle_mutex);
    Scheduler* s = g_scheduler.exchange(nullptr, std::memory_order_acq_rel);
    if (!s) {
        return;
    }
    s->shutdown();
    delete s;
    AE_LOG_INFO("Jobs", "job system stopped");
}

u32 JobSystem::worker_count() {
    Scheduler* s = g_scheduler.load(std::memory_order_acquire);
    return s ? s->worker_count() : 0;
}

void JobSystem::run(std::function<void()> job, JobCounter* counter) {
    if (!job) {
        return;
    }
    Scheduler* s = g_scheduler.load(std::memory_order_acquire);
    if (!s) {
        job(); // no pool: run inline, counter never goes pending
        return;
    }
    std::atomic<i32>* pending = nullptr;
    if (counter) {
        pending = &counter->pending_;
        pending->fetch_add(1, std::memory_order_acq_rel);
    }
    s->submit(Job{ std::move(job), pending });
}

void JobSystem::parallel_for(u32 count, u32 group_size, const std::function<void(u32 index)>& fn,
                             JobCounter* counter) {
    if (count == 0 || !fn) {
        return;
    }
    Scheduler* s = g_scheduler.load(std::memory_order_acquire);
    if (!s || count == 1) {
        for (u32 i = 0; i < count; ++i) {
            fn(i);
        }
        return;
    }
    if (group_size == 0) {
        const u32 target_groups = std::max<u32>(1u, s->worker_count() * 4u);
        group_size              = std::max<u32>(1u, (count + target_groups - 1) / target_groups);
    }
    const u32 group_count = (count + group_size - 1) / group_size;

    // Shared copy: jobs may outlive the caller's `fn` when a counter is supplied.
    auto        shared = std::make_shared<const std::function<void(u32)>>(fn);
    JobCounter  local;
    JobCounter* c = counter ? counter : &local;
    c->pending_.fetch_add(static_cast<i32>(group_count), std::memory_order_acq_rel);
    std::vector<Job> batch;
    batch.reserve(group_count);
    for (u32 g = 0; g < group_count; ++g) {
        const u32 begin = g * group_size;
        const u32 end   = std::min(count, begin + group_size);
        batch.push_back(Job{ [shared, begin, end] {
                                for (u32 i = begin; i < end; ++i) {
                                    (*shared)(i);
                                }
                            },
                             &c->pending_ });
    }
    s->submit_batch(batch);
    if (!counter) {
        wait(local);
    }
}

void JobSystem::wait(JobCounter& counter) {
    if (Scheduler* s = scheduler_for_wait()) {
        s->wait(counter.pending_);
        return;
    }
    // No pool (never initialized / already shut down): nothing can be queued, but be
    // robust against jobs still finishing on exiting workers.
    while (counter.pending_.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
}

} // namespace aether
