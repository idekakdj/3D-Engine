// vk_resources.cpp — buffers, textures (+ view caches), samplers, shaders, pipelines.
#include "vk_convert.h"
#include "vk_device.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>

namespace aether::rhi::vk {
namespace {

constexpr u32 kSpirvMagic = 0x07230203u;

VkImageViewType view_type(const TextureDesc& d) {
    switch (d.type) {
    case TextureType::Tex1D: return d.array_layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case TextureType::Tex2D: return d.array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    case TextureType::Tex3D: return VK_IMAGE_VIEW_TYPE_3D;
    case TextureType::Cube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TextureType::Tex2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TextureType::CubeArray: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}

VkImageViewType storage_view_type(const TextureDesc& d) {
    switch (d.type) {
    case TextureType::Tex1D: return d.array_layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case TextureType::Tex2D: return d.array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    case TextureType::Tex3D: return VK_IMAGE_VIEW_TYPE_3D;
    default: return VK_IMAGE_VIEW_TYPE_2D_ARRAY; // cubes/arrays are written as 2D arrays
    }
}

} // namespace

// ---------------------------------------------------------------------------------------
// buffers
// ---------------------------------------------------------------------------------------

BufferHandle VulkanDevice::create_buffer(const BufferDesc& desc) {
    if (desc.size == 0) {
        AE_LOG_ERROR("RHI", "create_buffer '{}': size must be > 0", desc.debug_name);
        return {};
    }
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size  = desc.size;
    bi.usage = to_vk(desc.usage) | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo ai{};
    switch (desc.memory) {
    case MemoryUsage::GpuOnly: ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE; break;
    case MemoryUsage::CpuToGpu:
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemoryUsage::GpuToCpu:
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemoryUsage::CpuOnly:
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    }

    BufferRecord      rec;
    VmaAllocationInfo info{};
    const VkResult    r = vmaCreateBuffer(allocator_, &bi, &ai, &rec.buffer, &rec.allocation, &info);
    if (r != VK_SUCCESS) {
        AE_LOG_ERROR("RHI", "create_buffer '{}' ({} bytes) failed: {}", desc.debug_name, desc.size, result_string(r));
        return {};
    }
    rec.mapped = info.pMappedData;
    VkBufferDeviceAddressInfo addr{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    addr.buffer = rec.buffer;
    rec.address = vkGetBufferDeviceAddress(device_, &addr);
    rec.desc    = desc;
    set_object_name(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(rec.buffer), desc.debug_name);
    if (!desc.debug_name.empty()) {
        vmaSetAllocationName(allocator_, rec.allocation, desc.debug_name.c_str());
    }
    const BufferHandle h = buffers_.allocate(std::move(rec));
    if (!h) {
        AE_LOG_ERROR("RHI", "create_buffer: buffer pool exhausted");
    }
    return h;
}

void VulkanDevice::destroy(BufferHandle h) {
    BufferRecord rec;
    if (!buffers_.release(h, rec)) {
        return; // stale / invalid handles are ignored (idempotent destroy)
    }
    defer([this, buffer = rec.buffer, allocation = rec.allocation] { vmaDestroyBuffer(allocator_, buffer, allocation); });
}

const BufferDesc* VulkanDevice::buffer_desc(BufferHandle h) const {
    const BufferRecord* b = buffers_.get(h);
    return b ? &b->desc : nullptr;
}

u64 VulkanDevice::buffer_device_address(BufferHandle h) {
    const BufferRecord* b = buffers_.get(h);
    return b ? b->address : 0;
}

// ---------------------------------------------------------------------------------------
// textures
// ---------------------------------------------------------------------------------------

TextureHandle VulkanDevice::create_texture(const TextureDesc& in) {
    TextureDesc desc = in;
    if (desc.width == 0 || desc.height == 0 || desc.depth == 0 || desc.format == Format::Undefined) {
        AE_LOG_ERROR("RHI", "create_texture '{}': invalid extent or format", desc.debug_name);
        return {};
    }
    const bool is3d = desc.type == TextureType::Tex3D;
    const bool cube = desc.type == TextureType::Cube || desc.type == TextureType::CubeArray;
    if (cube && (desc.array_layers < 6 || desc.array_layers % 6 != 0)) {
        desc.array_layers = std::max(6u, (desc.array_layers + 5) / 6 * 6);
    }
    if (is3d) {
        desc.array_layers = 1;
    } else {
        desc.depth = 1;
    }
    const u32 max_dim  = std::max({ desc.width, desc.height, desc.depth });
    const u32 max_mips = static_cast<u32>(std::bit_width(max_dim));
    desc.mip_levels    = std::clamp(desc.mip_levels, 1u, max_mips);
    desc.samples       = std::max(desc.samples, 1u);

    VkImageUsageFlags usage = to_vk(desc.usage);
    if (any(desc.usage & TextureUsage::Sampled) || desc.mip_levels > 1) {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // uploads + mip gen
    }
    if (usage == 0) {
        AE_LOG_ERROR("RHI", "create_texture '{}': no usage flags", desc.debug_name);
        return {};
    }

    const VkFormat format = to_vk(desc.format);
    VkImageCreateFlags flags = cube ? static_cast<VkImageCreateFlags>(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) : 0u;
    // sRGB formats cannot be storage images: create MUTABLE and write through a UNORM view.
    const bool     srgb_storage = any(desc.usage & TextureUsage::Storage) && is_srgb(desc.format);
    const VkFormat view_formats[2] = { format, to_vk(srgb_to_unorm(desc.format)) };
    VkImageFormatListCreateInfo format_list{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
    format_list.viewFormatCount = 2;
    format_list.pViewFormats    = view_formats;
    if (srgb_storage) {
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    }

    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.pNext         = srgb_storage ? &format_list : nullptr;
    ii.flags         = flags;
    ii.imageType     = desc.type == TextureType::Tex1D ? VK_IMAGE_TYPE_1D : is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ii.format        = format;
    ii.extent        = { desc.width, desc.type == TextureType::Tex1D ? 1u : desc.height, desc.depth };
    ii.mipLevels     = desc.mip_levels;
    ii.arrayLayers   = desc.array_layers;
    ii.samples       = to_vk_samples(desc.samples);
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.usage         = usage;
    ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageFormatProperties fp{};
    if (vkGetPhysicalDeviceImageFormatProperties(gpu_, format, ii.imageType, ii.tiling, usage, flags, &fp) != VK_SUCCESS) {
        AE_LOG_ERROR("RHI", "create_texture '{}': VkFormat {} unsupported for this usage/type", desc.debug_name,
                     static_cast<int>(format));
        return {};
    }

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (any(desc.usage & (TextureUsage::ColorAttach | TextureUsage::DepthAttach))) {
        ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT; // render targets: own allocation
    }

    TextureRecord rec;
    const VkResult r = vmaCreateImage(allocator_, &ii, &ai, &rec.image, &rec.allocation, nullptr);
    if (r != VK_SUCCESS) {
        AE_LOG_ERROR("RHI", "create_texture '{}' failed: {}", desc.debug_name, result_string(r));
        return {};
    }
    rec.format = format;
    rec.aspect = full_aspect(desc.format);
    rec.desc   = desc;
    if (!create_texture_views(rec)) {
        destroy_texture_record(rec);
        return {};
    }
    set_object_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(rec.image), desc.debug_name);
    set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<u64>(rec.view), desc.debug_name + " view");
    if (!desc.debug_name.empty()) {
        vmaSetAllocationName(allocator_, rec.allocation, desc.debug_name.c_str());
    }
    const TextureHandle h = textures_.allocate(std::move(rec));
    if (!h) {
        AE_LOG_ERROR("RHI", "create_texture: texture pool exhausted");
    }
    return h;
}

bool VulkanDevice::create_texture_views(TextureRecord& t) {
    const TextureDesc& d     = t.desc;
    const VkImageUsageFlags usage = to_vk(d.usage) |
                                    ((any(d.usage & TextureUsage::Sampled) || d.mip_levels > 1)
                                         ? (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                                         : 0u);
    const bool srgb_storage = any(d.usage & TextureUsage::Storage) && is_srgb(d.format);

    auto make_view = [&](VkImageViewType type, VkFormat format, VkImageAspectFlags aspect, u32 base_mip, u32 mips,
                         u32 base_layer, u32 layers, VkImageUsageFlags view_usage, VkImageView& out) {
        VkImageViewUsageCreateInfo usage_ci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
        usage_ci.usage = view_usage;
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.pNext            = srgb_storage ? &usage_ci : nullptr; // restrict usage per view format
        vi.image            = t.image;
        vi.viewType         = type;
        vi.format           = format;
        vi.subresourceRange = { aspect, base_mip, mips, base_layer, layers };
        const VkResult r    = vkCreateImageView(device_, &vi, nullptr, &out);
        if (r != VK_SUCCESS) {
            AE_LOG_ERROR("RHI", "texture '{}': vkCreateImageView failed: {}", d.debug_name, result_string(r));
            return false;
        }
        return true;
    };

    // Full view for sampling (depth aspect only for depth/stencil formats).
    if (!make_view(view_type(d), t.format, sampled_aspect(d.format), 0, d.mip_levels, 0, d.array_layers,
                   usage & ~VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT), t.view)) {
        return false;
    }

    // Per-(mip, layer) attachment views.
    if (any(d.usage & (TextureUsage::ColorAttach | TextureUsage::DepthAttach))) {
        const bool single = d.mip_levels == 1 && d.array_layers == 1 && d.type == TextureType::Tex2D;
        if (single && t.aspect == sampled_aspect(d.format)) {
            t.attachment_views = { t.view }; // alias (not destroyed twice)
        } else if (d.type == TextureType::Tex3D) {
            AE_LOG_WARN("RHI", "texture '{}': 3D render targets are not supported", d.debug_name);
        } else {
            const VkImageUsageFlags att_usage = usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
            t.attachment_views.assign(static_cast<usize>(d.mip_levels) * d.array_layers, VK_NULL_HANDLE);
            for (u32 mip = 0; mip < d.mip_levels; ++mip) {
                for (u32 layer = 0; layer < d.array_layers; ++layer) {
                    if (!make_view(VK_IMAGE_VIEW_TYPE_2D, t.format, t.aspect, mip, 1, layer, 1, att_usage,
                                   t.attachment_views[static_cast<usize>(mip) * d.array_layers + layer])) {
                        return false;
                    }
                }
            }
        }
    }

    // Per-mip storage views (sRGB images are written through their UNORM alias).
    if (any(d.usage & TextureUsage::Storage)) {
        t.storage_views.assign(d.mip_levels, VK_NULL_HANDLE);
        const VkFormat storage_format = to_vk(srgb_to_unorm(d.format));
        for (u32 mip = 0; mip < d.mip_levels; ++mip) {
            if (!make_view(storage_view_type(d), storage_format, VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, d.array_layers,
                           VK_IMAGE_USAGE_STORAGE_BIT, t.storage_views[mip])) {
                return false;
            }
        }
    }
    return true;
}

void VulkanDevice::destroy_texture_record(TextureRecord& t) {
    for (VkImageView v : t.attachment_views) {
        if (v && v != t.view) {
            vkDestroyImageView(device_, v, nullptr);
        }
    }
    for (VkImageView v : t.storage_views) {
        if (v) {
            vkDestroyImageView(device_, v, nullptr);
        }
    }
    if (t.view) {
        vkDestroyImageView(device_, t.view, nullptr);
    }
    if (t.owns_image && t.image) {
        vmaDestroyImage(allocator_, t.image, t.allocation);
    }
    t = TextureRecord{};
}

void VulkanDevice::destroy(TextureHandle h) {
    TextureRecord rec;
    if (!textures_.release(h, rec)) {
        return;
    }
    if (!rec.owns_image) {
        AE_LOG_ERROR("RHI", "destroy: swapchain images are owned by the device");
        return;
    }
    defer([this, rec]() mutable { destroy_texture_record(rec); });
}

const TextureDesc* VulkanDevice::texture_desc(TextureHandle h) const {
    const TextureRecord* t = textures_.get(h);
    return t ? &t->desc : nullptr;
}

VkImageView VulkanDevice::attachment_view(const TextureRecord& t, u32 mip, u32 layer) const {
    if (t.attachment_views.empty()) {
        AE_LOG_ERROR("RHI", "texture '{}' was not created with an attachment usage", t.desc.debug_name);
        return t.view;
    }
    const usize index = static_cast<usize>(mip) * t.desc.array_layers + layer;
    if (index >= t.attachment_views.size()) {
        AE_LOG_ERROR("RHI", "texture '{}': attachment mip {} layer {} out of range", t.desc.debug_name, mip, layer);
        return t.attachment_views[0];
    }
    return t.attachment_views[index];
}

// ---------------------------------------------------------------------------------------
// samplers / shaders
// ---------------------------------------------------------------------------------------

SamplerHandle VulkanDevice::create_sampler(const SamplerDesc& desc) {
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter    = to_vk(desc.mag_filter);
    si.minFilter    = to_vk(desc.min_filter);
    si.mipmapMode   = to_vk(desc.mipmap);
    si.addressModeU = to_vk(desc.address_u);
    si.addressModeV = to_vk(desc.address_v);
    si.addressModeW = to_vk(desc.address_w);
    si.anisotropyEnable = features_.sampler_anisotropy && desc.max_anisotropy > 1.0f;
    si.maxAnisotropy    = si.anisotropyEnable ? std::min(desc.max_anisotropy, props_.limits.maxSamplerAnisotropy) : 1.0f;
    si.compareEnable    = desc.compare_enable;
    si.compareOp        = to_vk(desc.compare_op);
    si.minLod           = desc.min_lod;
    si.maxLod           = desc.max_lod;
    si.borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    SamplerRecord  rec;
    const VkResult r = vkCreateSampler(device_, &si, nullptr, &rec.sampler);
    if (r != VK_SUCCESS) {
        AE_LOG_ERROR("RHI", "vkCreateSampler failed: {}", result_string(r));
        return {};
    }
    rec.desc = desc;
    return samplers_.allocate(std::move(rec));
}

void VulkanDevice::destroy(SamplerHandle h) {
    SamplerRecord rec;
    if (samplers_.release(h, rec)) {
        defer([this, s = rec.sampler] { vkDestroySampler(device_, s, nullptr); });
    }
}

ShaderHandle VulkanDevice::create_shader(const ShaderDesc& desc) {
    if (desc.spirv.empty() || desc.spirv[0] != kSpirvMagic) {
        AE_LOG_ERROR("RHI", "create_shader '{}': not SPIR-V", desc.debug_name);
        return {};
    }
    if (to_vk_stage(desc.stage) == VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM) {
        AE_LOG_ERROR("RHI", "create_shader '{}': stage must be a single stage", desc.debug_name);
        return {};
    }
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = desc.spirv.size() * sizeof(u32);
    ci.pCode    = desc.spirv.data();
    ShaderRecord   rec;
    const VkResult r = vkCreateShaderModule(device_, &ci, nullptr, &rec.module);
    if (r != VK_SUCCESS) {
        AE_LOG_ERROR("RHI", "create_shader '{}' failed: {}", desc.debug_name, result_string(r));
        return {};
    }
    rec.stage       = desc.stage;
    rec.entry_point = desc.entry_point.empty() ? "main" : desc.entry_point;
    rec.debug_name  = desc.debug_name;
    set_object_name(VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<u64>(rec.module), desc.debug_name);
    return shaders_.allocate(std::move(rec));
}

void VulkanDevice::destroy(ShaderHandle h) {
    ShaderRecord rec;
    if (shaders_.release(h, rec)) {
        defer([this, m = rec.module] { vkDestroyShaderModule(device_, m, nullptr); });
    }
}

// ---------------------------------------------------------------------------------------
// pipelines (dynamic rendering, universal layout)
// ---------------------------------------------------------------------------------------

PipelineHandle VulkanDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
    const ShaderRecord* vs = shaders_.get(desc.vertex);
    const ShaderRecord* fs = shaders_.get(desc.fragment);
    const ShaderRecord* task_sh = shaders_.get(desc.task);
    const ShaderRecord* mesh_sh = shaders_.get(desc.mesh);
    const bool          mesh_pipeline = desc.mesh.is_valid(); // ADR-0010
    if (mesh_pipeline) {
        if (!features_.mesh_shaders) {
            AE_LOG_ERROR("RHI", "pipeline '{}': mesh shaders are not supported by this device", desc.debug_name);
            return {};
        }
        if (!mesh_sh || mesh_sh->stage != ShaderStage::Mesh || desc.vertex.is_valid() ||
            (desc.task.is_valid() && (!task_sh || task_sh->stage != ShaderStage::Task))) {
            AE_LOG_ERROR("RHI", "pipeline '{}': invalid task/mesh shader combination", desc.debug_name);
            return {};
        }
    } else if (!vs || vs->stage != ShaderStage::Vertex) {
        AE_LOG_ERROR("RHI", "pipeline '{}': invalid vertex shader", desc.debug_name);
        return {};
    }
    if (desc.fragment && (!fs || fs->stage != ShaderStage::Fragment)) {
        AE_LOG_ERROR("RHI", "pipeline '{}': invalid fragment shader", desc.debug_name);
        return {};
    }
    if (desc.push_constant_size > 128) {
        AE_LOG_WARN("RHI", "pipeline '{}': push_constant_size {} > 128 (universal layout limit)", desc.debug_name,
                    desc.push_constant_size);
    }

