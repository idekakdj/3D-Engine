// anim_graph_instance.cpp — AnimGraphInstance: per-character graph runtime.
//
// update(): one top-down pass computes every relevant node's weight (blend weights from
// parameters, crossfade alphas from state machines) and advances unsynced clips; sync groups are
// then resolved (shared phase -> member clip times); notifies and root motion are gathered per
// advanced clip, weighted by that clip's contribution.
// evaluate(): a second top-down pass samples the relevant clips and blends, using a stack of
// pre-allocated scratch poses (one level per tree depth) and one snapshot pose per state machine.
#include "aether/animation/anim_graph.h"

#include "aether/animation/anim_math.h"
#include "aether/animation/skeleton.h"
#include "anim_graph_internal.h"
#include "playback.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace aether::animation {

using detail::NodeType;

namespace {

struct NodeRuntime {
    f64 time = 0.0;      // clip: unwrapped playback time (seconds)
    f64 prev_time = 0.0; // clip: time before the latest advance
    f32 weight = 0.0f;   // contribution in the latest update (0 = irrelevant)
    f32 rm_weight = 0.0f;
    f32 layer_weight = 0.0f; // additive / masked layer: effective weight
    u32 active_count = 0;    // blend1d/2d: contributing children
    std::array<u32, 3> active{};
    std::array<f32, 3> active_weight{};
};

struct MachineRuntime {
    StateId    current = 0;
    StateId    source = kInvalidState;
    u32        transition = kInvalidU32;
    f32        elapsed = 0.0f;
    f32        duration = 0.0f;
    f32        alpha = 1.0f;
    BlendCurve curve = BlendCurve::Linear;
    bool       initialized = false;
    bool       transitioning = false;
    bool       source_frozen = false;
    u64        snapshot_frame = 0; // frame whose evaluation wrote the snapshot (0 = never)
    u64        last_update_frame = 0;
    u32        state_offset = 0;
    u32        snapshot_offset = 0;
};

struct SyncRuntime {
    f64 phase = 0.0;
    f64 prev_phase = 0.0;
    f32 weight_sum = 0.0f;
    f32 rate_sum = 0.0f;
};

f32 apply_curve(BlendCurve curve, f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return curve == BlendCurve::SmoothStep ? t * t * (3.0f - 2.0f * t) : t;
}

f64 norm_duration(f32 d) { return d > kAnimEpsilon ? static_cast<f64>(d) : 1.0; }

bool exit_time_reached(f64 prev, f64 cur, f32 exit_time, bool looping) {
    const f64 e = static_cast<f64>(exit_time);
    if (cur < e) {
        return false;
    }
    if (!looping || e >= 1.0) {
        return true;
    }
    // Looping, fractional exit time: satisfied when e + k (k >= 0) was crossed in (prev, cur].
    return std::floor(cur - e) > std::floor(std::max(prev - e, -0.5));
}

} // namespace

struct AnimGraphInstance::Impl {
    std::shared_ptr<const AnimGraph> graph;
    const detail::AnimGraphDef*      def = nullptr;
    const Skeleton*                  skeleton = nullptr;
    u32                              joints = 0;

    std::vector<f32>             params;
    std::vector<NodeRuntime>     nodes;
    std::vector<ClipCursor>      cursors; // per clip node payload
    std::vector<MachineRuntime>  machines;
    std::vector<f64>             state_norm; // per (machine, state): normalized time since entry
    std::vector<f64>             state_prev;
    std::vector<SyncRuntime>     groups;
    std::vector<Transform>       pose_pool;
    std::vector<Transform>       snapshots;
    std::vector<AnimNotifyEvent> events;
    RootMotionDelta              root_motion{};
    f32                          root_motion_weight = 0.0f;
    const RootMotionSettings*    rm = nullptr; // valid during update() only
    u64                          frame = 0;

