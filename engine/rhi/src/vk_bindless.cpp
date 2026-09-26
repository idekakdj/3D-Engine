// vk_bindless.cpp — the universal binding model (ADR-0002, mirrors shaders/common/bindless.glsl):
//   set 0 binding 0: COMBINED_IMAGE_SAMPLER[N]  binding 1: STORAGE_IMAGE[M]
//   both PARTIALLY_BOUND | UPDATE_AFTER_BIND; 128 B push constants on VK_SHADER_STAGE_ALL.
// Every slot always holds a valid descriptor (1x1 fallbacks), so a stale index can never
// reference a destroyed view. Freed indices are recycled only after the GPU is done.
#include "vk_convert.h"
#include "vk_device.h"

#include <algorithm>
#include <array>

namespace aether::rhi::vk {
namespace {
constexpr u32 kMaxBindless       = 16384;
constexpr u32 kPushConstantBytes = 128;
} // namespace

void VulkanDevice::create_bindless() {
    VkPhysicalDeviceVulkan12Properties v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceProperties2        p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    p2.pNext = &v12;
    vkGetPhysicalDeviceProperties2(gpu_, &p2);

    sampled_capacity_ = std::min({ kMaxBindless, v12.maxDescriptorSetUpdateAfterBindSampledImages,
                                   v12.maxDescriptorSetUpdateAfterBindSamplers,
                                   v12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                   v12.maxPerStageDescriptorUpdateAfterBindSamplers });
    storage_capacity_ = std::min({ kMaxBindless, v12.maxDescriptorSetUpdateAfterBindStorageImages,
                                   v12.maxPerStageDescriptorUpdateAfterBindStorageImages });
    const u32 per_stage = v12.maxPerStageUpdateAfterBindResources;
    if (sampled_capacity_ + storage_capacity_ > per_stage) {
        storage_capacity_ = std::min(storage_capacity_, per_stage / 4);
        sampled_capacity_ = std::min(sampled_capacity_, per_stage - storage_capacity_);
    }

    const VkDescriptorBindingFlags binding_flags[2] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
    };
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
    flags_ci.bindingCount  = 2;
    flags_ci.pBindingFlags = binding_flags;
    const VkDescriptorSetLayoutBinding bindings[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampled_capacity_, VK_SHADER_STAGE_ALL, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage_capacity_, VK_SHADER_STAGE_ALL, nullptr },
    };
    VkDescriptorSetLayoutCreateInfo layout_ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layout_ci.pNext        = &flags_ci;
    layout_ci.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_ci.bindingCount = 2;
    layout_ci.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &bindless_layout_));

    const VkDescriptorPoolSize sizes[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampled_capacity_ },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage_capacity_ },
    };
    VkDescriptorPoolCreateInfo pool_ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool_ci.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_ci.maxSets       = 1;
    pool_ci.poolSizeCount = 2;
    pool_ci.pPoolSizes    = sizes;
    VK_CHECK(vkCreateDescriptorPool(device_, &pool_ci, nullptr, &bindless_pool_));

    VkDescriptorSetAllocateInfo alloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    alloc.descriptorPool     = bindless_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts        = &bindless_layout_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &alloc, &bindless_set_));
    set_object_name(VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<u64>(bindless_set_), "bindless set");

    const VkPushConstantRange push{ VK_SHADER_STAGE_ALL, 0, kPushConstantBytes };
    VkPipelineLayoutCreateInfo pl_ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl_ci.setLayoutCount         = 1;
    pl_ci.pSetLayouts            = &bindless_layout_;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges    = &push;
    VK_CHECK(vkCreatePipelineLayout(device_, &pl_ci, nullptr, &pipeline_layout_));
    set_object_name(VK_OBJECT_TYPE_PIPELINE_LAYOUT, reinterpret_cast<u64>(pipeline_layout_), "universal layout");

    // Free lists hand out the lowest index first.
    sampled_free_.resize(sampled_capacity_);
    storage_free_.resize(storage_capacity_);
    for (u32 i = 0; i < sampled_capacity_; ++i) sampled_free_[i] = sampled_capacity_ - 1 - i;
    for (u32 i = 0; i < storage_capacity_; ++i) storage_free_[i] = storage_capacity_ - 1 - i;
    sampled_gen_.assign(sampled_capacity_, 1);
    storage_gen_.assign(storage_capacity_, 1);
    sampled_live_.assign(sampled_capacity_, 0);
    storage_live_.assign(storage_capacity_, 0);

    // ---- fallbacks: every slot starts (and returns to) a valid descriptor ----
    fallback_sampler_ = create_sampler(SamplerDesc{});
    TextureDesc td;
    td.format     = Format::RGBA8Unorm;
    td.usage      = TextureUsage::Sampled;
    td.debug_name = "bindless fallback (sampled)";
    fallback_texture_ = create_texture(td);
    const u8 black[4] = { 0, 0, 0, 255 };
    update_texture(fallback_texture_, ByteSpan(reinterpret_cast<const byte*>(black), 4), false);

    td.format     = Format::RGBA16F;
    td.usage      = TextureUsage::Storage;
    td.debug_name = "bindless fallback (storage)";
    fallback_storage_ = create_texture(td);
    {
        const TextureRecord* t   = textures_.get(fallback_storage_);
        VkCommandBuffer      cmd = upload_cmd();
        const VkImageMemoryBarrier2 b =
            make_image_barrier(t->image, t->aspect, ResourceState::Undefined, ResourceState::ShaderWrite);
        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    const TextureRecord* ft = textures_.get(fallback_texture_);
    const TextureRecord* fs = textures_.get(fallback_storage_);
    const SamplerRecord* sm = samplers_.get(fallback_sampler_);
    std::vector<VkDescriptorImageInfo> sampled(sampled_capacity_,
                                               { sm->sampler, ft->view, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL });
    std::vector<VkDescriptorImageInfo> storage(storage_capacity_,
                                               { VK_NULL_HANDLE, fs->storage_views[0], VK_IMAGE_LAYOUT_GENERAL });
    VkWriteDescriptorSet writes[2]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bindless_set_, 0, 0, sampled_capacity_,
                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampled.data(), nullptr, nullptr };
    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bindless_set_, 1, 0, storage_capacity_,
                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage.data(), nullptr, nullptr };
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);

    features_.max_bindless_textures = sampled_capacity_;
    AE_LOG_INFO("RHI", "bindless: {} sampled + {} storage descriptors (update-after-bind)", sampled_capacity_,
                storage_capacity_);
}

