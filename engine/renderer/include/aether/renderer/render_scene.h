// aether/renderer/render_scene.h — the immutable-per-frame render input DTO.
//
// FROZEN CONTRACT (ADR-0001 / blueprint §7.5). The renderer consumes a RenderScene
// snapshot each frame and NEVER reaches into the ECS. Gameplay/editor PRODUCE it;
// physics appends debug lines; animation writes skinning palettes. Handles here are
// RENDERER-OWNED (register_* on the Renderer), decoupling the renderer from assets.
#pragma once

#include "aether/core/handle.h"
#include "aether/core/math.h"
#include "aether/core/types.h"

#include <vector>

namespace aether::renderer {

// Renderer-owned resource handles (phantom-tagged; see Renderer::register_*).
struct MeshTag; struct MaterialTag; struct TextureTag; struct EnvTag;
using MeshHandle     = Handle<MeshTag>;
using MaterialHandle = Handle<MaterialTag>;
using TextureHandle  = Handle<TextureTag>;
using EnvHandle      = Handle<EnvTag>;

// Camera / view parameters for the frame.
struct RenderView {
    Mat4  view{ 1.0f };
    Mat4  proj{ 1.0f };
    Mat4  view_proj{ 1.0f };
    Vec3  camera_position{ 0.0f };
    f32   near_z = 0.1f;
    f32   far_z  = 1000.0f;
    UVec2 viewport{ 1280, 720 };
    f32   exposure = 1.0f;
};

enum class LightType : u8 { Directional = 0, Point, Spot };

struct RenderLight {
    LightType type = LightType::Point;
    Vec3      position{ 0.0f };
    Vec3      direction{ 0.0f, -1.0f, 0.0f };
    Vec3      color{ 1.0f };
    f32       intensity = 1.0f;
    f32       range = 10.0f;          // point/spot
    f32       inner_cone = 0.9f;      // spot cos(angle)
    f32       outer_cone = 0.8f;      // spot cos(angle)
    bool      cast_shadows = false;
};

// One drawable instance. `first_joint`/`joint_count` index into `joint_matrices`
// when the mesh is skinned (animation fills the palette; renderer uploads it).
struct RenderMeshInstance {
    MeshHandle     mesh;
    MaterialHandle material;
    u32            submesh = 0;
    Mat4           transform{ 1.0f };
    AABB           world_bounds{};
    u32            flags = 0;         // bit 0 = casts shadow, bit 1 = skinned, ...
    u32            first_joint = kInvalidU32;
    u32            joint_count = 0;
};

// Debug line (1px on Intel Arc — no wideLines; see ADR-0001). Physics/anim emit these.
struct RenderLine {
    Vec3 a{ 0.0f };
    Vec3 b{ 0.0f };
    u32  color_a = 0xFFFFFFFFu; // packed RGBA8
    u32  color_b = 0xFFFFFFFFu;
};

// Environment / image-based lighting for the frame.
struct EnvironmentSettings {
    EnvHandle skybox;                 // prefiltered environment (invalid => flat ambient)
    Vec3      ambient_color{ 0.03f };
    f32       ambient_intensity = 1.0f;
    f32       skybox_lod = 0.0f;
};

// The frame's complete render input. Rebuilt (or double-buffered) each frame.
struct RenderScene {
    RenderView                      view{};
    EnvironmentSettings             environment{};
    std::vector<RenderMeshInstance> instances;
    std::vector<RenderLight>        lights;
    std::vector<RenderLine>         debug_lines;
    std::vector<Mat4>               joint_matrices; // global skinning palette pool

    void clear() {
        instances.clear();
        lights.clear();
        debug_lines.clear();
        joint_matrices.clear();
    }
};

} // namespace aether::renderer
