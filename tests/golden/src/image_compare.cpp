// image_compare.cpp — see aether/golden/image_compare.h.
#include "aether/golden/image_compare.h"

#include "aether/core/paths.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <limits>

// Decoding: stb_image's implementation lives in aether.assets (image_importer.cpp, STBI_NO_STDIO).
#define STBI_NO_STDIO
#include <stb_image.h>
// Encoding: private to this TU.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace aether::golden {

CompareResult compare_images(const Image& a, const Image& b, const CompareOptions& o) {
    CompareResult r;
    if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height) {
        r.size_mismatch = true;
        r.summary       = std::format("size mismatch: {}x{} vs reference {}x{}", a.width, a.height, b.width, b.height);
        return r;
    }
    const u64 pixels = u64(a.width) * a.height;
    u64       sum    = 0;
    f64       sq_sum = 0.0;
    for (u64 p = 0; p < pixels; ++p) {
        u32 pixel_max = 0;
        for (u32 c = 0; c < 4; ++c) {
            const u32 d = static_cast<u32>(std::abs(int(a.rgba8[p * 4 + c]) - int(b.rgba8[p * 4 + c])));
            pixel_max = std::max(pixel_max, d);
            sum += d;
            sq_sum += f64(d) * d;
        }
        r.max_channel_diff = std::max(r.max_channel_diff, pixel_max);
        if (pixel_max > o.channel_tolerance) {
            ++r.bad_pixels;
        }
    }
    const f64 samples = f64(pixels) * 4.0;
    r.mean_abs_error  = f64(sum) / samples;
    r.bad_fraction    = f64(r.bad_pixels) / f64(pixels);
    const f64 mse     = sq_sum / samples;
    r.psnr_db         = mse == 0.0 ? std::numeric_limits<f64>::infinity() : 10.0 * std::log10(255.0 * 255.0 / mse);
    r.passed          = r.bad_fraction <= o.max_bad_fraction && r.mean_abs_error <= o.max_mean_error;
    r.summary = std::format("{}: max diff {}, mean {:.3f}, {} pixels ({:.3f}%) beyond {}, PSNR {:.1f} dB",
                            r.passed ? "match" : "MISMATCH", r.max_channel_diff, r.mean_abs_error, r.bad_pixels,
                            r.bad_fraction * 100.0, o.channel_tolerance, r.psnr_db);
    return r;
}

Image make_diff_image(const Image& a, const Image& b, u32 tolerance) {
    Image out;
    if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height) {
        return out;
    }
    out.width  = a.width;
    out.height = a.height;
    out.rgba8.resize(a.rgba8.size());
    for (u64 p = 0; p < u64(a.width) * a.height; ++p) {
        u32 m = 0;
        for (u32 c = 0; c < 4; ++c) {
            m = std::max(m, static_cast<u32>(std::abs(int(a.rgba8[p * 4 + c]) - int(b.rgba8[p * 4 + c]))));
        }
        u8* px = &out.rgba8[p * 4];
        if (m > tolerance) {
            px[0] = 255, px[1] = 0, px[2] = 0;
        } else {
            px[0] = px[1] = px[2] = static_cast<u8>(std::min<u32>(m * 8, 255));
        }
        px[3] = 255;
    }
    return out;
}

Result<Image> load_png(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return Error{ ErrorCode::NotFound, std::format("no such image: {}", file.generic_string()) };
    }
    const std::vector<byte> bytes = paths::read_file(file);
    int w = 0, h = 0, n = 0;
    stbi_uc* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()),
                                          &w, &h, &n, 4);
    if (data == nullptr) {
        return Error{ ErrorCode::IoError, std::format("cannot decode {}: {}", file.generic_string(), stbi_failure_reason()) };
    }
    Image img;
    img.width  = static_cast<u32>(w);
    img.height = static_cast<u32>(h);
    img.rgba8.assign(data, data + u64(w) * h * 4);
    stbi_image_free(data);
    return img;
}

Result<void> save_png(const std::filesystem::path& file, const Image& image) {
    if (!image.valid()) {
        return Error{ ErrorCode::InvalidArgument, "save_png: invalid image" };
    }
    std::error_code ec;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    int         len = 0;
    stbi_uc*    png = stbi_write_png_to_mem(image.rgba8.data(), static_cast<int>(image.width * 4),
                                            static_cast<int>(image.width), static_cast<int>(image.height), 4, &len);
    if (png == nullptr) {
        return Error{ ErrorCode::Internal, "save_png: encoding failed" };
    }
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(png), len);
    STBIW_FREE(png);
    if (!f) {
        return Error{ ErrorCode::IoError, std::format("cannot write {}", file.generic_string()) };
    }
    return {};
}

std::string device_class(std::string_view adapter) {
    std::string lower(adapter);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* known : { "llvmpipe", "lavapipe", "swiftshader" }) {
        if (lower.find(known) != std::string::npos) {
            return known;
        }
    }
    std::string out;
    for (const char c : lower) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(c);
        } else if (!out.empty() && out.back() != '_') {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "unknown" : out;
}

} // namespace aether::golden
