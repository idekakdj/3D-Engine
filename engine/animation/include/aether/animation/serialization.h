// aether/animation/serialization.h — scene-serializer codec for AnimatorComponent.
//
// Registers a ComponentCodec named "Animator" on a World so scene save/load, copy/paste and
// play-in-editor snapshots keep an entity's animator SETTINGS:
//     { "skeleton": "<32-hex AssetId>", "graph": "<32-hex AssetId>", "playback_rate": 1.0,
//       "paused": false, "enabled": true,
//       "root_motion": { "mode": "ignore|extract|apply", "joint": "<joint name>",
//                        "horizontal": true, "vertical": false, "yaw": true } }
// Runtime objects (Skeleton / AnimGraph shared_ptrs, IK chains, playback state) are NOT
// serialized: after loading, the gameplay asset bridge resolves skeleton_asset / graph_asset and
// calls set_skeleton / set_graph. The root-motion joint is stored by name and re-resolved against
// the bound skeleton when available (else kept by index as "joint_index").
//
// Thread-safety: main thread only (like the serializer).
#pragma once

#include "aether/core/types.h"

namespace aether {
class World;
}

namespace aether::animation {

inline constexpr const char* kAnimatorCodecName = "Animator";

// Call once per World (e.g. at AnimationSubsystem / game startup). Returns false on failure.
bool register_animation_codecs(World& world);

} // namespace aether::animation
