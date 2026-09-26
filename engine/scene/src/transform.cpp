// transform.cpp — World::update_transforms(), World::world_matrix() and the transform helpers.
//
// update_transforms() algorithm (all passes iterative, no allocation once scratch capacity has
// been reached):
//   1. Linear scan of the dense TransformComponent storage collecting dirty entities.
//   2. Keep only DIRTY ROOTS — dirty entities none of whose ancestors is dirty. "Has a dirty
//      ancestor-or-self" is memoised per entity index with an epoch stamp, so every entity is
//      walked at most once per update: O(n) worst case even for deep chains of dirty nodes.
//   3. For each dirty root, a stackless pre-order walk (intrusive first_child / next_sibling /
//      parent links) recomputes world = parent.world * local and clears `dirty`. Parents are
//      always written before their children; dirty-root subtrees are disjoint, so every world
//      matrix is written exactly once.
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/transform_utils.h"
#include "scene_internal.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_inverse.hpp>

namespace aether {
namespace {

const Mat4 kIdentity{ 1.0f };

} // namespace

void World::update_transforms() {
    using scene::detail::index_of;
    scene::detail::SceneContext& ctx = scene::detail::context(registry_);
    auto&                        ts  = registry_.storage<TransformComponent>();
    auto&                        hs  = registry_.storage<HierarchyComponent>();

    ctx.stats = {};
    ctx.dirty.clear();

    // ---- pass 1: gather ------------------------------------------------------------------------
    for (auto [e, tc] : ts.each()) {
        if (tc.dirty) {
            ctx.dirty.push_back(e);
        }
    }
    if (ctx.dirty.empty()) {
        return;
    }
    ctx.stats.dirty_found = static_cast<u32>(ctx.dirty.size());

    // ---- pass 2: reduce to dirty roots ---------------------------------------------------------
    const usize id_range = registry_.storage<Entity>().size();
    if (ctx.marks.size() < id_range) {
        ctx.marks.resize(id_range, 0u);
    }
    if (++ctx.epoch >= (1u << 31)) {
        std::fill(ctx.marks.begin(), ctx.marks.end(), 0u);
        ctx.epoch = 1;
    }
    const u32 tag   = ctx.epoch << 1;
    u32*      marks = ctx.marks.data();

    auto parent_of = [&hs](Entity x) -> Entity {
        return hs.contains(x) ? hs.get(x).parent : Entity{ kNullEntity };
    };
    // True if `x` or any ancestor of `x` is dirty. Memoised along the walked path.
    auto covered = [&](Entity x) -> bool {
        Entity y         = x;
        bool   result    = false;
        bool   hit_dirty = false;
        while (y != kNullEntity) {
            const u32 m = marks[index_of(y)];
            if ((m & ~1u) == tag) {
                result = (m & 1u) != 0;
                break;
            }
            if (ts.contains(y) && ts.get(y).dirty) {
                result    = true;
                hit_dirty = true;
                break;
            }
            y = parent_of(y);
        }
        const u32 value = tag | (result ? 1u : 0u);
        for (Entity z = x; z != y; z = parent_of(z)) {
            marks[index_of(z)] = value;
        }
        if (hit_dirty) {
            marks[index_of(y)] = tag | 1u;
        }
        return result;
    };

    usize root_count = 0;
    for (usize i = 0, n = ctx.dirty.size(); i < n; ++i) {
        const Entity e = ctx.dirty[i];
        const Entity p = parent_of(e);
        if (p == kNullEntity || !covered(p)) {
            ctx.dirty[root_count++] = e;
        }
    }
    ctx.stats.dirty_roots = static_cast<u32>(root_count);

    // ---- pass 3: recompute dirty subtrees, parents before children ------------------------------
    auto world_of = [&ts](Entity x) -> const Mat4* {
        return (x != kNullEntity && ts.contains(x)) ? &ts.get(x).world : &kIdentity;
    };
    u32 recomputed = 0;
    for (usize i = 0; i < root_count; ++i) {
        const Entity        root = ctx.dirty[i];
        TransformComponent& rtc  = ts.get(root);
        rtc.world                = *world_of(parent_of(root)) * scene::compose_transform(rtc.local);
        rtc.dirty                = false;
        ++recomputed;
        if (!hs.contains(root)) {
            continue;
        }

        const Mat4* parent_world = &rtc.world; // world matrix of cur's parent
        Entity      cur          = hs.get(root).first_child;
        while (cur != kNullEntity) {
            const HierarchyComponent& h  = hs.get(cur);
            TransformComponent*       tc = ts.contains(cur) ? &ts.get(cur) : nullptr;
            if (tc != nullptr) {
                tc->world = *parent_world * scene::compose_transform(tc->local);
                tc->dirty = false;
                ++recomputed;
            }
            if (h.first_child != kNullEntity) {
                parent_world = tc != nullptr ? &tc->world : &kIdentity;
                cur          = h.first_child;
                continue;
            }
            // Leaf: next sibling (same parent), else climb until an ancestor has one.
            Entity node = cur;
            cur         = kNullEntity;
            for (;;) {
                const HierarchyComponent& nh = hs.get(node);
                if (nh.next_sibling != kNullEntity) {
                    cur = nh.next_sibling;
                    break;
                }
                node = nh.parent;
                if (node == root) {
                    break;
                }
                // Continuing among `node`'s siblings: their parent is node's parent.
                parent_world = world_of(hs.get(node).parent);
            }
        }
    }
    ctx.stats.recomputed = recomputed;
}

Mat4 World::world_matrix(Entity e) const {
    const TransformComponent* tc = registry_.try_get<TransformComponent>(e);
    if (tc == nullptr || !registry_.valid(e)) {
        return kIdentity;
    }
    const auto* hs        = registry_.storage<HierarchyComponent>();
    auto        parent_of = [hs](Entity x) -> Entity {
        return (hs != nullptr && hs->contains(x)) ? hs->get(x).parent : Entity{ kNullEntity };
    };

    // Find the topmost dirty entity on the chain up to the first transform-less ancestor.
    Entity topmost_dirty = kNullEntity;
    for (Entity x = e; x != kNullEntity; x = parent_of(x)) {
        const TransformComponent* xt = registry_.try_get<TransformComponent>(x);
        if (xt == nullptr) {
            break; // identity break: ancestors above do not contribute
        }
        if (xt->dirty) {
            topmost_dirty = x;
        }
    }
    if (topmost_dirty == kNullEntity) {
        return tc->world; // cache is current
    }
    // local(topmost_dirty) * ... * local(e), then the (clean) cached world above it.
    Mat4 m = scene::compose_transform(tc->local);
    for (Entity x = e; x != topmost_dirty;) {
        x = parent_of(x);
        m = scene::compose_transform(registry_.get<TransformComponent>(x).local) * m;
    }
    const Entity              above = parent_of(topmost_dirty);
    const TransformComponent* at    = above != kNullEntity
                                          ? registry_.try_get<TransformComponent>(above)
                                          : nullptr;
    return at != nullptr ? at->world * m : m;
}

} // namespace aether

