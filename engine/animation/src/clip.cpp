// clip.cpp — AnimationClip construction, keyframe sampling, wrap modes and notify collection.
#include "aether/animation/clip.h"

#include "aether/animation/anim_math.h"
#include "aether/animation/skeleton.h"
#include "aether/core/log.h"
#include "playback.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace aether::animation {

using assets::AnimPath;
using assets::Interpolation;

namespace {

// Segment k with times[k] <= t < times[k+1]. Requires count >= 2 and times[0] <= t < times[n-1].
u32 find_segment(const f32* times, u32 count, f32 t, u32 hint) noexcept {
    if (hint + 1 < count && times[hint] <= t) {
        if (t < times[hint + 1]) {
            return hint;
        }
        if (hint + 2 < count && t < times[hint + 2]) {
            return hint + 1;
        }
    }
    const u32 upper = static_cast<u32>(std::upper_bound(times, times + count, t) - times);
    return std::min(upper - 1, count - 2);
}

void write_path(AnimPath path, const Vec4& v, Transform& out) noexcept {
    switch (path) {
    case AnimPath::Translation: out.position = Vec3(v); break;
    case AnimPath::Rotation: out.rotation = normalize(quat_from_xyzw(v)); break;
    case AnimPath::Scale: out.scale = Vec3(v); break;
    }
}

} // namespace

Result<AnimationClip> AnimationClip::create(const assets::AnimationClipData& data,
                                            const Skeleton& skeleton) {
    return create(data, skeleton.joint_count());
}

Result<AnimationClip> AnimationClip::create(const assets::AnimationClipData& data, u32 joint_count) {
    AnimationClip clip;
    clip.name_ = data.name;
    clip.joint_tracks_.resize(joint_count);

    f32 last_time = 0.0f;
    for (usize ci = 0; ci < data.channels.size(); ++ci) {
        const assets::AnimationChannel& ch = data.channels[ci];
        const auto path_index = static_cast<u32>(ch.path);
        if (ch.joint >= joint_count) {
            return make_error<AnimationClip>(
                ErrorCode::InvalidArgument,
                std::format("AnimationClip '{}': channel {} targets joint {} but the skeleton has "
                            "{} joints",
                            data.name, ci, ch.joint, joint_count));
        }
        if (path_index > 2) {
            return make_error<AnimationClip>(
                ErrorCode::InvalidArgument,
                std::format("AnimationClip '{}': channel {} has an invalid path", data.name, ci));
        }
        if (ch.times.empty()) {
            AE_LOG_WARN("Animation", "AnimationClip '{}': channel {} has no keys; skipped",
                        data.name, ci);
            continue;
        }
        const bool  cubic = ch.interpolation == Interpolation::CubicSpline;
        const usize expected = ch.times.size() * (cubic ? 3u : 1u);
        if (ch.values.size() != expected) {
            return make_error<AnimationClip>(
                ErrorCode::InvalidArgument,
                std::format("AnimationClip '{}': channel {} has {} keys but {} values (expected {})",
                            data.name, ci, ch.times.size(), ch.values.size(), expected));
        }
        for (usize k = 0; k < ch.times.size(); ++k) {
            if (!std::isfinite(ch.times[k]) || (k > 0 && ch.times[k] < ch.times[k - 1])) {
                return make_error<AnimationClip>(
                    ErrorCode::InvalidArgument,
                    std::format("AnimationClip '{}': channel {} key times are not ascending",
                                data.name, ci));
            }
        }
        JointTracks& jt = clip.joint_tracks_[ch.joint];
        if (jt.track[path_index] != kInvalidU32) {
            AE_LOG_WARN("Animation", "AnimationClip '{}': duplicate channel {} for joint {}; skipped",
                        data.name, ci, ch.joint);
            continue;
        }

        Track track;
        track.joint = ch.joint;
        track.path = ch.path;
        track.interpolation = ch.interpolation;
        track.first_key = static_cast<u32>(clip.times_.size());
        track.key_count = static_cast<u32>(ch.times.size());
        track.first_value = static_cast<u32>(clip.values_.size());
        clip.times_.insert(clip.times_.end(), ch.times.begin(), ch.times.end());
        clip.values_.insert(clip.values_.end(), ch.values.begin(), ch.values.end());
        if (ch.path == AnimPath::Rotation && !cubic) {
            // Unit keys for slerp/step; cubic tangents must stay as authored.
            for (usize v = track.first_value; v < clip.values_.size(); ++v) {
                clip.values_[v] = quat_to_xyzw(normalize(quat_from_xyzw(clip.values_[v])));
            }
        }
        jt.track[path_index] = static_cast<u32>(clip.tracks_.size());
        clip.tracks_.push_back(track);
        clip.required_joints_ = std::max(clip.required_joints_, ch.joint + 1);
        last_time = std::max(last_time, ch.times.back());
    }
    clip.duration_ = data.duration > 0.0f ? data.duration : last_time;
    return clip;
}

bool AnimationClip::animates(u32 joint) const noexcept {
    if (joint >= joint_tracks_.size()) {
        return false;
    }
    const JointTracks& jt = joint_tracks_[joint];
    return jt.track[0] != kInvalidU32 || jt.track[1] != kInvalidU32 || jt.track[2] != kInvalidU32;
}

