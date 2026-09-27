// aether/assets/format.h — the cooked binary asset format (.aeasset) + checksums.
//
// File layout (all integers little-endian; floats IEEE-754 binary32 little-endian):
//
//   offset size  field
//   0      4     magic "AEAS"
//   4      4     u32 format_version            (kCookedFormatVersion)
//   8      1     u8  AssetType
//   9      7     reserved (zero)
//   16     16    AssetId (u64 hi, u64 lo)
//   32     8     u64 source_hash               (asset database content hash of the source)
//   40     4     u32 importer_version          (kImporterVersion at cook time)
//   44     4     reserved (zero)
//   48     8     u64 payload_bytes
//   56     4     u32 payload_crc32             (CRC-32/ISO-HDLC of the payload)
//   60     4     u32 header_crc32              (CRC-32 of bytes [0, 60))
//   64     ...   type-specific payload (see format.cpp; versioned by format_version)
//
// Decoding validates magic, version, both CRCs, the asset type, and every count/size in
// the payload against the remaining bytes, so corrupt or truncated files produce an
// Error, never UB. Serialisation uses memcpy only (no type punning).
//
// Thread-affinity: every function here is thread-safe (no shared state). File writes
// are atomic (write to "<path>.tmp", then rename over the target).
#pragma once

#include "aether/assets/asset_traits.h"
#include "aether/assets/asset_types.h"
#include "aether/core/error.h"
#include "aether/core/handle.h"
#include "aether/core/types.h"

#include <array>
#include <filesystem>
#include <vector>

namespace aether::assets {

inline constexpr std::array<char, 4> kCookedMagic{ 'A', 'E', 'A', 'S' };
inline constexpr u32   kCookedFormatVersion = 1;
inline constexpr usize kCookedHeaderSize = 64;
inline constexpr const char* kCookedExtension = ".aeasset";

struct CookedHeader {
    u32       format_version = kCookedFormatVersion;
    AssetType type = AssetType::Unknown;
    AssetId   id;
    u64       source_hash = 0;
    u32       importer_version = 0;
    u64       payload_bytes = 0;
    u32       payload_crc32 = 0;
};

// Identity + provenance written into a cooked file's header.
struct CookedMeta {
    AssetId id;
    u64     source_hash = 0;
    u32     importer_version = 0;
};

// ---------------------------------------------------------------------------
// Checksums / hashing (thread-safe, deterministic across platforms)
// ---------------------------------------------------------------------------

// CRC-32/ISO-HDLC (zlib/PNG polynomial 0xEDB88320). crc32("123456789") == 0xCBF43926.
// Pass the previous result as `seed` to checksum data incrementally.
[[nodiscard]] u32 crc32(ByteSpan data, u32 seed = 0) noexcept;

// 64-bit FNV-1a. Pass the previous result as `seed` to hash incrementally.
inline constexpr u64 kFnv1a64Offset = 0xcbf29ce484222325ull;
[[nodiscard]] u64 fnv1a64(ByteSpan data, u64 seed = kFnv1a64Offset) noexcept;
[[nodiscard]] u64 fnv1a64(StringView text, u64 seed = kFnv1a64Offset) noexcept;

// Views over raw memory for the functions above.
[[nodiscard]] inline ByteSpan as_bytes_view(const std::vector<u8>& v) noexcept {
    return ByteSpan(reinterpret_cast<const byte*>(v.data()), v.size());
}

// ---------------------------------------------------------------------------
// In-memory encode / decode
// ---------------------------------------------------------------------------
// encode_asset validates the data first (e.g. MeshData::skin size, texture byte size,
// skeleton array sizes and parent order, channel key counts) and returns
// ErrorCode::InvalidArgument for inconsistent data.
[[nodiscard]] Result<std::vector<u8>> encode_asset(const CookedMeta& meta, const MeshData& data);
[[nodiscard]] Result<std::vector<u8>> encode_asset(const CookedMeta& meta, const TextureData& data);
[[nodiscard]] Result<std::vector<u8>> encode_asset(const CookedMeta& meta, const MaterialData& data);
[[nodiscard]] Result<std::vector<u8>> encode_asset(const CookedMeta& meta, const SkeletonData& data);
[[nodiscard]] Result<std::vector<u8>> encode_asset(const CookedMeta& meta, const AnimationClipData& data);
[[nodiscard]] Result<std::vector<u8>> encode_asset(const CookedMeta& meta, const SceneData& data);

// Parses and validates only the 64-byte header (magic, version, header CRC).
[[nodiscard]] Result<CookedHeader> decode_header(ByteSpan bytes);

// Full decode of an asset of type T (T must match the header's AssetType).
template <CookableAsset T>
[[nodiscard]] Result<T> decode_asset(ByteSpan bytes, CookedHeader* out_header = nullptr);

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
template <CookableAsset T>
[[nodiscard]] Result<void> write_asset(const std::filesystem::path& path,
                                       const CookedMeta&            meta,
                                       const T&                     data);

template <CookableAsset T>
[[nodiscard]] Result<T> read_asset(const std::filesystem::path& path,
                                   CookedHeader*                out_header = nullptr);

// Reads just the header (cheap staleness checks).
[[nodiscard]] Result<CookedHeader> read_asset_header(const std::filesystem::path& path);

// decode_asset / write_asset / read_asset are explicitly instantiated in format.cpp for
// every CookableAsset type (MeshData, TextureData, MaterialData, SkeletonData,
// AnimationClipData, SceneData).

// Texture layout (asset_types.h): mips 0..n-1, each mip holds every layer (layer-major), tightly
// packed; block-compressed formats store ceil(w/4) x ceil(h/4) blocks per layer and mip.

// Expected byte size of TextureData::pixels for its dimensions/format/mips/layers.
[[nodiscard]] u64 texture_byte_size(const TextureData& texture) noexcept;
// Bytes per pixel of an uncompressed format; 0 for block-compressed formats.
[[nodiscard]] u32 texture_format_bytes_per_pixel(TextureFormat format) noexcept;
// BC5 / BC7: true (16-byte 4x4 blocks).
[[nodiscard]] bool is_block_compressed(TextureFormat format) noexcept;
// Bytes per 4x4 block (16 for BC5/BC7); 0 for uncompressed formats.
[[nodiscard]] u32 texture_format_block_bytes(TextureFormat format) noexcept;
// Bytes of ONE layer of mip `mip` for a texture whose mip 0 is width x height.
[[nodiscard]] u64 texture_layer_byte_size(TextureFormat format, u32 width, u32 height, u32 mip) noexcept;
// Byte offset of mip `mip` (layer 0) inside TextureData::pixels.
[[nodiscard]] u64 texture_mip_offset(const TextureData& texture, u32 mip) noexcept;

} // namespace aether::assets
