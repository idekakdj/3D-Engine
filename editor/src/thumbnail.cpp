// thumbnail.cpp — see aether/editor/thumbnail.h.
#include "aether/editor/thumbnail.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace aether::editor {

ThumbnailKind thumbnail_kind(const std::filesystem::path& file) {
    std::string e = file.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp" || e == ".hdr") return ThumbnailKind::Image;
    if (e == ".gltf" || e == ".glb") return ThumbnailKind::Model;
    if (e == ".aeprefab") return ThumbnailKind::Prefab;
    if (e == ".aescene") return ThumbnailKind::Scene;
    return ThumbnailKind::None;
}

ThumbnailImage fit_thumbnail(const u8* src, u32 w, u32 h, u32 size) {
    ThumbnailImage out;
    if (src == nullptr || w == 0 || h == 0 || size == 0) {
        return out;
    }
    out.width = out.height = size;
    out.rgba8.assign(u64(size) * size * 4, 0);
    // Destination rectangle of the fitted image inside the square.
    const f64 scale = std::min(f64(size) / w, f64(size) / h);
    const u32 dw = std::max(1u, static_cast<u32>(std::lround(w * scale)));
    const u32 dh = std::max(1u, static_cast<u32>(std::lround(h * scale)));
    const u32 ox = (size - std::min(dw, size)) / 2;
    const u32 oy = (size - std::min(dh, size)) / 2;
    const f64 sx = f64(w) / dw; // source pixels per destination pixel
    const f64 sy = f64(h) / dh;
    for (u32 y = 0; y < dh && oy + y < size; ++y) {
        for (u32 x = 0; x < dw && ox + x < size; ++x) {
            u8* d = &out.rgba8[(u64(oy + y) * size + ox + x) * 4];
            if (sx <= 1.0 && sy <= 1.0) { // magnification: nearest
                const u32 px = std::min(w - 1, static_cast<u32>(x * sx));
                const u32 py = std::min(h - 1, static_cast<u32>(y * sy));
                std::copy_n(&src[(u64(py) * w + px) * 4], 4, d);
                continue;
            }
            // Area average over [x0,x1) x [y0,y1) in source pixels, with fractional edge weights.
            const f64 x0 = x * sx, x1 = (x + 1) * sx, y0 = y * sy, y1 = (y + 1) * sy;
            f64 acc[4] = { 0, 0, 0, 0 };
            f64 area = 0.0;
            for (u32 py = static_cast<u32>(y0); py < std::min<f64>(h, std::ceil(y1)); ++py) {
                const f64 wy = std::min<f64>(py + 1, y1) - std::max<f64>(py, y0);
                for (u32 px = static_cast<u32>(x0); px < std::min<f64>(w, std::ceil(x1)); ++px) {
                    const f64 wx = std::min<f64>(px + 1, x1) - std::max<f64>(px, x0);
                    const u8* s = &src[(u64(py) * w + px) * 4];
                    const f64 wgt = wx * wy;
                    const f64 a = s[3] / 255.0;
                    acc[0] += s[0] * a * wgt; // alpha-weighted colour: transparent texels do not bleed
                    acc[1] += s[1] * a * wgt;
                    acc[2] += s[2] * a * wgt;
                    acc[3] += a * wgt;
                    area += wgt;
                }
            }
            const f64 alpha = area > 0.0 ? acc[3] / area : 0.0;
            for (int c = 0; c < 3; ++c) {
                d[c] = acc[3] > 0.0 ? static_cast<u8>(std::clamp(std::lround(acc[c] / acc[3]), 0L, 255L)) : 0;
            }
            d[3] = static_cast<u8>(std::clamp(std::lround(alpha * 255.0), 0L, 255L));
        }
    }
    return out;
}

std::vector<u8> hdr_to_display(const f32* rgba, u32 w, u32 h) {
    const u64 n = u64(w) * h;
    std::vector<u8> out(n * 4);
    f64 log_sum = 0.0;
    for (u64 i = 0; i < n; ++i) {
        const f64 l = 0.2126 * rgba[i * 4] + 0.7152 * rgba[i * 4 + 1] + 0.0722 * rgba[i * 4 + 2];
        log_sum += std::log(1e-4 + std::max(0.0, l));
    }
    const f64 avg = n > 0 ? std::exp(log_sum / static_cast<f64>(n)) : 1.0; // geometric mean luminance
    const f64 exposure = 0.18 / std::max(avg, 1e-4);
    auto encode = [](f64 v) {
        v = std::clamp(v, 0.0, 1.0);
        const f64 s = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
        return static_cast<u8>(std::lround(s * 255.0));
    };
    for (u64 i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            const f64 v = std::max(0.0, f64(rgba[i * 4 + c])) * exposure;
            out[i * 4 + c] = encode(v / (1.0 + v));
        }
        out[i * 4 + 3] = static_cast<u8>(std::lround(std::clamp(f64(rgba[i * 4 + 3]), 0.0, 1.0) * 255.0));
    }
    return out;
}

ThumbnailCamera frame_bounds(const AABB& bounds, f32 fov_y_radians, f32 margin) {
    ThumbnailCamera cam;
    Vec3 center(0.0f);
    f32  radius = 1.0f;
    if (bounds.valid()) {
        center = (bounds.min + bounds.max) * 0.5f;
        radius = std::max(glm::length(bounds.max - bounds.min) * 0.5f, 1e-3f);
    }
    const Vec3 dir = glm::normalize(Vec3(1.0f, 0.75f, 1.35f)); // from the centre towards the camera
    const f32  dist = radius * margin / std::sin(fov_y_radians * 0.5f);
    cam.position = center + dir * dist;
    cam.view = glm::lookAt(cam.position, center, Vec3(0.0f, 1.0f, 0.0f));
    cam.near_z = std::max(dist - radius * 1.5f, dist * 0.01f);
    cam.far_z = dist + radius * 1.5f;
    return cam;
}

} // namespace aether::editor
