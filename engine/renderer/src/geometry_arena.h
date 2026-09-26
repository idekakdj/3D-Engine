// geometry_arena.h — one large GPU geometry arena shared by every registered mesh.
//
// Private header. Three sub-arenas, each a big GpuOnly buffer + RangeAllocator (in
// elements):
//   * indices        : u32 index buffer shared by all meshes
//   * static vertices: Vertex stream for non-skinned meshes
//   * skinned        : Vertex + SkinVertex streams allocated in lockstep (same element
//                      offsets), so binding 0 and binding 1 share one vertexOffset.
// All meshes draw from the same buffers with (first_index, vertex_offset), which is what
// the M2 GPU-driven path (indirect draws, one bind per frame) needs.
//
// Growth: when a sub-arena is full a 2x buffer is created; the live ranges are copied
// GPU-side at the start of the next frame (record_pending_copies) - AFTER the device has
// flushed earlier update_buffer() uploads into the old buffer - and the old buffer is
// destroyed (deferred by the device). New allocations never overlap a pending copy
// because released ranges are only recycled after the frames-in-flight delay.
// Main thread only.
#pragma once

#include "range_allocator.h"

#include "aether/core/geometry.h"
#include "aether/core/types.h"
#include "aether/rhi/command_list.h"
#include "aether/rhi/device.h"

#include <optional>
#include <string>
#include <vector>

namespace aether::renderer {

struct GeometryAllocation {
    RangeAllocator::Range indices{};
    RangeAllocator::Range vertices{};
    bool                  skinned = false;
};

class GeometryArena {
public:
    struct Capacities {
        u64 indices = 2u << 20;          // 2M indices   (8 MB)
        u64 static_vertices = 512u << 10; // 512K verts  (24 MB)
        u64 skinned_vertices = 64u << 10; // 64K verts   (3 MB + 1.5 MB)
    };

    GeometryArena() = default;
    GeometryArena(const GeometryArena&) = delete;
    GeometryArena& operator=(const GeometryArena&) = delete;

    bool init(rhi::Device& device, const Capacities& caps);
    void shutdown();

    // Allocates ranges and queues the uploads. Returns nullopt on invalid input or if a
    // buffer could not be created.
    std::optional<GeometryAllocation> upload(Span<const Vertex> vertices,
                                             Span<const SkinVertex> skin,
                                             Span<const u32> indices);
    void release(const GeometryAllocation& alloc);

    // Records GPU copies for buffers that grew since the last frame. Call at the very
    // start of the frame's command recording (outside any rendering scope).
    void record_pending_copies(rhi::CommandList& cmd);
    [[nodiscard]] bool has_pending_copies() const noexcept { return !pending_.empty(); }

    [[nodiscard]] rhi::BufferHandle index_buffer() const noexcept { return indices_.buffer; }
    [[nodiscard]] rhi::BufferHandle static_vertex_buffer() const noexcept { return static_.buffer; }
    [[nodiscard]] rhi::BufferHandle skinned_vertex_buffer() const noexcept { return skinned_.buffer; }
    [[nodiscard]] rhi::BufferHandle skin_buffer() const noexcept { return skin_buffer_; }

    [[nodiscard]] u64 bytes_allocated() const noexcept;

private:
    struct SubArena {
        rhi::BufferHandle buffer;
        RangeAllocator    alloc;
        u64               stride = 0;
        rhi::BufferUsage  usage = rhi::BufferUsage::None;
        std::string       name;
    };
    struct PendingCopy {
        rhi::BufferHandle                  src;
        rhi::BufferHandle                  dst;
        u64                                stride = 0;
        std::vector<RangeAllocator::Range> ranges;
    };

    bool create_sub(SubArena& sub, u64 capacity);
    // Grows `sub` (and optionally a lockstep companion buffer) to fit `needed` elements.
    bool grow(SubArena& sub, u64 needed, rhi::BufferHandle* companion, u64 companion_stride);

    rhi::Device*             device_ = nullptr;
    SubArena                 indices_;
    SubArena                 static_;
    SubArena                 skinned_;
    rhi::BufferHandle        skin_buffer_; // lockstep with skinned_
    std::vector<PendingCopy> pending_;
};

} // namespace aether::renderer
