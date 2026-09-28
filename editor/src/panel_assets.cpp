// panel_assets.cpp — content browser (thumbnail grid or list; double-click / context menu /
// drag-and-drop sources) and the log console. Files are dragged as an "AE_ASSET_PATH" payload (content-relative, NUL-terminated)
// onto the viewport (placed at the cursor) or the hierarchy (instantiated as a child).
#include "editor_app.h"
#include "thumbnail_cache.h"

#include "aether/editor/console.h"
#include "aether/editor/thumbnail.h"

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
    if (e == ".aeprefab") return "prefab";
    if (e == ".lua") return "script";
    if (e == ".aegraph") return "graph"; // ADR-0018 visual script
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
    // Breadcrumbs + view options.
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
    ImGui::SameLine(std::max(ImGui::GetContentRegionAvail().x - 170.0f, ImGui::GetCursorPosX() + 8.0f));
    if (ImGui::SmallButton(assets_grid_ ? "List" : "Grid")) {
        assets_grid_ = !assets_grid_;
    }
    if (assets_grid_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        ImGui::SliderFloat("##tile", &assets_tile_, 56.0f, 160.0f, "%.0f px");
    }
    ImGui::Separator();

    const std::vector<DirEntry> entries = list_dir(root, assets_dir_);
    std::filesystem::path       navigate;
    bool                        go_up = false;

    // Shared per-entry behaviour: double-click opens / instantiates, drag source, context menu.
    auto action_label = [&](const DirEntry& d) -> const char* {
        const std::string kind = kind_of(d.path);
        return kind == "model" || kind == "prefab" ? "Add to scene"
               : kind == "scene"                   ? "Open"
               : kind == "script"                  ? "Attach"
               : kind == "graph"                   ? "Edit"
                                                   : nullptr;
    };
    auto run_action = [&](const DirEntry& d) {
        if (d.directory) {
            navigate = d.path;
        } else if (kind_of(d.path) == std::string("graph")) {
            open_graph(d.path);
        } else if (kind_of(d.path) != std::string("scene") || play_state_ == PlayState::Edit) {
            instantiate_asset(d.path);
        }
    };
    auto item_behaviour = [&](const DirEntry& d, u64 thumb) {
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            run_action(d);
        }
        if (!d.directory && ImGui::BeginDragDropSource()) {
            const std::string payload = d.path.generic_string();
            ImGui::SetDragDropPayload("AE_ASSET_PATH", payload.c_str(), payload.size() + 1);
            if (thumb != 0) {
                ImGui::Image(static_cast<ImTextureID>(thumb), ImVec2(48.0f, 48.0f));
                ImGui::SameLine();
            }
            ImGui::Text("%s (%s)", d.path.filename().string().c_str(), kind_of(d.path));
            ImGui::EndDragDropSource();
        }
        if (!d.directory && ImGui::BeginPopupContextItem("##ctx")) {
            if (const char* a = action_label(d)) {
                if (ImGui::MenuItem(a, nullptr, false, kind_of(d.path) != std::string("scene") || play_state_ == PlayState::Edit)) {
                    run_action(d);
                }
            }
            if (kind_of(d.path) == std::string("graph") && ImGui::MenuItem("Attach to selected entity", nullptr, false,
                                                                           selected() != kNullEntity)) {
                instantiate_asset(d.path);
            }
            if (thumbnail_kind(d.path) != ThumbnailKind::None && ImGui::MenuItem("Refresh thumbnail")) {
                thumbnails_->clear();
            }
            ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && ImGui::GetDragDropPayload() == nullptr) {
            ImGui::BeginTooltip();
            if (thumb != 0 && !assets_grid_) {
                ImGui::Image(static_cast<ImTextureID>(thumb), ImVec2(128.0f, 128.0f));
            }
            ImGui::TextUnformatted(d.path.generic_string().c_str());
            ImGui::TextDisabled("%s", d.directory ? "folder" : kind_of(d.path));
            if (!d.directory && thumbnails_ && thumbnails_->state(d.path) == ThumbnailCache::State::Pending) {
                ImGui::TextDisabled("thumbnail: generating...");
            } else if (!d.directory && thumbnails_ && thumbnails_->state(d.path) == ThumbnailCache::State::Failed) {
                ImGui::TextDisabled("thumbnail unavailable");
            }
            ImGui::EndTooltip();
        }
    };
    auto thumb_of = [&](const DirEntry& d) -> u64 {
        return !d.directory && thumbnails_ ? thumbnails_->get(d.path) : 0u;
    };

    if (assets_grid_) {
        ImGui::BeginChild("##grid");
        const f32   tile    = assets_tile_;
        const f32   label_h = ImGui::GetTextLineHeightWithSpacing();
        const f32   spacing = ImGui::GetStyle().ItemSpacing.x;
        const int   columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (tile + spacing)));
        ImDrawList* dl      = ImGui::GetWindowDrawList();
        int         index   = 0;
        auto tile_at = [&](const DirEntry* d, const char* up_label) {
            if (index % columns != 0) {
                ImGui::SameLine();
            }
            ImGui::PushID(index++);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const bool   clicked = ImGui::Selectable("##tile", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(tile, tile + label_h));
            const u64    thumb = d != nullptr ? thumb_of(*d) : 0u;
            if (d != nullptr) {
                item_behaviour(*d, thumb);
            } else if (clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                go_up = true;
            }
            // Picture (or placeholder) + clipped name.
            const ImVec2 a(p0.x + 4.0f, p0.y + 4.0f);
            const ImVec2 b(p0.x + tile - 4.0f, p0.y + tile - 4.0f);
            if (thumb != 0) {
                dl->AddRectFilled(a, b, IM_COL32(40, 40, 44, 255), 4.0f);
                dl->AddImage(static_cast<ImTextureID>(thumb), a, b);
            } else {
                const bool   folder = d == nullptr || d->directory;
                const ImU32  bg     = folder ? IM_COL32(92, 78, 44, 255) : IM_COL32(52, 56, 64, 255);
                dl->AddRectFilled(a, b, bg, 6.0f);
                const char*  label = d == nullptr ? ".." : folder ? "folder" : kind_of(d->path);
                const ImVec2 ts    = ImGui::CalcTextSize(label);
                dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), IM_COL32(200, 200, 205, 255), label);
            }
            std::string name = d == nullptr ? std::string(up_label) : d->path.filename().string();
            while (name.size() > 3 && ImGui::CalcTextSize(name.c_str()).x > tile - 4.0f) {
                name.erase(name.size() - 4);
                name += "..";
            }
            const f32 tw = ImGui::CalcTextSize(name.c_str()).x;
            dl->AddText(ImVec2(p0.x + (tile - tw) * 0.5f, p0.y + tile), IM_COL32(220, 220, 225, 255), name.c_str());
            ImGui::PopID();
        };
        if (!assets_dir_.empty()) {
            tile_at(nullptr, "..");
        }
        for (const DirEntry& d : entries) {
            tile_at(&d, nullptr);
        }
        ImGui::EndChild();
    } else if (ImGui::BeginTable("##assets", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();
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
            const u64 thumb = thumb_of(d);
            const f32 icon  = ImGui::GetTextLineHeight();
            if (thumb != 0) {
                ImGui::Image(static_cast<ImTextureID>(thumb), ImVec2(icon, icon));
            } else {
                ImGui::Dummy(ImVec2(icon, icon));
            }
            ImGui::SameLine();
            const std::string name = d.path.filename().string() + (d.directory ? "/" : "");
            ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
            item_behaviour(d, thumb);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", d.directory ? "folder" : kind_of(d.path));
            ImGui::TableNextColumn();
            if (!d.directory) {
                if (const char* action = action_label(d)) {
                    const bool enabled = kind_of(d.path) != std::string("scene") || play_state_ == PlayState::Edit;
                    ImGui::BeginDisabled(!enabled);
                    if (ImGui::SmallButton(action)) {
                        run_action(d);
                    }
                    ImGui::EndDisabled();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (go_up) {
        assets_dir_ = assets_dir_.parent_path();
    } else if (!navigate.empty()) {
        assets_dir_ = navigate;
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
