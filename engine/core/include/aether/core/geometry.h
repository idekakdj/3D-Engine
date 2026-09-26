// aether/core/geometry.h — canonical vertex layouts shared by assets and renderer.
//
// FROZEN CONTRACT (ADR-0002). Lives in core so the assets importer (Layer 3) and the
// renderer (Layer 3) agree on memory layout without depending on each other: the
// gameplay bridge hands assets::MeshData spans straight to renderer::MeshUpload with
// zero conversion. Layouts are GPU-facing - do not reorder fields.
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"

namespace aether {

// Static vertex, 48 bytes, tightly packed (glm default alignment = 4).
struct Vertex {
    Vec3 position{ 0.0f }; // offset  0
    Vec3 normal{ 0.0f, 1.0f, 0.0f }; // offset 12
    Vec4 tangent{ 1.0f, 0.0f, 0.0f, 1.0f }; // offset 24, w = bitangent sign (+1/-1)
    Vec2 uv0{ 0.0f };      // offset 40
};
static_assert(sizeof(Vertex) == 48, "Vertex layout is a GPU contract");

// Skinning stream, parallel to the Vertex stream (same count) for skinned meshes only.
struct SkinVertex {
    u16  joints[4]{ 0, 0, 0, 0 }; // offset 0, indices into the mesh's skeleton
    Vec4 weights{ 1.0f, 0.0f, 0.0f, 0.0f }; // offset 8, sum to 1
};
static_assert(sizeof(SkinVertex) == 24, "SkinVertex layout is a GPU contract");

// A contiguous index range drawn with one material slot.
struct Submesh {
    u32  first_index = 0;
    u32  index_count = 0;
    u32  material_slot = 0;
    AABB bounds{};
};

} // namespace aether
