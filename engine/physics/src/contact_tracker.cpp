// contact_tracker.cpp — see contact_tracker.h.
#include "contact_tracker.h"

#include "aether/core/log.h"

#include <algorithm>

namespace aether::physics::detail {

// ---------------------------------------------------------------------------------------------
// ContactRecorder (Jolt worker threads)
// ---------------------------------------------------------------------------------------------
void ContactRecorder::record(RawContact::Kind kind, const JPH::Body& body1, const JPH::Body& body2,
                             const JPH::ContactManifold& manifold) {
    RawContact rc;
    rc.kind = kind;
    rc.sensor1 = body1.IsSensor();
    rc.sensor2 = body2.IsSensor();
    rc.body1 = body1.GetID().GetIndexAndSequenceNumber();
    rc.body2 = body2.GetID().GetIndexAndSequenceNumber();
    rc.sub1 = manifold.mSubShapeID1.GetValue();
    rc.sub2 = manifold.mSubShapeID2.GetValue();
    rc.user1 = body1.GetUserData();
    rc.user2 = body2.GetUserData();

    // Average of the contact points on both surfaces (the midpoint of the penetration).
    JPH::Vec3        sum = JPH::Vec3::sZero();
    const JPH::uint  n1 = manifold.mRelativeContactPointsOn1.size();
    const JPH::uint  n2 = manifold.mRelativeContactPointsOn2.size();
    for (JPH::uint i = 0; i < n1; ++i)
        sum += manifold.mRelativeContactPointsOn1[i];
    for (JPH::uint i = 0; i < n2; ++i)
        sum += manifold.mRelativeContactPointsOn2[i];
    const JPH::uint   n = n1 + n2;
    const JPH::RVec3  point = manifold.mBaseOffset + (n > 0 ? sum / static_cast<float>(n) : sum);
    rc.point = to_ae(point);
    rc.normal = to_ae(manifold.mWorldSpaceNormal);
    rc.depth = manifold.mPenetrationDepth;

    // Velocities are readable in the collision phase (Jolt grants read access for this purpose).
    const JPH::Vec3 v1 = body1.GetPointVelocity(point);
    const JPH::Vec3 v2 = body2.GetPointVelocity(point);
    rc.approach_speed = (v1 - v2).Dot(manifold.mWorldSpaceNormal);

    std::lock_guard lock(mutex_);
    raw_.push_back(rc);
}

void ContactRecorder::OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                                     const JPH::ContactManifold& manifold,
                                     JPH::ContactSettings& /*settings*/) {
    record(RawContact::Kind::Added, body1, body2, manifold);
}

void ContactRecorder::OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2,
                                         const JPH::ContactManifold& manifold,
                                         JPH::ContactSettings& /*settings*/) {
    record(RawContact::Kind::Persisted, body1, body2, manifold);
}

void ContactRecorder::OnContactRemoved(const JPH::SubShapeIDPair& pair) {
    // Bodies must not be touched here (they may already be destroyed); IDs are all we need.
    RawContact rc;
    rc.kind = RawContact::Kind::Removed;
    rc.body1 = pair.GetBody1ID().GetIndexAndSequenceNumber();
    rc.body2 = pair.GetBody2ID().GetIndexAndSequenceNumber();
    rc.sub1 = pair.GetSubShapeID1().GetValue();
    rc.sub2 = pair.GetSubShapeID2().GetValue();
    std::lock_guard lock(mutex_);
    raw_.push_back(rc);
}

void ContactRecorder::take(std::vector<RawContact>& out) {
    std::lock_guard lock(mutex_);
    out.swap(raw_);
    raw_.clear();
}

// ---------------------------------------------------------------------------------------------
// ContactTracker (main thread)
// ---------------------------------------------------------------------------------------------
namespace {

[[nodiscard]] u64 pair_key(JPH::uint32 body1, JPH::uint32 body2) {
    return (static_cast<u64>(body1) << 32) | body2;
}

// Jolt orders body1 < body2 already; normalise defensively (swap + flip normal).
void normalise(RawContact& rc) {
    if (rc.body1 > rc.body2) {
        std::swap(rc.body1, rc.body2);
        std::swap(rc.sub1, rc.sub2);
        std::swap(rc.user1, rc.user2);
        std::swap(rc.sensor1, rc.sensor2);
        rc.normal = -rc.normal;
    }
}

} // namespace

