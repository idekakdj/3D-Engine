// aether/assets/texture_processing.h — CPU texture cooking: mip chains + BC7/BC5 compression.
//
// Used by the importers when ImportSettings::generate_mips / compress_textures are set (the
// aether-cook defaults, see ImportSettings::cooking()). Public so tools and tests can cook or
// inspect textures the same way. Every function is thread-safe (re-entrant; the encoders'
// global tables are initialised once, internally).
//
// Layout (asset_types.h): mips 0..n-1, each mip holds all layers (layer-major), tightly
// packed; BC formats store ceil(w/4) x ceil(h/4) 16-byte blocks per layer and mip.
//
// Mip generation
//   - Full chain down to 1x1 (NPOT-safe: each level is max(1, floor(prev / 2))).
//   - Color (sRGB) data is linearised, filtered and re-encoded (sRGB-correct); colour is
//     alpha-weighted so fully transparent texels do not bleed into their neighbours.
//   - Data (linear) textures are filtered as stored.
//   - Normal maps are decoded to vectors, filtered, renormalised per mip (z >= 0) and
//     re-encoded (xyz * 0.5 + 0.5).
//   - Filters: Kaiser-windowed sinc (default; width 3, alpha 4 - the NVTT mip filter) or an
//     exact area-weighted box. Edges use mirror (reflect-101) addressing.
//   - RGBA32F is filtered in float (results clamped to >= 0); RGBA16F is not supported.
//
// Compression (RGBA8 input, any mip count)
//   Color     -> BC7_SRGB   (bc7enc, perceptual error weights)
//   Data      -> BC7_UNORM  (bc7enc, linear error weights)
//   NormalMap -> BC5_UNORM  (rgbcx, R = x, G = y; shaders reconstruct z = sqrt(1 - x^2 - y^2))
#pragma once

#include "aether/assets/asset_types.h"
#include "aether/core/error.h"
#include "aether/core/types.h"

namespace aether::assets {

// What a texture's texels mean; decides filtering and the compressed format.
enum class TextureRole : u8 {
    Color = 0, // sRGB colour (base colour, emissive, UI)
    Data,      // linear data (metallic-roughness, occlusion, masks)
    NormalMap, // tangent-space normal map (+Y up), linear
};

enum class MipFilter : u8 {
    Kaiser = 0, // windowed sinc, sharper (default)
    Box,        // exact area average
};

// Number of levels of a full chain for a width x height texture (1 + floor(log2(max))).
[[nodiscard]] u32 full_mip_count(u32 width, u32 height) noexcept;

// Replaces `texture` (which must have mip_levels == 1; RGBA8_UNORM, RGBA8_SRGB or RGBA32F)
// with a full mip chain. No-op for a 1x1 texture. Returns InvalidArgument / Unsupported for
// other inputs (already mipped, compressed, RGBA16F, inconsistent byte size).
[[nodiscard]] Result<void> generate_mip_chain(TextureData& texture, TextureRole role,
                                              MipFilter filter = MipFilter::Kaiser);

// Block-compresses every mip and layer of an RGBA8 texture in place (format per role, see
// file comment). Partial edge blocks replicate the last row/column.
[[nodiscard]] Result<void> compress_texture(TextureData& texture, TextureRole role);

// Decodes a BC7/BC5 texture (all mips and layers) to RGBA8 (BC7_SRGB -> RGBA8_SRGB, BC7_UNORM
// and BC5_UNORM -> RGBA8_UNORM; BC5 gets b = reconstructed z, a = 255). Uncompressed input
// is returned unchanged. For tests, tools and thumbnails.
[[nodiscard]] Result<TextureData> decompress_texture(const TextureData& texture);

struct TextureCookOptions {
    bool      generate_mips = true;
    bool      compress = true; // RGBA8 only; implies generate_mips (BC textures carry every mip)
    MipFilter filter = MipFilter::Kaiser;
};

// Mips then compression, per options. HDR (RGBA32F) textures are left untouched (single
// mip, uncompressed): the renderer's IBL path consumes them as-is.
[[nodiscard]] Result<void> cook_texture(TextureData& texture, TextureRole role, const TextureCookOptions& options);

} // namespace aether::assets
