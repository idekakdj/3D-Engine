// resource_pool.h — generational object pool behind the RHI's opaque handles.
//
// * Slots live in fixed-size chunks that never move, so get() is lock-free and safe to
//   call from command-recording worker threads while the main thread creates/destroys
//   OTHER resources. allocate()/release() serialize on a mutex.
// * Every release bumps the slot generation (8 bits, as packed by aether::Handle; 0 is
//   skipped so zero-initialised handles never validate). Stale handles return nullptr.
// * Capacity: 2^20 live objects per pool (well below Handle's 24-bit index space).
#pragma once

#include "aether/core/handle.h"
#include "aether/core/types.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace aether::rhi::vk {

template <typename Tag, typename T>
class ResourcePool {
public:
    using HandleT = Handle<Tag>;

    static constexpr u32 kChunkBits   = 10;
    static constexpr u32 kChunkSize   = 1u << kChunkBits;
    static constexpr u32 kMaxChunks   = 1024;
    static constexpr u32 kMaxCapacity = kChunkSize * kMaxChunks;
    static constexpr u32 kGenMask     = 0xFFu;

    ResourcePool() : chunks_(std::make_unique<std::atomic<Slot*>[]>(kMaxChunks)) {}
    ~ResourcePool() {
        for (u32 i = 0; i < kMaxChunks; ++i) {
            delete[] chunks_[i].load(std::memory_order_relaxed);
        }
    }
    ResourcePool(const ResourcePool&)            = delete;
    ResourcePool& operator=(const ResourcePool&) = delete;

    // Returns an invalid handle when the pool is full.
    HandleT allocate(T&& value) {
        std::lock_guard lock(mutex_);
        u32             index = 0;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            if (next_ >= kMaxCapacity) {
                return HandleT{};
            }
            index = next_++;
            std::atomic<Slot*>& chunk = chunks_[index >> kChunkBits];
            if (!chunk.load(std::memory_order_relaxed)) {
                chunk.store(new Slot[kChunkSize], std::memory_order_release);
            }
        }
        Slot& slot = slot_at(index);
        slot.value = std::move(value);
        const u32 gen = slot.generation.load(std::memory_order_relaxed);
        slot.alive.store(true, std::memory_order_release);
        ++live_;
        return HandleT(index, gen);
    }

    // Lock-free lookup; nullptr for invalid / stale / destroyed handles.
    [[nodiscard]] T* get(HandleT h) const {
        if (!h.is_valid() || h.index() >= kMaxCapacity) {
            return nullptr;
        }
        Slot* chunk = chunks_[h.index() >> kChunkBits].load(std::memory_order_acquire);
        if (!chunk) {
            return nullptr;
        }
        Slot& slot = chunk[h.index() & (kChunkSize - 1)];
        if (!slot.alive.load(std::memory_order_acquire) ||
            slot.generation.load(std::memory_order_relaxed) != h.generation()) {
            return nullptr;
        }
        return &slot.value;
    }

    // Moves the object out and retires the handle. False if the handle is stale.
    bool release(HandleT h, T& out) {
        std::lock_guard lock(mutex_);
        if (!get(h)) {
            return false;
        }
        Slot& slot = slot_at(h.index());
        out        = std::move(slot.value);
        slot.value = T{};
        slot.alive.store(false, std::memory_order_release);
        u32 gen = (slot.generation.load(std::memory_order_relaxed) + 1) & kGenMask;
        slot.generation.store(gen == 0 ? 1u : gen, std::memory_order_relaxed);
        free_.push_back(h.index());
        --live_;
        return true;
    }

    // Visits every live object (teardown / leak reporting). Not concurrent-safe with
    // allocate/release from other threads.
    template <typename F>
    void for_each(F&& fn) {
        std::lock_guard lock(mutex_);
        for (u32 i = 0; i < next_; ++i) {
            Slot& slot = slot_at(i);
            if (slot.alive.load(std::memory_order_acquire)) {
                fn(HandleT(i, slot.generation.load(std::memory_order_relaxed)), slot.value);
            }
        }
    }

    [[nodiscard]] u32 live_count() const {
        std::lock_guard lock(mutex_);
        return live_;
    }

private:
    struct Slot {
        T                 value{};
        std::atomic<u32>  generation{ 1 };
        std::atomic<bool> alive{ false };
    };

    Slot& slot_at(u32 index) const {
        return chunks_[index >> kChunkBits].load(std::memory_order_relaxed)[index & (kChunkSize - 1)];
    }

    std::unique_ptr<std::atomic<Slot*>[]> chunks_;
    std::vector<u32>                      free_;
    u32                                   next_ = 0;
    u32                                   live_ = 0;
    mutable std::mutex                    mutex_;
};

} // namespace aether::rhi::vk
