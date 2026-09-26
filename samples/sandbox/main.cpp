// sandbox — the M0 artifact. Window + Vulkan device + ImGui overlay; a rotating triangle
// (runtime-compiled GLSL from shaders/sandbox, BDA vertex pulling, two bindless textures:
// a mipmapped checkerboard and a compute-written storage image) over a vertex-buffer quad
// and an animated clear colour, with depth (reverse-Z).
//
// Usage: sandbox [--frames N] [--test-resize] [--no-vsync]
//   --frames N      run N loop iterations then exit (automated verification)
//   --test-resize   scripted resize / minimize / restore mid-run (swapchain recreation)
// Exit code: 0 = clean; 1 = init failure, any Vulkan validation error/warning, or a leak.
#include "aether/core/log.h"
#include "aether/core/math.h"
#include "aether/core/paths.h"
#include "aether/core/time.h"
#include "aether/core/window.h"
#include "aether/rhi/device.h"
#include "aether/rhi/device_ext.h"
#include "aether/rhi/diagnostics.h"
#include "aether/rhi/imgui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace aether;
using namespace aether::rhi;

namespace {

struct Args {
    u64  frames      = 0; // 0 = until the window closes
    bool test_resize = false;
    bool vsync       = true;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            a.frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--test-resize") {
            a.test_resize = true;
        } else if (arg == "--no-vsync") {
            a.vsync = false;
        } else {
            AE_LOG_WARN("Sandbox", "unknown argument '{}'", arg);
        }
    }
    return a;
}

// Mirrors `SandboxPush` in shaders/sandbox/common.glsl (scalar layout).
struct SandboxPush {
    u64 vertices    = 0;
    f32 angle       = 0.0f;
    f32 aspect      = 1.0f;
    u32 checker_tex = 0;
    u32 pattern_tex = 0;
    u32 pattern_img = 0;
    f32 time        = 0.0f;
};
static_assert(sizeof(SandboxPush) == 32);

struct TriVertex {
    f32 pos[2];
    f32 uv[2];
    f32 color[3];
};
static_assert(sizeof(TriVertex) == 28);

struct QuadVertex {
    f32 pos[3];
    f32 color[3];
};

constexpr u32 kPatternSize = 256;

class Sandbox {
public:
    Sandbox(Device& device, Window& window, const Args& args) : device_(device), window_(window), args_(args) {}