void ContactTracker::emit(std::vector<ContactEvent>& events, u32 max_events, ContactEventType type,
                          const PairState& pair, const RawContact* raw) {
    if (events.size() >= max_events) {
        if (!overflow_warned_) {
            AE_LOG_WARN("Physics",
                        "contact event buffer full ({} events); dropping events until drained",
                        max_events);
            overflow_warned_ = true;
        }
        return;
    }
    overflow_warned_ = false;

    ContactEvent ev;
    ev.type = type;
    ev.is_trigger = pair.sensor_a || pair.sensor_b;
    const bool swap = !pair.sensor_a && pair.sensor_b; // triggers: `a` is the sensor
    ev.a = swap ? pair.b : pair.a;
    ev.b = swap ? pair.a : pair.b;
    if (raw != nullptr) {
        ev.point = raw->point;
        ev.normal = swap ? -raw->normal : raw->normal;
        ev.penetration_depth = raw->depth;
        ev.approach_speed = raw->approach_speed;
    }
    events.push_back(ev);
}

void ContactTracker::process(std::vector<RawContact>& raw, u64 step, bool emit_persist,
                             std::vector<ContactEvent>& events, u32 max_events,
                             std::vector<DebugContact>& debug_contacts) {
    debug_contacts.clear();
    if (raw.empty())
        return;

    for (RawContact& rc : raw)
        normalise(rc);
    // Deterministic order across threads; stable so each pair keeps Jolt's callback order.
    std::stable_sort(raw.begin(), raw.end(), [](const RawContact& l, const RawContact& r) {
        return pair_key(l.body1, l.body2) < pair_key(r.body1, r.body2);
    });

    for (const RawContact& rc : raw) {
        const u64 key = pair_key(rc.body1, rc.body2);
        const u64 sub_key = (static_cast<u64>(rc.sub1) << 32) | rc.sub2;

        if (rc.kind != RawContact::Kind::Removed && debug_contacts.size() < 4096)
            debug_contacts.push_back({ rc.point, rc.normal, rc.sensor1 || rc.sensor2 });

        switch (rc.kind) {
        case RawContact::Kind::Added:
        case RawContact::Kind::Persisted: {
            auto [it, inserted] = pairs_.try_emplace(key);
            PairState& pair = it->second;
            if (inserted) {
                pair.a = user_data_to_entity(rc.user1);
                pair.b = user_data_to_entity(rc.user2);
                pair.sensor_a = rc.sensor1;
                pair.sensor_b = rc.sensor2;
            }
            const bool was_touching = !pair.sub_pairs.empty();
            if (std::find(pair.sub_pairs.begin(), pair.sub_pairs.end(), sub_key) ==
                pair.sub_pairs.end())
                pair.sub_pairs.push_back(sub_key);

            if (!was_touching) {
                emit(events, max_events, ContactEventType::Begin, pair, &rc);
                pair.last_event_step = step;
            } else if (emit_persist && pair.last_event_step != step) {
                emit(events, max_events, ContactEventType::Persist, pair, &rc);
                pair.last_event_step = step;
            }
            break;
        }
        case RawContact::Kind::Removed: {
            auto it = pairs_.find(key);
            if (it == pairs_.end())
                break;
            PairState& pair = it->second;
            auto sub = std::find(pair.sub_pairs.begin(), pair.sub_pairs.end(), sub_key);
            if (sub != pair.sub_pairs.end())
                pair.sub_pairs.erase(sub);
            if (pair.sub_pairs.empty()) {
                emit(events, max_events, ContactEventType::End, pair, nullptr);
                pairs_.erase(it);
            }
            break;
        }
        }
    }
    raw.clear();
}

} // namespace aether::physics::detail
