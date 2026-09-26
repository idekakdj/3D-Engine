// log.cpp — thread-safe, category-based logging with pluggable sinks.
//
// Design notes
//  * All state lives in a never-destroyed singleton constructed on first use, so logging
//    is safe during static initialization AND static destruction of any translation unit.
//  * The level check in the AE_LOG_* macros reads one relaxed atomic (no lock) before any
//    formatting happens. log_message() re-checks, so direct callers are filtered too.
//  * Records are dispatched to the console sink and every registered sink while holding
//    one mutex: output from concurrent threads never interleaves, and remove_log_sink()
//    guarantees the sink is not running / never called again once it returns.
//  * A thread-local depth guard makes logging from inside a sink safe: nested records go
//    to the console only (no deadlock, no unbounded recursion).
#include "aether/core/log.h"
#include "aether/core/log_ext.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <chrono>
#    include <ctime>
#endif

namespace aether {
namespace {

struct SinkEntry {
    LogSink sink = nullptr;
    void*   user = nullptr;
};

LogLevel initial_level_from_env() {
    const char* env = std::getenv("AE_LOG_LEVEL");
    if (!env || !*env) {
        return LogLevel::Info;
    }
    const std::string_view v(env);
    if (v == "trace") return LogLevel::Trace;
    if (v == "debug") return LogLevel::Debug;
    if (v == "info") return LogLevel::Info;
    if (v == "warn" || v == "warning") return LogLevel::Warn;
    if (v == "error") return LogLevel::Error;
    if (v == "fatal") return LogLevel::Fatal;
    if (v == "off") return LogLevel::Off;
    return LogLevel::Info;
}

struct LogState {
    std::atomic<LogLevel>  level{ initial_level_from_env() };
    std::atomic<bool>      console_enabled{ true };
    std::mutex             mutex; // guards `sinks` and serializes dispatch
    std::vector<SinkEntry> sinks;
};

// Constructed on first use, intentionally never destroyed (logging must keep working
// while other translation units run their static destructors).
LogState& state() {
    alignas(LogState) static unsigned char storage[sizeof(LogState)];
    static LogState* instance = ::new (static_cast<void*>(storage)) LogState();
    return *instance;
}

thread_local int t_dispatch_depth = 0;

void write_console(LogLevel level, std::string_view category, std::string_view message) {
    char stamp[32];
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u", static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
                  static_cast<unsigned>(st.wMilliseconds));
#else
    const auto now  = std::chrono::system_clock::now();
    const auto secs = std::chrono::system_clock::to_time_t(now);
    const auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm    tm{};
    localtime_r(&secs, &tm);
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count()));
#endif

    std::string line;
    line.reserve(message.size() + category.size() + 40);
    line += '[';
    line += stamp;
    line += "] [";
    line += log_level_name(level);
    line += "] [";
    line += category;
    line += "] ";
    line += message;
    line += '\n';

    FILE* out = level >= LogLevel::Warn ? stderr : stdout;
    std::fwrite(line.data(), 1, line.size(), out);
    std::fflush(out);

#ifdef _WIN32
    if (IsDebuggerPresent()) {
        OutputDebugStringA(line.c_str());
    }
#endif
}

} // namespace

const char* log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    case LogLevel::Off: return "OFF  ";
    }
    return "?????";
}

void log_message(LogLevel level, std::string_view category, std::string_view message) {
    if (level >= LogLevel::Off || level < get_log_level()) {
        return;
    }
    LogState& s = state();

    if (t_dispatch_depth > 0) {
        // Re-entered from a sink: console only (the mutex is held by this thread).
        write_console(level, category, message);
        return;
    }

    ++t_dispatch_depth;
    {
        std::lock_guard lock(s.mutex);
        if (s.console_enabled.load(std::memory_order_relaxed)) {
            write_console(level, category, message);
        }
        for (const SinkEntry& e : s.sinks) {
            e.sink(level, category, message, e.user);
        }
    }
    --t_dispatch_depth;
}

void set_log_level(LogLevel level) {
    state().level.store(level, std::memory_order_relaxed);
}

LogLevel get_log_level() {
    return state().level.load(std::memory_order_relaxed);
}

void add_log_sink(LogSink sink, void* user) {
    if (!sink) {
        return;
    }
    LogState&       s = state();
    std::lock_guard lock(s.mutex);
    s.sinks.push_back(SinkEntry{ sink, user });
}

bool remove_log_sink(LogSink sink, void* user) {
    LogState&       s = state();
    std::lock_guard lock(s.mutex);
    for (auto it = s.sinks.begin(); it != s.sinks.end(); ++it) {
        if (it->sink == sink && it->user == user) {
            s.sinks.erase(it);
            return true;
        }
    }
    return false;
}

void set_console_log_enabled(bool enabled) {
    state().console_enabled.store(enabled, std::memory_order_relaxed);
}

void flush_log() {
    std::fflush(stdout);
    std::fflush(stderr);
}

} // namespace aether
