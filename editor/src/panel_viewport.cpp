// panel_viewport.cpp — the scene viewport: offscreen image, grid, picking (GPU with CPU fallback),
// box select, asset drops and the group transform gizmo.
#include "editor_app.h"

#include "aether/scene/components.h"
#include "aether/scene/id.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"

#include <imgui.h> // before ImGuizmo.h, which does not include it
#include <ImGuizmo.h>

#include <glm/gtc/type_ptr.hpp>

#include <format>

namespace aether::editor {

namespace {
constexpr const char* kAssetPayload = "AE_ASSET_PATH";
constexpr f32         kDragThreshold = 4.0f; // pixels before a click becomes a box select
} // namespace

void EditorApp::draw_viewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool open = ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) {
        viewport_hovered_ = false;
        box_pending_      = false;
        ImGui::End();
        return;
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const UVec2  size(static_cast<u32>(std::max(avail.x, 16.0f)), static_cast<u32>(std::max(avail.y, 16.0f)));
    ensure_viewport_target(size);
    viewport_focused_ = ImGui::IsWindowFocused();

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    viewport_min_       = Vec2(origin.x, origin.y);
    viewport_size_      = Vec2(static_cast<f32>(size.x), static_cast<f32>(size.y));
    if (viewport_.imgui_id != 0) {
        ImGui::Image(static_cast<ImTextureID>(viewport_.imgui_id), ImVec2(viewport_size_.x, viewport_size_.y));
    } else {
        ImGui::Dummy(ImVec2(viewport_size_.x, viewport_size_.y));
    }
    viewport_hovered_ = ImGui::IsItemHovered();
    auto to_uv        = [&](const Vec2& screen) { return (screen - viewport_min_) / viewport_size_; };
    const ImVec2 mouse_im = ImGui::GetMousePos();
    const Vec2   mouse(mouse_im.x, mouse_im.y);

    // ---- asset drops (glTF / prefab / scene placed at the cursor; scripts attach) ----
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            drop_asset_in_viewport(std::string(static_cast<const char*>(p->Data)), to_uv(mouse));
        }
        ImGui::EndDragDropTarget();
    }

    // ---- grid + gizmo ----
    const bool game_view   = play_state_ != PlayState::Edit && use_game_camera_ && camera_override() == std::nullopt;
    bool       gizmo_hover = false;
    draw_viewport_gizmo(viewport_min_, game_view, gizmo_hover);

    // ---- click to pick / drag to box select ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (viewport_hovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !gizmo_hover && !ImGuizmo::IsUsing() && !game_view) {
        box_pending_ = true;
        box_start_   = mouse;
    }
    if (box_pending_) {
        const bool dragged = glm::length(mouse - box_start_) > kDragThreshold;
        if (dragged) {
            const ImVec2 a(std::min(box_start_.x, mouse.x), std::min(box_start_.y, mouse.y));
            const ImVec2 b(std::max(box_start_.x, mouse.x), std::max(box_start_.y, mouse.y));
            dl->AddRectFilled(a, b, IM_COL32(90, 150, 255, 40));
            dl->AddRect(a, b, IM_COL32(90, 150, 255, 200));
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            box_pending_          = false;
            const ImGuiIO&   io   = ImGui::GetIO();
            const SelectMode mode = select_mode_from(io.KeyCtrl, io.KeyShift);
            if (dragged) {
                box_select(to_uv(box_start_), to_uv(mouse), mode);
            } else {
                request_viewport_pick(to_uv(box_start_), mode);
            }
        }
    }

    // ---- overlay text ----
    const char* hint = play_state_ == PlayState::Edit
                           ? "RMB + WASD/QE: fly   Wheel: dolly   LMB: select (Ctrl toggle, Shift add, drag: box)   W/E/R: gizmo   F: focus"
                           : "Playing - Ctrl+P / Stop to return to editing (changes are discarded)";
    dl->AddText(ImVec2(origin.x + 8.0f, origin.y + viewport_size_.y - 22.0f), IM_COL32(220, 220, 220, 180), hint);
    const usize count = selection_.size();
    if (count > 0 || !last_pick_source_.empty()) {
        const std::string info = std::format("{} selected   pick: {}{}", count, last_pick_source_.empty() ? "-" : last_pick_source_,
                                             pick_tracker_.gpu_available() ? "" : " (GPU picking unavailable)");
        dl->AddText(ImVec2(origin.x + 8.0f, origin.y + 6.0f), IM_COL32(220, 220, 220, 160), info.c_str());
    }
    ImGui::End();
}

