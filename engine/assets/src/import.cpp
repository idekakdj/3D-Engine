// import.cpp — importer dispatch, settings fingerprint, source-path canonicalisation.
#include "aether/assets/format.h"
#include "aether/assets/importers.h"

#include "file_util.h"

#include <array>
#include <format>
#include <system_error>

namespace fs = std::filesystem;

namespace aether::assets {

namespace {
constexpr std::array<StringView, 9> kImageExtensions{ ".png", ".jpg", ".jpeg", ".tga", ".bmp",
                                                      ".psd", ".gif", ".hdr", ".pic" };
} // namespace

u64 ImportSettings::fingerprint() const noexcept {
    const std::array<u8, 6> bits{
        static_cast<u8>(generate_normals),  static_cast<u8>(normal_generation),
        static_cast<u8>(generate_tangents), static_cast<u8>(import_textures),
        static_cast<u8>(image_color_space), u8{ 1 } /* layout version of this fingerprint */
    };
    return fnv1a64(ByteSpan(reinterpret_cast<const byte*>(bits.data()), bits.size()));
}

bool is_image_source(const fs::path& path) {
    const String ext = detail::extension_lower(path);
    for (StringView e : kImageExtensions) {
        if (ext == e) return true;
    }
    return false;
}

bool is_supported_source(const fs::path& path) {
    const String ext = detail::extension_lower(path);
    return ext == ".gltf" || ext == ".glb" || is_image_source(path);
}

String canonical_source_path(const fs::path& file, const fs::path& content_root) {
    std::error_code ec;
    fs::path        abs = fs::weakly_canonical(file, ec);
    if (ec || abs.empty()) abs = fs::absolute(file, ec).lexically_normal();
    if (content_root.empty()) return detail::to_utf8(abs.filename());

    fs::path root = fs::weakly_canonical(content_root, ec);
    if (ec || root.empty()) root = fs::absolute(content_root, ec).lexically_normal();
    const fs::path rel = abs.lexically_relative(root);
    if (rel.empty() || *rel.begin() == ".." || rel.is_absolute()) return detail::to_utf8(abs);
    return detail::to_utf8(rel);
}

Result<ImportResult> import_file(const fs::path& path, const ImportSettings& settings) {
    const String ext = detail::extension_lower(path);
    if (ext == ".gltf" || ext == ".glb") return import_gltf(path, settings);

    if (is_image_source(path)) {
        auto tex = import_image(path, settings);
        if (!tex) return tex.error();
        ImportResult result;
        result.source_path = canonical_source_path(path, settings.content_root);
        ImportedAsset<TextureData> asset;
        asset.key = "texture:0";
        asset.id = make_asset_id(result.source_path, asset.key);
        asset.name = detail::to_utf8(path.stem());
        asset.data = std::move(*tex);
        result.primary = asset.id;
        result.textures.push_back(std::move(asset));
        return result;
    }
    return Error{ ErrorCode::Unsupported, std::format("unsupported source type '{}': {}", ext, detail::to_utf8(path)) };
}

} // namespace aether::assets
