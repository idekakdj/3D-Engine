// vk_swapchain.cpp — swapchain (re)creation. Format B8G8R8A8_UNORM (ADR-0002); FIFO with
// vsync, else MAILBOX -> IMMEDIATE -> FIFO; minImageCount+1 clamped to the surface max.
#include "aether/core/window.h"
#include "vk_convert.h"
#include "vk_device.h"

#include <algorithm>
#include <format>

namespace aether::rhi::vk {

bool VulkanDevice::recreate_swapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    VkResult                 r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
    if (r != VK_SUCCESS) {
        AE_LOG_ERROR("RHI", "surface capabilities query failed: {}", result_string(r));
        return false;
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) { // surface size decided by the swapchain
        extent.width  = std::clamp(window_->width(), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(window_->height(), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        return false; // minimized: stay dirty, retry on a later frame
    }

    // No frame may reference the old images/views past this point.
    VK_CHECK(vkDeviceWaitIdle(device_));

    // ---- format: BGRA8 UNORM + sRGB-nonlinear colour space (display encoding in-shader) ----
    u32 format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &format_count, formats.data());
    VkSurfaceFormatKHR chosen{ VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    for (const VkSurfaceFormatKHR& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    if (chosen.format == VK_FORMAT_UNDEFINED) {
        AE_LOG_ERROR("RHI", "surface does not offer B8G8R8A8_UNORM/SRGB_NONLINEAR (ADR-0002)");
        return false;
    }

    // ---- present mode ----
    u32 mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &mode_count, modes.data());
    auto has_mode = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR; // always supported
    if (!vsync_) {
        if (has_mode(VK_PRESENT_MODE_MAILBOX_KHR)) {
            mode = VK_PRESENT_MODE_MAILBOX_KHR;
        } else if (has_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
            mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
    }

    u32 image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) {
        image_count = std::min(image_count, caps.maxImageCount);
    }

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    usage |= caps.supportedUsageFlags & (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (VkCompositeAlphaFlagBitsKHR a : { VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                               VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR }) {
            if (caps.supportedCompositeAlpha & a) {
                alpha = a;
                break;
            }
        }
    }

    VkSwapchainKHR           old = swapchain_;
    VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface          = surface_;
    ci.minImageCount    = image_count;
    ci.imageFormat      = chosen.format;
    ci.imageColorSpace  = chosen.colorSpace;
    ci.imageExtent      = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = usage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = alpha;
    ci.presentMode      = mode;
    ci.clipped          = VK_TRUE;
    ci.oldSwapchain     = old;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    r                      = vkCreateSwapchainKHR(device_, &ci, nullptr, &created);
    if (r != VK_SUCCESS) {
        AE_LOG_ERROR("RHI", "vkCreateSwapchainKHR failed: {}", result_string(r));
        return false;
    }

    // Retire the old swapchain's image wrappers (GPU is idle) and the swapchain itself.
    destroy_swapchain(true);
    if (old) {
        vkDestroySwapchainKHR(device_, old, nullptr);
    }
    swapchain_        = created;
    swapchain_format_ = chosen.format;
    color_space_      = chosen.colorSpace;
    swapchain_extent_ = extent;
    present_mode_     = mode;
    min_image_count_  = image_count;

    u32 count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swapchain_images_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchain_images_.data());

    swapchain_textures_.clear();
    for (u32 i = 0; i < count; ++i) {
        TextureRecord t;
        t.image      = swapchain_images_[i];
        t.format     = chosen.format;
        t.aspect     = VK_IMAGE_ASPECT_COLOR_BIT;
        t.owns_image = false;
        t.desc.type  = TextureType::Tex2D;
        t.desc.format = Format::BGRA8Unorm;
        t.desc.width  = extent.width;
        t.desc.height = extent.height;
        t.desc.usage  = TextureUsage::ColorAttach | TextureUsage::TransferDst;
        t.desc.debug_name = std::format("swapchain image {}", i);

        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image            = t.image;
        vi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vi.format           = chosen.format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &t.view));
        t.attachment_views = { t.view };
        set_object_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(t.image), t.desc.debug_name);
        set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<u64>(t.view), t.desc.debug_name + " view");
        swapchain_textures_.push_back(textures_.allocate(std::move(t)));
    }

    // Per-image present semaphores. Old ones may still be referenced by a pending present,
    // so they are retired through the deferred queue instead of destroyed right away.
    if (!render_finished_.empty()) {
        std::vector<VkSemaphore> retired = std::move(render_finished_);
        defer([this, retired] {
            for (VkSemaphore s : retired) {
                vkDestroySemaphore(device_, s, nullptr);
            }
        });
    }
    render_finished_.assign(count, VK_NULL_HANDLE);
    for (u32 i = 0; i < count; ++i) {
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &render_finished_[i]));
        set_object_name(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<u64>(render_finished_[i]),
                        std::format("render finished {}", i));
    }

    swapchain_dirty_ = false;
    AE_LOG_INFO("RHI", "swapchain {}x{}, {} images, {}", extent.width, extent.height, count, present_mode_name(mode));
    return true;
}

// keep_handle=true: only the image wrappers go (recreation); false: everything (teardown).
void VulkanDevice::destroy_swapchain(bool keep_handle) {
    for (TextureHandle h : swapchain_textures_) {
        TextureRecord t;
        if (textures_.release(h, t)) {
            vkDestroyImageView(device_, t.view, nullptr);
        }
    }
    swapchain_textures_.clear();
    swapchain_images_.clear();
    if (keep_handle) {
        return;
    }
    for (VkSemaphore s : render_finished_) {
        vkDestroySemaphore(device_, s, nullptr);
    }
    render_finished_.clear();
    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void VulkanDevice::set_vsync(bool enabled) {
    if (vsync_ != enabled) {
        vsync_           = enabled;
        swapchain_dirty_ = true;
    }
}

} // namespace aether::rhi::vk
