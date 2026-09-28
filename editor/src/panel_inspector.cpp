// panel_inspector.cpp — per-component property editing for the selection.
//
// Single selection: every component of the entity. Multi-selection: the widgets show the PRIMARY
// (last selected) entity's values and only the components ALL selected entities share; a changed
// field is copied to every selected entity (Unreal multi-edit: only the edited property
// propagates, per axis for vectors). Adding / removing a component applies to all.
//
// Undo: drags / text inputs open a continuous edit on the first change and close it when the
// widget is deactivated; toggles and combos record a discrete step. Both the close and the
// discrete record are deferred to the end of the panel (flush_deferred_edits) so the propagated
// copies land in the same undo step. Components that other modules track through registry
// signals (physics, scripts) are patch()ed after edits so the owning systems see them.
#include "editor_app.h"

#include "aether/animation/components.h"
#include "aether/editor/material_instance.h"
#include "aether/editor/multi_edit.h"
#include "aether/editor/prefab.h"
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
#include <span>

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

template <class T>
bool all_have(const World& w, std::span<const Entity> entities) {
    for (const Entity e : entities) {
        if (!w.has<T>(e)) {
            return false;
        }
    }
    return !entities.empty();
}

template <class T>
void remove_all(World& w, std::span<const Entity> entities) {
    for (const Entity e : entities) {
        w.registry().remove<T>(e);
    }
}

} // namespace

