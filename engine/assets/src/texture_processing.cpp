// texture_processing.cpp — mip chains (Kaiser / box, sRGB-correct, normal renormalisation) and
// BC7 / BC5 block compression through bc7enc_rdo (bc7enc.cpp, bc7decomp.cpp, rgbcx).
#include "aether/assets/texture_processing.h"

#include "aether/assets/format.h"

#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

namespace aether::assets {

namespace {

static_assert(sizeof(bc7decomp::color_rgba) == 4, "bc7decomp::color_rgba must be 4 packed bytes");

constexpr f64 kPi = 3.14159265358979323846;
constexpr f64 kKaiserWidth = 3.0; // lobes, in destination texels
constexpr f64 kKaiserAlpha = 4.0;

// ---------------------------------------------------------------------------
// Colour conversions
// ---------------------------------------------------------------------------
f32 srgb_to_linear(f32 c) noexcept {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

f32 linear_to_srgb(f32 c) noexcept {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

const std::array<f32, 256>& srgb_decode_lut() {
    static const std::array<f32, 256> lut = [] {
        std::array<f32, 256> t{};
        for (usize i = 0; i < 256; ++i) t[i] = srgb_to_linear(static_cast<f32>(i) / 255.0f);
        return t;
    }();
    return lut;
}

u8 to_unorm8(f32 v) noexcept {
    return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// ---------------------------------------------------------------------------
// Separable resampling
// ---------------------------------------------------------------------------
f64 bessel_i0(f64 x) noexcept {
    f64 sum = 1.0, term = 1.0;
    const f64 q = x * x * 0.25;
    for (int k = 1; k < 64; ++k) {
        term *= q / (static_cast<f64>(k) * static_cast<f64>(k));
        sum += term;
        if (term < sum * 1e-16) break;
    }
    return sum;
}

f64 sinc(f64 x) noexcept {
    if (std::fabs(x) < 1e-9) return 1.0;
    return std::sin(kPi * x) / (kPi * x);
}

f64 kaiser(f64 x) noexcept {
    if (std::fabs(x) >= kKaiserWidth) return 0.0;
    const f64 t = x / kKaiserWidth;
    return sinc(x) * bessel_i0(kKaiserAlpha * std::sqrt(1.0 - t * t)) / bessel_i0(kKaiserAlpha);
}

// Mirror addressing without edge duplication (reflect-101: -1 -> 1, n -> n - 2), which keeps
// the parity of high-frequency patterns intact at the borders.
u32 mirror_index(i64 i, i64 n) noexcept {
    if (n <= 1) return 0;
    const i64 period = 2 * (n - 1);
    i %= period;
    if (i < 0) i += period;
    return static_cast<u32>(i < n ? i : period - i);
}

struct Tap {
    u32 index;
    f32 weight;
};

// taps[offsets[i] .. offsets[i + 1]) produce destination texel i.
struct Resampler {
    std::vector<u32> offsets;
    std::vector<Tap> taps;
};

Resampler make_resampler(u32 src, u32 dst, MipFilter filter) {
    Resampler r;
    r.offsets.reserve(dst + 1);
    r.offsets.push_back(0);
    std::vector<std::pair<u32, f64>> acc;
    const f64 scale = static_cast<f64>(src) / static_cast<f64>(dst);
    for (u32 i = 0; i < dst; ++i) {
        acc.clear();
        auto add = [&](u32 index, f64 w) {
            for (auto& [idx, weight] : acc) {
                if (idx == index) {
                    weight += w;
                    return;
                }
            }
            acc.emplace_back(index, w);
        };
        if (src == dst) {
            add(i, 1.0);
        } else if (filter == MipFilter::Box) {
            // Exact area overlap of source texel [x, x + 1) with the footprint [lo, hi).
            const f64 lo = static_cast<f64>(i) * scale;
            const f64 hi = static_cast<f64>(i + 1) * scale;
            for (i64 x = static_cast<i64>(std::floor(lo)); static_cast<f64>(x) < hi; ++x) {
                const f64 overlap = std::min(static_cast<f64>(x + 1), hi) - std::max(static_cast<f64>(x), lo);
                if (overlap > 1e-12) add(mirror_index(x, src), overlap);
            }
        } else {
            const f64 center = (static_cast<f64>(i) + 0.5) * scale;
            const f64 support = kKaiserWidth * scale;
            const i64 x0 = static_cast<i64>(std::floor(center - support));
            const i64 x1 = static_cast<i64>(std::ceil(center + support));
            for (i64 x = x0; x <= x1; ++x) {
                const f64 w = kaiser((static_cast<f64>(x) + 0.5 - center) / scale);
                if (w != 0.0) add(mirror_index(x, src), w);
            }
        }
        f64 sum = 0.0;
        for (const auto& [idx, w] : acc) sum += w;
        if (!(std::fabs(sum) > 1e-12)) sum = 1.0;
        for (const auto& [idx, w] : acc) r.taps.push_back({ idx, static_cast<f32>(w / sum) });
        r.offsets.push_back(static_cast<u32>(r.taps.size()));
    }
    return r;
}

// Float RGBA image (4 floats per texel), separable downsample.
std::vector<f32> resample(const std::vector<f32>& src, u32 sw, u32 sh, u32 dw, u32 dh, MipFilter filter) {
    const Resampler rx = make_resampler(sw, dw, filter);
    const Resampler ry = make_resampler(sh, dh, filter);
    std::vector<f32> tmp(static_cast<usize>(dw) * sh * 4, 0.0f);
    for (u32 y = 0; y < sh; ++y) {
        const f32* row = src.data() + static_cast<usize>(y) * sw * 4;
        f32*       out = tmp.data() + static_cast<usize>(y) * dw * 4;
        for (u32 x = 0; x < dw; ++x) {
            f32 c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (u32 t = rx.offsets[x]; t < rx.offsets[x + 1]; ++t) {
                const f32* p = row + static_cast<usize>(rx.taps[t].index) * 4;
                const f32  w = rx.taps[t].weight;
                for (int k = 0; k < 4; ++k) c[k] += p[k] * w;
            }
            std::memcpy(out + static_cast<usize>(x) * 4, c, sizeof(c));
        }
    }
    std::vector<f32> dst(static_cast<usize>(dw) * dh * 4, 0.0f);
    for (u32 y = 0; y < dh; ++y) {
        f32* out = dst.data() + static_cast<usize>(y) * dw * 4;
        for (u32 t = ry.offsets[y]; t < ry.offsets[y + 1]; ++t) {
            const f32* row = tmp.data() + static_cast<usize>(ry.taps[t].index) * dw * 4;
            const f32  w = ry.taps[t].weight;
            for (usize i = 0; i < static_cast<usize>(dw) * 4; ++i) out[i] += row[i] * w;
        }
    }
    return dst;
}

// ---------------------------------------------------------------------------
// Working-space conversion. Color (sRGB): linear, alpha-premultiplied. Normal maps: unit
// vectors in [-1, 1]. Everything else: the stored values.
// ---------------------------------------------------------------------------
enum class Space : u8 { Raw = 0, SrgbPremultiplied, NormalVector };

void normalize_normal(f32* v) noexcept {
    f32 x = v[0], y = v[1], z = std::max(v[2], 0.0f); // tangent-space normals face +z
    const f32 len = std::sqrt(x * x + y * y + z * z);
    if (!(len > 1e-6f)) {
        x = 0.0f;
        y = 0.0f;
        z = 1.0f;
    } else {
        x /= len;
        y /= len;
        z /= len;
    }
    v[0] = x;
    v[1] = y;
    v[2] = z;
}

std::vector<f32> to_working(const u8* texels, usize count, Space space) {
    std::vector<f32> out(count * 4);
    const auto&      lut = srgb_decode_lut();
    for (usize i = 0; i < count; ++i) {
        const u8* p = texels + i * 4;
        f32*      o = out.data() + i * 4;
        switch (space) {
        case Space::SrgbPremultiplied: {
            const f32 a = static_cast<f32>(p[3]) / 255.0f;
            o[0] = lut[p[0]] * a;
            o[1] = lut[p[1]] * a;
            o[2] = lut[p[2]] * a;
            o[3] = a;
            break;
        }
        case Space::NormalVector:
            for (int k = 0; k < 3; ++k) o[k] = static_cast<f32>(p[k]) / 255.0f * 2.0f - 1.0f;
            o[3] = static_cast<f32>(p[3]) / 255.0f;
            normalize_normal(o);
            break;
        case Space::Raw:
            for (int k = 0; k < 4; ++k) o[k] = static_cast<f32>(p[k]) / 255.0f;
            break;
        }
    }
    return out;
}

// Encodes working texels to RGBA8, and canonicalises `work` in place (normal maps are
// renormalised) so the next level is filtered from what this level stores.
void from_working(std::vector<f32>& work, u8* out, Space space) {
    const usize count = work.size() / 4;
    for (usize i = 0; i < count; ++i) {
        f32* v = work.data() + i * 4;
        u8*  o = out + i * 4;
        switch (space) {
        case Space::SrgbPremultiplied: {
            const f32 a = std::clamp(v[3], 0.0f, 1.0f);
            for (int k = 0; k < 3; ++k) {
                const f32 straight = a > (0.5f / 255.0f) ? v[k] / a : v[k];
                o[k] = to_unorm8(linear_to_srgb(straight));
            }
            o[3] = to_unorm8(a);
            break;
        }
        case Space::NormalVector:
            normalize_normal(v);
            for (int k = 0; k < 3; ++k) o[k] = to_unorm8(v[k] * 0.5f + 0.5f);
            o[3] = to_unorm8(v[3]);
            break;
        case Space::Raw:
            for (int k = 0; k < 4; ++k) o[k] = to_unorm8(v[k]);
            break;
        }
    }
}

Error invalid(String msg) { return Error{ ErrorCode::InvalidArgument, std::move(msg) }; }

void init_encoders_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        bc7enc_compress_block_init();
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
    });
}

} // namespace

u32 full_mip_count(u32 width, u32 height) noexcept {
    return static_cast<u32>(std::bit_width(std::max({ width, height, 1u })));
}

Result<void> generate_mip_chain(TextureData& t, TextureRole role, MipFilter filter) {
    const bool is_8bit = t.format == TextureFormat::RGBA8_UNORM || t.format == TextureFormat::RGBA8_SRGB;
    if (!is_8bit && t.format != TextureFormat::RGBA32F) {
        return Error{ ErrorCode::Unsupported, "generate_mip_chain: only RGBA8 and RGBA32F textures are supported" };
    }
    if (t.mip_levels != 1) return invalid(std::format("generate_mip_chain: texture already has {} mips", t.mip_levels));
    if (t.width == 0 || t.height == 0 || t.array_layers == 0 || t.pixels.size() != texture_byte_size(t)) {
        return invalid("generate_mip_chain: inconsistent texture size");
    }
    const u32 levels = full_mip_count(t.width, t.height);
    if (levels == 1) return {};

    Space space = Space::Raw;
    if (is_8bit && role == TextureRole::NormalMap) {
        space = Space::NormalVector;
    } else if (t.format == TextureFormat::RGBA8_SRGB) {
        space = Space::SrgbPremultiplied;
    }

    TextureData out = t;
    out.mip_levels = levels;
    out.pixels.assign(static_cast<usize>(texture_byte_size(out)), u8{ 0 });
    std::memcpy(out.pixels.data(), t.pixels.data(), t.pixels.size()); // mip 0 verbatim

    const u64 layer0_bytes = texture_layer_byte_size(t.format, t.width, t.height, 0);
    for (u32 layer = 0; layer < t.array_layers; ++layer) {
        const u8* src0 = t.pixels.data() + layer * layer0_bytes;
        const usize count0 = static_cast<usize>(t.width) * t.height;
        std::vector<f32> work;
        if (is_8bit) {
            work = to_working(src0, count0, space);
        } else {
            work.resize(count0 * 4);
            std::memcpy(work.data(), src0, count0 * 4 * sizeof(f32));
        }
        u32 w = t.width, h = t.height;
        for (u32 mip = 1; mip < levels; ++mip) {
            const u32 nw = std::max(1u, w >> 1);
            const u32 nh = std::max(1u, h >> 1);
            work = resample(work, w, h, nw, nh, filter);
            const u64 layer_bytes = texture_layer_byte_size(out.format, out.width, out.height, mip);
            u8* dst = out.pixels.data() + texture_mip_offset(out, mip) + layer * layer_bytes;
            if (is_8bit) {
                from_working(work, dst, space);
            } else {
                for (usize i = 0; i < work.size(); ++i) {
                    if ((i & 3u) != 3u) work[i] = std::max(work[i], 0.0f); // no negative radiance
                }
                std::memcpy(dst, work.data(), work.size() * sizeof(f32));
            }
            w = nw;
            h = nh;
        }
    }
    t = std::move(out);
    return {};
}

Result<void> compress_texture(TextureData& t, TextureRole role) {
    if (t.format != TextureFormat::RGBA8_UNORM && t.format != TextureFormat::RGBA8_SRGB) {
        return Error{ ErrorCode::Unsupported, "compress_texture: input must be RGBA8" };
    }
    if (t.width == 0 || t.height == 0 || t.array_layers == 0 || t.mip_levels == 0 ||
        t.pixels.size() != texture_byte_size(t)) {
        return invalid("compress_texture: inconsistent texture size");
    }
    init_encoders_once();

    TextureData out = t;
    if (role == TextureRole::NormalMap) {
        out.format = TextureFormat::BC5_UNORM;
    } else {
        out.format = t.format == TextureFormat::RGBA8_SRGB ? TextureFormat::BC7_SRGB : TextureFormat::BC7_UNORM;
    }
    out.pixels.assign(static_cast<usize>(texture_byte_size(out)), u8{ 0 });

    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params); // perceptual weights (sRGB colour)
    if (out.format != TextureFormat::BC7_SRGB) bc7enc_compress_block_params_init_linear_weights(&params);

