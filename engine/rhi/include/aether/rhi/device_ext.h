// aether/rhi/device_ext.h — runtime device controls not covered by device.h (additive).
//
// Main thread only. All functions take the Device returned by rhi::create_device().
#pragma once

#include "aether/core/types.h"
#include "aether/rhi/device.h"

#include <string>
#include <vector>

namespace aether::rhi {

// Present mode: vsync=true -> FIFO; false -> MAILBOX, else IMMEDIATE, else FIFO.
// Takes effect at the next begin_frame() (the swapchain is recreated).
void               set_vsync(Device& device, bool enabled);
[[nodiscard]] bool vsync(const Device& device);

struct DeviceInfo {
    std::string adapter_name;
    std::string driver_name;     // e.g. "Intel Corporation"
    std::string driver_version;  // vendor-decoded / driver info string
    std::string api_version;     // e.g. "1.4.356"
    std::string present_mode;    // current swapchain present mode
    u32         swapchain_images = 0;
    u64         device_local_bytes = 0;
};
[[nodiscard]] DeviceInfo device_info(const Device& device);

// GPU -> CPU readback (golden-image tests, screenshots). Copies mip 0 / layer 0 of an 8-bit,
// 4-channel colour texture (RGBA8/BGRA8, Unorm or Srgb) into tightly packed RGBA8 bytes (BGRA
// is swizzled; values are returned as stored, i.e. still display-encoded for Srgb/OETF'd
// targets). The texture needs TextureUsage::TransferSrc and must be in `state`, which it is
// returned to. Blocks until the GPU finished (immediate_submit): call it outside
// begin_frame()/end_frame(), after the work that wrote the texture was submitted.
struct TextureReadback {
    u32             width  = 0;
    u32             height = 0;
    std::vector<u8> rgba8; // width * height * 4
};
[[nodiscard]] Result<TextureReadback> read_texture_rgba8(Device& device, TextureHandle texture,
                                                         ResourceState state);

} // namespace aether::rhi
