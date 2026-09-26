// aether/animation/clip.h — runtime animation clips: sampling, wrap modes, notifies.
//
// An AnimationClip is built from assets::AnimationClipData (glTF-shaped channels) and shared
// immutably. Sampling supports Step, Linear (shortest-arc slerp for rotations) and CubicSpline
// (glTF Hermite: tangents scaled by the key interval, rotations renormalized). Before the first
// key a channel holds its first value, after the last key its last value.
//
// Playback time: players keep an UNWRAPPED time (seconds, f64, monotonic in the play direction)
// and map it to a clip-local sample time with wrap_time(). Notifies are crossed on the unwrapped
// timeline with half-open intervals, so each notify fires exactly once per crossing no matter how
// updates are sliced:
//   * forward playback fires notifies in [from, to), backward playback in (to, from];
//   * Clamp: reaching an end of the clip also fires the notify sitting exactly on that end;
//   * Loop: time 0 and `duration` are the same instant — a notify placed at `duration` fires as
//     each cycle begins (including the first);
//   * PingPong: odd cycles play backwards; notifies fire on both legs.
//
// Thread-safety: an AnimationClip is immutable once shared (add_notify is setup-only, before
// sharing); sampling is const and thread-safe. A ClipCursor must not be shared across threads.
#pragma once

#include "aether/assets/asset_types.h"
#include "aether/core/error.h"
#include "aether/core/math.h"
#include "aether/core/types.h"

#include <span>
#include <string>
#include <vector>

namespace aether::animation {

class Skeleton;

enum class WrapMode : u8 {
    Loop = 0, // repeat
    Clamp,    // play once and hold the last (or first, when reversed) frame
    PingPong, // forward, backward, forward, ...
};

// A named time marker ("footstep_l", "spawn_fx").
struct AnimNotify {
    std::string name;
    f32         time = 0.0f; // clip-local seconds, in [0, duration]
};

// Per-player key cache. Sequential sampling (the common case) finds each track's key interval in
// O(1) instead of a binary search. Owned by the player; the clip only reads/writes the hints.
class ClipCursor {
public:
    void reset() noexcept { keys_.assign(keys_.size(), 0u); }

private:
    friend class AnimationClip;
    std::vector<u32> keys_;
};

class AnimationClip {
public:
    // Validates channels against `joint_count` (joint indices in range, key/value counts
    // consistent, times non-decreasing). Channels with no keys or duplicating an already
    // animated (joint, path) are skipped with a warning. Duration is data.duration when > 0,
    // otherwise the last key time.
    [[nodiscard]] static Result<AnimationClip> create(const assets::AnimationClipData& data,
                                                      u32 joint_count);
    [[nodiscard]] static Result<AnimationClip> create(const assets::AnimationClipData& data,
                                                      const Skeleton& skeleton);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] f32 duration() const noexcept { return duration_; }
    [[nodiscard]] u32 track_count() const noexcept { return static_cast<u32>(tracks_.size()); }
    // Highest animated joint index + 1 (0 for a clip without tracks).
    [[nodiscard]] u32 required_joint_count() const noexcept { return required_joints_; }
    [[nodiscard]] bool animates(u32 joint) const noexcept;

    // Setup only (not thread-safe): adds a notify, clamped into [0, duration], kept time-sorted.
    void add_notify(std::string name, f32 time);
    [[nodiscard]] std::span<const AnimNotify> notifies() const noexcept { return notifies_; }

    // Writes every animated channel at clip-local `time` into `inout_local`; joints/paths the
    // clip does not animate are left untouched (start from the bind pose for a full pose).
    void sample(f32 time, std::span<Transform> inout_local, ClipCursor* cursor = nullptr) const;
    // Copies the skeleton's bind pose into `out_local`, then samples.
    void sample_pose(f32 time, const Skeleton& skeleton, std::span<Transform> out_local,
                     ClipCursor* cursor = nullptr) const;
    // One joint's local transform at `time`; paths not animated come from `fallback`.
    [[nodiscard]] Transform sample_joint(u32 joint, f32 time, const Transform& fallback) const;

    // Sizes a cursor for this clip (call once at setup so sampling never allocates).
    void prepare_cursor(ClipCursor& cursor) const;

private:
    struct Track {
        u32                   joint = 0;
        assets::AnimPath      path = assets::AnimPath::Translation;
        assets::Interpolation interpolation = assets::Interpolation::Linear;
        u32                   first_key = 0;   // into times_
        u32                   key_count = 0;
        u32                   first_value = 0; // into values_ (3 per key for CubicSpline)
    };
    struct JointTracks {
        u32 track[3] = {kInvalidU32, kInvalidU32, kInvalidU32}; // indexed by AnimPath
    };

    AnimationClip() = default;
    void apply_track(const Track& track, f32 time, u32* hint, Transform& out) const noexcept;

    std::string              name_;
    f32                      duration_ = 0.0f;
    u32                      required_joints_ = 0;
    std::vector<Track>       tracks_;
    std::vector<f32>         times_;
    std::vector<Vec4>        values_;
    std::vector<JointTracks> joint_tracks_;
    std::vector<AnimNotify>  notifies_;
};

// Maps an unwrapped playback time to a clip-local sample time in [0, duration].
[[nodiscard]] f32 wrap_time(f64 time, f32 duration, WrapMode mode) noexcept;

// Appends every notify crossed when playback moves from unwrapped `from` to `to` (see the rules
// above), in playback order. Returns the number appended.
u32 collect_notifies(const AnimationClip& clip, f64 from, f64 to, WrapMode mode,
                     std::vector<const AnimNotify*>& out);

} // namespace aether::animation
