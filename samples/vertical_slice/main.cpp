// vertical_slice — the M1 artifact (blueprint §14) on top of gameplay::Application:
//   * clustered-forward PBR scene: sun (shadows) + point light, built-in meshes, runtime materials
//   * Jolt physics: a floor and a falling stack of boxes and spheres
//   * the glTF cube sample (textured) and the skinned sample (playing its clip)
//   * Lua scripts: a rotator, a bobber, a spawner and an impulse launcher using `physics.*`
//   * a fly camera (hold RMB + WASD/QE, Shift to boost) and the engine stats panel
//
// Usage: vertical_slice [--frames N] [--check] [--save <file.aescene>] [shared app flags]
//   --check  after the run, verify that physics simulated, scripts ran without errors and the
//            animation played; exit code 1 otherwise (automated verification, CI).
//   --save   write the constructed scene to a file (editor / --scene round-trip testing).
#include "aether/core/log.h"
#include "aether/gameplay/application.h"
#include "aether/gameplay/camera_controller.h"
#include "aether/gameplay/component_codecs.h"
#include "aether/gameplay/debug_ui.h"
#include "aether/gameplay/environment.h"
#include "aether/gameplay/input_subsystem.h"
#include "aether/gameplay/procedural_mesh.h"
#include "aether/gameplay/render_bridge.h"
#include "aether/gameplay/scene_instantiation.h"
#include "aether/physics/components.h"
#include "aether/physics/physics_subsystem.h"
#include "aether/renderer/renderer.h"
#include "aether/scene/components.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"
#include "aether/scripting/components.h"
#include "aether/scripting/scripting_subsystem.h"
#include "aether/animation/animation_subsystem.h"

#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::gameplay;

namespace {

struct SliceArgs {
    bool        check = false;
    std::string save;
};

SliceArgs parse_slice_args(int argc, char** argv) {
    SliceArgs a;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--check") == 0) {
            a.check = true;
        } else if (std::strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            a.save = argv[++i];
        }
    }
    return a;
}

AssetId material_id(const char* name) { return AssetId::from_string(std::string("slice:material/") + name); }

class VerticalSlice final : public Application {
public:
    VerticalSlice(AppDesc desc, SliceArgs args) : Application(std::move(desc)), args_(std::move(args)) {}

    [[nodiscard]] bool check_passed() const { return check_passed_; }

protected:
    Result<void> on_init() override {
        auto* input = find_subsystem<InputSubsystem>();
        add_subsystem<CameraControllerSubsystem>(SubsystemKind::Engine, input);
        if (auto* physics = find_subsystem<physics::PhysicsSubsystem>(); physics != nullptr && args_.check) {
            physics->set_debug_draw(physics::DebugDrawFlags::Shapes); // exercise the debug-line path
        }
        register_materials();
        if (auto built = build_scene(); !built) {
            return built;
        }
        setup_sky();
        if (!args_.save.empty()) {
            if (auto saved = scene::save_scene(world(), args_.save); !saved) {
                AE_LOG_ERROR("Slice", "saving {} failed: {}", args_.save, saved.error().message);
            } else {
                AE_LOG_INFO("Slice", "scene saved to {}", args_.save);
            }
        }
        return {};
    }

    void on_update(const FrameTime& t) override {
        if (t.frame_index == 1) {
            // Heights after the first frame; compared at shutdown.
            for (const Entity e : dynamic_bodies_) {
                start_heights_.push_back(scene::world_position(world(), e).y);
            }
        }
    }

    void on_imgui() override { draw_engine_stats(*this); }

    void on_shutdown() override {
        if (args_.check) {
            check_passed_ = run_checks();
        }
        if (sky_.is_valid()) {
            renderer().release(sky_);
            sky_ = {};
        }
    }

private:
    // Procedural HDR sky whose sun matches the directional light: skybox + image-based lighting.
    void setup_sky() {
        auto* bridge = find_subsystem<RenderBridgeSubsystem>();
        if (bridge == nullptr) {
            return;
        }
        SkySettings sky;
        sky.sun_direction = -(sun_rotation_ * Vec3(0, 0, -1)); // towards the sun
        const u32              w      = 256;
        const u32              h      = 128;
        const std::vector<f32> pixels = make_sky_equirect(w, h, sky);
        renderer::EnvironmentUpload up;
        up.width      = w;
        up.height     = h;
        up.rgba32f    = pixels;
        up.debug_name = "ProceduralSky";
        sky_          = renderer().register_environment(up);
        renderer::EnvironmentSettings env;
        env.skybox            = sky_;
        env.ambient_intensity = 1.0f;
        bridge->set_environment(env);
    }

