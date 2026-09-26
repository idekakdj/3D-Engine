// core_tests.cpp — unit tests for aether.core (no window / GPU required).
#include <doctest/doctest.h>

#include "aether/core/asset_id.h"
#include "aether/core/error.h"
#include "aether/core/geometry.h"
#include "aether/core/handle.h"
#include "aether/core/hash.h"
#include "aether/core/input.h"
#include "aether/core/job_system.h"
#include "aether/core/log.h"
#include "aether/core/log_ext.h"
#include "aether/core/math.h"
#include "aether/core/paths.h"
#include "aether/core/paths_ext.h"
#include "aether/core/time.h"
#include "input_internal.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace aether;

// ---------------------------------------------------------------------------------------
// Result / Error
// ---------------------------------------------------------------------------------------
namespace {
Result<int> parse_positive(int v) {
    if (v <= 0) {
        return make_error<int>(ErrorCode::InvalidArgument, "not positive");
    }
    return v;
}
Result<void> maybe_fail(bool fail) {
    if (fail) {
        return make_error(ErrorCode::IoError, "boom");
    }
    return {};
}
} // namespace

TEST_CASE("Result<T> carries a value or an error") {
    auto ok = parse_positive(7);
    REQUIRE(ok.has_value());
    CHECK(static_cast<bool>(ok));
    CHECK(*ok == 7);
    CHECK(ok.value_or(1) == 7);

    auto bad = parse_positive(-1);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().is(ErrorCode::InvalidArgument));
    CHECK(bad.error().message == "not positive");
    CHECK(bad.value_or(42) == 42);

    Result<std::string> s = std::string("hello");
    CHECK(s->size() == 5);
}

TEST_CASE("Result<void>") {
    CHECK(maybe_fail(false).has_value());
    auto r = maybe_fail(true);
    CHECK_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::IoError);
}

TEST_CASE("AE_VERIFY returns the condition and reports failures") {
    const LogLevel old = get_log_level();
    set_log_level(LogLevel::Off); // keep the expected failure out of the output
    CHECK(AE_VERIFY(1 + 1 == 2));
    CHECK_FALSE(AE_VERIFY_MSG(1 + 1 == 3, "expected failure (test)"));
    set_log_level(old);
}

// ---------------------------------------------------------------------------------------
// Handles / AssetId / hashing
// ---------------------------------------------------------------------------------------
namespace {
struct MeshTag;
struct TexTag;
} // namespace

TEST_CASE("Handle packs index and generation") {
    using H = Handle<MeshTag>;
    H def;
    CHECK_FALSE(def.is_valid());
    CHECK_FALSE(static_cast<bool>(def));

    H h(1234, 7);
    CHECK(h.is_valid());
    CHECK(h.index() == 1234);
    CHECK(h.generation() == 7);
    CHECK(h.value == ((7u << 24) | 1234u));

    H max_idx(H::kIndexMask - 1, 255);
    CHECK(max_idx.index() == H::kIndexMask - 1);
    CHECK(max_idx.generation() == 255);

    CHECK(H(3, 1) != H(3, 2)); // stale generation differs
    CHECK(H(3, 1) == H(3, 1));
    CHECK(std::hash<H>{}(h) == std::hash<u32>{}(h.value));

    static_assert(!std::is_convertible_v<Handle<MeshTag>, Handle<TexTag>>);
}

TEST_CASE("hash64 matches XXH64 reference vectors") {
    CHECK(hash64("", 0, 0) == 0xEF46DB3751D8E999ull);
    CHECK(hash64(StringView("abc")) == 0x44BC2CF5AD770999ull);
    // Long inputs exercise the 32-byte stripe loop + every tail path.
    std::string long_input;
    for (int i = 0; i < 1000; ++i) {
        long_input.push_back(static_cast<char>('a' + i % 26));
    }
    std::set<u64> seen;
    for (usize len = 0; len <= 100; ++len) {
        seen.insert(hash64(long_input.data(), len));
    }
    CHECK(seen.size() == 101); // no collisions across lengths
    CHECK(hash64(long_input, 1) != hash64(long_input, 2));
    CHECK(fnv1a64(StringView("a")) == 0xaf63dc4c8601ec8cull);
    CHECK(fnv1a64(StringView("")) == kFnv1a64Offset);
}

