// panel_hierarchy.cpp — scene tree: multi-selection (Ctrl toggle, Shift add), drag & drop
// reparenting of the whole selection, asset drops (as children), context menu. Prefab instances
// are tinted.
#include "editor_app.h"

#include "aether/editor/prefab.h"
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
constexpr const char* kAssetPayload  = "AE_ASSET_PATH";
} // namespace

void EditorApp::draw_hierarchy() {
    if (!ImGui::Begin("Hierarchy")) {
        ImGui::End();
        return;
    }
    World&         w       = world();
    const Entity   primary = selected();
    const ImGuiIO& io      = ImGui::GetIO();

    // Drop handling shared by nodes and the empty space (target uuid 0 = root level).
    auto accept_drops = [&](u64 target_uuid) {
        if (!ImGui::BeginDragDropTarget()) {
            return;
        }
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            const u64    dragged = *static_cast<const u64*>(p->Data);
            const Entity de      = scene::find_by_uuid(w, dragged);
            if (de != kNullEntity && is_selected(de)) {
                for (const Entity r : selection_roots(w, selection())) { // move the whole selection
                    const u64 ru = scene::uuid_of(w, r);
                    if (ru != target_uuid) {
                        pending_reparent_.emplace_back(ru, target_uuid);
                    }
                }
            } else if (dragged != target_uuid) {
                pending_reparent_.emplace_back(dragged, target_uuid);
            }
        }
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            pending_asset_drops_.emplace_back(std::string(static_cast<const char*>(p->Data)), target_uuid);
        }
        ImGui::EndDragDropTarget();
    };

    std::function<void(Entity)> draw_node = [&](Entity e) {
        const u64   uuid     = scene::uuid_of(w, e);
        const auto* name     = w.try_get<NameComponent>(e);
        const bool  leaf     = scene::child_count_of(w, e) == 0;
        const bool  visible  = scene::is_visible_in_hierarchy(w, e);
        const bool  prefab   = w.has<PrefabInstanceComponent>(e);
        ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (leaf) {
            f |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        if (selection_.contains(uuid)) {
            f |= ImGuiTreeNodeFlags_Selected;
        }
        if (primary != kNullEntity && scene::is_ancestor(w, e, primary)) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        }
        const int colors = (!visible ? 1 : 0) + (prefab && visible ? 1 : 0);
        if (!visible) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        } else if (prefab) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.75f, 1.0f, 1.0f));
        }
        const std::string label = std::format("{}##{:x}", name != nullptr ? name->name : std::string("Entity"), uuid);
        const bool        open  = ImGui::TreeNodeEx(label.c_str(), f);
        ImGui::PopStyleColor(colors);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            select_click(e, select_mode_from(io.KeyCtrl, io.KeyShift));
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kEntityPayload, &uuid, sizeof(uuid));
            if (selection_.contains(uuid) && selection_.size() > 1) {
                ImGui::Text("%zu entities", selection_.size());
            } else {
                ImGui::TextUnformatted(name != nullptr ? name->name.c_str() : "Entity");
            }
            ImGui::EndDragDropSource();
        }
        accept_drops(uuid);
        if (ImGui::BeginPopupContextItem()) {
            if (!selection_.contains(uuid)) {
                select(e); // right-click outside the selection selects just this entity
            }
            if (ImGui::MenuItem("Create Empty Child")) {
                create_entity(CreateKind::Empty, e);
            }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
                duplicate_selection();
            }
            if (ImGui::MenuItem("Unparent", nullptr, false, scene::parent_of(w, e) != kNullEntity)) {
                for (const Entity r : selection_roots(w, selection())) {
                    pending_reparent_.emplace_back(scene::uuid_of(w, r), 0);
                }
            }
            if (ImGui::MenuItem("Focus", "F")) {
                focus(e);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Create Prefab from Selection...")) {
                file_dialog_      = FileDialog::SavePrefab;
                file_dialog_path_ = std::format("prefabs/{}.aeprefab", name != nullptr ? name->name : std::string("prefab"));
            }
            if (prefab) {
                if (ImGui::MenuItem("Revert to Prefab")) {
                    pending_prefab_ops_.emplace_back(uuid, false);
                }
                if (ImGui::MenuItem("Apply to Prefab")) {
                    pending_prefab_ops_.emplace_back(uuid, true);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del")) {
                pending_delete_selection_ = true;
            }
            ImGui::EndPopup();
        }
        if (open && !leaf) {
            scene::for_each_child(w, e, [&](Entity c) { draw_node(c); });
            ImGui::TreePop();
        }
    };
    scene::for_each_root(w, [&](Entity r) { draw_node(r); });

    // Empty space: drop to unparent / instantiate at root level, click to deselect, right-click to create.
    const ImVec2 rest = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##hierarchy_rest", ImVec2(std::max(rest.x, 1.0f), std::max(rest.y, 32.0f)));
    if (ImGui::IsItemClicked() && !io.KeyCtrl && !io.KeyShift) {
        select(kNullEntity);
    }
    accept_drops(0);
    if (ImGui::BeginPopupContextItem("##hierarchy_ctx")) {
        if (ImGui::MenuItem("Create Empty")) create_entity(CreateKind::Empty);
        if (ImGui::MenuItem("Create Cube")) create_entity(CreateKind::Cube);
        if (ImGui::MenuItem("Create Point Light")) create_entity(CreateKind::PointLight);
        ImGui::EndPopup();
    }
    ImGui::End();
}

} // namespace aether::editor
