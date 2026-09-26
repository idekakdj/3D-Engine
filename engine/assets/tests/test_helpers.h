// test_helpers.h — shared fixtures for the aether.assets tests.
#pragma once

#include "aether/assets/asset_types.h"
#include "aether/core/types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace aether::assets::test {

// <repo>/content (compile definition from CMake).
std::filesystem::path content_dir();
std::filesystem::path sample(const char* relative); // content_dir() / "samples" / relative

// A unique, empty temporary directory removed (recursively) on destruction.
class TempDir {
public:
    explicit TempDir(const char* tag);
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path operator/(const char* rel) const { return path_ / rel; }

private:
    std::filesystem::path path_;
};

// Initialises aether::JobSystem once per process (shut down at exit).
void ensure_job_system();

std::string base64_encode(const void* data, usize size);
void        write_text(const std::filesystem::path& path, const std::string& text);
void        copy_into(const std::filesystem::path& from, const std::filesystem::path& to);
// Moves a file's last-write time forward so stamp-based change detection fires even on
// filesystems with coarse timestamps.
void        bump_mtime(const std::filesystem::path& path, int seconds = 5);

// Minimal glTF 2.0 document around one buffer (embedded as a data URI).
// `body` is spliced into the top-level JSON object (it must not contain "buffers").
std::string make_gltf(const std::vector<u8>& buffer, const std::string& body);

// Appends raw little-endian floats / u16 / u32 to a byte buffer; returns the start offset.
usize append_f32(std::vector<u8>& buf, const std::vector<f32>& values);
usize append_u16(std::vector<u8>& buf, const std::vector<u16>& values);

} // namespace aether::assets::test
