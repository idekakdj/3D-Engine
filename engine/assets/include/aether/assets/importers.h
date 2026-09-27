// aether/assets/importers.h — source-asset importers (glTF 2.0, images).
//
// Importers turn DCC source files into the CPU data types of asset_types.h. They are
// pure functions of (file contents, ImportSettings): no global state, no GPU, no job
// system, so they are safe to run on any thread and in parallel (the cooker does).
//
// Sub-asset identity is deterministic (see asset_traits.h):
//     id = AssetId::from_string(canonical_source_path + "#" + key)
// glTF keys (indices are glTF array indices, stable for a given file):
//     "mesh:<mesh>"  "material:<material>"  "texture:<image>:srgb|linear|normal"
//     "skeleton:<skin>"  "anim:<animation>"  "scene:<scene>"
//     "node_skeleton:<scene>"  "node_anim:<animation>"   (node animation, see below)
// Standalone images use the key "texture:0".
//
// Texture de-duplication: a glTF image stored in an external file that lies inside
// ImportSettings::content_root and would be imported standalone with the SAME cooked variant
// (colour space + role, see standalone_image_role()) is not imported by the glTF: materials
// reference the standalone image's asset (<image path>#texture:0) and the file is listed in
// ImportResult::referenced_sources (AssetDatabase imports those too). Otherwise (embedded
// images, files outside the root, a different variant) the glTF owns a "texture:*" copy.
//
// Node animation (animations of nodes that are not skin joints): channels targeting skin
// joints form the skeletal clip "anim:<a>" exactly as before; channels targeting other nodes
// of the default scene form "node_anim:<a>", an AnimationClipData whose `skeleton` is the
// "node_skeleton:<s>" SkeletonData of that scene: joint i == SceneData::nodes[i] (same names,
// parents and bind-local transforms; inverse_bind = inverse scene-space bind matrix), so
// AnimationChannel::joint is a SceneData node index. A consumer samples the clip with that
// skeleton and writes the local transforms of animated joints to the scene's node entities.
// Both are emitted only when such channels exist (no change for joint-only files).
#pragma once

#include "aether/assets/asset_traits.h"
#include "aether/assets/asset_types.h"
#include "aether/assets/texture_processing.h"
#include "aether/core/error.h"
#include "aether/core/handle.h"
#include "aether/core/types.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace aether::assets {

// Bump whenever importer OUTPUT changes for identical input (forces re-cooks).
// v2 (ADR-0009): MikkTSpace tangents (vertex splits), cooked mips/BC textures, normal-map
// texture keys, node animation, texture de-duplication.
inline constexpr u32 kImporterVersion = 2;

enum class NormalGeneration : u8 {
    Smooth = 0, // position-welded, area-weighted (default)
    Flat,       // per-face normals (vertices are un-welded); glTF-spec behaviour
};

enum class ImageColorSpace : u8 {
    Auto = 0, // standalone images: linear if the file name looks like data (normal, roughness,
              // metallic, orm, ao, mask, height, ...), else sRGB. glTF images: from usage.
    Srgb,
    Linear,
};

struct ImportSettings {
    // Root that source paths (and therefore AssetIds) are made relative to. Empty => the
    // source file's own directory (ids then depend only on the file name).
    std::filesystem::path content_root;

    bool             generate_normals = true;  // when a primitive has no NORMAL
    NormalGeneration normal_generation = NormalGeneration::Smooth;
    bool             generate_tangents = true; // when a primitive has no TANGENT (needs UVs)
    bool             import_textures = true;   // decode images referenced by glTF materials
    ImageColorSpace  image_color_space = ImageColorSpace::Auto; // standalone images only

    // Texture cooking (texture_processing.h). The defaults are the fast EDITOR-mode import
    // (mip 0 only, RGBA8; the renderer generates mips at upload) for iteration speed; the
    // cooker (aether-cook) uses cooking(): full CPU mip chains + BC7/BC5 compression.
    bool      generate_mips = false;     // full chain: sRGB-correct, normals renormalised
    bool      compress_textures = false; // BC7_SRGB / BC7_UNORM / BC5_UNORM (implies mips)
    MipFilter mip_filter = MipFilter::Kaiser;

    // Hash of every setting that affects importer output (NOT content_root). Folded into
    // the asset database's source hash so changing settings triggers a re-import.
    [[nodiscard]] u64 fingerprint() const noexcept;

    // Settings for shipping cooks: generate_mips + compress_textures on, rest default.
    [[nodiscard]] static ImportSettings cooking() noexcept;
};

template <typename T>
struct ImportedAsset {
    AssetId id;
    String  key;  // sub-asset key within the source, e.g. "mesh:0"
    String  name; // human-readable (DCC name, or a generated "<Kind>_<index>")
    T       data;
};

struct ImportResult {
    String source_path; // canonical content-relative source path ('/'-separated, UTF-8)
    AssetId primary;    // the main asset: default scene (glTF) or the texture (image)

    std::vector<ImportedAsset<MeshData>>          meshes;
    std::vector<ImportedAsset<MaterialData>>      materials;
    std::vector<ImportedAsset<TextureData>>       textures;
    std::vector<ImportedAsset<SkeletonData>>      skeletons;
    std::vector<ImportedAsset<AnimationClipData>> animations;
    std::vector<ImportedAsset<SceneData>>         scenes;

