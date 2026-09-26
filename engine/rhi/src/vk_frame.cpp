// vk_frame.cpp — frame pacing, submission/present, uploads, deferred deletion, staging.
#include "aether/core/window.h"
#include "vk_command_list.h"
#include "vk_convert.h"
#include "vk_device.h"

#include <algorithm>
#include <cstring>

namespace aether::rhi::vk {
namespace {

u64 align_up(u64 v, u64 a) {
    return (v + a - 1) / a * a;
}

void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    b.srcStageMask  = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask  = dst_stage;
    b.dstAccessMask = dst_access;
    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void image_barrier(VkCommandBuffer cmd, const VkImageMemoryBarrier2& b) {
    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// GpuToCpu readbacks: device writes must be made available to the HOST domain before the
// CPU observes the fence / timeline value (a queue signal alone does not do that).
void make_host_visible(VkCommandBuffer cmd) {
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                   VK_ACCESS_2_HOST_READ_BIT);
}

void begin_one_time(VkCommandBuffer cmd) {
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
}

} // namespace

// ---------------------------------------------------------------------------------------
// StagingArena
// ---------------------------------------------------------------------------------------

void StagingArena::init(VmaAllocator allocator, VkDevice device, u64 chunk_size) {
    allocator_  = allocator;
    device_     = device;
    chunk_size_ = chunk_size;
}

StagingAlloc StagingArena::allocate(u64 size, u64 alignment) {
    for (Chunk& c : chunks_) {
        const u64 offset = align_up(c.offset, alignment);
        if (offset + size <= c.size) {
            c.offset = offset + size;
            return { c.buffer, offset, c.mapped + offset };
        }
    }
    Chunk              c;
    c.size = std::max(chunk_size_, align_up(size, 256));
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size        = c.size;
    bi.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage         = VMA_MEMORY_USAGE_AUTO;
    ai.flags         = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT; // no flushes
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(allocator_, &bi, &ai, &c.buffer, &c.allocation, &info));
    c.mapped = static_cast<u8*>(info.pMappedData);
    c.offset = size;
    chunks_.push_back(c);
    return { c.buffer, 0, c.mapped };
}

void StagingArena::reset() {
    // Keep default-sized chunks for reuse; release one-off oversized ones.
    auto it = std::remove_if(chunks_.begin(), chunks_.end(), [&](Chunk& c) {
        if (c.size > chunk_size_) {
            vmaDestroyBuffer(allocator_, c.buffer, c.allocation);
            return true;
        }
        c.offset = 0;
        return false;
    });
    chunks_.erase(it, chunks_.end());
}

void StagingArena::destroy() {
    for (Chunk& c : chunks_) {
        vmaDestroyBuffer(allocator_, c.buffer, c.allocation);
    }
    chunks_.clear();
}

// ---------------------------------------------------------------------------------------
// slots / deferred deletion
// ---------------------------------------------------------------------------------------

u64 VulkanDevice::completed_timeline_value() const {
    u64 value = 0;
    VK_CHECK(vkGetSemaphoreCounterValue(device_, timeline_, &value));
    return value;
}

void VulkanDevice::prepare_slot(FrameSlot& slot) {
    if (slot.prepared) {
        return;
    }
    if (slot.submitted_value > 0) {
        VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores    = &timeline_;
        wi.pValues        = &slot.submitted_value;
        VK_CHECK(vkWaitSemaphores(device_, &wi, UINT64_MAX));
        query_timestamps(slot);
    }
    slot.staging.reset();
    VK_CHECK(vkResetCommandPool(device_, slot.pool, 0));
    slot.upload_cmds_used = 0;
    slot.upload_open      = VK_NULL_HANDLE;
    slot.prepared         = true;
    process_deferred(completed_timeline_value(), false);
}

void VulkanDevice::query_timestamps(FrameSlot& slot) {
    FrameStats stats = slot.recorded_stats;
    if (slot.timestamps_written && slot.timestamps) {
        u64            ticks[2] = {};
        const VkResult r        = vkGetQueryPoolResults(device_, slot.timestamps, 0, 2, sizeof(ticks), ticks,
                                                        sizeof(u64), VK_QUERY_RESULT_64_BIT);
        if (r == VK_SUCCESS && ticks[1] >= ticks[0]) {
            stats.gpu_time_ms = static_cast<f64>(ticks[1] - ticks[0]) * static_cast<f64>(timestamp_period_) * 1e-6;
        }
        slot.timestamps_written = false;
    }
    last_stats_ = stats;
}

