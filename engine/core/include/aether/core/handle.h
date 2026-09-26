// aether/core/handle.h — generational handles and the stable AssetId.
//
// FROZEN CONTRACT (ADR-0001). Handles replace raw pointers for engine resources so
// storage can relocate and use-after-free is detectable via the generation field.
// AssetId is the stable 128-bit GUID keying the asset database.
#pragma once

#include "aether/core/types.h"

#include <compare>
#include <functional>

namespace aether {

// A typed, generational handle. `T` is a phantom tag (e.g. struct Mesh;) so handles
// to different resource kinds are not interchangeable. 24 bits index / 8 bits gen by
// default is plenty for pools; widen via the template params if needed.
template <typename T, typename Index = u32, u32 IndexBits = 24>
struct Handle {
    static constexpr Index kIndexMask = (Index{1} << IndexBits) - 1;
    static constexpr Index kInvalid   = kIndexMask;

    Index value = kInvalid; // low IndexBits = slot index, high bits = generation

    constexpr Handle() = default;
    constexpr Handle(Index index, Index generation)
        : value(static_cast<Index>((index & kIndexMask) |
                                   (generation << IndexBits))) {}

    [[nodiscard]] constexpr Index index() const noexcept { return value & kIndexMask; }
    [[nodiscard]] constexpr Index generation() const noexcept { return value >> IndexBits; }
    [[nodiscard]] constexpr bool  is_valid() const noexcept { return index() != kInvalid; }

    constexpr explicit operator bool() const noexcept { return is_valid(); }
    friend constexpr bool operator==(Handle, Handle) = default;
};

// Stable content identity. Generated from source-asset path + import settings hash.
struct AssetId {
    u64 hi = 0;
    u64 lo = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return (hi | lo) != 0; }
    constexpr explicit operator bool() const noexcept { return is_valid(); }
    friend constexpr auto operator<=>(const AssetId&, const AssetId&) = default;

    static AssetId from_string(StringView s);      // FNV/xxhash of the string
    [[nodiscard]] String to_string() const;        // 32-hex-char form
};

inline constexpr AssetId kInvalidAsset{};

} // namespace aether

// Hashing so handles/ids can key unordered containers.
template <typename T, typename I, ae::u32 B>
struct std::hash<ae::Handle<T, I, B>> {
    ae::usize operator()(const ae::Handle<T, I, B>& h) const noexcept {
        return std::hash<I>{}(h.value);
    }
};
template <>
struct std::hash<ae::AssetId> {
    ae::usize operator()(const ae::AssetId& id) const noexcept {
        return std::hash<ae::u64>{}(id.hi) ^ (std::hash<ae::u64>{}(id.lo) << 1);
    }
};
