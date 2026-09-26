// clone.cpp — in-world deep copy of an entity subtree (editor Duplicate / prefab instancing).
//
// Components are copied generically through EnTT's type-erased storage API
// (basic_sparse_set::push(entity, const void* value) copy-constructs the element when the type
// is copyable and fires on_construct), so components of every module are duplicated without
// the scene module knowing their types.
#include "aether/core/log.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/id.h"
#include "aether/scene/scene_serializer.h"
#include "scene_internal.h"

#include <vector>

namespace aether::scene {
namespace {

Entity clone_impl(World& world, Entity source, Entity parent, Entity before) {
    entt::registry& reg = world.registry();
    if (!reg.valid(source)) {
        AE_LOG_WARN(detail::kLogCategory, "clone_entity: invalid source entity {}",
                    detail::to_string(source));
        return kNullEntity;
    }
    if (parent != kNullEntity && !reg.valid(parent)) {
        AE_LOG_WARN(detail::kLogCategory, "clone_entity: invalid parent {}; cloning as a root",
                    detail::to_string(parent));
        parent = kNullEntity;
        before = kNullEntity;
    }

    // Snapshot the source subtree (pre-order) with each node's parent index, so listeners
    // reacting to the copies cannot disturb the traversal.
    struct Node {
        Entity src;
        usize  parent_index; // index into `nodes`; SIZE_MAX for the subtree root
    };
    std::vector<Node> nodes;
    nodes.push_back({ source, SIZE_MAX });
    {
        std::vector<usize> open; // stack of indices of the current ancestor chain
        open.push_back(0);
        for_each_descendant(world, source, [&](Entity d) {
            const Entity p = parent_of(world, d);
            while (nodes[open.back()].src != p) {
                open.pop_back();
            }
            nodes.push_back({ d, open.back() });
            open.push_back(nodes.size() - 1);
        });
    }

    // Storages to copy: everything except the World-managed hierarchy and identity.
    std::vector<entt::sparse_set*> pools;
    for ([[maybe_unused]] auto [id, pool] : reg.storage()) {
        if (pool.type() == entt::type_id<HierarchyComponent>() ||
            pool.type() == entt::type_id<IdComponent>()) {
            continue;
        }
        pools.push_back(&pool);
    }

    std::vector<Entity> copies(nodes.size(), Entity{ kNullEntity });
    for (usize i = 0; i < nodes.size(); ++i) {
        const Entity src        = nodes[i].src;
        const Entity dst_parent = i == 0 ? parent : copies[nodes[i].parent_index];
        const Entity dst        = reg.create();
        copies[i]               = dst;

        // Link first (appended as last child / root), then give it a fresh identity.
        reg.emplace<HierarchyComponent>(dst, HierarchyComponent{ dst_parent });
        reg.emplace<IdComponent>(dst);
        for (entt::sparse_set* pool : pools) {
            if (pool->contains(src) && !pool->contains(dst)) {
                pool->push(dst, pool->value(src));
            }
        }
        if (!reg.valid(src)) {
            // A listener destroyed part of the source subtree mid-copy; stop here.
            AE_LOG_WARN(detail::kLogCategory, "clone_entity: source {} vanished during copy",
                        detail::to_string(src));
            break;
        }
    }

    const Entity root = copies[0];
    if (before != kNullEntity) {
        // Place the copy right after the source among its siblings.
        detail::reparent(world, root, parent, before, false);
    }
    // The copied TransformComponents carry the source's cached world/dirty state; flag the
    // root so the whole copy is recomputed under its (possibly different) parent.
    detail::mark_dirty(reg, root);
    return root;
}

} // namespace

Entity clone_entity(World& world, Entity source) {
    if (!world.valid(source)) {
        return clone_impl(world, source, kNullEntity, kNullEntity); // logs
    }
    return clone_impl(world, source, parent_of(world, source), next_sibling_of(world, source));
}

Entity clone_entity(World& world, Entity source, Entity new_parent) {
    return clone_impl(world, source, new_parent, kNullEntity);
}

} // namespace aether::scene
