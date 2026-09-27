// texture_tests.cpp — mip generation, BC7/BC5 compression, cooked .aeasset round trips.
#include "aether/assets/format.h"
#include "aether/assets/texture_processing.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

using namespace aether;
using namespace aether::assets;

namespace {

TextureData make_rgba8(u32 w, u32 h, TextureFormat format, u32 layers = 1) {
    TextureData t;
    t.width = w;
    t.height = h;
    t.array_layers = layers;
    t.format = format;
    t.pixels.assign(static_cast<usize>(w) * h * 4 * layers, u8{ 0 });
    return t;
}

u8* texel(TextureData& t, u32 x, u32 y, u32 mip = 0, u32 layer = 0) {
    const u32 w = std::max(1u, t.width >> mip);
    return t.pixels.data() + texture_mip_offset(t, mip) +
           layer * texture_layer_byte_size(t.format, t.width, t.height, mip) + (static_cast<usize>(y) * w + x) * 4;
}

void set(TextureData& t, u32 x, u32 y, u8 r, u8 g, u8 b, u8 a = 255) {
    u8* p = texel(t, x, y);
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
}

// PSNR over the first `channels` channels of every texel of mip `mip`.
f64 psnr(const TextureData& a, const TextureData& b, u32 mip, u32 channels) {
    const u64 offset = texture_mip_offset(a, mip);
    const u64 bytes = texture_layer_byte_size(a.format, a.width, a.height, mip) * a.array_layers;
    f64       se = 0.0;
    u64       n = 0;
    for (u64 i = 0; i < bytes; i += 4) {
        for (u32 c = 0; c < channels; ++c) {
            const f64 d = static_cast<f64>(a.pixels[offset + i + c]) - static_cast<f64>(b.pixels[offset + i + c]);
            se += d * d;
            ++n;
        }
    }
    const f64 mse = se / static_cast<f64>(n);
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// A "texture-like" test image: smooth colour gradients plus low-contrast detail.
TextureData test_image(u32 size, TextureFormat format, bool alpha) {
    TextureData t = make_rgba8(size, size, format);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const f32 fx = static_cast<f32>(x) / static_cast<f32>(size);
            const f32 fy = static_cast<f32>(y) / static_cast<f32>(size);
            const f32 detail = 12.0f * std::sin(fx * 25.0f) * std::cos(fy * 17.0f);
            set(t, x, y, static_cast<u8>(std::clamp(40.0f + 180.0f * fx + detail, 0.0f, 255.0f)),
                static_cast<u8>(std::clamp(200.0f - 150.0f * fy + detail, 0.0f, 255.0f)),
                static_cast<u8>(std::clamp(90.0f + 60.0f * fx * fy, 0.0f, 255.0f)),
                alpha ? static_cast<u8>(std::clamp(255.0f * (0.5f + 0.5f * std::sin(fx * 6.0f + fy * 3.0f)), 0.0f, 255.0f))
                      : u8{ 255 });
        }
    }
    return t;
}

// Smooth tangent-space normal field (a gentle bump), encoded xyz * 0.5 + 0.5.
TextureData normal_image(u32 size) {
    TextureData t = make_rgba8(size, size, TextureFormat::RGBA8_UNORM);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const f32 nx = 0.5f * std::sin(static_cast<f32>(x) * 0.2f);
            const f32 ny = 0.4f * std::cos(static_cast<f32>(y) * 0.15f);
            const f32 nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
            set(t, x, y, static_cast<u8>(std::lround((nx * 0.5f + 0.5f) * 255.0f)),
                static_cast<u8>(std::lround((ny * 0.5f + 0.5f) * 255.0f)),
                static_cast<u8>(std::lround((nz * 0.5f + 0.5f) * 255.0f)));
        }
    }
    return t;
}

} // namespace