void VulkanDevice::defer(std::function<void()> fn) {
    std::lock_guard lock(deferred_mutex_);
    // The next frame submission is the last one that may still reference the object.
    deferred_.push_back({ timeline_value_.load(std::memory_order_acquire) + 1, std::move(fn) });
}

void VulkanDevice::process_deferred(u64 completed_value, bool everything) {
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard lock(deferred_mutex_);
        while (!deferred_.empty() && (everything || deferred_.front().value <= completed_value)) {
            ready.push_back(std::move(deferred_.front().fn));
            deferred_.pop_front();
        }
    }
    for (auto& fn : ready) {
        fn();
    }
}

// ---------------------------------------------------------------------------------------
// uploads
// ---------------------------------------------------------------------------------------

VkCommandBuffer VulkanDevice::upload_cmd() {
    FrameSlot& slot = current_slot();
    prepare_slot(slot);
    if (slot.upload_open) {
        return slot.upload_open;
    }
    if (slot.upload_cmds_used == slot.upload_cmds.size()) {
        VkCommandBufferAllocateInfo alloc{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        alloc.commandPool        = slot.pool;
        alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkCommandBuffer cmd      = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(device_, &alloc, &cmd));
        slot.upload_cmds.push_back(cmd);
    }
    VkCommandBuffer cmd = slot.upload_cmds[slot.upload_cmds_used++];
    begin_one_time(cmd);
    // Earlier GPU work may still read/write what we are about to overwrite (WAR/WAW).
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    slot.upload_open = cmd;
    return cmd;
}

VkCommandBuffer VulkanDevice::close_upload_cmd(FrameSlot& slot) {
    VkCommandBuffer cmd = slot.upload_open;
    if (!cmd) {
        return VK_NULL_HANDLE;
    }
    // Make every transfer write visible to all later work (buffers; images carry their own).
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    VK_CHECK(vkEndCommandBuffer(cmd));
    slot.upload_open = VK_NULL_HANDLE;
    return cmd;
}

StagingAlloc VulkanDevice::stage(ByteSpan data, u64 alignment) {
    FrameSlot&   slot = current_slot();
    StagingAlloc s    = slot.staging.allocate(data.size(), alignment);
    std::memcpy(s.ptr, data.data(), data.size());
    return s;
}

void VulkanDevice::update_buffer(BufferHandle handle, ByteSpan data, u64 dst_offset) {
    BufferRecord* b = buffers_.get(handle);
    if (!b) {
        AE_LOG_ERROR("RHI", "update_buffer: invalid buffer handle");
        return;
    }
    if (data.empty()) {
        return;
    }
    if (dst_offset + data.size() > b->desc.size) {
        AE_LOG_ERROR("RHI", "update_buffer '{}': {} bytes at offset {} exceeds size {}", b->desc.debug_name,
                     data.size(), dst_offset, b->desc.size);
        return;
    }
    if (b->mapped) {
        std::memcpy(static_cast<u8*>(b->mapped) + dst_offset, data.data(), data.size());
        VK_CHECK(vmaFlushAllocation(allocator_, b->allocation, dst_offset, data.size()));
        return;
    }
    VkCommandBuffer    cmd = upload_cmd();
    const StagingAlloc s   = stage(data, 16);
    const VkBufferCopy region{ s.offset, dst_offset, data.size() };
    vkCmdCopyBuffer(cmd, s.buffer, b->buffer, 1, &region);
}

void* VulkanDevice::map(BufferHandle handle) {
    BufferRecord* b = buffers_.get(handle);
    if (!b) {
        AE_LOG_ERROR("RHI", "map: invalid buffer handle");
        return nullptr;
    }
    if (!b->mapped) {
        AE_LOG_ERROR("RHI", "map: buffer '{}' is GpuOnly (not host visible)", b->desc.debug_name);
    }
    return b->mapped;
}

void VulkanDevice::unmap(BufferHandle handle) {
    // Mapping is persistent; unmap only publishes host writes on non-coherent memory.
    if (BufferRecord* b = buffers_.get(handle); b && b->mapped) {
        VK_CHECK(vmaFlushAllocation(allocator_, b->allocation, 0, VK_WHOLE_SIZE));
    }
}

