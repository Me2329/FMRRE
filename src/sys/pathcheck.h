#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tw::pathcheck {

enum class Level { Ok, Info, Warn, Error };

struct Finding {
    Level level;
    std::string text;
};

struct Report {
    std::string input;      // what the user typed
    std::string resolved;   // the real path in recovery (e.g. /sdcard -> /data/media/0)
    bool exists = false, isFile = false, readable = false;
    uint64_t size = 0;
    bool shownInImagePicker = false;  // would appear in Install > Image without "All files"
    std::vector<Finding> findings;
    std::vector<std::string> suggestions;  // existing files that are probably what was meant
    bool usable() const { return exists && isFile && readable && size > 0; }
};

// Map Android paths (/sdcard, /storage/emulated/0, /storage/XXXX-XXXX) to where they live in recovery.
std::string resolve(const std::string& input);
// Inverse of resolve() for display: /data/media/0/x -> /sdcard/x.
std::string pretty(const std::string& real);

Report check(const std::string& input);

}  // namespace tw::pathcheck