TEST_CASE("texture: mip chain sizes, layout and verbatim mip 0") {
    CHECK(full_mip_count(1, 1) == 1);
    CHECK(full_mip_count(256, 256) == 9);
    CHECK(full_mip_count(300, 17) == 9);
    CHECK(full_mip_count(1, 64) == 7);

    TextureData t = make_rgba8(8, 4, TextureFormat::RGBA8_UNORM);
    for (u32 i = 0; i < t.pixels.size(); ++i) t.pixels[i] = static_cast<u8>(i * 7);
    const std::vector<u8> mip0 = t.pixels;
    REQUIRE(generate_mip_chain(t, TextureRole::Data));
    CHECK(t.mip_levels == 4); // 8x4 4x2 2x1 1x1
    CHECK(t.pixels.size() == (8 * 4 + 4 * 2 + 2 * 1 + 1) * 4);
    CHECK(t.pixels.size() == texture_byte_size(t));
    CHECK(std::equal(mip0.begin(), mip0.end(), t.pixels.begin()));
    CHECK(texture_mip_offset(t, 2) == (8 * 4 + 4 * 2) * 4);
    CHECK_FALSE(generate_mip_chain(t, TextureRole::Data)); // already mipped

    // NPOT: 5x3 -> 2x1 -> 1x1.
    TextureData npot = make_rgba8(5, 3, TextureFormat::RGBA8_UNORM);
    REQUIRE(generate_mip_chain(npot, TextureRole::Data, MipFilter::Box));
    CHECK(npot.mip_levels == 3);
    CHECK(npot.pixels.size() == (15 + 2 + 1) * 4);

    // 1x1: nothing to do.
    TextureData one = make_rgba8(1, 1, TextureFormat::RGBA8_UNORM);
    REQUIRE(generate_mip_chain(one, TextureRole::Data));
    CHECK(one.mip_levels == 1);

    // Layers stay layer-major within each mip.
    TextureData layered = make_rgba8(4, 4, TextureFormat::RGBA8_UNORM, 2);
    std::fill(layered.pixels.begin(), layered.pixels.begin() + 64, u8{ 10 });
    std::fill(layered.pixels.begin() + 64, layered.pixels.end(), u8{ 200 });
    REQUIRE(generate_mip_chain(layered, TextureRole::Data));
    REQUIRE(layered.mip_levels == 3);
    for (u32 mip = 1; mip < 3; ++mip) {
        CHECK(texel(layered, 0, 0, mip, 0)[0] == 10);
        CHECK(texel(layered, 0, 0, mip, 1)[0] == 200);
    }
}

TEST_CASE("texture: box filter averages a known pattern exactly") {
    // 4x4 of 2x2 quadrants -> mip 1 = quadrant values, mip 2 = their average.
    TextureData t = make_rgba8(4, 4, TextureFormat::RGBA8_UNORM);
    const u8    q[4] = { 0, 100, 200, 60 };
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u8 v = q[(y / 2) * 2 + x / 2];
            set(t, x, y, v, v, v, 255);
        }
    }
    REQUIRE(generate_mip_chain(t, TextureRole::Data, MipFilter::Box));
    CHECK(texel(t, 0, 0, 1)[0] == 0);
    CHECK(texel(t, 1, 0, 1)[0] == 100);
    CHECK(texel(t, 0, 1, 1)[0] == 200);
    CHECK(texel(t, 1, 1, 1)[0] == 60);
    CHECK(texel(t, 0, 0, 2)[0] == 90); // (0 + 100 + 200 + 60) / 4
    CHECK(texel(t, 0, 0, 2)[3] == 255);

    // NPOT box: 3x1 -> 1x1 averages all three texels.
    TextureData row = make_rgba8(3, 1, TextureFormat::RGBA8_UNORM);
    set(row, 0, 0, 30, 0, 0);
    set(row, 1, 0, 60, 0, 0);
    set(row, 2, 0, 90, 0, 0);
    REQUIRE(generate_mip_chain(row, TextureRole::Data, MipFilter::Box));
    REQUIRE(row.mip_levels == 2);
    CHECK(texel(row, 0, 0, 1)[0] == 60);
}

