// handle_pool.h — generational slot pool backing the renderer's opaque resource handles.
//
// Private header. Handle<Tag> packs {24-bit index, 8-bit generation}. Slots are reused
// through a free list; every release bumps the slot generation so stale handles are
// rejected by get(). Generation 0 is never issued, so a default-constructed or
// value-initialised handle never aliases a live slot. Main thread only.
#pragma once

#include "aether/core/error.h"
#include "aether/core/handle.h"
#include "aether/core/types.h"

#include <optional>
#include <utility>
#include <vector>

namespace aether::renderer {

template <typename Tag, typename T>
class HandlePool {
public:
    using HandleT = Handle<Tag>;
    static constexpr u32 kMaxGeneration = 0xFFu;

    [[nodiscard]] HandleT insert(T value) {
        u32 index;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            index = static_cast<u32>(slots_.size());
            if (index >= HandleT::kIndexMask) {
                return HandleT{}; // pool exhausted (16M live objects)
            }
            slots_.emplace_back();
        }
        Slot& s = slots_[index];
        s.value = std::move(value);
        ++live_;
        return HandleT(index, s.generation);
    }

    [[nodiscard]] T* get(HandleT h) {
        if (!h.is_valid() || h.index() >= slots_.size()) {
            return nullptr;
        }
        Slot& s = slots_[h.index()];
        return (s.value && s.generation == h.generation()) ? &*s.value : nullptr;
    }
    [[nodiscard]] const T* get(HandleT h) const {
        return const_cast<HandlePool*>(this)->get(h);
    }

    // Removes the object and returns it (so the caller can schedule GPU destruction).
    std::optional<T> remove(HandleT h) {
        T* v = get(h);
        if (!v) {
            return std::nullopt;
        }
        Slot& s = slots_[h.index()];
        std::optional<T> out = std::move(s.value);
        s.value.reset();
        // Generation 1..255, skipping 0 so value-initialised handles never match.
        s.generation = s.generation >= kMaxGeneration ? 1u : s.generation + 1u;
        free_.push_back(h.index());
        --live_;
        return out;
    }

    template <typename Fn>
    void for_each(Fn&& fn) {
        for (u32 i = 0; i < slots_.size(); ++i) {
            if (slots_[i].value) {
                fn(HandleT(i, slots_[i].generation), *slots_[i].value);
            }
        }
    }

    // Removes every object, invoking fn(value&&) on each (shutdown path).
    template <typename Fn>
    void drain(Fn&& fn) {
        for (Slot& s : slots_) {
            if (s.value) {
                fn(std::move(*s.value));
                s.value.reset();
            }
        }
        slots_.clear();
        free_.clear();
        live_ = 0;
    }

    [[nodiscard]] u32 size() const noexcept { return live_; }
    [[nodiscard]] u32 capacity() const noexcept { return static_cast<u32>(slots_.size()); }

private:
    struct Slot {
        std::optional<T> value;
        u32              generation = 1;
    };
    std::vector<Slot> slots_;
    std::vector<u32>  free_;
    u32               live_ = 0;
};

} // namespace aether::renderer
