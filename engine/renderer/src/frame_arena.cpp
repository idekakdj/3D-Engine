// frame_arena.cpp — per-frame upload ring (see frame_arena.h).
#include "frame_arena.h"

#include "aether/core/log.h"

#include <algorithm>
#include <format>

namespace aether::renderer {

bool FrameArena::create_slot(Slot& s, u64 capacity) {
    if (s.buffer.is_valid()) {
        device_->unmap(s.buffer);
        device_->destroy(s.buffer); // deferred by the device
        s = Slot{};
    }
    rhi::BufferDesc desc;
    desc.size = capacity;
    desc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Vertex | rhi::BufferUsage::Index |
                 rhi::BufferUsage::Indirect | rhi::BufferUsage::TransferSrc;
    desc.memory = rhi::MemoryUsage::CpuToGpu;
    desc.debug_name = std::format("FrameArena.Slot{}", static_cast<usize>(&s - slots_.data()));
    s.buffer = device_->create_buffer(desc);
    if (!s.buffer.is_valid()) {
        AE_LOG_ERROR("Renderer", "FrameArena: failed to create {} byte upload buffer", capacity);
        return false;
    }
    s.mapped = static_cast<u8*>(device_->map(s.buffer));
    s.address = device_->buffer_device_address(s.buffer);
    s.capacity = capacity;
    if (!s.mapped) {
        AE_LOG_ERROR("Renderer", "FrameArena: failed to map upload buffer");
        return false;
    }
    return true;
}

void FrameArena::init(rhi::Device& device, u32 slot_count, u64 initial_capacity) {
    device_ = &device;
    slots_.resize(std::max(1u, slot_count));
    for (Slot& s : slots_) {
        create_slot(s, initial_capacity);
    }
}

void FrameArena::shutdown() {
    if (!device_) {
        return;
    }
    for (Slot& s : slots_) {
        if (s.buffer.is_valid()) {
            device_->unmap(s.buffer);
            device_->destroy(s.buffer);
        }
    }
    slots_.clear();
    device_ = nullptr;
}

bool FrameArena::begin_frame(u32 slot, u64 required_bytes) {
    if (!device_ || slots_.empty()) {
        return false;
    }
    current_ = slot % static_cast<u32>(slots_.size());
    cursor_ = 0;
    Slot& s = slots_[current_];
    if (s.capacity < required_bytes || !s.mapped) {
        const u64 cap = std::max<u64>(required_bytes + required_bytes / 2, s.capacity * 2);
        if (!create_slot(s, cap)) {
            return false;
        }
    }
    return true;
}

FrameArena::Allocation FrameArena::allocate(u64 size) {
    Allocation a;
    if (slots_.empty()) {
        return a;
    }
    Slot& s = slots_[current_];
    const u64 aligned = padded(cursor_);
    if (!s.mapped || aligned + size > s.capacity) {
        AE_LOG_ERROR("Renderer", "FrameArena: out of space ({} + {} > {})", aligned, size, s.capacity);
        return a;
    }
    a.cpu = s.mapped + aligned;
    a.gpu = s.address + aligned;
    a.offset = aligned;
    a.size = size;
    cursor_ = aligned + size;
    return a;
}

rhi::BufferHandle FrameArena::current_buffer() const noexcept {
    return slots_.empty() ? rhi::BufferHandle{} : slots_[current_].buffer;
}

u64 FrameArena::capacity(u32 slot) const noexcept {
    return slot < slots_.size() ? slots_[slot].capacity : 0;
}

} // namespace aether::renderer
