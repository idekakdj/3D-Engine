// hierarchy.cpp — intrusive hierarchy lists (children + root list), the registry listeners
// that keep them consistent, reparenting, and the non-template hierarchy utilities.
#include "aether/core/error.h"
#include "aether/core/log.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/id.h"
#include "scene_internal.h"

#include <algorithm>
#include <format>
#include <vector>

namespace aether::scene::detail {
namespace {

// A view of one intrusive list: a parent's children, or the world's root list.
struct ListRef {
    Entity* first;
    Entity* last;
    u32*    count;
};

ListRef list_of(entt::registry& reg, SceneContext& ctx, Entity parent) {
    if (parent == kNullEntity) {
        return { &ctx.first_root, &ctx.last_root, &ctx.root_count };
    }
    const usize idx = index_of(parent);
    if (idx >= ctx.last_child.size()) {
        const usize grown = std::max<usize>({ idx + 1, ctx.last_child.size() * 2, 64 });
        ctx.last_child.resize(grown, Entity{ kNullEntity });
    }
    HierarchyComponent& ph   = reg.get<HierarchyComponent>(parent);
    Entity*             last = &ctx.last_child[idx];
    if (ph.first_child == kNullEntity) {
        *last = kNullEntity; // slot may be stale (recycled entity index)
    }
    return { &ph.first_child, last, &ph.child_count };
}

// ---- registry listeners -----------------------------------------------------------------------

void on_hierarchy_construct(entt::registry& reg, Entity e) {
    SceneContext&       ctx       = context(reg);
    HierarchyComponent& h         = reg.get<HierarchyComponent>(e);
    Entity              requested = h.parent;
    if (h.first_child != kNullEntity || h.next_sibling != kNullEntity ||
        h.prev_sibling != kNullEntity || h.child_count != 0) {
        AE_LOG_WARN(kLogCategory,
                    "HierarchyComponent added to entity {} with link data; links are managed by "
                    "World and were reset (only `parent` is honoured)",
                    to_string(e));
    }
    h = HierarchyComponent{};
    if (requested != kNullEntity &&
        (requested == e || !reg.valid(requested) || !reg.all_of<HierarchyComponent>(requested))) {
        AE_LOG_WARN(kLogCategory,
                    "HierarchyComponent added to entity {} requests invalid parent {}; linked as "
                    "a root instead",
                    to_string(e), to_string(requested));
        requested = kNullEntity;
    }
    link(reg, ctx, e, requested, kNullEntity);
    mark_dirty(reg, e);
    if (ctx.world != nullptr) {
        refresh_visibility(*ctx.world, e);
    }
}

void on_hierarchy_update(entt::registry&, Entity e) {
    AE_LOG_ERROR(kLogCategory,
                 "HierarchyComponent of entity {} was replaced/patched directly; hierarchy links "
                 "are owned by World (use World::set_parent). The hierarchy may be inconsistent.",
                 to_string(e));
}

void on_hierarchy_destroy(entt::registry& reg, Entity e) {
    SceneContext& ctx = context(reg);
    if (ctx.bulk_clear) {
        return;
    }
    // Orphan remaining children (World::destroy removes them first; this path is reached via
    // raw registry.destroy() or World::remove<HierarchyComponent>()): they become roots.
    Entity child = reg.get<HierarchyComponent>(e).first_child;
    while (child != kNullEntity) {
        HierarchyComponent& ch   = reg.get<HierarchyComponent>(child);
        const Entity        next = ch.next_sibling;
        ch.parent = ch.prev_sibling = ch.next_sibling = kNullEntity;
        link(reg, ctx, child, kNullEntity, kNullEntity);
        mark_dirty(reg, child);
        if (ctx.world != nullptr) {
            refresh_visibility(*ctx.world, child);
        }
        child = next;
    }
    HierarchyComponent& h = reg.get<HierarchyComponent>(e);
    h.first_child         = kNullEntity;
    h.child_count         = 0;
    unlink(reg, ctx, e);
}

// Ensures `e` has a HierarchyComponent (added as a root if missing).
void ensure_hierarchy(entt::registry& reg, Entity e) {
    if (!reg.all_of<HierarchyComponent>(e)) {
        reg.emplace<HierarchyComponent>(e);
    }
}

} // namespace

std::string to_string(Entity e) {
    if (e == kNullEntity) {
        return "null";
    }
    return std::format("{}:{}", static_cast<u32>(entt::to_entity(e)),
                       static_cast<u32>(entt::to_version(e)));
}

void connect_listeners(entt::registry& reg) {
    reg.on_construct<HierarchyComponent>().connect<&on_hierarchy_construct>();
    reg.on_update<HierarchyComponent>().connect<&on_hierarchy_update>();
    reg.on_destroy<HierarchyComponent>().connect<&on_hierarchy_destroy>();

    reg.on_construct<IdComponent>().connect<&on_id_construct>();
    reg.on_update<IdComponent>().connect<&on_id_update>();
    reg.on_destroy<IdComponent>().connect<&on_id_destroy>();

    reg.on_construct<VisibilityComponent>().connect<&on_visibility_construct>();
}

void link(entt::registry& reg, SceneContext& ctx, Entity e, Entity parent, Entity before) {
    const ListRef       list = list_of(reg, ctx, parent);
    HierarchyComponent& h    = reg.get<HierarchyComponent>(e);
    h.parent                 = parent;
    if (before == kNullEntity) {
        h.prev_sibling = *list.last;
        h.next_sibling = kNullEntity;
        if (*list.last != kNullEntity) {
            reg.get<HierarchyComponent>(*list.last).next_sibling = e;
        } else {
            *list.first = e;
        }
        *list.last = e;
    } else {
        HierarchyComponent& hb = reg.get<HierarchyComponent>(before);
        AE_ASSERT(hb.parent == parent);
        h.prev_sibling = hb.prev_sibling;
        h.next_sibling = before;
        if (hb.prev_sibling != kNullEntity) {
            reg.get<HierarchyComponent>(hb.prev_sibling).next_sibling = e;
        } else {
            *list.first = e;
        }
        hb.prev_sibling = e;
    }
    ++*list.count;
}

void unlink(entt::registry& reg, SceneContext& ctx, Entity e) {
    HierarchyComponent& h    = reg.get<HierarchyComponent>(e);
    const ListRef       list = list_of(reg, ctx, h.parent);
    if (h.prev_sibling != kNullEntity) {
        reg.get<HierarchyComponent>(h.prev_sibling).next_sibling = h.next_sibling;
    } else {
        *list.first = h.next_sibling;
    }
    if (h.next_sibling != kNullEntity) {
        reg.get<HierarchyComponent>(h.next_sibling).prev_sibling = h.prev_sibling;
    } else {
        *list.last = h.prev_sibling;
    }
    AE_ASSERT(*list.count > 0);
    --*list.count;
    h.parent = h.prev_sibling = h.next_sibling = kNullEntity;
}

bool reparent(World& world, Entity child, Entity parent, Entity before, bool noop_if_same_parent) {
    entt::registry& reg = world.registry();
    if (!reg.valid(child)) {
        AE_LOG_WARN(kLogCategory, "set_parent: invalid child entity {}", to_string(child));
        return false;
    }
    if (parent != kNullEntity && !reg.valid(parent)) {
        AE_LOG_WARN(kLogCategory, "set_parent: invalid parent entity {} for {}", to_string(parent),
                    to_string(child));
        return false;
    }
    if (parent == child) {
        AE_LOG_WARN(kLogCategory, "set_parent: entity {} cannot be its own parent",
                    to_string(child));
        return false;
    }
    if (parent != kNullEntity && is_ancestor(world, child, parent)) {
        AE_LOG_WARN(kLogCategory,
                    "set_parent: rejected, parent {} is a descendant of {} (would create a cycle)",
                    to_string(parent), to_string(child));
        return false;
    }
    ensure_hierarchy(reg, child);
    if (parent != kNullEntity) {
        ensure_hierarchy(reg, parent);
    }
    if (before != kNullEntity) {
        const HierarchyComponent* hb = reg.try_get<HierarchyComponent>(before);
        if (!reg.valid(before) || hb == nullptr || hb->parent != parent) {
            AE_LOG_WARN(kLogCategory, "move: reference sibling {} is not a child of {}",
                        to_string(before), to_string(parent));
            return false;
        }
    }

    const HierarchyComponent& h = reg.get<HierarchyComponent>(child);
    if (h.parent == parent) {
        if (noop_if_same_parent || before == child || before == h.next_sibling ||
            (before == kNullEntity && h.next_sibling == kNullEntity)) {
            return true; // already in place: no relink, no dirtying
        }
    }

    SceneContext& ctx = context(reg);
    unlink(reg, ctx, child);
    link(reg, ctx, child, parent, before);
    mark_dirty(reg, child);
    refresh_visibility(world, child);
    return true;
}

Entity create_entity(World& world, std::string name, Entity parent, u64 uuid) {
    entt::registry& reg = world.registry();
    if (parent != kNullEntity) {
        ensure_hierarchy(reg, parent);
    }
    const Entity e = reg.create();
    reg.emplace<NameComponent>(e, std::move(name));
    reg.emplace<TransformComponent>(e);
    reg.emplace<VisibilityComponent>(e);
    // The construct listener links the entity (as the last child of `parent`, or last root)
    // and derives visible_in_hierarchy from the parent.
    reg.emplace<HierarchyComponent>(e, HierarchyComponent{ parent });
    reg.emplace<IdComponent>(e, IdComponent{ uuid }); // listener assigns/validates the uuid
    return e;
}

} // namespace aether::scene::detail

