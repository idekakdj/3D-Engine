// range_allocator.h — first-fit free-list allocator over an abstract [0, capacity) range.
//
// Private header. Used by the geometry arena to sub-allocate vertex/index ranges (in
// elements) inside large GPU buffers. Frees coalesce with adjacent free ranges. Pure CPU,
// deterministic, unit-tested. Not thread-safe (main thread only).
#pragma once

#include "aether/core/types.h"

#include <map>
#include <optional>

namespace aether::renderer {

class RangeAllocator {
public:
    struct Range {
        u64 offset = 0;
        u64 size = 0;
    };

    RangeAllocator() = default;
    explicit RangeAllocator(u64 capacity) { reset(capacity); }

    // Drops every allocation.
    void reset(u64 capacity);

    // First-fit allocation with `alignment` (elements, power of two). nullopt when full.
    [[nodiscard]] std::optional<Range> allocate(u64 size, u64 alignment = 1);

    // Returns a range previously produced by allocate() (exact offset/size).
    void free(Range range);

    // Extends the managed range to `new_capacity` (>= capacity()); the new tail is free.
    void grow(u64 new_capacity);

    [[nodiscard]] u64 capacity() const noexcept { return capacity_; }
    [[nodiscard]] u64 used() const noexcept { return used_; }
    [[nodiscard]] u64 largest_free() const noexcept;
    [[nodiscard]] usize free_block_count() const noexcept { return free_.size(); }

    // Visits every allocated range (derived from the free list) in ascending order.
    template <typename Fn>
    void for_each_allocated(Fn&& fn) const {
        u64 cursor = 0;
        for (const auto& [off, sz] : free_) {
            if (off > cursor) {
                fn(Range{ cursor, off - cursor });
            }
            cursor = off + sz;
        }
        if (cursor < capacity_) {
            fn(Range{ cursor, capacity_ - cursor });
        }
    }

private:
    void insert_free(u64 offset, u64 size);

    std::map<u64, u64> free_; // offset -> size, non-adjacent (always coalesced)
    u64                capacity_ = 0;
    u64                used_ = 0;
};

} // namespace aether::renderer