    explicit Impl(std::shared_ptr<const AnimGraph> g) : graph(std::move(g)) {
        def = &graph->definition();
        skeleton = def->skeleton.get();
        joints = skeleton->joint_count();
        params.resize(def->params.size());
        nodes.resize(def->nodes.size());
        cursors.resize(def->clip_nodes.size());
        for (usize i = 0; i < cursors.size(); ++i) {
            def->clip_nodes[i].clip->prepare_cursor(cursors[i]);
        }
        machines.resize(def->state_machines.size());
        u32 states = 0;
        for (usize i = 0; i < machines.size(); ++i) {
            machines[i].state_offset = states;
            machines[i].snapshot_offset = static_cast<u32>(i) * joints;
            states += static_cast<u32>(def->state_machines[i].states.size());
        }
        state_norm.assign(states, 0.0);
        state_prev.assign(states, 0.0);
        groups.resize(def->sync_groups.size());
        pose_pool.resize(static_cast<usize>(def->pose_levels) * joints);
        snapshots.resize(machines.size() * joints);
        events.reserve(16);
        reset_parameters();
        reset();
    }

    void reset_parameters() {
        for (usize i = 0; i < params.size(); ++i) {
            params[i] = def->params[i].default_value;
        }
    }

    void reset() {
        for (usize n = 0; n < nodes.size(); ++n) {
            nodes[n] = NodeRuntime{};
            if (def->nodes[n].type == NodeType::Clip) {
                const f32 start = def->clip_nodes[def->nodes[n].payload].start_time;
                nodes[n].time = nodes[n].prev_time = start;
            }
        }
        for (auto& m : machines) {
            const u32 so = m.state_offset, sn = m.snapshot_offset;
            m = MachineRuntime{};
            m.state_offset = so;
            m.snapshot_offset = sn;
        }
        std::fill(state_norm.begin(), state_norm.end(), 0.0);
        std::fill(state_prev.begin(), state_prev.end(), 0.0);
        std::fill(groups.begin(), groups.end(), SyncRuntime{});
        events.clear();
        root_motion = {};
        frame = 0;
    }

    [[nodiscard]] std::span<Transform> scratch(u32 level) {
        AE_ASSERT(level < def->pose_levels);
        return {pose_pool.data() + static_cast<usize>(level) * joints, joints};
    }

    [[nodiscard]] f32 param_or_one(ParamId p) const { return p == kInvalidParam ? 1.0f : params[p]; }

    // ---------------------------------------------------------------------------------------------
    // update
    // ---------------------------------------------------------------------------------------------
    void update(f32 dt, const RootMotionSettings* settings) {
        ++frame;
        events.clear();
        root_motion = {};
        root_motion_weight = 0.0f;
        rm = (settings != nullptr && settings->joint < joints) ? settings : nullptr;
        for (auto& n : nodes) {
            n.weight = 0.0f;
            n.rm_weight = 0.0f;
        }
        for (auto& g : groups) {
            g.weight_sum = 0.0f;
            g.rate_sum = 0.0f;
        }
        update_node(def->root, dt, 1.0f, 1.0f);
        resolve_sync_groups(dt);
        if (root_motion_weight > kAnimEpsilon) {
            root_motion.translation /= root_motion_weight;
            root_motion.yaw /= root_motion_weight;
        } else {
            root_motion = {};
        }
        rm = nullptr;
    }

