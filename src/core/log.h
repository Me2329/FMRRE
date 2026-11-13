#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace tw {

enum class LogLevel { Debug, Info, Warn, Error, Print };

struct LogLine {
    LogLevel level;
    int64_t timeMs;
    std::string text;
};

// Thread-safe ring-buffered logger. Lines also go to /tmp/recovery.log and
// to the kernel log (/dev/kmsg) so they survive a UI crash.
namespace log {
void init(const std::string& logFile);
void write(LogLevel lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void raw(LogLevel lvl, const std::string& text);
// Snapshot of the last `max` lines.
std::vector<LogLine> tail(size_t max);
// Monotonic counter, increments each time a line is added (UI uses it to know
// when to redraw the console).
uint64_t generation();
std::string filePath();
// Echo log lines to stderr (default on). The `fmrre` CLI turns it off to keep its output clean.
void setConsole(bool on);
}  // namespace log

}  // namespace tw

#define LOGD(...) ::tw::log::write(::tw::LogLevel::Debug, __VA_ARGS__)
#define LOGI(...) ::tw::log::write(::tw::LogLevel::Info, __VA_ARGS__)
#define LOGW(...) ::tw::log::write(::tw::LogLevel::Warn, __VA_ARGS__)
#define LOGE(...) ::tw::log::write(::tw::LogLevel::Error, __VA_ARGS__)
#define UIPRINT(...) ::tw::log::write(::tw::LogLevel::Print, __VA_ARGS__)