    void register_materials() {
        auto* bridge = find_subsystem<RenderBridgeSubsystem>();
        if (bridge == nullptr || bridge->cache() == nullptr) {
            return;
        }
        struct Mat {
            const char* name;
            Vec4        color;
            f32         metallic;
            f32         roughness;
        };
        const Mat mats[] = {
            { "floor", Vec4(0.55f, 0.57f, 0.6f, 1.0f), 0.0f, 0.85f },
            { "red", Vec4(0.85f, 0.18f, 0.15f, 1.0f), 0.0f, 0.45f },
            { "blue", Vec4(0.15f, 0.35f, 0.85f, 1.0f), 0.0f, 0.35f },
            { "gold", Vec4(1.0f, 0.78f, 0.34f, 1.0f), 1.0f, 0.25f },
            { "steel", Vec4(0.75f, 0.75f, 0.78f, 1.0f), 1.0f, 0.4f },
        };
        for (const Mat& m : mats) {
            assets::MaterialData d;
            d.name              = m.name;
            d.base_color_factor = m.color;
            d.metallic_factor   = m.metallic;
            d.roughness_factor  = m.roughness;
            bridge->cache()->add_material(material_id(m.name), d);
        }
    }

    Entity mesh_entity(const char* name, BuiltinMesh mesh, const char* material, const Vec3& pos,
                       const Vec3& scale = Vec3(1.0f)) {
        World&       w = world();
        const Entity e = w.create(name);
        scene::set_local_position(w, e, pos);
        scene::set_local_scale(w, e, scale);
        MeshRendererComponent mr;
        mr.mesh     = builtin_mesh_id(mesh);
        mr.material = material_id(material);
        w.add<MeshRendererComponent>(e, mr);
        return e;
    }

    void add_script(Entity e, const char* script, std::initializer_list<scripting::ScriptProperty> props = {}) {
        scripting::ScriptComponent sc;
        sc.script = script;
        for (const auto& p : props) {
            sc.set_property(p.name, p.value);
        }
        world().add<scripting::ScriptComponent>(e, std::move(sc));
    }

