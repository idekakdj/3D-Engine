// panel_projects.cpp — File > Projects: open recent / found / by path, create new (ADR-0014).
//
// Switching project restarts the editor on the chosen .aeproject (a new process started with
// --project; this one exits), the same model Unity / Unreal use: the asset database, the script VM
// and every cache are bound to the project's folders. Unsaved scene changes are offered for saving
// first.
#include "editor_app.h"

#include "aether/core/log.h"
#include "aether/core/paths.h"

#include <imgui.h>

#include <cstdio>

namespace aether::editor {

namespace {
bool input_text(const char* label, std::string& value, f32 width) {
    char buf[512] = {};
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::InputText(label, buf, sizeof(buf));
    value = buf;
    return changed;
}

std::filesystem::path u8path(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
} // namespace

bool EditorApp::switch_project(const std::filesystem::path& manifest) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(manifest, ec)) {
        projects_error_ = "No project file at " + manifest.generic_string();
        return false;
    }
    recent_.add(manifest);
    if (auto s = recent_.save(); !s) {
        AE_LOG_WARN("Editor", "{}", s.error().message);
    }
    const Launcher& launch = launcher_ ? launcher_ : Launcher(runtime::launch_detached);
    if (auto r = launch(runtime::executable_path(), { "--project", manifest.string() }); !r) {
        projects_error_ = r.error().message;
        AE_LOG_ERROR("Editor", "cannot reopen the editor on {}: {}", manifest.generic_string(), r.error().message);
        return false;
    }
    AE_LOG_INFO("Editor", "reopening on project {}", manifest.generic_string());
    if (!options_.self_test) {
        request_exit(0);
    }
    return true;
}

bool EditorApp::create_and_open_project(const std::filesystem::path& parent, const std::string& name,
                                        runtime::ProjectTemplate tmpl) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    auto manifest = runtime::create_project(parent, name, tmpl, paths::engine_root() / "content");
    if (!manifest) {
        projects_error_ = manifest.error().message;
        return false;
    }
    return switch_project(*manifest);
}

void EditorApp::open_projects_window(bool new_tab) {
    show_projects_    = true;
    projects_new_tab_ = new_tab;
    projects_error_.clear();
    recent_.load();
    found_projects_ = runtime::find_projects(runtime::projects_dir());
    if (new_project_location_.empty()) {
        new_project_location_ = runtime::projects_dir().string();
    }
}

void EditorApp::draw_projects_window() {
    // ---- unsaved changes before a switch ----
    if (!pending_switch_.empty()) {
        if (!ImGui::IsPopupOpen("Unsaved changes")) {
            ImGui::OpenPopup("Unsaved changes");
        }
        if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("The current scene has unsaved changes.");
            ImGui::TextDisabled("Opening another project restarts the editor.");
            bool go = false;
            if (ImGui::Button(scene_path_.empty() ? "Save As..." : "Save and switch")) {
                if (scene_path_.empty()) {
                    file_dialog_      = FileDialog::SaveAs;
                    file_dialog_path_ = "scenes/untitled.aescene";
                    pending_switch_.clear();
                } else {
                    go = save_scene(scene_path_);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard and switch")) {
                go = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                pending_switch_.clear();
                ImGui::CloseCurrentPopup();
            }
            if (go) {
                const std::filesystem::path target = pending_switch_;
                pending_switch_.clear();
                switch_project(target);
            }
            ImGui::EndPopup();
        }
    }
    if (!show_projects_) {
        return;
    }
    auto request_switch = [&](const std::filesystem::path& m) {
        if (play_state_ != PlayState::Edit) {
            stop();
        }
        if (history_.dirty()) {
            pending_switch_ = m; // ask first
        } else {
            switch_project(m);
        }
    };

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620.0f, 440.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin("Projects", &show_projects_, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Current project:");
    ImGui::SameLine();
    ImGui::TextUnformatted(options_.project_name.empty() ? "(engine content folder)" : options_.project_name.c_str());
    ImGui::Separator();

    if (ImGui::BeginTabBar("##projtabs")) {
        const ImGuiTabItemFlags open_flags = projects_new_tab_ ? 0 : ImGuiTabItemFlags_SetSelected;
        const ImGuiTabItemFlags new_flags  = projects_new_tab_ ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("Open", nullptr, ImGui::IsWindowAppearing() ? open_flags : 0)) {
            auto project_row = [&](const std::string& name, const std::filesystem::path& manifest, int id) {
                ImGui::PushID(id);
                const bool current = !options_.project_file.empty() && manifest == options_.project_file;
                if (ImGui::Selectable(name.c_str(), current, ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !current) {
                    request_switch(manifest);
                }
                ImGui::SameLine(220.0f);
                ImGui::TextDisabled("%s", manifest.parent_path().generic_string().c_str());
                ImGui::PopID();
            };
            ImGui::TextUnformatted("Recent");
            if (recent_.entries().empty()) {
                ImGui::TextDisabled("  (none yet)");
            }
            int id = 0;
            for (const auto& m : recent_.entries()) {
                project_row(m.stem().string(), m, id++);
            }
            ImGui::Spacing();
            ImGui::Text("In %s", runtime::projects_dir().generic_string().c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Refresh")) {
                found_projects_ = runtime::find_projects(runtime::projects_dir());
            }
            if (found_projects_.empty()) {
                ImGui::TextDisabled("  (none)");
            }
            for (const auto& p : found_projects_) {
                project_row(p.name, p.manifest, id++);
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted("Open a project file (.aeproject):");
            input_text("##openpath", open_project_path_, 470.0f);
            ImGui::SameLine();
            if (ImGui::Button("Open")) {
                request_switch(u8path(open_project_path_));
            }
            ImGui::TextDisabled("Double-click a project to open it (the editor restarts on it).");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("New", nullptr, ImGui::IsWindowAppearing() ? new_flags : 0)) {
            ImGui::TextUnformatted("Name");
            input_text("##newname", new_project_name_, 300.0f);
            const bool name_ok = runtime::valid_project_name(new_project_name_);
            if (!name_ok) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "invalid name");
            }
            ImGui::TextUnformatted("Location");
            input_text("##newloc", new_project_location_, 470.0f);
            ImGui::TextUnformatted("Template");
            ImGui::RadioButton("Starter content (showcase scene, sample models and scripts)", &new_project_template_, 0);
            ImGui::RadioButton("Empty (a camera, a sun and a floor)", &new_project_template_, 1);
            const std::filesystem::path folder = u8path(new_project_location_) / u8path(new_project_name_);
            ImGui::TextDisabled("Creates %s", folder.generic_string().c_str());
            ImGui::BeginDisabled(!name_ok || new_project_location_.empty());
            if (ImGui::Button("Create and open")) {
                if (history_.dirty()) {
                    // Create now, switch after the unsaved-changes answer.
                    auto m = runtime::create_project(u8path(new_project_location_), new_project_name_,
                                                     new_project_template_ == 0 ? runtime::ProjectTemplate::Starter
                                                                                : runtime::ProjectTemplate::Empty,
                                                     paths::engine_root() / "content");
                    if (m) {
                        pending_switch_ = *m;
                    } else {
                        projects_error_ = m.error().message;
                    }
                } else {
                    create_and_open_project(u8path(new_project_location_), new_project_name_,
                                            new_project_template_ == 0 ? runtime::ProjectTemplate::Starter
                                                                       : runtime::ProjectTemplate::Empty);
                }
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!projects_error_.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", projects_error_.c_str());
    }
    ImGui::End();
}

} // namespace aether::editor