TEST_CASE("AssetId: deterministic, two independent halves, hex round trip") {
    const AssetId a = AssetId::from_string("content/meshes/cube.gltf");
    const AssetId b = AssetId::from_string("content/meshes/cube.gltf");
    const AssetId c = AssetId::from_string("content/meshes/cube.gltF");
    CHECK(a.is_valid());
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a.hi != a.lo);
    CHECK_FALSE(AssetId::from_string("").is_valid());

    const String hex = a.to_string();
    CHECK(hex.size() == 32);
    for (char ch : hex) {
        CHECK(((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')));
    }
    auto parsed = asset_id_from_hex(hex);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == a);

    AssetId small{ 0x1, 0xABC };
    CHECK(small.to_string() == "0000000000000001" "0000000000000abc");
    auto upper = asset_id_from_hex("0000000000000001" "0000000000000ABC");
    REQUIRE(upper.has_value());
    CHECK(*upper == small);

    CHECK_FALSE(asset_id_from_hex("123").has_value());
    CHECK_FALSE(asset_id_from_hex("zz000000000000000000000000000000").has_value());

    std::unordered_set<AssetId> set{ a, c };
    CHECK(set.size() == 2);
}

// ---------------------------------------------------------------------------------------
// Math / geometry
// ---------------------------------------------------------------------------------------
TEST_CASE("AABB expand / center / extent") {
    AABB box{ Vec3(0.0f), Vec3(0.0f) };
    box.expand(Vec3(1.0f, 2.0f, 3.0f));
    box.expand(Vec3(-1.0f, -2.0f, -3.0f));
    CHECK(box.valid());
    CHECK(box.center() == Vec3(0.0f));
    CHECK(box.extent() == Vec3(1.0f, 2.0f, 3.0f));
    AABB other{ Vec3(5.0f), Vec3(6.0f) };
    box.expand(other);
    CHECK(box.max == Vec3(6.0f));
    CHECK_FALSE(AABB{ Vec3(1.0f), Vec3(0.0f) }.valid());
}

TEST_CASE("Transform::to_matrix applies scale, then rotation, then translation") {
    Transform t;
    t.position = Vec3(10.0f, 0.0f, 0.0f);
    t.rotation = glm::angleAxis(kHalfPi, Vec3(0.0f, 0.0f, 1.0f));
    t.scale    = Vec3(2.0f);
    const Vec4 p = t.to_matrix() * Vec4(1.0f, 0.0f, 0.0f, 1.0f);
    // (1,0,0) * 2 = (2,0,0); rotate 90deg about Z -> (0,2,0); translate -> (10,2,0)
    CHECK(p.x == doctest::Approx(10.0f).epsilon(1e-5));
    CHECK(p.y == doctest::Approx(2.0f).epsilon(1e-5));
    CHECK(p.z == doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("perspective: Vulkan clip space (Y down, depth 0..1) and reverse-Z friendly") {
    const f32  n = 0.1f, f = 100.0f;
    const Mat4 proj = perspective(kHalfPi, 16.0f / 9.0f, n, f);
    const Mat4 view = look_at(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 1.0f, 0.0f));

    auto ndc = [&](Vec3 world) {
        Vec4 c = proj * view * Vec4(world, 1.0f);
        return Vec3(c) / c.w;
    };
    CHECK(ndc(Vec3(0.0f, 0.0f, -n)).z == doctest::Approx(0.0f).epsilon(1e-4)); // near -> 0
    CHECK(ndc(Vec3(0.0f, 0.0f, -f)).z == doctest::Approx(1.0f).epsilon(1e-4)); // far  -> 1
    CHECK(ndc(Vec3(0.0f, 1.0f, -5.0f)).y < 0.0f); // world up -> NDC -y (top of screen)

    // Reverse-Z: swap near/far planes -> near maps to 1, far to 0 (GreaterEqual depth test).
    const Mat4 rev = perspective(kHalfPi, 1.0f, f, n);
    Vec4       cn  = rev * Vec4(0.0f, 0.0f, -n, 1.0f);
    Vec4       cf  = rev * Vec4(0.0f, 0.0f, -f, 1.0f);
    CHECK(cn.z / cn.w == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(cf.z / cf.w == doctest::Approx(0.0f).epsilon(1e-4));
}

TEST_CASE("geometry layouts are the frozen GPU contract") {
    CHECK(sizeof(Vertex) == 48);
    CHECK(offsetof(Vertex, normal) == 12);
    CHECK(offsetof(Vertex, tangent) == 24);
    CHECK(offsetof(Vertex, uv0) == 40);
    CHECK(sizeof(SkinVertex) == 24);
    CHECK(offsetof(SkinVertex, weights) == 8);
}

// ---------------------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------------------
TEST_CASE("now_seconds is monotonic and ScopedTimer measures") {
    const f64 a = now_seconds();
    f64       elapsed = -1.0;
    {
        ScopedTimer t(&elapsed);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const f64 b = now_seconds();
    CHECK(a >= 0.0);
    CHECK(b > a);
    CHECK(elapsed >= 0.004);
}

// ---------------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------------
namespace {
struct CaptureSink {
    std::mutex               mutex;
    std::vector<std::string> lines;
    std::vector<LogLevel>    levels;
    static void fn(LogLevel level, std::string_view cat, std::string_view msg, void* user) {
        auto*           self = static_cast<CaptureSink*>(user);
        std::lock_guard lock(self->mutex);
        self->lines.push_back(std::string(cat) + "|" + std::string(msg));
        self->levels.push_back(level);
    }
};
} // namespace

TEST_CASE("log sinks receive formatted records, respect the level, and can be removed") {
    CaptureSink    sink;
    const LogLevel old = get_log_level();
    set_console_log_enabled(false);
    add_log_sink(&CaptureSink::fn, &sink);
    set_log_level(LogLevel::Info);

    AE_LOG_INFO("Test", "value={} name={}", 42, "x");
    AE_LOG_DEBUG("Test", "filtered {}", 1);
    AE_LOG_ERROR("Other", "err");
    REQUIRE(sink.lines.size() == 2);
    CHECK(sink.lines[0] == "Test|value=42 name=x");
    CHECK(sink.levels[0] == LogLevel::Info);
    CHECK(sink.lines[1] == "Other|err");

    // Thread safety: many threads logging concurrently lose nothing.
    sink.lines.clear();
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 100; ++i) {
                AE_LOG_WARN("MT", "t{} i{}", t, i);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    CHECK(sink.lines.size() == 800);

    CHECK(remove_log_sink(&CaptureSink::fn, &sink));
    CHECK_FALSE(remove_log_sink(&CaptureSink::fn, &sink));
    AE_LOG_ERROR("Test", "after removal");
    CHECK(sink.lines.size() == 800);

    set_log_level(old);
    set_console_log_enabled(true);
    CHECK(std::string(log_level_name(LogLevel::Warn)) == "WARN ");
}

// ---------------------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------------------
TEST_CASE("paths: executable dir, engine root with shaders/, file reads") {
    namespace fs = std::filesystem;
    CHECK(fs::is_directory(paths::executable_dir()));
    // Build trees live outside the repo: the root must still resolve to a directory that
    // has shaders/ (via AE_SOURCE_DIR in dev builds).
    CHECK(fs::is_directory(paths::shader_dir()));
    CHECK(paths::shader_dir() == paths::engine_root() / "shaders");
    CHECK(fs::is_regular_file(paths::shader_dir() / "common" / "bindless.glsl"));

    const fs::path tmp = paths::cache_dir() / "core_tests_tmp.txt";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << "\xEF\xBB\xBFhello\nworld";
    }
    CHECK(paths::read_text_file(tmp) == "hello\nworld"); // BOM stripped
    CHECK(paths::read_file(tmp).size() == 14);
    fs::remove(tmp);

    const LogLevel old = get_log_level();
    set_log_level(LogLevel::Off);
    CHECK(paths::read_file(paths::cache_dir() / "does_not_exist.bin").empty());
    set_log_level(old);
}

// ---------------------------------------------------------------------------------------
// Input edge semantics
// ---------------------------------------------------------------------------------------
TEST_CASE("Input edges: Pressed -> Held -> Released -> Up, quick taps survive") {
    detail::input_reset();
    const i32 w = static_cast<i32>(Key::W);

    detail::input_on_key(w, true);
    CHECK(Input::state().keys[w] == ButtonState::Pressed);
    CHECK(Input::state().key_pressed(Key::W));
    CHECK(Input::state().key_down(Key::W));
    Input::begin_frame();
    CHECK(Input::state().keys[w] == ButtonState::Held);
    CHECK_FALSE(Input::state().key_pressed(Key::W));
    CHECK(Input::state().key_down(Key::W));
    detail::input_on_key(w, false);
    CHECK(Input::state().keys[w] == ButtonState::Released);
    CHECK_FALSE(Input::state().key_down(Key::W));
    Input::begin_frame();
    CHECK(Input::state().keys[w] == ButtonState::Up);

    // Press + release inside one frame: visible as Pressed, then Released, then Up.
    detail::input_on_key(w, true);
    detail::input_on_key(w, false);
    CHECK(Input::state().keys[w] == ButtonState::Pressed);
    Input::begin_frame();
    CHECK(Input::state().keys[w] == ButtonState::Released);
    Input::begin_frame();
    CHECK(Input::state().keys[w] == ButtonState::Up);

    // Mouse, cursor delta and scroll accumulate within a frame and reset at begin_frame.
    detail::input_on_mouse_button(0, true);
    CHECK(Input::state().mouse_down(MouseButton::Left));
    detail::input_on_cursor(10.0, 10.0);
    detail::input_on_cursor(15.0, 12.0);
    detail::input_on_scroll(0.0, 1.0);
    detail::input_on_scroll(0.0, 2.0);
    CHECK(Input::state().cursor_delta == Vec2(5.0f, 2.0f));
    CHECK(Input::state().scroll == Vec2(0.0f, 3.0f));
    Input::begin_frame();
    CHECK(Input::state().cursor_delta == Vec2(0.0f));
    CHECK(Input::state().scroll == Vec2(0.0f));
    CHECK(Input::state().mouse[0] == ButtonState::Held);

    detail::input_on_key(-1, true);   // GLFW_KEY_UNKNOWN ignored
    detail::input_on_key(4096, true); // out of range ignored
    detail::input_reset();
}

// ---------------------------------------------------------------------------------------
// Job system
// ---------------------------------------------------------------------------------------
TEST_CASE("JobSystem runs inline before initialize") {
    JobSystem::shutdown();
    int        x = 0;
    JobCounter c;
    JobSystem::run([&] { x = 5; }, &c);
    CHECK(x == 5);
    CHECK(c.load() == 0);
    JobSystem::wait(c);
}

TEST_CASE("JobSystem: parallel_for touches every index exactly once under contention") {
    JobSystem::initialize(0);
    JobSystem::initialize(3); // idempotent
    REQUIRE(JobSystem::worker_count() >= 1);

    constexpr u32                  kCount = 100'000;
    std::vector<std::atomic<u32>> hits(kCount);
    for (auto& h : hits) {
        h.store(0);
    }
    for (u32 group : { 0u, 1u, 7u, 1024u }) {
        JobSystem::parallel_for(kCount, group, [&](u32 i) { hits[i].fetch_add(1, std::memory_order_relaxed); });
    }
    // Asynchronous variant with a caller-owned counter and a temporary lambda.
    JobCounter c;
    JobSystem::parallel_for(kCount, 256, [&](u32 i) { hits[i].fetch_add(1, std::memory_order_relaxed); }, &c);
    JobSystem::wait(c);
    CHECK(c.load() == 0);

    bool all_five = true;
    for (auto& h : hits) {
        all_five = all_five && h.load() == 5;
    }
    CHECK(all_five);

    // Several external threads hammering the pool at once.
    std::atomic<u64>         sum{ 0 };
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t) {
        producers.emplace_back([&] {
            JobSystem::parallel_for(10'000, 16, [&](u32 i) { sum.fetch_add(i, std::memory_order_relaxed); });
        });
    }
    for (auto& p : producers) {
        p.join();
    }
    CHECK(sum.load() == 4ull * (9'999ull * 10'000ull / 2ull));
}

TEST_CASE("JobSystem: nested run from inside a job, and wait() helps") {
    JobSystem::initialize();
    std::atomic<int> leaves{ 0 };
    JobCounter       outer;
    for (int i = 0; i < 16; ++i) {
        JobSystem::run(
            [&] {
                JobCounter inner;
                for (int j = 0; j < 16; ++j) {
                    JobSystem::run([&] { leaves.fetch_add(1); }, &inner);
                }
                JobSystem::wait(inner); // waiting inside a worker must not deadlock
                CHECK(inner.load() == 0);
            },
            &outer);
    }
    JobSystem::wait(outer);
    CHECK(leaves.load() == 256);

    // wait() executes jobs on the calling thread: with every worker blocked, the only way
    // the counter can drain is if the waiting (main) thread runs the queued jobs itself.
    const u32               workers = JobSystem::worker_count();
    std::atomic<bool>       release{ false };
    std::atomic<u32>        blocked{ 0 };
    JobCounter              blockers;
    for (u32 i = 0; i < workers; ++i) {
        JobSystem::run(
            [&] {
                blocked.fetch_add(1);
                while (!release.load()) {
                    std::this_thread::yield();
                }
            },
            &blockers);
    }
    while (blocked.load() < workers) {
        std::this_thread::yield();
    }
    const auto main_id = std::this_thread::get_id();
    std::atomic<int> ran_on_main{ 0 };
    JobCounter       work;
    for (int i = 0; i < 8; ++i) {
        JobSystem::run([&] { if (std::this_thread::get_id() == main_id) ran_on_main.fetch_add(1); }, &work);
    }
    JobSystem::wait(work);
    CHECK(ran_on_main.load() == 8);
    release.store(true);
    JobSystem::wait(blockers);
}

TEST_CASE("JobSystem shutdown drains queued jobs and can restart") {
    JobSystem::initialize(2);
    std::atomic<int> done{ 0 };
    JobCounter       c;
    for (int i = 0; i < 1000; ++i) {
        JobSystem::run([&] { done.fetch_add(1); }, &c);
    }
    JobSystem::shutdown();
    CHECK(done.load() == 1000);
    CHECK(c.load() == 0);
    CHECK(JobSystem::worker_count() == 0);
    JobSystem::shutdown(); // idempotent

    JobSystem::initialize(2);
    CHECK(JobSystem::worker_count() == 2);
    JobSystem::shutdown();
}
