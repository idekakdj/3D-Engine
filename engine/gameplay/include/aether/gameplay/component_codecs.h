// aether/gameplay/component_codecs.h — scene-serializer codecs for components that have no
// serialization of their own.
//
// register_gameplay_codecs() registers (as scene::ComponentCodec, see scene_serializer.h):
//   "RigidBody", "Collider", "CharacterController", "FixedConstraint", "PointConstraint",
//   "HingeConstraint", "DistanceConstraint"          (physics components; physics does not
//                                                      depend on the serializer's JSON)
//   "FlyCamera", "OrbitCamera", "MaterialOverrides"   (gameplay components)
// Entity references (constraint targets, orbit follow) are stored as uuids ("" = none).
// Only authoring data is saved: runtime outputs (CharacterController velocity / ground state,
// camera controller yaw/pitch state) are rebuilt.
//
// register_default_codecs() additionally registers the Layer-4a module codecs (Animator,
// Script) when those modules are part of the build. The Application calls it for its world.
//
// Main thread only. Idempotent.
#pragma once

namespace aether {
class World;
}

namespace aether::gameplay {

bool register_gameplay_codecs(World& world);
bool register_default_codecs(World& world);

} // namespace aether::gameplay
