// handle.cpp — AssetId hashing / text form and the XXH64 implementation behind hash64().
#include "aether/core/asset_id.h"
#include "aether/core/handle.h"
#include "aether/core/hash.h"

#include <bit>
#include <cstring>

namespace aether {
namespace {

// ---- XXH64 (reference algorithm, little-endian reads) ------------------------------
constexpr u64 kP1 = 0x9E3779B185EBCA87ull;
constexpr u64 kP2 = 0xC2B2AE3D27D4EB4Full;
constexpr u64 kP3 = 0x165667B19E3779F9ull;
constexpr u64 kP4 = 0x85EBCA77C2B2AE63ull;
constexpr u64 kP5 = 0x27D4EB2F165667C5ull;

static_assert(std::endian::native == std::endian::little, "hash64 assumes a little-endian host");

inline u64 read_u64(const unsigned char* p) {
    u64 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline u32 read_u32(const unsigned char* p) {
    u32 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline u64 xxh_round(u64 acc, u64 input) {
    acc += input * kP2;
    acc = std::rotl(acc, 31);
    acc *= kP1;
    return acc;
}

inline u64 xxh_merge(u64 acc, u64 val) {
    acc ^= xxh_round(0, val);
    return acc * kP1 + kP4;
}

// Seed for the `hi` half of AssetId ("AETHERID"), independent of FNV's offset basis.
constexpr u64 kAssetIdSeed = 0x4145544845524944ull;

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

u64 hash64(const void* data, usize size, u64 seed) noexcept {
    const auto*       p   = static_cast<const unsigned char*>(data);
    const auto* const end = p + size;
    u64               h   = 0;

    if (size >= 32) {
        const auto* const limit = end - 32;
        u64               v1    = seed + kP1 + kP2;
        u64               v2    = seed + kP2;
        u64               v3    = seed;
        u64               v4    = seed - kP1;
        do {
            v1 = xxh_round(v1, read_u64(p));
            v2 = xxh_round(v2, read_u64(p + 8));
            v3 = xxh_round(v3, read_u64(p + 16));
            v4 = xxh_round(v4, read_u64(p + 24));
            p += 32;
        } while (p <= limit);
        h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
        h = xxh_merge(h, v1);
        h = xxh_merge(h, v2);
        h = xxh_merge(h, v3);
        h = xxh_merge(h, v4);
    } else {
        h = seed + kP5;
    }

    h += static_cast<u64>(size);

    while (end - p >= 8) {
        h ^= xxh_round(0, read_u64(p));
        h = std::rotl(h, 27) * kP1 + kP4;
        p += 8;
    }
    if (end - p >= 4) {
        h ^= static_cast<u64>(read_u32(p)) * kP1;
        h = std::rotl(h, 23) * kP2 + kP3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<u64>(*p) * kP5;
        h = std::rotl(h, 11) * kP1;
        ++p;
    }

    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

// ---- AssetId -------------------------------------------------------------------------

AssetId AssetId::from_string(StringView s) {
    if (s.empty()) {
        return kInvalidAsset;
    }
    AssetId id;
    id.hi = hash64(s.data(), s.size(), kAssetIdSeed);
    id.lo = fnv1a64(s);
    if (!id.is_valid()) {
        id.lo = 1; // astronomically unlikely; never hand out the invalid id for real input
    }
    return id;
}

String AssetId::to_string() const {
    String out(32, '0');
    for (int i = 0; i < 16; ++i) {
        out[static_cast<usize>(15 - i)] = kHexDigits[(hi >> (i * 4)) & 0xF];
        out[static_cast<usize>(31 - i)] = kHexDigits[(lo >> (i * 4)) & 0xF];
    }
    return out;
}

Result<AssetId> asset_id_from_hex(StringView hex) {
    if (hex.size() != 32) {
        return Error{ ErrorCode::InvalidArgument, "AssetId hex must be exactly 32 characters" };
    }
    AssetId id;
    for (usize i = 0; i < 32; ++i) {
        const int v = hex_value(hex[i]);
        if (v < 0) {
            return Error{ ErrorCode::InvalidArgument, "AssetId hex contains a non-hex character" };
        }
        u64& half = i < 16 ? id.hi : id.lo;
        half      = (half << 4) | static_cast<u64>(v);
    }
    return id;
}

} // namespace aether
