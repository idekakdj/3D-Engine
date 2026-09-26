// aether/core/window.h — platform window + surface source for the RHI.
//
// FROZEN CONTRACT (ADR-0001). Core owns the window (GLFW hidden in the .cpp). RHI
// creates the Vulkan surface itself from native_handle(); core never links Vulkan.
// Wave 0 ships a WORKING window (not a stub) so RHI bring-up is not blocked.
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"

#include <functional>
#include <memory>

namespace aether {

struct WindowDesc {
    String title  = "Aether";
    u32    width  = 1280;
    u32    height = 720;
    bool   resizable = true;
    bool   vsync     = true; // hint consumed by the swapchain, surfaced here for convenience
};

class Window {
public:
    // Creates and shows a window. Also calls glfwInitVulkanLoader(vkGetInstanceProcAddr)
    // so GLFW and volk share one loader (ADR-0001). Returns nullptr on failure.
    static std::unique_ptr<Window> create(const WindowDesc& desc);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Pump OS events, update Input edge states, and refresh framebuffer size.
    void poll_events();

    [[nodiscard]] bool should_close() const;
    void request_close();

    [[nodiscard]] u32  width() const;   // framebuffer size in pixels
    [[nodiscard]] u32  height() const;
    [[nodiscard]] bool minimized() const; // zero-area: renderer should skip the frame

    // Native handles for surface creation. On Win32 this is the HWND.
    [[nodiscard]] void* native_handle() const;         // HWND
    [[nodiscard]] void* native_instance_handle() const; // HINSTANCE

    // Vulkan instance extensions GLFW requires (VK_KHR_surface + platform surface).
    // Returned as C strings valid for the window's lifetime.
    [[nodiscard]] Span<const char* const> required_instance_extensions() const;

    // Resize callback (framebuffer size). RHI subscribes to recreate the swapchain.
    void set_resize_callback(std::function<void(u32, u32)> cb);

    [[nodiscard]] void* glfw_handle() const; // GLFWwindow* (for the ImGui GLFW backend)

private:
    Window();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aether
