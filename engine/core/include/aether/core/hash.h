// aether/core/hash.h — non-cryptographic hashing utilities.
//
// ADDITIVE (not a frozen contract). Stable across runs, platforms and builds, so the
// results may be persisted (asset ids, PSO/shader cache keys). Thread-safe (pure).
//   fnv1a64 : FNV-1a 64-bit        - tiny, fine for short keys.
//   hash64  : XXH64-compatible     - fast and well distributed for any size / seed.
#pragma once

#include "aether/core/types.h"

namespace aether {

inline constexpr u64 kFnv1a64Offset = 0xcbf29ce484222325ull;
inline constexpr u64 kFnv1a64Prime  = 0x00000100000001b3ull;

// FNV-1a over raw bytes. `seed` chains hashes (pass a previous result).
[[nodiscard]] constexpr u64 fnv1a64(const void* data, usize size, u64 seed = kFnv1a64Offset) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    u64         h = seed;
    for (usize i = 0; i < size; ++i) {
        h ^= static_cast<u64>(p[i]);
        h *= kFnv1a64Prime;
    }
    return h;
}

[[nodiscard]] constexpr u64 fnv1a64(StringView s, u64 seed = kFnv1a64Offset) noexcept {
    u64 h = seed;
    for (const char c : s) {
        h ^= static_cast<u64>(static_cast<unsigned char>(c));
        h *= kFnv1a64Prime;
    }
    return h;
}

// XXH64 (bit-exact with the reference implementation for the same seed).
[[nodiscard]] u64 hash64(const void* data, usize size, u64 seed = 0) noexcept;
[[nodiscard]] inline u64 hash64(StringView s, u64 seed = 0) noexcept {
    return hash64(s.data(), s.size(), seed);
}

// Order-dependent combination of two hashes (boost-style, 64-bit constants).
[[nodiscard]] constexpr u64 hash_combine(u64 seed, u64 value) noexcept {
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 12) + (seed >> 4));
}

} // namespace aether
