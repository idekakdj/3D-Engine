// vk_command_list.h — Vulkan implementation of rhi::CommandList.
#pragma once

#include "aether/rhi/command_list.h"
#include "aether/rhi/device.h"
#include "vk_common.h"

namespace aether::rhi::vk {

class VulkanDevice;

class VulkanCommandList final : public CommandList {
public:
    explicit VulkanCommandList(VulkanDevice& device) : device_(device) {}

    // Attach to an already-begun command buffer and reset all tracking/counters.
    void            begin(VkCommandBuffer cmd);
    VkCommandBuffer vk() const { return cmd_; }
    // Foreign code (the ImGui backend) bound its own layout/pipeline: re-bind lazily.
    void            invalidate_bindings() { graphics_set_bound_ = compute_set_bound_ = false; }
    bool            in_rendering() const { return in_rendering_; }
    FrameStats      counters() const { return stats_; }

    void begin_rendering(const RenderingInfo& info) override;
    void end_rendering() override;
    void set_viewport(const Viewport& vp) override;
    void set_scissor(const Scissor& sc) override;
    void bind_pipeline(PipelineHandle pipeline) override;
    void push_constants(ShaderStage stages, u32 offset, u32 size, const void* data) override;
    void bind_vertex_buffer(u32 binding, BufferHandle buffer, u64 offset) override;
    void bind_index_buffer(BufferHandle buffer, u64 offset, bool index16) override;
    void draw(u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) override;
    void draw_indexed(u32 index_count, u32 instance_count, u32 first_index, i32 vertex_offset,
                      u32 first_instance) override;
    void draw_indexed_indirect(BufferHandle args, u64 offset, u32 draw_count, u32 stride) override;
    void draw_indexed_indirect_count(BufferHandle args, u64 offset, BufferHandle count_buffer,
                                     u64 count_offset, u32 max_draws, u32 stride) override;
    void dispatch(u32 gx, u32 gy, u32 gz) override;
    void dispatch_indirect(BufferHandle args, u64 offset) override;
    void set_depth_bias(f32 constant_factor, f32 clamp, f32 slope_factor) override;
    void fill_buffer(BufferHandle buffer, u64 offset, u64 size, u32 value) override;
    void barrier(TextureHandle texture, ResourceState from, ResourceState to) override;
    void barrier(BufferHandle buffer, ResourceState from, ResourceState to) override;
    void copy_buffer(BufferHandle src, BufferHandle dst, u64 size, u64 src_off, u64 dst_off) override;
    void copy_buffer_to_texture(BufferHandle src, TextureHandle dst, u32 mip) override;
    void push_debug_group(const char* name) override;
    void pop_debug_group() override;

private:
    void bind_set(VkPipelineBindPoint point);

    VulkanDevice&   device_;
    VkCommandBuffer cmd_                = VK_NULL_HANDLE;
    bool            graphics_set_bound_ = false;
    bool            compute_set_bound_  = false;
    bool            in_rendering_       = false;
    FrameStats      stats_{};
};

} // namespace aether::rhi::vk
