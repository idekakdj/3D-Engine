// command_line.cpp — the shared application CLI flags (application.h: parse_command_line).
#include "aether/core/log.h"
#include "aether/gameplay/application.h"

#include <charconv>
#include <string_view>

namespace aether::gameplay {

namespace {

template <typename T>
bool parse_uint(std::string_view text, T& out) {
    T value{};
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

} // namespace

AppDesc parse_command_line(int argc, char** argv, AppDesc defaults) {
    AppDesc desc = std::move(defaults);
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i] != nullptr ? argv[i] : "");
        const bool             has_value = i + 1 < argc && argv[i + 1] != nullptr;
        auto                   value     = [&]() -> std::string_view { return argv[++i]; };

        if (arg == "--no-validation") {
            desc.enable_validation = false;
        } else if (arg == "--no-vsync") {
            desc.vsync        = false;
            desc.window.vsync = false;
        } else if (arg == "--frames" && has_value) {
            const std::string_view v = value();
            if (!parse_uint(v, desc.max_frames)) {
                AE_LOG_WARN("App", "--frames: '{}' is not a frame count; ignored", v);
            }
        } else if (arg == "--width" && has_value) {
            const std::string_view v = value();
            u32                    w = 0;
            if (parse_uint(v, w) && w > 0) {
                desc.window.width = w;
            } else {
                AE_LOG_WARN("App", "--width: '{}' is not a positive integer; ignored", v);
            }
        } else if (arg == "--height" && has_value) {
            const std::string_view v = value();
            u32                    h = 0;
            if (parse_uint(v, h) && h > 0) {
                desc.window.height = h;
            } else {
                AE_LOG_WARN("App", "--height: '{}' is not a positive integer; ignored", v);
            }
        } else if (arg == "--scene" && has_value) {
            desc.startup_scene = std::filesystem::path(std::string(value()));
        } else if (arg == "--title" && has_value) {
            desc.window.title = std::string(value());
        }
        // Anything else belongs to the derived application.
    }
    return desc;
}

} // namespace aether::gameplay
