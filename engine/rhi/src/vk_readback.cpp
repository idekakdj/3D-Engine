// vk_readback.cpp — rhi::read_texture_rgba8 (device_ext.h): texture -> host buffer copy.
#include "aether/rhi/device_ext.h"

#include "vk_command_list.h"
#include "vk_convert.h"
#include "vk_device.h"

#include <cstring>
#include <format>

namespace aether::rhi {

Result<TextureReadback> read_texture_rgba8(Device& device, TextureHandle handle, ResourceState state) {
    vk::VulkanDevice&        dev = vk::as_vulkan(device);
    const vk::TextureRecord* tex = dev.texture(handle);
    if (tex == nullptr) {
        return Error{ ErrorCode::InvalidArgument, "read_texture_rgba8: invalid texture" };
    }
    const TextureDesc& d = tex->desc;
    const bool bgra = d.format == Format::BGRA8Unorm || d.format == Format::BGRA8Srgb;
    if (!bgra && d.format != Format::RGBA8Unorm && d.format != Format::RGBA8Srgb) {
        return Error{ ErrorCode::Unsupported, "read_texture_rgba8: only RGBA8/BGRA8 textures can be read back" };
    }
    if (!any(d.usage & TextureUsage::TransferSrc)) {
        return Error{ ErrorCode::InvalidArgument, "read_texture_rgba8: texture lacks TextureUsage::TransferSrc" };
    }

    const u64    size = u64(d.width) * d.height * 4;
    BufferDesc   bd;
    bd.size       = size;
    bd.usage      = BufferUsage::TransferDst;
    bd.memory     = MemoryUsage::GpuToCpu;
    bd.debug_name = "readback";
    BufferHandle staging = dev.create_buffer(bd);
    const vk::BufferRecord* buf = dev.buffer(staging);
    if (buf == nullptr) {
        return Error{ ErrorCode::OutOfMemory, "read_texture_rgba8: cannot allocate the readback buffer" };
    }
    const VkImage  image  = tex->image;
    const VkBuffer target = buf->buffer;
    dev.immediate_submit([&](CommandList& list) {
        VkCommandBuffer cmd = static_cast<vk::VulkanCommandList&>(list).vk();
        auto transition = [&](ResourceState from, ResourceState to) {
            VkImageMemoryBarrier2 b = vk::make_image_barrier(image, VK_IMAGE_ASPECT_COLOR_BIT, from, to, 0, 1, 0, 1);
            VkDependencyInfo      dep{};
            dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers    = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };
        if (state != ResourceState::TransferSrc) transition(state, ResourceState::TransferSrc);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent      = { d.width, d.height, 1 };
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, 1, &region);
        if (state != ResourceState::TransferSrc && state != ResourceState::Undefined) {
            transition(ResourceState::TransferSrc, state);
        }
    });

    TextureReadback out;
    out.width  = d.width;
    out.height = d.height;
    out.rgba8.resize(size);
    dev.invalidate_host(staging);
    const auto* src = static_cast<const u8*>(dev.map(staging));
    if (src == nullptr) {
        dev.destroy(staging);
        return Error{ ErrorCode::Internal, "read_texture_rgba8: cannot map the readback buffer" };
    }
    std::memcpy(out.rgba8.data(), src, size);
    dev.destroy(staging);
    if (bgra) {
        for (u64 i = 0; i < size; i += 4) std::swap(out.rgba8[i], out.rgba8[i + 2]);
    }
    return out;
}

} // namespace aether::rhi