    void update_node(NodeId id, f32 dt, f32 w, f32 rmw) {
        const detail::NodeDef& nd = def->nodes[id];
        NodeRuntime&           rt = nodes[id];
        rt.weight = w;
        rt.rm_weight = rmw;
        switch (nd.type) {
        case NodeType::Clip: update_clip(id, nd, rt, dt); break;
        case NodeType::Blend1D:
            compute_blend1d(def->blend1d[nd.payload], rt);
            update_active(nd, rt, dt, w, rmw);
            break;
        case NodeType::Blend2D: {
            const auto& bd = def->blend2d[nd.payload];
            u32         index[3] = {};
            f32         weight[3] = {};
            const Vec2  p(params[bd.param_x], params[bd.param_y]);
            rt.active_count = detail::blend2d_weights(bd.points, bd.triangulation, p, index, weight);
            for (u32 k = 0; k < rt.active_count; ++k) {
                rt.active[k] = index[k];
                rt.active_weight[k] = weight[k];
            }
            update_active(nd, rt, dt, w, rmw);
            break;
        }
        case NodeType::Additive: {
            const auto& ad = def->additive[nd.payload];
            rt.layer_weight = std::clamp(ad.weight * param_or_one(ad.weight_param), 0.0f, 1.0f);
            update_node(nd.children[0], dt, w, rmw);
            if (rt.layer_weight > 0.0f) {
                update_node(nd.children[1], dt, w * rt.layer_weight, 0.0f); // no root motion
            }
            break;
        }
        case NodeType::MaskedLayer: {
            const auto& ld = def->layers[nd.payload];
            rt.layer_weight = std::clamp(ld.weight * param_or_one(ld.weight_param), 0.0f, 1.0f);
            const f32 root_share = rm != nullptr ? rt.layer_weight * ld.mask.weight(rm->joint) : 0.0f;
            update_node(nd.children[0], dt, w, rmw * (1.0f - root_share));
            if (rt.layer_weight > 0.0f) {
                update_node(nd.children[1], dt, w * rt.layer_weight, rmw * root_share);
            }
            break;
        }
        case NodeType::StateMachine: update_machine(nd, dt, w, rmw); break;
        }
    }

    void update_active(const detail::NodeDef& nd, const NodeRuntime& rt, f32 dt, f32 w, f32 rmw) {
        for (u32 k = 0; k < rt.active_count; ++k) {
            const f32 aw = rt.active_weight[k];
            update_node(nd.children[rt.active[k]], dt, w * aw, rmw * aw);
        }
    }

    void compute_blend1d(const detail::Blend1DDef& bd, NodeRuntime& rt) const {
        const auto& th = bd.thresholds;
        const u32   n = static_cast<u32>(th.size());
        const f32   v = params[bd.param];
        rt.active_count = 0;
        auto push = [&rt](u32 index, f32 weight) {
            if (weight > 0.0f) {
                rt.active[rt.active_count] = index;
                rt.active_weight[rt.active_count] = weight;
                ++rt.active_count;
            }
        };
        if (n == 1 || !(v > th[0])) { // also catches NaN
            push(0, 1.0f);
            return;
        }
        if (v >= th[n - 1]) {
            push(n - 1, 1.0f);
            return;
        }
        u32 i = 0;
        while (i + 2 < n && v >= th[i + 1]) {
            ++i;
        }
        const f32 span = th[i + 1] - th[i];
        const f32 a = span > kAnimEpsilon ? (v - th[i]) / span : 1.0f;
        push(i, 1.0f - a);
        push(i + 1, a);
    }

    void update_clip(NodeId id, const detail::NodeDef& nd, NodeRuntime& rt, f32 dt) {
        const detail::ClipNodeDef& cd = def->clip_nodes[nd.payload];
        const f32                  duration = cd.clip->duration();
        const f32                  speed = cd.speed * param_or_one(cd.speed_param);
        if (cd.sync_group != kNoSyncGroup) {
            SyncRuntime& g = groups[cd.sync_group];
            g.weight_sum += rt.weight;
            if (duration > kAnimEpsilon) {
                g.rate_sum += rt.weight * speed / duration;
            }
            return; // time assigned in resolve_sync_groups
        }
        rt.prev_time = rt.time;
        f64 t = rt.time + static_cast<f64>(dt) * static_cast<f64>(speed);
        if (cd.wrap == WrapMode::Clamp) {
            t = std::clamp(t, 0.0, static_cast<f64>(duration));
        }
        rt.time = t;
        after_advance(id, cd, rt);
    }

    void resolve_sync_groups(f32 dt) {
        for (usize gi = 0; gi < groups.size(); ++gi) {
            SyncRuntime& g = groups[gi];
            if (g.weight_sum <= 0.0f) {
                continue;
            }
            g.prev_phase = g.phase;
            g.phase += static_cast<f64>(dt) * static_cast<f64>(g.rate_sum / g.weight_sum);
            for (const NodeId id : def->sync_members[gi]) {
                NodeRuntime& rt = nodes[id];
                if (rt.weight <= 0.0f) {
                    continue; // not relevant this update
                }
                const detail::ClipNodeDef& cd = def->clip_nodes[def->nodes[id].payload];
                const f64                  d = static_cast<f64>(cd.clip->duration());
                rt.prev_time = g.prev_phase * d;
                rt.time = g.phase * d;
                if (cd.wrap == WrapMode::Clamp) {
                    rt.prev_time = std::clamp(rt.prev_time, 0.0, d);
                    rt.time = std::clamp(rt.time, 0.0, d);
                }
                after_advance(id, cd, rt);
            }
        }
    }

