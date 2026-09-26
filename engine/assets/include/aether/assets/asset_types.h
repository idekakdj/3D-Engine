// aether/assets/asset_types.h — runtime asset DATA types (CPU-side, import/cook output).
//
// FROZEN CONTRACT (ADR-0001/0002). `assets` owns data; consumers own runtime objects:
//   - renderer: receives MeshData/TextureData via the gameplay bridge (as MeshUpload etc.)
//   - animation: builds runtime Skeleton / clips from SkeletonData / AnimationClipData
// The assets agent implements importers, the cooked binary format, and AssetManager
// around these types (in other headers). Do not add GPU or ECS types here.
#pragma once

#include "aether/core/geometry.h"
#include "aether/core/handle.h"
#include "aether/core/math.h"
#include "aether/core/types.h"

#include <string>
#include <vector>

namespace aether::assets {

enum class AssetType : u8 {
    Unknown = 0,
    Mesh,
    Texture,
    Material,
    Skeleton,
    AnimationClip,
    Scene,
    Shader,
    Script,
};

// ---------------------------------------------------------------------------
// Mesh
// ---------------------------------------------------------------------------
struct MeshData {
    std::vector<Vertex>     vertices;
    std::vector<SkinVertex> skin;      // empty unless skinned; else skin.size()==vertices.size()
    std::vector<u32>        indices;   // triangle list
    std::vector<Submesh>    submeshes; // >= 1
    AABB                    bounds{};
    AssetId                 skeleton;  // valid iff skinned
};

// ---------------------------------------------------------------------------
// Texture
// ---------------------------------------------------------------------------
enum class TextureFormat : u8 {
    RGBA8_UNORM = 0, // linear data (normal, metallic-roughness, occlusion maps)
    RGBA8_SRGB,      // color data (base color, emissive)
    RGBA16F,
    RGBA32F,         // HDR environment maps (equirect)
};

struct TextureData {
    u32           width = 0;
    u32           height = 0;
    u32           mip_levels = 1;   // levels present in `pixels` (1 = renderer may generate)
    u32           array_layers = 1; // 6 for cubemaps
    bool          is_cubemap = false;
    TextureFormat format = TextureFormat::RGBA8_SRGB;
    std::vector<u8> pixels;         // tightly packed; layer-major within each mip, mip 0 first
};

// ---------------------------------------------------------------------------
// Material (glTF metallic-roughness)
// ---------------------------------------------------------------------------
enum class AlphaMode : u8 { Opaque = 0, Mask, Blend };

struct MaterialData {
    std::string name;
    Vec4        base_color_factor{ 1.0f };
    Vec3        emissive_factor{ 0.0f };
    f32         metallic_factor = 1.0f;
    f32         roughness_factor = 1.0f;
    f32         normal_scale = 1.0f;
    f32         occlusion_strength = 1.0f;
    f32         alpha_cutoff = 0.5f;
    AlphaMode   alpha_mode = AlphaMode::Opaque;
    bool        double_sided = false;
    AssetId     base_color_texture;         // RGBA8_SRGB
    AssetId     metallic_roughness_texture; // RGBA8_UNORM (G=roughness, B=metallic)
    AssetId     normal_texture;             // RGBA8_UNORM
    AssetId     occlusion_texture;          // RGBA8_UNORM (R)
    AssetId     emissive_texture;           // RGBA8_SRGB
};

// ---------------------------------------------------------------------------
// Skeleton + animation
// ---------------------------------------------------------------------------
struct SkeletonData {
    std::vector<std::string> joint_names;
    std::vector<i32>         parents;      // -1 for roots; topologically sorted: parents[i] < i
    std::vector<Transform>   bind_local;   // local-space bind pose per joint
    std::vector<Mat4>        inverse_bind; // model-space inverse bind matrices per joint
};

enum class Interpolation : u8 { Step = 0, Linear, CubicSpline };
enum class AnimPath : u8 { Translation = 0, Rotation, Scale };

struct AnimationChannel {
    u32               joint = 0; // index into SkeletonData joints
    AnimPath          path = AnimPath::Translation;
    Interpolation     interpolation = Interpolation::Linear;
    std::vector<f32>  times;  // seconds, ascending
    // Translation/Scale: xyz (w unused). Rotation: quaternion stored (x,y,z,w) as in glTF.
    // CubicSpline: 3 entries per key (in-tangent, value, out-tangent), as in glTF.
    std::vector<Vec4> values;
};

struct AnimationClipData {
    std::string                   name;
    f32                           duration = 0.0f; // seconds
    AssetId                       skeleton;
    std::vector<AnimationChannel> channels;
};

// ---------------------------------------------------------------------------
// Scene / prefab (node hierarchy referencing other assets)
// ---------------------------------------------------------------------------
struct SceneNodeData {
    std::string          name;
    i32                  parent = -1; // index into SceneData::nodes; parents precede children
    Transform            local{};
    AssetId              mesh;        // optional
    std::vector<AssetId> materials;   // one per submesh material_slot
    AssetId              skeleton;    // optional (skinned mesh)
};

struct SceneData {
    std::string                name;
    std::vector<SceneNodeData> nodes;
};

} // namespace aether::assets
