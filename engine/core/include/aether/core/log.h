// aether/core/log.h — category-based logging.
//
// FROZEN CONTRACT (ADR-0001). Call shape:
//     AE_LOG_INFO("Renderer", "swapchain {}x{}", w, h);
// Categories are free-form strings. Formatting is std::format. The sink is
// pluggable (see log.cpp); default sink writes to stdout + the debugger.
#pragma once

#include "aether/core/types.h"

#include <format>
#include <string_view>

namespace aether {

enum class LogLevel : u8 { Trace = 0, Debug, Info, Warn, Error, Fatal, Off };

// Low-level entry point (thread-safe). Prefer the AE_LOG_* macros.
void log_message(LogLevel level, std::string_view category, std::string_view message);

// Runtime controls.
void     set_log_level(LogLevel level);
LogLevel get_log_level();

// A sink receives already-formatted records; register your own (e.g. editor console).
using LogSink = void (*)(LogLevel level, std::string_view category, std::string_view message,
                         void* user);
void add_log_sink(LogSink sink, void* user);

namespace detail {
template <typename... Args>
inline void log_fmt(LogLevel level, std::string_view category, std::format_string<Args...> fmt,
                    Args&&... args) {
    if (level < get_log_level())
        return;
    log_message(level, category, std::format(fmt, std::forward<Args>(args)...));
}
} // namespace detail

} // namespace aether

#define AE_LOG_TRACE(cat, ...) ::aether::detail::log_fmt(::aether::LogLevel::Trace, (cat), __VA_ARGS__)
#define AE_LOG_DEBUG(cat, ...) ::aether::detail::log_fmt(::aether::LogLevel::Debug, (cat), __VA_ARGS__)
#define AE_LOG_INFO(cat, ...)  ::aether::detail::log_fmt(::aether::LogLevel::Info,  (cat), __VA_ARGS__)
#define AE_LOG_WARN(cat, ...)  ::aether::detail::log_fmt(::aether::LogLevel::Warn,  (cat), __VA_ARGS__)
#define AE_LOG_ERROR(cat, ...) ::aether::detail::log_fmt(::aether::LogLevel::Error, (cat), __VA_ARGS__)
#define AE_LOG_FATAL(cat, ...) ::aether::detail::log_fmt(::aether::LogLevel::Fatal, (cat), __VA_ARGS__)