    Result<void> build_scene() {
        World& w = world();

        // ---- camera ----
        const Entity camera = w.create("Camera");
        w.add<CameraComponent>(camera, CameraComponent{ 60.0f, 0.1f, 500.0f, true });
        w.add<FlyCameraComponent>(camera);
        scene::set_local_position(w, camera, Vec3(0.0f, 5.0f, 14.0f));
        scene::set_local_rotation(w, camera, rotation_from_yaw_pitch(0.0f, -18.0f));

        // ---- lights ----
        const Entity sun = w.create("Sun");
        LightComponent sl;
        sl.kind         = LightKind::Directional;
        sl.color        = Vec3(1.0f, 0.96f, 0.9f);
        sl.intensity    = 3.0f;
        sl.cast_shadows = true;
        w.add<LightComponent>(sun, sl);
        sun_rotation_ = rotation_from_yaw_pitch(35.0f, -50.0f);
        scene::set_local_rotation(w, sun, sun_rotation_);
        const Entity lamp = w.create("Lamp");
        LightComponent pl;
        pl.kind      = LightKind::Point;
        pl.color     = Vec3(1.0f, 0.55f, 0.25f);
        pl.intensity = 20.0f;
        pl.range     = 12.0f;
        w.add<LightComponent>(lamp, pl);
        scene::set_local_position(w, lamp, Vec3(-3.0f, 2.5f, 3.0f));

        // ---- floor (static) ----
        const Entity floor = mesh_entity("Floor", BuiltinMesh::Plane, "floor", Vec3(0.0f), Vec3(2.0f, 1.0f, 2.0f));
        physics::ColliderComponent floor_col = physics::ColliderComponent::box(Vec3(5.0f, 0.1f, 5.0f));
        floor_col.local_offset               = Vec3(0.0f, -0.1f, 0.0f);
        w.add<physics::ColliderComponent>(floor, floor_col);

        // ---- dynamic stack ----
        const char* colors[] = { "red", "blue", "gold" };
        int         n        = 0;
        for (int layer = 0; layer < 4; ++layer) {
            for (int i = 0; i < 4 - layer; ++i) {
                const Vec3   pos(-1.6f + static_cast<f32>(i) * 1.05f + static_cast<f32>(layer) * 0.52f,
                                 0.5f + static_cast<f32>(layer) * 1.02f + 2.0f, -2.0f);
                const Entity box =
                    mesh_entity(("Box " + std::to_string(n)).c_str(), BuiltinMesh::Cube, colors[n % 3], pos);
                w.add<physics::RigidBodyComponent>(box);
                w.add<physics::ColliderComponent>(box, physics::ColliderComponent::box(Vec3(0.5f)));
                dynamic_bodies_.push_back(box);
                ++n;
            }
        }
        for (int i = 0; i < 3; ++i) {
            const Entity ball = mesh_entity(("Ball " + std::to_string(i)).c_str(), BuiltinMesh::Sphere, "steel",
                                            Vec3(2.5f + static_cast<f32>(i) * 0.3f, 4.0f + static_cast<f32>(i) * 1.5f,
                                                 -1.0f + static_cast<f32>(i) * 0.2f));
            physics::RigidBodyComponent rb;
            rb.restitution = 0.6f;
            w.add<physics::RigidBodyComponent>(ball, rb);
            w.add<physics::ColliderComponent>(ball, physics::ColliderComponent::sphere(0.5f));
            dynamic_bodies_.push_back(ball);
        }

        // ---- scripted props ----
        const Entity launcher = mesh_entity("Launcher", BuiltinMesh::Cube, "gold", Vec3(4.0f, 0.5f, 2.0f));
        w.add<physics::RigidBodyComponent>(launcher);
        w.add<physics::ColliderComponent>(launcher, physics::ColliderComponent::box(Vec3(0.5f)));
        add_script(launcher, "slice/launcher.lua", { { "interval", 1.5 } });
        scripted_.push_back(launcher);

        const Entity bobber = mesh_entity("Bobber", BuiltinMesh::Sphere, "blue", Vec3(-4.0f, 1.5f, 2.0f));
        add_script(bobber, "bobber.lua", { { "height", 0.6 } });
        scripted_.push_back(bobber);

        const Entity capsule = mesh_entity("Spinner", BuiltinMesh::Capsule, "red", Vec3(0.0f, 1.0f, 3.0f),
                                           Vec3(0.5f));
        add_script(capsule, "rotator.lua", { { "speed", 120.0 }, { "axis", Vec3(1, 0, 0) } });
        scripted_.push_back(capsule);

        const Entity spawner = w.create("Spawner");
        scene::set_local_position(w, spawner, Vec3(0.0f, 0.2f, 5.0f));
        add_script(spawner, "spawner.lua", { { "interval", 0.5 } });
        scripted_.push_back(spawner);

        // ---- imported models ----
        SceneInstantiateOptions cube_opt;
        cube_opt.root_name = "Textured Cube";
        auto cube = instantiate_model(assets(), w, "samples/cube/cube.gltf", cube_opt);
        if (!cube) {
            return cube.error();
        }
        scene::set_local_position(w, cube->root, Vec3(-2.5f, 1.2f, 1.0f));
        add_script(cube->root, "rotator.lua", { { "speed", 45.0 } });
        scripted_.push_back(cube->root);

        auto skinned = instantiate_model(assets(), w, "samples/skinned/skinned.gltf");
        if (!skinned) {
            return skinned.error();
        }
        scene::set_local_position(w, skinned->root, Vec3(2.0f, 0.0f, 1.5f));
        expected_animators_ = skinned->animator_count;

        w.update_transforms();
        AE_LOG_INFO("Slice", "scene built: {} entities, {} dynamic bodies, {} scripted", w.entity_count(),
                    dynamic_bodies_.size(), scripted_.size());
        return {};
    }

