// vk_command_list.cpp — rhi::CommandList recording on a VkCommandBuffer (sync2 barriers,
// dynamic rendering, universal pipeline layout with the bindless set bound lazily).
#include "vk_command_list.h"

#include "vk_convert.h"
#include "vk_device.h"

#include <algorithm>
#include <array>

namespace aether::rhi::vk {
namespace {

constexpr u32 kMaxColorAttachments = 8;
constexpr u32 kPushConstantBytes   = 128;

} // namespace

void VulkanCommandList::begin(VkCommandBuffer cmd) {
    cmd_                = cmd;
    graphics_set_bound_ = false;
    compute_set_bound_  = false;
    in_rendering_       = false;
    stats_              = {};
}

void VulkanCommandList::bind_set(VkPipelineBindPoint point) {
    bool& bound = point == VK_PIPELINE_BIND_POINT_COMPUTE ? compute_set_bound_ : graphics_set_bound_;
    if (bound) {
        return; // one universal layout: the set stays bound across pipeline switches
    }
    const VkDescriptorSet set = device_.bindless_set();
    vkCmdBindDescriptorSets(cmd_, point, device_.pipeline_layout(), 0, 1, &set, 0, nullptr);
    bound = true;
}

void VulkanCommandList::begin_rendering(const RenderingInfo& info) {
    AE_ASSERT_MSG(!in_rendering_, "begin_rendering while a rendering scope is already open");
    AE_ASSERT(info.color.size() <= kMaxColorAttachments);

    std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> colors{};
    UVec2 extent = info.render_area;
    u32   count  = 0;

    auto mip_extent = [](const TextureRecord& t, u32 mip) {
        return UVec2(std::max(1u, t.desc.width >> mip), std::max(1u, t.desc.height >> mip));
    };

    for (const ColorAttachment& c : info.color) {
        const TextureRecord* t = device_.texture(c.texture);
        if (!t) {
            AE_LOG_ERROR("RHI", "begin_rendering: invalid color attachment handle");
            continue;
        }
        VkRenderingAttachmentInfo& a = colors[count++];
        a.sType                      = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        a.imageView                  = device_.attachment_view(*t, c.mip, c.layer);
        a.imageLayout                = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        a.loadOp                     = to_vk(c.load);
        a.storeOp                    = to_vk(c.store);
        a.clearValue.color           = { { c.clear_color.x, c.clear_color.y, c.clear_color.z, c.clear_color.w } };
        if (extent.x == 0 || extent.y == 0) {
            extent = mip_extent(*t, c.mip);
        }
    }

    VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo stencil{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    bool                      use_depth = false, use_stencil = false;
    if (info.has_depth) {
        if (const TextureRecord* t = device_.texture(info.depth.texture)) {
            depth.imageView               = device_.attachment_view(*t, info.depth.mip, info.depth.layer);
            depth.imageLayout             = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
            depth.loadOp                  = to_vk(info.depth.load);
            depth.storeOp                 = to_vk(info.depth.store);
            depth.clearValue.depthStencil = { info.depth.clear_depth, info.depth.clear_stencil };
            use_depth                     = true;
            if (has_stencil(t->desc.format)) {
                stencil     = depth;
                use_stencil = true;
            }
            if (extent.x == 0 || extent.y == 0) {
                extent = mip_extent(*t, info.depth.mip);
            }
        } else {
            AE_LOG_ERROR("RHI", "begin_rendering: invalid depth attachment handle");
        }
    }

    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea           = { { 0, 0 }, { extent.x, extent.y } };
    ri.layerCount           = 1;
    ri.colorAttachmentCount = count;
    ri.pColorAttachments    = colors.data();
    ri.pDepthAttachment     = use_depth ? &depth : nullptr;
    ri.pStencilAttachment   = use_stencil ? &stencil : nullptr;
    vkCmdBeginRendering(cmd_, &ri);
    in_rendering_ = true;

    // Sensible defaults for the dynamic viewport/scissor (full render area, depth 0..1).
    set_viewport({ 0.0f, 0.0f, static_cast<f32>(extent.x), static_cast<f32>(extent.y), 0.0f, 1.0f });
    set_scissor({ 0, 0, extent.x, extent.y });
}

void VulkanCommandList::end_rendering() {
    AE_ASSERT_MSG(in_rendering_, "end_rendering without begin_rendering");
    vkCmdEndRendering(cmd_);
    in_rendering_ = false;
}

void VulkanCommandList::set_viewport(const Viewport& vp) {
    const VkViewport v{ vp.x, vp.y, std::max(vp.width, 1.0f), std::max(vp.height, 1.0f), vp.min_depth, vp.max_depth };
    vkCmdSetViewport(cmd_, 0, 1, &v);
}

void VulkanCommandList::set_scissor(const Scissor& sc) {
    const VkRect2D r{ { std::max(sc.x, 0), std::max(sc.y, 0) }, { sc.width, sc.height } };
    vkCmdSetScissor(cmd_, 0, 1, &r);
}

void VulkanCommandList::bind_pipeline(PipelineHandle pipeline) {
    const PipelineRecord* p = device_.pipeline(pipeline);
    if (!p) {
        AE_LOG_ERROR("RHI", "bind_pipeline: invalid pipeline handle");
        return;
    }
    vkCmdBindPipeline(cmd_, p->bind_point, p->pipeline);
    bind_set(p->bind_point);
    ++stats_.pipeline_binds;
}

void VulkanCommandList::push_constants(ShaderStage /*stages*/, u32 offset, u32 size, const void* data) {
    AE_ASSERT_MSG(offset + size <= kPushConstantBytes, "push constants exceed the 128-byte universal range");
    AE_ASSERT_MSG((offset % 4) == 0 && (size % 4) == 0, "push constant offset/size must be 4-byte aligned");
    vkCmdPushConstants(cmd_, device_.pipeline_layout(), VK_SHADER_STAGE_ALL, offset, size, data);
}

void VulkanCommandList::bind_vertex_buffer(u32 binding, BufferHandle buffer, u64 offset) {
    const BufferRecord* b = device_.buffer(buffer);
    if (!b) {
        AE_LOG_ERROR("RHI", "bind_vertex_buffer: invalid buffer handle");
        return;
    }
    const VkDeviceSize off = offset;
    vkCmdBindVertexBuffers(cmd_, binding, 1, &b->buffer, &off);
}

void VulkanCommandList::bind_index_buffer(BufferHandle buffer, u64 offset, bool index16) {
    const BufferRecord* b = device_.buffer(buffer);
    if (!b) {
        AE_LOG_ERROR("RHI", "bind_index_buffer: invalid buffer handle");
        return;
    }
    vkCmdBindIndexBuffer(cmd_, b->buffer, offset, index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void VulkanCommandList::draw(u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) {
    vkCmdDraw(cmd_, vertex_count, instance_count, first_vertex, first_instance);
    ++stats_.draw_calls;
}

void VulkanCommandList::draw_indexed(u32 index_count, u32 instance_count, u32 first_index, i32 vertex_offset,
                                     u32 first_instance) {
    vkCmdDrawIndexed(cmd_, index_count, instance_count, first_index, vertex_offset, first_instance);
    ++stats_.draw_calls;
}

void VulkanCommandList::draw_indexed_indirect(BufferHandle args, u64 offset, u32 draw_count, u32 stride) {
    const BufferRecord* b = device_.buffer(args);
    if (!b) {
        AE_LOG_ERROR("RHI", "draw_indexed_indirect: invalid buffer handle");
        return;
    }
    vkCmdDrawIndexedIndirect(cmd_, b->buffer, offset, draw_count, stride);
    ++stats_.draw_calls;
}

void VulkanCommandList::draw_indexed_indirect_count(BufferHandle args, u64 offset, BufferHandle count_buffer,
                                                    u64 count_offset, u32 max_draws, u32 stride) {
    if (!device_.features().draw_indirect_count) {
        AE_LOG_ERROR("RHI", "draw_indexed_indirect_count: drawIndirectCount not supported (check DeviceFeatures)");
        return;
    }
    const BufferRecord* a = device_.buffer(args);
    const BufferRecord* c = device_.buffer(count_buffer);
    if (!a || !c) {
        AE_LOG_ERROR("RHI", "draw_indexed_indirect_count: invalid buffer handle");
        return;
    }
    vkCmdDrawIndexedIndirectCount(cmd_, a->buffer, offset, c->buffer, count_offset, max_draws, stride);
    ++stats_.draw_calls;
}

void VulkanCommandList::draw_mesh_tasks(u32 gx, u32 gy, u32 gz) {
    if (!device_.features().mesh_shaders) {
        AE_LOG_ERROR("RHI", "draw_mesh_tasks: mesh shaders not supported (check DeviceFeatures)");
        return;
    }
    vkCmdDrawMeshTasksEXT(cmd_, gx, gy, gz);
    ++stats_.draw_calls;
}

void VulkanCommandList::draw_mesh_tasks_indirect_count(BufferHandle args, u64 offset, BufferHandle count_buffer,
                                                       u64 count_offset, u32 max_draws, u32 stride) {
    if (!device_.features().mesh_shaders || !device_.features().draw_indirect_count) {
        AE_LOG_ERROR("RHI", "draw_mesh_tasks_indirect_count: mesh shaders / drawIndirectCount not supported");
        return;
    }
    const BufferRecord* a = device_.buffer(args);
    const BufferRecord* c = device_.buffer(count_buffer);
    if (!a || !c) {
        AE_LOG_ERROR("RHI", "draw_mesh_tasks_indirect_count: invalid buffer handle");
        return;
    }
    vkCmdDrawMeshTasksIndirectCountEXT(cmd_, a->buffer, offset, c->buffer, count_offset, max_draws, stride);
    ++stats_.draw_calls;
}

void VulkanCommandList::dispatch(u32 gx, u32 gy, u32 gz) {
    vkCmdDispatch(cmd_, gx, gy, gz);
    ++stats_.dispatches;
}

void VulkanCommandList::dispatch_indirect(BufferHandle args, u64 offset) {
    const BufferRecord* b = device_.buffer(args);
    if (!b) {
        AE_LOG_ERROR("RHI", "dispatch_indirect: invalid buffer handle");
        return;
    }
    vkCmdDispatchIndirect(cmd_, b->buffer, offset);
    ++stats_.dispatches;
}

void VulkanCommandList::set_depth_bias(f32 constant_factor, f32 clamp, f32 slope_factor) {
    vkCmdSetDepthBias(cmd_, constant_factor, clamp, slope_factor);
}

void VulkanCommandList::fill_buffer(BufferHandle buffer, u64 offset, u64 size, u32 value) {
    const BufferRecord* b = device_.buffer(buffer);
    if (!b) {
        AE_LOG_ERROR("RHI", "fill_buffer: invalid buffer handle");
        return;
    }
    AE_ASSERT_MSG(in_rendering_ == false, "fill_buffer is a transfer command: record it outside rendering");
    vkCmdFillBuffer(cmd_, b->buffer, offset, size == ~0ull ? VK_WHOLE_SIZE : size, value);
}

void VulkanCommandList::barrier(TextureHandle texture, ResourceState from, ResourceState to) {
    AE_ASSERT_MSG(to != ResourceState::Undefined, "cannot transition a texture TO Undefined");
    const TextureRecord* t = device_.texture(texture);
    if (!t) {
        AE_LOG_ERROR("RHI", "barrier: invalid texture handle");
        return;
    }
    if (from == to && !is_write_state(from)) {
        return; // read -> same read state: nothing to do
    }
    const VkImageMemoryBarrier2 b = make_image_barrier(t->image, t->aspect, from, to);
    VkDependencyInfo            dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd_, &dep);
}

void VulkanCommandList::barrier(BufferHandle buffer, ResourceState from, ResourceState to) {
    const BufferRecord* br = device_.buffer(buffer);
    if (!br) {
        AE_LOG_ERROR("RHI", "barrier: invalid buffer handle");
        return;
    }
    if (from == to && !is_write_state(from)) {
        return;
    }
    const StateInfo        src = buffer_state(from, true);
    const StateInfo        dst = buffer_state(to, false);
    VkBufferMemoryBarrier2 b{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
    b.srcStageMask        = src.stages;
    b.srcAccessMask       = src.access;
    b.dstStageMask        = dst.stages;
    b.dstAccessMask       = dst.access;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer              = br->buffer;
    b.offset              = 0;
    b.size                = VK_WHOLE_SIZE;
    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd_, &dep);
}

void VulkanCommandList::copy_buffer(BufferHandle src, BufferHandle dst, u64 size, u64 src_off, u64 dst_off) {
    const BufferRecord* s = device_.buffer(src);
    const BufferRecord* d = device_.buffer(dst);
    if (!s || !d) {
        AE_LOG_ERROR("RHI", "copy_buffer: invalid buffer handle");
        return;
    }
    const VkBufferCopy region{ src_off, dst_off, size };
    vkCmdCopyBuffer(cmd_, s->buffer, d->buffer, 1, &region);
}

void VulkanCommandList::copy_buffer_to_texture(BufferHandle src, TextureHandle dst, u32 mip) {
    const BufferRecord*  s = device_.buffer(src);
    const TextureRecord* t = device_.texture(dst);
    if (!s || !t) {
        AE_LOG_ERROR("RHI", "copy_buffer_to_texture: invalid handle");
        return;
    }
    const bool        is3d = t->desc.type == TextureType::Tex3D;
    VkBufferImageCopy region{};
    region.imageSubresource = { sampled_aspect(t->desc.format), mip, 0, is3d ? 1u : t->desc.array_layers };
    region.imageExtent      = { std::max(1u, t->desc.width >> mip), std::max(1u, t->desc.height >> mip),
                                is3d ? std::max(1u, t->desc.depth >> mip) : 1u };
    vkCmdCopyBufferToImage(cmd_, s->buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void VulkanCommandList::push_debug_group(const char* name) {
    if (!device_.debug_utils_enabled()) {
        return;
    }
    VkDebugUtilsLabelEXT label{ VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
    label.pLabelName = name ? name : "";
    vkCmdBeginDebugUtilsLabelEXT(cmd_, &label);
}

void VulkanCommandList::pop_debug_group() {
    if (device_.debug_utils_enabled()) {
        vkCmdEndDebugUtilsLabelEXT(cmd_);
    }
}

} // namespace aether::rhi::vk
