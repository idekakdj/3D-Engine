// panel_hierarchy.cpp — scene tree: selection, drag & drop reparenting, context menu.
#include "editor_app.h"

#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/id.h"
#include "aether/scene/visibility.h"
#include "aether/scene/world.h"

#include <imgui.h>

#include <format>
#include <functional>

namespace aether::editor {

namespace {
constexpr const char* kEntityPayload = "AE_ENTITY_UUID";
}

void EditorApp::draw_hierarchy() {
    if (!ImGui::Begin("Hierarchy")) {
        ImGui::End();
        return;
    }
    World&       w   = world();
    const Entity sel = selected();

    std::function<void(Entity)> draw_node = [&](Entity e) {
        const u64   uuid     = scene::uuid_of(w, e);
        const auto* name     = w.try_get<NameComponent>(e);
        const bool  leaf     = scene::child_count_of(w, e) == 0;
        const bool  visible  = scene::is_visible_in_hierarchy(w, e);
        ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (leaf) {
            f |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        if (e == sel) {
            f |= ImGuiTreeNodeFlags_Selected;
        }
        if (sel != kNullEntity && scene::is_ancestor(w, e, sel)) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        }
        if (!visible) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        }
        const std::string label = std::format("{}##{:x}", name != nullptr ? name->name : std::string("Entity"), uuid);
        const bool        open  = ImGui::TreeNodeEx(label.c_str(), f);
        if (!visible) {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            select(e);
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kEntityPayload, &uuid, sizeof(uuid));
            ImGui::TextUnformatted(name != nullptr ? name->name.c_str() : "Entity");
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload)) {
                const u64 child = *static_cast<const u64*>(p->Data);
                if (child != uuid) {
                    pending_reparent_.emplace_back(child, uuid);
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem()) {
            select(e);
            if (ImGui::MenuItem("Create Empty Child")) {
                create_entity(CreateKind::Empty, e);
            }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
                duplicate_entity(e);
            }
            if (ImGui::MenuItem("Unparent", nullptr, false, scene::parent_of(w, e) != kNullEntity)) {
                pending_reparent_.emplace_back(uuid, 0);
            }
            if (ImGui::MenuItem("Focus", "F")) {
                focus(e);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del")) {
                pending_delete_.push_back(uuid);
            }
            ImGui::EndPopup();
        }
        if (open && !leaf) {
            scene::for_each_child(w, e, [&](Entity c) { draw_node(c); });
            ImGui::TreePop();
        }
    };
    scene::for_each_root(w, [&](Entity r) { draw_node(r); });

    // Empty space: drop to unparent, click to deselect, right-click to create.
    const ImVec2 rest = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##hierarchy_rest", ImVec2(std::max(rest.x, 1.0f), std::max(rest.y, 32.0f)));
    if (ImGui::IsItemClicked()) {
        select(kNullEntity);
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            pending_reparent_.emplace_back(*static_cast<const u64*>(p->Data), 0);
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##hierarchy_ctx")) {
        if (ImGui::MenuItem("Create Empty")) create_entity(CreateKind::Empty);
        if (ImGui::MenuItem("Create Cube")) create_entity(CreateKind::Cube);
        if (ImGui::MenuItem("Create Point Light")) create_entity(CreateKind::PointLight);
        ImGui::EndPopup();
    }
    ImGui::End();
}

} // namespace aether::editor