// =================================================================================================
// Public hierarchy utilities
// =================================================================================================
namespace aether::scene {
namespace {

const HierarchyComponent* hier(const World& world, Entity e) {
    return world.try_get<HierarchyComponent>(e);
}

bool name_equals(const World& world, Entity e, StringView name) {
    const NameComponent* n = world.try_get<NameComponent>(e);
    return n != nullptr && n->name == name;
}

// First sibling at or after `start` whose name matches.
Entity match_from(const World& world, Entity start, StringView name) {
    for (Entity cur = start; cur != kNullEntity;) {
        if (name_equals(world, cur, name)) {
            return cur;
        }
        const HierarchyComponent* h = hier(world, cur);
        cur                         = h != nullptr ? h->next_sibling : Entity{ kNullEntity };
    }
    return kNullEntity;
}

std::vector<StringView> split_path(StringView path) {
    std::vector<StringView> segments;
    usize                   pos = 0;
    while (pos <= path.size()) {
        const usize slash = path.find('/', pos);
        const usize end   = slash == StringView::npos ? path.size() : slash;
        if (end > pos) {
            segments.push_back(path.substr(pos, end - pos));
        }
        if (slash == StringView::npos) {
            break;
        }
        pos = slash + 1;
    }
    return segments;
}

// Backtracking resolution: `first_candidate` is the first entity of the list that segment 0
// is matched against (first root, or base's first child).
Entity resolve_path(const World& world, Entity first_candidate, const std::vector<StringView>& segs) {
    if (segs.empty()) {
        return kNullEntity;
    }
    std::vector<Entity> cand(segs.size(), Entity{ kNullEntity });
    usize               k = 0;
    cand[0]               = match_from(world, first_candidate, segs[0]);
    for (;;) {
        if (cand[k] == kNullEntity) {
            if (k == 0) {
                return kNullEntity;
            }
            --k; // backtrack: try the next sibling matching this level's segment
            cand[k] = match_from(world, next_sibling_of(world, cand[k]), segs[k]);
            continue;
        }
        if (k + 1 == segs.size()) {
            return cand[k];
        }
        cand[k + 1] = match_from(world, first_child_of(world, cand[k]), segs[k + 1]);
        ++k;
    }
}

} // namespace

Entity parent_of(const World& world, Entity e) {
    const HierarchyComponent* h = hier(world, e);
    return h != nullptr ? h->parent : Entity{ kNullEntity };
}

Entity first_child_of(const World& world, Entity e) {
    const HierarchyComponent* h = hier(world, e);
    return h != nullptr ? h->first_child : Entity{ kNullEntity };
}

Entity last_child_of(const World& world, Entity e) {
    const HierarchyComponent* h = hier(world, e);
    if (h == nullptr || h->first_child == kNullEntity) {
        return kNullEntity;
    }
    const detail::SceneContext& ctx = detail::context(world.registry());
    const usize                 idx = detail::index_of(e);
    return idx < ctx.last_child.size() ? ctx.last_child[idx] : Entity{ kNullEntity };
}

Entity next_sibling_of(const World& world, Entity e) {
    const HierarchyComponent* h = hier(world, e);
    return h != nullptr ? h->next_sibling : Entity{ kNullEntity };
}

Entity prev_sibling_of(const World& world, Entity e) {
    const HierarchyComponent* h = hier(world, e);
    return h != nullptr ? h->prev_sibling : Entity{ kNullEntity };
}

u32 child_count_of(const World& world, Entity e) {
    const HierarchyComponent* h = hier(world, e);
    return h != nullptr ? h->child_count : 0u;
}

Entity first_root(const World& world) {
    return detail::context(world.registry()).first_root;
}

Entity last_root(const World& world) {
    return detail::context(world.registry()).last_root;
}

u32 root_count(const World& world) {
    return detail::context(world.registry()).root_count;
}

bool is_ancestor(const World& world, Entity ancestor, Entity e) {
    if (ancestor == kNullEntity || !world.valid(ancestor) || !world.valid(e)) {
        return false;
    }
    const auto* hs = world.registry().storage<HierarchyComponent>();
    if (hs == nullptr) {
        return false;
    }
    // Bounded walk: a (corrupted) cycle can never hang the caller.
    usize budget = hs->size() + 1;
    for (Entity cur = hs->contains(e) ? hs->get(e).parent : Entity{ kNullEntity };
         cur != kNullEntity && budget != 0; --budget) {
        if (cur == ancestor) {
            return true;
        }
        cur = hs->contains(cur) ? hs->get(cur).parent : Entity{ kNullEntity };
    }
    return false;
}

Entity root_of(const World& world, Entity e) {
    if (!world.valid(e)) {
        return kNullEntity;
    }
    Entity cur = e;
    for (Entity p = parent_of(world, cur); p != kNullEntity; p = parent_of(world, cur)) {
        cur = p;
    }
    return cur;
}

u32 depth_of(const World& world, Entity e) {
    u32 depth = 0;
    for (Entity p = parent_of(world, e); p != kNullEntity; p = parent_of(world, p)) {
        ++depth;
    }
    return depth;
}

bool can_set_parent(const World& world, Entity child, Entity parent) {
    if (!world.valid(child) || parent == child) {
        return false;
    }
    if (parent == kNullEntity) {
        return true;
    }
    return world.valid(parent) && !is_ancestor(world, child, parent);
}

bool move_before(World& world, Entity e, Entity sibling) {
    if (sibling == e) {
        return world.valid(e);
    }
    if (!world.valid(sibling) || !world.has<HierarchyComponent>(sibling)) {
        AE_LOG_WARN(detail::kLogCategory, "move_before: invalid reference sibling {}",
                    detail::to_string(sibling));
        return false;
    }
    return detail::reparent(world, e, parent_of(world, sibling), sibling, false);
}

bool move_after(World& world, Entity e, Entity sibling) {
    if (sibling == e) {
        return world.valid(e);
    }
    if (!world.valid(sibling) || !world.has<HierarchyComponent>(sibling)) {
        AE_LOG_WARN(detail::kLogCategory, "move_after: invalid reference sibling {}",
                    detail::to_string(sibling));
        return false;
    }
    Entity before = next_sibling_of(world, sibling);
    if (before == e) {
        // Already directly after `sibling` (same parent); nothing to do.
        return true;
    }
    return detail::reparent(world, e, parent_of(world, sibling), before, false);
}

Entity find_by_name(const World& world, StringView name) {
    Entity found = kNullEntity;
    for_each_in_hierarchy(world, [&](Entity e) {
        if (name_equals(world, e, name)) {
            found = e;
            return Visit::Stop;
        }
        return Visit::Continue;
    });
    return found;
}

Entity find_child_by_name(const World& world, Entity parent, StringView name) {
    const Entity first = parent == kNullEntity ? first_root(world) : first_child_of(world, parent);
    return match_from(world, first, name);
}

Entity find_descendant_by_name(const World& world, Entity root, StringView name) {
    Entity found = kNullEntity;
    for_each_descendant(world, root, [&](Entity e) {
        if (name_equals(world, e, name)) {
            found = e;
            return Visit::Stop;
        }
        return Visit::Continue;
    });
    return found;
}

Entity find_by_path(const World& world, StringView path) {
    return resolve_path(world, first_root(world), split_path(path));
}

Entity find_by_path(const World& world, Entity base, StringView relative_path) {
    if (!world.valid(base)) {
        return kNullEntity;
    }
    const std::vector<StringView> segs = split_path(relative_path);
    if (segs.empty()) {
        return base;
    }
    return resolve_path(world, first_child_of(world, base), segs);
}

String path_of(const World& world, Entity e) {
    if (!world.valid(e)) {
        return {};
    }
    std::vector<Entity> chain;
    for (Entity cur = e; cur != kNullEntity; cur = parent_of(world, cur)) {
        chain.push_back(cur);
    }
    String out;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!out.empty() || it != chain.rbegin()) {
            out += '/';
        }
        if (const NameComponent* n = world.try_get<NameComponent>(*it)) {
            out += n->name;
        }
    }
    return out;
}

