#pragma once
#include <string>
#include <vector>

namespace tw::rescue {

// "Why did my phone bootloop?" - reads what the previous boot left behind (the kernel's
// pstore / last_kmsg console log, the boot reason, A/B slot state) and turns known failure
// patterns into plain explanations with a fix.

enum class Fix {
    None,
    DisableModules,  // Magisk / KernelSU modules
    DisableVerity,   // vbmeta flags
    SwitchSlot,      // the other A/B slot
    FormatData,      // encryption / different ROM
    ReflashSystem,   // GSI / ROM image problem
    RestoreBoot,     // stock boot / init_boot backup
};

struct Finding {
    int severity = 1;          // 0 info, 1 likely cause, 2 certain
    std::string title;         // "Verified boot rejected the system partition"
    std::string explanation;   // what it means and what to do
    std::string evidence;      // the log line that matched
    Fix fix = Fix::None;
};

struct Report {
    std::string bootReason;    // ro.boot.bootreason (e.g. kernel_panic, reboot,recovery)
    std::string logSource;     // which file the previous boot's log came from ("" = none)
    std::string log;           // that log (may be long)
    std::vector<std::string> otherLogs;  // more pstore files (panic dumps, ...)
    std::string slotInfo;      // "slot A: active, successful; slot B: unbootable"
    bool magiskModules = false;
    std::vector<Finding> findings;
};

Report analyze();
// The pattern matcher on its own (self-tests feed it sample logs).
std::vector<Finding> scan(const std::string& log, const std::string& bootReason);
const char* fixLabel(Fix f);
// Copy the logs to <storage>/TWRP/logs/<date>/; returns the folder ("" on failure).
std::string saveLogs(const Report& r);

}  // namespace tw::rescue
