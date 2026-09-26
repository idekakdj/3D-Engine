// aether/core/log_ext.h — extra logging controls (additive to log.h).
//
// ADDITIVE (not a frozen contract). All functions are thread-safe and usable during
// static initialization / destruction.
#pragma once

#include "aether/core/log.h"

namespace aether {

// Unregister a sink previously added with add_log_sink (matched on sink + user).
// Returns true if it was registered. After this returns the sink is never called again.
bool remove_log_sink(LogSink sink, void* user);

// The built-in console sink (stdout for < Warn, stderr for >= Warn, plus
// OutputDebugStringA while a debugger is attached). Enabled by default.
void set_console_log_enabled(bool enabled);

// Flush stdout/stderr (the console sink already flushes Warn+ records eagerly).
void flush_log();

// Short fixed-width level name ("TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL").
[[nodiscard]] const char* log_level_name(LogLevel level);

} // namespace aether
