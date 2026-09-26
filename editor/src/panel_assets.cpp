// panel_assets.cpp — content browser and the log console.
#include "editor_app.h"

#include "aether/editor/console.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <vector>

namespace aether::editor {

namespace {

struct DirEntry {
    std::filesystem::path path; // content-relative
    bool                  directory = false;
};

std::vector<DirEntry> list_dir(const std::filesystem::path& root, const std::filesystem::path& rel) {
    std::vector<DirEntry> out;
    std::error_code       ec;
    for (const auto& it : std::filesystem::directory_iterator(root / rel, ec)) {
        const std::string name = it.path().filename().string();
        if (name.empty() || name[0] == '.') {
            continue;
        }
        out.push_back(DirEntry{ rel / it.path().filename(), it.is_directory(ec) });
    }
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        return a.directory != b.directory ? a.directory : a.path.filename() < b.path.filename();
    });
    return out;
}

const char* kind_of(const std::filesystem::path& p) {
    const std::string e = p.extension().string();
    if (e == ".gltf" || e == ".glb") return "model";
    if (e == ".aescene") return "scene";
    if (e == ".lua") return "script";
    if (e == ".png" || e == ".jpg" || e == ".hdr" || e == ".ktx2") return "image";
    if (e == ".json") return "data";
    return "file";
}

ImVec4 level_color(LogLevel l) {
    switch (l) {
    case LogLevel::Trace:
    case LogLevel::Debug: return ImVec4(0.55f, 0.55f, 0.6f, 1.0f);
    case LogLevel::Warn: return ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
    case LogLevel::Error:
    case LogLevel::Fatal: return ImVec4(1.0f, 0.4f, 0.35f, 1.0f);
    default: return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    }
}

} // namespace

void EditorApp::draw_assets() {
    if (!ImGui::Begin("Assets", &show_assets_)) {
        ImGui::End();
        return;
    }
    const std::filesystem::path root = content_root();
    // Breadcrumbs.
    if (ImGui::SmallButton("content")) {
        assets_dir_.clear();
    }
    std::filesystem::path acc;
    for (const auto& part : assets_dir_) {
        acc /= part;
        ImGui::SameLine();
        ImGui::TextUnformatted("/");
        ImGui::SameLine();
        if (ImGui::SmallButton(part.string().c_str())) {
            assets_dir_ = acc;
        }
    }
    ImGui::Separator();

    const std::vector<DirEntry> entries = list_dir(root, assets_dir_);
    if (ImGui::BeginTable("##assets", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();
        std::filesystem::path navigate;
        bool                  go_up = false;
        if (!assets_dir_.empty()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable("..", false, ImGuiSelectableFlags_SpanAllColumns)) {
                go_up = true;
            }
        }
        int row = 0;
        for (const DirEntry& d : entries) {
            ImGui::PushID(row++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string name = d.path.filename().string() + (d.directory ? "/" : "");
            if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (d.directory) {
                    navigate = d.path;
                } else {
                    instantiate_asset(d.path);
                }
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", d.directory ? "folder" : kind_of(d.path));
            ImGui::TableNextColumn();
            if (!d.directory) {
                const std::string kind   = kind_of(d.path);
                const char*       action = kind == "model" ? "Add to scene" : kind == "scene" ? "Open" : kind == "script" ? "Attach" : nullptr;
                if (action != nullptr) {
                    const bool enabled = kind != "scene" || play_state_ == PlayState::Edit;
                    ImGui::BeginDisabled(!enabled);
                    if (ImGui::SmallButton(action)) {
                        instantiate_asset(d.path);
                    }
                    ImGui::EndDisabled();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (go_up) {
            assets_dir_ = assets_dir_.parent_path();
        } else if (!navigate.empty()) {
            assets_dir_ = navigate;
        }
    }
    ImGui::End();
}

void EditorApp::draw_console() {
    if (!ImGui::Begin("Console", &show_console_)) {
        ImGui::End();
        return;
    }
    if (ImGui::SmallButton("Clear")) {
        clear_console();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &console_autoscroll_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    char buf[128] = {};
    std::snprintf(buf, sizeof(buf), "%s", console_filter_.c_str());
    if (ImGui::InputTextWithHint("##filter", "filter", buf, sizeof(buf))) {
        console_filter_ = buf;
    }
    ImGui::Separator();
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const std::vector<ConsoleRecord> records = console_records();
    for (const ConsoleRecord& r : records) {
        if (!console_filter_.empty() && r.message.find(console_filter_) == std::string::npos &&
            r.category.find(console_filter_) == std::string::npos) {
            continue;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, level_color(r.level));
        ImGui::TextUnformatted(std::format("[{}] {}", r.category, r.message).c_str());
        ImGui::PopStyleColor();
    }
    const u64 last = records.empty() ? 0 : records.back().sequence;
    if (console_autoscroll_ && last != console_seen_) {
        ImGui::SetScrollHereY(1.0f);
    }
    console_seen_ = last;
    ImGui::EndChild();
    ImGui::End();
}

} // namespace aether::editor
