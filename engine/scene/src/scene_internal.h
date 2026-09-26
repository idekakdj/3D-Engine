// scene_internal.h — PRIVATE to aether.scene. Per-world bookkeeping (stored in the registry
// context so the frozen World layout stays unchanged) and the internal primitives shared by
// the module's translation units.
#pragma once

#include "aether/core/types.h"
#include "aether/scene/components.h"
#include "aether/scene/entity.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace aether::scene::detail {

inline constexpr const char* kLogCategory = "Scene";

struct SceneContext {
    World* world = nullptr; // owning world (World is non-movable, so this stays valid)

    // Root list: roots are chained through HierarchyComponent::prev/next_sibling.
    Entity first_root = kNullEntity;
    Entity last_root  = kNullEntity;
    u32    root_count = 0;

    // Tail of each entity's child list, indexed by entity index (O(1) append). Only
    // meaningful while the entity's HierarchyComponent::first_child is not null.
    std::vector<Entity> last_child;

    // uuid -> entity. Lookups validate the entity's IdComponent, so a stale entry (possible
    // only after a raw replace of an IdComponent) can never produce a wrong answer.
    std::unordered_map<u64, Entity> uuid_to_entity;
    u64                             uuid_state = 0; // splitmix64 counter

    // Set by World::clear(): per-entity unlink/uuid bookkeeping is skipped (reset wholesale).
    bool bulk_clear = false;

    // update_transforms() scratch; capacity is retained => allocation-free steady state.
    std::vector<Entity>  dirty;
    std::vector<u32>     marks; // per entity index: (epoch << 1) | covered_by_dirty
    u32                  epoch = 0;
    TransformUpdateStats stats{};

    // Serializer extension codecs for components owned by other modules (by name).
    std::vector<ComponentCodec> codecs;
};

[[nodiscard]] inline u32 index_of(Entity e) noexcept {
    return static_cast<u32>(entt::to_entity(e));
}

[[nodiscard]] inline SceneContext& context(entt::registry& reg) {
    return reg.ctx().get<SceneContext>();
}
[[nodiscard]] inline const SceneContext& context(const entt::registry& reg) {
    return reg.ctx().get<SceneContext>();
}

// "index:version" for log messages.
[[nodiscard]] std::string to_string(Entity e);

// ---- hierarchy.cpp --------------------------------------------------------------------------
// Connects every scene-owned registry listener (hierarchy, id, visibility).
void connect_listeners(entt::registry& reg);
// Inserts an UNLINKED entity into `parent`'s child list (kNullEntity = root list) before
// `before` (kNullEntity = append).
void link(entt::registry& reg, SceneContext& ctx, Entity e, Entity parent, Entity before);
// Removes a linked entity from its current list; leaves it unlinked (all links null).
void unlink(entt::registry& reg, SceneContext& ctx, Entity e);
// Validated relink used by set_parent / move_before / keep-world. `before` must be a child of
// `parent` (or null = append). When `noop_if_same_parent`, a request whose parent is already
// the current parent returns true without reordering. Logs and returns false on rejection.
bool reparent(World& world, Entity child, Entity parent, Entity before, bool noop_if_same_parent);
// World::create/create_child implementation. uuid == 0 => fresh uuid.
Entity create_entity(World& world, std::string name, Entity parent, u64 uuid);

// ---- id.cpp -------------------------------------------------------------------------------
void seed_uuid_generator(SceneContext& ctx);
[[nodiscard]] u64 next_free_uuid(SceneContext& ctx);
void on_id_construct(entt::registry& reg, Entity e);
void on_id_update(entt::registry& reg, Entity e);
void on_id_destroy(entt::registry& reg, Entity e);

// ---- visibility.cpp -----------------------------------------------------------------------
// Re-derives visible_in_hierarchy for `e` and propagates the change down its subtree,
// stopping at entities whose derived value is unchanged.
void refresh_visibility(World& world, Entity e);
void on_visibility_construct(entt::registry& reg, Entity e);

// ---- transform.cpp ------------------------------------------------------------------------
void mark_dirty(entt::registry& reg, Entity e);

} // namespace aether::scene::detail
