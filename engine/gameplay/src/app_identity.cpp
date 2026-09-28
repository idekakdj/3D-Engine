// app_identity.cpp — see app_identity.h.
#include "app_identity.h"

#include "aether/core/log.h"
#include "aether/core/paths.h"
#include "aether/core/window.h"
#include "aether/rhi/command_list.h"
#include "aether/rhi/device.h"
#include "aether/rhi/imgui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>

// stb_image's implementation lives in aether.assets (image_importer.cpp, STBI_NO_STDIO).
#define STBI_NO_STDIO
#include <stb_image.h>

#include <algorithm>
#include <vector>

namespace aether::gameplay {

namespace {
struct Pixels {
    int             w = 0, h = 0;
    std::vector<u8> rgba;
};

Pixels load_png(const std::filesystem::path& file) {
    Pixels                  p;
    const std::vector<byte> bytes = paths::read_file(file);
    if (bytes.empty()) {
        return p;
    }
    int      n = 0;
    stbi_uc* data =
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &p.w, &p.h, &n, 4);
    if (data != nullptr) {
        p.rgba.assign(data, data + static_cast<size_t>(p.w) * p.h * 4);
        stbi_image_free(data);
    }
    return p;
}
} // namespace

bool set_window_icon_png(Window& window, const std::filesystem::path& png) {
    Pixels p = load_png(png);
    auto*  win = static_cast<GLFWwindow*>(window.glfw_handle());
    if (p.rgba.empty() || win == nullptr) {
        AE_LOG_WARN("App", "window icon '{}' could not be loaded", png.generic_string());
        return false;
    }
    GLFWimage img{ p.w, p.h, p.rgba.data() };
    glfwSetWindowIcon(win, 1, &img); // copied by GLFW
    return true;
}

SplashScreen::SplashScreen(rhi::Device& device, Window& window, const std::filesystem::path& image, std::string title)
    : device_(device), window_(window), title_(std::move(title)) {
    Pixels p = load_png(image);
    if (p.rgba.empty()) {
        AE_LOG_WARN("App", "splash image '{}' could not be loaded", image.generic_string());
        return;
    }
    rhi::TextureDesc td;
    td.type       = rhi::TextureType::Tex2D;
    td.format     = rhi::Format::RGBA8Unorm; // display-encoded bytes, shown as-is
    td.width      = static_cast<u32>(p.w);
    td.height     = static_cast<u32>(p.h);
    td.usage      = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.debug_name = "Splash";
    texture_      = device_.create_texture(td);
    if (!texture_.is_valid()) {
        return;
    }
    device_.update_texture(texture_, ByteSpan(reinterpret_cast<const byte*>(p.rgba.data()), p.rgba.size()), false);
    imgui_id_ = rhi::imgui_add_texture(device_, texture_, rhi::SamplerHandle{});
    width_    = td.width;
    height_   = td.height;
}

SplashScreen::~SplashScreen() { release(); }

void SplashScreen::show(const char* status) {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    window_.poll_events(); // keep the window responsive (and let it paint) during long steps
    rhi::FrameInfo frame = device_.begin_frame();
    if (!frame.valid) {
        return;
    }
    rhi::imgui_new_frame();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.04f, 0.08f, 1.0f));
    ImGui::Begin("##splash", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Image centred, scaled to fit ~60% of the window.
    f32 iw = 0.0f, ih = 0.0f;
    if (imgui_id_ != 0) {
        const f32 scale = std::min({ vp->Size.x * 0.6f / f32(width_), vp->Size.y * 0.6f / f32(height_), 1.5f });
        iw = f32(width_) * scale;
        ih = f32(height_) * scale;
    }
    const f32    text_h = ImGui::GetTextLineHeightWithSpacing();
    const f32    block  = ih + text_h * 3.0f;
    const ImVec2 top(vp->Pos.x + (vp->Size.x - iw) * 0.5f, vp->Pos.y + (vp->Size.y - block) * 0.5f);
    if (imgui_id_ != 0) {
        dl->AddImage(static_cast<ImTextureID>(imgui_id_), top, ImVec2(top.x + iw, top.y + ih));
    }
    auto centred = [&](const std::string& text, f32 y, ImU32 colour) {
        const ImVec2 sz = ImGui::CalcTextSize(text.c_str());
        dl->AddText(ImVec2(vp->Pos.x + (vp->Size.x - sz.x) * 0.5f, y), colour, text.c_str());
    };
    const f32 y = top.y + ih + text_h * 0.5f;
    centred(title_, y, IM_COL32(230, 235, 255, 255));
    centred(status != nullptr ? status : "", y + text_h * 1.5f, IM_COL32(140, 150, 180, 255));
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    rhi::CommandList& cmd = *frame.cmd;
    cmd.barrier(frame.swapchain_image, rhi::ResourceState::Undefined, rhi::ResourceState::ColorAttachment);
    rhi::RenderingInfo ri;
    ri.render_area = frame.extent;
    ri.color = { rhi::ColorAttachment{ frame.swapchain_image, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                       Vec4(0.03f, 0.04f, 0.08f, 1.0f) } };
    cmd.begin_rendering(ri);
    rhi::imgui_render(cmd);
    cmd.end_rendering();
    device_.end_frame(frame);
}

void SplashScreen::release() {
    if (!texture_.is_valid()) {
        return;
    }
    device_.wait_idle();
    if (imgui_id_ != 0) {
        rhi::imgui_remove_texture(device_, imgui_id_);
        imgui_id_ = 0;
    }
    device_.destroy(texture_);
    texture_ = {};
}

} // namespace aether::gameplay