    // Notifies + root motion for a clip that moved prev_time -> time this update.
    void after_advance(NodeId id, const detail::ClipNodeDef& cd, const NodeRuntime& rt) {
        if (rm != nullptr) {
            root_motion_weight += rt.rm_weight;
        }
        if (rt.prev_time == rt.time) {
            return;
        }
        const AnimationClip& clip = *cd.clip;
        if (rt.weight > def->notify_weight_threshold) {
            detail::for_each_crossed_notify(clip, rt.prev_time, rt.time, cd.wrap,
                                            [&](const AnimNotify& n) {
                                                events.push_back({n.name, n.time, rt.weight, id,
                                                                  &clip});
                                            });
        }
        if (rm != nullptr && rt.rm_weight > 0.0f) {
            const RootMotionDelta d =
                extract_clip_root_motion(clip, *skeleton, *rm, rt.prev_time, rt.time, cd.wrap);
            root_motion.translation += d.translation * rt.rm_weight;
            root_motion.yaw += d.yaw * rt.rm_weight;
        }
    }

    // ---- state machines -------------------------------------------------------------------------
    // The clip that dominates a subtree (highest active weight), used for state progress.
    [[nodiscard]] NodeId primary_clip(NodeId id) const {
        for (;;) {
            const detail::NodeDef& nd = def->nodes[id];
            const NodeRuntime&     rt = nodes[id];
            switch (nd.type) {
            case NodeType::Clip: return id;
            case NodeType::Blend1D:
            case NodeType::Blend2D: {
                if (rt.active_count == 0) {
                    return kInvalidNode;
                }
                u32 best = 0;
                for (u32 k = 1; k < rt.active_count; ++k) {
                    if (rt.active_weight[k] > rt.active_weight[best]) {
                        best = k;
                    }
                }
                id = nd.children[rt.active[best]];
                break;
            }
            case NodeType::Additive:
            case NodeType::MaskedLayer: id = nd.children[0]; break;
            case NodeType::StateMachine: {
                const auto& sm = def->state_machines[nd.payload];
                id = sm.states[machines[nd.payload].current].node;
                break;
            }
            }
        }
    }

    // Normalized progress of a state's node during its latest update.
    [[nodiscard]] f64 node_progress(NodeId node) const {
        const NodeId clip = primary_clip(node);
        if (clip == kInvalidNode) {
            return 0.0;
        }
        const NodeRuntime& rt = nodes[clip];
        const f32 d = def->clip_nodes[def->nodes[clip].payload].clip->duration();
        return (rt.time - rt.prev_time) / norm_duration(d);
    }

    [[nodiscard]] bool node_looping(NodeId node) const {
        const NodeId clip = primary_clip(node);
        return clip != kInvalidNode &&
               def->clip_nodes[def->nodes[clip].payload].wrap != WrapMode::Clamp;
    }

    void reset_subtree(NodeId id) {
        const detail::NodeDef& nd = def->nodes[id];
        NodeRuntime&           rt = nodes[id];
        switch (nd.type) {
        case NodeType::Clip: {
            rt.time = rt.prev_time = def->clip_nodes[nd.payload].start_time;
            return;
        }
        case NodeType::StateMachine: {
            MachineRuntime& m = machines[nd.payload];
            m.initialized = false; // re-enters its entry state on the next update
            m.transitioning = false;
            return;
        }
        default:
            for (const NodeId c : nd.children) {
                reset_subtree(c);
            }
            return;
        }
    }

    void enter_state(const detail::StateMachineDef& sd, MachineRuntime& m, StateId s, bool force_reset) {
        if (sd.states[s].reset_on_enter || force_reset) {
            reset_subtree(sd.states[s].node);
        }
        state_norm[m.state_offset + s] = 0.0;
        state_prev[m.state_offset + s] = 0.0;
    }

