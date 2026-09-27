// panel_material.cpp — the material editor (material instances with live preview + undo) and the
// read-only animation graph view.
#include "editor_app.h"

#include "aether/animation/anim_graph.h"
#include "aether/animation/components.h"
#include "aether/assets/asset_manager.h"
#include "aether/core/log.h"
#include "aether/editor/anim_view.h"
#include "aether/editor/material_instance.h"
#include "aether/gameplay/components.h"
#include "aether/gameplay/procedural_mesh.h"
#include "aether/gameplay/render_bridge.h"
#include "aether/scene/components.h"
#include "aether/scene/world.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <format>

namespace aether::editor {

namespace {

std::string material_label(const AssetId& id) {
    if (const auto b = gameplay::builtin_material_from_id(id)) {
        return std::string("Built-in: ") + gameplay::builtin_material_name(*b);
    }
    if (is_material_instance_id(id)) {
        return "Instance";
    }
    return id.is_valid() ? "Asset " + id.to_string().substr(0, 12) + "..." : std::string("(mesh material)");
}

} // namespace

void EditorApp::draw_material_editor() {
    if (!ImGui::Begin("Material", &show_material_)) {
        ImGui::End();
        return;
    }
    World&       w  = world();
    const Entity e  = selected();
    auto*        mr = e != kNullEntity ? w.try_get<MeshRendererComponent>(e) : nullptr;
    if (mr == nullptr) {
        ImGui::TextDisabled("Select an entity with a Mesh Renderer.");
        ImGui::End();
        return;
    }

    // ---- slot ----
    std::vector<i32> slots{ kPrimaryMaterialSlot };
    if (gameplay::RenderResourceCache* cache = render_cache()) {
        if (const auto* mesh = cache->mesh(mr->mesh); mesh != nullptr && mesh->submeshes.size() > 1) {
            for (const Submesh& sm : mesh->submeshes) {
                slots.push_back(static_cast<i32>(sm.material_slot));
            }
        }
    }
    if (const auto* mo = w.try_get<gameplay::MaterialOverridesComponent>(e)) {
        for (usize i = 0; i < mo->materials.size(); ++i) {
            if (mo->materials[i].is_valid()) {
                slots.push_back(static_cast<i32>(i));
            }
        }
    }
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    if (std::find(slots.begin(), slots.end(), material_slot_) == slots.end()) {
        material_slot_ = kPrimaryMaterialSlot;
    }
    auto slot_name = [](i32 s) { return s < 0 ? std::string("Mesh Renderer material") : std::format("Submesh slot {} (override)", s); };
    if (slots.size() > 1 && ImGui::BeginCombo("Slot", slot_name(material_slot_).c_str())) {
        for (const i32 s : slots) {
            if (ImGui::Selectable(slot_name(s).c_str(), s == material_slot_)) {
                material_slot_ = s;
            }
        }
        ImGui::EndCombo();
    }
    const i32     slot    = material_slot_;
    const AssetId current = material_slot_target(w, e, slot);
    ImGui::Text("%s", material_label(current).c_str());

    // ---- assignment ----
    if (ImGui::BeginCombo("Assign", "choose...")) {
        if (slot >= 0 && ImGui::Selectable("(mesh material)")) {
            remove_material_instance(w, e, slot, AssetId{});
            set_material_slot_target(w, e, slot, AssetId{});
            record_edit("Material");
        }
        for (u8 i = 0; i < static_cast<u8>(gameplay::BuiltinMaterial::Count); ++i) {
            const auto m = static_cast<gameplay::BuiltinMaterial>(i);
            if (ImGui::Selectable(gameplay::builtin_material_name(m))) {
                remove_material_instance(w, e, slot, gameplay::builtin_material_id(m));
                set_material_slot_target(w, e, slot, gameplay::builtin_material_id(m));
                record_edit("Material");
            }
        }
        ImGui::EndCombo();
    }

    auto* mi = w.try_get<MaterialInstanceComponent>(e);
    auto* si = mi != nullptr ? mi->find(slot) : nullptr;
    if (si == nullptr) {
        ImGui::Spacing();
        if (ImGui::Button("Create Material Instance", ImVec2(-1, 0))) {
            make_material_instance(e, slot);
        }
        ImGui::TextDisabled("Copies the current material into editable, scene-saved parameters.");
        ImGui::End();
        return;
    }

    // ---- instance parameters (live preview; drags are one undo step each) ----
    assets::MaterialData d       = si->data;
    bool                 changed = false; // discrete
    bool                 dragged = false; // continuous
    auto cont = [&](bool c) {
        dragged |= c;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            defer_end();
        }
    };
    char name[128] = {};
    std::snprintf(name, sizeof(name), "%s", d.name.c_str());
    if (ImGui::InputText("Name", name, sizeof(name))) {
        d.name  = name;
        dragged = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        defer_end();
    }
    ImGui::SeparatorText("Surface");
    cont(ImGui::ColorEdit4("Base color", &d.base_color_factor.x, ImGuiColorEditFlags_Float));
    cont(ImGui::SliderFloat("Metallic", &d.metallic_factor, 0.0f, 1.0f));
    cont(ImGui::SliderFloat("Roughness", &d.roughness_factor, 0.0f, 1.0f));
    cont(ImGui::ColorEdit3("Emissive", &d.emissive_factor.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR));
    f32 strength = std::max({ d.emissive_factor.x, d.emissive_factor.y, d.emissive_factor.z });
    if (strength > 0.0f) {
        f32 s = strength;
        if (ImGui::DragFloat("Emissive strength", &s, 0.05f, 0.0f, 1000.0f)) {
            d.emissive_factor *= s / strength;
            dragged = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            defer_end();
        }
    }
    cont(ImGui::DragFloat("Normal scale", &d.normal_scale, 0.01f, 0.0f, 4.0f));
    cont(ImGui::SliderFloat("Occlusion", &d.occlusion_strength, 0.0f, 1.0f));
    int alpha = static_cast<int>(d.alpha_mode);
    if (ImGui::Combo("Alpha", &alpha, "Opaque\0Mask\0Blend\0")) {
        d.alpha_mode = static_cast<assets::AlphaMode>(alpha);
        changed      = true;
    }
    if (d.alpha_mode == assets::AlphaMode::Mask) {
        cont(ImGui::SliderFloat("Alpha cutoff", &d.alpha_cutoff, 0.0f, 1.0f));
    }
    changed |= ImGui::Checkbox("Double sided", &d.double_sided);

