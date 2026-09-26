// shape_builder.cpp — see shape_builder.h.
#include "shape_builder.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace aether::physics::detail {
namespace {

constexpr f32 kMinExtent = 1.0e-3f; // 1 mm: smaller primitives are clamped (Jolt rejects them)

[[nodiscard]] f32 convex_radius_for(f32 smallest_extent) {
    return std::min(JPH::cDefaultConvexRadius, 0.5f * smallest_extent);
}

[[nodiscard]] BuiltShape fail(String message) {
    BuiltShape out;
    out.error = std::move(message);
    return out;
}

[[nodiscard]] BuiltShape build_base(const ColliderComponent& c) {
    BuiltShape out;
    JPH::ShapeSettings::ShapeResult result;
    switch (c.shape) {
    case ColliderShape::Box: {
        const Vec3 he = glm::max(c.half_extents, Vec3(kMinExtent));
        const f32  smallest = std::min({ he.x, he.y, he.z });
        result = JPH::BoxShapeSettings(to_jolt(he), convex_radius_for(smallest)).Create();
        break;
    }
    case ColliderShape::Sphere:
        result = JPH::SphereShapeSettings(std::max(c.radius, kMinExtent)).Create();
        break;
    case ColliderShape::Capsule: {
        const f32 r = std::max(c.radius, kMinExtent);
        if (c.half_height <= kMinExtent)
            result = JPH::SphereShapeSettings(r).Create(); // degenerate capsule == sphere
        else
            result = JPH::CapsuleShapeSettings(c.half_height, r).Create();
        break;
    }
    case ColliderShape::Cylinder: {
        const f32 r = std::max(c.radius, kMinExtent);
        const f32 hh = std::max(c.half_height, kMinExtent);
        result = JPH::CylinderShapeSettings(hh, r, convex_radius_for(std::min(r, hh))).Create();
        break;
    }
    case ColliderShape::ConvexHull: {
        if (c.points.size() < 4)
            return fail(std::format("convex hull needs >= 4 points (got {})", c.points.size()));
        JPH::Array<JPH::Vec3> points;
        points.reserve(c.points.size());
        for (const Vec3& p : c.points)
            points.push_back(to_jolt(p));
        result = JPH::ConvexHullShapeSettings(points, JPH::cDefaultConvexRadius).Create();
        break;
    }
    case ColliderShape::TriangleMesh: {
        if (c.indices.empty() || c.indices.size() % 3 != 0)
            return fail(std::format("triangle mesh index count {} is not a non-zero multiple of 3",
                                    c.indices.size()));
        JPH::VertexList vertices;
        vertices.reserve(c.vertices.size());
        for (const Vec3& v : c.vertices)
            vertices.push_back(JPH::Float3(v.x, v.y, v.z));
        JPH::IndexedTriangleList triangles;
        triangles.reserve(c.indices.size() / 3);
        const usize vertex_count = c.vertices.size();
        for (usize i = 0; i < c.indices.size(); i += 3) {
            const u32 a = c.indices[i], b = c.indices[i + 1], d = c.indices[i + 2];
            if (a >= vertex_count || b >= vertex_count || d >= vertex_count)
                return fail(std::format("triangle mesh index out of range at triangle {}", i / 3));
            triangles.push_back(JPH::IndexedTriangle(a, b, d, 0));
        }
        result = JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
        break;
    }
    }

    if (result.HasError())
        return fail(String(result.GetError().c_str()));
    out.shape = result.Get();
    out.base = out.shape;
    return out;
}

} // namespace

bool needs_scaling(const Vec3& scale) {
    const Vec3 d = glm::abs(scale - Vec3(1.0f));
    return d.x > 1.0e-5f || d.y > 1.0e-5f || d.z > 1.0e-5f;
}

BuiltShape build_collider_shape(const ColliderComponent& collider, const Vec3& scale) {
    BuiltShape out = build_base(collider);
    if (!out.error.empty())
        return out;

    // Local placement relative to the entity origin.
    const Quat r = collider.local_rotation;
    const bool has_rotation = std::abs(r.w) < 1.0f - 1.0e-6f;
    const bool has_offset = glm::dot(collider.local_offset, collider.local_offset) > 1.0e-12f;
    if (has_rotation || has_offset) {
        const Quat rn = glm::normalize(r);
        auto rt = JPH::RotatedTranslatedShapeSettings(to_jolt(collider.local_offset), to_jolt(rn),
                                                      out.shape)
                      .Create();
        if (rt.HasError())
            return fail(String(rt.GetError().c_str()));
        out.shape = rt.Get();
    }

    // Entity world scale (applies to the local offset too, like the render mesh).
    if (needs_scaling(scale)) {
        if (std::abs(scale.x) < kMinExtent || std::abs(scale.y) < kMinExtent ||
            std::abs(scale.z) < kMinExtent)
            return fail(std::format("degenerate world scale ({}, {}, {})", scale.x, scale.y, scale.z));
        auto scaled = out.shape->ScaleShape(to_jolt(scale));
        if (scaled.HasError())
            return fail(String(scaled.GetError().c_str()));
        out.shape = scaled.Get();
    }
    return out;
}

JPH::RefConst<JPH::Shape> build_character_shape(f32 half_height, f32 radius) {
    const f32 r = std::max(radius, kMinExtent);
    const f32 hh = std::max(half_height, 0.0f);
    JPH::RefConst<JPH::Shape> capsule;
    if (hh <= kMinExtent)
        capsule = JPH::SphereShapeSettings(r).Create().Get();
    else
        capsule = JPH::CapsuleShapeSettings(hh, r).Create().Get();
    // Lift the capsule so the character origin sits at its feet.
    return JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0f, hh + r, 0.0f), JPH::Quat::sIdentity(),
                                               capsule)
        .Create()
        .Get();
}

} // namespace aether::physics::detail
