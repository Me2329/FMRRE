#pragma once
#include <string>
#include <vector>

namespace tw {

class Task;

// Automation:
//  - OpenRecoveryScript (/cache/recovery/openrecoveryscript), the format apps and OTA
//    updaters write for TWRP: install, wipe, backup, restore, mount, cmd, reboot ...
//  - AOSP recovery arguments (/cache/recovery/command and the bootloader message in
//    /misc): --update_package=, --wipe_data, --wipe_cache, --sideload
//  - the `fmrre` command line tool (adb shell fmrre install /sdcard/rom.zip; `twrp` is an alias).
// FMRRE extensions: flash <image> <partition>, gsi <image>, lp resize|delete,
// vbmeta disable|enable, sleep <s>.
namespace ors {

struct Script {
    std::string source;              // where it came from (shown to the user)
    std::vector<std::string> lines;  // ORS commands
};

// Finds a pending script and consumes it (files deleted, bootloader message cleared)
// so that a crash can't trap the device in a recovery boot loop.
bool takePending(Script& out);
std::vector<std::string> fromAospArgs(const std::vector<std::string>& args);

// Runs commands in order; stops at the first failure. A trailing reboot happens after success.
bool run(const std::vector<std::string>& lines, Task& t);
std::string help();

// ---- fmrre command line -------------------------------------------------
// The running recovery UI picks up queued commands (and shows their progress);
// without a UI the commands run in the calling process.
int cliMain(const std::vector<std::string>& args);
std::string queueDir();
std::string uiPidFile();

}  // namespace ors
}  // namespace tw
