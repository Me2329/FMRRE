#include "core/log.h"
#include "core/util.h"

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <fcntl.h>
#include <unistd.h>

namespace tw::log {

static std::mutex g_mu;
static std::deque<LogLine> g_lines;
static uint64_t g_gen = 0;
static FILE* g_file = nullptr;
static std::string g_path;
static int g_kmsg = -1;
static bool g_console = true;
static const size_t kMaxLines = 4000;

void init(const std::string& logFile) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_path = logFile;
    if (g_file) fclose(g_file);
    g_file = fopen(logFile.c_str(), "a");
    if (g_kmsg < 0) g_kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
}

std::string filePath() { return g_path; }
void setConsole(bool on) { g_console = on; }

static const char* tag(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "D";
        case LogLevel::Info: return "I";
        case LogLevel::Warn: return "W";
        case LogLevel::Error: return "E";
        case LogLevel::Print: return "P";
    }
    return "?";
}

void raw(LogLevel lvl, const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mu);
    // Split multi-line messages so the console renders each line.
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!(line.empty() && nl == std::string::npos && start != 0)) {
            g_lines.push_back({lvl, util::nowMs(), line});
            if (g_lines.size() > kMaxLines) g_lines.pop_front();
            if (g_file) {
                fprintf(g_file, "[%s] %s\n", tag(lvl), line.c_str());
                fflush(g_file);
            }
            if (g_console && lvl != LogLevel::Debug) fprintf(stderr, "[%s] %s\n", tag(lvl), line.c_str());
            if (g_kmsg >= 0 && lvl != LogLevel::Debug) {
                std::string k = "<6>twrpv2: " + line + "\n";
                ssize_t r = ::write(g_kmsg, k.data(), k.size());
                (void)r;
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    g_gen++;
}

void write(LogLevel lvl, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    raw(lvl, buf);
}

std::vector<LogLine> tail(size_t max) {
    std::lock_guard<std::mutex> lk(g_mu);
    size_t n = std::min(max, g_lines.size());
    return std::vector<LogLine>(g_lines.end() - n, g_lines.end());
}

uint64_t generation() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_gen;
}

}  // namespace tw::log
