// meshlets.cpp — see meshlets.h.
#include "meshlets.h"

#include <meshoptimizer.h>

namespace aether::renderer {

namespace {
constexpr float kConeWeight = 0.25f; // favour tighter normal cones (more backface culling)
} // namespace

MeshletBuild build_meshlets(Span<const Vertex> vertices, Span<const u32> indices, Span<const Submesh> submeshes) {
    MeshletBuild out;
    out.submeshes.resize(submeshes.size());
    const float* positions = &vertices[0].position.x;

    std::vector<meshopt_Meshlet> ml;
    std::vector<unsigned int>    ml_vertices;
    std::vector<unsigned char>   ml_triangles;
    for (usize s = 0; s < submeshes.size(); ++s) {
        const Submesh& sm = submeshes[s];
        out.submeshes[s].first = static_cast<u32>(out.meshlets.size());
        if (sm.index_count < 3 || static_cast<u64>(sm.first_index) + sm.index_count > indices.size()) {
            continue;
        }
        const u32* idx = indices.data() + sm.first_index;
        const usize max_meshlets = meshopt_buildMeshletsBound(sm.index_count, kMeshletMaxVertices, kMeshletMaxTriangles);
        ml.resize(max_meshlets);
        ml_vertices.resize(max_meshlets * kMeshletMaxVertices);
        ml_triangles.resize(max_meshlets * kMeshletMaxTriangles * 3);
        const usize count = meshopt_buildMeshlets(ml.data(), ml_vertices.data(), ml_triangles.data(), idx,
                                                  sm.index_count, positions, vertices.size(), sizeof(Vertex),
                                                  kMeshletMaxVertices, kMeshletMaxTriangles, kConeWeight);
        for (usize i = 0; i < count; ++i) {
            const meshopt_Meshlet& m = ml[i];
            unsigned int*  mv = &ml_vertices[m.vertex_offset];
            unsigned char* mt = &ml_triangles[m.triangle_offset];
            meshopt_optimizeMeshlet(mv, mt, m.triangle_count, m.vertex_count);
            const meshopt_Bounds b =
                meshopt_computeMeshletBounds(mv, mt, m.triangle_count, positions, vertices.size(), sizeof(Vertex));

            GpuMeshlet g;
            g.center = Vec3(b.center[0], b.center[1], b.center[2]);
            g.radius = b.radius;
            g.cone_apex = Vec3(b.cone_apex[0], b.cone_apex[1], b.cone_apex[2]);
            g.cone_axis = Vec3(b.cone_axis[0], b.cone_axis[1], b.cone_axis[2]);
            g.cone_cutoff = b.cone_cutoff;
            g.vertex_count = m.vertex_count;
            g.triangle_count = m.triangle_count;
            g.vertex_offset = static_cast<u32>(out.words.size());
            out.words.insert(out.words.end(), mv, mv + m.vertex_count);
            g.triangle_offset = static_cast<u32>(out.words.size());
            for (u32 t = 0; t < m.triangle_count; ++t) {
                out.words.push_back(u32(mt[t * 3]) | (u32(mt[t * 3 + 1]) << 8) | (u32(mt[t * 3 + 2]) << 16));
            }
            out.meshlets.push_back(g);
        }
        out.submeshes[s].count = static_cast<u32>(out.meshlets.size()) - out.submeshes[s].first;
    }
    return out;
}

} // namespace aether::renderer
