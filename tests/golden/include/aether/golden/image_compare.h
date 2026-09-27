// aether/golden/image_compare.h — golden-image comparison (render regression tests).
//
// Images are tightly packed RGBA8 (display-encoded, as read back from the final render target).
// Two images match when (a) they have the same size, (b) at most `max_bad_fraction` of the
// pixels have any channel differing by more than `channel_tolerance`, and (c) the mean absolute
// channel error stays below `max_mean_error` (0..255 scale). The tolerances absorb harmless
// rasterisation / floating-point noise while catching real shading changes.
//
// References are per DEVICE CLASS (device_class()): different GPUs and drivers legitimately
// produce different images, so a reference is only meaningful for the class that recorded it.
//
// Thread-affinity: none (pure functions).
#pragma once

#include "aether/core/error.h"
#include "aether/core/types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace aether::golden {

struct Image {
    u32             width  = 0;
    u32             height = 0;
    std::vector<u8> rgba8; // width * height * 4

    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0 && rgba8.size() == u64(width) * height * 4; }
};

struct CompareOptions {
    u32 channel_tolerance = 4;     // per-channel difference that counts as "the same"
    f64 max_bad_fraction  = 0.002; // fraction of pixels allowed beyond the tolerance
    f64 max_mean_error    = 0.75;  // mean |a-b| over all channels (0..255)
};

struct CompareResult {
    bool        passed           = false;
    bool        size_mismatch    = false;
    u32         max_channel_diff = 0;
    f64         mean_abs_error   = 0.0;
    u64         bad_pixels       = 0;
    f64         bad_fraction     = 0.0;
    f64         psnr_db          = 0.0; // +inf for identical images
    std::string summary;                // one line, for logs
};

[[nodiscard]] CompareResult compare_images(const Image& actual, const Image& reference,
                                           const CompareOptions& options = {});

// Visualises the difference: per-pixel max channel difference amplified x8 in grey, pixels
// beyond `channel_tolerance` in red. Empty image on a size mismatch.
[[nodiscard]] Image make_diff_image(const Image& actual, const Image& reference, u32 channel_tolerance = 4);

[[nodiscard]] Result<Image> load_png(const std::filesystem::path& file);
[[nodiscard]] Result<void>  save_png(const std::filesystem::path& file, const Image& image);

// A stable, file-name-safe key for the adapter: "llvmpipe", "lavapipe", "swiftshader", else
// the adapter name lower-cased with runs of non-alphanumerics replaced by '_'.
[[nodiscard]] std::string device_class(std::string_view adapter_name);

} // namespace aether::golden
