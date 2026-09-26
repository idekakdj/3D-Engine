// aether/scene/components.h — the core ECS components.
// FROZEN CONTRACT (ADR-0001). Scene stores stable AssetIds, never renderer/asset
// handles (removes scene->assets/renderer sideways deps). Reflection annotations are
// no-ops today (blueprint §5.4) but the syntax is final.
#pragma once

#include "aether/core/handle.h"
#include "aether/core/math.h"
#include "aether/core/reflect.h"
#include "aether/core/types.h"
#include "aether/scene/entity.h"

#include <string>

namespace aether {

struct NameComponent {
    std::string name = "Entity";
    AE_REFLECT(NameComponent, AE_FIELD(name))
};

struct TagComponent {
    u32 tag = 0;
};

// Local TRS + cached world matrix (recomputed by World::update_transforms()).
struct TransformComponent {
    Transform local{};
    Mat4      world{ 1.0f };
    bool      dirty = true;

    AE_REFLECT(TransformComponent, AE_FIELD(local))
};

// Intrusive hierarchy links (parent/child/sibling). kNullEntity terminates.
struct HierarchyComponent {
    Entity parent = kNullEntity;
    Entity first_child = kNullEntity;
    Entity next_sibling = kNullEntity;
    Entity prev_sibling = kNullEntity;
    u32    child_count = 0;
};

struct VisibilityComponent {
    bool visible = true;
    bool visible_in_hierarchy = true; // computed
};

// Drawable. Resolved to renderer handles by the asset->render bridge in gameplay.
struct MeshRendererComponent {
    AssetId mesh;
    AssetId material;
    bool    cast_shadows = true;
    AABB    local_bounds{};

    AE_REFLECT(MeshRendererComponent,
        AE_FIELD(mesh), AE_FIELD(material), AE_FIELD(cast_shadows))
};

enum class LightKind : u8 { Directional = 0, Point, Spot };

struct LightComponent {
    LightKind kind = LightKind::Point;
    Vec3      color{ 1.0f };
    f32       intensity = 1.0f;
    f32       range = 10.0f;
    f32       inner_cone_deg = 30.0f;
    f32       outer_cone_deg = 35.0f;
    bool      cast_shadows = false;

    AE_REFLECT(LightComponent,
        AE_FIELD(color), AE_FIELD(intensity), AE_FIELD(range), AE_FIELD(cast_shadows))
};

struct CameraComponent {
    f32  fov_y_deg = 60.0f;
    f32  near_z = 0.1f;
    f32  far_z  = 1000.0f;
    bool primary = true;

    AE_REFLECT(CameraComponent,
        AE_FIELD(fov_y_deg), AE_FIELD(near_z), AE_FIELD(far_z), AE_FIELD(primary))
};

} // namespace aether