    bool run_checks() {
        bool ok = true;
        auto fail = [&](const std::string& what) {
            AE_LOG_ERROR("Slice", "CHECK FAILED: {}", what);
            ok = false;
        };
        World& w = world();

        // Physics: every dynamic body fell (or was launched) and none fell through the floor.
        u32 moved = 0;
        for (usize i = 0; i < dynamic_bodies_.size() && i < start_heights_.size(); ++i) {
            const Vec3 p = scene::world_position(w, dynamic_bodies_[i]);
            const f32  y = p.y;
            // Only a body BELOW the floor while still inside its footprint fell through it (a
            // body that rolled off the 20 m floor edge is fine).
            if (y < -1.0f && std::abs(p.x) < 9.5f && std::abs(p.z) < 9.5f) {
                fail(std::format("body '{}' fell through the floor (at {:.2f}, {:.2f}, {:.2f})",
                                 w.get<NameComponent>(dynamic_bodies_[i]).name, p.x, p.y, p.z));
            }
            moved += std::abs(y - start_heights_[i]) > 0.05f ? 1u : 0u;
        }
        if (moved < dynamic_bodies_.size() / 2) {
            fail("physics did not move the dynamic bodies (" + std::to_string(moved) + " moved)");
        }
        auto* physics = find_subsystem<physics::PhysicsSubsystem>();
        if (physics == nullptr || physics->physics_world() == nullptr ||
            physics->physics_world()->body_count() < dynamic_bodies_.size()) {
            fail("physics world missing or incomplete");
        }

        // Scripts: every scripted entity has a running instance and nothing failed.
        auto* scripting = find_subsystem<scripting::ScriptingSubsystem>();
        if (scripting == nullptr || scripting->vm() == nullptr) {
            fail("scripting VM missing");
        } else {
            scripting::ScriptVM& vm = *scripting->vm();
            for (const Entity e : scripted_) {
                if (vm.instance_state(e) != scripting::ScriptInstanceState::Running) {
                    fail("script on '" + w.get<NameComponent>(e).name + "' is not running: " + vm.instance_error(e));
                }
            }
            if (vm.error_count() != 0) {
                fail("script errors: " + std::to_string(vm.error_count()));
            }
            const auto launches = vm.get_field(scripted_.front(), "launches");
            if (!launches || !std::holds_alternative<i64>(*launches) || std::get<i64>(*launches) < 1) {
                fail("the launcher script never launched");
            }
        }

        // Animation: the skinned character is bound and animating.
        auto* anim = find_subsystem<animation::AnimationSubsystem>();
        if (expected_animators_ > 0 && (anim == nullptr || anim->animated_count() < expected_animators_)) {
            fail("animators not ticking (" + std::to_string(anim != nullptr ? anim->animated_count() : 0) + ")");
        }

        // Rendering: everything resolved and was submitted.
        auto* bridge = find_subsystem<RenderBridgeSubsystem>();
        if (bridge == nullptr || bridge->last_stats().instances < dynamic_bodies_.size() ||
            bridge->last_stats().pending_meshes != 0 || bridge->last_stats().skinned < 1) {
            fail("render bridge did not submit the full scene");
        } else {
            const RenderCacheStats cs = bridge->cache()->stats();
            if (cs.failed != 0) {
                fail("render cache has failed assets");
            }
            AE_LOG_INFO("Slice", "rendered {} instances ({} skinned), {} lights; cache {} meshes / {} materials / {} textures",
                        bridge->last_stats().instances, bridge->last_stats().skinned, bridge->last_stats().lights,
                        cs.meshes, cs.materials, cs.textures);
        }
        AE_LOG_INFO("Slice", "self-check {}", ok ? "PASSED" : "FAILED");
        return ok;
    }

    SliceArgs           args_;
    Quat                sun_rotation_{ 1, 0, 0, 0 };
    renderer::EnvHandle sky_;
    std::vector<Entity> dynamic_bodies_;
    std::vector<f32>    start_heights_;
    std::vector<Entity> scripted_;
    u32                 expected_animators_ = 0;
    bool                check_passed_       = true;
};

} // namespace

int main(int argc, char** argv) {
    AppDesc defaults;
    defaults.window.title  = "Aether - Vertical Slice";
    defaults.window.width  = 1280;
    defaults.window.height = 720;
    const AppDesc   desc = parse_command_line(argc, argv, defaults);
    const SliceArgs args = parse_slice_args(argc, argv);

    VerticalSlice app(desc, args);
    if (auto init = app.initialize(); !init) {
        AE_LOG_ERROR("Slice", "initialization failed: {}", init.error().message);
        return 1;
    }
    const int code = app.run();
    return code != 0 ? code : (app.check_passed() ? 0 : 1);
}
