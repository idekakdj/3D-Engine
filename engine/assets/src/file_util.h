// file_util.h — private filesystem/string helpers for aether.assets (thread-safe).
#pragma once

#include "aether/core/error.h"
#include "aether/core/types.h"

#include <filesystem>
#include <vector>

namespace aether::assets::detail {

// UTF-8 <-> std::filesystem::path (wide on Windows), lossless.
[[nodiscard]] String                to_utf8(const std::filesystem::path& p);
[[nodiscard]] std::filesystem::path from_utf8(StringView utf8);

// Whole-file read (unicode-safe). Missing file -> ErrorCode::NotFound.
[[nodiscard]] Result<std::vector<u8>> read_file_bytes(const std::filesystem::path& p);
// Writes "<p>.tmp" then renames over `p` (creating parent directories).
[[nodiscard]] Result<void> write_file_atomic(const std::filesystem::path& p, ByteSpan bytes);

[[nodiscard]] String to_lower_ascii(StringView s);
[[nodiscard]] String extension_lower(const std::filesystem::path& p); // ".png"

[[nodiscard]] inline ByteSpan bytes_of(const std::vector<u8>& v) noexcept {
    return ByteSpan(reinterpret_cast<const byte*>(v.data()), v.size());
}
[[nodiscard]] inline ByteSpan bytes_of(StringView s) noexcept {
    return ByteSpan(reinterpret_cast<const byte*>(s.data()), s.size());
}

} // namespace aether::assets::detail
