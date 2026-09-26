// aether/scene/hierarchy_utils.h — hierarchy queries, traversal and lookup.
//
// HIERARCHY MODEL
// ---------------
// HierarchyComponent holds intrusive, kNullEntity-terminated links: `parent`, `first_child`,
// and a doubly linked sibling list (`prev_sibling` / `next_sibling`) plus `child_count`.
// Root entities (parent == kNullEntity) are chained through the same sibling links into a
// world-level ROOT LIST whose head/tail live in the registry context, so roots have a stable,
// user-controllable order exactly like children do. New entities are appended (creation order
// is preserved); World::set_parent() appends; move_before()/move_after() reorder.
//
// The links are OWNED BY THE WORLD. Never add/replace/patch HierarchyComponent yourself:
//   * adding one (world.add<HierarchyComponent>(e) on an entity without it) is supported —
//     the entity is linked as the last root (or under `parent` if that field was set);
//   * removing one (or raw registry.destroy()) is supported — the entity is unlinked and its
//     children become roots (World::destroy() instead destroys the subtree);
//   * replacing/patching an existing one corrupts the lists (logged as an error).
// Use World::set_parent(), scene::set_parent_keep_world(), move_before()/move_after().
//
// TRAVERSAL
// ---------
// All traversals are iterative (no recursion, no allocation) and walk the intrusive links.
// Visitors take `Entity` and return either void or scene::Visit:
//     Visit::Continue      — keep going (descend into this entity's children)
//     Visit::SkipChildren  — do not descend below this entity (depth-first traversals only)
//     Visit::Stop          — abort the traversal
// for_each_child / for_each_root read the next sibling BEFORE invoking the visitor, so the
// visitor may destroy or reparent the entity it is given (but not its next sibling).
// Depth-first traversals must not mutate the hierarchy while running.
//
// Thread-affinity: queries/traversals are safe for concurrent readers while no thread mutates
// the world; mutators (move_before/move_after) are main-thread only.
#pragma once

#include "aether/core/types.h"
#include "aether/scene/components.h"
#include "aether/scene/entity.h"
#include "aether/scene/world.h"

#include <type_traits>

