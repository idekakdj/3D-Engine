// id.cpp — per-world uuid generation, the uuid -> entity map (kept in sync through registry
// listeners) and the public IdComponent API.
#include "aether/core/log.h"
#include "aether/scene/id.h"
#include "scene_internal.h"

#include <chrono>
#include <random>

namespace aether::scene::detail {
namespace {

// splitmix64 finalizer: a bijection on u64, so consecutive counter values never collide.
u64 mix64(u64 z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Is `uuid` owned by a live entity other than `self`?
bool owned_by_other(const entt::registry& reg, const SceneContext& ctx, u64 uuid, Entity self) {
    const auto it = ctx.uuid_to_entity.find(uuid);
    if (it == ctx.uuid_to_entity.end() || it->second == self || !reg.valid(it->second)) {
        return false;
    }
    const IdComponent* id = reg.try_get<IdComponent>(it->second);
    return id != nullptr && id->uuid == uuid;
}

// Validates/assigns e's uuid and registers it. Shared by construct and update listeners.
void register_id(entt::registry& reg, Entity e) {
    SceneContext& ctx = context(reg);
    IdComponent&  id  = reg.get<IdComponent>(e);
    if (id.uuid == kInvalidUuid) {
        id.uuid = next_free_uuid(ctx);
    } else if (owned_by_other(reg, ctx, id.uuid, e)) {
        const u64 fresh = next_free_uuid(ctx);
        AE_LOG_WARN(kLogCategory,
                    "uuid {} requested by entity {} is already owned by entity {}; assigned {}",
                    uuid_to_string(id.uuid), to_string(e),
                    to_string(ctx.uuid_to_entity.at(id.uuid)), uuid_to_string(fresh));
        id.uuid = fresh;
    }
    ctx.uuid_to_entity[id.uuid] = e;
}

} // namespace

void seed_uuid_generator(SceneContext& ctx) {
    std::random_device rd;
    const u64 entropy = (static_cast<u64>(rd()) << 32) ^ static_cast<u64>(rd());
    const u64 clock   = static_cast<u64>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    ctx.uuid_state = mix64(entropy ^ mix64(clock) ^ reinterpret_cast<std::uintptr_t>(&ctx));
}

u64 next_free_uuid(SceneContext& ctx) {
    for (;;) {
        ctx.uuid_state += 0x9E3779B97F4A7C15ull;
        const u64 candidate = mix64(ctx.uuid_state);
        if (candidate != kInvalidUuid && !ctx.uuid_to_entity.contains(candidate)) {
            return candidate;
        }
    }
}

void on_id_construct(entt::registry& reg, Entity e) {
    register_id(reg, e);
}

void on_id_update(entt::registry& reg, Entity e) {
    // The previous key (unknown here) is left behind; lookups validate the entity's current
    // uuid, so it can never resolve incorrectly. set_uuid() re-keys without leaving one.
    register_id(reg, e);
}

void on_id_destroy(entt::registry& reg, Entity e) {
    SceneContext& ctx = context(reg);
    if (ctx.bulk_clear) {
        return;
    }
    const u64  uuid = reg.get<IdComponent>(e).uuid;
    const auto it   = ctx.uuid_to_entity.find(uuid);
    if (it != ctx.uuid_to_entity.end() && it->second == e) {
        ctx.uuid_to_entity.erase(it);
    }
}

} // namespace aether::scene::detail

namespace aether::scene {

Entity find_by_uuid(const World& world, u64 uuid) {
    if (uuid == kInvalidUuid) {
        return kNullEntity;
    }
    const entt::registry&       reg = world.registry();
    const detail::SceneContext& ctx = detail::context(reg);
    const auto                  it  = ctx.uuid_to_entity.find(uuid);
    if (it == ctx.uuid_to_entity.end() || !reg.valid(it->second)) {
        return kNullEntity;
    }
    const IdComponent* id = reg.try_get<IdComponent>(it->second);
    return (id != nullptr && id->uuid == uuid) ? it->second : Entity{ kNullEntity };
}

u64 uuid_of(const World& world, Entity e) {
    const IdComponent* id = world.try_get<IdComponent>(e);
    return id != nullptr ? id->uuid : kInvalidUuid;
}

bool set_uuid(World& world, Entity e, u64 uuid) {
    entt::registry& reg = world.registry();
    if (!reg.valid(e)) {
        AE_LOG_WARN(detail::kLogCategory, "set_uuid: invalid entity {}", detail::to_string(e));
        return false;
    }
    if (uuid == kInvalidUuid) {
        AE_LOG_WARN(detail::kLogCategory, "set_uuid: 0 is not a valid uuid");
        return false;
    }
    const Entity owner = find_by_uuid(world, uuid);
    if (owner == e) {
        return true;
    }
    if (owner != kNullEntity) {
        AE_LOG_WARN(detail::kLogCategory, "set_uuid: uuid {} is already owned by entity {}",
                    uuid_to_string(uuid), detail::to_string(owner));
        return false;
    }
    if (!reg.all_of<IdComponent>(e)) {
        reg.emplace<IdComponent>(e, IdComponent{ uuid });
        return true;
    }
    detail::SceneContext& ctx = detail::context(reg);
    IdComponent&          id  = reg.get<IdComponent>(e);
    const auto            old = ctx.uuid_to_entity.find(id.uuid);
    if (old != ctx.uuid_to_entity.end() && old->second == e) {
        ctx.uuid_to_entity.erase(old);
    }
    id.uuid                   = uuid; // direct write: no on_update, map maintained here
    ctx.uuid_to_entity[uuid] = e;
    return true;
}

u64 generate_uuid(World& world) {
    return detail::next_free_uuid(detail::context(world.registry()));
}

String uuid_to_string(u64 uuid) {
    constexpr char kHex[] = "0123456789abcdef";
    String         out(16, '0');
    for (usize i = 0; i < 16; ++i) {
        out[15 - i] = kHex[(uuid >> (i * 4)) & 0xF];
    }
    return out;
}

bool uuid_from_string(StringView text, u64& out_uuid) {
    if (text.empty() || text.size() > 16) {
        return false;
    }
    u64 value = 0;
    for (const char c : text) {
        u64 digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<u64>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<u64>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<u64>(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    out_uuid = value;
    return true;
}

} // namespace aether::scene