    for (u32 mip = 0; mip < t.mip_levels; ++mip) {
        const u32 w = std::max(1u, t.width >> mip);
        const u32 h = std::max(1u, t.height >> mip);
        const u32 bw = (w + 3) / 4;
        const u32 bh = (h + 3) / 4;
        const u64 src_layer = texture_layer_byte_size(t.format, t.width, t.height, mip);
        const u64 dst_layer = texture_layer_byte_size(out.format, out.width, out.height, mip);
        for (u32 layer = 0; layer < t.array_layers; ++layer) {
            const u8* src = t.pixels.data() + texture_mip_offset(t, mip) + layer * src_layer;
            u8*       dst = out.pixels.data() + texture_mip_offset(out, mip) + layer * dst_layer;
            for (u32 by = 0; by < bh; ++by) {
                for (u32 bx = 0; bx < bw; ++bx) {
                    u8 block[16 * 4];
                    for (u32 py = 0; py < 4; ++py) {
                        const u32 sy = std::min(by * 4 + py, h - 1);
                        for (u32 px = 0; px < 4; ++px) {
                            const u32 sx = std::min(bx * 4 + px, w - 1);
                            std::memcpy(block + (py * 4 + px) * 4, src + (static_cast<usize>(sy) * w + sx) * 4, 4);
                        }
                    }
                    u8* out_block = dst + (static_cast<usize>(by) * bw + bx) * 16;
                    if (out.format == TextureFormat::BC5_UNORM) {
                        rgbcx::encode_bc5_hq(out_block, block, 0, 1, 4);
                    } else {
                        bc7enc_compress_block(out_block, block, &params);
                    }
                }
            }
        }
    }
    t = std::move(out);
    return {};
}

