// aether/editor/thumbnail.h — GPU-free pieces of the asset-browser thumbnails (unit-tested).
//
// The editor's ThumbnailCache (src/thumbnail_cache.*) produces one small RGBA8 texture per asset:
//   * images (.png/.jpg/.tga/.bmp/.hdr) are decoded on the CPU and area-downscaled here (exact
//     colours; HDR is tonemapped for display);
//   * models, prefabs and scenes are instantiated into a scratch World and rendered offscreen by a
//     dedicated thumbnail renderer, with a camera framed by frame_bounds().
// Thread-affinity: pure functions.
#pragma once

#include "aether/core/geometry.h"
#include "aether/core/math.h"
#include "aether/core/types.h"

#include <filesystem>
#include <vector>

namespace aether::editor {

enum class ThumbnailKind : u8 {
    None = 0, // no thumbnail (scripts, data, folders): the browser shows the kind label
    Image,    // CPU-decoded
    Model,    // .gltf / .glb
    Prefab,   // .aeprefab
    Scene,    // .aescene
};

[[nodiscard]] ThumbnailKind thumbnail_kind(const std::filesystem::path& file);
[[nodiscard]] constexpr bool thumbnail_is_rendered(ThumbnailKind k) noexcept {
    return k == ThumbnailKind::Model || k == ThumbnailKind::Prefab || k == ThumbnailKind::Scene;
}

// Tightly packed RGBA8 image.
struct ThumbnailImage {
    u32             width  = 0;
    u32             height = 0;
    std::vector<u8> rgba8;
    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0 && rgba8.size() == u64(width) * height * 4; }
};

// Fits `src` (w x h RGBA8, straight alpha) into a size x size square, preserving the aspect ratio
// (letterbox pixels are transparent black). Box-filters over the exact source footprint of each
// destination pixel (area average, alpha-weighted colour), so downscaling never aliases; upscaling
// small images uses nearest-neighbour (crisp pixel art).
[[nodiscard]] ThumbnailImage fit_thumbnail(const u8* src, u32 w, u32 h, u32 size);

// HDR (linear RGBA32F) -> display RGBA8: exposure so the mean luminance maps to ~0.18, Reinhard,
// sRGB OETF. Alpha is kept.
[[nodiscard]] std::vector<u8> hdr_to_display(const f32* rgba, u32 w, u32 h);

// Camera for a thumbnail of `bounds`: looks at the centre from a fixed 3/4 view above-front-right,
// far enough that the bounding sphere fits the vertical field of view (square image) with
// `margin` (1.1 = 10% border). near/far enclose the sphere.
struct ThumbnailCamera {
    Mat4 view{ 1.0f };
    Vec3 position{ 0.0f };
    f32  near_z = 0.1f;
    f32  far_z  = 100.0f;
};
[[nodiscard]] ThumbnailCamera frame_bounds(const AABB& bounds, f32 fov_y_radians, f32 margin = 1.1f);

} // namespace aether::editor
