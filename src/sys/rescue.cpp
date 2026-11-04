#include "sys/rescue.h"

#include <algorithm>
#include <cstring>
#include <sys/mount.h>

#include "core/log.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/bootimg.h"
#include "sys/history.h"
#include "sys/partitions.h"

namespace tw::rescue {

const char* fixLabel(Fix f) {
    switch (f) {
        case Fix::DisableModules: return "Disable all modules";
        case Fix::DisableVerity: return "Disable verified boot";
        case Fix::SwitchSlot: return "Boot the other slot";
        case Fix::FormatData: return "Format Data";
        case Fix::ReflashSystem: return "Flash a system image";
        case Fix::RestoreBoot: return "Restore stock boot";
        default: return "";
    }
}

namespace {

struct Rule {
    std::vector<std::vector<const char*>> any;  // a line matches if it contains every word of one group
    int severity;
    const char* title;
    const char* explanation;
    Fix fix;
};

// Lower-case patterns; most specific first. Each rule reports its first matching line.
const Rule kRules[] = {
    {{{"device-mapper: verity", "corrupt"}, {"dm-verity", "corrupt"}, {"verity", "mismatch"}, {"avb_slot_verify", "error"},
      {"avb", "verification failed"}, {"libfs_avb", "fail"}, {"libfs_avb", "error"}, {"vbmeta", "verification", "fail"},
      {"verified boot", "fail"}},
     2, "Verified boot rejected a partition",
     "The system (or vendor) image doesn't match what vbmeta expects - normal after flashing a GSI or a modified image. "
     "Disable verification in vbmeta, or flash the vbmeta that belongs to the image.",
     Fix::DisableVerity},
    {{{"failed to load", "sepolicy"}, {"selinux", "could not load policy"}, {"selinux", "failed to load"}, {"split policy", "fail"}},
     2, "The SELinux policy didn't load",
     "System and vendor don't fit together (for example a GSI that is too new or too old for this vendor). Try another GSI "
     "build, or go back to the stock system.",
     Fix::ReflashSystem},
    {{{"failed to mount", "/system"}, {"failed to mount required partitions"}, {"first_stage_mount", "fail"},
      {"failed to find device for partition", "system"}, {"__mount", "/system", "failed"}, {"failed to mount", "/vendor"}},
     2, "The system partition didn't mount",
     "Usually an image for the wrong CPU type (arm64 vs 32-bit 'a64'), a non-A/B image on an A/B phone, or a damaged "
     "download. Flash the right image - for GSIs, arm64-ab (vndklite if vendor is read-only).",
     Fix::ReflashSystem},
    {{{"fscrypt", "fail"}, {"failed to install", "key"}, {"init_user0_failed"}, {"vold", "failed to unlock"},
      {"e4crypt", "fail"}, {"cryptfs", "fail"}, {"failed to mount", "/data"}, {"metadata encryption", "fail"}, {"keymaster", "error"}},
     2, "Android couldn't decrypt or mount /data",
     "Typical after switching ROMs or GSIs: the old encryption keys don't fit any more. Format Data fixes it but erases "
     "internal storage - copy your files to a computer or SD card first.",
     Fix::FormatData},
    {{{"kernel panic - not syncing"}}, 2, "The kernel crashed (panic)",
     "See the line below. If this began after flashing a kernel, a boot image or a Magisk/KernelSU patch, restore the stock "
     "boot image.",
     Fix::RestoreBoot},
    {{{"unable to handle kernel"}, {"internal error: oops"}}, 1, "The kernel hit a fault (oops)",
     "A driver or kernel bug. If it started after flashing a kernel or boot image, restore the stock one.", Fix::RestoreBoot},
    {{{"rescueparty"}, {"rescue party"}}, 2, "Android gave up after repeated crashes (Rescue Party)",
     "An app or the framework crashed over and over. Disable Magisk modules first; if that doesn't help, Format Data.",
     Fix::DisableModules},
    {{{"zygote", "crashed"}, {"zygote", "restarting"}, {"service 'zygote'", "exited"}, {"system_server", "crash"},
      {"fatal signal", "system_server"}, {"fatal signal", "zygote"}, {"zygote", "died"}},
     1, "The Android framework keeps crashing (zygote / system_server)",
     "Common with leftover data from another ROM (Format Data), a GSI that doesn't suit this vendor, or a broken "
     "Magisk / Xposed module (disable modules).",
     Fix::FormatData},
    {{{"fatal exception in system process"}}, 2, "system_server crashed (Java exception)",
     "The core Android process threw the exception below - usually a ROM/GSI that doesn't fit this vendor, a broken Xposed / "
     "Magisk module, or leftover data from another ROM.",
     Fix::DisableModules},
    {{{"initfatalreboot"}, {"init_fatal_reboot_target"}, {"init: init encountered errors"}}, 1, "init hit a fatal error",
     "Android's first process stopped the boot. The line below names the cause; often the system image or its vendor "
     "doesn't fit.",
     Fix::ReflashSystem},
    {{{"watchdog", "bark"}, {"watchdog", "bite"}, {"watchdog", "timeout"}, {"watchdog did not stop"}, {"hard lockup"}, {"soft lockup"}},
     1, "The system froze (watchdog reset)",
     "Usually a kernel or driver hang. Try the other slot, or the stock boot image if you changed the kernel.", Fix::SwitchSlot},
};

std::string firstLine(const std::string& log, const std::vector<std::vector<const char*>>& any) {
    size_t pos = 0;
    while (pos < log.size()) {
        size_t nl = log.find('\n', pos);
        if (nl == std::string::npos) nl = log.size();
        std::string line = log.substr(pos, nl - pos), low = util::toLower(line);
        for (auto& group : any) {
            bool all = true;
            for (auto w : group)
                if (low.find(w) == std::string::npos) { all = false; break; }
            if (all) return util::trim(line).substr(0, 300);
        }
        pos = nl + 1;
    }
    return "";
}

// android_pmsg_log_header_t: 'l', u16 len (whole record), u16 uid, u16 pid; then
// android_log_header_t: u8 log id, u16 tid, u32 sec, u32 nsec; then the entry:
// u8 priority, tag, NUL, message, NUL (text buffers; the events buffer is binary and skipped).
std::string decodePmsgImpl(const std::string& raw) {
    static const char kPrio[] = "??VDIWEF";
    std::string out;
    const uint8_t* d = (const uint8_t*)raw.data();
    size_t n = raw.size();
    for (size_t i = 0; i + 7 + 11 + 3 <= n;) {
        uint16_t len = uint16_t(d[i + 1] | d[i + 2] << 8);
        if (d[i] != 'l' || len < 7 + 11 + 3 || i + len > n) { i++; continue; }  // resync on the next record
        const uint8_t* e = d + i + 7;
        uint8_t id = e[0];
        size_t body = len - 7 - 11;
        const char* p = (const char*)e + 11;
        i += len;
        if (id == 2 || id > 7) continue;  // events (binary) / unknown
        uint8_t prio = (uint8_t)p[0];
        if (prio < 2 || prio > 7) continue;
        std::string tag(p + 1, strnlen(p + 1, body - 1));
        size_t mo = 1 + tag.size() + 1;
        if (mo >= body) continue;
        std::string msg(p + mo, strnlen(p + mo, body - mo));
        if (prio < 5 && id != 4) continue;  // only W/E/F, but the whole crash buffer
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
        for (auto& line : util::split(msg, '\n', false)) out += std::string(1, kPrio[prio]) + "/" + tag + ": " + line + "\n";
        if (out.size() > (2u << 20)) break;
    }
    return out;
}

std::vector<std::string> pstoreFiles() {
    std::string dir = device::path("/sys/fs/pstore");
    if (!device::isSim() && util::isDir(dir) && util::listDir(dir).empty()) mount("pstore", dir.c_str(), "pstore", 0, nullptr);
    std::vector<std::string> out;
    for (auto& e : util::listDir(dir))
        if (!e.dir) out.push_back(dir + "/" + e.name);
    std::sort(out.begin(), out.end());
    return out;
}

std::string slotInfo() {
    const device::Info& i = device::info();
    if (!i.ab) return "Not an A/B phone";
    std::string s = "Booted slot " + std::string(i.slot == "_b" ? "B" : "A");
    if (device::isSim()) return s + " (simulator: slot details need bootctl)";
    if (!util::haveBinary("bootctl")) return s;
    for (int n = 0; n < 2; n++) {
        std::string out;
        bool bootable = util::runCapture("bootctl is-slot-bootable " + std::to_string(n), out) == 0;
        bool ok = util::runCapture("bootctl is-slot-marked-successful " + std::to_string(n), out) == 0;
        s += std::string(n ? "; " : " - ") + (n ? "B: " : "A: ") + (bootable ? "bootable" : "UNBOOTABLE") + (ok ? ", booted fine before" : ", never booted successfully");
    }
    return s;
}

}  // namespace

std::string decodePmsg(const std::string& raw) { return decodePmsgImpl(raw); }

std::vector<Finding> scan(const std::string& log, const std::string& bootReason, const std::string& androidReason) {
    std::vector<Finding> out;
    for (auto& r : kRules) {
        std::string ev = firstLine(log, r.any);
        if (ev.empty()) continue;
        if (ev.find("FATAL EXCEPTION IN SYSTEM PROCESS") != std::string::npos) {  // add the exception itself
            size_t at = log.find(ev);
            size_t nl = at == std::string::npos ? std::string::npos : log.find('\n', at);
            if (nl != std::string::npos) {
                size_t end = log.find('\n', nl + 1);
                ev += "\n" + util::trim(log.substr(nl + 1, end == std::string::npos ? std::string::npos : end - nl - 1)).substr(0, 300);
            }
        }
        out.push_back({r.severity, r.title, r.explanation, ev, r.fix});
    }
    std::string br = util::toLower(bootReason);
    auto has = [&](const char* t) { for (auto& f : out) if (f.title == std::string(t)) return true; return false; };
    if (out.empty() && br.find("kernel_panic") != std::string::npos)
        out.push_back({2, "The kernel crashed (panic)", "The bootloader recorded a kernel panic as the reason for the last reset. If it started "
                       "after flashing a kernel or boot image, restore the stock one.", "boot reason: " + bootReason, Fix::RestoreBoot});
    if (!has("The system froze (watchdog reset)") && br.find("watchdog") != std::string::npos)
        out.push_back({1, "The system froze (watchdog reset)", "The last reset came from the watchdog: something hung. Try the other slot.",
                       "boot reason: " + bootReason, Fix::SwitchSlot});
    auto rule = [&](const char* title, int severity, const std::string& ev) {
        if (has(title)) return;
        for (auto& r : kRules)
            if (std::string(r.title) == title) out.push_back({severity, r.title, r.explanation, ev, r.fix});
    };
    if (br.find("rescueparty") != std::string::npos) rule("Android gave up after repeated crashes (Rescue Party)", 2, "boot reason: " + bootReason);
    // What Android said when it sent the phone to recovery (bootloader message --reason=).
    std::string ar = util::toLower(androidReason);
    if (!ar.empty()) {
        std::string ev = "Android: --reason=" + androidReason;
        auto any = [&](std::initializer_list<const char*> ws) { for (auto w : ws) if (ar.find(w) != std::string::npos) return true; return false; };
        if (any({"rescueparty", "rescue_party"})) {
            rule("Android gave up after repeated crashes (Rescue Party)", 2, ev);
        } else if (any({"init_user0", "set_policy_failed", "enablefilecrypto", "fbe", "vold", "keystore", "encrypt", "fs_mgr_mount_all",
                        "mount_userdata", "userdata", "metadata"})) {
            rule("Android couldn't decrypt or mount /data", 2, ev);
        } else if (!has("Android sent the phone to recovery")) {
            out.push_back({1, "Android sent the phone to recovery", "Android stopped the boot itself and asked for recovery, with the reason "
                           "below. Auto repair tries the safe fixes first.", ev, Fix::None});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) { return a.severity > b.severity; });
    return out;
}

namespace {
std::string reasonFile() { return device::path("/tmp/fmrre/android_reason"); }
}  // namespace

void setAndroidReason(const std::string& reason) {
    util::mkdirs(util::dirname(reasonFile()));
    util::writeFile(reasonFile(), reason + "\n");
}

std::string androidReason() { return util::readLine(reasonFile()); }

Report analyze() {
    Report r;
    r.bootReason = device::prop("ro.boot.bootreason");
    r.androidReason = androidReason();
    for (auto& f : pstoreFiles()) {
        std::string b = util::basename(f);
        if (r.logSource.empty() && util::startsWith(b, "console-ramoops")) {
            r.logSource = f;
            util::readFile(f, r.log, 4 << 20);
        } else {
            r.otherLogs.push_back(f);
        }
    }
    if (r.logSource.empty() && util::exists("/proc/last_kmsg") && !device::isSim()) {
        r.logSource = "/proc/last_kmsg";
        util::readFile(r.logSource, r.log, 4 << 20);
    }
    std::string extra;  // panic dumps (dmesg-ramoops-N) and the Android log (pmsg-ramoops-N) count too
    for (auto& f : r.otherLogs) {
        std::string b = util::basename(f), d;
        if (b.find("dmesg-ramoops") == 0 && util::readFile(f, d, 1 << 20)) extra += d + "\n";
        if (b.find("pmsg-ramoops") == 0 && util::readFile(f, d, 8 << 20)) r.logcat += decodePmsg(d);
    }
    // The exception line that follows "FATAL EXCEPTION IN SYSTEM PROCESS" is the interesting part.
    extra += r.logcat;
    r.findings = scan(r.log + "\n" + extra, r.bootReason, r.androidReason);
    r.slotInfo = slotInfo();
    for (auto& e : history::recent(5)) {
        std::string d = e.detail;  // auto repair entries end with " [step id]" - not for people
        if (!d.empty() && d.back() == ']' && d.rfind(" [") != std::string::npos) d.erase(d.rfind(" ["));
        r.changes.push_back(e.action + ": " + d + (e.ok ? "" : " (failed)") + "  \xE2\x80\xA2  " + history::ago(e.time));
    }
    for (auto& m : bootimg::modules()) r.magiskModules |= !m.disabled;
    // Modules are the first thing to try for framework crashes when there are any.
    if (r.magiskModules) {
        bool crash = false;
        for (auto& f : r.findings) crash |= f.fix == Fix::FormatData && f.title.find("framework") != std::string::npos;
        r.findings.push_back({crash ? 1 : 0, "Magisk / KernelSU modules are enabled",
                              "If the bootloop began after adding or updating a module, disable all of them - they can be turned back on "
                              "in the Magisk app.", "", Fix::DisableModules});
    }
    if (r.logSource.empty())
        r.findings.push_back({0, "No log from the previous boot",
                              "This phone keeps no kernel log across reboots (no pstore / last_kmsg), or the previous boot was a clean "
                              "shutdown. Boot once more, then come back here right after the bootloop.", "", Fix::None});
    return r;
}

std::string saveLogs(const Report& r) {
    std::string dir = Partitions::get().appDir() + "/logs/" + util::dateStamp();
    if (!util::mkdirs(dir)) return "";
    std::vector<std::string> files = r.otherLogs;
    if (!r.logSource.empty()) files.push_back(r.logSource);
    for (auto& f : files) util::copyFile(f, dir + "/" + util::basename(f));
    util::copyFile(log::filePath(), dir + "/recovery.log");
    if (!r.logcat.empty()) util::writeFile(dir + "/logcat-previous-boot.txt", r.logcat);
    std::string summary = "boot reason: " + r.bootReason + "\n" + r.slotInfo + "\n\nrecent changes:\n";
    for (auto& c : r.changes) summary += "  " + c + "\n";
    summary += "\n";
    for (auto& f : r.findings) summary += f.title + "\n  " + f.evidence + "\n\n";
    util::writeFile(dir + "/diagnosis.txt", summary);
    return dir;
}

}  // namespace tw::rescue
