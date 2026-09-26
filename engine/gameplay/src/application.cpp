// application.cpp — gameplay::Application: services, default engine assembly and the frame loop
// (see application.h for the frame timeline).
#include "aether/gameplay/application.h"

#include "aether/assets/asset_manager.h"
#include "aether/core/input.h"
#include "aether/core/job_system.h"
#include "aether/core/log.h"
#include "aether/core/paths.h"
#include "aether/gameplay/animation_bridge.h"
#include "aether/gameplay/asset_hot_reload.h"
#include "aether/gameplay/component_codecs.h"
#include "aether/gameplay/fixed_step.h"
#include "aether/gameplay/input_subsystem.h"
#include "aether/gameplay/render_bridge.h"
#include "aether/physics/components.h"
#include "aether/physics/physics_subsystem.h"
#include "aether/renderer/renderer.h"
#include "aether/rhi/device.h"
#include "aether/rhi/imgui.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/world.h"

#if AE_WITH_ANIMATION
#    include "aether/animation/animation_subsystem.h"
#    include "aether/animation/components.h"
#endif
#if AE_WITH_SCRIPTING
#    include "aether/scripting/scripting_subsystem.h"
#endif
#include "lua_bindings.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace aether::gameplay {

namespace {
constexpr f32 kMaxFrameDelta = 0.25f; // clamp for breakpoints / hitches (seconds)
}

struct Application::Impl {
    AppDesc desc;

    std::unique_ptr<Window>               window;
    std::unique_ptr<rhi::Device>          device;
    std::unique_ptr<renderer::Renderer>   renderer;
    std::unique_ptr<assets::AssetManager> assets;
    std::unique_ptr<World>                world;
    EngineContext                         engine{};
    FrameTime                             time{};

    struct Entry {
        std::unique_ptr<ISubsystem> subsystem;
        SubsystemKind               kind    = SubsystemKind::Engine;
        bool                        started = false;
    };
    std::vector<Entry>       entries;
    std::vector<ISubsystem*> raw; // parallel to entries (subsystems() view)

    FixedStepAccumulator accumulator;
    u64                  fixed_step_index = 0;
    bool                 simulation       = true;
    bool                 step_once        = false;

    std::optional<CameraOverride> camera_override;
    std::optional<UVec2>          render_extent;
    UVec2                         renderer_size{ 0, 0 };
    renderer::RenderScene         scene;

    bool initialized     = false;
    bool owns_job_system = false;
    bool imgui           = false;
    bool exit_requested  = false;
    int  exit_code       = 0;

    // Default subsystems (null when absent).
    InputSubsystem*            input_sys   = nullptr;
    physics::PhysicsSubsystem* physics_sys = nullptr;
    RenderBridgeSubsystem*     bridge_sys  = nullptr;
#if AE_WITH_SCRIPTING
    scripting::ScriptingSubsystem* scripting_sys = nullptr;
#endif

    void start(Entry& e) {
        if (!e.started) {
            e.subsystem->on_startup(engine);
            e.started = true;
        }
    }
    void stop(Entry& e) {
        if (e.started) {
            e.subsystem->on_shutdown();
            e.started = false;
        }
    }
};

// =================================================================================================
// construction / services
// =================================================================================================
Application::Application(AppDesc desc) : impl_(std::make_unique<Impl>()) {
    impl_->desc = std::move(desc);
    impl_->accumulator.configure(impl_->desc.fixed_delta, std::max(impl_->desc.max_fixed_steps, 1u));
}

Application::~Application() {
    // run() normally tears everything down; this covers initialize() failures and apps that
    // never ran the loop.
    if (impl_->device) {
        impl_->device->wait_idle();
    }
    for (auto it = impl_->entries.rbegin(); it != impl_->entries.rend(); ++it) {
        impl_->stop(*it);
    }
    impl_->entries.clear();
    impl_->raw.clear();
    impl_->world.reset();
    if (impl_->assets) {
        impl_->assets->shutdown();
    }
    impl_->assets.reset();
    impl_->renderer.reset();
    if (impl_->imgui && impl_->device) {
        rhi::imgui_shutdown(*impl_->device);
        impl_->imgui = false;
    }
    impl_->device.reset();
    impl_->window.reset();
    if (impl_->owns_job_system) {
        JobSystem::shutdown();
        impl_->owns_job_system = false;
    }
}

