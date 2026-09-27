// panel_main.cpp — dock layout, main menu, toolbar, shortcuts and the open/save dialog.
#include "editor_app.h"

#include "aether/core/input.h"
#include "aether/editor/prefab.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/world.h"

#include <imgui.h> // before ImGuizmo.h, which does not include it
#include <ImGuizmo.h>
#include <imgui_internal.h> // DockBuilder

#include <algorithm>
#include <cstdio>

namespace aether::editor {

void EditorApp::draw_dockspace() {
    const ImGuiID dock_id = ImGui::DockSpaceOverViewport(ImGui::GetID("EditorDockSpace"), ImGui::GetMainViewport());
    if (dock_built_) {
        return;
    }
    dock_built_ = true;
    // Default layout on first use (an imgui.ini layout, if any, has already been applied).
    ImGuiDockNode* node = ImGui::DockBuilderGetNode(dock_id);
    if (node != nullptr && node->IsSplitNode()) {
        return;
    }
    ImGui::DockBuilderRemoveNode(dock_id);
    ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock_id, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dock_id;
    ImGuiID left   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    ImGuiID top    = ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.06f, nullptr, &center);
    ImGuiID right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.35f, nullptr, &right);
    ImGui::DockBuilderDockWindow("Toolbar", top);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Material", right);
    ImGui::DockBuilderDockWindow("Engine", right_bottom);
    ImGui::DockBuilderDockWindow("Animation", right_bottom);
    ImGui::DockBuilderDockWindow("Assets", bottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderFinish(dock_id);
}

void EditorApp::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    const bool editing = play_state_ == PlayState::Edit;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene", "Ctrl+N", false, editing)) {
            new_scene();
        }
        if (ImGui::MenuItem("Open...", "Ctrl+O", false, editing)) {
            file_dialog_      = FileDialog::Open;
            file_dialog_path_ = scene_path_.empty() ? "scenes/untitled.aescene"
                                                    : scene_path_.lexically_relative(content_root()).generic_string();
        }
        if (ImGui::MenuItem("Save", "Ctrl+S", false, editing)) {
            if (scene_path_.empty()) {
                file_dialog_      = FileDialog::SaveAs;
                file_dialog_path_ = "scenes/untitled.aescene";
            } else {
                save_scene(scene_path_);
            }
        }
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, editing)) {
            file_dialog_      = FileDialog::SaveAs;
            file_dialog_path_ = scene_path_.empty() ? "scenes/untitled.aescene"
                                                    : scene_path_.lexically_relative(content_root()).generic_string();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            request_exit(0);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const std::string undo_label = "Undo " + history_.next_undo_label();
        const std::string redo_label = "Redo " + history_.next_redo_label();
        if (ImGui::MenuItem(undo_label.c_str(), "Ctrl+Z", false, editing && history_.can_undo())) {
            undo();
        }
        if (ImGui::MenuItem(redo_label.c_str(), "Ctrl+Y", false, editing && history_.can_redo())) {
            redo();
        }
        ImGui::Separator();
        const bool has_sel = selected() != kNullEntity;
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, has_sel)) {
            duplicate_selection();
        }
        if (ImGui::MenuItem("Delete", "Del", false, has_sel)) {
            delete_selection();
        }
        if (ImGui::MenuItem("Focus", "F", false, has_sel)) {
            focus(selected());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", "Ctrl+A")) {
            std::vector<Entity> all;
            scene::for_each_in_hierarchy(world(), [&](Entity e) { all.push_back(e); });
            select_entities(all);
        }
        if (ImGui::MenuItem("Deselect", nullptr, false, has_sel)) {
            select(kNullEntity);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Create Prefab from Selection...", nullptr, false, has_sel && editing)) {
            file_dialog_      = FileDialog::SavePrefab;
            file_dialog_path_ = "prefabs/" + world().get<NameComponent>(selected()).name + ".aeprefab";
        }
        const Entity primary = selected();
        const bool   instance = has_sel && world().has<PrefabInstanceComponent>(primary);
        if (ImGui::MenuItem("Revert to Prefab", nullptr, false, instance && editing)) {
            revert_prefab(primary);
        }
        if (ImGui::MenuItem("Apply to Prefab", nullptr, false, instance && editing)) {
            apply_prefab(primary);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Create")) {
        const Entity parent = ImGui::GetIO().KeyShift ? selected() : kNullEntity;
        if (ImGui::MenuItem("Empty")) create_entity(CreateKind::Empty, parent);
        ImGui::Separator();
        if (ImGui::MenuItem("Cube")) create_entity(CreateKind::Cube, parent);
        if (ImGui::MenuItem("Sphere")) create_entity(CreateKind::Sphere, parent);
        if (ImGui::MenuItem("Plane")) create_entity(CreateKind::Plane, parent);
        if (ImGui::MenuItem("Capsule")) create_entity(CreateKind::Capsule, parent);
        ImGui::Separator();
        if (ImGui::MenuItem("Directional Light")) create_entity(CreateKind::DirectionalLight, parent);
        if (ImGui::MenuItem("Point Light")) create_entity(CreateKind::PointLight, parent);
        if (ImGui::MenuItem("Spot Light")) create_entity(CreateKind::SpotLight, parent);
        ImGui::Separator();
        if (ImGui::MenuItem("Camera")) create_entity(CreateKind::Camera, parent);
        ImGui::TextDisabled("(hold Shift to create under the selection)");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Assets", nullptr, &show_assets_);
        ImGui::MenuItem("Material", nullptr, &show_material_);
        ImGui::MenuItem("Animation", nullptr, &show_animation_);
        ImGui::MenuItem("Console", nullptr, &show_console_);
        ImGui::MenuItem("Engine stats", nullptr, &show_stats_);
        ImGui::Separator();
        ImGui::MenuItem("Game camera in play mode", nullptr, &use_game_camera_);
        ImGui::SliderFloat("Editor FOV", &fov_y_deg_, 20.0f, 110.0f, "%.0f deg");
        ImGui::SliderFloat("Fly speed", &fly_.move_speed, fly_.min_speed, 100.0f, "%.1f m/s", ImGuiSliderFlags_Logarithmic);
        ImGui::EndMenu();
    }
    // Scene name + dirty marker on the right.
    const std::string title = (scene_path_.empty() ? std::string("untitled") : scene_path_.filename().string()) +
                              (history_.dirty() ? " *" : "");
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(title.c_str()).x - 16.0f);
    ImGui::TextDisabled("%s", title.c_str());
    ImGui::EndMainMenuBar();
}

