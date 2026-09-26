// aether/scene/transform_utils.h — transform mutation helpers, world-space helpers and
// transform-update instrumentation.
//
// DIRTY-FLAG CONTRACT (read this before touching TransformComponent)
// -------------------------------------------------------------------
// TransformComponent::world is a CACHE of `parent.world * local.to_matrix()` that
// World::update_transforms() refreshes once per frame. The cache is refreshed only for
// entities whose `dirty` flag is set, together with their whole subtree (a dirty parent
// invalidates every descendant; descendants never need to be flagged individually).
//
//   * Whoever mutates TransformComponent::local MUST set `dirty = true`. The helpers below
//     do this for you and are the preferred way to move entities:
//         scene::set_local_position(world, e, {1, 2, 3});
//     Writing `world.get<TransformComponent>(e).local.position = ...` without setting
//     `dirty` leaves the cached world matrix (and every descendant's) stale.
//   * Hierarchy changes (World::set_parent, create_child, ...) flag the moved entity.
//   * `world` is written only by update_transforms() (or by the set_world_* helpers, which
//     convert to local space first). Never write it directly.
//   * Between a mutation and the next update_transforms(), TransformComponent::world is stale.
//     World::world_matrix(e) is always correct (it recomputes from locals when any entity on
//     the parent chain is dirty) at O(depth) cost; bulk consumers (render extraction, physics
//     sync) should run after update_transforms() and read TransformComponent::world directly.
//   * An entity without a TransformComponent acts as the identity for its children (its own
//     ancestors are ignored). World::create() always adds a TransformComponent.
//
// Thread-affinity: all mutating helpers are main-thread only; const queries are safe for
// concurrent readers while no thread mutates the world.
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"
#include "aether/scene/entity.h"

namespace aether {
class World;
} // namespace aether

namespace aether::scene {

// ---- local-space mutation (sets dirty; adds a TransformComponent if missing) ----------------
void set_local_position(World& world, Entity e, const Vec3& position);
void set_local_rotation(World& world, Entity e, const Quat& rotation);
void set_local_scale(World& world, Entity e, const Vec3& scale);
void set_local_transform(World& world, Entity e, const Transform& local);
// Flags `e` (and therefore its subtree) for recomputation by the next update_transforms().
void mark_transform_dirty(World& world, Entity e);

// Local TRS of `e` (identity Transform if it has none).
[[nodiscard]] Transform local_transform(const World& world, Entity e);

// ---- world-space helpers --------------------------------------------------------------------
// Translation part of World::world_matrix(e).
[[nodiscard]] Vec3 world_position(const World& world, Entity e);

// Re-expresses `world_matrix` in the parent's space and stores it as the local transform.
// Exact for parents with uniform scale; with non-uniform parent scale combined with rotation
// the result may contain shear, which TRS cannot represent (closest TRS is stored).
void set_world_matrix(World& world, Entity e, const Mat4& world_matrix);
// Moves `e` so its world-space position becomes `position`; local rotation/scale unchanged.
void set_world_position(World& world, Entity e, const Vec3& position);

// Like World::set_parent(), but rewrites the local transform so the entity's WORLD transform
// is preserved (Unreal's KeepWorldTransform attachment rule). Same shear caveat as
// set_world_matrix(). Returns false (and changes nothing) if the reparent is rejected
// (invalid entities, self-parenting, or `parent` inside `child`'s subtree).
bool set_parent_keep_world(World& world, Entity child, Entity parent);

// ---- TRS <-> matrix -----------------------------------------------------------------------------
// Bitwise-identical to Transform::to_matrix(), but avoids the three 4x4 multiplies.
[[nodiscard]] Mat4 compose_transform(const Transform& t);
// Decomposes an affine matrix into translation / rotation / (signed) scale. Shear is discarded
// (rotation is orthonormalised with Gram-Schmidt); a negative determinant is folded into scale.x.
[[nodiscard]] Transform decompose_transform(const Mat4& m);

// ---- instrumentation --------------------------------------------------------------------------
struct TransformUpdateStats {
    u32 dirty_found = 0; // entities that had `dirty` set when the update started
    u32 dirty_roots = 0; // dirty entities without a dirty ancestor (subtree roots recomputed)
    u32 recomputed  = 0; // world matrices written (dirty roots + all their descendants)
};

// Statistics of the most recent World::update_transforms() call.
[[nodiscard]] TransformUpdateStats last_transform_update_stats(const World& world);

} // namespace aether::scene
