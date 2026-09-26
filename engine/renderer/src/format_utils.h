// format_utils.h — small rhi::Format / ResourceState helpers used across the renderer.
// Private header. Pure functions; thread-safe.
#pragma once

#include "aether/core/types.h"
#include "aether/rhi/enums.h"

#include <bit>

namespace aether::renderer {

[[nodiscard]] constexpr bool is_depth_format(rhi::Format f) noexcept {
    return f == rhi::Format::D32F || f == rhi::Format::D24UnormS8 || f == rhi::Format::D32FS8;
}

// ADR-0002 encoding rule: *Unorm targets get the sRGB OETF in-shader, *Srgb targets are
// encoded by the hardware, everything else (float) receives linear values.
[[nodiscard]] constexpr bool is_unorm_color_format(rhi::Format f) noexcept {
    switch (f) {
    case rhi::Format::R8Unorm:
    case rhi::Format::RG8Unorm:
    case rhi::Format::RGBA8Unorm:
    case rhi::Format::BGRA8Unorm:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr bool is_srgb_format(rhi::Format f) noexcept {
    switch (f) {
    case rhi::Format::RGBA8Srgb:
    case rhi::Format::BGRA8Srgb:
    case rhi::Format::BC1Srgb:
    case rhi::Format::BC3Srgb:
    case rhi::Format::BC7Srgb:
        return true;
    default:
        return false;
    }
}

// Bytes per texel for uncompressed formats (0 for block-compressed / undefined).
[[nodiscard]] constexpr u32 bytes_per_texel(rhi::Format f) noexcept {
    switch (f) {
    case rhi::Format::R8Unorm: return 1;
    case rhi::Format::RG8Unorm: return 2;
    case rhi::Format::RGBA8Unorm:
    case rhi::Format::RGBA8Srgb:
    case rhi::Format::BGRA8Unorm:
    case rhi::Format::BGRA8Srgb: return 4;
    case rhi::Format::R16F: return 2;
    case rhi::Format::RG16F: return 4;
    case rhi::Format::RGBA16F: return 8;
    case rhi::Format::R32F: return 4;
    case rhi::Format::RG32F: return 8;
    case rhi::Format::RGB32F: return 12;
    case rhi::Format::RGBA32F: return 16;
    case rhi::Format::R32Uint: return 4;
    case rhi::Format::RG32Uint: return 8;
    case rhi::Format::RGBA32Uint: return 16;
    case rhi::Format::D32F: return 4;
    case rhi::Format::D24UnormS8: return 4;
    case rhi::Format::D32FS8: return 8;
    default: return 0;
    }
}

// Full mip chain length for a 2D extent.
[[nodiscard]] constexpr u32 full_mip_count(u32 width, u32 height) noexcept {
    const u32 m = width > height ? width : height;
    return m == 0 ? 1u : static_cast<u32>(std::bit_width(m));
}

// States that imply a write by the GPU (used for hazard detection in the render graph).
[[nodiscard]] constexpr bool is_write_state(rhi::ResourceState s) noexcept {
    switch (s) {
    case rhi::ResourceState::General:
    case rhi::ResourceState::ColorAttachment:
    case rhi::ResourceState::DepthStencilAttachment:
    case rhi::ResourceState::ShaderWrite:
    case rhi::ResourceState::TransferDst:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr const char* state_name(rhi::ResourceState s) noexcept {
    switch (s) {
    case rhi::ResourceState::Undefined: return "Undefined";
    case rhi::ResourceState::General: return "General";
    case rhi::ResourceState::ColorAttachment: return "ColorAttachment";
    case rhi::ResourceState::DepthStencilAttachment: return "DepthStencilAttachment";
    case rhi::ResourceState::DepthStencilRead: return "DepthStencilRead";
    case rhi::ResourceState::ShaderRead: return "ShaderRead";
    case rhi::ResourceState::ShaderWrite: return "ShaderWrite";
    case rhi::ResourceState::TransferSrc: return "TransferSrc";
    case rhi::ResourceState::TransferDst: return "TransferDst";
    case rhi::ResourceState::Present: return "Present";
    case rhi::ResourceState::IndirectArgument: return "IndirectArgument";
    }
    return "?";
}

// IEEE 754 binary32 -> binary16 with round-to-nearest-even; handles inf/NaN/denormals.
[[nodiscard]] inline u16 f32_to_f16(f32 value) noexcept {
    const u32 x = std::bit_cast<u32>(value);
    const u32 sign = (x >> 16) & 0x8000u;
    const u32 exp = (x >> 23) & 0xFFu;
    u32 mant = x & 0x007F'FFFFu;

    if (exp == 0xFFu) { // inf / NaN
        return static_cast<u16>(sign | 0x7C00u | (mant != 0 ? 0x0200u : 0u));
    }
    const i32 e = static_cast<i32>(exp) - 127 + 15;
    if (e >= 0x1F) { // overflow -> inf
        return static_cast<u16>(sign | 0x7C00u);
    }
    if (e <= 0) { // subnormal half or zero
        if (e < -10) {
            return static_cast<u16>(sign);
        }
        mant |= 0x0080'0000u;
        const u32 shift = static_cast<u32>(14 - e);
        u32 half_mant = mant >> shift;
        const u32 rem = mant & ((1u << shift) - 1u);
        const u32 halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (half_mant & 1u) != 0)) {
            ++half_mant;
        }
        return static_cast<u16>(sign | half_mant);
    }
    u32 half = sign | (static_cast<u32>(e) << 10) | (mant >> 13);
    const u32 rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u) != 0)) {
        ++half; // may carry into the exponent, which is the correct rounding behaviour
    }
    return static_cast<u16>(half);
}

} // namespace aether::renderer