    [[nodiscard]] bool conditions_pass(const detail::TransitionDef& tr) const {
        for (const TransitionCondition& c : tr.conditions) {
            const f32 v = params[c.param];
            bool      ok = false;
            switch (c.op) {
            case ConditionOp::Greater: ok = v > c.value; break;
            case ConditionOp::Less: ok = v < c.value; break;
            case ConditionOp::Equal: ok = std::abs(v - c.value) <= 1.0e-5f; break;
            case ConditionOp::NotEqual: ok = std::abs(v - c.value) > 1.0e-5f; break;
            case ConditionOp::IsTrue:
            case ConditionOp::Triggered: ok = v != 0.0f; break;
            case ConditionOp::IsFalse: ok = v == 0.0f; break;
            }
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    // `from` is the live state whose progress drives the exit time (ignored for any-state).
    [[nodiscard]] bool transition_passes(const detail::StateMachineDef& sd, const MachineRuntime& m,
                                         u32 t, StateId from) const {
        const detail::TransitionDef& tr = sd.transitions[t];
        if (tr.from == detail::kAnyState && tr.to == m.current && !tr.options.can_transition_to_self) {
            return false;
        }
        if (tr.options.has_exit_time && tr.from != detail::kAnyState) {
            const u32 i = m.state_offset + from;
            if (!exit_time_reached(state_prev[i], state_norm[i], tr.options.exit_time,
                                   node_looping(sd.states[from].node))) {
                return false;
            }
        }
        return conditions_pass(tr);
    }

    [[nodiscard]] u32 pick_from(const detail::StateMachineDef& sd, const MachineRuntime& m,
                                const std::vector<u32>& list, StateId from, u32 exclude) const {
        for (const u32 t : list) {
            if (t != exclude && transition_passes(sd, m, t, from)) {
                return t;
            }
        }
        return kInvalidU32;
    }

    [[nodiscard]] u32 pick_transition(const detail::StateMachineDef& sd, const MachineRuntime& m) const {
        if (!m.transitioning) {
            const u32 t = pick_from(sd, m, sd.any_state, m.current, kInvalidU32);
            return t != kInvalidU32 ? t : pick_from(sd, m, sd.outgoing[m.current], m.current, kInvalidU32);
        }
        const InterruptionSource rule = sd.transitions[m.transition].options.interruption;
        if (rule == InterruptionSource::None) {
            return kInvalidU32;
        }
        const u32 active = m.transition;
        u32       t = pick_from(sd, m, sd.any_state, m.current, active);
        const bool live_source = !m.source_frozen && m.source != kInvalidState;
        auto from_source = [&]() {
            return live_source ? pick_from(sd, m, sd.outgoing[m.source], m.source, active) : kInvalidU32;
        };
        auto from_dest = [&]() { return pick_from(sd, m, sd.outgoing[m.current], m.current, active); };
        if (t == kInvalidU32) {
            switch (rule) {
            case InterruptionSource::Source: t = from_source(); break;
            case InterruptionSource::Destination: t = from_dest(); break;
            case InterruptionSource::SourceThenDestination:
                t = from_source();
                t = t != kInvalidU32 ? t : from_dest();
                break;
            case InterruptionSource::DestinationThenSource:
                t = from_dest();
                t = t != kInvalidU32 ? t : from_source();
                break;
            case InterruptionSource::None: break;
            }
        }
        // A transition into the state we are already heading to is not an interruption.
        if (t != kInvalidU32 && sd.transitions[t].to == m.current &&
            !sd.transitions[t].options.can_transition_to_self) {
            return kInvalidU32;
        }
        return t;
    }

    void start_transition(const detail::StateMachineDef& sd, MachineRuntime& m, u32 t) {
        const detail::TransitionDef& tr = sd.transitions[t];
        for (const TransitionCondition& c : tr.conditions) {
            if (c.op == ConditionOp::Triggered) {
                params[c.param] = 0.0f; // consume
            }
        }
        const StateId from = m.current;
        const StateId to = tr.to;
        const bool    snapshot_ok = m.snapshot_frame != 0 && m.snapshot_frame + 1 == frame;
        const bool    needs_snapshot = m.transitioning || from == to;
        if (tr.options.duration <= 0.0f || (needs_snapshot && !snapshot_ok)) {
            m.transitioning = false; // instant switch
            m.source = kInvalidState;
            m.transition = kInvalidU32;
            m.alpha = 1.0f;
            m.current = to;
            enter_state(sd, m, to, from == to);
            return;
        }
        m.source_frozen = needs_snapshot;
        m.source = needs_snapshot ? kInvalidState : from;
        m.transition = t;
        m.transitioning = true;
        m.elapsed = 0.0f;
        m.duration = tr.options.duration;
        m.curve = tr.options.curve;
        m.alpha = 0.0f;
        m.current = to;
        enter_state(sd, m, to, from == to);
    }

    void update_machine(const detail::NodeDef& nd, f32 dt, f32 w, f32 rmw) {
        const detail::StateMachineDef& sd = def->state_machines[nd.payload];
        MachineRuntime&                m = machines[nd.payload];
        if (!m.initialized) {
            m = MachineRuntime{.state_offset = m.state_offset, .snapshot_offset = m.snapshot_offset};
            m.current = sd.entry;
            m.initialized = true;
            enter_state(sd, m, sd.entry, true);
        } else if (m.last_update_frame + 1 == frame) {
            // Accumulate the progress live states made during the previous update.
            auto advance = [&](StateId s) {
                const u32 i = m.state_offset + s;
                state_prev[i] = state_norm[i];
                state_norm[i] += node_progress(sd.states[s].node);
            };
            advance(m.current);
            if (m.transitioning && !m.source_frozen) {
                advance(m.source);
            }
        }
        m.last_update_frame = frame;

        const u32 t = pick_transition(sd, m);
        if (t != kInvalidU32) {
            start_transition(sd, m, t);
        }
        if (m.transitioning) {
            m.elapsed += dt;
            if (m.elapsed >= m.duration) {
                m.transitioning = false;
                m.source_frozen = false;
                m.source = kInvalidState;
                m.transition = kInvalidU32;
                m.alpha = 1.0f;
            } else {
                m.alpha = apply_curve(m.curve, m.elapsed / m.duration);
            }
        }
        if (m.transitioning) {
            const f32 a = m.alpha;
            if (!m.source_frozen) {
                update_node(sd.states[m.source].node, dt, w * (1.0f - a), rmw * (1.0f - a));
            }
            update_node(sd.states[m.current].node, dt, w * a, rmw * a);
        } else {
            update_node(sd.states[m.current].node, dt, w, rmw);
        }
    }

    // ---------------------------------------------------------------------------------------------
    // evaluate
    // ---------------------------------------------------------------------------------------------
    void copy_bind(std::span<Transform> out) const {
        const auto bind = skeleton->bind_pose();
        std::copy(bind.begin(), bind.end(), out.begin());
    }

    void eval_node(NodeId id, std::span<Transform> out, u32 level) {
        const detail::NodeDef& nd = def->nodes[id];
        const NodeRuntime&     rt = nodes[id];
        switch (nd.type) {
        case NodeType::Clip: {
            const detail::ClipNodeDef& cd = def->clip_nodes[nd.payload];
            copy_bind(out);
            cd.clip->sample(wrap_time(rt.time, cd.clip->duration(), cd.wrap), out,
                            &cursors[nd.payload]);
            return;
        }
        case NodeType::Blend1D:
        case NodeType::Blend2D: {
            if (rt.active_count == 0) {
                copy_bind(out);
                return;
            }
            eval_node(nd.children[rt.active[0]], out, level);
            f32 accumulated = rt.active_weight[0];
            for (u32 k = 1; k < rt.active_count; ++k) {
                const std::span<Transform> tmp = scratch(level);
                eval_node(nd.children[rt.active[k]], tmp, level + 1);
                const f32 wk = rt.active_weight[k];
                blend_poses(out, tmp, wk / (accumulated + wk), out);
                accumulated += wk;
            }
            return;
        }
        case NodeType::Additive: {
            eval_node(nd.children[0], out, level);
            if (rt.layer_weight > 0.0f) {
                const detail::AdditiveDef& ad = def->additive[nd.payload];
                const std::span<Transform> tmp = scratch(level);
                eval_node(nd.children[1], tmp, level + 1);
                make_additive(tmp, ad.reference, tmp);
                if (ad.mask.empty()) {
                    apply_additive(out, tmp, rt.layer_weight, out);
                } else {
                    apply_additive_masked(out, tmp, rt.layer_weight, ad.mask, out);
                }
            }
            return;
        }
        case NodeType::MaskedLayer: {
            eval_node(nd.children[0], out, level);
            if (rt.layer_weight > 0.0f) {
                const std::span<Transform> tmp = scratch(level);
                eval_node(nd.children[1], tmp, level + 1);
                blend_poses_masked(out, tmp, rt.layer_weight, def->layers[nd.payload].mask, out);
            }
            return;
        }
        case NodeType::StateMachine: {
            const detail::StateMachineDef& sd = def->state_machines[nd.payload];
            MachineRuntime&                m = machines[nd.payload];
            const std::span<Transform>     snapshot(snapshots.data() + m.snapshot_offset, joints);
            if (m.transitioning) {
                if (m.source_frozen) {
                    std::copy(snapshot.begin(), snapshot.end(), out.begin());
                } else {
                    eval_node(sd.states[m.source].node, out, level);
                }
                const std::span<Transform> tmp = scratch(level);
                eval_node(sd.states[m.current].node, tmp, level + 1);
                blend_poses(out, tmp, m.alpha, out);
            } else {
                eval_node(sd.states[m.current].node, out, level);
            }
            std::copy(out.begin(), out.end(), snapshot.begin());
            m.snapshot_frame = frame;
            return;
        }
        }
    }

    void evaluate(std::span<Transform> out) {
        AE_ASSERT(out.size() >= joints);
        if (frame == 0) {
            update(0.0f, nullptr);
        }
        eval_node(def->root, out.first(joints), 0);
    }

    // ---- introspection helpers ----
    [[nodiscard]] const MachineRuntime* machine(NodeId sm) const {
        if (sm >= def->nodes.size() || def->nodes[sm].type != NodeType::StateMachine) {
            return nullptr;
        }
        return &machines[def->nodes[sm].payload];
    }
};

// ================================================================================================
// Public forwarding
// ================================================================================================
AnimGraphInstance::AnimGraphInstance() = default;
AnimGraphInstance::AnimGraphInstance(std::shared_ptr<const AnimGraph> graph) {
    if (graph) {
        impl_ = std::make_unique<Impl>(std::move(graph));
    }
}
AnimGraphInstance::~AnimGraphInstance() = default;
AnimGraphInstance::AnimGraphInstance(const AnimGraphInstance& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}
AnimGraphInstance& AnimGraphInstance::operator=(const AnimGraphInstance& other) {
    if (this != &other) {
        impl_ = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
    }
    return *this;
}
AnimGraphInstance::AnimGraphInstance(AnimGraphInstance&&) noexcept = default;
AnimGraphInstance& AnimGraphInstance::operator=(AnimGraphInstance&&) noexcept = default;

const std::shared_ptr<const AnimGraph>& AnimGraphInstance::graph() const noexcept {
    static const std::shared_ptr<const AnimGraph> kNone;
    return impl_ ? impl_->graph : kNone;
}

void AnimGraphInstance::set_float(ParamId id, f32 value) {
    if (impl_ && id < impl_->params.size()) {
        impl_->params[id] = value;
    }
}
void AnimGraphInstance::set_bool(ParamId id, bool value) { set_float(id, value ? 1.0f : 0.0f); }
void AnimGraphInstance::set_trigger(ParamId id) { set_float(id, 1.0f); }
void AnimGraphInstance::reset_trigger(ParamId id) { set_float(id, 0.0f); }
f32  AnimGraphInstance::get_float(ParamId id) const {
    return impl_ && id < impl_->params.size() ? impl_->params[id] : 0.0f;
}
bool AnimGraphInstance::get_bool(ParamId id) const { return get_float(id) != 0.0f; }
bool AnimGraphInstance::is_trigger_set(ParamId id) const { return get_float(id) != 0.0f; }

namespace {
ParamId lookup(const std::shared_ptr<const AnimGraph>& g, std::string_view name, ParamType type) {
    if (!g) {
        return kInvalidParam;
    }
    const ParamId id = g->find_parameter(name);
    return id != kInvalidParam && g->parameter_type(id) == type ? id : kInvalidParam;
}
} // namespace

bool AnimGraphInstance::set_float(std::string_view name, f32 value) {
    const ParamId id = lookup(graph(), name, ParamType::Float);
    set_float(id, value);
    return id != kInvalidParam;
}
bool AnimGraphInstance::set_bool(std::string_view name, bool value) {
    const ParamId id = lookup(graph(), name, ParamType::Bool);
    set_bool(id, value);
    return id != kInvalidParam;
}
bool AnimGraphInstance::set_trigger(std::string_view name) {
    const ParamId id = lookup(graph(), name, ParamType::Trigger);
    set_trigger(id);
    return id != kInvalidParam;
}

void AnimGraphInstance::reset() {
    if (impl_) {
        impl_->reset();
    }
}
void AnimGraphInstance::reset_parameters() {
    if (impl_) {
        impl_->reset_parameters();
    }
}
void AnimGraphInstance::update(f32 dt, const RootMotionSettings* root_motion) {
    if (impl_) {
        impl_->update(dt, root_motion);
    }
}
void AnimGraphInstance::evaluate(std::span<Transform> out_local) {
    if (impl_) {
        impl_->evaluate(out_local);
    }
}

std::span<const AnimNotifyEvent> AnimGraphInstance::notifies() const noexcept {
    return impl_ ? std::span<const AnimNotifyEvent>(impl_->events) : std::span<const AnimNotifyEvent>{};
}
const RootMotionDelta& AnimGraphInstance::root_motion() const noexcept {
    static const RootMotionDelta kNone{};
    return impl_ ? impl_->root_motion : kNone;
}

StateId AnimGraphInstance::current_state(NodeId sm) const {
    const auto* m = impl_ ? impl_->machine(sm) : nullptr;
    return m != nullptr && m->initialized ? m->current : kInvalidState;
}
bool AnimGraphInstance::in_transition(NodeId sm) const {
    const auto* m = impl_ ? impl_->machine(sm) : nullptr;
    return m != nullptr && m->transitioning;
}
StateId AnimGraphInstance::transition_source(NodeId sm) const {
    const auto* m = impl_ ? impl_->machine(sm) : nullptr;
    return m != nullptr && m->transitioning ? m->source : kInvalidState;
}
f32 AnimGraphInstance::transition_alpha(NodeId sm) const {
    const auto* m = impl_ ? impl_->machine(sm) : nullptr;
    return m != nullptr && m->transitioning ? m->alpha : 1.0f;
}
f64 AnimGraphInstance::state_normalized_time(NodeId sm, StateId state) const {
    const auto* m = impl_ ? impl_->machine(sm) : nullptr;
    if (m == nullptr || state >= impl_->graph->state_count(sm)) {
        return 0.0;
    }
    return impl_->state_norm[m->state_offset + state];
}
f32 AnimGraphInstance::node_weight(NodeId node) const {
    return impl_ && node < impl_->nodes.size() ? impl_->nodes[node].weight : 0.0f;
}
f32 AnimGraphInstance::clip_time(NodeId node) const {
    if (!impl_ || node >= impl_->nodes.size() || impl_->def->nodes[node].type != NodeType::Clip) {
        return 0.0f;
    }
    const auto& cd = impl_->def->clip_nodes[impl_->def->nodes[node].payload];
    return wrap_time(impl_->nodes[node].time, cd.clip->duration(), cd.wrap);
}
f64 AnimGraphInstance::sync_phase(SyncGroupId group) const {
    return impl_ && group < impl_->groups.size() ? impl_->groups[group].phase : 0.0;
}

} // namespace aether::animation
