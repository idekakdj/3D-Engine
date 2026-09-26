// console.cpp — log capture for the editor console (see console.h).
#include "aether/editor/console.h"

#include <deque>
#include <mutex>

namespace aether::editor {

namespace {
struct ConsoleBuffer {
    std::mutex                mutex;
    std::deque<ConsoleRecord> records;
    usize                     capacity  = 4096;
    u64                       next_seq  = 1;
    u64                       errors    = 0;
    bool                      installed = false;
};

ConsoleBuffer& buffer() {
    static ConsoleBuffer* b = new ConsoleBuffer(); // intentionally leaked: sinks cannot be removed
    return *b;
}

void sink(LogLevel level, std::string_view category, std::string_view message, void* user) {
    auto&            b = *static_cast<ConsoleBuffer*>(user);
    std::lock_guard  lock(b.mutex);
    b.records.push_back(ConsoleRecord{ level, std::string(category), std::string(message), b.next_seq++ });
    if (level >= LogLevel::Error) {
        ++b.errors;
    }
    while (b.records.size() > b.capacity) {
        b.records.pop_front();
    }
}
} // namespace

void install_console_sink(usize capacity) {
    ConsoleBuffer& b = buffer();
    std::lock_guard lock(b.mutex);
    b.capacity = capacity == 0 ? 1 : capacity;
    if (!b.installed) {
        b.installed = true;
        add_log_sink(&sink, &b);
    }
}

std::vector<ConsoleRecord> console_records(u64 after) {
    ConsoleBuffer&             b = buffer();
    std::lock_guard            lock(b.mutex);
    std::vector<ConsoleRecord> out;
    for (const ConsoleRecord& r : b.records) {
        if (r.sequence > after) {
            out.push_back(r);
        }
    }
    return out;
}

void clear_console() {
    ConsoleBuffer&  b = buffer();
    std::lock_guard lock(b.mutex);
    b.records.clear();
    b.errors = 0;
}

u64 console_error_count() {
    ConsoleBuffer&  b = buffer();
    std::lock_guard lock(b.mutex);
    return b.errors;
}

} // namespace aether::editor
