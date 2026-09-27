// test_thumbnail.cpp — GPU-free thumbnail helpers (kinds, downscaling, HDR, camera framing).
#include "aether/editor/thumbnail.h"

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using namespace aether;
using namespace aether::editor;

namespace {
std::vector<u8> solid(u32 w, u32 h, u8 r, u8 g, u8 b, u8 a = 255) {
    std::vector<u8> px(u64(w) * h * 4);
    for (u64 i = 0; i < u64(w) * h; ++i) {
        px[i * 4] = r, px[i * 4 + 1] = g, px[i * 4 + 2] = b, px[i * 4 + 3] = a;
    }
    return px;
}
const u8* at(const ThumbnailImage& img, u32 x, u32 y) { return &img.rgba8[(u64(y) * img.width + x) * 4]; }
} // namespace

TEST_CASE("thumbnail: kinds by extension") {
    CHECK(thumbnail_kind("a/b.png") == ThumbnailKind::Image);
    CHECK(thumbnail_kind("x.JPG") == ThumbnailKind::Image);
    CHECK(thumbnail_kind("sky.hdr") == ThumbnailKind::Image);
    CHECK(thumbnail_kind("m.gltf") == ThumbnailKind::Model);
    CHECK(thumbnail_kind("m.GLB") == ThumbnailKind::Model);
    CHECK(thumbnail_kind("p.aeprefab") == ThumbnailKind::Prefab);
    CHECK(thumbnail_kind("s.aescene") == ThumbnailKind::Scene);
    CHECK(thumbnail_kind("script.lua") == ThumbnailKind::None);
    CHECK(thumbnail_kind("folder") == ThumbnailKind::None);
    CHECK(thumbnail_is_rendered(ThumbnailKind::Model));
    CHECK(thumbnail_is_rendered(ThumbnailKind::Scene));
    CHECK_FALSE(thumbnail_is_rendered(ThumbnailKind::Image));
}

TEST_CASE("thumbnail: area downscale averages exactly") {
    // 4x4 checker of black/white 2x2 blocks... every 2x2 destination footprint holds 2 white + 2 black.
    std::vector<u8> src(4 * 4 * 4);
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u8 v = ((x + y) & 1) ? 255 : 0;
            u8*      p = &src[(y * 4 + x) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    const ThumbnailImage t = fit_thumbnail(src.data(), 4, 4, 2);
    REQUIRE(t.valid());
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 2; ++x) {
            CHECK(at(t, x, y)[0] == 128); // round(127.5)
            CHECK(at(t, x, y)[3] == 255);
        }
    }
    // Non-integer ratio (5 -> 2) still preserves a uniform colour exactly.
    const std::vector<u8> red = solid(5, 5, 200, 10, 30);
    const ThumbnailImage  r   = fit_thumbnail(red.data(), 5, 5, 2);
    CHECK(at(r, 1, 1)[0] == 200);
    CHECK(at(r, 1, 1)[1] == 10);
    CHECK(at(r, 1, 1)[2] == 30);
}

TEST_CASE("thumbnail: transparent texels do not bleed colour") {
    // Left half opaque green, right half fully transparent RED: the average stays pure green.
    std::vector<u8> src(2 * 1 * 4);
    src[0] = 0, src[1] = 255, src[2] = 0, src[3] = 255;
    src[4] = 255, src[5] = 0, src[6] = 0, src[7] = 0;
    std::vector<u8> tall;
    for (int row = 0; row < 2; ++row) tall.insert(tall.end(), src.begin(), src.end()); // 2x2
    const ThumbnailImage t = fit_thumbnail(tall.data(), 2, 2, 1);
    REQUIRE(t.valid());
    CHECK(at(t, 0, 0)[0] == 0);
    CHECK(at(t, 0, 0)[1] == 255);
    CHECK(at(t, 0, 0)[3] == 128); // half coverage
}

