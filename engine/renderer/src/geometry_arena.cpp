// geometry_arena.cpp — shared vertex/index arena (see geometry_arena.h).
#include "geometry_arena.h"

#include "aether/core/log.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace aether::renderer {

namespace {
constexpr const char* kLogCat = "Renderer";

ByteSpan as_bytes_of(const void* data, u64 size) {
    return ByteSpan(static_cast<const byte*>(data), static_cast<usize>(size));
}
} // namespace

bool GeometryArena::create_sub(SubArena& sub, u64 capacity) {
    rhi::BufferDesc desc;
    desc.size = capacity * sub.stride;
    desc.usage = sub.usage | rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSrc |
                 rhi::BufferUsage::TransferDst;
    desc.memory = rhi::MemoryUsage::GpuOnly;
    desc.debug_name = sub.name;
    sub.buffer = device_->create_buffer(desc);
    if (!sub.buffer.is_valid()) {
        AE_LOG_ERROR(kLogCat, "GeometryArena: failed to create '{}' ({} bytes)", sub.name, desc.size);
        return false;
    }
    sub.alloc.reset(capacity);
    return true;
}

bool GeometryArena::init(rhi::Device& device, const Capacities& caps) {
    device_ = &device;

    indices_.stride = sizeof(u32);
    indices_.usage = rhi::BufferUsage::Index;
    indices_.name = "GeometryArena.Indices";

    static_.stride = sizeof(Vertex);
    static_.usage = rhi::BufferUsage::Vertex;
    static_.name = "GeometryArena.StaticVertices";

    skinned_.stride = sizeof(Vertex);
    skinned_.usage = rhi::BufferUsage::Vertex;
    skinned_.name = "GeometryArena.SkinnedVertices";

    if (!create_sub(indices_, caps.indices) || !create_sub(static_, caps.static_vertices) ||
        !create_sub(skinned_, caps.skinned_vertices)) {
        return false;
    }
    rhi::BufferDesc skin;
    skin.size = caps.skinned_vertices * sizeof(SkinVertex);
    skin.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::Storage |
                 rhi::BufferUsage::TransferSrc | rhi::BufferUsage::TransferDst;
    skin.debug_name = "GeometryArena.SkinStream";
    skin_buffer_ = device.create_buffer(skin);
    return skin_buffer_.is_valid();
}

void GeometryArena::shutdown() {
    if (!device_) {
        return;
    }
    for (PendingCopy& p : pending_) {
        device_->destroy(p.src);
    }
    pending_.clear();
    for (SubArena* s : { &indices_, &static_, &skinned_ }) {
        if (s->buffer.is_valid()) {
            device_->destroy(s->buffer);
        }
        s->buffer = {};
        s->alloc.reset(0);
    }
    if (skin_buffer_.is_valid()) {
        device_->destroy(skin_buffer_);
    }
    skin_buffer_ = {};
    device_ = nullptr;
}

bool GeometryArena::grow(SubArena& sub, u64 needed, rhi::BufferHandle* companion,
                         u64 companion_stride) {
    const u64 old_cap = sub.alloc.capacity();
    u64 new_cap = std::max<u64>(old_cap * 2, 1024);
    while (new_cap - sub.alloc.used() < needed || new_cap < old_cap + needed) {
        new_cap *= 2;
    }

    PendingCopy copy;
    copy.src = sub.buffer;
    copy.stride = sub.stride;
    sub.alloc.for_each_allocated([&](RangeAllocator::Range r) { copy.ranges.push_back(r); });

    rhi::BufferDesc desc;
    desc.size = new_cap * sub.stride;
    desc.usage = sub.usage | rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSrc |
                 rhi::BufferUsage::TransferDst;
    desc.debug_name = sub.name;
    const rhi::BufferHandle nb = device_->create_buffer(desc);
    if (!nb.is_valid()) {
        AE_LOG_ERROR(kLogCat, "GeometryArena: growing '{}' to {} elements failed", sub.name, new_cap);
        return false;
    }
    copy.dst = nb;

    if (companion) {
        rhi::BufferDesc cdesc;
        cdesc.size = new_cap * companion_stride;
        cdesc.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::Storage |
                      rhi::BufferUsage::TransferSrc | rhi::BufferUsage::TransferDst;
        cdesc.debug_name = "GeometryArena.SkinStream";
        const rhi::BufferHandle cb = device_->create_buffer(cdesc);
        if (!cb.is_valid()) {
            device_->destroy(nb);
            return false;
        }
        PendingCopy ccopy;
        ccopy.src = *companion;
        ccopy.dst = cb;
        ccopy.stride = companion_stride;
        ccopy.ranges = copy.ranges;
        pending_.push_back(std::move(ccopy));
        *companion = cb;
    }

    pending_.push_back(std::move(copy));
    sub.buffer = nb;
    sub.alloc.grow(new_cap);
    AE_LOG_INFO(kLogCat, "GeometryArena: grew '{}' {} -> {} elements", sub.name, old_cap, new_cap);
    return true;
}

