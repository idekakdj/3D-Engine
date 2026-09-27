// test_image_compare.cpp — the golden-image comparison library (no GPU).
#include "aether/golden/image_compare.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>

using namespace aether;
using namespace aether::golden;

namespace {

Image gradient(u32 w, u32 h) {
    Image img{ w, h, std::vector<u8>(u64(w) * h * 4) };
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u8* p = &img.rgba8[(u64(y) * w + x) * 4];
            p[0] = u8(x * 255 / (w - 1));
            p[1] = u8(y * 255 / (h - 1));
            p[2] = 128;
            p[3] = 255;
        }
    }
    return img;
}

} // namespace

TEST_CASE("golden: identical images match with infinite PSNR") {
    const Image a = gradient(64, 32);
    const CompareResult r = compare_images(a, a);
    CHECK(r.passed);
    CHECK(r.max_channel_diff == 0);
    CHECK(r.bad_pixels == 0);
    CHECK(r.mean_abs_error == 0.0);
    CHECK(std::isinf(r.psnr_db));
}

TEST_CASE("golden: small noise within tolerance passes, localized change fails") {
    const Image ref = gradient(64, 64);
    Image       noisy = ref;
    for (u64 i = 0; i < noisy.rgba8.size(); i += 7) {
        noisy.rgba8[i] = u8(std::min(255, noisy.rgba8[i] + 2));
    }
    const CompareResult ok = compare_images(noisy, ref);
    CHECK(ok.passed);
    CHECK(ok.max_channel_diff == 2);
    CHECK(ok.bad_pixels == 0);
    CHECK(ok.psnr_db > 40.0);

    Image changed = ref;
    for (u32 y = 10; y < 20; ++y) { // a 10x10 block turns white: 100 / 4096 pixels = 2.4%
        for (u32 x = 10; x < 20; ++x) {
            u8* p = &changed.rgba8[(u64(y) * 64 + x) * 4];
            p[0] = p[1] = p[2] = 255;
        }
    }
    const CompareResult bad = compare_images(changed, ref);
    CHECK_FALSE(bad.passed);
    CHECK(bad.bad_pixels == 100);
    CHECK(bad.bad_fraction == doctest::Approx(100.0 / 4096.0));
    CHECK(bad.summary.find("MISMATCH") != std::string::npos);

    // A single deviating pixel is tolerated by the bad-pixel budget only if the mean error is low.
    CompareOptions strict;
    strict.max_bad_fraction = 0.0;
    Image one = ref;
    one.rgba8[0] = u8(one.rgba8[0] ^ 0xFF);
    CHECK_FALSE(compare_images(one, ref, strict).passed);
    CHECK(compare_images(one, ref).passed);
}

TEST_CASE("golden: a global brightness shift fails on mean error") {
    const Image ref = gradient(32, 32);
    Image       shifted = ref;
    for (u64 i = 0; i < shifted.rgba8.size(); ++i) {
        if (i % 4 != 3) shifted.rgba8[i] = u8(std::min(255, shifted.rgba8[i] + 3)); // within tolerance per pixel
    }
    const CompareResult r = compare_images(shifted, ref);
    CHECK(r.bad_pixels == 0);
    CHECK(r.mean_abs_error > 0.75);
    CHECK_FALSE(r.passed);
}

TEST_CASE("golden: size mismatch and invalid images fail") {
    CHECK(compare_images(gradient(8, 8), gradient(8, 9)).size_mismatch);
    CHECK_FALSE(compare_images(gradient(8, 8), gradient(8, 9)).passed);
    CHECK_FALSE(compare_images(Image{}, gradient(8, 8)).passed);
    CHECK_FALSE(make_diff_image(gradient(8, 8), gradient(4, 4)).valid());
}

TEST_CASE("golden: diff image marks pixels beyond tolerance in red") {
    const Image ref = gradient(16, 16);
    Image       act = ref;
    act.rgba8[0]    = u8(act.rgba8[0] + 100); // pixel 0: way off
    act.rgba8[4]    = u8(act.rgba8[4] + 2);   // pixel 1: within tolerance
    const Image d   = make_diff_image(act, ref);
    REQUIRE(d.valid());
    CHECK(d.rgba8[0] == 255);
    CHECK(d.rgba8[1] == 0);
    CHECK(d.rgba8[4] == 16); // 2 * 8, grey
    CHECK(d.rgba8[5] == 16);
    CHECK(d.rgba8[8] == 0);  // untouched pixel
}

TEST_CASE("golden: PNG round trip is lossless") {
    const auto path = std::filesystem::temp_directory_path() /
                      ("aether_golden_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                       "/img.png");
    const Image a = gradient(37, 21);
    REQUIRE(save_png(path, a).has_value());
    auto b = load_png(path);
    REQUIRE_MESSAGE(b.has_value(), b.error().message);
    CHECK(b->width == 37);
    CHECK(b->height == 21);
    CHECK(b->rgba8 == a.rgba8);
    CHECK(load_png(path.parent_path() / "missing.png").error().code == ErrorCode::NotFound);
    CHECK_FALSE(save_png(path, Image{}).has_value());
    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
}

TEST_CASE("golden: device classes are stable file-name keys") {
    CHECK(device_class("llvmpipe (LLVM 20.1.2, 256 bits)") == "llvmpipe");
    CHECK(device_class("Google SwiftShader Device") == "swiftshader");
    CHECK(device_class("Intel(R) Arc(TM) A770 Graphics") == "intel_r_arc_tm_a770_graphics");
    CHECK(device_class("NVIDIA GeForce RTX 4090") == "nvidia_geforce_rtx_4090");
    CHECK(device_class("") == "unknown");
    CHECK(device_class("---") == "unknown");
}