    bool init() {
        vs_tri_  = load_shader("sandbox/triangle.vert", ShaderStage::Vertex);
        fs_tri_  = load_shader("sandbox/triangle.frag", ShaderStage::Fragment);
        vs_quad_ = load_shader("sandbox/quad.vert", ShaderStage::Vertex);
        fs_quad_ = load_shader("sandbox/quad.frag", ShaderStage::Fragment);
        cs_      = load_shader("sandbox/pattern.comp", ShaderStage::Compute);
        if (!vs_tri_ || !fs_tri_ || !vs_quad_ || !fs_quad_ || !cs_) {
            return false;
        }

        GraphicsPipelineDesc tri;
        tri.vertex             = vs_tri_;
        tri.fragment           = fs_tri_;
        tri.cull               = CullMode::None;
        tri.depth              = { true, true, CompareOp::GreaterEqual };
        tri.targets.color      = { Format::BGRA8Unorm };
        tri.targets.depth      = Format::D32F;
        tri.push_constant_size = sizeof(SandboxPush);
        tri.debug_name         = "sandbox triangle";
        tri_pipeline_          = device_.create_graphics_pipeline(tri);

        GraphicsPipelineDesc quad = tri;
        quad.vertex               = vs_quad_;
        quad.fragment             = fs_quad_;
        quad.vertex_bindings      = { { 0, sizeof(QuadVertex), false } };
        quad.vertex_attributes    = { { 0, 0, Format::RGB32F }, { 1, 12, Format::RGB32F } };
        quad.debug_name           = "sandbox quad";
        quad_pipeline_            = device_.create_graphics_pipeline(quad);

        compute_pipeline_ = device_.create_compute_pipeline({ cs_, sizeof(SandboxPush), "sandbox pattern" });
        if (!tri_pipeline_ || !quad_pipeline_ || !compute_pipeline_) {
            return false;
        }

        // Triangle vertices: GPU-only storage buffer read through its device address.
        const TriVertex tri_vertices[3] = {
            { { 0.0f, -0.6f }, { 0.5f, 0.0f }, { 1.0f, 0.35f, 0.3f } },
            { { 0.6f, 0.45f }, { 1.0f, 1.0f }, { 0.3f, 1.0f, 0.4f } },
            { { -0.6f, 0.45f }, { 0.0f, 1.0f }, { 0.35f, 0.5f, 1.0f } },
        };
        tri_vb_ = device_.create_buffer({ sizeof(tri_vertices), BufferUsage::Storage, MemoryUsage::GpuOnly, "tri vertices" });
        device_.update_buffer(tri_vb_, std::as_bytes(std::span(tri_vertices)));

        // Background quad: classic vertex + 16-bit index buffers (z = 0.25: behind, reverse-Z).
        const QuadVertex quad_vertices[4] = {
            { { -0.9f, -0.8f, 0.25f }, { 0.10f, 0.10f, 0.14f } },
            { { 0.9f, -0.8f, 0.25f }, { 0.14f, 0.10f, 0.10f } },
            { { 0.9f, 0.8f, 0.25f }, { 0.10f, 0.14f, 0.10f } },
            { { -0.9f, 0.8f, 0.25f }, { 0.12f, 0.12f, 0.12f } },
        };
        const u16 quad_indices[6] = { 0, 1, 2, 2, 3, 0 };
        quad_vb_ = device_.create_buffer({ sizeof(quad_vertices), BufferUsage::Vertex, MemoryUsage::GpuOnly, "quad vb" });
        quad_ib_ = device_.create_buffer({ sizeof(quad_indices), BufferUsage::Index, MemoryUsage::GpuOnly, "quad ib" });
        device_.update_buffer(quad_vb_, std::as_bytes(std::span(quad_vertices)));
        device_.update_buffer(quad_ib_, std::as_bytes(std::span(quad_indices)));

        // Mipmapped checkerboard (mips generated by the device via blits).
        constexpr u32   n = 256;
        std::vector<u8> pixels(n * n * 4);
        for (u32 y = 0; y < n; ++y) {
            for (u32 x = 0; x < n; ++x) {
                const bool on = ((x / 32) + (y / 32)) % 2 == 0;
                u8*        p  = &pixels[(y * n + x) * 4];
                p[0]          = on ? 235 : 40;
                p[1]          = on ? 225 : 45;
                p[2]          = on ? 200 : 60;
                p[3]          = 255;
            }
        }
        TextureDesc td;
        td.format     = Format::RGBA8Unorm;
        td.width      = n;
        td.height     = n;
        td.mip_levels = 9;
        td.usage      = TextureUsage::Sampled;
        td.debug_name = "checker";
        checker_      = device_.create_texture(td);
        device_.update_texture(checker_, std::as_bytes(std::span(pixels)), true);

        td.width      = kPatternSize;
        td.height     = kPatternSize;
        td.mip_levels = 1;
        td.usage      = TextureUsage::Sampled | TextureUsage::Storage;
        td.debug_name = "compute pattern";
        pattern_      = device_.create_texture(td);

        // Self-check of immediate_submit + fill_buffer + persistent readback mapping (the
        // pending uploads above are flushed ahead of this submission).
        const BufferHandle readback =
            device_.create_buffer({ 256, BufferUsage::TransferDst, MemoryUsage::GpuToCpu, "readback check" });
        device_.immediate_submit([&](CommandList& c) { c.fill_buffer(readback, 0, ~0ull, 0xAE7E0001u); });
        const auto* mapped = static_cast<const u32*>(device_.map(readback));
        const bool  readback_ok = mapped && mapped[0] == 0xAE7E0001u && mapped[63] == 0xAE7E0001u;
        device_.destroy(readback);
        if (!readback_ok) {
            AE_LOG_ERROR("Sandbox", "immediate_submit readback self-check failed");
            return false;
        }

        SamplerDesc sd;
        sd.max_anisotropy = 8.0f;
        sampler_          = device_.create_sampler(sd);
        checker_idx_      = device_.register_texture(checker_, sampler_);
        pattern_idx_      = device_.register_texture(pattern_, sampler_);
        pattern_img_      = device_.register_storage_texture(pattern_, 0);
        return checker_ && pattern_ && sampler_ && checker_idx_ && pattern_idx_ && pattern_img_;
    }

