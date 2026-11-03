#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tw::history {

// What was flashed, wiped or restored, and when: <storage>/TWRP/history.txt, one line per action.
// Bootloop rescue shows the last entries ("it began after flashing X").
struct Entry {
    int64_t time = 0;  // seconds since the epoch
    bool ok = false;
    std::string action, detail;
};

void record(const std::string& action, const std::string& detail, bool ok);
std::vector<Entry> recent(size_t n);  // newest first
std::string ago(int64_t t);           // "5 min ago", "yesterday"
std::string path();

}  // namespace tw::history
