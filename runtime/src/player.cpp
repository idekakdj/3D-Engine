// player.cpp — the runtime player application (see aether/runtime/player.h).
#include "aether/runtime/player.h"

#include "aether/core/log.h"
#include "aether/gameplay/camera_controller.h"
#include "aether/gameplay/input_subsystem.h"
#include "aether/gameplay/render_bridge.h"
#include "aether/physics/components.h"
#include "aether/physics/physics_subsystem.h"
#include "aether/physics/physics_world.h"
#include "aether/scene/components.h"
#include "aether/scene/world.h"
#include "aether/scripting/components.h"
#include "aether/scripting/script_vm.h"
#include "aether/scripting/scripting_subsystem.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <format>
#include <utility>

namespace aether::runtime {

using namespace aether::gameplay;

PlayerApp::PlayerApp(AppDesc desc, PlayerOptions options) : Application(std::move(desc)), options_(std::move(options)) {}

PlayerApp::~PlayerApp() = default;

Result<void> PlayerApp::on_init() {
    const ProjectDesc& p = options_.project;

    // ---- input ----------------------------------------------------------------------------------
    auto* input = find_subsystem<InputSubsystem>();
    if (input == nullptr) {
        return Error{ ErrorCode::NotInitialized, "the player needs the default subsystems (input)" };
    }
    InputMap& map = input->input_map();
    if (!p.input_map.empty()) {
        const std::filesystem::path file = p.input_map.is_absolute() ? p.input_map : desc().content_root / p.input_map;
        auto loaded = map.load_file(file);
        if (!loaded) {
            return Error{ loaded.error().code, std::format("input map: {}", loaded.error().message) };
        }
        AE_LOG_INFO("Player", "input map {} ({} unknown inputs skipped)", file.generic_string(), *loaded);
    } else {
        map.add_default_camera_bindings();
    }
    if (!map.has_action(kQuitAction)) {
        map.bind_action(kQuitAction, Key::Escape);
    }
    if (!map.has_action(kFullscreenAction)) {
        map.bind_action(kFullscreenAction, Key::F11);
    }
    if (p.camera_controller) {
        // Simulation kind: the game camera moves with the game (and stops if it is paused).
        add_subsystem<CameraControllerSubsystem>(SubsystemKind::Simulation, input);
    }

    // ---- rendering ------------------------------------------------------------------------------
    if (auto* bridge = find_subsystem<RenderBridgeSubsystem>()) {
        bridge->set_exposure(p.exposure);
        if (!p.sky) {
            bridge->set_environment(renderer::EnvironmentSettings{}); // flat ambient, no skybox
        }
    }
    if (p.fullscreen) {
        set_fullscreen(true);
    }

    World& w = world();
    if (find_primary_camera(w) == kNullEntity) {
        AE_LOG_WARN("Player", "the scene has no camera: rendering from the default view");
    }
    AE_LOG_INFO("Player", "running '{}' ({} entities)", p.name, w.entity_count());
    return {};
}

void PlayerApp::on_update(const FrameTime&) {
    ++frames_;
    if (auto* bridge = find_subsystem<RenderBridgeSubsystem>()) {
        max_instances_ = std::max(max_instances_, bridge->last_stats().instances);
    }
    auto* input = find_subsystem<InputSubsystem>();
    if (input == nullptr) {
        return;
    }
    const InputMap& map = input->input_map();
    if (map.action_pressed(kQuitAction)) {
        request_exit(0);
    }
    if (map.action_pressed(kFullscreenAction)) {
        set_fullscreen(!fullscreen_);
    }
}

void PlayerApp::set_fullscreen(bool enable) {
    auto* win = static_cast<GLFWwindow*>(window().glfw_handle());
    if (win == nullptr || enable == fullscreen_) {
        return;
    }
    if (enable) {
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = monitor != nullptr ? glfwGetVideoMode(monitor) : nullptr;
        if (mode == nullptr) {
            AE_LOG_WARN("Player", "fullscreen unavailable (no primary monitor)");
            return;
        }
        glfwGetWindowPos(win, &windowed_[0], &windowed_[1]);
        glfwGetWindowSize(win, &windowed_[2], &windowed_[3]);
        glfwSetWindowMonitor(win, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(win, nullptr, windowed_[0], windowed_[1], windowed_[2], windowed_[3], GLFW_DONT_CARE);
    }
    fullscreen_ = enable;
}

void PlayerApp::on_shutdown() {
    if (options_.check) {
        run_checks();
        AE_LOG_INFO("Player", "self-check {}", check_passed_ ? "PASSED" : "FAILED");
    }
}

void PlayerApp::fail(std::string message) {
    AE_LOG_ERROR("Player", "check failed: {}", message);
    failures_.push_back(std::move(message));
    check_passed_ = false;
}

void PlayerApp::run_checks() {
    World& w = world();
    if (frames_ == 0) {
        fail("no frames ran");
    }
    if (!options_.project.startup_scene.empty() && w.entity_count() == 0) {
        fail("the startup scene is empty");
    }

    // Rendering: a camera looked at the scene and every asset resolved from the (cooked) data.
    auto* bridge = find_subsystem<RenderBridgeSubsystem>();
    if (bridge == nullptr || bridge->cache() == nullptr) {
        fail("render bridge missing");
    } else {
        const SceneExtractStats& s = bridge->last_stats();
        if (s.camera == kNullEntity && find_primary_camera(w) != kNullEntity) {
            fail("the scene camera was not used");
        }
        if (w.view<MeshRendererComponent>().size() > 0 && max_instances_ == 0) {
            fail("nothing was rendered");
        }
        if (s.pending_meshes != 0) {
            fail(std::format("{} mesh renderers have no mesh (loading or failed)", s.pending_meshes));
        }
        const RenderCacheStats cs = bridge->cache()->stats();
        if (cs.failed != 0) {
            fail(std::format("{} assets failed to load", cs.failed));
        }
        AE_LOG_INFO("Player", "rendered {} instances, {} lights; cache {} meshes / {} materials / {} textures",
                    s.instances, s.lights, cs.meshes, cs.materials, cs.textures);
    }

    // Physics: every rigid body made it into the physics world.
    const usize bodies = w.view<physics::RigidBodyComponent, physics::ColliderComponent>().size_hint();
    if (bodies > 0) {
        auto* physics = find_subsystem<physics::PhysicsSubsystem>();
        if (physics == nullptr || physics->physics_world() == nullptr || physics->physics_world()->body_count() == 0) {
            fail("physics world missing or empty");
        }
    }

    // Scripts: every enabled script is running and none reported an error.
    auto* scripting = find_subsystem<scripting::ScriptingSubsystem>();
    if (scripting != nullptr && scripting->vm() != nullptr) {
        scripting::ScriptVM& vm = *scripting->vm();
        for (const Entity e : w.view<scripting::ScriptComponent>()) {
            if (!w.get<scripting::ScriptComponent>(e).enabled) {
                continue;
            }
            if (vm.instance_state(e) != scripting::ScriptInstanceState::Running) {
                const auto* name = w.try_get<NameComponent>(e);
                fail(std::format("script on '{}' is not running: {}", name != nullptr ? name->name : "?",
                                 vm.instance_error(e)));
            }
        }
        if (vm.error_count() != 0) {
            fail(std::format("{} script errors", vm.error_count()));
        }
    } else if (w.view<scripting::ScriptComponent>().size() > 0) {
        fail("scripting VM missing");
    }
}

} // namespace aether::runtime