    ImGui::SeparatorText("Textures (asset ids; drop an image from Assets)");
    struct TexField {
        const char* label;
        AssetId*    id;
    };
    const TexField fields[] = { { "Base color##tex", &d.base_color_texture },
                                { "Metal/rough##tex", &d.metallic_roughness_texture },
                                { "Normal##tex", &d.normal_texture },
                                { "Occlusion##tex", &d.occlusion_texture },
                                { "Emissive##tex", &d.emissive_texture } };
    for (const TexField& f : fields) {
        ImGui::PushID(f.label);
        char buf[40] = {};
        std::snprintf(buf, sizeof(buf), "%s", f.id->is_valid() ? f.id->to_string().c_str() : "");
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::InputTextWithHint(f.label, "32 hex digits", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            AssetId parsed;
            if (parse_asset_id(buf, parsed)) {
                *f.id   = parsed;
                changed = true;
            }
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AE_ASSET_PATH")) {
                const AssetId id = assets().resolve(static_cast<const char*>(p->Data));
                if (id.is_valid()) {
                    *f.id   = id;
                    changed = true;
                } else {
                    AE_LOG_WARN("Editor", "{} is not an imported texture", static_cast<const char*>(p->Data));
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            *f.id   = AssetId{};
            changed = true;
        }
        ImGui::PopID();
    }

    if (dragged || changed) {
        edit_material(e, slot, d, /*continuous=*/!changed);
    }

    ImGui::Separator();
    const std::vector<Entity> sel = selection();
    if (sel.size() > 1) {
        const std::string label = std::format("Apply to selection ({} entities)", sel.size());
        if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) {
            for (const Entity x : sel) {
                if (x != e && w.has<MeshRendererComponent>(x)) {
                    editor::make_material_instance(w, x, slot, d);
                }
            }
            record_edit("Material to selection");
        }
    }
    if (ImGui::Button("Remove Instance", ImVec2(-1, 0))) {
        remove_material_instance(w, e, slot, slot < 0 ? gameplay::default_material_id() : AssetId{});
        record_edit("Remove material instance");
    }
    flush_deferred_edits();
    ImGui::End();
}