namespace aether::scene {

enum class Visit : u8 { Continue = 0, SkipChildren, Stop };

// ---- link queries (kNullEntity / 0 for invalid entities or entities outside the hierarchy) ---
[[nodiscard]] Entity parent_of(const World& world, Entity e);
[[nodiscard]] Entity first_child_of(const World& world, Entity e);
[[nodiscard]] Entity last_child_of(const World& world, Entity e);
[[nodiscard]] Entity next_sibling_of(const World& world, Entity e);
[[nodiscard]] Entity prev_sibling_of(const World& world, Entity e);
[[nodiscard]] u32    child_count_of(const World& world, Entity e);

// Root list (ordered). O(1).
[[nodiscard]] Entity first_root(const World& world);
[[nodiscard]] Entity last_root(const World& world);
[[nodiscard]] u32    root_count(const World& world);

// True if `ancestor` is a strict ancestor of `e` (an entity is not its own ancestor). O(depth).
[[nodiscard]] bool   is_ancestor(const World& world, Entity ancestor, Entity e);
// Topmost ancestor of `e` (e itself if it is a root). O(depth).
[[nodiscard]] Entity root_of(const World& world, Entity e);
// Number of ancestors (0 for roots). O(depth).
[[nodiscard]] u32    depth_of(const World& world, Entity e);
// Would World::set_parent(child, parent) be accepted? (valid entities, no self/descendant).
[[nodiscard]] bool   can_set_parent(const World& world, Entity child, Entity parent);

// ---- ordering (main thread only) ------------------------------------------------------------
// Moves `e` to sit immediately before `sibling`, adopting sibling's parent (reparenting if
// needed; LOCAL transform kept, like World::set_parent). Returns false if rejected.
bool move_before(World& world, Entity e, Entity sibling);
// Moves `e` to sit immediately after `sibling` (see move_before).
bool move_after(World& world, Entity e, Entity sibling);

// ---- lookup ---------------------------------------------------------------------------------
// First entity named `name` in depth-first pre-order over the whole hierarchy (roots in root
// order, children in sibling order). O(n).
[[nodiscard]] Entity find_by_name(const World& world, StringView name);
// First direct child of `parent` named `name` (parent == kNullEntity searches the roots).
[[nodiscard]] Entity find_child_by_name(const World& world, Entity parent, StringView name);
// First strict descendant of `root` named `name` (depth-first pre-order).
[[nodiscard]] Entity find_descendant_by_name(const World& world, Entity root, StringView name);
// Resolves a '/'-separated name path. Absolute form ("Root/Arm/Hand", leading '/' optional)
// starts at the roots; the relative overload starts at `base`'s children. Duplicate names are
// handled by backtracking (the first path in hierarchy order that fully matches wins); empty
// segments are ignored. Returns kNullEntity if nothing matches (relative "" returns `base`).
[[nodiscard]] Entity find_by_path(const World& world, StringView path);
[[nodiscard]] Entity find_by_path(const World& world, Entity base, StringView relative_path);
// Inverse of find_by_path: "Root/Arm/Hand" (entities without a NameComponent appear as "").
[[nodiscard]] String path_of(const World& world, Entity e);

// Full consistency check of every link, the root list, child counts, tail pointers and
// acyclicity. O(n). Intended for tests, asserts and editor diagnostics; on failure returns
// false and (if `out_error`) describes the first violation.
[[nodiscard]] bool validate_hierarchy(const World& world, String* out_error = nullptr);

// ---- traversal ------------------------------------------------------------------------------
namespace detail {

template <typename Fn>
[[nodiscard]] Visit invoke_visitor(Fn& fn, Entity e) {
    if constexpr (std::is_same_v<std::invoke_result_t<Fn&, Entity>, Visit>) {
        return fn(e);
    } else {
        fn(e);
        return Visit::Continue;
    }
}

// Pre-order walk of the strict descendants of `root` using the intrusive links only.
template <typename Storage, typename Fn>
void walk_descendants(const Storage& hs, Entity root, Fn& fn) {
    if (!hs.contains(root)) {
        return;
    }
    Entity cur = hs.get(root).first_child;
    while (cur != kNullEntity) {
        const Visit v = invoke_visitor(fn, cur);
        if (v == Visit::Stop) {
            return;
        }
        const HierarchyComponent& h = hs.get(cur);
        if (v == Visit::Continue && h.first_child != kNullEntity) {
            cur = h.first_child;
            continue;
        }
        // Advance to the next sibling, climbing while the current branch is exhausted.
        Entity node = cur;
        cur         = kNullEntity;
        while (node != root) {
            const HierarchyComponent& nh = hs.get(node);
            if (nh.next_sibling != kNullEntity) {
                cur = nh.next_sibling;
                break;
            }
            node = nh.parent;
        }
    }
}

} // namespace detail

// Direct children of `parent` in sibling order.
template <typename Fn>
void for_each_child(const World& world, Entity parent, Fn&& fn) {
    const auto* hs = world.registry().storage<HierarchyComponent>();
    if (hs == nullptr || !hs->contains(parent)) {
        return;
    }
    Entity child = hs->get(parent).first_child;
    while (child != kNullEntity) {
        const Entity next = hs->get(child).next_sibling;
        if (detail::invoke_visitor(fn, child) == Visit::Stop) {
            return;
        }
        child = next;
    }
}

// Root entities in root-list order (stable; creation order unless reordered).
template <typename Fn>
void for_each_root(const World& world, Fn&& fn) {
    const auto* hs = world.registry().storage<HierarchyComponent>();
    if (hs == nullptr) {
        return;
    }
    Entity root = first_root(world);
    while (root != kNullEntity) {
        const Entity next = hs->get(root).next_sibling;
        if (detail::invoke_visitor(fn, root) == Visit::Stop) {
            return;
        }
        root = next;
    }
}

// Strict descendants of `root`, depth-first pre-order, sibling order preserved. Iterative.
template <typename Fn>
void for_each_descendant(const World& world, Entity root, Fn&& fn) {
    const auto* hs = world.registry().storage<HierarchyComponent>();
    if (hs == nullptr) {
        return;
    }
    detail::walk_descendants(*hs, root, fn);
}

// Every entity in the hierarchy: each root followed by its descendants (pre-order).
template <typename Fn>
void for_each_in_hierarchy(const World& world, Fn&& fn) {
    const auto* hs = world.registry().storage<HierarchyComponent>();
    if (hs == nullptr) {
        return;
    }
    bool   stop = false;
    auto   wrapped = [&](Entity e) -> Visit {
        const Visit v = detail::invoke_visitor(fn, e);
        stop          = stop || v == Visit::Stop;
        return v;
    };
    Entity root = first_root(world);
    while (root != kNullEntity && !stop) {
        const Entity next = hs->get(root).next_sibling;
        const Visit  v    = wrapped(root);
        if (v == Visit::Stop) {
            return;
        }
        if (v == Visit::Continue) {
            detail::walk_descendants(*hs, root, wrapped);
        }
        root = next;
    }
}

} // namespace aether::scene