void AnimationClip::add_notify(std::string name, f32 time) {
    time = std::clamp(time, 0.0f, duration_);
    const auto it = std::upper_bound(notifies_.begin(), notifies_.end(), time,
                                     [](f32 t, const AnimNotify& n) { return t < n.time; });
    notifies_.insert(it, AnimNotify{std::move(name), time});
}

void AnimationClip::prepare_cursor(ClipCursor& cursor) const {
    cursor.keys_.assign(tracks_.size(), 0u);
}

void AnimationClip::apply_track(const Track& tr, f32 t, u32* hint, Transform& out) const noexcept {
    const f32*  times = times_.data() + tr.first_key;
    const Vec4* values = values_.data() + tr.first_value;
    const u32   n = tr.key_count;
    const bool  cubic = tr.interpolation == Interpolation::CubicSpline;
    const u32   stride = cubic ? 3u : 1u;
    const u32   value_offset = cubic ? 1u : 0u; // cubic keys are (in-tangent, value, out-tangent)

    if (n == 1 || t <= times[0]) {
        if (hint != nullptr) {
            *hint = 0;
        }
        write_path(tr.path, values[value_offset], out);
        return;
    }
    if (t >= times[n - 1]) {
        if (hint != nullptr) {
            *hint = n - 2;
        }
        write_path(tr.path, values[(n - 1) * stride + value_offset], out);
        return;
    }
    const u32 k = find_segment(times, n, t, hint != nullptr ? *hint : 0u);
    if (hint != nullptr) {
        *hint = k;
    }
    const f32 span = times[k + 1] - times[k];
    const f32 u = span > 0.0f ? (t - times[k]) / span : 0.0f;

    switch (tr.interpolation) {
    case Interpolation::Step: write_path(tr.path, values[k], out); return;
    case Interpolation::Linear:
        if (tr.path == AnimPath::Rotation) {
            out.rotation = slerp(quat_from_xyzw(values[k]), quat_from_xyzw(values[k + 1]), u);
        } else {
            write_path(tr.path, values[k] + (values[k + 1] - values[k]) * u, out);
        }
        return;
    case Interpolation::CubicSpline: {
        // glTF: p(u) = h00 p0 + h10 (dt * b0) + h01 p1 + h11 (dt * a1)
        const Vec4& p0 = values[k * 3 + 1];
        const Vec4& b0 = values[k * 3 + 2];
        const Vec4& a1 = values[(k + 1) * 3 + 0];
        const Vec4& p1 = values[(k + 1) * 3 + 1];
        const f32   u2 = u * u;
        const f32   u3 = u2 * u;
        const f32   h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
        const f32   h10 = u3 - 2.0f * u2 + u;
        const f32   h01 = -2.0f * u3 + 3.0f * u2;
        const f32   h11 = u3 - u2;
        write_path(tr.path, p0 * h00 + b0 * (h10 * span) + p1 * h01 + a1 * (h11 * span), out);
        return;
    }
    }
}

void AnimationClip::sample(f32 time, std::span<Transform> inout_local, ClipCursor* cursor) const {
    AE_ASSERT(inout_local.size() >= required_joints_);
    u32* hints = nullptr;
    if (cursor != nullptr) {
        if (cursor->keys_.size() != tracks_.size()) {
            prepare_cursor(*cursor); // first use only; prepare at setup to avoid this allocation
        }
        hints = cursor->keys_.data();
    }
    for (usize i = 0; i < tracks_.size(); ++i) {
        const Track& tr = tracks_[i];
        apply_track(tr, time, hints != nullptr ? hints + i : nullptr, inout_local[tr.joint]);
    }
}

void AnimationClip::sample_pose(f32 time, const Skeleton& skeleton, std::span<Transform> out_local,
                                ClipCursor* cursor) const {
    const auto bind = skeleton.bind_pose();
    AE_ASSERT(out_local.size() >= bind.size());
    std::copy(bind.begin(), bind.end(), out_local.begin());
    sample(time, out_local, cursor);
}

Transform AnimationClip::sample_joint(u32 joint, f32 time, const Transform& fallback) const {
    Transform result = fallback;
    if (joint < joint_tracks_.size()) {
        for (const u32 ti : joint_tracks_[joint].track) {
            if (ti != kInvalidU32) {
                apply_track(tracks_[ti], time, nullptr, result);
            }
        }
    }
    return result;
}

f32 wrap_time(f64 time, f32 duration, WrapMode mode) noexcept {
    if (!(duration > 0.0f)) {
        return 0.0f;
    }
    const f64 d = static_cast<f64>(duration);
    switch (mode) {
    case WrapMode::Loop: {
        const f64 r = time - d * std::floor(time / d);
        return static_cast<f32>(r >= d ? 0.0 : std::max(r, 0.0));
    }
    case WrapMode::Clamp: return static_cast<f32>(std::clamp(time, 0.0, d));
    case WrapMode::PingPong: {
        const f64 period = 2.0 * d;
        f64       r = time - period * std::floor(time / period);
        if (r > d) {
            r = period - r;
        }
        return static_cast<f32>(std::clamp(r, 0.0, d));
    }
    }
    return 0.0f;
}

u32 collect_notifies(const AnimationClip& clip, f64 from, f64 to, WrapMode mode,
                     std::vector<const AnimNotify*>& out) {
    const usize before = out.size();
    detail::for_each_crossed_notify(clip, from, to, mode,
                                    [&](const AnimNotify& n) { out.push_back(&n); });
    return static_cast<u32>(out.size() - before);
}

} // namespace aether::animation
