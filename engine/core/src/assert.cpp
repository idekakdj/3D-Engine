// assert.cpp — assertion failure handlers behind AE_ASSERT / AE_VERIFY / AE_CHECK.
//
// assert_fail  : logs FATAL, breaks into an attached debugger, then aborts (no CRT
//                "abort() has been called" dialog, so unattended runs never hang).
// assert_report: logs ERROR and continues (the caller receives `false`). With a debugger
//                attached it breaks the FIRST time each call site fails (UE "ensure").
#include "aether/core/error.h"
#include "aether/core/hash.h"
#include "aether/core/log.h"
#include "aether/core/log_ext.h"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <mutex>
#include <unordered_set>

#ifdef _WIN32
#    include <windows.h>
#endif

namespace aether::detail {
namespace {

bool debugger_attached() {
#ifdef _WIN32
    return IsDebuggerPresent() != 0;
#else
    return false;
#endif
}

void debug_break() {
#ifdef _WIN32
    __debugbreak();
#endif
}

std::string describe(const char* kind, const char* expr, const char* file, int line, const char* msg) {
    return std::format("{} failed: ({}){}{} at {}:{}", kind, expr ? expr : "?", msg ? " - " : "",
                       msg ? msg : "", file ? file : "?", line);
}

thread_local bool t_in_assert = false;

} // namespace

[[noreturn]] void assert_fail(const char* expr, const char* file, int line, const char* msg) {
    if (!t_in_assert) {
        t_in_assert = true;
        const std::string text = describe("Assertion", expr, file, line, msg);
        if (get_log_level() > LogLevel::Fatal) {
            std::fprintf(stderr, "%s\n", text.c_str()); // logging disabled: still report
        }
        log_message(LogLevel::Fatal, "Assert", text);
        flush_log();
    }
    if (debugger_attached()) {
        debug_break();
    }
#ifdef _MSC_VER
    // Suppress the Debug-CRT abort message box / WER dialog: exit immediately.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::abort();
}

void assert_report(const char* expr, const char* file, int line, const char* msg) {
    log_message(LogLevel::Error, "Assert", describe("Verify", expr, file, line, msg));

    if (debugger_attached()) {
        static std::mutex             mutex;
        static std::unordered_set<u64> reported; // call sites that already broke once
        const u64 key = hash_combine(fnv1a64(StringView(file ? file : "")), static_cast<u64>(line));
        bool      first = false;
        {
            std::lock_guard lock(mutex);
            first = reported.insert(key).second;
        }
        if (first) {
            debug_break();
        }
    }
}

} // namespace aether::detail
