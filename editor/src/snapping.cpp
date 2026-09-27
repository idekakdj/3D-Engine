// snapping.cpp — see snapping.h.
#include "aether/editor/snapping.h"

#include <algorithm>
#include <cmath>

namespace aether::editor {

f32 snap_value(f32 value, f32 step) {
    if (!(step > 0.0f)) {
        return value;
    }
    const f32 snapped = std::round(value / step) * step;
    return snapped == 0.0f ? 0.0f : snapped; // no -0
}

Vec3 snap_position(const Vec3& p, f32 step) { return Vec3(snap_value(p.x, step), snap_value(p.y, step), snap_value(p.z, step)); }

Quat snap_rotation(const Quat& rotation, f32 step_deg) {
    if (!(step_deg > 0.0f)) {
        return rotation;
    }
    const Vec3 euler = glm::degrees(glm::eulerAngles(rotation));
    const Vec3 snapped(snap_value(euler.x, step_deg), snap_value(euler.y, step_deg), snap_value(euler.z, step_deg));
    return glm::normalize(Quat(glm::radians(snapped)));
}

Vec3 snap_scale(const Vec3& s, f32 step) {
    if (!(step > 0.0f)) {
        return s;
    }
    Vec3 out;
    for (int i = 0; i < 3; ++i) {
        const f32 mag = std::max(snap_value(std::abs(s[i]), step), step);
        out[i]        = s[i] < 0.0f ? -mag : mag;
    }
    return out;
}

Mat4 snap_translation(const Mat4& m, f32 step) {
    Mat4 out  = m;
    out[3]    = Vec4(snap_position(Vec3(m[3]), step), m[3].w);
    return out;
}

Mat4 selection_pivot(std::span<const Mat4> worlds, usize primary_index, PivotMode mode, bool local_orientation) {
    if (worlds.empty()) {
        return Mat4(1.0f);
    }
    primary_index = std::min(primary_index, worlds.size() - 1);
    Vec3 position{ 0.0f };
    if (mode == PivotMode::Primary) {
        position = Vec3(worlds[primary_index][3]);
    } else {
        Vec3 lo(worlds[0][3]);
        Vec3 hi = lo;
        for (const Mat4& w : worlds) {
            lo = glm::min(lo, Vec3(w[3]));
            hi = glm::max(hi, Vec3(w[3]));
        }
        position = (lo + hi) * 0.5f;
    }
    Mat4 pivot(1.0f);
    if (local_orientation) {
        const Mat4& p = worlds[primary_index];
        // Orthonormal rotation columns of the primary (scale removed).
        for (int c = 0; c < 3; ++c) {
            const Vec3 axis = Vec3(p[c]);
            const f32  len  = glm::length(axis);
            Vec3       unit(0.0f);
            unit[c]         = 1.0f;
            pivot[c]        = Vec4(len > 1e-8f ? axis / len : unit, 0.0f);
        }
    }
    pivot[3] = Vec4(position, 1.0f);
    return pivot;
}

Mat4 apply_pivot_delta(const Mat4& pivot_start, const Mat4& pivot_now, const Mat4& world_start) {
    return pivot_now * glm::inverse(pivot_start) * world_start;
}

} // namespace aether::editor
