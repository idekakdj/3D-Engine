// window_glfw.cpp — GLFW-backed aether::Window (GLFW stays private to core).
//
// Loader sharing (ADR-0001): volkInitialize() + glfwInitVulkanLoader(vkGetInstanceProcAddr)
// run BEFORE glfwInit, so GLFW (and ImGui's secondary-viewport surfaces created through
// glfwCreateWindowSurface) use the same vulkan-1.dll entry point as the RHI. volk.h is
// included before GLFW so GLFW's Vulkan-typed declarations are visible.
//
// Callback ordering contract: the Window installs its key / mouse / cursor / scroll /
// framebuffer callbacks at creation. ImGui's GLFW backend (rhi::imgui_init, installed
// later with install_callbacks=true) saves these as its "previous" callbacks and CHAINS
// to them, so both ImGui and Input see every event. Anything that calls glfwSet*Callback
// on this window after imgui_init breaks that chain - don't.
#include "aether/core/input.h"
#include "aether/core/log.h"
#include "aether/core/window.h"
#include "input_internal.h"

#include <volk.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef _WIN32
#    define GLFW_EXPOSE_NATIVE_WIN32
#    include <GLFW/glfw3native.h>
#endif

#include <mutex>

namespace aether {
namespace {

std::mutex g_glfw_mutex;
int        g_glfw_refs = 0;

void glfw_error_callback(int code, const char* description) {
    AE_LOG_ERROR("Window", "GLFW error {:#x}: {}", code, description ? description : "?");
}

bool acquire_glfw() {
    std::lock_guard lock(g_glfw_mutex);
    if (g_glfw_refs++ > 0) {
        return true;
    }
    glfwSetErrorCallback(glfw_error_callback);
    if (volkInitialize() == VK_SUCCESS) {
        glfwInitVulkanLoader(vkGetInstanceProcAddr);
    } else {
        AE_LOG_WARN("Window", "Vulkan loader (vulkan-1) not found; window has no Vulkan support");
    }
    if (!glfwInit()) {
        --g_glfw_refs;
        AE_LOG_ERROR("Window", "glfwInit failed");
        return false;
    }
    return true;
}

void release_glfw() {
    std::lock_guard lock(g_glfw_mutex);
    if (--g_glfw_refs == 0) {
        glfwTerminate();
    }
}

} // namespace

struct Window::Impl {
    GLFWwindow*                   window    = nullptr;
    u32                           fb_width  = 0;
    u32                           fb_height = 0;
    std::function<void(u32, u32)> resize_callback;
    Span<const char* const>       instance_extensions;

    static Impl* from(GLFWwindow* w) { return static_cast<Impl*>(glfwGetWindowUserPointer(w)); }

    void refresh_framebuffer_size() {
        int w = 0;
        int h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        fb_width  = static_cast<u32>(w > 0 ? w : 0);
        fb_height = static_cast<u32>(h > 0 ? h : 0);
    }

    static void key_cb(GLFWwindow*, int key, int /*scancode*/, int action, int /*mods*/) {
        if (action == GLFW_REPEAT) {
            return;
        }
        detail::input_on_key(key, action == GLFW_PRESS);
    }
    static void mouse_button_cb(GLFWwindow*, int button, int action, int /*mods*/) {
        detail::input_on_mouse_button(button, action == GLFW_PRESS);
    }
    static void cursor_cb(GLFWwindow*, double x, double y) { detail::input_on_cursor(x, y); }
    static void scroll_cb(GLFWwindow*, double dx, double dy) { detail::input_on_scroll(dx, dy); }
    static void framebuffer_cb(GLFWwindow* w, int width, int height) {
        Impl* impl = from(w);
        if (!impl) {
            return;
        }
        impl->fb_width  = static_cast<u32>(width > 0 ? width : 0);
        impl->fb_height = static_cast<u32>(height > 0 ? height : 0);
        if (impl->resize_callback) {
            impl->resize_callback(impl->fb_width, impl->fb_height);
        }
    }
};

Window::Window() : impl_(std::make_unique<Impl>()) {}

Window::~Window() {
    if (impl_ && impl_->window) {
        glfwDestroyWindow(impl_->window);
        impl_->window = nullptr;
        release_glfw();
    }
}

std::unique_ptr<Window> Window::create(const WindowDesc& desc) {
    if (!acquire_glfw()) {
        return nullptr;
    }
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);

    GLFWwindow* handle = glfwCreateWindow(static_cast<int>(desc.width), static_cast<int>(desc.height),
                                          desc.title.c_str(), nullptr, nullptr);
    if (!handle) {
        AE_LOG_ERROR("Window", "glfwCreateWindow({}x{}) failed", desc.width, desc.height);
        release_glfw();
        return nullptr;
    }

    std::unique_ptr<Window> window(new Window());
    Impl&                   impl = *window->impl_;
    impl.window                  = handle;
    glfwSetWindowUserPointer(handle, &impl);

    // Installed BEFORE any ImGui backend so it can chain to them (see file comment).
    glfwSetKeyCallback(handle, &Impl::key_cb);
    glfwSetMouseButtonCallback(handle, &Impl::mouse_button_cb);
    glfwSetCursorPosCallback(handle, &Impl::cursor_cb);
    glfwSetScrollCallback(handle, &Impl::scroll_cb);
    glfwSetFramebufferSizeCallback(handle, &Impl::framebuffer_cb);

    impl.refresh_framebuffer_size();
    if (glfwVulkanSupported()) {
        uint32_t     count = 0;
        const char** exts  = glfwGetRequiredInstanceExtensions(&count);
        if (exts) {
            impl.instance_extensions = Span<const char* const>(exts, count);
        }
    }

    double cx = 0.0;
    double cy = 0.0;
    glfwGetCursorPos(handle, &cx, &cy);
    detail::input_on_cursor(cx, cy);

    AE_LOG_INFO("Window", "created '{}' {}x{} (framebuffer {}x{})", desc.title, desc.width, desc.height,
                impl.fb_width, impl.fb_height);
    return window;
}

void Window::poll_events() {
    Input::begin_frame(); // advance edges BEFORE this frame's events arrive
    glfwPollEvents();
    impl_->refresh_framebuffer_size();
}

bool Window::should_close() const {
    return glfwWindowShouldClose(impl_->window) != 0;
}

void Window::request_close() {
    glfwSetWindowShouldClose(impl_->window, GLFW_TRUE);
}

u32 Window::width() const {
    return impl_->fb_width;
}

u32 Window::height() const {
    return impl_->fb_height;
}

bool Window::minimized() const {
    return impl_->fb_width == 0 || impl_->fb_height == 0 ||
           glfwGetWindowAttrib(impl_->window, GLFW_ICONIFIED) != 0;
}

void* Window::native_handle() const {
#ifdef _WIN32
    return glfwGetWin32Window(impl_->window);
#else
    return nullptr;
#endif
}

void* Window::native_instance_handle() const {
#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(impl_->window);
    return hwnd ? reinterpret_cast<void*>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)) : nullptr;
#else
    return nullptr;
#endif
}

Span<const char* const> Window::required_instance_extensions() const {
    return impl_->instance_extensions;
}

void Window::set_resize_callback(std::function<void(u32, u32)> cb) {
    impl_->resize_callback = std::move(cb);
}

void* Window::glfw_handle() const {
    return impl_->window;
}

} // namespace aether