TEST_CASE("thumbnail: aspect fit letterboxes with transparency") {
    const std::vector<u8> wide = solid(40, 10, 255, 255, 255);
    const ThumbnailImage  t    = fit_thumbnail(wide.data(), 40, 10, 8); // -> 8x2 centred
    REQUIRE(t.width == 8);
    REQUIRE(t.height == 8);
    CHECK(at(t, 4, 0)[3] == 0);   // above the image
    CHECK(at(t, 4, 7)[3] == 0);   // below
    CHECK(at(t, 4, 3)[3] == 255); // image rows 3..4
    CHECK(at(t, 0, 3)[0] == 255);
    CHECK(at(t, 7, 4)[0] == 255);
}

TEST_CASE("thumbnail: small images upscale with nearest neighbour") {
    std::vector<u8> src = solid(2, 2, 0, 0, 0);
    src[0] = 255; // top-left pixel white
    const ThumbnailImage t = fit_thumbnail(src.data(), 2, 2, 8);
    CHECK(at(t, 0, 0)[0] == 255);
    CHECK(at(t, 3, 3)[0] == 255);
    CHECK(at(t, 4, 4)[0] == 0);
    CHECK(at(t, 7, 0)[0] == 0);
    CHECK_FALSE(fit_thumbnail(nullptr, 2, 2, 8).valid());
    CHECK_FALSE(fit_thumbnail(src.data(), 0, 2, 8).valid());
}

TEST_CASE("thumbnail: HDR tonemap is monotonic, bounded and keeps alpha") {
    const std::vector<f32> px = { 0.0f, 0.0f, 0.0f, 1.0f,  0.5f, 0.5f, 0.5f, 1.0f,
                                  2.0f, 2.0f, 2.0f, 0.5f,  500.0f, 500.0f, 500.0f, 1.0f };
    const std::vector<u8> d = hdr_to_display(px.data(), 4, 1);
    REQUIRE(d.size() == 16);
    CHECK(d[0] == 0);
    CHECK(d[4] < d[8]);
    CHECK(d[8] < d[12]);
    CHECK(d[12] <= 255);
    CHECK(d[11] == 128); // alpha 0.5
}

TEST_CASE("thumbnail: framing keeps every corner of the bounds on screen") {
    const f32 fov = 0.7f;
    for (const AABB& b : { AABB{ Vec3(-1.0f), Vec3(1.0f) }, AABB{ Vec3(10, 0, -3), Vec3(14, 0.2f, 9) },
                           AABB{ Vec3(0.0f), Vec3(0.001f) } }) {
        const ThumbnailCamera cam  = frame_bounds(b, fov);
        const Mat4            proj = glm::perspective(fov, 1.0f, cam.near_z, cam.far_z);
        CHECK(cam.near_z > 0.0f);
        CHECK(cam.far_z > cam.near_z);
        for (int k = 0; k < 8; ++k) {
            const Vec3 c((k & 1) ? b.max.x : b.min.x, (k & 2) ? b.max.y : b.min.y, (k & 4) ? b.max.z : b.min.z);
            const Vec4 clip = proj * cam.view * Vec4(c, 1.0f);
            REQUIRE(clip.w > 0.0f);
            const Vec3 ndc = Vec3(clip) / clip.w;
            CHECK(std::abs(ndc.x) <= 1.0f);
            CHECK(std::abs(ndc.y) <= 1.0f);
            CHECK(ndc.z >= -1.0f); // inside the (GL-style) depth range
            CHECK(ndc.z <= 1.0f);
        }
        // The camera looks at the centre from above.
        CHECK(cam.position.y > (b.min.y + b.max.y) * 0.5f);
    }
    // Invalid bounds fall back to a unit sphere at the origin.
    const ThumbnailCamera def = frame_bounds(AABB{ Vec3(1.0f), Vec3(-1.0f) }, fov);
    CHECK(glm::length(def.position) > 1.0f);
}
