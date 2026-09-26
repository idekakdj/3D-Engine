// vk_convert.h — RHI enum -> Vulkan conversions and the ResourceState barrier table.
#pragma once

#include "aether/rhi/enums.h"
#include "aether/rhi/resources.h"
#include "vk_common.h"

namespace aether::rhi::vk {

VkFormat              to_vk(Format f);
Format                from_vk(VkFormat f); // Undefined when not representable
VkImageUsageFlags     to_vk(TextureUsage u);
VkBufferUsageFlags    to_vk(BufferUsage u);
VkShaderStageFlagBits to_vk_stage(ShaderStage s); // single stage only
VkPrimitiveTopology   to_vk(PrimitiveTopology t);
VkCullModeFlags       to_vk(CullMode c);
VkFrontFace           to_vk(FrontFace f);
VkPolygonMode         to_vk(PolygonMode p);
VkCompareOp           to_vk(CompareOp c);
VkFilter              to_vk(Filter f);
VkSamplerMipmapMode   to_vk(MipmapMode m);
VkSamplerAddressMode  to_vk(AddressMode a);
VkBlendFactor         to_vk(BlendFactor b);
VkBlendOp             to_vk(BlendOp o);
VkAttachmentLoadOp    to_vk(LoadOp o);
VkAttachmentStoreOp   to_vk(StoreOp o);
VkSampleCountFlagBits to_vk_samples(u32 samples);

bool               is_depth_format(Format f);
bool               has_stencil(Format f);
bool               is_srgb(Format f);
bool               is_block_compressed(Format f);
Format             srgb_to_unorm(Format f); // identity for non-sRGB formats
VkImageAspectFlags full_aspect(Format f);   // depth|stencil for DS formats
VkImageAspectFlags sampled_aspect(Format f); // depth only for DS formats
// Bytes per texel, or per 4x4 block for BC formats.
u32 format_block_bytes(Format f);
u32 format_block_dim(Format f); // 1, or 4 for BC formats
// Tightly packed byte size of one subresource of the given extent.
u64 subresource_bytes(Format f, u32 width, u32 height, u32 depth);

// synchronization2 view of a ResourceState. `layout` is meaningless for buffers.
struct StateInfo {
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        access = VK_ACCESS_2_NONE;
    VkImageLayout         layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
// `as_source`: the state being left (only its WRITE accesses need to be made available).
StateInfo texture_state(ResourceState s, bool as_source);
StateInfo buffer_state(ResourceState s, bool as_source);
bool      is_write_state(ResourceState s);

VkImageMemoryBarrier2 make_image_barrier(VkImage image, VkImageAspectFlags aspect, ResourceState from,
                                         ResourceState to, u32 base_mip = 0,
                                         u32 mip_count = VK_REMAINING_MIP_LEVELS, u32 base_layer = 0,
                                         u32 layer_count = VK_REMAINING_ARRAY_LAYERS);

const char* present_mode_name(VkPresentModeKHR mode);

} // namespace aether::rhi::vk
