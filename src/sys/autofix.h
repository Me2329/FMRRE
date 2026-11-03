#pragma once
#include <set>
#include <string>
#include <vector>

#include "sys/rescue.h"

namespace tw {

class Task;

// Auto repair: finds out what is wrong without the user having to know, and fixes it - safest fix
// first. It combines the previous boot's logs (rescue::analyze), what Android said when it gave up,
// the last thing that was changed (flash history) and health checks of the parts a phone needs to
// boot (boot images, vbmeta, super, slots, storage space, modules). It escalates across boots: a fix
// an earlier auto repair already made (and the phone still came back here) is skipped next time.
namespace autofix {

enum class Risk { Safe, Undoable, ErasesData };
const char* riskName(Risk r);

struct Check {
    std::string title;   // "Boot image (boot_a)"
    std::string status;  // "OK", "damaged - no ANDROID! header"
    int level = 0;       // 0 ok, 1 warning, 2 problem
};

struct Step {
    std::string id;      // disable_modules wipe_caches disable_verity restore_vbmeta restore_super restore_boot
                         // restore_init_boot restore_vendor_boot switch_slot repair_data_fs format_data
    std::string title, why;
    Risk risk = Risk::Safe;
    bool recommended = false;  // planned for this run
    bool triedBefore = false;  // an earlier auto repair already did it and the phone still failed
    std::string file;          // backup to restore, when the step uses one
    std::string note;          // a warning to show with it ("This also removes TWRP v2 from boot.")
};

struct Plan {
    bool bootFailed = false;     // the previous boot failed (logs, boot reason, Android's reason, or the user says so)
    std::string headline;        // "The last boot failed: the Android framework keeps crashing"
    std::string cause;           // the part after "The last boot failed: " (translatable on its own)
    std::string lastChange;      // "Flash image: init_boot_a <- magisk_patched.img (2 h ago)"
    std::vector<Check> checks;
    std::vector<Step> steps;     // in the order they would run
    std::vector<std::string> advice;  // what only the user can do (flash a ROM, use a computer ...)
    rescue::Report report;
    int earlierFixes = 0;        // auto repair steps in the last 24 hours
};

// userSaysBootFails: the phone doesn't start but left no log (many phones keep none) - plan the
// usual bootloop fixes anyway.
Plan diagnose(bool userSaysBootFails = false);
// The previous boot clearly failed? Computed once per run of TWRP v2; cheap afterwards.
bool lastBootFailed();

struct Options {
    std::set<std::string> only;  // ids to run; empty = every recommended step the permissions below allow
    bool allowSlotSwitch = false;
    bool allowFormatData = false;
    bool reboot = false;         // reboot to system afterwards (only when every step worked)
};
bool apply(const Plan& p, const Options& o, Task& t);

// "Repair automatically after a failed boot": at most 3 unattended runs a day, so a phone that can't
// be fixed this way doesn't loop between Android and recovery for ever.
bool autoRunAllowed();
void noteAutoRun();

}  // namespace autofix
}  // namespace tw