void EditorApp::draw_toolbar() {
    ImGuiWindowClass wc;
    wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&wc);
    ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar);

    auto toggle = [](const char* label, bool active) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        const bool pressed = ImGui::Button(label);
        if (active) {
            ImGui::PopStyleColor();
        }
        return pressed;
    };
    if (toggle("Move (W)", gizmo_operation_ == 0)) gizmo_operation_ = 0;
    ImGui::SameLine();
    if (toggle("Rotate (E)", gizmo_operation_ == 1)) gizmo_operation_ = 1;
    ImGui::SameLine();
    if (toggle("Scale (R)", gizmo_operation_ == 2)) gizmo_operation_ = 2;
    ImGui::SameLine();
    if (ImGui::Button(gizmo_local_ ? "Local" : "World")) gizmo_local_ = !gizmo_local_;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    int pivot = static_cast<int>(pivot_mode_);
    if (ImGui::Combo("##pivot", &pivot, "Center\0Primary\0")) {
        pivot_mode_ = static_cast<PivotMode>(pivot);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Group pivot: bounds centre of the selection, or the primary (last selected) entity");
    }
    // Grid + snapping (translate snap is absolute on the world grid; rotate / scale incremental).
    auto snap_field = [](const char* id, bool* on, f32* step, f32 speed, f32 lo, f32 hi, const char* fmt, const char* tip) {
        ImGui::SameLine();
        ImGui::Checkbox(id, on);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tip);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(58.0f);
        ImGui::PushID(id);
        ImGui::DragFloat("##step", step, speed, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopID();
    };
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &snap_.show_grid);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(58.0f);
    ImGui::DragFloat("##grid", &snap_.grid_spacing, 0.05f, 0.05f, 100.0f, "%.2fm", ImGuiSliderFlags_AlwaysClamp);
    snap_field("Snap T", &snap_.translate, &snap_.translate_step, 0.01f, 0.01f, 100.0f, "%.2fm", "Translation snap (world grid)");
    snap_field("Snap R", &snap_.rotate, &snap_.rotate_step, 0.5f, 0.5f, 180.0f, "%.0f deg", "Rotation snap increment");
    snap_field("Snap S", &snap_.scale, &snap_.scale_step, 0.01f, 0.01f, 10.0f, "%.2f", "Scale snap increment");

    // Play controls, centred.
    const f32 controls_width = 230.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20.0f, (ImGui::GetWindowWidth() - controls_width) * 0.5f));
    const bool editing = play_state_ == PlayState::Edit;
    if (editing) {
        if (ImGui::Button("  Play  ")) play();
    } else {
        if (ImGui::Button("  Stop  ")) stop();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(editing);
    if (ImGui::Button(play_state_ == PlayState::Paused ? "Resume" : "Pause")) toggle_pause();
    ImGui::SameLine();
    if (ImGui::Button("Step")) step();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(editing ? ImVec4(0.6f, 0.6f, 0.6f, 1) : ImVec4(0.3f, 0.9f, 0.4f, 1), "%s",
                       editing ? "Editing" : (play_state_ == PlayState::Paused ? "Paused" : "Playing"));
    ImGui::End();
}

