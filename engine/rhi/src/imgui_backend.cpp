// imgui_backend.cpp — Dear ImGui (docking + multi-viewport) on GLFW + Vulkan dynamic
// rendering, confined to aether.rhi (ADR-0001). Main thread only.
//
// Ordering: imgui_init must run AFTER Window::create (the Window's GLFW callbacks must
// already be installed so ImGui's GLFW backend, installed with install_callbacks=true,
// chains to them) and after create_device (needs the swapchain format / image count).
// Fonts: 1.92's texture system creates/updates atlas textures inside RenderDrawData.
#include "aether/rhi/imgui.h"

#include "aether/core/paths_ext.h"
#include "aether/core/window.h"
#include "vk_command_list.h"
#include "vk_device.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>

#include <cstdlib>

#include <string>
#include <unordered_set>

namespace aether::rhi {
namespace {

struct ImGuiBridge {
    vk::VulkanDevice*                   device = nullptr;
    std::string                         ini_path;
    VkFormat                            color_format = VK_FORMAT_B8G8R8A8_UNORM;
    std::unordered_set<VkDescriptorSet> pending_removals;
};
ImGuiBridge g_bridge;

void check_vk_result(VkResult r) {
    if (r != VK_SUCCESS) {
        AE_LOG_ERROR("ImGui", "Vulkan backend error: {}", vk::result_string(r));
    }
}

} // namespace

void imgui_init(Device& device, Window& window) {
    AE_ASSERT_MSG(g_bridge.device == nullptr, "imgui_init called twice");
    vk::VulkanDevice& vd = vk::as_vulkan(device);
    g_bridge.device      = &vd;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    // Multi-viewport (panels / popups / tooltips outside the main window become OS windows with their
    // own swapchains) is opt-in: creating and destroying those windows made the whole editor flicker
    // on Windows (e.g. a tooltip crossing the window edge) and left stray taskbar entries.
    if (const char* v = std::getenv("AETHER_IMGUI_VIEWPORTS"); v != nullptr && v[0] == '1') {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        io.ConfigViewportsNoTaskBarIcon = true;
    }
    // Never CWD-relative: layout persistence lives in the per-user cache.
    g_bridge.ini_path = paths::to_utf8(paths::cache_dir() / "imgui.ini");
    io.IniFilename    = g_bridge.ini_path.c_str();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg].w = 1.0f; // opaque floating panels (the dark theme's is translucent)
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f; // platform windows look native
    }

    ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(window.glfw_handle()), true);

    g_bridge.color_format = vd.swapchain_vk_format();
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion          = VK_API_VERSION_1_3;
    info.Instance            = vd.vk_instance();
    info.PhysicalDevice      = vd.vk_physical_device();
    info.Device              = vd.vk_device();
    info.QueueFamily         = vd.queue_family();
    info.Queue               = vd.vk_queue();
    info.DescriptorPoolSize  = 256; // atlas + user textures (imgui_add_texture)
    info.MinImageCount       = std::max(2u, vd.min_image_count());
    info.ImageCount          = std::max(info.MinImageCount, vd.swapchain_image_count());
    info.PipelineCache       = vd.pipeline_cache();
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_bridge.color_format;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn              = &check_vk_result;
    if (!ImGui_ImplVulkan_Init(&info)) {
        AE_LOG_ERROR("ImGui", "ImGui_ImplVulkan_Init failed");
    }

    // Secondary viewports are rendered + presented right after the main present.
    vd.set_post_present_hook([] {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (!ctx || !(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) {
            return;
        }
        // Only when this frame's UI was actually rendered (EndFrame/Render ran).
        if (ctx->FrameCountEnded == ctx->FrameCount && ctx->FrameCountPlatformEnded < ctx->FrameCount) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
    });
    AE_LOG_INFO("ImGui", "Dear ImGui {} initialized (docking{}, dynamic rendering)", ImGui::GetVersion(),
                (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) ? " + viewports" : "");
}

void imgui_shutdown(Device& device) {
    vk::VulkanDevice& vd = vk::as_vulkan(device);
    if (!g_bridge.device) {
        return;
    }
    vd.wait_idle();
    vd.set_post_present_hook(nullptr);
    for (VkDescriptorSet ds : g_bridge.pending_removals) {
        ImGui_ImplVulkan_RemoveTexture(ds); // GPU idle: safe now
    }
    g_bridge.pending_removals.clear();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    g_bridge.device = nullptr;
}

void imgui_new_frame() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx && ctx->WithinFrameScope) {
        ImGui::EndFrame(); // previous frame was skipped (e.g. minimized): close it cleanly
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void imgui_render(CommandList& cmd) {
    auto& list = static_cast<vk::VulkanCommandList&>(cmd);
    AE_ASSERT_MSG(list.in_rendering(), "imgui_render must be called inside begin_rendering()");
    ImGui::Render();
    // Diagnostics: font atlas (re)creations upload with a queue wait-idle mid-frame; logged so they
    // can be correlated with visual hitches.
    if (const ImDrawData* dd = ImGui::GetDrawData(); dd != nullptr && dd->Textures != nullptr) {
        for (const ImTextureData* t : *dd->Textures) {
            if (t->Status == ImTextureStatus_WantCreate) {
                AE_LOG_INFO("ImGui", "UI texture #{} created ({}x{})", t->UniqueID, t->Width, t->Height);
            }
        }
    }
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), list.vk());
    list.invalidate_bindings(); // ImGui bound its own pipeline layout / descriptor set
}

u64 imgui_add_texture(Device& device, TextureHandle texture, SamplerHandle /*sampler*/) {
    // 1.92 backend: sampled-image descriptor + ImGui's own linear sampler (sampler ignored).
    const vk::TextureRecord* t = vk::as_vulkan(device).texture(texture);
    if (!t) {
        AE_LOG_ERROR("ImGui", "imgui_add_texture: invalid texture handle");
        return 0;
    }
    const VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture(t->view, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
    return reinterpret_cast<u64>(ds);
}

void imgui_remove_texture(Device& device, u64 imgui_texture_id) {
    if (imgui_texture_id == 0) {
        return;
    }
    const auto ds = reinterpret_cast<VkDescriptorSet>(imgui_texture_id);
    g_bridge.pending_removals.insert(ds);
    // In-flight frames may still sample it: free once the GPU is past them.
    vk::as_vulkan(device).defer([ds] {
        if (g_bridge.device && g_bridge.pending_removals.erase(ds) > 0) {
            ImGui_ImplVulkan_RemoveTexture(ds);
        }
    });
}

} // namespace aether::rhi
