// aether/assets/asset_traits.h — compile-time asset type mapping + AssetId helpers.
//
// Maps each asset DATA type (asset_types.h) to its AssetType tag, and defines the one
// canonical rule for deriving deterministic AssetIds from source content:
//
//     AssetId = AssetId::from_string(<content-relative source path> + "#" + <sub-asset key>)
//
// where the source path is '/'-separated, relative to the project's content root (see
// importers.h canonical_source_path), and the sub-asset key is "<kind>:<index>[...]",
// e.g. "mesh:0", "material:2", "texture:1:srgb", "skeleton:0", "anim:0", "scene:0".
// All functions here are pure and thread-safe.
#pragma once

#include "aether/assets/asset_types.h"
#include "aether/core/handle.h"
#include "aether/core/types.h"

#include <optional>
#include <string>
#include <type_traits>

namespace aether::assets {

// ---------------------------------------------------------------------------
// Data type <-> AssetType
// ---------------------------------------------------------------------------
template <typename T>
struct AssetTypeOf {
    static constexpr AssetType value = AssetType::Unknown;
};
template <> struct AssetTypeOf<MeshData>          { static constexpr AssetType value = AssetType::Mesh; };
template <> struct AssetTypeOf<TextureData>       { static constexpr AssetType value = AssetType::Texture; };
template <> struct AssetTypeOf<MaterialData>      { static constexpr AssetType value = AssetType::Material; };
template <> struct AssetTypeOf<SkeletonData>      { static constexpr AssetType value = AssetType::Skeleton; };
template <> struct AssetTypeOf<AnimationClipData> { static constexpr AssetType value = AssetType::AnimationClip; };
template <> struct AssetTypeOf<SceneData>         { static constexpr AssetType value = AssetType::Scene; };

template <typename T>
inline constexpr AssetType asset_type_of_v = AssetTypeOf<std::remove_cvref_t<T>>::value;

// Satisfied by every data type that can be cooked, loaded and hot-reloaded.
template <typename T>
concept CookableAsset = (asset_type_of_v<T> != AssetType::Unknown);

// ---------------------------------------------------------------------------
// Names (stable; used in asset_db.json). Thread-safe.
// ---------------------------------------------------------------------------
[[nodiscard]] StringView               asset_type_name(AssetType type) noexcept;
[[nodiscard]] std::optional<AssetType> parse_asset_type(StringView name) noexcept;

// ---------------------------------------------------------------------------
// AssetId helpers. Thread-safe.
// ---------------------------------------------------------------------------

// The canonical sub-asset identity string: "<source_path>#<sub_key>".
[[nodiscard]] String make_asset_key(StringView source_path, StringView sub_key);

// Deterministic id for a sub-asset of a source (see file comment).
[[nodiscard]] AssetId make_asset_id(StringView source_path, StringView sub_key);

// Fixed 32-lowercase-hex-digit encoding (hi then lo) used by the asset database. Kept
// in this module so the on-disk DB format never depends on core's to_string() format.
[[nodiscard]] String                 asset_id_to_hex(AssetId id);
[[nodiscard]] std::optional<AssetId> asset_id_from_hex(StringView hex) noexcept;

} // namespace aether::assets
