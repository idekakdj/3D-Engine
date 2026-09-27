// meshlets.h — CPU meshlet building for the mesh-shader path (ADR-0010).
//
// Private header. build_meshlets() splits every submesh of a static mesh into meshlets of at
// most kMeshletMaxVertices / kMeshletMaxTriangles (meshoptimizer, with cone-aware clustering),
// optimises each for locality and computes its bounding sphere and normal cone. The result is
// exactly the GPU layout: GpuMeshlet records plus one u32 word stream holding each meshlet's
// vertex indices (mesh-local) followed by its triangles packed i0 | i1 << 8 | i2 << 16.
// Triangle winding is preserved. Pure CPU; any thread.
#pragma once

#include "gpu_data.h"

#include "aether/core/geometry.h"
#include "aether/core/types.h"

#include <vector>

namespace aether::renderer {

struct MeshletBuild {
    struct Range {
        u32 first = 0; // first GpuMeshlet of the submesh
        u32 count = 0;
    };
    std::vector<GpuMeshlet> meshlets;
    std::vector<u32>        words;
    std::vector<Range>      submeshes; // parallel to the input submeshes

    [[nodiscard]] u64 gpu_bytes() const noexcept {
        return meshlets.size() * sizeof(GpuMeshlet) + words.size() * sizeof(u32);
    }
};

// `indices` are mesh-local (0-based into `vertices`); submesh ranges index into `indices`.
[[nodiscard]] MeshletBuild build_meshlets(Span<const Vertex> vertices, Span<const u32> indices,
                                          Span<const Submesh> submeshes);

} // namespace aether::renderer