    std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
    u32                                            stage_count = 0;
    auto add_stage = [&](VkShaderStageFlagBits bit, const ShaderRecord& r) {
        VkPipelineShaderStageCreateInfo& st = stages[stage_count++];
        st        = {};
        st.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.stage  = bit;
        st.module = r.module;
        st.pName  = r.entry_point.c_str();
    };
    if (mesh_pipeline) {
        if (task_sh) {
            add_stage(VK_SHADER_STAGE_TASK_BIT_EXT, *task_sh);
        }
        add_stage(VK_SHADER_STAGE_MESH_BIT_EXT, *mesh_sh);
    } else {
        add_stage(VK_SHADER_STAGE_VERTEX_BIT, *vs);
    }
    if (fs) {
        add_stage(VK_SHADER_STAGE_FRAGMENT_BIT, *fs);
    }

    std::vector<VkVertexInputBindingDescription> bindings;
    for (const VertexBinding& b : desc.vertex_bindings) {
        bindings.push_back({ b.binding, b.stride, b.per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX });
    }
    // VertexAttribute::binding (ADR-0009) names the source binding; an attribute whose
    // binding is not declared falls back to the first declared binding (pre-M2 behaviour).
    const u32 first_binding = desc.vertex_bindings.empty() ? 0u : desc.vertex_bindings[0].binding;
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const VertexAttribute& a : desc.vertex_attributes) {
        bool      declared = false;
        const u32 binding  = vertex_attribute_binding(desc, a, &declared);
        if (!declared && desc.vertex_bindings.size() > 1) {
            AE_LOG_WARN("RHI", "pipeline '{}': attribute {} names undeclared binding {}; using binding {}",
                        desc.debug_name, a.location, a.binding, first_binding);
        }
        attributes.push_back({ a.location, binding, to_vk(a.format), a.offset });
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertex_input.vertexBindingDescriptionCount   = static_cast<u32>(bindings.size());
    vertex_input.pVertexBindingDescriptions      = bindings.data();
    vertex_input.vertexAttributeDescriptionCount = static_cast<u32>(attributes.size());
    vertex_input.pVertexAttributeDescriptions    = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = to_vk(desc.topology);

    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;

    VkPolygonMode polygon = to_vk(desc.polygon);
    if (polygon != VK_POLYGON_MODE_FILL && !features_.fill_mode_non_solid) {
        AE_LOG_WARN("RHI", "pipeline '{}': fillModeNonSolid unsupported; using Fill", desc.debug_name);
        polygon = VK_POLYGON_MODE_FILL;
    }
    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.depthClampEnable = desc.depth.clamp_enable && features_.depth_clamp;
    raster.polygonMode      = polygon;
    raster.cullMode         = to_vk(desc.cull);
    raster.frontFace        = to_vk(desc.front_face);
    raster.depthBiasEnable  = desc.depth.bias_enable;
    raster.lineWidth        = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = to_vk_samples(desc.targets.samples);

    const bool has_depth = desc.targets.depth != Format::Undefined;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = has_depth && desc.depth.test;
    ds.depthWriteEnable = has_depth && desc.depth.write;
    ds.depthCompareOp   = to_vk(desc.depth.compare);

    std::vector<VkPipelineColorBlendAttachmentState> blends(desc.targets.color.size());
    std::vector<VkFormat>                            color_formats;
    for (usize i = 0; i < desc.targets.color.size(); ++i) {
        VkPipelineColorBlendAttachmentState& b = blends[i];
        b.blendEnable         = desc.blend.enable;
        b.srcColorBlendFactor = to_vk(desc.blend.src_color);
        b.dstColorBlendFactor = to_vk(desc.blend.dst_color);
        b.colorBlendOp        = to_vk(desc.blend.color_op);
        b.srcAlphaBlendFactor = to_vk(desc.blend.src_alpha);
        b.dstAlphaBlendFactor = to_vk(desc.blend.dst_alpha);
        b.alphaBlendOp        = to_vk(desc.blend.alpha_op);
        b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
        color_formats.push_back(to_vk(desc.targets.color[i]));
    }
    VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = static_cast<u32>(blends.size());
    blend.pAttachments    = blends.data();

    std::vector<VkDynamicState> dynamic{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    if (desc.depth.bias_enable) {
        dynamic.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
    }
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = static_cast<u32>(dynamic.size());
    dyn.pDynamicStates    = dynamic.data();

    VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rendering.colorAttachmentCount    = static_cast<u32>(color_formats.size());
    rendering.pColorAttachmentFormats = color_formats.data();
    rendering.depthAttachmentFormat   = has_depth ? to_vk(desc.targets.depth) : VK_FORMAT_UNDEFINED;
    rendering.stencilAttachmentFormat = has_depth && has_stencil(desc.targets.depth) ? to_vk(desc.targets.depth)
                                                                                    : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo ci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.pNext               = &rendering;
    ci.stageCount          = stage_count;
    ci.pStages             = stages.data();
    ci.pVertexInputState   = mesh_pipeline ? nullptr : &vertex_input; // mesh pipelines have no vertex input
    ci.pInputAssemblyState = mesh_pipeline ? nullptr : &ia;
    ci.pViewportState      = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState   = &ms;
    ci.pDepthStencilState  = &ds;
    ci.pColorBlendState    = &blend;
    ci.pDynamicState       = &dyn;
    ci.layout              = pipeline_layout_;

    PipelineRecord rec;
    rec.bind_point   = VK_PIPELINE_BIND_POINT_GRAPHICS;
    rec.debug_name   = desc.debug_name;
    const VkResult r = vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &ci, nullptr, &rec.pipeline);
    if (r != VK_SUCCESS) {
        AE_LOG_ERROR("RHI", "graphics pipeline '{}' creation failed: {}", desc.debug_name, result_string(r));
        return {};
    }
    set_object_name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(rec.pipeline), desc.debug_name);
    return pipelines_.allocate(std::move(rec));
}