    void shutdown() {
        device_.wait_idle();
        device_.unregister_texture(checker_idx_);
        device_.unregister_texture(pattern_idx_);
        device_.unregister_storage_texture(pattern_img_);
        for (TextureHandle t : { checker_, pattern_, depth_ }) device_.destroy(t);
        for (BufferHandle b : { tri_vb_, quad_vb_, quad_ib_ }) device_.destroy(b);
        for (PipelineHandle p : { tri_pipeline_, quad_pipeline_, compute_pipeline_ }) device_.destroy(p);
        for (ShaderHandle s : { vs_tri_, fs_tri_, vs_quad_, fs_quad_, cs_ }) device_.destroy(s);
        device_.destroy(sampler_);
    }

    void build_ui(f64 fps, f64 frame_ms) {
        ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("Aether Sandbox");
        const DeviceFeatures& f    = device_.features();
        const DeviceInfo      info = device_info(device_);
        ImGui::Text("FPS %.1f  (%.2f ms CPU frame)", fps, frame_ms);
        ImGui::Text("GPU frame %.3f ms, %u draws, %u dispatches", device_.last_frame_stats().gpu_time_ms,
                    device_.last_frame_stats().draw_calls, device_.last_frame_stats().dispatches);
        ImGui::Text("GPU: %s", f.adapter_name.c_str());
        ImGui::Text("Driver: %s %s | Vulkan %s", info.driver_name.c_str(), info.driver_version.c_str(),
                    info.api_version.c_str());
        ImGui::Text("Swapchain: %u images, %s", info.swapchain_images, info.present_mode.c_str());
        bool vs = vsync(device_);
        if (ImGui::Checkbox("VSync", &vs)) {
            set_vsync(device_, vs);
        }
        ImGui::SameLine();
        ImGui::Checkbox("ImGui demo", &show_demo_);
        if (ImGui::CollapsingHeader("Device features", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto row = [](const char* name, bool on) { ImGui::BulletText("%-26s %s", name, on ? "yes" : "no"); };
            row("dynamic rendering", f.dynamic_rendering);
            row("synchronization2", f.synchronization2);
            row("timeline semaphores", f.timeline_semaphores);
            row("descriptor indexing", f.descriptor_indexing);
            row("buffer device address", f.buffer_device_address);
            row("draw indirect count", f.draw_indirect_count);
            row("mesh shaders", f.mesh_shaders);
            row("ray tracing", f.ray_tracing);
            row("wide lines", f.wide_lines);
            row("fill mode non-solid", f.fill_mode_non_solid);
            row("depth clamp", f.depth_clamp);
            row("sampler anisotropy", f.sampler_anisotropy);
            ImGui::BulletText("bindless textures         %u", f.max_bindless_textures);
        }
        const ValidationCounts vc = validation_counts();
        ImGui::Text("Validation: %s  (%u errors, %u warnings)", validation_layer_active() ? "ON" : "off", vc.errors,
                    vc.warnings);
        ImGui::End();
        if (show_demo_) {
            ImGui::ShowDemoWindow(&show_demo_);
        }
    }

    void record(const FrameInfo& frame, f32 time) {
        CommandList& cmd = *frame.cmd;
        ensure_depth(frame.extent);

        SandboxPush push;
        push.vertices    = device_.buffer_device_address(tri_vb_);
        push.angle       = time * 0.8f;
        push.aspect      = static_cast<f32>(frame.extent.x) / static_cast<f32>(std::max(frame.extent.y, 1u));
        push.checker_tex = checker_idx_.index();
        push.pattern_tex = pattern_idx_.index();
        push.pattern_img = pattern_img_.index();
        push.time        = time;

        // Compute pass: animate the storage image, then make it sampleable.
        cmd.push_debug_group("pattern (compute)");
        cmd.barrier(pattern_, pattern_written_ ? ResourceState::ShaderRead : ResourceState::Undefined,
                    ResourceState::ShaderWrite);
        cmd.bind_pipeline(compute_pipeline_);
        cmd.push_constants(ShaderStage::Compute, 0, sizeof(push), &push);
        cmd.dispatch(kPatternSize / 8, kPatternSize / 8, 1);
        cmd.barrier(pattern_, ResourceState::ShaderWrite, ResourceState::ShaderRead);
        pattern_written_ = true;
        cmd.pop_debug_group();

        // Scene pass (swapchain: Undefined -> ColorAttachment per the state contract).
        cmd.push_debug_group("scene");
        cmd.barrier(frame.swapchain_image, ResourceState::Undefined, ResourceState::ColorAttachment);
        cmd.barrier(depth_, ResourceState::Undefined, ResourceState::DepthStencilAttachment);
        const Vec4    clear(0.08f + 0.06f * std::sin(time * 0.7f), 0.10f + 0.05f * std::sin(time * 0.9f + 2.0f),
                            0.16f + 0.06f * std::sin(time * 1.1f + 4.0f), 1.0f);
        RenderingInfo scene;
        scene.color     = { ColorAttachment{ frame.swapchain_image, 0, 0, LoadOp::Clear, StoreOp::Store, clear } };
        scene.has_depth = true;
        scene.depth     = DepthAttachment{ depth_, 0, 0, LoadOp::Clear, StoreOp::DontCare, 0.0f, 0 };
        cmd.begin_rendering(scene);
        cmd.bind_pipeline(quad_pipeline_);
        cmd.push_constants(ShaderStage::AllGraphics, 0, sizeof(push), &push);
        cmd.bind_vertex_buffer(0, quad_vb_);
        cmd.bind_index_buffer(quad_ib_, 0, true);
        cmd.draw_indexed(6);
        cmd.bind_pipeline(tri_pipeline_);
        cmd.push_constants(ShaderStage::AllGraphics, 0, sizeof(push), &push);
        cmd.draw(3);
        cmd.end_rendering();
        cmd.pop_debug_group();

        // UI pass: single colour attachment (the ImGui pipeline has no depth format).
        cmd.push_debug_group("imgui");
        RenderingInfo ui;
        ui.color = { ColorAttachment{ frame.swapchain_image, 0, 0, LoadOp::Load, StoreOp::Store, {} } };
        cmd.begin_rendering(ui);
        imgui_render(cmd);
        cmd.end_rendering();
        cmd.pop_debug_group();
    }

private:
    ShaderHandle load_shader(const char* rel, ShaderStage stage) {
        const auto path = paths::shader_dir() / rel;
        auto       spirv = device_.compile_glsl_file(stage, path);
        if (!spirv) {
            AE_LOG_ERROR("Sandbox", "shader compile failed:\n{}", spirv.error().message);
            return {};
        }
        return device_.create_shader({ stage, std::move(*spirv), "main", rel });
    }

    void ensure_depth(UVec2 extent) {
        if (depth_ && depth_extent_ == extent) {
            return;
        }
        device_.destroy(depth_); // deferred: safe while earlier frames are in flight
        TextureDesc td;
        td.format     = Format::D32F;
        td.width      = extent.x;
        td.height     = extent.y;
        td.usage      = TextureUsage::DepthAttach;
        td.debug_name = "sandbox depth";
        depth_        = device_.create_texture(td);
        depth_extent_ = extent;
    }

    Device&     device_;
    Window&     window_;
    const Args& args_;

    ShaderHandle     vs_tri_, fs_tri_, vs_quad_, fs_quad_, cs_;
    PipelineHandle   tri_pipeline_, quad_pipeline_, compute_pipeline_;
    BufferHandle     tri_vb_, quad_vb_, quad_ib_;
    TextureHandle    checker_, pattern_, depth_;
    UVec2            depth_extent_{ 0, 0 };
    SamplerHandle    sampler_;
    DescriptorHandle checker_idx_, pattern_idx_, pattern_img_;
    bool             pattern_written_ = false;
    bool             show_demo_       = false;
};

// Scripted window events for --test-resize (loop iteration -> action).
void scripted_window_events(Window& window, u64 iteration) {
    auto* w = static_cast<GLFWwindow*>(window.glfw_handle());
    switch (iteration) {
    case 120: glfwSetWindowSize(w, 960, 540); AE_LOG_INFO("Sandbox", "[test] resize 960x540"); break;
    case 200: glfwSetWindowSize(w, 1440, 810); AE_LOG_INFO("Sandbox", "[test] resize 1440x810"); break;
    case 280: glfwIconifyWindow(w); AE_LOG_INFO("Sandbox", "[test] minimize"); break;
    case 340: glfwRestoreWindow(w); AE_LOG_INFO("Sandbox", "[test] restore"); break;
    case 420: glfwSetWindowSize(w, 1280, 720); AE_LOG_INFO("Sandbox", "[test] resize 1280x720"); break;
    default: break;
    }
}

int run(const Args& args) {
    std::unique_ptr<Window> window = Window::create({ "Aether Sandbox", 1280, 720, true, args.vsync });
    if (!window) {
        return 1;
    }
    DeviceDesc dd;
    dd.window            = window.get();
    dd.enable_validation = true;
    dd.vsync             = args.vsync;
    auto created         = create_device(dd);
    if (!created) {
        AE_LOG_ERROR("Sandbox", "create_device failed: {}", created.error().message);
        return 1;
    }
    std::unique_ptr<Device> device = std::move(*created);
    window->set_resize_callback([&](u32 w, u32 h) { device->on_resize(w, h); });
    imgui_init(*device, *window);

    int  exit_code = 0;
    auto sandbox   = std::make_unique<Sandbox>(*device, *window, args);
    if (!sandbox->init()) {
        AE_LOG_ERROR("Sandbox", "initialization failed");
        exit_code = 1;
    } else {
        const f64 start      = now_seconds();
        f64       last       = start;
        f64       fps        = 0.0;
        f64       frame_ms   = 0.0;
        u64       iterations = 0;
        u64       rendered   = 0;
        while (!window->should_close()) {
            window->poll_events();
            if (args.frames > 0 && iterations >= args.frames) {
                break;
            }
            if (args.test_resize) {
                scripted_window_events(*window, iterations);
            }
            ++iterations;

            const f64 now = now_seconds();
            const f64 dt  = now - last;
            last          = now;
            frame_ms      = frame_ms * 0.9 + dt * 1000.0 * 0.1;
            fps           = frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0;

            if (window->minimized()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16)); // don't spin
                continue;
            }
            imgui_new_frame();
            sandbox->build_ui(fps, frame_ms);
            const FrameInfo frame = device->begin_frame();
            if (!frame.valid) {
                continue; // swapchain out of date / zero-sized: retry next iteration
            }
            sandbox->record(frame, static_cast<f32>(now - start));
            device->end_frame(frame);
            ++rendered;
        }
        AE_LOG_INFO("Sandbox", "{} loop iterations, {} frames rendered", iterations, rendered);
    }
    sandbox->shutdown();
    sandbox.reset();
    imgui_shutdown(*device);
    device.reset();
    window.reset();
    return exit_code;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    int        code = run(args);

    const ValidationCounts vc    = validation_counts();
    const LeakReport       leaks = last_leak_report();
    AE_LOG_INFO("Sandbox", "validation: {} ({} errors, {} warnings); leaks: {} resources, {} allocations",
                validation_layer_active() ? "active" : "not active", vc.errors, vc.warnings, leaks.resources,
                leaks.allocations);
    if (vc.total() > 0 || leaks.resources > 0 || leaks.allocations > 0) {
        AE_LOG_ERROR("Sandbox", "FAILED: validation messages or leaks reported");
        code = 1;
    }
    return code;
}