void EditorApp::draw_viewport_gizmo(const Vec2& origin, bool game_view, bool& gizmo_hover) {
    if (game_view) {
        return;
    }
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(origin.x, origin.y, viewport_size_.x, viewport_size_.y);
    const Mat4 view = view_matrix();
    // ImGuizmo expects an OpenGL-style (Y-up NDC) projection.
    const Mat4 proj = glm::perspectiveRH_NO(fov_y_deg_ * kDeg2Rad, viewport_size_.x / viewport_size_.y, 0.1f, 2000.0f);

    if (snap_.show_grid) {
        const f32  spacing = std::max(snap_.grid_spacing, 0.01f);
        const Mat4 grid    = glm::scale(Mat4(1.0f), Vec3(spacing));
        ImGuizmo::DrawGrid(glm::value_ptr(view), glm::value_ptr(proj), glm::value_ptr(grid),
                           static_cast<f32>(std::max(snap_.grid_extent, 1)));
    }

    const std::vector<Entity> roots = selection_roots(world(), selection());
    if (roots.empty()) {
        if (gizmo_dragging_) {
            gizmo_dragging_ = false;
            gizmo_start_worlds_.clear();
            end_edit();
        }
        return;
    }
    Mat4       gizmo  = gizmo_dragging_ ? gizmo_matrix_ : selection_pivot_matrix();
    const Mat4 before = gizmo;
    const ImGuizmo::OPERATION op = gizmo_operation_ == 0   ? ImGuizmo::TRANSLATE
                                   : gizmo_operation_ == 1 ? ImGuizmo::ROTATE
                                                           : ImGuizmo::SCALE;
    const ImGuizmo::MODE mode = gizmo_local_ || op == ImGuizmo::SCALE ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    // Rotation / scale snap through ImGuizmo (incremental); translation is snapped absolutely below.
    f32        snap[3]  = { 0.0f, 0.0f, 0.0f };
    const f32* snap_ptr = nullptr;
    if (op == ImGuizmo::ROTATE && snap_.rotate) {
        snap[0] = snap[1] = snap[2] = snap_.rotate_step;
        snap_ptr                    = snap;
    } else if (op == ImGuizmo::SCALE && snap_.scale) {
        snap[0] = snap[1] = snap[2] = snap_.scale_step;
        snap_ptr                    = snap;
    }
    const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, mode, glm::value_ptr(gizmo),
                                              nullptr, snap_ptr);
    gizmo_hover = ImGuizmo::IsOver();
    if (changed) {
        if (!gizmo_dragging_) {
            gizmo_dragging_    = true;
            gizmo_start_pivot_ = before;
            gizmo_start_worlds_.clear();
            for (const Entity e : roots) {
                gizmo_start_worlds_.emplace_back(scene::uuid_of(world(), e), world().world_matrix(e));
            }
            begin_edit(roots.size() > 1 ? "Transform selection" : "Transform");
        }
        if (op == ImGuizmo::TRANSLATE && snap_.translate) {
            gizmo = snap_translation(gizmo, snap_.translate_step);
        }
        gizmo_matrix_ = gizmo;
        for (const auto& [uuid, start] : gizmo_start_worlds_) {
            const Entity e = scene::find_by_uuid(world(), uuid);
            if (e != kNullEntity) {
                scene::set_world_matrix(world(), e, apply_pivot_delta(gizmo_start_pivot_, gizmo, start));
            }
        }
        world().update_transforms();
    }
    if (gizmo_dragging_ && !ImGuizmo::IsUsing()) {
        gizmo_dragging_ = false;
        gizmo_start_worlds_.clear();
        end_edit();
    }
}

} // namespace aether::editor