Result<TextureData> decompress_texture(const TextureData& t) {
    if (!is_block_compressed(t.format)) return t;
    if (t.width == 0 || t.height == 0 || t.array_layers == 0 || t.mip_levels == 0 ||
        t.pixels.size() != texture_byte_size(t)) {
        return invalid("decompress_texture: inconsistent texture size");
    }
    TextureData out = t;
    out.format = t.format == TextureFormat::BC7_SRGB ? TextureFormat::RGBA8_SRGB : TextureFormat::RGBA8_UNORM;
    out.pixels.assign(static_cast<usize>(texture_byte_size(out)), u8{ 0 });

    for (u32 mip = 0; mip < t.mip_levels; ++mip) {
        const u32 w = std::max(1u, t.width >> mip);
        const u32 h = std::max(1u, t.height >> mip);
        const u32 bw = (w + 3) / 4;
        const u32 bh = (h + 3) / 4;
        const u64 src_layer = texture_layer_byte_size(t.format, t.width, t.height, mip);
        const u64 dst_layer = texture_layer_byte_size(out.format, out.width, out.height, mip);
        for (u32 layer = 0; layer < t.array_layers; ++layer) {
            const u8* src = t.pixels.data() + texture_mip_offset(t, mip) + layer * src_layer;
            u8*       dst = out.pixels.data() + texture_mip_offset(out, mip) + layer * dst_layer;
            for (u32 by = 0; by < bh; ++by) {
                for (u32 bx = 0; bx < bw; ++bx) {
                    const u8* in_block = src + (static_cast<usize>(by) * bw + bx) * 16;
                    u8        block[16 * 4] = {};
                    if (t.format == TextureFormat::BC5_UNORM) {
                        rgbcx::unpack_bc5(in_block, block, 0, 1, 4);
                        for (u32 i = 0; i < 16; ++i) {
                            const f32 x = static_cast<f32>(block[i * 4]) / 255.0f * 2.0f - 1.0f;
                            const f32 y = static_cast<f32>(block[i * 4 + 1]) / 255.0f * 2.0f - 1.0f;
                            const f32 z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
                            block[i * 4 + 2] = to_unorm8(z * 0.5f + 0.5f);
                            block[i * 4 + 3] = 255;
                        }
                    } else {
                        bc7decomp::color_rgba px[16];
                        bc7decomp::unpack_bc7(in_block, px);
                        std::memcpy(block, px, sizeof(block));
                    }
                    for (u32 py = 0; py < 4; ++py) {
                        const u32 y = by * 4 + py;
                        if (y >= h) break;
                        for (u32 pxi = 0; pxi < 4; ++pxi) {
                            const u32 x = bx * 4 + pxi;
                            if (x >= w) break;
                            std::memcpy(dst + (static_cast<usize>(y) * w + x) * 4, block + (py * 4 + pxi) * 4, 4);
                        }
                    }
                }
            }
        }
    }
    return out;
}

Result<void> cook_texture(TextureData& t, TextureRole role, const TextureCookOptions& options) {
    if (t.format != TextureFormat::RGBA8_UNORM && t.format != TextureFormat::RGBA8_SRGB) return {};
    if ((options.generate_mips || options.compress) && t.mip_levels == 1) {
        if (auto r = generate_mip_chain(t, role, options.filter); !r) return r;
    }
    if (options.compress) return compress_texture(t, role);
    return {};
}

} // namespace aether::assets
