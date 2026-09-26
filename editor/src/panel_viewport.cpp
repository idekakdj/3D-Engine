// panel_viewport.cpp — the scene viewport: offscreen image, picking and transform gizmos.
#include "editor_app.h"

#include "aether/scene/components.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"

#include <imgui.h> // before ImGuizmo.h, which does not include it
#include <ImGuizmo.h>

#include <glm/gtc/type_ptr.hpp>

namespace aether::editor {

void EditorApp::draw_viewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool open = ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) {
        viewport_hovered_ = false;
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
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    // ---- gizmo ----
    const Entity sel         = selected();
    bool         gizmo_hover = false;
    const bool   game_view   = play_state_ != PlayState::Edit && use_game_camera_ &&
                           camera_override() == std::nullopt;
    if (sel != kNullEntity && !game_view) {
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(origin.x, origin.y, viewport_size_.x, viewport_size_.y);
        const Mat4 view = view_matrix();
        // ImGuizmo expects an OpenGL-style (Y-up NDC) projection.
        const Mat4 proj = glm::perspectiveRH_NO(fov_y_deg_ * kDeg2Rad, viewport_size_.x / viewport_size_.y, 0.1f, 2000.0f);
        Mat4       model = world().world_matrix(sel);
        const ImGuizmo::OPERATION op = gizmo_operation_ == 0   ? ImGuizmo::TRANSLATE
                                       : gizmo_operation_ == 1 ? ImGuizmo::ROTATE
                                                               : ImGuizmo::SCALE;
        const ImGuizmo::MODE mode = gizmo_local_ || op == ImGuizmo::SCALE ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
        f32 snap[3]               = { 0.0f, 0.0f, 0.0f };
        const f32 s = op == ImGuizmo::TRANSLATE ? snap_translate_ : op == ImGuizmo::ROTATE ? snap_rotate_deg_ : snap_scale_;
        snap[0] = snap[1] = snap[2] = s;
        const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, mode,
                                                  glm::value_ptr(model), nullptr, gizmo_snap_ ? snap : nullptr);
        gizmo_hover = ImGuizmo::IsOver();
        if (changed) {
            begin_edit("Transform");
            scene::set_world_matrix(world(), sel, model);
        }
        const bool using_now = ImGuizmo::IsUsing();
        if (gizmo_was_using_ && !using_now) {
            end_edit();
        }
        gizmo_was_using_ = using_now;
    }

    // ---- picking ----
    if (clicked && !gizmo_hover && !game_view) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const Vec2   uv((mouse.x - origin.x) / viewport_size_.x, (mouse.y - origin.y) / viewport_size_.y);
        const auto   hit = pick(uv);
        select(hit ? hit->entity : kNullEntity);
    }

    // ---- overlay text ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* hint = play_state_ == PlayState::Edit
                           ? "RMB + WASD/QE: fly   Wheel: dolly   LMB: select   W/E/R: gizmo   F: focus"
                           : "Playing - Ctrl+P / Stop to return to editing (changes are discarded)";
    dl->AddText(ImVec2(origin.x + 8.0f, origin.y + viewport_size_.y - 22.0f), IM_COL32(220, 220, 220, 180), hint);
    ImGui::End();
}

} // namespace aether::editor
