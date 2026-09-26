// image_importer.cpp — stb_image decoding (the one STB_IMAGE_IMPLEMENTATION TU).
#include "aether/assets/importers.h"

#include "file_util.h"

#include <array>
#include <climits>
#include <cstring>
#include <format>
#include <memory>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO          // memory-only: all file access goes through std::filesystem
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

namespace fs = std::filesystem;

namespace aether::assets {

namespace {
struct StbiFree {
    void operator()(void* p) const noexcept { stbi_image_free(p); }
};

bool checked_rgba_size(int w, int h, usize bytes_per_texel, usize& out) {
    if (w <= 0 || h <= 0) return false;
    const u64 n = static_cast<u64>(w) * static_cast<u64>(h) * 4u * bytes_per_texel;
    if (n > (u64{ 1 } << 34)) return false; // 16 GiB sanity cap
    out = static_cast<usize>(n);
    return true;
}
} // namespace

Result<TextureData> decode_image(ByteSpan bytes, bool srgb, StringView debug_name) {
    if (bytes.empty() || bytes.size() > static_cast<usize>(INT_MAX)) {
        return Error{ ErrorCode::InvalidArgument, std::format("image '{}': empty or too large", debug_name) };
    }
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int   len = static_cast<int>(bytes.size());
    int         w = 0, h = 0, comp = 0;

    TextureData tex;
    tex.mip_levels = 1;
    tex.array_layers = 1;
    tex.is_cubemap = false;

    if (stbi_is_hdr_from_memory(data, len)) {
        std::unique_ptr<float, StbiFree> pixels(stbi_loadf_from_memory(data, len, &w, &h, &comp, 4));
        usize size = 0;
        if (!pixels || !checked_rgba_size(w, h, sizeof(float), size)) {
            return Error{ ErrorCode::IoError, std::format("image '{}': HDR decode failed: {}", debug_name,
                                                          stbi_failure_reason() ? stbi_failure_reason() : "?") };
        }
        tex.format = TextureFormat::RGBA32F;
        tex.pixels.resize(size);
        std::memcpy(tex.pixels.data(), pixels.get(), size);
    } else {
        std::unique_ptr<stbi_uc, StbiFree> pixels(stbi_load_from_memory(data, len, &w, &h, &comp, 4));
        usize size = 0;
        if (!pixels || !checked_rgba_size(w, h, 1, size)) {
            return Error{ ErrorCode::IoError, std::format("image '{}': decode failed: {}", debug_name,
                                                          stbi_failure_reason() ? stbi_failure_reason() : "?") };
        }
        tex.format = srgb ? TextureFormat::RGBA8_SRGB : TextureFormat::RGBA8_UNORM;
        tex.pixels.assign(pixels.get(), pixels.get() + size);
    }
    tex.width = static_cast<u32>(w);
    tex.height = static_cast<u32>(h);
    return tex;
}

bool guess_image_is_srgb(const fs::path& path) {
    const String stem = detail::to_lower_ascii(detail::to_utf8(path.stem()));
    // Whole-token markers ("brick_n.png", "rock-mr.png") ...
    static constexpr std::array<StringView, 16> kTokens{ "n",   "nrm", "nor", "norm",  "ddna", "mr",
                                                         "orm", "arm", "rma", "ao",    "rough", "metal",
                                                         "spec", "disp", "bump", "mask" };
    // ... and substrings ("RockNormal.png", "wood_roughness.jpg").
    static constexpr std::array<StringView, 8> kSubstrings{ "normal", "roughness", "metallic", "metalness",
                                                            "occlusion", "height", "displace", "gloss" };
    for (StringView s : kSubstrings) {
        if (stem.find(s) != String::npos) return false;
    }
    usize start = 0;
    while (start <= stem.size()) {
        usize end = start;
        while (end < stem.size() && ((stem[end] >= 'a' && stem[end] <= 'z') || (stem[end] >= '0' && stem[end] <= '9')))
            ++end;
        const StringView token(stem.data() + start, end - start);
        for (StringView t : kTokens) {
            if (token == t) return false;
        }
        start = end + 1;
    }
    return true;
}

Result<TextureData> import_image(const fs::path& path, const ImportSettings& settings) {
    auto bytes = detail::read_file_bytes(path);
    if (!bytes) return bytes.error();
    bool srgb = true;
    switch (settings.image_color_space) {
    case ImageColorSpace::Srgb: srgb = true; break;
    case ImageColorSpace::Linear: srgb = false; break;
    case ImageColorSpace::Auto: srgb = guess_image_is_srgb(path); break;
    }
    return decode_image(detail::bytes_of(*bytes), srgb, detail::to_utf8(path.filename()));
}

} // namespace aether::assets
