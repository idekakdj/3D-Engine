// asset_traits.cpp — AssetType names and deterministic AssetId helpers.
#include "aether/assets/asset_traits.h"

#include <array>

namespace aether::assets {

namespace {
struct TypeName {
    AssetType  type;
    StringView name;
};
constexpr std::array<TypeName, 9> kTypeNames{ {
    { AssetType::Unknown, "Unknown" },
    { AssetType::Mesh, "Mesh" },
    { AssetType::Texture, "Texture" },
    { AssetType::Material, "Material" },
    { AssetType::Skeleton, "Skeleton" },
    { AssetType::AnimationClip, "AnimationClip" },
    { AssetType::Scene, "Scene" },
    { AssetType::Shader, "Shader" },
    { AssetType::Script, "Script" },
} };

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

StringView asset_type_name(AssetType type) noexcept {
    for (const TypeName& t : kTypeNames) {
        if (t.type == type) return t.name;
    }
    return "Unknown";
}

std::optional<AssetType> parse_asset_type(StringView name) noexcept {
    for (const TypeName& t : kTypeNames) {
        if (t.name == name) return t.type;
    }
    return std::nullopt;
}

String make_asset_key(StringView source_path, StringView sub_key) {
    String key;
    key.reserve(source_path.size() + 1 + sub_key.size());
    key.append(source_path);
    key.push_back('#');
    key.append(sub_key);
    return key;
}

AssetId make_asset_id(StringView source_path, StringView sub_key) {
    return AssetId::from_string(make_asset_key(source_path, sub_key));
}

String asset_id_to_hex(AssetId id) {
    String out(32, '0');
    for (int i = 0; i < 16; ++i) {
        out[static_cast<usize>(i)] = kHexDigits[(id.hi >> (60 - 4 * i)) & 0xF];
        out[static_cast<usize>(16 + i)] = kHexDigits[(id.lo >> (60 - 4 * i)) & 0xF];
    }
    return out;
}

std::optional<AssetId> asset_id_from_hex(StringView hex) noexcept {
    if (hex.size() != 32) return std::nullopt;
    AssetId id;
    for (usize i = 0; i < 32; ++i) {
        const int v = hex_value(hex[i]);
        if (v < 0) return std::nullopt;
        u64& word = i < 16 ? id.hi : id.lo;
        word = (word << 4) | static_cast<u64>(v);
    }
    return id;
}

} // namespace aether::assets
