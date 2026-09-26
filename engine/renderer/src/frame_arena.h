// frame_arena.h — ring-buffered, persistently mapped per-frame upload memory.
//
// Private header. One CpuToGpu buffer per frame slot (slot = renderer frame counter %
// Device::frames_in_flight()). All per-frame GPU data (frame constants, instances,
// materials, lights, joint palettes, debug lines) is bump-allocated here each frame and
// read by shaders through buffer device addresses. A slot is only rewritten
// frames_in_flight renders later, after Device::begin_frame() waited for that slot's
// previous GPU work, so no extra synchronisation is needed. Main thread only.
#pragma once

#include "aether/core/types.h"
#include "aether/rhi/device.h"

#include <vector>

namespace aether::renderer {

class FrameArena {
public:
    struct Allocation {
        void* cpu = nullptr; // write-only (write-combined memory)
        u64   gpu = 0;       // buffer device address
        u64   offset = 0;
        u64   size = 0;
        [[nodiscard]] bool valid() const noexcept { return cpu != nullptr; }
    };

    static constexpr u64 kAlignment = 64;

    FrameArena() = default;
    FrameArena(const FrameArena&) = delete;
    FrameArena& operator=(const FrameArena&) = delete;

    void init(rhi::Device& device, u32 slot_count, u64 initial_capacity);
    void shutdown();

    // Selects `slot`, grows it to hold at least `required_bytes`, resets the cursor.
    // Returns false if the buffer could not be (re)created or mapped.
    bool begin_frame(u32 slot, u64 required_bytes);

    // Bump allocation (kAlignment aligned). Invalid allocation if out of space.
    Allocation allocate(u64 size);

    template <typename T>
    Allocation allocate_array(usize count) {
        return allocate(static_cast<u64>(sizeof(T)) * static_cast<u64>(count));
    }

    // Worst-case bytes consumed by an allocation of `size` (for capacity planning).
    [[nodiscard]] static constexpr u64 padded(u64 size) noexcept {
        return (size + kAlignment - 1) & ~(kAlignment - 1);
    }

    [[nodiscard]] rhi::BufferHandle current_buffer() const noexcept;
    [[nodiscard]] u64 capacity(u32 slot) const noexcept;

private:
    struct Slot {
        rhi::BufferHandle buffer;
        u8*               mapped = nullptr;
        u64               address = 0;
        u64               capacity = 0;
    };
    bool create_slot(Slot& s, u64 capacity);

    rhi::Device*      device_ = nullptr;
    std::vector<Slot> slots_;
    u32               current_ = 0;
    u64               cursor_ = 0;
};

} // namespace aether::renderer
