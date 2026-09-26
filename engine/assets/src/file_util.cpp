// file_util.cpp — private filesystem/string helpers for aether.assets.
#include "file_util.h"

#include <format>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace aether::assets::detail {

String to_utf8(const fs::path& p) {
    const std::u8string u8 = p.generic_u8string();
    return String(reinterpret_cast<const char*>(u8.data()), u8.size());
}

fs::path from_utf8(StringView utf8) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

Result<std::vector<u8>> read_file_bytes(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        return make_error<std::vector<u8>>(ErrorCode::NotFound,
                                           std::format("file not found: {}", to_utf8(p)));
    }
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) {
        return make_error<std::vector<u8>>(ErrorCode::IoError,
                                           std::format("cannot open: {}", to_utf8(p)));
    }
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return make_error<std::vector<u8>>(ErrorCode::IoError,
                                           std::format("cannot size: {}", to_utf8(p)));
    }
    std::vector<u8> bytes(static_cast<usize>(size));
    in.seekg(0, std::ios::beg);
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return make_error<std::vector<u8>>(ErrorCode::IoError,
                                           std::format("read failed: {}", to_utf8(p)));
    }
    return bytes;
}

Result<void> write_file_atomic(const fs::path& p, ByteSpan bytes) {
    std::error_code ec;
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
        if (ec) {
            return make_error(ErrorCode::IoError, std::format("cannot create directory {}: {}",
                                                              to_utf8(p.parent_path()), ec.message()));
        }
    }
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return make_error(ErrorCode::IoError, std::format("cannot write: {}", to_utf8(tmp)));
        }
        if (!bytes.empty()) {
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        out.flush();
        if (!out) {
            return make_error(ErrorCode::IoError, std::format("write failed: {}", to_utf8(tmp)));
        }
    }
    fs::rename(tmp, p, ec); // replaces an existing target (MoveFileEx REPLACE_EXISTING on Windows)
    if (ec) {
        fs::remove(tmp, ec);
        return make_error(ErrorCode::IoError, std::format("cannot replace {}", to_utf8(p)));
    }
    return {};
}

String to_lower_ascii(StringView s) {
    String out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

String extension_lower(const fs::path& p) {
    return to_lower_ascii(to_utf8(p.extension()));
}

} // namespace aether::assets::detail
