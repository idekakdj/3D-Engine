// self_test.cpp — `aether-editor --self-test`: drives the editor through a scripted workflow
// (create, edit, undo/redo, duplicate/delete, add physics, pick, play/stop, save/open,
// instantiate a model, attach a script) with the real UI running, then exits 0 on success.
// Used for headless verification (Xvfb + llvmpipe) and CI.
#include "editor_app.h"

#include "aether/editor/console.h"
#include "aether/gameplay/render_bridge.h"
#include "aether/physics/components.h"
#include "aether/renderer/renderer.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/id.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"
#include "aether/scripting/components.h"
#include "aether/scripting/scripting_subsystem.h"

#include <cmath>
#include <format>

namespace aether::editor {

void EditorApp::self_test_tick() {
    ++self_test_frame_;
    World& w     = world();
    auto   check = [&](bool ok, const std::string& what) {
        if (!ok) {
            self_test_passed_ = false;
            self_test_failures_.push_back(std::format("step {}: {}", self_test_step_, what));
            AE_LOG_ERROR("SelfTest", "step {}: {}", self_test_step_, what);
        }
    };
    auto cube = [&]() { return scene::find_by_uuid(w, self_test_uuid_); };
    auto next = [&]() {
        ++self_test_step_;
        self_test_frame_ = 0;
    };
    if (self_test_frame_ < 3) {
        return; // let a few frames render between steps
    }

    switch (self_test_step_) {
    case 0: { // create + move
        new_scene();
        const usize before = w.entity_count();
        const Entity e     = create_entity(CreateKind::Cube);
        check(w.entity_count() == before + 1, "create_entity added an entity");
        check(selected() == e, "the new entity is selected");
        self_test_uuid_ = scene::uuid_of(w, e);
        self_test_value_ = scene::local_transform(w, e).position.y;
        scene::set_local_position(w, e, Vec3(0.0f, 3.0f, 0.0f));
        record_edit("Move");
        check(history_.undo_count() == 2, std::format("two undo steps (got {})", history_.undo_count()));
        next();
        break;
    }
    case 1: { // undo / redo survive the world reload (selection by uuid)
        undo();
        check(cube() != kNullEntity, "cube still exists after undoing the move");
        if (cube() != kNullEntity) {
            check(std::abs(scene::local_transform(w, cube()).position.y - self_test_value_) < 1e-4f, "undo restored the position");
        }
        redo();
        check(cube() != kNullEntity && std::abs(scene::local_transform(w, cube()).position.y - 3.0f) < 1e-4f,
              "redo re-applied the move");
        check(selected() == cube(), "selection survived undo/redo");
        next();
        break;
    }
    case 2: { // duplicate / delete / undo delete
        const usize  before = w.entity_count();
        const Entity copy   = duplicate_entity(cube());
        check(copy != kNullEntity && w.entity_count() == before + 1, "duplicate");
        delete_entity(copy);
        check(w.entity_count() == before, "delete");
        undo();
        check(w.entity_count() == before + 1, "undo delete restores the entity");
        redo();
        check(w.entity_count() == before, "redo delete");
        next();
        break;
    }
    case 3: { // physics components + reparent round trip
        const Entity c = cube();
        w.add<physics::RigidBodyComponent>(c);
        w.add<physics::ColliderComponent>(c, physics::ColliderComponent::box(Vec3(0.5f)));
        record_edit("Add physics");
        const Entity ground = scene::find_by_name(w, "Ground");
        check(ground != kNullEntity, "default scene has a ground");
        if (ground != kNullEntity) {
            w.add<physics::ColliderComponent>(ground, physics::ColliderComponent::box(Vec3(5.0f, 0.05f, 5.0f)));
            record_edit("Ground collider");
            const Entity empty = create_entity(CreateKind::Empty);
            check(reparent(cube(), empty), "reparent under an empty");
            check(scene::parent_of(w, cube()) == empty, "parent set");
            check(std::abs(scene::world_position(w, cube()).y - 3.0f) < 1e-3f, "reparent keeps the world position");
            check(reparent(cube(), kNullEntity), "unparent");
            check(!reparent(empty, empty), "self-parenting is rejected");
        }
        next();
        break;
    }
    case 4: { // picking through the editor camera
        set_editor_camera(Vec3(0.0f, 3.0f, 8.0f), Vec3(0.0f, 3.0f, 0.0f));
        if (self_test_frame_ < 6) {
            return; // viewport size settles
        }
        const auto hit = pick(Vec2(0.5f, 0.5f));
        check(hit.has_value() && hit->entity == cube(), "picking the cube at the viewport centre");
        const auto miss = pick(Vec2(0.02f, 0.02f));
        check(!miss.has_value() || miss->entity != cube(), "picking beside the cube misses it");
        next();
        break;
    }
    case 5: { // play: physics runs
        play();
        check(play_state_ == PlayState::Playing && simulation_enabled(), "play mode");
        next();
        break;
    }
    case 6: {
        if (self_test_frame_ < 40) {
            return;
        }
        check(cube() != kNullEntity && scene::world_position(w, cube()).y < 2.5f,
              std::format("the cube fell while playing (y = {:.2f})",
                          cube() != kNullEntity ? scene::world_position(w, cube()).y : -99.0f));
        stop();
        check(play_state_ == PlayState::Edit && !simulation_enabled(), "edit mode after stop");
        check(cube() != kNullEntity && std::abs(scene::world_position(w, cube()).y - 3.0f) < 1e-3f,
              "stop restored the pre-play world");
        next();
        break;
    }
    case 7: { // save / new / open
        self_test_dir_ = std::filesystem::temp_directory_path() / "aether_editor_self_test";
        std::filesystem::create_directories(self_test_dir_);
        const usize count = w.entity_count();
        check(save_scene(self_test_dir_ / "test.aescene"), "save");
        check(!history_.dirty(), "saving clears the dirty flag");
        new_scene();
        check(cube() == kNullEntity, "new scene is empty of the cube");
        check(open_scene(self_test_dir_ / "test.aescene"), "open");
        check(w.entity_count() == count, std::format("reopened entity count ({} vs {})", w.entity_count(), count));
        check(cube() != kNullEntity && w.has<physics::RigidBodyComponent>(cube()), "physics components round-tripped");
        next();
        break;
    }
    case 8: { // model + script
        check(instantiate_asset("samples/cube/cube.gltf"), "instantiate the glTF sample");
        select(cube());
        check(instantiate_asset("scripts/rotator.lua"), "attach a script from the asset browser");
        check(w.has<scripting::ScriptComponent>(cube()) && w.get<scripting::ScriptComponent>(cube()).script == "rotator.lua",
              "script component path relative to content/scripts");
        play();
        next();
        break;
    }
    case 9: {
        if (self_test_frame_ < 15) {
            return;
        }
        auto* scripting = find_subsystem<scripting::ScriptingSubsystem>();
        check(scripting != nullptr && scripting->vm() != nullptr &&
                  scripting->vm()->instance_state(cube()) == scripting::ScriptInstanceState::Running,
              "the script runs in play mode");
        stop();
        next();
        break;
    }
    case 10: { // final checks
        auto* bridge = find_subsystem<gameplay::RenderBridgeSubsystem>();
        check(bridge != nullptr && bridge->last_stats().instances >= 2, "the viewport renders the scene");
        check(viewport_.texture.is_valid(), "viewport render target exists");
        check(renderer().stats().instances_submitted >= 2, "renderer received instances");
        check(console_error_count() == 0 || !self_test_passed_,
              std::format("{} error(s) logged during the run", console_error_count()));
        std::error_code ec;
        std::filesystem::remove_all(self_test_dir_, ec);
        AE_LOG_INFO("SelfTest", "finished: {}", self_test_passed_ ? "PASSED" : "FAILED");
        request_exit(self_test_passed_ ? 0 : 1);
        ++self_test_step_;
        break;
    }
    default: break;
    }
}

} // namespace aether::editor