    // External files read besides the source itself (glTF .bin buffers, image files).
    std::vector<std::filesystem::path> dependencies;
    // Other SOURCE files whose assets this result references instead of importing them
    // (de-duplicated standalone images). They are not dependencies: their own import owns them.
    std::vector<std::filesystem::path> referenced_sources;
    // Non-fatal problems (unsupported features skipped, data repaired, ...).
    std::vector<String> warnings;

    [[nodiscard]] usize asset_count() const noexcept {
        return meshes.size() + materials.size() + textures.size() + skeletons.size() +
               animations.size() + scenes.size();
    }

    // Calls f(const ImportedAsset<T>&) for every sub-asset, grouped by type in the order
    // textures, materials, skeletons, meshes, animations, scenes (dependencies first).
    template <typename F>
    void for_each_asset(F&& f) const {
        for (const auto& a : textures)   f(a);
        for (const auto& a : materials)  f(a);
        for (const auto& a : skeletons)  f(a);
        for (const auto& a : meshes)     f(a);
        for (const auto& a : animations) f(a);
        for (const auto& a : scenes)     f(a);
    }

    // Returns the sub-asset with `id` in the typed list, or nullptr.
    template <typename T>
    [[nodiscard]] const ImportedAsset<T>* find(AssetId id) const noexcept {
        const auto& list = list_for<T>();
        for (const auto& a : list) {
            if (a.id == id) return &a;
        }
        return nullptr;
    }

    template <typename T>
    [[nodiscard]] std::vector<ImportedAsset<T>>& list_for() noexcept {
        return const_cast<std::vector<ImportedAsset<T>>&>(std::as_const(*this).list_for<T>());
    }
    template <typename T>
    [[nodiscard]] const std::vector<ImportedAsset<T>>& list_for() const noexcept {
        if constexpr (std::is_same_v<T, MeshData>) return meshes;
        else if constexpr (std::is_same_v<T, MaterialData>) return materials;
        else if constexpr (std::is_same_v<T, TextureData>) return textures;
        else if constexpr (std::is_same_v<T, SkeletonData>) return skeletons;
        else if constexpr (std::is_same_v<T, AnimationClipData>) return animations;
        else {
            static_assert(std::is_same_v<T, SceneData>, "not an asset data type");
            return scenes;
        }
    }
};

// ---------------------------------------------------------------------------
// Importers. All thread-safe (no shared state).
// ---------------------------------------------------------------------------

// Imports a .gltf (JSON + external/embedded buffers) or .glb (binary) file: meshes,
// materials, referenced textures, skins -> skeletons, animations, scenes.
[[nodiscard]] Result<ImportResult> import_gltf(const std::filesystem::path& path,
                                               const ImportSettings&        settings = {});

// Decodes a standalone image file. png/jpg/tga/bmp/psd/gif -> RGBA8 (sRGB or UNORM per
// settings.image_color_space), .hdr -> RGBA32F (always linear). With generate_mips /
// compress_textures the result is cooked per standalone_image_role() (HDR stays 1 mip).
[[nodiscard]] Result<TextureData> import_image(const std::filesystem::path& path,
                                               const ImportSettings&        settings = {});

// Decodes an in-memory encoded image (any stb_image format). `srgb` selects RGBA8_SRGB
// vs RGBA8_UNORM for 8-bit images; Radiance HDR data always yields RGBA32F.
[[nodiscard]] Result<TextureData> decode_image(ByteSpan bytes, bool srgb, StringView debug_name = {});

// Dispatches on the file extension to the importer above; images become a single texture
// sub-asset (key "texture:0", primary).
[[nodiscard]] Result<ImportResult> import_file(const std::filesystem::path& path,
                                               const ImportSettings&        settings = {});

// ---------------------------------------------------------------------------
// Source-path utilities. Thread-safe.
// ---------------------------------------------------------------------------

// True for extensions import_file() understands (.gltf .glb .png .jpg .jpeg .tga .bmp
// .psd .gif .hdr), case-insensitive.
[[nodiscard]] bool is_supported_source(const std::filesystem::path& path);
[[nodiscard]] bool is_image_source(const std::filesystem::path& path);

// Canonical content-relative path of `file`: resolved (symlinks, "..", on-disk case on
// Windows), made relative to `content_root`, '/'-separated, UTF-8. Files outside the root
// (or an empty root) fall back to the file name alone / the absolute generic path.
[[nodiscard]] String canonical_source_path(const std::filesystem::path& file,
                                           const std::filesystem::path& content_root);

// The ImageColorSpace::Auto decision for a standalone image path.
[[nodiscard]] bool guess_image_is_srgb(const std::filesystem::path& path);
// Name heuristic for tangent-space normal maps ("normal", tokens "n", "nrm", "nor", "norm",
// "nml"): such standalone images are cooked as normal maps (BC5, renormalised mips).
[[nodiscard]] bool guess_image_is_normal_map(const std::filesystem::path& path);
// The role a standalone image is imported with: NormalMap when guess_image_is_normal_map()
// and the colour space resolves to linear; else Color (sRGB) or Data (linear) per
// settings.image_color_space / guess_image_is_srgb().
[[nodiscard]] TextureRole standalone_image_role(const std::filesystem::path& path, const ImportSettings& settings);

} // namespace aether::assets
