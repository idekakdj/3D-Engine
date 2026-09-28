// app_identity.h — PRIVATE: window icon + startup splash of the Application (ADR-0014).
#pragma once

#include "aether/core/types.h"
#include "aether/rhi/resources.h"

#include <filesystem>
#include <string>

namespace aether {
class Window;
}
namespace aether::rhi {
class Device;
}

namespace aether::gameplay {

// Sets the window's title-bar / taskbar icon from a PNG (any size; GLFW picks per DPI). Returns
// false (logged) when the file cannot be read. Main thread.
bool set_window_icon_png(Window& window, const std::filesystem::path& png);

// A splash drawn with Dear ImGui directly on the swapchain while the Application initialises: the
// image, a title, the version and a status line. show() presents one frame. Main thread; needs an
// initialised ImGui context. Destroy (or release()) before the device.
class SplashScreen {
public:
    SplashScreen(rhi::Device& device, Window& window, const std::filesystem::path& image, std::string title);
    ~SplashScreen();
    SplashScreen(const SplashScreen&) = delete;
    SplashScreen& operator=(const SplashScreen&) = delete;

    void show(const char* status);
    void release(); // waits for the GPU, frees the image

private:
    rhi::Device&       device_;
    Window&            window_;
    std::string        title_;
    rhi::TextureHandle texture_;
    u64                imgui_id_ = 0;
    u32                width_ = 0, height_ = 0;
};

} // namespace aether::gameplay
