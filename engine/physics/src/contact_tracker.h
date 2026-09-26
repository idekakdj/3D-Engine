// contact_tracker.h (private) — Jolt contact callbacks -> deterministic body-pair events.
//
// Jolt invokes ContactListener callbacks from its worker threads, per SUB-SHAPE pair. The
// recorder appends compact raw records under a mutex (callbacks for one body pair are serialized
// by Jolt, so per-pair order is preserved). After the step, the main thread stable-sorts the raw
// records by body pair and folds them into per-pair state, emitting Begin (first sub-shape
// contact), Persist (once per pair per step) and End (last sub-shape contact removed).
#pragma once

#include "jolt_common.h"

#include "aether/physics/physics_types.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace aether::physics::detail {

struct RawContact {
    enum class Kind : u8 { Added, Persisted, Removed };
    Kind        kind = Kind::Added;
    bool        sensor1 = false;
    bool        sensor2 = false;
    JPH::uint32 body1 = 0; // BodyID values (index + sequence), body1 < body2
    JPH::uint32 body2 = 0;
    JPH::uint32 sub1 = 0;  // SubShapeID values
    JPH::uint32 sub2 = 0;
    JPH::uint64 user1 = 0; // entity user data (Added/Persisted only)
    JPH::uint64 user2 = 0;
    Vec3        point{ 0.0f };
    Vec3        normal{ 0.0f }; // from body1 towards body2
    f32         depth = 0.0f;
    f32         approach_speed = 0.0f;
};

// Debug record of a contact point from the last step.
struct DebugContact {
    Vec3 point{ 0.0f };
    Vec3 normal{ 0.0f };
    bool sensor = false;
};

class ContactRecorder final : public JPH::ContactListener {
public:
    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                        const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override;
    void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2,
                            const JPH::ContactManifold& manifold,
                            JPH::ContactSettings& settings) override;
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override;

    // Main thread, outside PhysicsSystem::Update: moves the recorded contacts into `out`.
    void take(std::vector<RawContact>& out);

private:
    void record(RawContact::Kind kind, const JPH::Body& body1, const JPH::Body& body2,
                const JPH::ContactManifold& manifold);

    std::mutex              mutex_;
    std::vector<RawContact> raw_;
};

class ContactTracker {
public:
    // Folds one step's raw contacts into events (appended to `events`, bounded by `max_events`)
    // and collects debug contact points. Main thread.
    void process(std::vector<RawContact>& raw, u64 step, bool emit_persist,
                 std::vector<ContactEvent>& events, u32 max_events,
                 std::vector<DebugContact>& debug_contacts);

    [[nodiscard]] usize active_pair_count() const { return pairs_.size(); }
    void clear() { pairs_.clear(); }

private:
    struct PairState {
        Entity             a = kNullEntity; // body1's entity
        Entity             b = kNullEntity;
        bool               sensor_a = false;
        bool               sensor_b = false;
        u64                last_event_step = ~u64{ 0 };
        std::vector<u64>   sub_pairs; // active sub-shape pairs (usually 1)
    };

    void emit(std::vector<ContactEvent>& events, u32 max_events, ContactEventType type,
              const PairState& pair, const RawContact* raw);

    std::unordered_map<u64, PairState> pairs_;
    bool                               overflow_warned_ = false;
};

} // namespace aether::physics::detail
