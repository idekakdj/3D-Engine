// aether/rhi/device_ext.h — runtime device controls not covered by device.h (additive).
//
// Main thread only. All functions take the Device returned by rhi::create_device().
#pragma once

#include "aether/core/types.h"
#include "aether/rhi/device.h"

#include <string>

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

} // namespace aether::rhi
