// vk_convert.cpp — RHI <-> Vulkan enum mapping and the sync2 ResourceState table.
#include "vk_convert.h"


namespace aether::rhi::vk {

VkFormat to_vk(Format f) {
    switch (f) {
    case Format::Undefined: return VK_FORMAT_UNDEFINED;
    case Format::R8Unorm: return VK_FORMAT_R8_UNORM;
    case Format::RG8Unorm: return VK_FORMAT_R8G8_UNORM;
    case Format::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::BGRA8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::R16F: return VK_FORMAT_R16_SFLOAT;
    case Format::RG16F: return VK_FORMAT_R16G16_SFLOAT;
    case Format::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::R32F: return VK_FORMAT_R32_SFLOAT;
    case Format::RG32F: return VK_FORMAT_R32G32_SFLOAT;
    case Format::RGB32F: return VK_FORMAT_R32G32B32_SFLOAT;
    case Format::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::R32Uint: return VK_FORMAT_R32_UINT;
    case Format::RG32Uint: return VK_FORMAT_R32G32_UINT;
    case Format::RGBA32Uint: return VK_FORMAT_R32G32B32A32_UINT;
    case Format::D32F: return VK_FORMAT_D32_SFLOAT;
    case Format::D24UnormS8: return VK_FORMAT_D24_UNORM_S8_UINT;
    case Format::D32FS8: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case Format::BC1Srgb: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case Format::BC3Srgb: return VK_FORMAT_BC3_SRGB_BLOCK;
    case Format::BC5Unorm: return VK_FORMAT_BC5_UNORM_BLOCK;
    case Format::BC7Srgb: return VK_FORMAT_BC7_SRGB_BLOCK;
    case Format::BC7Unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
    }
    return VK_FORMAT_UNDEFINED;
}

Format from_vk(VkFormat f) {
    for (u16 i = 0; i <= static_cast<u16>(Format::BC7Unorm); ++i) {
        if (to_vk(static_cast<Format>(i)) == f) {
            return static_cast<Format>(i);
        }
    }
    return Format::Undefined;
}

VkImageUsageFlags to_vk(TextureUsage u) {
    VkImageUsageFlags r = 0;
    if (any(u & TextureUsage::Sampled)) r |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (any(u & TextureUsage::Storage)) r |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (any(u & TextureUsage::ColorAttach)) r |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (any(u & TextureUsage::DepthAttach)) r |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (any(u & TextureUsage::TransferSrc)) r |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (any(u & TextureUsage::TransferDst)) r |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return r;
}

VkBufferUsageFlags to_vk(BufferUsage u) {
    VkBufferUsageFlags r = 0;
    if (any(u & BufferUsage::Vertex)) r |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (any(u & BufferUsage::Index)) r |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (any(u & BufferUsage::Uniform)) r |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (any(u & BufferUsage::Storage)) r |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (any(u & BufferUsage::Indirect)) r |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (any(u & BufferUsage::TransferSrc)) r |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (any(u & BufferUsage::TransferDst)) r |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return r;
}

VkShaderStageFlagBits to_vk_stage(ShaderStage s) {
    switch (s) {
    case ShaderStage::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderStage::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
    case ShaderStage::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case ShaderStage::Task: return VK_SHADER_STAGE_TASK_BIT_EXT;
    case ShaderStage::Mesh: return VK_SHADER_STAGE_MESH_BIT_EXT;
    default: return VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
    }
}

VkPrimitiveTopology to_vk(PrimitiveTopology t) {
    switch (t) {
    case PrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveTopology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PrimitiveTopology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkCullModeFlags to_vk(CullMode c) {
    switch (c) {
    case CullMode::None: return VK_CULL_MODE_NONE;
    case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    case CullMode::Back: return VK_CULL_MODE_BACK_BIT;
    }
    return VK_CULL_MODE_NONE;
}

VkFrontFace to_vk(FrontFace f) {
    return f == FrontFace::Clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

VkPolygonMode to_vk(PolygonMode p) {
    switch (p) {
    case PolygonMode::Fill: return VK_POLYGON_MODE_FILL;
    case PolygonMode::Line: return VK_POLYGON_MODE_LINE;
    case PolygonMode::Point: return VK_POLYGON_MODE_POINT;
    }
    return VK_POLYGON_MODE_FILL;
}

VkCompareOp to_vk(CompareOp c) {
    switch (c) {
    case CompareOp::Never: return VK_COMPARE_OP_NEVER;
    case CompareOp::Less: return VK_COMPARE_OP_LESS;
    case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
    case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
    case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkFilter to_vk(Filter f) {
    return f == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerMipmapMode to_vk(MipmapMode m) {
    return m == MipmapMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

VkSamplerAddressMode to_vk(AddressMode a) {
    switch (a) {
    case AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case AddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case AddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkBlendFactor to_vk(BlendFactor b) {
    switch (b) {
    case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp to_vk(BlendOp o) {
    switch (o) {
    case BlendOp::Add: return VK_BLEND_OP_ADD;
    case BlendOp::Subtract: return VK_BLEND_OP_SUBTRACT;
    case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::Min: return VK_BLEND_OP_MIN;
    case BlendOp::Max: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

VkAttachmentLoadOp to_vk(LoadOp o) {
    switch (o) {
    case LoadOp::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

VkAttachmentStoreOp to_vk(StoreOp o) {
    return o == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

VkSampleCountFlagBits to_vk_samples(u32 samples) {
    switch (samples) {
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    case 16: return VK_SAMPLE_COUNT_16_BIT;
    default: return VK_SAMPLE_COUNT_1_BIT;
    }
}

bool is_depth_format(Format f) {
    return f == Format::D32F || f == Format::D24UnormS8 || f == Format::D32FS8;
}

bool has_stencil(Format f) {
    return f == Format::D24UnormS8 || f == Format::D32FS8;
}

bool is_srgb(Format f) {
    return f == Format::RGBA8Srgb || f == Format::BGRA8Srgb || f == Format::BC1Srgb ||
           f == Format::BC3Srgb || f == Format::BC7Srgb;
}

bool is_block_compressed(Format f) {
    return f == Format::BC1Srgb || f == Format::BC3Srgb || f == Format::BC5Unorm || f == Format::BC7Srgb ||
           f == Format::BC7Unorm;
}

Format srgb_to_unorm(Format f) {
    switch (f) {
    case Format::RGBA8Srgb: return Format::RGBA8Unorm;
    case Format::BGRA8Srgb: return Format::BGRA8Unorm;
    default: return f;
    }
}

VkImageAspectFlags full_aspect(Format f) {
    if (!is_depth_format(f)) {
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
    return has_stencil(f) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
}

VkImageAspectFlags sampled_aspect(Format f) {
    return is_depth_format(f) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

u32 format_block_bytes(Format f) {
    switch (f) {
    case Format::Undefined: return 0;
    case Format::R8Unorm: return 1;
    case Format::RG8Unorm: return 2;
    case Format::RGBA8Unorm:
    case Format::RGBA8Srgb:
    case Format::BGRA8Unorm:
    case Format::BGRA8Srgb: return 4;
    case Format::R16F: return 2;
    case Format::RG16F: return 4;
    case Format::RGBA16F: return 8;
    case Format::R32F: return 4;
    case Format::RG32F: return 8;
    case Format::RGB32F: return 12;
    case Format::RGBA32F: return 16;
    case Format::R32Uint: return 4;
    case Format::RG32Uint: return 8;
    case Format::RGBA32Uint: return 16;
    case Format::D32F: return 4;
    case Format::D24UnormS8: return 4;
    case Format::D32FS8: return 8;
    case Format::BC1Srgb: return 8;
    case Format::BC3Srgb:
    case Format::BC5Unorm:
    case Format::BC7Srgb:
    case Format::BC7Unorm: return 16;
    }
    return 0;
}

u32 format_block_dim(Format f) {
    return is_block_compressed(f) ? 4u : 1u;
}

u32 vertex_attribute_binding(const GraphicsPipelineDesc& desc, const VertexAttribute& a, bool* declared) {
    for (const VertexBinding& b : desc.vertex_bindings) {
        if (b.binding == a.binding) {
            if (declared) {
                *declared = true;
            }
            return a.binding;
        }
    }
    if (declared) {
        *declared = false;
    }
    return desc.vertex_bindings.empty() ? a.binding : desc.vertex_bindings[0].binding;
}

u64 subresource_bytes(Format f, u32 width, u32 height, u32 depth) {
    const u32 dim = format_block_dim(f);
    const u64 bw  = (static_cast<u64>(width) + dim - 1) / dim;
    const u64 bh  = (static_cast<u64>(height) + dim - 1) / dim;
    return bw * bh * static_cast<u64>(depth) * format_block_bytes(f);
}

// ---- ResourceState table -------------------------------------------------------------
//
// Layouts use the synchronization2 generic layouts (ATTACHMENT_OPTIMAL /
// READ_ONLY_OPTIMAL) so colour and depth images share one mapping, and bindless
// descriptors (written with READ_ONLY_OPTIMAL) match sampled depth textures too.
// "Undefined" as a source waits on ALL_COMMANDS: it orders the discard/layout transition
// after every earlier use (no WAR hazards on transient reuse) and chains with the
// swapchain-acquire semaphore wait regardless of which stage first touches the image.

namespace {

constexpr VkPipelineStageFlags2 kShaderStages = VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT |
                                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 kDepthStages =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

constexpr VkAccessFlags2 kWriteAccess =
    VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

StateInfo make_source(StateInfo s, bool as_source) {
    if (as_source) {
        s.access &= kWriteAccess; // only writes need an availability operation
    }
    return s;
}

} // namespace

bool is_write_state(ResourceState s) {
    switch (s) {
    case ResourceState::General:
    case ResourceState::ColorAttachment:
    case ResourceState::DepthStencilAttachment:
    case ResourceState::ShaderWrite:
    case ResourceState::TransferDst: return true;
    default: return false;
    }
}

StateInfo texture_state(ResourceState s, bool as_source) {
    StateInfo r;
    switch (s) {
    case ResourceState::Undefined:
        r = { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED };
        break;
    case ResourceState::General:
    case ResourceState::IndirectArgument: // not meaningful for images: treat as General
        r = { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
              VK_IMAGE_LAYOUT_GENERAL };
        break;
    case ResourceState::ColorAttachment:
        r = { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL };
        break;
    case ResourceState::DepthStencilAttachment:
        r = { kDepthStages,
              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL };
        break;
    case ResourceState::DepthStencilRead:
        r = { kDepthStages | kShaderStages,
              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
              VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL };
        break;
    case ResourceState::ShaderRead:
        r = { kShaderStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
              VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL };
        break;
    case ResourceState::ShaderWrite:
        r = { kShaderStages, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
              VK_IMAGE_LAYOUT_GENERAL };
        break;
    case ResourceState::TransferSrc:
        r = { VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
        break;
    case ResourceState::TransferDst:
        r = { VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL };
        break;
    case ResourceState::Present:
        // Leaving Present (re-acquired image): wait for everything; entering it: the
        // semaphore signal after the barrier orders presentation.
        r = { as_source ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
        break;
    }
    return make_source(r, as_source);
}

StateInfo buffer_state(ResourceState s, bool as_source) {
    StateInfo r;
    switch (s) {
    case ResourceState::Undefined: r = { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE }; break;
    case ResourceState::ShaderRead:
        r = { kShaderStages | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT |
                  VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT };
        break;
    case ResourceState::ShaderWrite:
        r = { kShaderStages, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
        break;
    case ResourceState::TransferSrc:
        r = { VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT };
        break;
    case ResourceState::TransferDst:
        r = { VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT };
        break;
    case ResourceState::IndirectArgument:
        r = { VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT };
        break;
    default: // General and image-only states: conservative full barrier
        r = { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT };
        break;
    }
    return make_source(r, as_source);
}

VkImageMemoryBarrier2 make_image_barrier(VkImage image, VkImageAspectFlags aspect, ResourceState from,
                                         ResourceState to, u32 base_mip, u32 mip_count, u32 base_layer,
                                         u32 layer_count) {
    const StateInfo src = texture_state(from, true);
    const StateInfo dst = texture_state(to, false);
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask        = src.stages;
    b.srcAccessMask       = src.access;
    b.dstStageMask        = dst.stages;
    b.dstAccessMask       = dst.access;
    b.oldLayout           = src.layout;
    b.newLayout           = dst.layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange    = { aspect, base_mip, mip_count, base_layer, layer_count };
    return b;
}

const char* present_mode_name(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "OTHER";
    }
}

const char* result_string(VkResult result) {
    switch (result) {
#define AE_VK_RESULT(r) case r: return #r;
        AE_VK_RESULT(VK_SUCCESS)
        AE_VK_RESULT(VK_NOT_READY)
        AE_VK_RESULT(VK_TIMEOUT)
        AE_VK_RESULT(VK_EVENT_SET)
        AE_VK_RESULT(VK_EVENT_RESET)
        AE_VK_RESULT(VK_INCOMPLETE)
        AE_VK_RESULT(VK_ERROR_OUT_OF_HOST_MEMORY)
        AE_VK_RESULT(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        AE_VK_RESULT(VK_ERROR_INITIALIZATION_FAILED)
        AE_VK_RESULT(VK_ERROR_DEVICE_LOST)
        AE_VK_RESULT(VK_ERROR_MEMORY_MAP_FAILED)
        AE_VK_RESULT(VK_ERROR_LAYER_NOT_PRESENT)
        AE_VK_RESULT(VK_ERROR_EXTENSION_NOT_PRESENT)
        AE_VK_RESULT(VK_ERROR_FEATURE_NOT_PRESENT)
        AE_VK_RESULT(VK_ERROR_INCOMPATIBLE_DRIVER)
        AE_VK_RESULT(VK_ERROR_TOO_MANY_OBJECTS)
        AE_VK_RESULT(VK_ERROR_FORMAT_NOT_SUPPORTED)
        AE_VK_RESULT(VK_ERROR_FRAGMENTED_POOL)
        AE_VK_RESULT(VK_ERROR_UNKNOWN)
        AE_VK_RESULT(VK_ERROR_OUT_OF_POOL_MEMORY)
        AE_VK_RESULT(VK_ERROR_INVALID_EXTERNAL_HANDLE)
        AE_VK_RESULT(VK_ERROR_FRAGMENTATION)
        AE_VK_RESULT(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS)
        AE_VK_RESULT(VK_PIPELINE_COMPILE_REQUIRED)
        AE_VK_RESULT(VK_ERROR_SURFACE_LOST_KHR)
        AE_VK_RESULT(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
        AE_VK_RESULT(VK_SUBOPTIMAL_KHR)
        AE_VK_RESULT(VK_ERROR_OUT_OF_DATE_KHR)
        AE_VK_RESULT(VK_ERROR_VALIDATION_FAILED_EXT)
#undef AE_VK_RESULT
    default: return "VK_RESULT_<unknown>";
    }
}

[[noreturn]] void vk_check_failed(VkResult result, const char* expr, const char* file, int line) {
    AE_LOG_FATAL("Vulkan", "{} failed with {} ({}:{})", expr, result_string(result), file, line);
    ::aether::detail::assert_fail(expr, file, line, result_string(result));
}

} // namespace aether::rhi::vk