TEST_CASE("texture: sRGB-correct filtering (linearise, filter, re-encode)") {
    for (MipFilter filter : { MipFilter::Box, MipFilter::Kaiser }) {
        // 1-texel black/white checker: the linear average is 0.5 -> sRGB 188, not 128.
        TextureData srgb = make_rgba8(16, 16, TextureFormat::RGBA8_SRGB);
        TextureData unorm = make_rgba8(16, 16, TextureFormat::RGBA8_UNORM);
        for (u32 y = 0; y < 16; ++y) {
            for (u32 x = 0; x < 16; ++x) {
                const u8 v = ((x + y) & 1u) ? 255 : 0;
                set(srgb, x, y, v, v, v);
                set(unorm, x, y, v, v, v);
            }
        }
        REQUIRE(generate_mip_chain(srgb, TextureRole::Color, filter));
        REQUIRE(generate_mip_chain(unorm, TextureRole::Data, filter));
        CHECK(srgb.mip_levels == 5);
        for (u32 mip = 1; mip < srgb.mip_levels; ++mip) {
            const u32 w = 16u >> mip;
            for (u32 y = 0; y < w; ++y) {
                for (u32 x = 0; x < w; ++x) {
                    CHECK(std::abs(static_cast<int>(texel(srgb, x, y, mip)[0]) - 188) <= 1);
                    CHECK(std::abs(static_cast<int>(texel(unorm, x, y, mip)[0]) - 128) <= 1);
                    CHECK(texel(srgb, x, y, mip)[3] == 255);
                }
            }
        }
    }

    // Transparent texels do not bleed their colour into the average (alpha-weighted).
    TextureData cut = make_rgba8(2, 1, TextureFormat::RGBA8_SRGB);
    set(cut, 0, 0, 255, 0, 0, 255);
    set(cut, 1, 0, 0, 255, 0, 0); // invisible green
    REQUIRE(generate_mip_chain(cut, TextureRole::Color, MipFilter::Box));
    CHECK(texel(cut, 0, 0, 1)[0] == 255);
    CHECK(texel(cut, 0, 0, 1)[1] == 0);
    CHECK(texel(cut, 0, 0, 1)[3] == 128);
}

TEST_CASE("texture: Kaiser preserves the mean of smooth content") {
    TextureData t = test_image(64, TextureFormat::RGBA8_UNORM, false);
    REQUIRE(generate_mip_chain(t, TextureRole::Data));
    auto mean = [&](u32 mip, u32 c) {
        const u32 w = 64u >> mip;
        f64       sum = 0.0;
        for (u32 y = 0; y < w; ++y) {
            for (u32 x = 0; x < w; ++x) sum += texel(t, x, y, mip)[c];
        }
        return sum / (static_cast<f64>(w) * w);
    };
    for (u32 c = 0; c < 3; ++c) {
        CHECK(mean(1, c) == doctest::Approx(mean(0, c)).epsilon(0.01));
        CHECK(mean(3, c) == doctest::Approx(mean(0, c)).epsilon(0.02));
    }
}

TEST_CASE("texture: normal maps are renormalised per mip") {
    // Columns alternate between (+0.6, 0, 0.8) and (-0.6, 0, 0.8): the raw average (0, 0, 0.8)
    // must come out as the unit +Z normal (128, 128, 255), not (128, 128, 230).
    TextureData t = make_rgba8(8, 8, TextureFormat::RGBA8_UNORM);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) set(t, x, y, (x & 1u) ? u8{ 204 } : u8{ 51 }, 128, 230);
    }
    REQUIRE(generate_mip_chain(t, TextureRole::NormalMap));
    CHECK(t.mip_levels == 4);
    for (u32 mip = 1; mip < 4; ++mip) {
        const u32 w = 8u >> mip;
        for (u32 y = 0; y < w; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const u8* p = texel(t, x, y, mip);
                CHECK(std::abs(static_cast<int>(p[0]) - 128) <= 1);
                CHECK(std::abs(static_cast<int>(p[1]) - 128) <= 1);
                CHECK(p[2] == 255);
            }
        }
    }
    // A smooth field stays unit length at every level.
    TextureData field = normal_image(32);
    REQUIRE(generate_mip_chain(field, TextureRole::NormalMap));
    for (u32 mip = 1; mip < field.mip_levels; ++mip) {
        const u32 w = 32u >> mip;
        for (u32 y = 0; y < w; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const u8* p = texel(field, x, y, mip);
                const f32 n[3] = { p[0] / 255.0f * 2 - 1, p[1] / 255.0f * 2 - 1, p[2] / 255.0f * 2 - 1 };
                CHECK(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) == doctest::Approx(1.0).epsilon(0.02));
            }
        }
    }
}