std::span<ISubsystem* const> Application::subsystems() const { return impl_->raw; }
Window&                      Application::window() { return *impl_->window; }
rhi::Device&                 Application::device() { return *impl_->device; }
renderer::Renderer&          Application::renderer() { return *impl_->renderer; }
assets::AssetManager&        Application::assets() { return *impl_->assets; }
World&                       Application::world() { return *impl_->world; }
EngineContext&               Application::engine_context() { return impl_->engine; }
const FrameTime&             Application::time() const { return impl_->time; }
const AppDesc&               Application::desc() const { return impl_->desc; }

void Application::add_subsystem_impl(std::unique_ptr<ISubsystem> subsystem, SubsystemKind kind) {
    impl_->entries.push_back(Impl::Entry{ std::move(subsystem), kind, false });
    impl_->raw.push_back(impl_->entries.back().subsystem.get());
    if (impl_->initialized) {
        impl_->start(impl_->entries.back());
    }
}

// =================================================================================================
// initialize
// =================================================================================================
Result<void> Application::initialize() {
    Impl& s = *impl_;
    if (s.initialized) {
        return {};
    }
    if (JobSystem::worker_count() == 0) {
        JobSystem::initialize();
        s.owns_job_system = true;
    }

    // ---- window + device ------------------------------------------------------------------------
    s.desc.window.vsync = s.desc.vsync;
    s.window            = Window::create(s.desc.window);
    if (!s.window) {
        return Error{ ErrorCode::Unsupported, "failed to create the window" };
    }
    rhi::DeviceDesc dd;
    dd.window            = s.window.get();
    dd.enable_validation = s.desc.enable_validation;
    dd.vsync             = s.desc.vsync;
    dd.frames_in_flight  = std::max(s.desc.frames_in_flight, 1u);
    auto device          = rhi::create_device(dd);
    if (!device) {
        return device.error();
    }
    s.device = std::move(*device);
    s.window->set_resize_callback([dev = s.device.get()](u32 w, u32 h) { dev->on_resize(w, h); });
    if (s.desc.imgui) {
        rhi::imgui_init(*s.device, *s.window);
        s.imgui = true;
    }

    // ---- renderer ---------------------------------------------------------------------------------
    renderer::RendererDesc rd;
    rd.device      = s.device.get();
    rd.output_size = UVec2(std::max(s.window->width(), 1u), std::max(s.window->height(), 1u));
    auto r         = renderer::Renderer::create(rd);
    if (!r) {
        return r.error();
    }
    s.renderer      = std::move(*r);
    s.renderer_size = rd.output_size;
    // Software rasterizers (CI / headless verification): Mesa llvmpipe 25.x crashes inside its
    // JIT when sampling the cascaded shadow map (bindless depth-array compare sampling, a null
    // sample-function table) - keep everything else enabled and turn shadows off.
    {
        const std::string& adapter = s.device->features().adapter_name;
        if (adapter.find("llvmpipe") != std::string::npos || adapter.find("lavapipe") != std::string::npos ||
            adapter.find("SwiftShader") != std::string::npos) {
            s.renderer->settings().shadows = false;
            AE_LOG_WARN("App", "software rasterizer '{}': cascaded shadows disabled (driver workaround)", adapter);
        }
    }

    // ---- assets -----------------------------------------------------------------------------------
    s.assets = std::make_unique<assets::AssetManager>();
    assets::AssetManagerConfig ac;
    ac.content_root = s.desc.content_root.empty() ? paths::content_dir() : s.desc.content_root;
    if (auto a = s.assets->initialize(ac); !a) {
        return a.error();
    }

    // ---- world ------------------------------------------------------------------------------------
    s.world = std::make_unique<World>();
    register_default_codecs(*s.world);
    s.engine.window   = s.window.get();
    s.engine.world    = s.world.get();
    s.engine.renderer = s.renderer.get();

    // ---- default engine ---------------------------------------------------------------------------
    if (s.desc.default_subsystems) {
        s.input_sys = &add_subsystem<InputSubsystem>(SubsystemKind::Engine);
        add_subsystem<AnimationAssetBinder>(SubsystemKind::Engine, s.assets.get());
        s.physics_sys = &add_subsystem<physics::PhysicsSubsystem>(SubsystemKind::Simulation);
#if AE_WITH_SCRIPTING
        scripting::ScriptVMConfig sc;
        sc.script_root  = ac.content_root / "scripts";
        s.scripting_sys = &add_subsystem<scripting::ScriptingSubsystem>(SubsystemKind::Simulation, sc);
        s.scripting_sys->add_binding_registrar([&s](scripting::ScriptVM& vm) {
            register_gameplay_lua_bindings(vm, LuaBindingContext{ s.physics_sys, s.input_sys });
        });
#endif
#if AE_WITH_ANIMATION
        add_subsystem<animation::AnimationSubsystem>(SubsystemKind::Simulation);
#endif
        add_subsystem<AssetHotReloadSubsystem>(SubsystemKind::Engine, s.assets.get());
        s.bridge_sys = &add_subsystem<RenderBridgeSubsystem>(SubsystemKind::Engine, s.assets.get());

        // Physics: interpolated poses for moving bodies; debug lines into the scene.
        s.bridge_sys->set_world_matrix_hook([&s](Entity e, Mat4& out) {
            physics::PhysicsWorld* pw = s.physics_sys != nullptr ? s.physics_sys->physics_world() : nullptr;
            if (pw == nullptr || !s.simulation) {
                return false;
            }
            const World& w  = *s.world;
            const auto*  rb = w.try_get<physics::RigidBodyComponent>(e);
            const bool   moving =
                (rb != nullptr && rb->motion_type == physics::MotionType::Dynamic && pw->has_body(e)) ||
                (w.has<physics::CharacterControllerComponent>(e) && pw->has_character(e));
            if (!moving) {
                return false;
            }
            out = pw->interpolated_world_matrix(e, s.physics_sys->interpolation_alpha());
            return true;
        });
        s.bridge_sys->set_post_extract_hook([&s](renderer::RenderScene& scene) {
            if (s.physics_sys == nullptr) {
                return;
            }
            for (const physics::DebugLine& l : s.physics_sys->debug_lines()) {
                scene.debug_lines.push_back(renderer::RenderLine{ l.a, l.b, l.rgba, l.rgba });
            }
        });
#if AE_WITH_ANIMATION
        // Skinning palette: the entity's own animator, else the nearest animated ancestor.
        s.bridge_sys->set_palette_hook([&s](Entity e) -> std::span<const Mat4> {
            const World& w = *s.world;
            for (Entity cur = e; cur != kNullEntity; cur = scene::parent_of(w, cur)) {
                if (const auto* a = w.try_get<animation::AnimatorComponent>(cur); a != nullptr && a->skeleton()) {
                    return a->palette();
                }
            }
            return {};
        });
#endif
    }

    s.initialized = true;
    for (Impl::Entry& e : s.entries) {
        s.start(e);
    }

    // ---- startup scene ------------------------------------------------------------------------------
    if (!s.desc.startup_scene.empty()) {
        const std::filesystem::path file = s.desc.startup_scene.is_absolute()
                                               ? s.desc.startup_scene
                                               : ac.content_root / s.desc.startup_scene;
        auto loaded = reload_world([&](World& w) -> Result<void> {
            auto r2 = scene::load_scene(w, file);
            if (!r2) {
                return r2.error();
            }
            AE_LOG_INFO("App", "loaded scene {} ({} entities, {} warnings)", file.generic_string(),
                        r2->entity_count, r2->warning_count);
            return {};
        });
        if (!loaded) {
            return loaded.error();
        }
    }

    if (auto init = on_init(); !init) {
        return init;
    }
    AE_LOG_INFO("App", "initialized: {} subsystems", s.entries.size());
    return {};
}