void EditorApp::handle_shortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) {
        return;
    }
    const bool ctrl    = io.KeyCtrl;
    const bool editing = play_state_ == PlayState::Edit;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && editing) {
        io.KeyShift ? redo() : undo();
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false) && editing) {
        redo();
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && editing) {
        if (scene_path_.empty() || io.KeyShift) {
            file_dialog_      = FileDialog::SaveAs;
            file_dialog_path_ = "scenes/untitled.aescene";
        } else {
            save_scene(scene_path_);
        }
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false) && editing) {
        new_scene();
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false) && editing) {
        file_dialog_      = FileDialog::Open;
        file_dialog_path_ = "scenes/untitled.aescene";
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && selected() != kNullEntity) {
        duplicate_selection();
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        std::vector<Entity> all;
        scene::for_each_in_hierarchy(world(), [&](Entity e) { all.push_back(e); });
        select_entities(all);
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        editing ? play() : stop();
    }
    if (!ctrl && !looking_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && selected() != kNullEntity) {
            delete_selection();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false) && selected() != kNullEntity) {
            focus(selected());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmo_operation_ = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmo_operation_ = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) gizmo_operation_ = 2;
    }
}

void EditorApp::draw_file_dialog() {
    if (file_dialog_ == FileDialog::None) {
        return;
    }
    const bool  prefab = file_dialog_ == FileDialog::SavePrefab;
    const char* title  = file_dialog_ == FileDialog::Open ? "Open Scene" : prefab ? "Create Prefab" : "Save Scene As";
    if (!ImGui::IsPopupOpen(title)) {
        ImGui::OpenPopup(title);
    }
    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Path relative to %s", content_root().generic_string().c_str());
        char buf[512] = {};
        std::snprintf(buf, sizeof(buf), "%s", file_dialog_path_.c_str());
        ImGui::SetNextItemWidth(420.0f);
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        const bool enter = ImGui::InputText("##path", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue);
        file_dialog_path_ = buf;
        if (ImGui::Button(file_dialog_ == FileDialog::Open ? "Open" : prefab ? "Create" : "Save") || enter) {
            std::filesystem::path p = file_dialog_path_;
            if (p.extension().empty()) {
                p += prefab ? std::string(kPrefabExtension) : std::string(".aescene");
            }
            const bool ok = file_dialog_ == FileDialog::Open ? open_scene(p) : prefab ? create_prefab(p) : save_scene(p);
            if (ok) {
                file_dialog_ = FileDialog::None;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            file_dialog_ = FileDialog::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace aether::editor
