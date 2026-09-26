// aether/renderer/instance_flags.h — bit meanings of RenderMeshInstance::flags.
//
// Additive companion to render_scene.h (which reserves bit 0 = casts shadow and bit 1 =
// skinned). Every bit the renderer interprets is listed here; unlisted bits in the
// renderer range are reserved and must be zero. The mirrored GPU value lives in the
// per-instance record (GpuInstance::flags) so shaders see the same bits.
//
// Skinning convention (bit 1): RenderScene::joint_matrices holds SKINNING matrices, i.e.
// joint_model_space * inverse_bind (glTF / assets::SkeletonData::inverse_bind), expressed
// in the mesh's model space. The instance palette is
//     joint_matrices[first_joint .. first_joint + joint_count)
// indexed by SkinVertex::joints. The vertex shader applies the palette first and the
// instance transform afterwards:  world = transform * (sum_i w_i * palette[j_i]) * pos.
// The mesh must have been registered with a SkinVertex stream (MeshUpload::skin).
// Header-only; thread-safe (constants only).
#pragma once

#include "aether/core/types.h"

namespace aether::renderer::instance_flags {

// Bit 0: the instance is rendered into the directional light's cascaded shadow maps.
inline constexpr u32 kCastShadow = 1u << 0;

// Bit 1: GPU-skinned. Requires a mesh registered with skin data and a valid
// first_joint/joint_count palette range inside RenderScene::joint_matrices. If either is
// missing the instance is silently drawn unskinned (bind pose).
inline constexpr u32 kSkinned = 1u << 1;

// Bit 2: shadow-only proxy - never drawn in the main view (prepass, forward, debug views)
// but still casts shadows when kCastShadow is also set.
inline constexpr u32 kHiddenInMainView = 1u << 2;

// Bit 3: skip CPU frustum culling for this instance (always considered visible). Useful
// while world_bounds are not yet known, e.g. freshly spawned skinned characters.
inline constexpr u32 kNeverCull = 1u << 3;

// Bits 4..15 are reserved for future renderer use and must be zero.
inline constexpr u32 kRendererReservedMask = 0x0000'FFF0u;

// Bits 16..31 are free for callers (editor selection ids, gameplay tags, ...). The
// renderer ignores them and never forwards them to shaders.
inline constexpr u32 kUserMask = 0xFFFF'0000u;

// All bits the renderer understands.
inline constexpr u32 kKnownMask = kCastShadow | kSkinned | kHiddenInMainView | kNeverCull;

[[nodiscard]] constexpr bool has(u32 flags, u32 bit) noexcept { return (flags & bit) != 0; }

} // namespace aether::renderer::instance_flags