// =================================================================================================
// run
// =================================================================================================
int Application::run() {
    Impl& s = *impl_;
    if (!s.initialized) {
        AE_LOG_ERROR("App", "run() called before a successful initialize()");
        return 1;
    }
    const f64 start = now_seconds();
    f64       last  = start;
    s.time          = FrameTime{};
    s.time.fixed_delta = s.accumulator.fixed_delta();

    while (!s.exit_requested) {
        s.window->poll_events();
        if (s.window->should_close()) {
            break;
        }
        if (s.desc.max_frames != 0 && s.time.frame_index >= s.desc.max_frames) {
            break;
        }

        // ---- time ----
        const f64 now       = now_seconds();
        const f32 raw       = static_cast<f32>(now - last);
        last                = now;
        s.time.total        = now - start;
        s.time.unscaled_delta = raw;
        s.time.delta        = std::clamp(raw, 0.0f, kMaxFrameDelta);
        if (s.time.delta > 0.0f) {
            const f32 fps = 1.0f / s.time.delta;
            s.time.fps_smoothed = s.time.fps_smoothed <= 0.0f ? fps : s.time.fps_smoothed * 0.95f + fps * 0.05f;
        }
        FrameContext frame_ctx{ &s.engine, s.time };

        for (ISubsystem* sub : s.raw) {
            sub->on_begin_frame(frame_ctx);
        }

        // ---- fixed steps ----
        u32 steps = s.accumulator.advance(s.simulation ? s.time.delta : 0.0f);
        if (!s.simulation && s.step_once) {
            steps = 1;
        }
        s.step_once = false;
        for (u32 i = 0; i < steps; ++i) {
            FixedContext fixed{ &s.engine, s.accumulator.fixed_delta(), s.fixed_step_index++ };
            for (Impl::Entry& e : s.entries) {
                if (e.kind == SubsystemKind::Simulation) {
                    e.subsystem->on_fixed_update(fixed);
                }
            }
        }
#if AE_WITH_SCRIPTING
        if (s.scripting_sys != nullptr && s.physics_sys != nullptr && s.scripting_sys->vm() != nullptr && steps > 0) {
            dispatch_contact_events(*s.scripting_sys->vm(), *s.physics_sys);
        }
#endif

        // ---- variable update ----
        for (Impl::Entry& e : s.entries) {
            if (e.kind == SubsystemKind::Simulation && !s.simulation) {
                continue;
            }
            e.subsystem->on_update(frame_ctx);
        }
        if (s.physics_sys != nullptr) {
            s.physics_sys->set_interpolation_alpha(s.simulation ? s.accumulator.alpha() : 1.0f);
        }
        on_update(s.time);
        s.world->update_transforms();

        if (s.window->minimized()) {
            for (ISubsystem* sub : s.raw) {
                sub->on_end_frame(frame_ctx);
            }
            ++s.time.frame_index;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        // ---- UI ----
        if (s.imgui) {
            rhi::imgui_new_frame();
            on_imgui();
        }

        // ---- render scene ----
        const UVec2 extent = s.render_extent.value_or(UVec2(s.window->width(), s.window->height()));
        if (s.bridge_sys != nullptr) {
            s.bridge_sys->set_viewport(UVec2(std::max(extent.x, 1u), std::max(extent.y, 1u)));
            s.bridge_sys->set_camera_override(s.camera_override);
        }
        s.scene.clear();
        RenderContext render_ctx{ &s.engine, &s.scene, s.time };
        for (ISubsystem* sub : s.raw) {
            sub->on_render(render_ctx);
        }

        rhi::FrameInfo frame = s.device->begin_frame();
        if (frame.valid) {
            on_render_frame(frame, s.scene);
            s.device->end_frame(frame);
        } else if (s.imgui && ImGui::GetCurrentContext() != nullptr) {
            ImGui::EndFrame(); // nothing was rendered: close the UI frame cleanly
        }

        for (ISubsystem* sub : s.raw) {
            sub->on_end_frame(frame_ctx);
        }
        ++s.time.frame_index;
    }

    AE_LOG_INFO("App", "exiting after {} frames ({:.2f} s)", s.time.frame_index, now_seconds() - start);
    on_shutdown();
    s.device->wait_idle();
    for (auto it = s.entries.rbegin(); it != s.entries.rend(); ++it) {
        s.stop(*it);
    }
    s.entries.clear();
    s.raw.clear();
    s.input_sys   = nullptr;
    s.physics_sys = nullptr;
    s.bridge_sys  = nullptr;
#if AE_WITH_SCRIPTING
    s.scripting_sys = nullptr;
#endif
    s.world.reset();
    s.engine.world = nullptr;
    s.assets->shutdown();
    s.renderer.reset();
    s.engine.renderer = nullptr;
    if (s.imgui) {
        rhi::imgui_shutdown(*s.device);
        s.imgui = false;
    }
    s.device.reset();
    s.window.reset();
    s.engine.window = nullptr;
    if (s.owns_job_system) {
        JobSystem::shutdown();
        s.owns_job_system = false;
    }
    s.initialized = false;
    return s.exit_code;
}

void Application::request_exit(int exit_code) {
    impl_->exit_requested = true;
    impl_->exit_code      = exit_code;
}

// =================================================================================================
// simulation control
// =================================================================================================
void Application::set_simulation_enabled(bool enabled) {
    if (enabled && !impl_->simulation) {
        impl_->accumulator.reset(); // no catch-up burst after a pause
    }
    impl_->simulation = enabled;
}

bool Application::simulation_enabled() const { return impl_->simulation; }

void Application::step_simulation_once() { impl_->step_once = true; }

Result<void> Application::reload_world(const std::function<Result<void>(World&)>& loader) {
    Impl& s = *impl_;
    if (!s.world) {
        return Error{ ErrorCode::NotInitialized, "reload_world: no world" };
    }
    for (auto it = s.entries.rbegin(); it != s.entries.rend(); ++it) {
        if (it->kind == SubsystemKind::Simulation) {
            s.stop(*it);
        }
    }
    s.world->clear();
    Result<void> result = loader ? loader(*s.world) : Result<void>{};
    s.world->update_transforms();
    if (s.initialized) {
        for (Impl::Entry& e : s.entries) {
            if (e.kind == SubsystemKind::Simulation) {
                s.start(e);
            }
        }
    }
    s.accumulator.reset();
    return result;
}

void Application::set_camera_override(std::optional<CameraOverride> camera) {
    impl_->camera_override = std::move(camera);
}

const std::optional<CameraOverride>& Application::camera_override() const { return impl_->camera_override; }

void Application::set_render_extent(std::optional<UVec2> extent) { impl_->render_extent = extent; }

// =================================================================================================
// default frame recording
// =================================================================================================
void Application::on_render_frame(rhi::FrameInfo& frame, renderer::RenderScene& scene) {
    Impl&       s      = *impl_;
    const UVec2 extent = frame.extent;
    if (extent != s.renderer_size && extent.x > 0 && extent.y > 0) {
        s.renderer->resize(extent);
        s.renderer_size = extent;
    }
    scene.view.viewport = extent;

    renderer::RenderTarget target;
    target.texture       = frame.swapchain_image;
    target.format        = frame.swapchain_format;
    target.extent        = extent;
    target.initial_state = rhi::ResourceState::Undefined;
    target.final_state   = rhi::ResourceState::ColorAttachment;
    s.renderer->render(scene, *frame.cmd, target);
    if (s.imgui) {
        render_imgui_overlay(frame, false);
    }
}

void Application::render_imgui_overlay(rhi::FrameInfo& frame, bool clear_first) {
    if (!impl_->imgui) {
        return;
    }
    rhi::CommandList& cmd = *frame.cmd;
    if (clear_first) {
        cmd.barrier(frame.swapchain_image, rhi::ResourceState::Undefined, rhi::ResourceState::ColorAttachment);
    }
    rhi::RenderingInfo ui;
    ui.color = { rhi::ColorAttachment{ frame.swapchain_image, 0, 0,
                                       clear_first ? rhi::LoadOp::Clear : rhi::LoadOp::Load, rhi::StoreOp::Store,
                                       Vec4(0.0f, 0.0f, 0.0f, 1.0f) } };
    cmd.push_debug_group("imgui");
    cmd.begin_rendering(ui);
    rhi::imgui_render(cmd);
    cmd.end_rendering();
    cmd.pop_debug_group();
}

} // namespace aether::gameplay