namespace aether::scene {

void detail::mark_dirty(entt::registry& reg, Entity e) {
    if (TransformComponent* tc = reg.try_get<TransformComponent>(e)) {
        tc->dirty = true;
    }
}

namespace {

TransformComponent* ensure_transform(World& world, Entity e) {
    if (!world.valid(e)) {
        return nullptr;
    }
    if (TransformComponent* tc = world.try_get<TransformComponent>(e)) {
        return tc;
    }
    return &world.registry().emplace<TransformComponent>(e);
}

} // namespace

void set_local_position(World& world, Entity e, const Vec3& position) {
    if (TransformComponent* tc = ensure_transform(world, e)) {
        tc->local.position = position;
        tc->dirty          = true;
    }
}

void set_local_rotation(World& world, Entity e, const Quat& rotation) {
    if (TransformComponent* tc = ensure_transform(world, e)) {
        tc->local.rotation = rotation;
        tc->dirty          = true;
    }
}

void set_local_scale(World& world, Entity e, const Vec3& scale) {
    if (TransformComponent* tc = ensure_transform(world, e)) {
        tc->local.scale = scale;
        tc->dirty       = true;
    }
}

void set_local_transform(World& world, Entity e, const Transform& local) {
    if (TransformComponent* tc = ensure_transform(world, e)) {
        tc->local = local;
        tc->dirty = true;
    }
}

void mark_transform_dirty(World& world, Entity e) {
    detail::mark_dirty(world.registry(), e);
}

Transform local_transform(const World& world, Entity e) {
    const TransformComponent* tc = world.try_get<TransformComponent>(e);
    return tc != nullptr ? tc->local : Transform{};
}

Vec3 world_position(const World& world, Entity e) {
    return Vec3(world.world_matrix(e)[3]);
}

void set_world_matrix(World& world, Entity e, const Mat4& world_matrix) {
    if (!world.valid(e)) {
        return;
    }
    const Entity parent = parent_of(world, e);
    const Mat4   local  = parent != kNullEntity
                              ? glm::affineInverse(world.world_matrix(parent)) * world_matrix
                              : world_matrix;
    set_local_transform(world, e, decompose_transform(local));
}

void set_world_position(World& world, Entity e, const Vec3& position) {
    if (!world.valid(e)) {
        return;
    }
    const Entity parent = parent_of(world, e);
    const Vec3   local  = parent != kNullEntity
                              ? Vec3(glm::affineInverse(world.world_matrix(parent)) *
                                     Vec4(position, 1.0f))
                              : position;
    set_local_position(world, e, local);
}

bool set_parent_keep_world(World& world, Entity child, Entity parent) {
    if (!can_set_parent(world, child, parent)) {
        // Let reparent() produce the diagnostic; it will reject without side effects.
        return detail::reparent(world, child, parent, kNullEntity, true);
    }
    if (parent_of(world, child) == parent && world.has<HierarchyComponent>(child)) {
        return true;
    }
    const Mat4 child_world = world.world_matrix(child);
    const Mat4 local = parent != kNullEntity
                           ? glm::affineInverse(world.world_matrix(parent)) * child_world
                           : child_world;
    if (!detail::reparent(world, child, parent, kNullEntity, true)) {
        return false;
    }
    set_local_transform(world, child, decompose_transform(local));
    return true;
}

Mat4 compose_transform(const Transform& t) {
    const Mat3 r = glm::mat3_cast(t.rotation);
    return Mat4(Vec4(r[0] * t.scale.x, 0.0f),
                Vec4(r[1] * t.scale.y, 0.0f),
                Vec4(r[2] * t.scale.z, 0.0f),
                Vec4(t.position, 1.0f));
}

Transform decompose_transform(const Mat4& m) {
    constexpr f32 kEps = 1e-12f;
    Transform     out;
    out.position = Vec3(m[3]);

    const Vec3 c0(m[0]);
    const Vec3 c1(m[1]);
    const Vec3 c2(m[2]);
    Vec3       s(glm::length(c0), glm::length(c1), glm::length(c2));
    if (glm::dot(c0, glm::cross(c1, c2)) < 0.0f) {
        s.x = -s.x; // fold the reflection into one axis
    }
    out.scale = s;

    // Orthonormal basis via Gram-Schmidt (discards shear), with fallbacks for zero scale.
    Vec3 r0 = std::abs(s.x) > kEps ? c0 / s.x : Vec3(1.0f, 0.0f, 0.0f);
    r0      = glm::normalize(r0);
    Vec3 r1 = c1 - glm::dot(c1, r0) * r0;
    if (glm::dot(r1, r1) <= kEps) {
        r1 = std::abs(r0.x) < 0.9f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);
        r1 = r1 - glm::dot(r1, r0) * r0;
    }
    r1            = glm::normalize(r1);
    const Vec3 r2 = glm::cross(r0, r1);
    out.rotation  = glm::normalize(glm::quat_cast(Mat3(r0, r1, r2)));
    return out;
}

TransformUpdateStats last_transform_update_stats(const World& world) {
    return detail::context(world.registry()).stats;
}

} // namespace aether::scene