void EditorApp::draw_inspector() {
    if (!ImGui::Begin("Inspector")) {
        ImGui::End();
        return;
    }
    World&                    w   = world();
    const std::vector<Entity> sel = selection();
    const Entity              e   = selected();
    if (e == kNullEntity) {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::TextDisabled("Click an entity in the viewport or the hierarchy.");
        ImGui::End();
        return;
    }
    const bool          multi = sel.size() > 1;
    std::vector<Entity> others;
    for (const Entity x : sel) {
        if (x != e) {
            others.push_back(x);
        }
    }
    auto&      reg = w.registry();
    const Edit ed{ *this, "Edit", [this](const char* l) { begin_edit(l); }, [this] { defer_end(); },
                   [this](const char* l) { defer_commit(l); } };
    auto with = [&](const char* label) {
        Edit x = ed;
        x.label = label;
        return x;
    };
    // A section is shown for the primary if (single) it has it, or (multi) every selected entity has it.
    auto shown = [&](auto* component, auto tag) {
        using T = typename decltype(tag)::type;
        return component != nullptr && (!multi || all_have<T>(w, sel));
    };
    auto finish = [&] {
        flush_deferred_edits();
        ImGui::End();
    };

    // ---- identity ----
    if (multi) {
        ImGui::Text("%zu entities selected", sel.size());
        ImGui::TextDisabled("Showing '%s' (primary); edits apply to all.", w.get<NameComponent>(e).name.c_str());
        bool visible = scene::is_visible(w, e);
        if (ImGui::Checkbox("Visible", &visible)) {
            for (const Entity x : sel) {
                scene::set_visible(w, x, visible);
            }
            defer_commit("Visibility");
        }
    } else if (auto* name = w.try_get<NameComponent>(e)) {
        ImGui::SetNextItemWidth(-80.0f);
        with("Rename").drag(input_text("##name", name->name));
        ImGui::SameLine();
        bool visible = scene::is_visible(w, e);
        if (ImGui::Checkbox("Visible", &visible)) {
            scene::set_visible(w, e, visible);
            defer_commit("Visibility");
        }
        ImGui::TextDisabled("uuid %s", scene::uuid_to_string(scene::uuid_of(w, e)).c_str());
    }
    ImGui::Separator();

    // ---- prefab link ----
    if (const auto* link = w.try_get<PrefabInstanceComponent>(e); link != nullptr && !multi) {
        if (component_header("Prefab", nullptr)) {
            ImGui::TextWrapped("%s", link->source.c_str());
            const u64 uuid = scene::uuid_of(w, e);
            if (ImGui::SmallButton("Revert")) {
                pending_prefab_ops_.emplace_back(uuid, false);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Apply")) {
                pending_prefab_ops_.emplace_back(uuid, true);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Unlink")) {
                reg.remove<PrefabInstanceComponent>(e);
                defer_commit("Unlink prefab");
                finish();
                return;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Browse")) {
                assets_dir_  = std::filesystem::path(link->source).parent_path();
                show_assets_ = true;
            }
        }
    }

    // ---- transform (per-axis multi-edit) ----
    if (reg.all_of<TransformComponent>(e) && component_header("Transform", nullptr)) {
        Transform  t      = w.get<TransformComponent>(e).local;
        Vec3       euler  = glm::degrees(glm::eulerAngles(t.rotation));
        const Transform t0 = t;
        const Vec3 euler0 = euler;
        bool       changed = false;
        bool       rotated = false;
        changed |= with("Move").drag(ImGui::DragFloat3("Position", &t.position.x, 0.05f));
        if (with("Rotate").drag(ImGui::DragFloat3("Rotation", &euler.x, 0.5f))) {
            t.rotation = glm::normalize(Quat(glm::radians(euler)));
            changed    = true;
            rotated    = true;
        }
        changed |= with("Scale").drag(ImGui::DragFloat3("Scale", &t.scale.x, 0.01f));
        if (changed) {
            scene::set_local_transform(w, e, t);
            for (const Entity o : others) {
                Transform ot = scene::local_transform(w, o);
                Vec3      oe = glm::degrees(glm::eulerAngles(ot.rotation));
                for (int i = 0; i < 3; ++i) {
                    if (t.position[i] != t0.position[i]) ot.position[i] = t.position[i];
                    if (t.scale[i] != t0.scale[i]) ot.scale[i] = t.scale[i];
                    if (rotated && euler[i] != euler0[i]) oe[i] = euler[i];
                }
                if (rotated) {
                    ot.rotation = glm::normalize(Quat(glm::radians(oe)));
                }
                scene::set_local_transform(w, o, ot);
            }
        }
        if (ImGui::SmallButton("Reset")) {
            for (const Entity x : sel) {
                scene::set_local_transform(w, x, Transform{});
            }
            defer_commit("Reset transform");
        }
    }

    // ---- mesh renderer ----
    if (auto* mr = w.try_get<MeshRendererComponent>(e); shown(mr, std::type_identity<MeshRendererComponent>{})) {
        const MeshRendererComponent before = *mr;
        bool                        remove = false;
        if (component_header("Mesh Renderer", &remove)) {
            const auto builtin_mesh = gameplay::builtin_mesh_from_id(mr->mesh);
            const char* preview     = builtin_mesh ? gameplay::builtin_mesh_name(*builtin_mesh) : "Asset";
            if (ImGui::BeginCombo("Mesh", preview)) {
                for (u8 i = 0; i < static_cast<u8>(gameplay::BuiltinMesh::Count); ++i) {
                    const auto m = static_cast<gameplay::BuiltinMesh>(i);
                    if (ImGui::Selectable(gameplay::builtin_mesh_name(m), builtin_mesh == m)) {
                        mr->mesh = gameplay::builtin_mesh_id(m);
                        defer_commit("Mesh");
                    }
                }
                ImGui::EndCombo();
            }
            if (!builtin_mesh) {
                ImGui::TextDisabled("mesh %s", asset_label(mr->mesh).c_str());
            }
            const auto  builtin_mat = gameplay::builtin_material_from_id(mr->material);
            const char* mat_preview = builtin_mat                              ? gameplay::builtin_material_name(*builtin_mat)
                                      : is_material_instance_id(mr->material) ? "Instance"
                                      : mr->material.is_valid()               ? "Asset"
                                                                              : "(default)";
            if (ImGui::BeginCombo("Material", mat_preview)) {
                for (u8 i = 0; i < static_cast<u8>(gameplay::BuiltinMaterial::Count); ++i) {
                    const auto m = static_cast<gameplay::BuiltinMaterial>(i);
                    if (ImGui::Selectable(gameplay::builtin_material_name(m), builtin_mat == m)) {
                        mr->material = gameplay::builtin_material_id(m);
                        defer_commit("Material");
                    }
                }
                ImGui::EndCombo();
            }
            if (mr->material.is_valid() && !builtin_mat && !is_material_instance_id(mr->material)) {
                ImGui::TextDisabled("material %s", asset_label(mr->material).c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Edit...")) {
                show_material_ = true;
                ImGui::SetWindowFocus("Material");
            }
            with("Cast shadows").toggle(ImGui::Checkbox("Cast shadows", &mr->cast_shadows));
            if (const auto* mo = w.try_get<gameplay::MaterialOverridesComponent>(e)) {
                ImGui::TextDisabled("%zu per-slot material overrides", mo->materials.size());
            }
        }
        if (multi) {
            propagate_fields(w, before, *mr, others, &MeshRendererComponent::mesh, &MeshRendererComponent::material,
                      &MeshRendererComponent::cast_shadows);
        }
        // Picking a non-instance material replaces the entity's primary material instance.
        if (!(before.material == mr->material) && !is_material_instance_id(mr->material)) {
            for (const Entity x : sel) {
                const auto* mi = w.try_get<MaterialInstanceComponent>(x);
                if (mi != nullptr && mi->find(kPrimaryMaterialSlot) != nullptr) {
                    remove_material_instance(w, x, kPrimaryMaterialSlot, mr->material);
                }
            }
        }
        if (remove) {
            remove_all<MeshRendererComponent>(w, sel);
            defer_commit("Remove Mesh Renderer");
        }
    }

    // ---- light ----
    if (auto* l = w.try_get<LightComponent>(e); shown(l, std::type_identity<LightComponent>{})) {
        const LightComponent before = *l;
        bool                 remove = false;
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
        if (multi) {
            propagate_fields(w, before, *l, others, &LightComponent::kind, &LightComponent::color, &LightComponent::intensity,
                      &LightComponent::range, &LightComponent::inner_cone_deg, &LightComponent::outer_cone_deg,
                      &LightComponent::cast_shadows);
        }
        if (remove) {
            remove_all<LightComponent>(w, sel);
            defer_commit("Remove Light");
        }
    }

    // ---- camera ----
    if (auto* c = w.try_get<CameraComponent>(e); shown(c, std::type_identity<CameraComponent>{})) {
        const CameraComponent before = *c;
        bool                  remove = false;
        if (component_header("Camera", &remove)) {
            with("Camera FOV").drag(ImGui::DragFloat("FOV", &c->fov_y_deg, 0.2f, 5.0f, 170.0f));
            with("Camera near").drag(ImGui::DragFloat("Near", &c->near_z, 0.01f, 0.001f, 100.0f));
            with("Camera far").drag(ImGui::DragFloat("Far", &c->far_z, 1.0f, 1.0f, 100000.0f));
            with("Primary camera").toggle(ImGui::Checkbox("Primary", &c->primary));
        }
        if (multi) {
            propagate_fields(w, before, *c, others, &CameraComponent::fov_y_deg, &CameraComponent::near_z, &CameraComponent::far_z,
                      &CameraComponent::primary);
        }
        if (remove) {
            remove_all<CameraComponent>(w, sel);
            defer_commit("Remove Camera");
        }
    }
    if (auto* gv = w.try_get<gameplay::GIVolumeComponent>(e); shown(gv, std::type_identity<gameplay::GIVolumeComponent>{})) {
        const gameplay::GIVolumeComponent before = *gv;
        bool                              remove = false;
        if (component_header("GI Volume", &remove)) {
            with("GI enabled").toggle(ImGui::Checkbox("Enabled##gi", &gv->enabled));
            with("GI spacing").drag(ImGui::DragFloat("Probe spacing", &gv->probe_spacing, 0.05f, 0.25f, 50.0f, "%.2f m"));
            with("GI intensity").drag(ImGui::DragFloat("Intensity##gi", &gv->intensity, 0.01f, 0.0f, 10.0f));
            const renderer::GiVolume box = gameplay::gi_volume_from(w.world_matrix(e), *gv);
            if (box.enabled) {
                ImGui::TextDisabled("%u x %u x %u probes (box = the entity's scale)", box.probe_counts.x,
                                    box.probe_counts.y, box.probe_counts.z);
            } else {
                ImGui::TextDisabled("give the entity a non-zero scale on every axis");
            }
        }
        if (multi) {
            propagate_fields(w, before, *gv, others, &gameplay::GIVolumeComponent::enabled,
                             &gameplay::GIVolumeComponent::probe_spacing, &gameplay::GIVolumeComponent::intensity);
        }
        if (remove) {
            remove_all<gameplay::GIVolumeComponent>(w, sel);
            defer_commit("Remove GI Volume");
        }
    }
    if (auto* rp = w.try_get<gameplay::ReflectionProbeComponent>(e);
        shown(rp, std::type_identity<gameplay::ReflectionProbeComponent>{})) {
        const gameplay::ReflectionProbeComponent before = *rp;
        bool                                     remove = false;
        if (component_header("Reflection Probe", &remove)) {
            with("Probe enabled").toggle(ImGui::Checkbox("Enabled##refl", &rp->enabled));
            with("Probe intensity").drag(ImGui::DragFloat("Intensity##refl", &rp->intensity, 0.01f, 0.0f, 10.0f));
            with("Probe blend").drag(ImGui::DragFloat("Blend distance", &rp->blend_distance, 0.02f, 0.0f, 20.0f, "%.2f m"));
            ImGui::TextDisabled("captured at the entity's position; box = its scale");
        }
        if (multi) {
            propagate_fields(w, before, *rp, others, &gameplay::ReflectionProbeComponent::enabled,
                             &gameplay::ReflectionProbeComponent::intensity,
                             &gameplay::ReflectionProbeComponent::blend_distance);
        }
        if (remove) {
            remove_all<gameplay::ReflectionProbeComponent>(w, sel);
            defer_commit("Remove Reflection Probe");
        }
    }
    if (auto* fc = w.try_get<gameplay::FlyCameraComponent>(e); shown(fc, std::type_identity<gameplay::FlyCameraComponent>{})) {
        const gameplay::FlyCameraComponent before = *fc;
        bool                               remove = false;
        if (component_header("Fly Camera Controller", &remove, false)) {
            with("Fly speed").drag(ImGui::DragFloat("Move speed", &fc->move_speed, 0.1f, 0.01f, 500.0f));
            with("Fly boost").drag(ImGui::DragFloat("Boost", &fc->boost_multiplier, 0.05f, 1.0f, 50.0f));
            with("Fly look").toggle(ImGui::Checkbox("Require look button", &fc->require_look_button));
        }
        if (multi) {
            propagate_fields(w, before, *fc, others, &gameplay::FlyCameraComponent::move_speed,
                      &gameplay::FlyCameraComponent::boost_multiplier, &gameplay::FlyCameraComponent::require_look_button);
        }
        if (remove) {
            remove_all<gameplay::FlyCameraComponent>(w, sel);
            defer_commit("Remove Fly Camera");
        }
    }

    // ---- physics ----
    if (auto* rb = w.try_get<RigidBodyComponent>(e); shown(rb, std::type_identity<RigidBodyComponent>{})) {
        const RigidBodyComponent before = *rb;
        bool                     remove = false;
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
        if (multi && propagate_fields(w, before, *rb, others, &RigidBodyComponent::motion_type, &RigidBodyComponent::mass,
                               &RigidBodyComponent::friction, &RigidBodyComponent::restitution,
                               &RigidBodyComponent::linear_damping, &RigidBodyComponent::angular_damping,
                               &RigidBodyComponent::gravity_factor, &RigidBodyComponent::is_sensor,
                               &RigidBodyComponent::continuous_collision, &RigidBodyComponent::allow_sleeping)) {
            patch_all<RigidBodyComponent>(w, others);
        }
        if (remove) {
            remove_all<RigidBodyComponent>(w, sel);
            defer_commit("Remove Rigid Body");
        }
    }
    if (auto* col = w.try_get<ColliderComponent>(e); shown(col, std::type_identity<ColliderComponent>{})) {
        const ColliderComponent before = *col;
        bool                    remove = false;
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
            if (const auto* mr = w.try_get<MeshRendererComponent>(e); mr != nullptr && render_cache() != nullptr && !multi) {
                if (ImGui::SmallButton("Fit box to mesh")) {
                    if (const auto* mesh = render_cache()->mesh(mr->mesh)) {
                        col->shape        = ColliderShape::Box;
                        col->half_extents = glm::max(mesh->bounds.extent(), Vec3(0.005f));
                        col->local_offset = mesh->bounds.center();
                        changed           = true;
                        defer_commit("Fit collider");
                    }
                }
            }
            if (changed) {
                reg.patch<ColliderComponent>(e);
            }
        }
        if (multi && propagate_fields(w, before, *col, others, &ColliderComponent::shape, &ColliderComponent::half_extents,
                               &ColliderComponent::radius, &ColliderComponent::half_height, &ColliderComponent::local_offset)) {
            patch_all<ColliderComponent>(w, others);
        }
        if (remove) {
            remove_all<ColliderComponent>(w, sel);
            defer_commit("Remove Collider");
        }
    }
    if (auto* cc = w.try_get<CharacterControllerComponent>(e); shown(cc, std::type_identity<CharacterControllerComponent>{})) {
        const CharacterControllerComponent before = *cc;
        bool                               remove = false;
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
        if (multi && propagate_fields(w, before, *cc, others, &CharacterControllerComponent::radius,
                               &CharacterControllerComponent::half_height, &CharacterControllerComponent::max_slope_deg,
                               &CharacterControllerComponent::step_up_height, &CharacterControllerComponent::jump_speed)) {
            patch_all<CharacterControllerComponent>(w, others);
        }
        if (remove) {
            remove_all<CharacterControllerComponent>(w, sel);
            defer_commit("Remove Character Controller");
        }
    }

    // ---- script (single selection) ----
    if (auto* sc = w.try_get<scripting::ScriptComponent>(e); sc != nullptr && !multi) {
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
                                    defer_commit("Script property");
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
                    defer_commit("Remove script property");
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
                            defer_commit("Override script property");
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
            defer_commit("Remove Script");
        }
    }

    // ---- animator (single selection; graph view in the Animation panel) ----
    if (auto* a = w.try_get<animation::AnimatorComponent>(e); a != nullptr && !multi) {
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
            defer_commit("Remove Animator");
        }
    }
    if (multi) {
        ImGui::TextDisabled("Components not shared by every selected entity are hidden.");
    }

    // ---- add component (to every selected entity lacking it) ----
    ImGui::Separator();
    if (ImGui::Button(multi ? "Add Component to Selection" : "Add Component", ImVec2(-1, 0))) {
        ImGui::OpenPopup("AddComponent");
    }
    if (ImGui::BeginPopup("AddComponent")) {
        auto item = [&]<class T>(const char* label, std::type_identity<T>, auto&& add) {
            if (ImGui::MenuItem(label, nullptr, false, !all_have<T>(w, sel))) {
                for (const Entity x : sel) {
                    if (!w.has<T>(x)) {
                        add(x);
                    }
                }
                defer_commit("Add component");
            }
        };
        item("Mesh Renderer", std::type_identity<MeshRendererComponent>{}, [&](Entity x) {
            MeshRendererComponent mr;
            mr.mesh     = gameplay::builtin_mesh_id(gameplay::BuiltinMesh::Cube);
            mr.material = gameplay::default_material_id();
            w.add<MeshRendererComponent>(x, mr);
        });
        item("Light", std::type_identity<LightComponent>{}, [&](Entity x) { w.add<LightComponent>(x); });
        item("Camera", std::type_identity<CameraComponent>{}, [&](Entity x) { w.add<CameraComponent>(x); });
        item("GI Volume", std::type_identity<gameplay::GIVolumeComponent>{},
             [&](Entity x) { w.add<gameplay::GIVolumeComponent>(x); });
        item("Reflection Probe", std::type_identity<gameplay::ReflectionProbeComponent>{},
             [&](Entity x) { w.add<gameplay::ReflectionProbeComponent>(x); });
        item("Fly Camera Controller", std::type_identity<gameplay::FlyCameraComponent>{},
             [&](Entity x) { w.add<gameplay::FlyCameraComponent>(x); });
        ImGui::Separator();
        item("Rigid Body", std::type_identity<RigidBodyComponent>{}, [&](Entity x) { w.add<RigidBodyComponent>(x); });
        item("Collider", std::type_identity<ColliderComponent>{}, [&](Entity x) {
            ColliderComponent c;
            if (const auto* mr = w.try_get<MeshRendererComponent>(x); mr != nullptr && render_cache() != nullptr) {
                if (const auto* mesh = render_cache()->mesh(mr->mesh)) {
                    c.half_extents = glm::max(mesh->bounds.extent(), Vec3(0.005f));
                    c.local_offset = mesh->bounds.center();
                }
            }
            w.add<ColliderComponent>(x, c);
        });
        item("Character Controller", std::type_identity<CharacterControllerComponent>{},
             [&](Entity x) { w.add<CharacterControllerComponent>(x); });
        ImGui::Separator();
        item("Script", std::type_identity<scripting::ScriptComponent>{}, [&](Entity x) { w.add<scripting::ScriptComponent>(x); });
        ImGui::EndPopup();
    }
    finish();
}

} // namespace aether::editor
