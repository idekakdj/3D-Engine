// aether/editor/console.h — captures engine log records for the editor console panel.
//
// install_console_sink() registers a core log sink (once per process) that appends every record
// to a bounded, thread-safe ring buffer. The sink stays registered (and the buffer alive) for the
// rest of the process, so panels can come and go freely.
#pragma once

#include "aether/core/log.h"
#include "aether/core/types.h"

#include <string>
#include <vector>

namespace aether::editor {

struct ConsoleRecord {
    LogLevel    level = LogLevel::Info;
    std::string category;
    std::string message;
    u64         sequence = 0;
};

void install_console_sink(usize capacity = 4096);
// Copies the records with sequence > `after` (all when after == 0).
[[nodiscard]] std::vector<ConsoleRecord> console_records(u64 after = 0);
void                                     clear_console();
[[nodiscard]] u64                        console_error_count(); // errors + fatals since start/clear

} // namespace aether::editor
