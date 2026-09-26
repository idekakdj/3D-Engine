// test_helpers.cpp — shared fixtures for the aether.assets tests.
#include "test_helpers.h"

#include "aether/core/job_system.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace aether::assets::test {

fs::path content_dir() { return fs::path(AE_TEST_CONTENT_DIR); }
fs::path sample(const char* relative) { return content_dir() / "samples" / relative; }

TempDir::TempDir(const char* tag) {
    static std::atomic<u32> counter{ 0 };
    std::random_device       rd;
    path_ = fs::temp_directory_path() /
            std::format("aether_assets_{}_{:08x}_{}", tag, rd(), counter.fetch_add(1));
    fs::remove_all(path_);
    fs::create_directories(path_);
}

TempDir::~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
}

namespace {
struct JobSystemGuard {
    JobSystemGuard() { JobSystem::initialize(4); }
    ~JobSystemGuard() { JobSystem::shutdown(); }
};
} // namespace

void ensure_job_system() {
    static JobSystemGuard guard;
    (void)guard;
}

std::string base64_encode(const void* data, usize size) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto*           p = static_cast<const u8*>(data);
    std::string           out;
    out.reserve((size + 2) / 3 * 4);
    for (usize i = 0; i < size; i += 3) {
        const u32 b0 = p[i];
        const u32 b1 = i + 1 < size ? p[i + 1] : 0u;
        const u32 b2 = i + 2 < size ? p[i + 2] : 0u;
        const u32 v = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(i + 1 < size ? kTable[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? kTable[v & 63] : '=');
    }
    return out;
}

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << text;
}

void copy_into(const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path());
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
}

void bump_mtime(const fs::path& path, int seconds) {
    fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(seconds));
}

std::string make_gltf(const std::vector<u8>& buffer, const std::string& body) {
    return std::format(R"({{"asset":{{"version":"2.0"}},"buffers":[{{"byteLength":{},"uri":"data:application/octet-stream;base64,{}"}}],{}}})",
                       buffer.size(), base64_encode(buffer.data(), buffer.size()), body);
}

usize append_f32(std::vector<u8>& buf, const std::vector<f32>& values) {
    while (buf.size() % 4) buf.push_back(0);
    const usize at = buf.size();
    buf.resize(at + values.size() * 4);
    std::memcpy(buf.data() + at, values.data(), values.size() * 4);
    return at;
}

usize append_u16(std::vector<u8>& buf, const std::vector<u16>& values) {
    while (buf.size() % 4) buf.push_back(0);
    const usize at = buf.size();
    buf.resize(at + values.size() * 2);
    std::memcpy(buf.data() + at, values.data(), values.size() * 2);
    return at;
}

} // namespace aether::assets::test