std::optional<GeometryAllocation> GeometryArena::upload(Span<const Vertex> vertices,
                                                        Span<const SkinVertex> skin,
                                                        Span<const u32> indices) {
    if (!device_ || vertices.empty() || indices.empty()) {
        return std::nullopt;
    }
    const bool skinned = !skin.empty();
    if (skinned && skin.size() != vertices.size()) {
        return std::nullopt;
    }

    SubArena& vsub = skinned ? skinned_ : static_;
    auto vrange = vsub.alloc.allocate(vertices.size());
    if (!vrange) {
        if (!grow(vsub, vertices.size(), skinned ? &skin_buffer_ : nullptr, sizeof(SkinVertex))) {
            return std::nullopt;
        }
        vrange = vsub.alloc.allocate(vertices.size());
    }
    auto irange = indices_.alloc.allocate(indices.size());
    if (!irange) {
        if (!grow(indices_, indices.size(), nullptr, 0)) {
            vsub.alloc.free(*vrange);
            return std::nullopt;
        }
        irange = indices_.alloc.allocate(indices.size());
    }
    if (!vrange || !irange) {
        return std::nullopt;
    }

    device_->update_buffer(vsub.buffer, as_bytes_of(vertices.data(), vertices.size_bytes()),
                           vrange->offset * sizeof(Vertex));
    if (skinned) {
        device_->update_buffer(skin_buffer_, as_bytes_of(skin.data(), skin.size_bytes()),
                               vrange->offset * sizeof(SkinVertex));
    }
    device_->update_buffer(indices_.buffer, as_bytes_of(indices.data(), indices.size_bytes()),
                           irange->offset * sizeof(u32));

    GeometryAllocation out;
    out.indices = *irange;
    out.vertices = *vrange;
    out.skinned = skinned;
    return out;
}

void GeometryArena::release(const GeometryAllocation& alloc) {
    (alloc.skinned ? skinned_ : static_).alloc.free(alloc.vertices);
    indices_.alloc.free(alloc.indices);
}

void GeometryArena::record_pending_copies(rhi::CommandList& cmd) {
    if (pending_.empty()) {
        return;
    }
    cmd.push_debug_group("GeometryArena.Grow");
    for (PendingCopy& p : pending_) {
        cmd.barrier(p.src, rhi::ResourceState::ShaderRead, rhi::ResourceState::TransferSrc);
        cmd.barrier(p.dst, rhi::ResourceState::Undefined, rhi::ResourceState::TransferDst);
        for (const RangeAllocator::Range& r : p.ranges) {
            const u64 off = r.offset * p.stride;
            cmd.copy_buffer(p.src, p.dst, r.size * p.stride, off, off);
        }
        cmd.barrier(p.dst, rhi::ResourceState::TransferDst, rhi::ResourceState::ShaderRead);
        device_->destroy(p.src); // deferred by the device until the copy has executed
    }
    cmd.pop_debug_group();
    pending_.clear();
}

u64 GeometryArena::bytes_allocated() const noexcept {
    return indices_.alloc.capacity() * indices_.stride + static_.alloc.capacity() * static_.stride +
           skinned_.alloc.capacity() * (skinned_.stride + sizeof(SkinVertex));
}

} // namespace aether::renderer