void VulkanDevice::destroy_bindless() {
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (bindless_pool_) vkDestroyDescriptorPool(device_, bindless_pool_, nullptr);
    if (bindless_layout_) vkDestroyDescriptorSetLayout(device_, bindless_layout_, nullptr);
    pipeline_layout_ = VK_NULL_HANDLE;
    bindless_pool_   = VK_NULL_HANDLE;
    bindless_layout_ = VK_NULL_HANDLE;
    bindless_set_    = VK_NULL_HANDLE;
}

void VulkanDevice::write_sampled_descriptor(u32 index, VkImageView view, VkSampler sampler) {
    const VkDescriptorImageInfo info{ sampler, view, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL };
    const VkWriteDescriptorSet  w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bindless_set_, 0, index, 1,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &info, nullptr, nullptr };
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
}

void VulkanDevice::write_storage_descriptor(u32 index, VkImageView view) {
    const VkDescriptorImageInfo info{ VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL };
    const VkWriteDescriptorSet  w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bindless_set_, 1, index, 1,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &info, nullptr, nullptr };
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
}

DescriptorHandle VulkanDevice::register_texture(TextureHandle texture, SamplerHandle sampler) {
    const TextureRecord* t = textures_.get(texture);
    const SamplerRecord* s = samplers_.get(sampler);
    if (!t || !s) {
        AE_LOG_ERROR("RHI", "register_texture: invalid texture or sampler handle");
        return {};
    }
    if (!any(t->desc.usage & TextureUsage::Sampled)) {
        AE_LOG_ERROR("RHI", "register_texture '{}': texture lacks TextureUsage::Sampled", t->desc.debug_name);
        return {};
    }
    std::lock_guard lock(bindless_mutex_);
    if (sampled_free_.empty()) {
        AE_LOG_ERROR("RHI", "register_texture: bindless sampled table full ({})", sampled_capacity_);
        return {};
    }
    const u32 index = sampled_free_.back();
    sampled_free_.pop_back();
    sampled_live_[index] = 1;
    write_sampled_descriptor(index, t->view, s->sampler);
    return DescriptorHandle(index, sampled_gen_[index]);
}

DescriptorHandle VulkanDevice::register_storage_texture(TextureHandle texture, u32 mip) {
    const TextureRecord* t = textures_.get(texture);
    if (!t || mip >= t->storage_views.size()) {
        AE_LOG_ERROR("RHI", "register_storage_texture: invalid handle, no Storage usage, or mip out of range");
        return {};
    }
    std::lock_guard lock(bindless_mutex_);
    if (storage_free_.empty()) {
        AE_LOG_ERROR("RHI", "register_storage_texture: bindless storage table full ({})", storage_capacity_);
        return {};
    }
    const u32 index = storage_free_.back();
    storage_free_.pop_back();
    storage_live_[index] = 1;
    write_storage_descriptor(index, t->storage_views[mip]);
    return DescriptorHandle(index, storage_gen_[index]);
}

void VulkanDevice::unregister_texture(DescriptorHandle h) {
    {
        std::lock_guard lock(bindless_mutex_);
        const u32       index = h.index();
        if (!h.is_valid() || index >= sampled_capacity_ || !sampled_live_[index] || sampled_gen_[index] != h.generation()) {
            return; // stale / double unregister
        }
        sampled_live_[index] = 0;
        sampled_gen_[index]  = static_cast<u8>(sampled_gen_[index] == 255 ? 1 : sampled_gen_[index] + 1);
    }
    defer([this, index = h.index()] {
        const TextureRecord* ft = textures_.get(fallback_texture_);
        const SamplerRecord* sm = samplers_.get(fallback_sampler_);
        std::lock_guard      lock(bindless_mutex_);
        if (ft && sm) {
            write_sampled_descriptor(index, ft->view, sm->sampler);
        }
        sampled_free_.push_back(index);
    });
}

void VulkanDevice::unregister_storage_texture(DescriptorHandle h) {
    {
        std::lock_guard lock(bindless_mutex_);
        const u32       index = h.index();
        if (!h.is_valid() || index >= storage_capacity_ || !storage_live_[index] || storage_gen_[index] != h.generation()) {
            return;
        }
        storage_live_[index] = 0;
        storage_gen_[index]  = static_cast<u8>(storage_gen_[index] == 255 ? 1 : storage_gen_[index] + 1);
    }
    defer([this, index = h.index()] {
        const TextureRecord* fs = textures_.get(fallback_storage_);
        std::lock_guard      lock(bindless_mutex_);
        if (fs) {
            write_storage_descriptor(index, fs->storage_views[0]);
        }
        storage_free_.push_back(index);
    });
}

} // namespace aether::rhi::vk
