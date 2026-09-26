// range_allocator.cpp — first-fit free-list range allocator (see range_allocator.h).
#include "range_allocator.h"

#include "aether/core/error.h"

#include <iterator>

namespace aether::renderer {

void RangeAllocator::reset(u64 capacity) {
    free_.clear();
    capacity_ = capacity;
    used_ = 0;
    if (capacity > 0) {
        free_.emplace(0, capacity);
    }
}

std::optional<RangeAllocator::Range> RangeAllocator::allocate(u64 size, u64 alignment) {
    if (size == 0) {
        return std::nullopt;
    }
    if (alignment == 0) {
        alignment = 1;
    }
    AE_ASSERT((alignment & (alignment - 1)) == 0);

    for (auto it = free_.begin(); it != free_.end(); ++it) {
        const u64 block_off = it->first;
        const u64 block_size = it->second;
        const u64 aligned = (block_off + alignment - 1) & ~(alignment - 1);
        const u64 padding = aligned - block_off;
        if (padding + size > block_size) {
            continue;
        }
        // Carve [aligned, aligned + size) out of the block; keep the head/tail free.
        free_.erase(it);
        if (padding > 0) {
            free_.emplace(block_off, padding);
        }
        const u64 tail = block_size - padding - size;
        if (tail > 0) {
            free_.emplace(aligned + size, tail);
        }
        used_ += size;
        return Range{ aligned, size };
    }
    return std::nullopt;
}

void RangeAllocator::insert_free(u64 offset, u64 size) {
    auto next = free_.lower_bound(offset);
    // Merge with the previous block if adjacent.
    if (next != free_.begin()) {
        auto prev = std::prev(next);
        AE_ASSERT(prev->first + prev->second <= offset); // double free / overlap
        if (prev->first + prev->second == offset) {
            offset = prev->first;
            size += prev->second;
            free_.erase(prev);
        }
    }
    // Merge with the following block if adjacent.
    if (next != free_.end()) {
        AE_ASSERT(offset + size <= next->first); // double free / overlap
        if (offset + size == next->first) {
            size += next->second;
            free_.erase(next);
        }
    }
    free_.emplace(offset, size);
}

void RangeAllocator::free(Range range) {
    if (range.size == 0) {
        return;
    }
    AE_ASSERT(range.offset + range.size <= capacity_);
    AE_ASSERT(used_ >= range.size);
    used_ -= range.size;
    insert_free(range.offset, range.size);
}

void RangeAllocator::grow(u64 new_capacity) {
    if (new_capacity <= capacity_) {
        return;
    }
    const u64 old = capacity_;
    capacity_ = new_capacity;
    insert_free(old, new_capacity - old);
}

u64 RangeAllocator::largest_free() const noexcept {
    u64 best = 0;
    for (const auto& [off, sz] : free_) {
        (void)off;
        best = sz > best ? sz : best;
    }
    return best;
}

} // namespace aether::renderer
