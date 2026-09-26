// aether/scene/id.h — persistent entity identity (64-bit UUID) and UUID lookup.
//
// Every entity created through World::create()/create_child() carries an IdComponent whose
// `uuid` is a random, non-zero 64-bit value that is UNIQUE WITHIN ITS WORLD. Unlike the
// EnTT entity id (index + version, recycled at runtime), the uuid is stable across
// save/load and is what scene files, prefabs, undo/redo and cross-entity references use.
//
// Uniqueness is enforced by the World itself (registry signals keep a uuid -> entity map in
// the registry context):
//   * emplacing an IdComponent with uuid == 0 assigns a fresh random uuid;
//   * emplacing/replacing with a uuid already owned by another live entity logs a warning
//     and assigns a fresh uuid instead.
// Treat `IdComponent::uuid` as READ-ONLY: writing the field directly bypasses the lookup map
// (find_by_uuid() would then miss the entity). Change a uuid with set_uuid().
//
// The component lives in `aether` next to the frozen core components (it is an ECS component
// like NameComponent); the functions live in `aether::scene`.
#pragma once

#include "aether/core/types.h"
#include "aether/scene/entity.h"

namespace aether {

class World;

// Persistent identity of an entity. 0 is never a valid uuid.
struct IdComponent {
    u64 uuid = 0;
};

} // namespace aether

namespace aether::scene {

using aether::IdComponent;

inline constexpr u64 kInvalidUuid = 0;

// Entity owning `uuid`, or kNullEntity if none. O(1) (hash lookup).
// Thread-safe for concurrent readers while no thread mutates the world.
[[nodiscard]] Entity find_by_uuid(const World& world, u64 uuid);

// The entity's uuid, or kInvalidUuid if it is invalid or has no IdComponent.
// Thread-safe for concurrent readers while no thread mutates the world.
[[nodiscard]] u64 uuid_of(const World& world, Entity e);

// Re-keys `e` (adding an IdComponent if missing). Fails (returns false, logs) when `uuid` is
// 0 or already owned by another live entity. O(1). Main thread only.
bool set_uuid(World& world, Entity e, u64 uuid);

// A fresh random uuid that no entity of `world` currently owns. Main thread only.
[[nodiscard]] u64 generate_uuid(World& world);

// Canonical text form: exactly 16 lowercase hex digits (JSON numbers cannot hold 64 bits
// losslessly in most tools, so scene files store uuids as strings). Thread-safe.
[[nodiscard]] String uuid_to_string(u64 uuid);
// Parses 1..16 hex digits (either case). Returns false on malformed input. Thread-safe.
[[nodiscard]] bool uuid_from_string(StringView text, u64& out_uuid);

} // namespace aether::scene