bool VulkanDevice::format_supports_blit(VkFormat format, bool& linear) const {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(gpu_, format, &props);
    const VkFormatFeatureFlags f = props.optimalTilingFeatures;
    linear = (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    return (f & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (f & VK_FORMAT_FEATURE_BLIT_DST_BIT);
}

// Expects every mip in TransferDst with mip 0 written; leaves every mip in ShaderRead.
void VulkanDevice::generate_mips(VkCommandBuffer cmd, const TextureRecord& t) {
    const u32  mips   = t.desc.mip_levels;
    const bool is3d   = t.desc.type == TextureType::Tex3D;
    const u32  layers = is3d ? 1u : t.desc.array_layers;
    bool       linear = false;
    const bool blit   = format_supports_blit(t.format, linear) && !is_block_compressed(t.desc.format) &&
                      !is_depth_format(t.desc.format);
    if (!blit) {
        AE_LOG_WARN("RHI", "texture '{}': format cannot be blitted; mips 1..{} left undefined", t.desc.debug_name,
                    mips - 1);
        image_barrier(cmd, make_image_barrier(t.image, t.aspect, ResourceState::TransferDst, ResourceState::ShaderRead));
        return;
    }
    i32 w = static_cast<i32>(t.desc.width);
    i32 h = static_cast<i32>(t.desc.height);
    i32 d = static_cast<i32>(is3d ? t.desc.depth : 1u);
    for (u32 mip = 1; mip < mips; ++mip) {
        image_barrier(cmd, make_image_barrier(t.image, t.aspect, ResourceState::TransferDst, ResourceState::TransferSrc,
                                              mip - 1, 1, 0, layers));
        const i32   nw = std::max(1, w / 2), nh = std::max(1, h / 2), nd = std::max(1, d / 2);
        VkImageBlit region{};
        region.srcSubresource = { t.aspect, mip - 1, 0, layers };
        region.srcOffsets[1]  = { w, h, d };
        region.dstSubresource = { t.aspect, mip, 0, layers };
        region.dstOffsets[1]  = { nw, nh, nd };
        vkCmdBlitImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        w = nw;
        h = nh;
        d = nd;
    }
    VkImageMemoryBarrier2 bs[2] = {
        make_image_barrier(t.image, t.aspect, ResourceState::TransferSrc, ResourceState::ShaderRead, 0, mips - 1, 0,
                           layers),
        make_image_barrier(t.image, t.aspect, ResourceState::TransferDst, ResourceState::ShaderRead, mips - 1, 1, 0,
                           layers),
    };
    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers    = bs;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void VulkanDevice::update_texture(TextureHandle handle, ByteSpan data, bool gen_mips) {
    TextureRecord* t = textures_.get(handle);
    if (!t || !t->owns_image) {
        AE_LOG_ERROR("RHI", "update_texture: invalid texture handle");
        return;
    }
    const bool is3d     = t->desc.type == TextureType::Tex3D;
    const u32  layers   = is3d ? 1u : t->desc.array_layers;
    const u64  expected = subresource_bytes(t->desc.format, t->desc.width, t->desc.height, is3d ? t->desc.depth : 1u) *
                         layers;
    if (data.size() < expected) {
        AE_LOG_ERROR("RHI", "update_texture '{}': got {} bytes, need {}", t->desc.debug_name, data.size(), expected);
        return;
    }
    VkCommandBuffer    cmd = upload_cmd();
    const StagingAlloc s   = stage(data.first(expected), 16);

    const ResourceState from = t->initialized ? ResourceState::ShaderRead : ResourceState::Undefined;
    image_barrier(cmd, make_image_barrier(t->image, t->aspect, from, ResourceState::TransferDst));

    VkBufferImageCopy region{};
    region.bufferOffset     = s.offset;
    region.imageSubresource = { sampled_aspect(t->desc.format), 0, 0, layers };
    region.imageExtent      = { t->desc.width, t->desc.height, is3d ? t->desc.depth : 1u };
    vkCmdCopyBufferToImage(cmd, s.buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    if (gen_mips && t->desc.mip_levels > 1) {
        generate_mips(cmd, *t);
    } else {
        image_barrier(cmd, make_image_barrier(t->image, t->aspect, ResourceState::TransferDst, ResourceState::ShaderRead));
    }
    t->initialized = true;
}

void VulkanDevice::update_texture_mip(TextureHandle handle, u32 mip, u32 layer, ByteSpan data) {
    TextureRecord* t = textures_.get(handle);
    if (!t || !t->owns_image) {
        AE_LOG_ERROR("RHI", "update_texture_mip: invalid texture handle");
        return;
    }
    const bool is3d = t->desc.type == TextureType::Tex3D;
    if (mip >= t->desc.mip_levels || (!is3d && layer >= t->desc.array_layers)) {
        AE_LOG_ERROR("RHI", "update_texture_mip '{}': mip {} / layer {} out of range", t->desc.debug_name, mip, layer);
        return;
    }
    const u32 w = std::max(1u, t->desc.width >> mip);
    const u32 h = std::max(1u, t->desc.height >> mip);
    const u32 d = is3d ? std::max(1u, t->desc.depth >> mip) : 1u;
    const u64 expected = subresource_bytes(t->desc.format, w, h, d);
    if (data.size() < expected) {
        AE_LOG_ERROR("RHI", "update_texture_mip '{}': got {} bytes, need {}", t->desc.debug_name, data.size(), expected);
        return;
    }
    VkCommandBuffer    cmd = upload_cmd();
    const StagingAlloc s   = stage(data.first(expected), 16);

    // First upload initialises the whole image (known layout for every subresource).
    const bool whole = !t->initialized;
    const u32  bm = whole ? 0 : mip, mc = whole ? VK_REMAINING_MIP_LEVELS : 1;
    const u32  bl = whole ? 0 : (is3d ? 0 : layer), lc = whole ? VK_REMAINING_ARRAY_LAYERS : 1;
    image_barrier(cmd, make_image_barrier(t->image, t->aspect, whole ? ResourceState::Undefined : ResourceState::ShaderRead,
                                          ResourceState::TransferDst, bm, mc, bl, lc));
    VkBufferImageCopy region{};
    region.bufferOffset     = s.offset;
    region.imageSubresource = { sampled_aspect(t->desc.format), mip, is3d ? 0u : layer, 1 };
    region.imageExtent      = { w, h, d };
    vkCmdCopyBufferToImage(cmd, s.buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image_barrier(cmd, make_image_barrier(t->image, t->aspect, ResourceState::TransferDst, ResourceState::ShaderRead, bm,
                                          mc, bl, lc));
    t->initialized = true;
}

// ---------------------------------------------------------------------------------------
// frame lifecycle
// ---------------------------------------------------------------------------------------

FrameInfo VulkanDevice::begin_frame() {
    AE_ASSERT_MSG(!frame_open_, "begin_frame called twice without end_frame");
    FrameInfo  info;
    FrameSlot& slot = current_slot();
    prepare_slot(slot); // waits for this slot's previous GPU work

    if (window_->minimized()) {
        return info; // valid=false: skip rendering, nothing acquired
    }
    if ((swapchain_dirty_ || !swapchain_) && !recreate_swapchain()) {
        return info;
    }

    u32      image = 0;
    VkResult r     = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, slot.acquire, VK_NULL_HANDLE, &image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_dirty_ = true; // nothing acquired, semaphore untouched
        return info;
    }
    if (r == VK_SUBOPTIMAL_KHR) {
        swapchain_dirty_ = true; // image IS acquired: render + present it, recreate next frame
    } else if (r != VK_SUCCESS) {
        AE_LOG_ERROR("RHI", "vkAcquireNextImageKHR failed: {}", result_string(r));
        if (r == VK_ERROR_DEVICE_LOST) {
            vk_check_failed(r, "vkAcquireNextImageKHR", __FILE__, __LINE__);
        }
        swapchain_dirty_ = true;
        return info;
    }

    begin_one_time(slot.cmd);
    if (slot.timestamps) {
        vkCmdResetQueryPool(slot.cmd, slot.timestamps, 0, 2);
        vkCmdWriteTimestamp2(slot.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, slot.timestamps, 0);
    }
    slot.list->begin(slot.cmd);
    frame_open_    = true;
    current_image_ = image;

    info.cmd              = slot.list.get();
    info.swapchain_image  = swapchain_textures_[image];
    info.swapchain_format = Format::BGRA8Unorm;
    info.extent           = UVec2(swapchain_extent_.width, swapchain_extent_.height);
    info.frame_index      = frame_index_;
    info.frame_slot       = static_cast<u32>(frame_index_ % frames_.size());
    info.image_index      = image;
    info.valid            = true;
    return info;
}

void VulkanDevice::end_frame(const FrameInfo& info) {
    if (!info.valid || !frame_open_) {
        return; // skipped frame (minimized / out of date): nothing was begun
    }
    FrameSlot&      slot = current_slot();
    VkCommandBuffer cmd  = slot.cmd;
    if (slot.list->in_rendering()) {
        AE_LOG_ERROR("RHI", "end_frame: rendering scope still open; closing it");
        slot.list->end_rendering();
    }

    // Swapchain state contract: ColorAttachment -> Present.
    image_barrier(cmd, make_image_barrier(swapchain_images_[current_image_], VK_IMAGE_ASPECT_COLOR_BIT,
                                          ResourceState::ColorAttachment, ResourceState::Present));
    make_host_visible(cmd);
    if (slot.timestamps) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, slot.timestamps, 1);
    }
    VK_CHECK(vkEndCommandBuffer(cmd));
    slot.recorded_stats = slot.list->counters();

    VkCommandBufferSubmitInfo cmds[2]{};
    u32                       cmd_count = 0;
    if (VkCommandBuffer upload = close_upload_cmd(slot)) {
        cmds[cmd_count++] = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, upload, 0 };
    }
    cmds[cmd_count++] = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, cmd, 0 };

    const u64             value = timeline_value_.load(std::memory_order_relaxed) + 1;
    VkSemaphoreSubmitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    wait.semaphore = slot.acquire;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signals[2]{};
    signals[0]           = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signals[0].semaphore = render_finished_[current_image_];
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1]           = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signals[1].semaphore = timeline_;
    signals[1].value     = value;
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submit.waitSemaphoreInfoCount   = 1;
    submit.pWaitSemaphoreInfos      = &wait;
    submit.commandBufferInfoCount   = cmd_count;
    submit.pCommandBufferInfos      = cmds;
    submit.signalSemaphoreInfoCount = 2;
    submit.pSignalSemaphoreInfos    = signals;
    VK_CHECK(vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE));

    timeline_value_.store(value, std::memory_order_release);
    slot.submitted_value    = value;
    slot.prepared           = false;
    slot.timestamps_written = slot.timestamps != VK_NULL_HANDLE;
    frame_open_             = false;

    VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = &render_finished_[current_image_];
    present.swapchainCount     = 1;
    present.pSwapchains        = &swapchain_;
    present.pImageIndices      = &current_image_;
    const VkResult r           = vkQueuePresentKHR(queue_, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        swapchain_dirty_ = true;
    } else if (r != VK_SUCCESS) {
        AE_LOG_ERROR("RHI", "vkQueuePresentKHR failed: {}", result_string(r));
        if (r == VK_ERROR_DEVICE_LOST) {
            vk_check_failed(r, "vkQueuePresentKHR", __FILE__, __LINE__);
        }
        swapchain_dirty_ = true;
    }
    ++frame_index_;

    if (post_present_hook_) {
        post_present_hook_(); // ImGui platform windows (secondary viewports)
    }
}