bool validate_hierarchy(const World& world, String* out_error) {
    const entt::registry&       reg = world.registry();
    const detail::SceneContext& ctx = detail::context(reg);
    const auto*                 hs  = reg.storage<HierarchyComponent>();

    auto fail = [&](String message) {
        if (out_error != nullptr) {
            *out_error = std::move(message);
        }
        return false;
    };
    using detail::to_string;

    // Walks one list and checks prev/next symmetry, parent back-links, count and tail.
    usize linked = 0;
    auto  check_list = [&](Entity parent, Entity first, Entity last, u32 count) -> bool {
        Entity prev = kNullEntity;
        u32    n    = 0;
        for (Entity cur = first; cur != kNullEntity;) {
            if (!reg.valid(cur) || hs == nullptr || !hs->contains(cur)) {
                return fail(std::format("list of {} references dead/unlinked entity {}",
                                        to_string(parent), to_string(cur)));
            }
            const HierarchyComponent& h = hs->get(cur);
            if (h.parent != parent) {
                return fail(std::format("entity {} is in the list of {} but its parent is {}",
                                        to_string(cur), to_string(parent), to_string(h.parent)));
            }
            if (h.prev_sibling != prev) {
                return fail(std::format("entity {}: prev_sibling {} but previous node is {}",
                                        to_string(cur), to_string(h.prev_sibling),
                                        to_string(prev)));
            }
            if (++n > (hs->size() + 1)) {
                return fail(std::format("sibling list of {} is cyclic", to_string(parent)));
            }
            prev = cur;
            cur  = h.next_sibling;
        }
        if (n != count) {
            return fail(std::format("list of {} has {} entries but count says {}",
                                    to_string(parent), n, count));
        }
        if (prev != last) {
            return fail(std::format("list of {} ends at {} but tail says {}", to_string(parent),
                                    to_string(prev), to_string(last)));
        }
        linked += n;
        return true;
    };

    if (!check_list(kNullEntity, ctx.first_root, ctx.last_root, ctx.root_count)) {
        return false;
    }
    if (hs == nullptr) {
        return ctx.root_count == 0 ? true : fail("roots exist but no hierarchy storage");
    }
    for (auto [e, h] : hs->each()) {
        if (h.parent != kNullEntity && (!reg.valid(h.parent) || !hs->contains(h.parent))) {
            return fail(std::format("entity {} has dead/unlinked parent {}", to_string(e),
                                    to_string(h.parent)));
        }
        const Entity last = last_child_of(world, e);
        if (h.first_child == kNullEntity && h.child_count != 0) {
            return fail(std::format("entity {} has no children but child_count {}", to_string(e),
                                    h.child_count));
        }
        if (!check_list(e, h.first_child, last, h.child_count)) {
            return false;
        }
    }
    if (linked != hs->size()) {
        return fail(std::format("{} hierarchy components but {} linked entries", hs->size(),
                                linked));
    }
    // Acyclicity: a pre-order walk from the roots must reach every linked entity.
    usize reached = 0;
    for_each_in_hierarchy(world, [&](Entity) {
        ++reached;
        return reached > hs->size() ? Visit::Stop : Visit::Continue;
    });
    if (reached != hs->size()) {
        return fail(std::format("only {} of {} entities are reachable from the roots (cycle)",
                                reached, hs->size()));
    }
    return true;
}

} // namespace aether::scene
