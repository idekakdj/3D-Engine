// aether/core/types.h — fundamental scalar aliases and universal vocabulary.
//
// FROZEN CONTRACT (ADR-0001). Core's universal vocabulary lives directly in
// `namespace aether` (not aether::core); module-specific APIs use aether::<module>.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace aether {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using f32 = float;
using f64 = double;

using usize = std::size_t;
using isize = std::ptrdiff_t;
using byte  = std::byte;

template <typename T>
using Span = std::span<T>;

using String     = std::string;
using StringView = std::string_view;

// Non-owning, non-null-by-convention view of raw bytes (uploads, asset blobs).
using ByteSpan = std::span<const byte>;

inline constexpr u32 kInvalidU32 = 0xFFFF'FFFFu;
inline constexpr u64 kInvalidU64 = 0xFFFF'FFFF'FFFF'FFFFull;

} // namespace aether

namespace ae = aether;
