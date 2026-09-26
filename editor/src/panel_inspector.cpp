// panel_inspector.cpp — per-component property editing for the selected entity.
//
// Undo: drags / text inputs open a continuous edit on the first change and close it when the
// widget is deactivated; toggles and combos record a discrete step. Components that other
// modules track through registry signals (physics, scripts) are patch()ed after edits so the
// owning systems see them.
#include "editor_app.h"

#include "aether/animation/components.h"
#include "aether/gameplay/camera_controller.h"
#include "aether/gameplay/components.h"
#include "aether/gameplay/render_bridge.h"
#include "aether/physics/components.h"
#include "aether/scene/components.h"
#include "aether/scene/id.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/visibility.h"
#include "aether/scene/world.h"
#include "aether/scripting/components.h"
#include "aether/scripting/scripting_subsystem.h"

#include <imgui.h>

#include <array>
#include <cstdio>
#include <format>

namespace aether::editor {

namespace {

using namespace aether::physics;

// Widget helpers returning "changed"; continuous edits are tracked by the caller via Edit.
struct Edit {
    EditorApp&  app;
    const char* label;
    std::function<void(const char*)> begin;
    std::function<void()>            end;
    std::function<void(const char*)> commit;

    bool drag(bool changed) const {
        if (changed) {
            begin(label);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            end();
        }
        return changed;
    }
    bool toggle(bool changed) const {
        if (changed) {
            commit(label);
        }
        return changed;
    }
};

bool input_text(const char* label, std::string& value) {
    char buf[512] = {};
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    if (ImGui::InputText(label, buf, sizeof(buf))) {
        value = buf;
        return true;
    }
    return false;
}

std::string asset_label(const AssetId& id) { return id.is_valid() ? id.to_string().substr(0, 12) + "..." : "(none)"; }

// Header with a remove button; returns false when collapsed. `remove` is set when clicked.
bool component_header(const char* title, bool* remove, bool default_open = true) {
    bool keep = true;
    const bool open = ImGui::CollapsingHeader(title, remove != nullptr ? &keep : nullptr,
                                              default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    if (remove != nullptr && !keep) {
        *remove = true;
    }
    return open;
}

} // namespace

void EditorApp::draw_inspector() {
    if (!ImGui::Begin("Inspector")) {
        ImGui::End();
        return;
    }
    World&       w = world();
    const Entity e = selected();
    if (e == kNullEntity) {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::TextDisabled("Click an entity in the viewport or the hierarchy.");
        ImGui::End();
        return;
    }
    auto&      reg = w.registry();
    const Edit ed{ *this, "Edit", [this](const char* l) { begin_edit(l); }, [this] { end_edit(); },
                   [this](const char* l) { record_edit(l); } };
    auto with = [&](const char* label) {
        Edit x = ed;
        x.label = label;
        return x;
    };

    // ---- identity ----
    if (auto* name = w.try_get<NameComponent>(e)) {
        ImGui::SetNextItemWidth(-80.0f);
        with("Rename").drag(input_text("##name", name->name));
        ImGui::SameLine();
        bool visible = scene::is_visible(w, e);
        if (ImGui::Checkbox("Visible", &visible)) {
            scene::set_visible(w, e, visible);
            record_edit("Visibility");
        }
    }
    ImGui::TextDisabled("uuid %s", scene::uuid_to_string(scene::uuid_of(w, e)).c_str());
    ImGui::Separator();

    // ---- transform ----
    if (reg.all_of<TransformComponent>(e) && component_header("Transform", nullptr)) {
        Transform t       = w.get<TransformComponent>(e).local;
        Vec3      euler   = glm::degrees(glm::eulerAngles(t.rotation));
        bool      changed = false;
        changed |= with("Move").drag(ImGui::DragFloat3("Position", &t.position.x, 0.05f));
        if (with("Rotate").drag(ImGui::DragFloat3("Rotation", &euler.x, 0.5f))) {
            t.rotation = glm::normalize(Quat(glm::radians(euler)));
            changed    = true;
        }
        changed |= with("Scale").drag(ImGui::DragFloat3("Scale", &t.scale.x, 0.01f));
        if (changed) {
            scene::set_local_transform(w, e, t);
        }
        if (ImGui::SmallButton("Reset")) {
            scene::set_local_transform(w, e, Transform{});
            record_edit("Reset transform");
        }
    }

    // ---- mesh renderer ----
    if (auto* mr = w.try_get<MeshRendererComponent>(e)) {
        bool remove = false;
        if (component_header("Mesh Renderer", &remove)) {
            const auto builtin_mesh = gameplay::builtin_mesh_from_id(mr->mesh);
            const char* preview     = builtin_mesh ? gameplay::builtin_mesh_name(*builtin_mesh) : "Asset";
            if (ImGui::BeginCombo("Mesh", preview)) {
                for (u8 i = 0; i < static_cast<u8>(gameplay::BuiltinMesh::Count); ++i) {
                    const auto m = static_cast<gameplay::BuiltinMesh>(i);
                    if (ImGui::Selectable(gameplay::builtin_mesh_name(m), builtin_mesh == m)) {
                        mr->mesh = gameplay::builtin_mesh_id(m);
                        record_edit("Mesh");
                    }
                }
                ImGui::EndCombo();
            }
            if (!builtin_mesh) {
                ImGui::TextDisabled("mesh %s", asset_label(mr->mesh).c_str());
            }
            const auto  builtin_mat = gameplay::builtin_material_from_id(mr->material);
            const char* mat_preview = builtin_mat ? gameplay::builtin_material_name(*builtin_mat)
                                      : mr->material.is_valid() ? "Asset" : "(default)";
            if (ImGui::BeginCombo("Material", mat_preview)) {
                for (u8 i = 0; i < static_cast<u8>(gameplay::BuiltinMaterial::Count); ++i) {
                    const auto m = static_cast<gameplay::BuiltinMaterial>(i);
                    if (ImGui::Selectable(gameplay::builtin_material_name(m), builtin_mat == m)) {
                        mr->material = gameplay::builtin_material_id(m);
                        record_edit("Material");
                    }
                }
                ImGui::EndCombo();
            }
            if (mr->material.is_valid() && !builtin_mat) {
                ImGui::TextDisabled("material %s", asset_label(mr->material).c_str());
            }
            with("Cast shadows").toggle(ImGui::Checkbox("Cast shadows", &mr->cast_shadows));
            if (const auto* mo = w.try_get<gameplay::MaterialOverridesComponent>(e)) {
                ImGui::TextDisabled("%zu per-slot material overrides", mo->materials.size());
            }
        }
        if (remove) {
            w.remove<MeshRendererComponent>(e);
            record_edit("Remove Mesh Renderer");
        }
    }

    // ---- light ----
    if (auto* l = w.try_get<LightComponent>(e)) {
        bool remove = false;
        if (component_header("Light", &remove)) {
            int kind = static_cast<int>(l->kind);
            if (with("Light kind").toggle(ImGui::Combo("Kind", &kind, "Directional\0Point\0Spot\0"))) {
                l->kind = static_cast<LightKind>(kind);
            }
            with("Light color").drag(ImGui::ColorEdit3("Color", &l->color.x));
            with("Light intensity").drag(ImGui::DragFloat("Intensity", &l->intensity, 0.1f, 0.0f, 10000.0f));
            if (l->kind != LightKind::Directional) {
                with("Light range").drag(ImGui::DragFloat("Range", &l->range, 0.1f, 0.01f, 10000.0f));
            }
            if (l->kind == LightKind::Spot) {
                with("Spot cone").drag(ImGui::DragFloat("Inner cone", &l->inner_cone_deg, 0.2f, 0.0f, 89.0f));
                with("Spot cone").drag(ImGui::DragFloat("Outer cone", &l->outer_cone_deg, 0.2f, 0.0f, 89.0f));
            }
            with("Light shadows").toggle(ImGui::Checkbox("Cast shadows##light", &l->cast_shadows));
        }
        if (remove) {
            w.remove<LightComponent>(e);
            record_edit("Remove Light");
        }
    }

    // ---- camera ----
    if (auto* c = w.try_get<CameraComponent>(e)) {
        bool remove = false;
        if (component_header("Camera", &remove)) {
            with("Camera FOV").drag(ImGui::DragFloat("FOV", &c->fov_y_deg, 0.2f, 5.0f, 170.0f));
            with("Camera near").drag(ImGui::DragFloat("Near", &c->near_z, 0.01f, 0.001f, 100.0f));
            with("Camera far").drag(ImGui::DragFloat("Far", &c->far_z, 1.0f, 1.0f, 100000.0f));
            with("Primary camera").toggle(ImGui::Checkbox("Primary", &c->primary));
        }
        if (remove) {
            w.remove<CameraComponent>(e);
            record_edit("Remove Camera");
        }
    }
    if (auto* fc = w.try_get<gameplay::FlyCameraComponent>(e)) {
        bool remove = false;
        if (component_header("Fly Camera Controller", &remove, false)) {
            with("Fly speed").drag(ImGui::DragFloat("Move speed", &fc->move_speed, 0.1f, 0.01f, 500.0f));
            with("Fly boost").drag(ImGui::DragFloat("Boost", &fc->boost_multiplier, 0.05f, 1.0f, 50.0f));
            with("Fly look").toggle(ImGui::Checkbox("Require look button", &fc->require_look_button));
        }
        if (remove) {
            w.remove<gameplay::FlyCameraComponent>(e);
            record_edit("Remove Fly Camera");
        }
    }

    // ---- physics ----
    if (auto* rb = w.try_get<RigidBodyComponent>(e)) {
        bool remove = false;
        if (component_header("Rigid Body", &remove)) {
            bool changed = false;
            int  motion  = static_cast<int>(rb->motion_type);
            if (with("Motion type").toggle(ImGui::Combo("Motion", &motion, "Static\0Kinematic\0Dynamic\0"))) {
                rb->motion_type = static_cast<MotionType>(motion);
                changed         = true;
            }
            changed |= with("Mass").drag(ImGui::DragFloat("Mass (0 = auto)", &rb->mass, 0.1f, 0.0f, 100000.0f));
            changed |= with("Friction").drag(ImGui::DragFloat("Friction", &rb->friction, 0.01f, 0.0f, 2.0f));
            changed |= with("Restitution").drag(ImGui::DragFloat("Restitution", &rb->restitution, 0.01f, 0.0f, 1.0f));
            changed |= with("Damping").drag(ImGui::DragFloat("Linear damping", &rb->linear_damping, 0.01f, 0.0f, 10.0f));
            changed |= with("Damping").drag(ImGui::DragFloat("Angular damping", &rb->angular_damping, 0.01f, 0.0f, 10.0f));
            changed |= with("Gravity").drag(ImGui::DragFloat("Gravity factor", &rb->gravity_factor, 0.01f, -10.0f, 10.0f));
            changed |= with("Sensor").toggle(ImGui::Checkbox("Sensor (trigger)", &rb->is_sensor));
            changed |= with("CCD").toggle(ImGui::Checkbox("Continuous collision", &rb->continuous_collision));
            changed |= with("Sleeping").toggle(ImGui::Checkbox("Allow sleeping", &rb->allow_sleeping));
            if (changed) {
                reg.patch<RigidBodyComponent>(e);
            }
        }
        if (remove) {
            w.remove<RigidBodyComponent>(e);
            record_edit("Remove Rigid Body");
        }
    }
    if (auto* col = w.try_get<ColliderComponent>(e)) {
        bool remove = false;
        if (component_header("Collider", &remove)) {
            bool changed = false;
            int  shape   = static_cast<int>(col->shape);
            if (col->shape <= ColliderShape::Cylinder) {
                if (with("Collider shape").toggle(ImGui::Combo("Shape", &shape, "Box\0Sphere\0Capsule\0Cylinder\0"))) {
                    col->shape = static_cast<ColliderShape>(shape);
                    changed    = true;
                }
            } else {
                ImGui::Text("Shape: %s (%zu points)", col->shape == ColliderShape::ConvexHull ? "Convex hull" : "Triangle mesh",
                            col->shape == ColliderShape::ConvexHull ? col->points.size() : col->vertices.size());
            }
            switch (col->shape) {
            case ColliderShape::Box:
                changed |= with("Collider size").drag(ImGui::DragFloat3("Half extents", &col->half_extents.x, 0.01f, 0.001f, 1000.0f));
                break;
            case ColliderShape::Sphere:
                changed |= with("Collider size").drag(ImGui::DragFloat("Radius", &col->radius, 0.01f, 0.001f, 1000.0f));
                break;
            case ColliderShape::Capsule:
            case ColliderShape::Cylinder:
                changed |= with("Collider size").drag(ImGui::DragFloat("Radius", &col->radius, 0.01f, 0.001f, 1000.0f));
                changed |= with("Collider size").drag(ImGui::DragFloat("Half height", &col->half_height, 0.01f, 0.001f, 1000.0f));
                break;
            default: break;
            }
            changed |= with("Collider offset").drag(ImGui::DragFloat3("Offset", &col->local_offset.x, 0.01f));
            if (const auto* mr = w.try_get<MeshRendererComponent>(e); mr != nullptr && render_cache() != nullptr) {
                if (ImGui::SmallButton("Fit box to mesh")) {
                    if (const auto* mesh = render_cache()->mesh(mr->mesh)) {
                        col->shape        = ColliderShape::Box;
                        col->half_extents = glm::max(mesh->bounds.extent(), Vec3(0.005f));
                        col->local_offset = mesh->bounds.center();
                        changed           = true;
                        record_edit("Fit collider");
                    }
                }
            }
            if (changed) {
                reg.patch<ColliderComponent>(e);
            }
        }
        if (remove) {
            w.remove<ColliderComponent>(e);
            record_edit("Remove Collider");
        }
    }
    if (auto* cc = w.try_get<CharacterControllerComponent>(e)) {
        bool remove = false;
        if (component_header("Character Controller", &remove)) {
            bool changed = false;
            changed |= with("Character").drag(ImGui::DragFloat("Radius##cc", &cc->radius, 0.01f, 0.01f, 10.0f));
            changed |= with("Character").drag(ImGui::DragFloat("Half height##cc", &cc->half_height, 0.01f, 0.01f, 10.0f));
            changed |= with("Character").drag(ImGui::DragFloat("Max slope", &cc->max_slope_deg, 0.5f, 0.0f, 89.0f));
            changed |= with("Character").drag(ImGui::DragFloat("Step height", &cc->step_up_height, 0.01f, 0.0f, 2.0f));
            changed |= with("Character").drag(ImGui::DragFloat("Jump speed", &cc->jump_speed, 0.1f, 0.0f, 50.0f));
            if (ImGui::IsItemDeactivatedAfterEdit() || changed) {
                reg.patch<CharacterControllerComponent>(e);
            }
            if (play_state_ != PlayState::Edit) {
                ImGui::Text("ground: %s  velocity (%.2f, %.2f, %.2f)", cc->on_ground ? "yes" : "no", cc->velocity.x,
                            cc->velocity.y, cc->velocity.z);
            }
        }
        if (remove) {
            w.remove<CharacterControllerComponent>(e);
            record_edit("Remove Character Controller");
        }
    }

    // ---- script ----
    if (auto* sc = w.try_get<scripting::ScriptComponent>(e)) {
        bool remove = false;
        if (component_header("Script", &remove)) {
            bool changed = with("Script path").drag(input_text("Script", sc->script));
            changed |= with("Script enabled").toggle(ImGui::Checkbox("Enabled##script", &sc->enabled));
            auto* scripting = find_subsystem<scripting::ScriptingSubsystem>();
            scripting::ScriptVM* vm = scripting != nullptr ? scripting->vm() : nullptr;
            if (vm != nullptr && play_state_ != PlayState::Edit) {
                const auto state = vm->instance_state(e);
                const char* names[] = { "none", "pending", "running", "inactive", "FAILED" };
                ImGui::Text("instance: %s", names[static_cast<int>(state)]);
                if (state == scripting::ScriptInstanceState::Failed) {
                    ImGui::TextWrapped("%s", vm->instance_error(e).c_str());
                }
            }
            // Property overrides, typed.
            ImGui::TextDisabled("Properties");
            for (usize i = 0; i < sc->properties.size(); ++i) {
                scripting::ScriptProperty& p = sc->properties[i];
                ImGui::PushID(static_cast<int>(i));
                std::visit(
                    [&](auto& v) {
                        using T = std::decay_t<decltype(v)>;
                        const char* n = p.name.c_str();
                        if constexpr (std::is_same_v<T, bool>) {
                            changed |= with("Script property").toggle(ImGui::Checkbox(n, &v));
                        } else if constexpr (std::is_same_v<T, i64>) {
                            int iv = static_cast<int>(v);
                            if (with("Script property").drag(ImGui::DragInt(n, &iv))) {
                                v       = iv;
                                changed = true;
                            }
                        } else if constexpr (std::is_same_v<T, f64>) {
                            f32 fv = static_cast<f32>(v);
                            if (with("Script property").drag(ImGui::DragFloat(n, &fv, 0.01f))) {
                                v       = fv;
                                changed = true;
                            }
                        } else if constexpr (std::is_same_v<T, std::string>) {
                            changed |= with("Script property").drag(input_text(n, v));
                        } else if constexpr (std::is_same_v<T, Vec2>) {
                            changed |= with("Script property").drag(ImGui::DragFloat2(n, &v.x, 0.01f));
                        } else if constexpr (std::is_same_v<T, Vec3>) {
                            changed |= with("Script property").drag(ImGui::DragFloat3(n, &v.x, 0.01f));
                        } else if constexpr (std::is_same_v<T, Vec4>) {
                            changed |= with("Script property").drag(ImGui::DragFloat4(n, &v.x, 0.01f));
                        } else if constexpr (std::is_same_v<T, Quat>) {
                            ImGui::Text("%s: quat(%.2f, %.2f, %.2f, %.2f)", n, v.x, v.y, v.z, v.w);
                        } else if constexpr (std::is_same_v<T, Entity>) {
                            const auto* en = world().valid(v) ? world().try_get<NameComponent>(v) : nullptr;
                            ImGui::Text("%s: %s", n, en != nullptr ? en->name.c_str() : "(none)");
                            if (ImGui::BeginDragDropTarget()) {
                                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("AE_ENTITY_UUID")) {
                                    v       = scene::find_by_uuid(world(), *static_cast<const u64*>(pl->Data));
                                    changed = true;
                                    record_edit("Script property");
                                }
                                ImGui::EndDragDropTarget();
                            }
                        } else {
                            ImGui::Text("%s: nil", n);
                        }
                    },
                    p.value);
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    sc->properties.erase(sc->properties.begin() + static_cast<std::ptrdiff_t>(i));
                    record_edit("Remove script property");
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            // Declared defaults that are not overridden yet.
            if (vm != nullptr && !sc->script.empty()) {
                if (auto declared = vm->declared_properties(sc->script); declared) {
                    for (const scripting::ScriptProperty& d : *declared) {
                        if (sc->find_property(d.name) != nullptr) {
                            continue;
                        }
                        const std::string label = std::format("+ {}", d.name);
                        if (ImGui::SmallButton(label.c_str())) {
                            sc->set_property(d.name, d.value);
                            record_edit("Override script property");
                            changed = true;
                        }
                        ImGui::SameLine();
                    }
                    ImGui::NewLine();
                } else {
                    ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", declared.error().message.c_str());
                }
            }
            if (changed) {
                reg.patch<scripting::ScriptComponent>(e);
            }
        }
        if (remove) {
            w.remove<scripting::ScriptComponent>(e);
            record_edit("Remove Script");
        }
    }

    // ---- animator ----
    if (auto* a = w.try_get<animation::AnimatorComponent>(e)) {
        bool remove = false;
        if (component_header("Animator", &remove)) {
            ImGui::Text("skeleton %s  (%s)", asset_label(a->skeleton_asset).c_str(),
                        a->skeleton() ? std::format("{} joints", a->skeleton()->joint_count()).c_str() : "unbound");
            ImGui::Text("clip %s  (%s)", asset_label(a->graph_asset).c_str(),
                        a->graph_instance().valid() ? "bound" : "unbound");
            f32 rate = a->playback_rate();
            if (with("Playback rate").drag(ImGui::DragFloat("Playback rate", &rate, 0.01f, -4.0f, 4.0f))) {
                a->set_playback_rate(rate);
            }
            bool paused = a->paused();
            if (with("Animator pause").toggle(ImGui::Checkbox("Paused##anim", &paused))) {
                a->set_paused(paused);
            }
        }
        if (remove) {
            w.remove<animation::AnimatorComponent>(e);
            record_edit("Remove Animator");
        }
    }

    // ---- add component ----
    ImGui::Separator();
    if (ImGui::Button("Add Component", ImVec2(-1, 0))) {
        ImGui::OpenPopup("AddComponent");
    }
    if (ImGui::BeginPopup("AddComponent")) {
        auto item = [&](const char* label, bool has, auto&& add) {
            if (ImGui::MenuItem(label, nullptr, false, !has)) {
                add();
                record_edit("Add component");
            }
        };
        item("Mesh Renderer", reg.all_of<MeshRendererComponent>(e), [&] {
            MeshRendererComponent mr;
            mr.mesh     = gameplay::builtin_mesh_id(gameplay::BuiltinMesh::Cube);
            mr.material = gameplay::default_material_id();
            w.add<MeshRendererComponent>(e, mr);
        });
        item("Light", reg.all_of<LightComponent>(e), [&] { w.add<LightComponent>(e); });
        item("Camera", reg.all_of<CameraComponent>(e), [&] { w.add<CameraComponent>(e); });
        item("Fly Camera Controller", reg.all_of<gameplay::FlyCameraComponent>(e), [&] { w.add<gameplay::FlyCameraComponent>(e); });
        ImGui::Separator();
        item("Rigid Body", reg.all_of<RigidBodyComponent>(e), [&] { w.add<RigidBodyComponent>(e); });
        item("Collider", reg.all_of<ColliderComponent>(e), [&] {
            ColliderComponent c;
            if (const auto* mr = w.try_get<MeshRendererComponent>(e); mr != nullptr && render_cache() != nullptr) {
                if (const auto* mesh = render_cache()->mesh(mr->mesh)) {
                    c.half_extents = glm::max(mesh->bounds.extent(), Vec3(0.005f));
                    c.local_offset = mesh->bounds.center();
                }
            }
            w.add<ColliderComponent>(e, c);
        });
        item("Character Controller", reg.all_of<CharacterControllerComponent>(e), [&] { w.add<CharacterControllerComponent>(e); });
        ImGui::Separator();
        item("Script", reg.all_of<scripting::ScriptComponent>(e), [&] { w.add<scripting::ScriptComponent>(e); });
        ImGui::EndPopup();
    }
    ImGui::End();
}

} // namespace aether::editor