TEST_CASE("texture: RGBA32F mips filter in float") {
    TextureData t;
    t.width = 2;
    t.height = 2;
    t.format = TextureFormat::RGBA32F;
    const f32 v[16] = { 1, 2, 4, 1, 3, 2, 0, 1, 5, 2, 8, 1, 7, 2, 4, 1 };
    t.pixels.resize(sizeof(v));
    std::memcpy(t.pixels.data(), v, sizeof(v));
    REQUIRE(generate_mip_chain(t, TextureRole::Color, MipFilter::Box));
    REQUIRE(t.mip_levels == 2);
    f32 m[4];
    std::memcpy(m, t.pixels.data() + 64, sizeof(m));
    CHECK(m[0] == doctest::Approx(4.0f));
    CHECK(m[1] == doctest::Approx(2.0f));
    CHECK(m[2] == doctest::Approx(4.0f));
    CHECK(m[3] == doctest::Approx(1.0f));

    TextureData hdr = t; // cook_texture leaves HDR alone
    hdr.mip_levels = 1;
    hdr.pixels.resize(64);
    REQUIRE(cook_texture(hdr, TextureRole::Color, {}));
    CHECK(hdr.format == TextureFormat::RGBA32F);
    CHECK(hdr.mip_levels == 1);
}

TEST_CASE("texture: BC7 encode/decode quality (sRGB colour, linear data, alpha)") {
    struct Case {
        TextureFormat in;
        TextureRole   role;
        TextureFormat out;
        bool          alpha;
    };
    for (const Case c : { Case{ TextureFormat::RGBA8_SRGB, TextureRole::Color, TextureFormat::BC7_SRGB, false },
                          Case{ TextureFormat::RGBA8_UNORM, TextureRole::Data, TextureFormat::BC7_UNORM, false },
                          Case{ TextureFormat::RGBA8_SRGB, TextureRole::Color, TextureFormat::BC7_SRGB, true } }) {
        TextureData src = test_image(64, c.in, c.alpha);
        REQUIRE(generate_mip_chain(src, c.role));
        TextureData bc = src;
        REQUIRE(compress_texture(bc, c.role));
        CHECK(bc.format == c.out);
        CHECK(bc.mip_levels == 7);
        // 16x16 + 8x8 + 4x4 + 2x2 + 1 + 1 + 1 blocks of 16 bytes.
        CHECK(bc.pixels.size() == (256 + 64 + 16 + 4 + 1 + 1 + 1) * 16);
        CHECK(bc.pixels.size() == texture_byte_size(bc));

        auto decoded = decompress_texture(bc);
        REQUIRE(decoded.has_value());
        CHECK(decoded->format == c.in);
        // bc7enc (modes 1/6 opaque, 5/6/7 alpha): high quality on the full-size levels; the
        // small levels hold near-Nyquist detail (and independent alpha), so they only get a
        // sanity bound that still catches any block-layout / format error (< 15 dB).
        for (u32 mip = 0; mip < 4; ++mip) { // 64x64 .. 8x8 (single-block levels are not scored)
            const f64 p = psnr(src, *decoded, mip, 4);
            INFO("format ", static_cast<int>(c.out), " mip ", mip, " psnr ", p);
            CHECK(p > (mip == 0 ? 38.0 : mip == 1 ? 34.0 : 22.0));
        }
    }
}

TEST_CASE("texture: BC5 normal maps (RG stored, z reconstructed)") {
    TextureData src = normal_image(64);
    REQUIRE(generate_mip_chain(src, TextureRole::NormalMap));
    TextureData bc = src;
    REQUIRE(compress_texture(bc, TextureRole::NormalMap));
    CHECK(bc.format == TextureFormat::BC5_UNORM);
    CHECK(bc.pixels.size() == texture_byte_size(bc));
    auto decoded = decompress_texture(bc);
    REQUIRE(decoded.has_value());
    CHECK(decoded->format == TextureFormat::RGBA8_UNORM);
    for (u32 mip = 0; mip < 4; ++mip) {
        const f64 p = psnr(src, *decoded, mip, 2);
        INFO("mip ", mip, " psnr ", p);
        CHECK(p > 40.0);
    }
    // z is reconstructed from x, y.
    for (u32 i = 0; i < 64 * 64; ++i) {
        CHECK(std::abs(static_cast<int>(decoded->pixels[i * 4 + 2]) - static_cast<int>(src.pixels[i * 4 + 2])) <= 3);
        CHECK(decoded->pixels[i * 4 + 3] == 255);
    }
}

