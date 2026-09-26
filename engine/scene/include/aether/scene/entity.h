// aether/scene/entity.h — Entity handle (EnTT-backed).
//
// FROZEN CONTRACT (ADR-0001 amendment): EnTT is permitted in scene's PUBLIC headers
// (analogous to glm in core) to keep the ECS API ergonomic. No other module exposes
// EnTT types in its public surface.
#pragma once

#include <entt/entt.hpp>

namespace aether {

using Entity = entt::entity;
inline constexpr auto kNullEntity = entt::null;

} // namespace aether
