// self_test.cpp — `aether-editor --self-test`: drives the editor through a scripted workflow
// (create, edit, undo/redo, duplicate/delete, add physics, pick, play/stop, save/open,
// instantiate a model, attach a script; M2: multi-select + group transform + undo, snapping,
// multi-edit, selection duplicate/delete/reparent, prefab create/instantiate/revert, asset
// drag-and-drop, material instance edit + undo, async GPU picking with CPU fallback, animation
// view; thumbnails of images / models / prefabs / scenes; projects) with the real UI running, then exits 0 on
// success.
// Used for headless verification (Xvfb + llvmpipe) and CI.
#include "editor_app.h"
#include "thumbnail_cache.h"

#include "aether/animation/components.h"
#include "aether/editor/anim_view.h"
#include "aether/editor/console.h"
#include "aether/editor/material_instance.h"
#include "aether/editor/multi_edit.h"
#include "aether/editor/prefab.h"
#include "aether/gameplay/procedural_mesh.h"
#include "aether/gameplay/render_bridge.h"
#include "aether/gameplay/components.h"
#include "aether/physics/components.h"
#include "aether/renderer/renderer.h"
#include "aether/rhi/device_ext.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/id.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"
#include "aether/scripting/components.h"
#include "aether/scripting/scripting_subsystem.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>

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
    auto ent  = [&](usize i) { return i < self_test_uuids_.size() ? scene::find_by_uuid(w, self_test_uuids_[i]) : kNullEntity; };
    auto pos  = [&](usize i) { return ent(i) != kNullEntity ? scene::world_position(w, ent(i)) : Vec3(-999.0f); };
    auto near3 = [](const Vec3& a, const Vec3& b) { return glm::all(glm::lessThan(glm::abs(a - b), Vec3(1e-3f))); };
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
    // ============================ M2 workflows (ADR-0009 section 3) ============================
    case 10: { // multi-select + group transform about the pivot, one undo step
        new_scene();
        const Entity a = create_entity(CreateKind::Cube);
        const Entity b = create_entity(CreateKind::Cube);
        scene::set_local_position(w, a, Vec3(-2.0f, 1.0f, 0.0f));
        scene::set_local_position(w, b, Vec3(2.0f, 1.0f, 0.0f));
        w.update_transforms();
        record_edit("Place");
        self_test_uuids_ = { scene::uuid_of(w, a), scene::uuid_of(w, b) };
        select(a);
        select_click(b, SelectMode::Add);
        check(selection_.size() == 2 && selected() == b, "shift-click adds; the last selected is primary");
        select_click(a, SelectMode::Toggle);
        check(selection_.size() == 1 && !is_selected(a), "ctrl-click toggles off");
        select_click(a, SelectMode::Toggle);
        check(selection_.size() == 2 && selected() == a, "ctrl-click toggles on");
        const Mat4 pivot = selection_pivot_matrix();
        check(near3(Vec3(pivot[3]), Vec3(0.0f, 1.0f, 0.0f)), "group pivot at the selection's bounds centre");
        const usize steps = history_.undo_count();
        check(transform_selection(glm::translate(Mat4(1.0f), Vec3(0.0f, 1.0f, 0.0f)) * pivot), "group translate");
        check(near3(pos(0), Vec3(-2, 2, 0)) && near3(pos(1), Vec3(2, 2, 0)), "both entities moved with the pivot");
        check(history_.undo_count() == steps + 1, "a group transform is one undo step");
        undo();
        check(near3(pos(0), Vec3(-2, 1, 0)) && near3(pos(1), Vec3(2, 1, 0)), "one undo restores the whole group");
        check(selection_.size() == 2, "the multi-selection survives undo");
        redo();
        check(near3(pos(0), Vec3(-2, 2, 0)) && near3(pos(1), Vec3(2, 2, 0)), "redo re-applies the group transform");
        transform_selection(selection_pivot_matrix() * glm::rotate(Mat4(1.0f), glm::radians(180.0f), Vec3(0, 1, 0)));
        check(near3(pos(0), Vec3(2, 2, 0)) && near3(pos(1), Vec3(-2, 2, 0)), "group rotation turns about the pivot");
        undo();
        next();
        break;
    }
    case 11: { // absolute translate snapping + snapped drop placement
        snap_.translate      = true;
        snap_.translate_step = 0.5f;
        transform_selection(glm::translate(Mat4(1.0f), Vec3(0.3f, 2.2f, 0.1f)));
        check(near3(pos(0), Vec3(-1.5f, 2.0f, 0.0f)) && near3(pos(1), Vec3(2.5f, 2.0f, 0.0f)),
              std::format("pivot snapped to the 0.5 m grid (A at {:.2f},{:.2f},{:.2f})", pos(0).x, pos(0).y, pos(0).z));
        const Vec3 p = placement_point(Vec2(0.37f, 0.61f));
        check(near3(p, snap_position(p, 0.5f)), "drop placement lands on the snap grid");
        check(snap_.show_grid, "viewport grid on by default");
        snap_.translate = false;
        undo();
        next();
        break;
    }
    case 12: { // multi-entity inspector edit (the inspector's propagation path), one undo step
        const Entity a = ent(0);
        const Entity b = ent(1);
        const Entity pair[] = { a, b };
        select_entities(pair); // primary b
        const MeshRendererComponent before = w.get<MeshRendererComponent>(b);
        w.get<MeshRendererComponent>(b).cast_shadows = false;
        w.get<MeshRendererComponent>(b).material     = gameplay::builtin_material_id(gameplay::BuiltinMaterial::Red);
        const Entity others[] = { a };
        propagate_fields(w, before, w.get<MeshRendererComponent>(b), others, &MeshRendererComponent::mesh,
                         &MeshRendererComponent::material, &MeshRendererComponent::cast_shadows);
        const usize steps = history_.undo_count();
        record_edit("Multi-edit");
        check(!w.get<MeshRendererComponent>(a).cast_shadows &&
                  w.get<MeshRendererComponent>(a).material == gameplay::builtin_material_id(gameplay::BuiltinMaterial::Red),
              "the edited fields propagated to the other selected entity");
        check(w.get<MeshRendererComponent>(a).mesh == before.mesh, "unedited fields untouched");
        check(history_.undo_count() == steps + 1, "a multi-edit is one undo step");
        undo();
        check(ent(0) != kNullEntity && ent(1) != kNullEntity && w.get<MeshRendererComponent>(ent(0)).cast_shadows &&
                  w.get<MeshRendererComponent>(ent(1)).cast_shadows,
              "one undo reverts every entity");
        next();
        break;
    }
    case 13: { // duplicate / delete / reparent a selection
        const Entity pair[] = { ent(0), ent(1) };
        select_entities(pair);
        const usize count  = w.entity_count();
        const auto  copies = duplicate_selection();
        check(copies.size() == 2 && w.entity_count() == count + 2, "duplicate selection copies both");
        check(selection_.size() == 2 && copies.size() == 2 && is_selected(copies[0]) && is_selected(copies[1]),
              "the copies become the selection");
        delete_selection();
        check(w.entity_count() == count && selection_.empty(), "delete selection");
        undo();
        check(w.entity_count() == count + 2, "undo delete restores both (one step)");
        undo();
        check(w.entity_count() == count, "undo duplicate (one step)");
        const Entity holder = create_entity(CreateKind::Empty);
        scene::set_local_position(w, holder, Vec3(0.0f, 5.0f, 0.0f));
        w.update_transforms();
        record_edit("Move holder");
        const Entity again[] = { ent(0), ent(1) };
        select_entities(again);
        const Vec3 before = pos(0);
        check(reparent_selection(holder), "reparent the selection");
        check(scene::parent_of(w, ent(0)) == holder && scene::parent_of(w, ent(1)) == holder, "both parented");
        check(near3(pos(0), before), "reparenting keeps world transforms");
        undo();
        check(scene::parent_of(w, ent(0)) == kNullEntity && scene::parent_of(w, ent(1)) == kNullEntity,
              "undo reparent (one step)");
        next();
        break;
    }
    case 14: { // prefab: create from the selection, instantiate, undo
        const Entity pair[] = { ent(0), ent(1) };
        select_entities(pair);
        const std::filesystem::path file = self_test_dir_ / "pair.aeprefab";
        const Vec3                  pa   = pos(0);
        const Vec3                  mid  = (pos(0) + pos(1)) * 0.5f;
        check(create_prefab(file), "create a prefab from the selection");
        check(std::filesystem::exists(file), "prefab file written");
        check(ent(0) != kNullEntity && w.has<PrefabInstanceComponent>(ent(0)) && w.has<PrefabInstanceComponent>(ent(1)),
              "the selection is linked to the new prefab");
        const usize count = w.entity_count();
        const auto  roots = instantiate_prefab_file(file, kNullEntity, glm::translate(Mat4(1.0f), Vec3(0.0f, 0.0f, -6.0f)));
        record_edit("Instantiate prefab");
        check(roots.size() == 2 && w.entity_count() == count + 2, "prefab instantiated (2 roots)");
        if (roots.size() == 2) {
            check(near3(scene::world_position(w, roots[0]), Vec3(0.0f, 0.0f, -6.0f) + (pa - mid)),
                  "instance roots keep their layout around the placement");
            check(w.get<PrefabInstanceComponent>(roots[1]).source == content_relative_string(file) &&
                      w.get<PrefabInstanceComponent>(roots[1]).root_index == 1,
                  "instances remember their source prefab");
            w.get<NameComponent>(roots[1]).name = "Edited";
            record_edit("Rename");
            check(revert_prefab(roots[1]), "revert an instance to its prefab");
        }
        undo();
        undo();
        undo();
        check(w.entity_count() == count, "undo removes the instances");
        next();
        break;
    }
    case 15: { // asset drag-and-drop through the programmatic drop targets
        set_editor_camera(Vec3(0.0f, 6.0f, 10.0f), Vec3(0.0f, 0.0f, 0.0f));
        if (self_test_frame_ < 6) {
            return;
        }
        const Vec2  uv(0.5f, 0.75f);
        const Vec3  expected = placement_point(uv);
        const usize count    = w.entity_count();
        check(expected.y > -0.01f && expected.y < 3.0f, std::format("drop point on the ground / a surface (y = {:.2f})", expected.y));
        check(drop_asset_in_viewport("samples/cube/cube.gltf", uv), "drop a glTF into the viewport");
        const Entity dropped = selected();
        check(dropped != kNullEntity && w.entity_count() > count, "the dropped model is created and selected");
        check(dropped != kNullEntity && near3(scene::world_position(w, dropped), expected),
              "the model lands at the cursor's placement point");
        const Entity parent = ent(0);
        check(drop_asset_on_entity(self_test_dir_ / "pair.aeprefab", parent), "drop a prefab onto an entity (hierarchy)");
        const std::vector<Entity> dropped_prefab = selection();
        check(!dropped_prefab.empty() && scene::parent_of(w, dropped_prefab.front()) == parent,
              "the dropped prefab becomes a child of the target");
        check(drop_asset_in_viewport(self_test_dir_ / "test.aescene", uv), "drop a scene into the viewport (additive)");
        undo();
        undo();
        undo();
        check(w.entity_count() == count, "every drop is one undo step");
        next();
        break;
    }
    case 16: { // material editor: instance, live edit, undo
        const Entity a = ent(0);
        select(a);
        make_material_instance(a, kPrimaryMaterialSlot);
        const AssetId id = material_instance_id(scene::uuid_of(w, a), kPrimaryMaterialSlot);
        check(w.get<MeshRendererComponent>(a).material == id, "material instance bound to the mesh renderer");
        gameplay::RenderResourceCache* cache = render_cache();
        check(cache != nullptr && !(cache->material(id) == cache->default_material()),
              "the instance is registered with the renderer");
        const auto*          mi  = w.try_get<MaterialInstanceComponent>(a);
        assets::MaterialData d   = mi != nullptr && mi->find(kPrimaryMaterialSlot) != nullptr ? mi->find(kPrimaryMaterialSlot)->data
                                                                                              : assets::MaterialData{};
        const f32            old = d.roughness_factor;
        d.roughness_factor       = 0.123f;
        d.base_color_factor      = Vec4(0.1f, 0.9f, 0.2f, 1.0f);
        edit_material(a, kPrimaryMaterialSlot, d, false);
        check(registered_materials_[id] == material_data_hash(d), "the edit re-registered the material (live preview)");
        undo();
        const auto* ri = w.try_get<MaterialInstanceComponent>(ent(0));
        check(ri != nullptr && ri->find(kPrimaryMaterialSlot) != nullptr &&
                  ri->find(kPrimaryMaterialSlot)->data.roughness_factor == old,
              "undo restores the material parameters");
        sync_materials();
        check(ri != nullptr && ri->find(kPrimaryMaterialSlot) != nullptr &&
                  registered_materials_[id] == material_data_hash(ri->find(kPrimaryMaterialSlot)->data),
              "the restored parameters are re-registered");
        redo();
        const usize steps = history_.undo_count();
        d.roughness_factor = 0.2f;
        edit_material(ent(0), kPrimaryMaterialSlot, d, true);
        d.roughness_factor = 0.3f;
        edit_material(ent(0), kPrimaryMaterialSlot, d, true);
        end_material_edit();
        check(history_.undo_count() == steps + 1, "a material drag is one undo step");
        next();
        break;
    }
    case 17: { // click-select through the async GPU pick (CPU fallback when unsupported)
        const Vec3 target = pos(0);
        set_editor_camera(target + Vec3(0.0f, 1.0f, 5.0f), target);
        if (self_test_frame_ < 6) {
            return;
        }
        if (self_test_frame_ == 6) {
            select(kNullEntity);
            request_viewport_pick(Vec2(0.5f, 0.5f), SelectMode::Replace);
            return;
        }
        if (pick_tracker_.pending() && self_test_frame_ < 6 + pick_tracker_.timeout_frames() + 4) {
            return;
        }
        check(!pick_tracker_.pending(), "the pick resolved within frames_in_flight + 2 frames");
        check(selected() == ent(0), std::format("click-select through the '{}' path", last_pick_source_));
        AE_LOG_INFO("SelfTest", "viewport pick resolved via {} (GPU answers {}, fallbacks {}, GPU picking {})",
                    last_pick_source_, pick_tracker_.gpu_results(), pick_tracker_.fallbacks(),
                    pick_tracker_.gpu_available() ? "available" : "unavailable");
        next();
        break;
    }
    case 18: { // ctrl-click through the same path toggles the entity off
        if (self_test_frame_ == 3) {
            request_viewport_pick(Vec2(0.5f, 0.5f), SelectMode::Toggle);
            return;
        }
        if (pick_tracker_.pending() && self_test_frame_ < 3 + pick_tracker_.timeout_frames() + 4) {
            return;
        }
        check(selection_.empty(), std::format("ctrl-click toggled the selection off ('{}' path)", last_pick_source_));
        next();
        break;
    }
    case 19: { // animation view of a skinned model
        check(instantiate_asset("samples/skinned/skinned.gltf"), "instantiate the skinned sample");
        const Entity animated = w.registry().view<animation::AnimatorComponent>().front();
        self_test_uuid_       = w.valid(animated) ? scene::uuid_of(w, animated) : 0;
        check(self_test_uuid_ != 0, "the skinned sample has an animator");
        select(scene::find_by_uuid(w, self_test_uuid_));
        play();
        next();
        break;
    }
    case 20: {
        if (self_test_frame_ < 20) {
            return;
        }
        const Entity e = scene::find_by_uuid(w, self_test_uuid_);
        if (const auto* a = e != kNullEntity ? w.try_get<animation::AnimatorComponent>(e) : nullptr) {
            const AnimatorView v = describe_animator(a->graph_instance());
            check(!a->graph_asset.is_valid() || v.valid, "the animation view reads the playing graph");
            AE_LOG_INFO("SelfTest", "animation view: valid {}, {} node(s), {} parameter(s), {} state machine(s)", v.valid,
                        v.node_count, v.parameters.size(), v.machines.size());
        }
        stop();
        next();
        break;
    }
    case 21: { // asset-browser thumbnails: CPU images + rendered models / prefab / scene
        ThumbnailCache* tc = thumbnails();
        check(tc != nullptr, "thumbnail cache exists");
        if (tc == nullptr) {
            next();
            break;
        }
        const std::filesystem::path files[] = { "samples/props/props.gltf", "samples/cube/cube.gltf",
                                                "samples/props/textures/panel_albedo.png", "scenes/showcase.aescene",
                                                self_test_dir_ / "pair.aeprefab" };
        bool done = true;
        for (const auto& f : files) {
            (void)tc->get(f);
            done = done && tc->state(f) != ThumbnailCache::State::Pending;
        }
        if (!done && self_test_frame_ < 1200) {
            return;
        }
        for (const auto& f : files) {
            const ThumbnailCache::State st = tc->state(f);
            check(st == ThumbnailCache::State::Ready, std::format("thumbnail of {} is ready (state {})", f.generic_string(), int(st)));
            if (st != ThumbnailCache::State::Ready) {
                continue;
            }
            auto rb = rhi::read_texture_rgba8(device(), tc->texture(f), rhi::ResourceState::ShaderRead);
            check(rb.has_value(), "thumbnail readback");
            if (!rb) {
                continue;
            }
            // A real picture: visible coverage and spatial variation (not a flat colour).
            f64 sum = 0.0, sum2 = 0.0;
            u64 covered = 0;
            const u64 n = u64(rb->width) * rb->height;
            for (u64 i = 0; i < n; ++i) {
                const u8* p = &rb->rgba8[i * 4];
                const f64 l = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
                sum += l;
                sum2 += l * l;
                covered += p[3] > 0 ? 1u : 0u;
            }
            const f64 mean = sum / f64(n);
            const f64 stddev = std::sqrt(std::max(0.0, sum2 / f64(n) - mean * mean));
            AE_LOG_INFO("SelfTest", "thumbnail {}: {}x{}, mean {:.1f}, stddev {:.1f}, coverage {:.0f}%", f.generic_string(),
                        rb->width, rb->height, mean, stddev, 100.0 * f64(covered) / f64(n));
            check(rb->width == ThumbnailCache::kSize && rb->height == ThumbnailCache::kSize, "thumbnail size");
            check(covered > n / 4, std::format("thumbnail {} has visible content", f.generic_string()));
            check(stddev > 4.0, std::format("thumbnail {} is not a flat colour (stddev {:.1f})", f.generic_string(), stddev));
            if (const char* dump = std::getenv("AE_THUMBNAIL_DUMP")) { // manual review: binary PPM
                std::filesystem::create_directories(dump);
                std::ofstream out(std::filesystem::path(dump) / (f.stem().string() + ".ppm"), std::ios::binary);
                out << "P6\n" << rb->width << " " << rb->height << "\n255\n";
                for (u64 i = 0; i < n; ++i) {
                    out.write(reinterpret_cast<const char*>(&rb->rgba8[i * 4]), 3);
                }
            }
        }
        const ThumbnailCache::Stats ts = tc->stats();
        check(ts.rendered >= 4 && ts.decoded >= 1, std::format("{} rendered + {} decoded thumbnails", ts.rendered, ts.decoded));
        // A file edit regenerates the thumbnail (mtime check).
        std::filesystem::last_write_time(self_test_dir_ / "pair.aeprefab",
                                         std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2));
        self_test_value_ = static_cast<f32>(ts.rendered);
        next();
        break;
    }
    case 22: {
        ThumbnailCache* tc = thumbnails();
        const std::filesystem::path prefab = self_test_dir_ / "pair.aeprefab";
        (void)tc->get(prefab);
        if (tc->stats().rendered <= static_cast<u32>(self_test_value_) && self_test_frame_ < 600) {
            return;
        }
        check(tc->stats().rendered > static_cast<u32>(self_test_value_) && tc->state(prefab) == ThumbnailCache::State::Ready,
              "an edited file's thumbnail is regenerated");
        next();
        break;
    }
    case 23: { // projects (ADR-0014): create + "reopen" through a recording launcher
        std::vector<std::string>  launched;
        std::filesystem::path     launched_exe;
        set_launcher([&](const std::filesystem::path& exe, const std::vector<std::string>& args) -> Result<void> {
            launched_exe = exe;
            launched     = args;
            return {};
        });
        const std::filesystem::path parent = self_test_dir_ / "projects";
        check(create_and_open_project(parent, "Selftest Empty", runtime::ProjectTemplate::Empty), "create an empty project");
        const std::filesystem::path empty = parent / "Selftest Empty" / "Selftest Empty.aeproject";
        check(std::filesystem::exists(empty), "empty project manifest written");
        check(std::filesystem::exists(parent / "Selftest Empty/content/scenes/main.aescene"), "empty project scene written");
        check(launched.size() == 2 && launched[0] == "--project" && launched[1] == empty.string(),
              "the editor is relaunched with --project <manifest>");
        check(launched_exe == runtime::executable_path(), "relaunch uses this executable");
        check(!recent_projects().entries().empty() &&
                  recent_projects().entries().front() == std::filesystem::weakly_canonical(empty),
              "the project is first in the recent list");
        check(create_and_open_project(parent, "Selftest Starter", runtime::ProjectTemplate::Starter),
              "create a project from the starter content");
        check(std::filesystem::exists(parent / "Selftest Starter/content/scenes/showcase.aescene"), "starter content copied");
        check(!create_and_open_project(parent, "bad/name", runtime::ProjectTemplate::Empty), "invalid names are refused");
        // Leave the user's recent list as it was.
        for (const auto* n : { "Selftest Empty", "Selftest Starter" }) {
            recent_projects().remove(std::filesystem::weakly_canonical(parent / n / (std::string(n) + ".aeproject")));
        }
        (void)recent_projects().save();
        set_launcher({});
        next();
        break;
    }
    case 24: { // GI volume (ADR-0016): Create > GI Volume turns on probe captures
        if (self_test_frame_ == 3) {
            const Entity v = create_entity(CreateKind::GIVolume);
            self_test_uuids_ = { scene::uuid_of(w, v) };
            return;
        }
        if (self_test_frame_ < 10) {
            return;
        }
        const Entity v = ent(0);
        check(v != kNullEntity && w.has<gameplay::GIVolumeComponent>(v), "GI volume entity created");
        check(renderer().stats().gi_probes > 0, "the renderer received the GI volume");
        check(renderer().stats().gi_probes_updated > 0, "GI probes are captured every frame");
        if (v != kNullEntity) {
            w.destroy(v);
        }
        next();
        break;
    }
    case 25: { // final checks
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
