// test_command_line.cpp — parse_command_line (shared application flags).
#include "aether/gameplay/application.h"

#include <doctest/doctest.h>

#include <vector>

using namespace aether;
using namespace aether::gameplay;

namespace {
AppDesc parse(std::vector<const char*> args, AppDesc defaults = {}) {
    args.insert(args.begin(), "app");
    return parse_command_line(static_cast<int>(args.size()), const_cast<char**>(args.data()), std::move(defaults));
}
} // namespace

TEST_CASE("command line: defaults are kept without flags") {
    AppDesc d;
    d.window.title = "Mine";
    d.max_frames   = 7;
    const AppDesc out = parse({}, d);
    CHECK(out.window.title == "Mine");
    CHECK(out.max_frames == 7);
    CHECK(out.enable_validation);
    CHECK(out.vsync);
}

TEST_CASE("command line: every shared flag") {
    const AppDesc out = parse({ "--frames", "120", "--scene", "levels/test.aescene", "--no-validation",
                                "--no-vsync", "--width", "800", "--height", "600", "--title", "Hello World" });
    CHECK(out.max_frames == 120);
    CHECK(out.startup_scene == std::filesystem::path("levels/test.aescene"));
    CHECK_FALSE(out.enable_validation);
    CHECK_FALSE(out.vsync);
    CHECK_FALSE(out.window.vsync);
    CHECK(out.window.width == 800);
    CHECK(out.window.height == 600);
    CHECK(out.window.title == "Hello World");
}

TEST_CASE("command line: malformed values and unknown flags are ignored") {
    AppDesc d;
    d.window.width = 1024;
    const AppDesc out = parse({ "--frames", "ten", "--width", "0", "--custom", "x", "--height", "-5", "--frames" }, d);
    CHECK(out.max_frames == 0);
    CHECK(out.window.width == 1024);
    CHECK(out.window.height == d.window.height);
}