PipelineHandle VulkanDevice::create_compute_pipeline(const ComputePipelineDesc& desc) {
    const ShaderRecord* cs = shaders_.get(desc.compute);
    if (!cs || cs->stage != ShaderStage::Compute) {
        AE_LOG_ERROR("RHI", "compute pipeline '{}': invalid compute shader", desc.debug_name);
        return {};
    }
    VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage  = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs->module,
                  cs->entry_point.c_str(), nullptr };
    ci.layout = pipeline_layout_;
    PipelineRecord rec;
    rec.bind_point   = VK_PIPELINE_BIND_POINT_COMPUTE;
    rec.debug_name   = desc.debug_name;
    const VkResult r = vkCreateComputePipelines(device_, pipeline_cache_, 1, &ci, nullptr, &rec.pipeline);
    if (r != VK_SUCCESS) {
        AE_LOG_ERROR("RHI", "compute pipeline '{}' creation failed: {}", desc.debug_name, result_string(r));
        return {};
    }
    set_object_name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(rec.pipeline), desc.debug_name);
    return pipelines_.allocate(std::move(rec));
}

void VulkanDevice::destroy(PipelineHandle h) {
    PipelineRecord rec;
    if (pipelines_.release(h, rec)) {
        defer([this, p = rec.pipeline] { vkDestroyPipeline(device_, p, nullptr); });
    }
}

} // namespace aether::rhi::vk
