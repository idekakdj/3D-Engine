// aether/editor/snapping.h — grid/rotation/scale snapping and group-transform math.
//
// Snapping is ABSOLUTE for translation (positions land on the world grid, like Unreal's grid
// snap) and incremental for rotation and scale (ImGuizmo snaps the rotation angle / scale ratio
// of the current drag). A step <= 0 disables that snap.
//
// Group transforms: the gizmo manipulates a PIVOT matrix; every selection root keeps its pose
// relative to the pivot:  world' = (pivot_now * inverse(pivot_at_drag_start)) * world_at_drag_start.
// Computing from the drag-start state (not incrementally per frame) keeps scale drags exact.
//
// Thread-safe (pure functions).
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"

#include <span>

namespace aether::editor {

struct SnapSettings {
    bool translate      = false;
    f32  translate_step = 0.5f;  // metres
    bool rotate         = false;
    f32  rotate_step    = 15.0f; // degrees
    bool scale          = false;
    f32  scale_step     = 0.1f;
    bool show_grid      = true;
    f32  grid_spacing   = 1.0f;  // metres between grid lines
    i32  grid_extent    = 50;    // lines each side of the origin
};

[[nodiscard]] f32  snap_value(f32 value, f32 step);
[[nodiscard]] Vec3 snap_position(const Vec3& position, f32 step);
// Each Euler angle (glm::eulerAngles order) rounded to a multiple of step_deg.
[[nodiscard]] Quat snap_rotation(const Quat& rotation, f32 step_deg);
// Each component rounded to a multiple of step, never below one step (no collapse to zero).
[[nodiscard]] Vec3 snap_scale(const Vec3& scale, f32 step);
// `m` with its translation snapped to the grid (rotation/scale untouched).
[[nodiscard]] Mat4 snap_translation(const Mat4& m, f32 step);

enum class PivotMode : u8 {
    BoundsCenter = 0, // centre of the bounding box of the selected positions (Unreal default)
    Primary,          // the primary (last selected) entity's position
};

// Pivot of a selection given the world matrices of its roots. Orientation: identity, or the
// primary's rotation when `local_orientation`. Scale is always 1. `worlds` must not be empty.
[[nodiscard]] Mat4 selection_pivot(std::span<const Mat4> worlds, usize primary_index, PivotMode mode,
                                   bool local_orientation);

[[nodiscard]] Mat4 apply_pivot_delta(const Mat4& pivot_start, const Mat4& pivot_now, const Mat4& world_start);

} // namespace aether::editor