TEST_CASE("texture: BC block layout (row-major 4x4 blocks, partial edge blocks)") {
    // 8x8 with four solid 4x4 quadrants -> mip 0 has exactly one block per quadrant.
    const u8    colors[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 } };
    TextureData t = make_rgba8(8, 8, TextureFormat::RGBA8_UNORM);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const u8* c = colors[(y / 4) * 2 + x / 4];
            set(t, x, y, c[0], c[1], c[2]);
        }
    }
    TextureData bc = t;
    REQUIRE(compress_texture(bc, TextureRole::Data)); // single mip: compress without a chain
    REQUIRE(bc.pixels.size() == 4 * 16);
    for (u32 b = 0; b < 4; ++b) {
        TextureData block;
        block.width = 4;
        block.height = 4;
        block.format = TextureFormat::BC7_UNORM;
        block.pixels.assign(bc.pixels.begin() + b * 16, bc.pixels.begin() + (b + 1) * 16);
        auto d = decompress_texture(block);
        REQUIRE(d.has_value());
        for (u32 i = 0; i < 16; ++i) {
            for (u32 k = 0; k < 3; ++k) CHECK(std::abs(static_cast<int>(d->pixels[i * 4 + k]) - colors[b][k]) <= 1);
        }
    }

    // 6x6 with mips: 2x2 + 1 + 1 blocks; edge texels survive the round trip.
    TextureData odd = make_rgba8(6, 6, TextureFormat::RGBA8_UNORM);
    for (u32 y = 0; y < 6; ++y) {
        for (u32 x = 0; x < 6; ++x) set(odd, x, y, static_cast<u8>(x * 40), static_cast<u8>(y * 40), 128);
    }
    REQUIRE(generate_mip_chain(odd, TextureRole::Data, MipFilter::Box));
    TextureData odd_bc = odd;
    REQUIRE(compress_texture(odd_bc, TextureRole::Data));
    CHECK(odd_bc.mip_levels == 3);
    CHECK(odd_bc.pixels.size() == (4 + 1 + 1) * 16);
    auto odd_dec = decompress_texture(odd_bc);
    REQUIRE(odd_dec.has_value());
    CHECK(psnr(odd, *odd_dec, 0, 4) > 28.0); // steep independent R/G ramps: a hard case for BC7
    CHECK(std::abs(static_cast<int>(texel(*odd_dec, 5, 5)[0]) - 200) <= 4);

    TextureData hdr;
    hdr.width = 4;
    hdr.height = 4;
    hdr.format = TextureFormat::RGBA32F;
    hdr.pixels.resize(4 * 4 * 16);
    CHECK_FALSE(compress_texture(hdr, TextureRole::Color)); // RGBA8 only
}

TEST_CASE("texture: compressed textures round-trip through .aeasset") {
    TextureData color = test_image(32, TextureFormat::RGBA8_SRGB, true);
    REQUIRE(cook_texture(color, TextureRole::Color, {}));
    TextureData normal = normal_image(16);
    REQUIRE(cook_texture(normal, TextureRole::NormalMap, {}));
    CHECK(color.format == TextureFormat::BC7_SRGB);
    CHECK(normal.format == TextureFormat::BC5_UNORM);
    CHECK(normal.mip_levels == 5);

    for (const TextureData* t : { &color, &normal }) {
        const CookedMeta meta{ AssetId::from_string("tex#test"), 0x1234, 7 };
        auto             bytes = encode_asset(meta, *t);
        REQUIRE(bytes.has_value());
        auto back = decode_asset<TextureData>(as_bytes_view(*bytes));
        REQUIRE(back.has_value());
        CHECK(back->format == t->format);
        CHECK(back->width == t->width);
        CHECK(back->height == t->height);
        CHECK(back->mip_levels == t->mip_levels);
        CHECK(back->pixels == t->pixels);
    }
    TextureData bad = color;
    bad.pixels.pop_back(); // size must match the block layout
    CHECK_FALSE(encode_asset(CookedMeta{}, bad).has_value());
    CHECK(is_block_compressed(TextureFormat::BC5_UNORM));
    CHECK_FALSE(is_block_compressed(TextureFormat::RGBA8_SRGB));
    CHECK(texture_layer_byte_size(TextureFormat::BC7_SRGB, 5, 9, 0) == 2 * 3 * 16);
    CHECK(texture_layer_byte_size(TextureFormat::BC7_SRGB, 5, 9, 3) == 16);
}