void EditorApp::draw_animation_panel() {
    if (!ImGui::Begin("Animation", &show_animation_)) {
        ImGui::End();
        return;
    }
    const Entity e = selected();
    auto*        a = e != kNullEntity ? world().try_get<animation::AnimatorComponent>(e) : nullptr;
    if (a == nullptr) {
        ImGui::TextDisabled("Select an entity with an Animator.");
        ImGui::End();
        return;
    }
    const AnimatorView v = describe_animator(a->graph_instance());
    if (!v.valid) {
        ImGui::TextDisabled("No graph bound yet (bound once its assets load).");
        ImGui::End();
        return;
    }
    ImGui::Text("%u node(s), root node %u, %s", v.node_count, v.root, a->paused() ? "paused" : "playing");

    if (!v.parameters.empty() && ImGui::BeginTable("##params", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Parameter");
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        for (const AnimParamView& p : v.parameters) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", p.kind == AnimParamKind::Float ? "float" : p.kind == AnimParamKind::Bool ? "bool" : "trigger");
            ImGui::TableNextColumn();
            if (p.kind == AnimParamKind::Float) {
                ImGui::Text("%.3f", p.value);
            } else {
                ImGui::TextUnformatted(p.value != 0.0f ? (p.kind == AnimParamKind::Trigger ? "set" : "true") : "false");
            }
        }
        ImGui::EndTable();
    }
    if (v.machines.empty()) {
        ImGui::TextDisabled("No state machine (the graph is a clip / blend tree).");
    }
    for (const AnimStateMachineView& sm : v.machines) {
        ImGui::SeparatorText(std::format("State machine (node {}), weight {:.2f}", sm.node, sm.weight).c_str());
        // State boxes: current = green, transition source = orange, with the blend progress.
        ImDrawList*  dl     = ImGui::GetWindowDrawList();
        const ImVec2 start  = ImGui::GetCursorScreenPos();
        const f32    box_w  = 86.0f;
        const f32    box_h  = 30.0f;
        const f32    gap    = 14.0f;
        const f32    width  = std::max(ImGui::GetContentRegionAvail().x, box_w);
        const int    per_row = std::max(1, static_cast<int>((width + gap) / (box_w + gap)));
        ImVec2       src_center{};
        ImVec2       cur_center{};
        for (usize i = 0; i < sm.states.size(); ++i) {
            const AnimStateView& st = sm.states[i];
            const int            r  = static_cast<int>(i) / per_row;
            const int            c  = static_cast<int>(i) % per_row;
            const ImVec2 a0(start.x + static_cast<f32>(c) * (box_w + gap), start.y + static_cast<f32>(r) * (box_h + gap));
            const ImVec2 a1(a0.x + box_w, a0.y + box_h);
            const ImU32  fill = st.current ? IM_COL32(40, 140, 70, 255) : st.transition_source ? IM_COL32(170, 110, 30, 255) : IM_COL32(55, 55, 62, 255);
            dl->AddRectFilled(a0, a1, fill, 4.0f);
            dl->AddRect(a0, a1, IM_COL32(150, 150, 160, 255), 4.0f);
            // Normalised time as a thin bar along the bottom.
            const f32 t = static_cast<f32>(st.normalized_time - static_cast<f64>(static_cast<i64>(st.normalized_time)));
            dl->AddRectFilled(ImVec2(a0.x + 2, a1.y - 4), ImVec2(a0.x + 2 + (box_w - 4) * std::clamp(t, 0.0f, 1.0f), a1.y - 2),
                              IM_COL32(220, 220, 220, 180));
            const std::string label = std::format("State {}", st.id);
            dl->AddText(ImVec2(a0.x + 8, a0.y + 7), IM_COL32(235, 235, 235, 255), label.c_str());
            const ImVec2 center((a0.x + a1.x) * 0.5f, (a0.y + a1.y) * 0.5f);
            if (st.current) cur_center = center;
            if (st.transition_source) src_center = center;
        }
        if (sm.in_transition) {
            dl->AddLine(src_center, cur_center, IM_COL32(255, 200, 80, 255), 2.0f);
        }
        const int rows = (static_cast<int>(sm.states.size()) + per_row - 1) / per_row;
        ImGui::Dummy(ImVec2(width, static_cast<f32>(rows) * (box_h + gap)));
        if (sm.in_transition) {
            ImGui::Text("Transition: state %u -> state %u", sm.source, sm.current);
            ImGui::ProgressBar(sm.alpha, ImVec2(-1, 0));
        } else if (sm.current != animation::kInvalidState) {
            ImGui::Text("In state %u", sm.current);
        } else {
            ImGui::TextDisabled("Not started (runs while the simulation plays).");
        }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Read-only. State names and the transition list need animation API additions.");
    ImGui::End();
}

} // namespace aether::editor