void VulkanDevice::on_resize(u32 /*width*/, u32 /*height*/) {
    swapchain_dirty_ = true; // recreated lazily by the next begin_frame (safe from callbacks)
}

void VulkanDevice::wait_idle() {
    VK_CHECK(vkDeviceWaitIdle(device_));
    process_deferred(completed_timeline_value(), false);
}

void VulkanDevice::immediate_submit(const std::function<void(CommandList&)>& record) {
    FrameSlot& slot = current_slot();
    prepare_slot(slot);
    VkCommandBuffer upload = close_upload_cmd(slot); // pending uploads must precede this work

    VK_CHECK(vkResetCommandBuffer(immediate_cmd_, 0));
    begin_one_time(immediate_cmd_);
    immediate_list_->begin(immediate_cmd_);
    record(*immediate_list_);
    if (immediate_list_->in_rendering()) {
        AE_LOG_ERROR("RHI", "immediate_submit: rendering scope left open; closing it");
        immediate_list_->end_rendering();
    }
    make_host_visible(immediate_cmd_);
    VK_CHECK(vkEndCommandBuffer(immediate_cmd_));

    VkCommandBufferSubmitInfo cmds[2]{};
    u32                       count = 0;
    if (upload) {
        cmds[count++] = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, upload, 0 };
    }
    cmds[count++] = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, immediate_cmd_, 0 };
    VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submit.commandBufferInfoCount = count;
    submit.pCommandBufferInfos    = cmds;
    VK_CHECK(vkQueueSubmit2(queue_, 1, &submit, immediate_fence_));
    VK_CHECK(vkWaitForFences(device_, 1, &immediate_fence_, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(device_, 1, &immediate_fence_));
}

} // namespace aether::rhi::vk
