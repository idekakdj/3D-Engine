// aether/rhi/command_list.h — command recording interface.
// FROZEN CONTRACT (ADR-0001). Recorded on worker threads, submitted by the Device.
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"
#include "aether/rhi/resources.h"

namespace aether::rhi {

struct Viewport { f32 x, y, width, height, min_depth, max_depth; };
struct Scissor  { i32 x, y; u32 width, height; };

// Abstract command recorder. One instance per worker thread per frame.
class CommandList {
public:
    virtual ~CommandList() = default;

    // Dynamic rendering scope (no VkRenderPass).
    virtual void begin_rendering(const RenderingInfo& info) = 0;
    virtual void end_rendering() = 0;

    virtual void set_viewport(const Viewport& vp) = 0;
    virtual void set_scissor(const Scissor& sc) = 0;

    virtual void bind_pipeline(PipelineHandle pipeline) = 0;
    // Bindless global descriptor set is bound implicitly by bind_pipeline. Push constants
    // carry bindless indices + buffer device addresses. Universal layout: <=128 bytes,
    // visible to ALL stages (backend ignores `stages` and uses VK_SHADER_STAGE_ALL).
    virtual void push_constants(ShaderStage stages, u32 offset, u32 size, const void* data) = 0;

    virtual void bind_vertex_buffer(u32 binding, BufferHandle buffer, u64 offset = 0) = 0;
    virtual void bind_index_buffer(BufferHandle buffer, u64 offset = 0, bool index16 = false) = 0;

    virtual void draw(u32 vertex_count, u32 instance_count = 1, u32 first_vertex = 0,
                      u32 first_instance = 0) = 0;
    virtual void draw_indexed(u32 index_count, u32 instance_count = 1, u32 first_index = 0,
                              i32 vertex_offset = 0, u32 first_instance = 0) = 0;
    virtual void draw_indexed_indirect(BufferHandle args, u64 offset, u32 draw_count, u32 stride) = 0;
    // GPU-driven path (M2): draw count read from `count_buffer` (core 1.2 drawIndirectCount).
    virtual void draw_indexed_indirect_count(BufferHandle args, u64 offset, BufferHandle count_buffer,
                                             u64 count_offset, u32 max_draws, u32 stride) = 0;

    virtual void dispatch(u32 gx, u32 gy, u32 gz) = 0;
    virtual void dispatch_indirect(BufferHandle args, u64 offset) = 0;

    // Dynamic depth bias (pipeline must set DepthState::bias_enable).
    virtual void set_depth_bias(f32 constant_factor, f32 clamp, f32 slope_factor) = 0;

    // vkCmdFillBuffer: e.g. reset GPU counters. size = ~0ull means whole buffer.
    virtual void fill_buffer(BufferHandle buffer, u64 offset, u64 size, u32 value) = 0;

    // Explicit resource state transitions (sync2-shaped).
    virtual void barrier(TextureHandle texture, ResourceState from, ResourceState to) = 0;
    virtual void barrier(BufferHandle buffer, ResourceState from, ResourceState to) = 0;

    virtual void copy_buffer(BufferHandle src, BufferHandle dst, u64 size, u64 src_off = 0,
                             u64 dst_off = 0) = 0;
    virtual void copy_buffer_to_texture(BufferHandle src, TextureHandle dst, u32 mip = 0) = 0;

    // Debug markers for RenderDoc / validation.
    virtual void push_debug_group(const char* name) = 0;
    virtual void pop_debug_group() = 0;
};

} // namespace aether::rhi
