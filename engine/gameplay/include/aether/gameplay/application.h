// aether/gameplay/application.h — the engine application: owns every service and the
// frame loop, assembles the default engine (physics, animation, scripting, render bridge).
//
// FROZEN CONTRACT (ADR-0003). gameplay is the engine ASSEMBLY layer (Layer 4b): it may
// depend on every Layer 1-4a module (core, rhi, renderer, scene, assets, physics,
// animation, scripting); nothing below it may depend on it. The editor, the runtime
// player and the samples (Layer 5) derive from Application.
//
// Frame timeline (blueprint §5.1):
//   poll events -> begin_frame(all) -> fixed_update x N (simulation subsystems, if enabled)
//   -> update(all*) -> on_update hook -> ImGui: new_frame + on_imgui hook
//   -> build RenderScene: on_render(all) -> device.begin_frame -> on_render_frame hook
//   -> device.end_frame -> end_frame(all)
//   (* simulation subsystems skip update while simulation is disabled, e.g. editor paused)
#pragma once

#include "aether/core/error.h"
#include "aether/core/math.h"
#include "aether/core/subsystem.h"
#include "aether/core/time.h"
#include "aether/core/types.h"
#include "aether/core/window.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace aether {
class World;
}
namespace aether::rhi {
class Device;
struct FrameInfo;
} // namespace aether::rhi
namespace aether::renderer {
class Renderer;
struct RenderScene;
} // namespace aether::renderer
namespace aether::assets {
class AssetManager;
}

namespace aether::gameplay {

struct AppDesc {
    WindowDesc            window{};
    bool                  enable_validation = true;
    bool                  vsync = true;
    bool                  imgui = true;             // create the ImGui context + overlay pass
    u32                   frames_in_flight = 2;
    f32                   fixed_delta = 1.0f / 60.0f;
    u32                   max_fixed_steps = 8;      // per frame; spiral-of-death clamp
    bool                  default_subsystems = true; // physics, animation, scripting, render bridge
    std::filesystem::path content_root;             // empty => paths::content_dir()
    std::filesystem::path startup_scene;            // optional .aescene, relative to content_root
    u64                   max_frames = 0;           // 0 = unlimited; automation exits after N frames
    // ADR-0006 (additive): shipping builds load cooked data only (AssetLoadMode::Runtime: no
    // source import, no hot reload). cooked_root: empty => paths::asset_dir().
    bool                  cooked_assets_only = false;
    std::filesystem::path cooked_root;
};

// Parses the shared CLI flags into `defaults`: --frames N, --scene <path>, --no-validation,
// --no-vsync, --width W, --height H, --title T. Unknown flags are ignored (apps add their own).
AppDesc parse_command_line(int argc, char** argv, AppDesc defaults = {});

// Camera used for the frame when set (the editor's viewport camera); otherwise the render
// bridge uses the ECS entity with a primary CameraComponent.
struct CameraOverride {
    Mat4 view{ 1.0f };
    f32  fov_y_radians = 1.0472f; // 60 degrees
    f32  near_z = 0.1f;
    f32  far_z = 1000.0f;
};

enum class SubsystemKind : u8 {
    Simulation = 0, // gameplay/physics/animation/scripting: paused with the simulation
    Engine,         // always ticks: render bridge, asset hot reload, tooling
};

class Application {
public:
    explicit Application(AppDesc desc);
    virtual ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Creates window, device, renderer, asset manager and world; registers the default
    // subsystems (if desc.default_subsystems); loads desc.startup_scene; then on_init().
    // Main thread only. Never throws; returns the first failure.
    Result<void> initialize();

    // Runs the frame loop until request_exit() / window close / max_frames, then shuts
    // everything down in reverse order (calling on_shutdown() first). Returns exit code.
    int  run();
    void request_exit(int exit_code = 0);

    // ---- subsystems (main thread). Ticked in insertion order; shut down in reverse. ----
    template <class T, class... Args>
    T& add_subsystem(SubsystemKind kind, Args&&... args) {
        auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
        T&   ref = *ptr;
        add_subsystem_impl(std::move(ptr), kind);
        return ref;
    }
    template <class T>
    T* find_subsystem() const {
        for (ISubsystem* s : subsystems()) {
            if (auto* t = dynamic_cast<T*>(s)) {
                return t;
            }
        }
        return nullptr;
    }
    [[nodiscard]] std::span<ISubsystem* const> subsystems() const;

    // ---- services (valid after initialize()) ----
    [[nodiscard]] Window&               window();
    [[nodiscard]] rhi::Device&          device();
    [[nodiscard]] renderer::Renderer&   renderer();
    [[nodiscard]] assets::AssetManager& assets();
    [[nodiscard]] World&                world();
    [[nodiscard]] EngineContext&        engine_context();
    [[nodiscard]] const FrameTime&      time() const;
    [[nodiscard]] const AppDesc&        desc() const;

    // ---- simulation control (editor play / pause / step) ----
    void set_simulation_enabled(bool enabled);
    [[nodiscard]] bool simulation_enabled() const;
    void step_simulation_once(); // runs exactly one fixed step while paused

    // Replace the world's contents safely: shuts down Simulation subsystems (reverse order),
    // clears the world, runs `loader`, then starts them again. Used by scene loading and
    // play-in-editor restore. Main thread only.
    Result<void> reload_world(const std::function<Result<void>(World&)>& loader);

    void set_camera_override(std::optional<CameraOverride> camera);
    [[nodiscard]] const std::optional<CameraOverride>& camera_override() const;

    // Size the renderer renders at (defaults to the window framebuffer). The editor sets
    // this to its viewport panel size.
    void set_render_extent(std::optional<UVec2> extent);

protected:
    // Hooks for derived applications. Defaults do nothing except on_render_frame().
    virtual Result<void> on_init() { return {}; }
    virtual void         on_shutdown() {}
    virtual void         on_update(const FrameTime&) {}
    virtual void         on_imgui() {} // called between ImGui new_frame and render, if imgui

    // Record the frame into frame.cmd. Default: renderer().render(scene, cmd, swapchain
    // target Undefined -> ColorAttachment), then (if imgui) an overlay pass that draws ImGui
    // on top with LoadOp::Load. The editor overrides this to render into its viewport texture.
    virtual void on_render_frame(rhi::FrameInfo& frame, renderer::RenderScene& scene);

    // Helper for overrides: the default ImGui overlay pass on the swapchain image.
    void render_imgui_overlay(rhi::FrameInfo& frame, bool clear_first);

private:
    void add_subsystem_impl(std::unique_ptr<ISubsystem> subsystem, SubsystemKind kind);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aether::gameplay
