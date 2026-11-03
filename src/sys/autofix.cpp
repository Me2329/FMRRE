#include "sys/autofix.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <sys/statvfs.h>
#include <unistd.h>

#include "core/log.h"
#include "core/settings.h"
#include "core/sha256.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/avb.h"
#include "sys/bootimg.h"
#include "sys/history.h"
#include "sys/imagesrc.h"
#include "sys/partitions.h"
#include "sys/super.h"

namespace tw::autofix {

const char* riskName(Risk r) {
    switch (r) {
        case Risk::Safe: return "Safe";
        case Risk::Undoable: return "Can be undone";
        default: return "Erases data";
    }
}

namespace {

constexpr const char* kAction = "Auto repair";

std::string other(const std::string& slot) { return slot == "_b" ? "_a" : "_b"; }
std::string slotLetter(const std::string& slot) { return slot == "_b" ? "B" : "A"; }

std::string byName(const std::string& name) {
    std::string p = Partitions::byNameDir() + "/" + name;
    return util::exists(p) ? p : "";
}

std::string slotDev(const std::string& name, const std::string& slot) {
    std::string d = byName(name + slot);
    return d.empty() ? byName(name) : d;
}

// Newest stock image TWRP v2 saved for <name><slot> (before rooting: TWRP/root, before installing
// TWRP v2: TWRP/stock). `fromStock` tells which.
std::string stockImageFor(const std::string& name, const std::string& slot, bool* fromStock = nullptr) {
    std::string best;
    int64_t bestTime = -1;
    for (int k = 0; k < 2; k++) {
        std::string dir = k ? bootimg::stockDir() : bootimg::backupDir();
        for (auto& e : util::listDir(dir)) {
            if (e.dir || !util::endsWith(e.name, ".img") || !util::startsWith(e.name, name + slot + "_stock_")) continue;
            if (e.mtime > bestTime) {
                bestTime = e.mtime;
                best = dir + "/" + e.name;
                if (fromStock) *fromStock = k == 1;
            }
        }
    }
    return best;
}

std::string newestFile(const std::string& dir, const std::string& prefix) {
    std::string best;
    int64_t t = -1;
    for (auto& e : util::listDir(dir))
        if (!e.dir && util::startsWith(e.name, prefix) && e.mtime > t) { t = e.mtime; best = dir + "/" + e.name; }
    return best;
}

bool hasFinding(const rescue::Report& r, rescue::Fix f, int minSeverity = 1) {
    for (auto& x : r.findings)
        if (x.fix == f && x.severity >= minSeverity) return true;
    return false;
}

// A clear sign that the previous boot failed - not just a suspicious line (normal shutdowns log
// "watchdog did not stop" and the like).
bool failedFrom(const rescue::Report& r) {
    if (!r.androidReason.empty()) return true;
    for (auto& f : r.findings)
        if (f.severity >= 2) return true;
    std::string br = util::toLower(r.bootReason);
    for (auto k : {"kernel_panic", "watchdog", "rescueparty", "bootloop"})
        if (br.find(k) != std::string::npos) return true;
    return false;
}

std::string stepIdOf(const std::string& detail) {  // "Clear the caches [wipe_caches]"
    if (detail.empty() || detail.back() != ']') return "";
    size_t b = detail.rfind(" [");
    return b == std::string::npos ? "" : detail.substr(b + 2, detail.size() - b - 3);
}

std::string stripSlot(std::string n) {
    if (n.size() > 2 && (util::endsWith(n, "_a") || util::endsWith(n, "_b"))) n.resize(n.size() - 2);
    return n;
}

// The partition the last change wrote ("" if it wasn't a single partition).
std::string changedPartition(const history::Entry& e) {
    if (e.action == "Flash image") return stripSlot(e.detail.substr(0, e.detail.find(" <- ")));
    if (e.action == "fastboot flash") return stripSlot(e.detail.substr(0, e.detail.find(" (")));
    return "";
}

}  // namespace

bool lastBootFailed() {
    static int cached = -1;
    if (cached < 0) cached = failedFrom(rescue::analyze());
    return cached == 1;
}

Plan diagnose(bool userSaysBootFails) {
    Plan p;
    const device::Info& di = device::info();
    const std::string slot = di.slot;
    p.report = rescue::analyze();
    const rescue::Report& r = p.report;
    p.bootFailed = failedFrom(r) || userSaysBootFails;
    bool suspicious = p.bootFailed;
    for (auto& f : r.findings) suspicious |= f.severity >= 1 && f.fix != rescue::Fix::DisableModules;

    // ---- what earlier auto repairs did, and what was changed last
    std::set<std::string> tried;
    int64_t now = (int64_t)time(nullptr);
    std::vector<history::Entry> hist = history::recent(80);
    const history::Entry* last = nullptr;
    for (auto& e : hist) {
        if (e.action == kAction) {
            if (now - e.time < 86400) {
                p.earlierFixes++;
                tried.insert(stepIdOf(e.detail));
            }
        } else if (!last && e.ok && now - e.time < 7 * 86400 &&
                   !util::endsWith(e.detail, std::string(" (by ") + history::kByAutoRepair + ")")) {
            last = &e;
        }
    }
    std::string lastPart = last ? changedPartition(*last) : "";
    bool lastWasSystem = last && (last->action == "A/B OTA" || last->action == "fastboot update-super" ||
                                  last->action == "fastboot set_active" || lastPart == "system" || lastPart == "super");
    if (last) p.lastChange = last->action + ": " + last->detail + "  (" + history::ago(last->time) + ")";

    // ---- health checks
    auto check = [&](const std::string& t, const std::string& s, int level) { p.checks.push_back({t, s, level}); };
    std::map<std::string, bool> damaged;
    for (auto name : {"boot", "init_boot", "vendor_boot"}) {
        std::string dev = slotDev(name, slot);
        if (dev.empty()) continue;
        bootimg::Info bi = bootimg::read(dev);
        damaged[name] = !bi.ok;
        std::string ok = "OK" + (bi.patchLevel.empty() ? std::string() : ", patch " + bi.patchLevel);
        check(std::string("Boot image (") + name + (byName(name + slot).empty() ? "" : slot) + ")", bi.ok ? ok : "damaged - " + bi.error,
              bi.ok ? 0 : 2);
    }
    std::string vbDev = avb::vbmetaDevice(slot);
    avb::Info vb;
    if (!vbDev.empty()) {
        vb = avb::read(vbDev);
        check("Verified boot (" + util::basename(vbDev) + ")",
              !vb.ok ? "damaged - " + vb.error : vb.flags ? "verification off" : "verification on", vb.ok ? 0 : 2);
    }
    lp::Metadata m;
    std::string lpErr;
    bool haveSuper = util::exists(lp::superDevice());
    bool superOk = haveSuper && lp::read(lp::superDevice(), lp::currentSlotIndex(), m, lpErr);
    bool systemMissing = false, otherComplete = false;
    if (haveSuper) {
        check("Dynamic partitions (super)", superOk ? "OK, " + std::to_string(m.parts.size()) + " partitions" : "unreadable - " + lpErr,
              superOk ? 0 : 2);
        if (superOk && di.ab) {
            const lp::Part* sys = m.find("system" + slot);
            const lp::Part* sysO = m.find("system" + other(slot));
            systemMissing = !sys || sys->bytes() == 0;
            std::string bootO = byName("boot" + other(slot));
            otherComplete = sysO && sysO->bytes() > 0 && !bootO.empty() && bootimg::read(bootO).ok;
            check("System (slot " + slotLetter(slot) + ")", systemMissing ? "missing - this slot has no system" : "present",
                  systemMissing ? 2 : 0);
            check("Other slot (" + slotLetter(other(slot)) + ")", otherComplete ? "complete - boot and system present" : "incomplete", 0);
        }
    }
    std::string busyWhy;
    if (lp::busy(busyWhy)) check("System update", busyWhy, 1);
    Partitions& parts = Partitions::get();
    Partition* data = parts.find("/data");
    bool storageFull = false;
    if (data && parts.isMounted(*data) && !parts.dataEncrypted()) {
        struct statvfs vf;
        if (statvfs(data->contentPath().c_str(), &vf) == 0 && vf.f_blocks) {
            uint64_t freeB = uint64_t(vf.f_bavail) * vf.f_frsize, total = uint64_t(vf.f_blocks) * vf.f_frsize;
            storageFull = freeB < std::min<uint64_t>(300ull << 20, total / 50);
            check("Storage space (/data)", util::humanSize(freeB) + " / " + util::humanSize(total) + (storageFull ? " free - too full to boot" : " free"),
                  storageFull ? 2 : 0);
        }
    } else if (data) {
        check("Storage space (/data)", parts.dataEncrypted() ? "encrypted - not readable here" : "not mounted", 1);
    }
    int enabled = 0;
    for (auto& mod : bootimg::modules()) enabled += !mod.disabled;
    if (enabled) check("Magisk / KernelSU modules", std::to_string(enabled) + " enabled", p.bootFailed ? 1 : 0);
    device::Battery bat = device::battery();
    if (bat.percent >= 0)
        check("Battery", std::to_string(bat.percent) + "%" + (bat.charging ? ", charging" : ""), bat.percent < 15 && !bat.charging ? 1 : 0);

    // ---- the plan, safest first
    bool escalate = p.bootFailed && p.earlierFixes > 0;  // an earlier auto repair didn't help
    auto add = [&](const std::string& id, const std::string& title, const std::string& why, Risk risk, bool recommend,
                   const std::string& file = "") {
        Step s{id, title, why, risk, false, tried.count(id) > 0, file};
        s.recommended = recommend && !s.triedBefore;
        p.steps.push_back(s);
    };
    bool kernelCrash = hasFinding(r, rescue::Fix::RestoreBoot);
    bool dataProblem = hasFinding(r, rescue::Fix::FormatData, 2);
    if (enabled)
        add("disable_modules", "Disable all Magisk / KernelSU modules",
            "A module is the most common cause of a bootloop on a rooted phone. They are only disabled: turn them back on in the Magisk app.",
            Risk::Undoable, p.bootFailed || hasFinding(r, rescue::Fix::DisableModules));
    if (suspicious || storageFull)
        add("wipe_caches", "Clear the app caches (ART / Dalvik and /cache)",
            storageFull ? "Frees space on a full /data. Android rebuilds the caches on the next boot (the first boot takes longer)."
                        : "Stale compiled app code after an update can crash Android at boot. It is rebuilt automatically; no apps or files are lost.",
            Risk::Safe, p.bootFailed || storageFull);
    if (vb.ok && vb.flags == 0 && hasFinding(r, rescue::Fix::DisableVerity))
        add("disable_verity", "Disable verified boot (vbmeta)",
            "Verified boot stopped the last boot: the system image is modified or not the one vbmeta expects (normal after a GSI).",
            Risk::Undoable, true);
    if (!vbDev.empty() && !vb.ok) {
        std::string f = newestFile(parts.storagePath() + "/TWRP/vbmeta", util::basename(vbDev) + "_header_");
        if (!f.empty())
            add("restore_vbmeta", "Restore the saved vbmeta header", "vbmeta is damaged; TWRP v2 saved its header before changing it.",
                Risk::Undoable, true, f);
    }
    if (haveSuper && !superOk) {
        std::string f = newestFile(lp::backupDir(), "super_metadata_");
        if (!f.empty())
            add("restore_super", "Restore the last dynamic partition table", "The partition table in super is unreadable; TWRP v2 saved a copy.",
                Risk::Undoable, true, f);
    }
    for (auto name : {"boot", "init_boot", "vendor_boot"}) {
        bool fromStock = false;
        std::string f = stockImageFor(name, slot, &fromStock);
        bool changedLast = lastPart == name || (last && last->action == "Install TWRP v2" && fromStock);
        if (f.empty() || !(damaged[name] || kernelCrash || changedLast)) continue;
        std::string why = damaged[name] ? std::string("The ") + name + " image is damaged; TWRP v2 saved the original."
                          : changedLast ? std::string("The phone stopped starting after ") + name + " was changed; this puts the original back."
                                        : "The kernel crashed; if a kernel, boot image or root patch caused it, the original image undoes it.";
        bool rec = damaged[name] || (changedLast && (kernelCrash || !enabled || escalate)) || (kernelCrash && escalate);
        add(std::string("restore_") + name, std::string("Restore the original ") + name + slot, why, Risk::Undoable, rec && p.bootFailed, f);
        if (fromStock && name != std::string("init_boot")) p.steps.back().note = "This also removes TWRP v2 from " + std::string(name) + ".";
    }
    if (di.ab && otherComplete && (systemMissing || p.bootFailed))
        add("switch_slot", "Start the other slot (" + slotLetter(other(slot)) + ")",
            systemMissing   ? "This slot has no system; the other one is complete."
            : lastWasSystem ? "A new system was installed to this slot and it doesn't start; the other slot still has the previous one."
                            : "The other slot still has a complete system - usually the version that worked before.",
            Risk::Undoable, systemMissing || lastWasSystem || escalate || hasFinding(r, rescue::Fix::SwitchSlot, 2));
    if (data && (dataProblem || (escalate && !parts.isMounted(*data))))
        add("repair_data_fs", "Check and repair the /data file system",
            "Android couldn't mount or unlock /data. A file system check fixes damage from a crash or a full disk without erasing anything.",
            Risk::Safe, true);
    if (data && (dataProblem || hasFinding(r, rescue::Fix::FormatData) || escalate))
        add("format_data", "Format Data (factory reset)",
            "Fixes encryption errors and leftovers from another ROM - the last resort. Erases apps, settings and internal storage: copy your "
            "files to a computer or SD card first.",
            Risk::ErasesData, dataProblem && escalate);

    // ---- what only the user can do
    if (hasFinding(r, rescue::Fix::ReflashSystem))
        p.advice.push_back("The system image doesn't fit this phone: flash the ROM or GSI again (for GSIs, the arm64 A/B build), or go back to "
                           "the stock firmware.");
    bool safeLeft = false;
    for (auto& s : p.steps) safeLeft |= !s.triedBefore && s.risk != Risk::ErasesData;
    if (p.bootFailed && !safeLeft)
        p.advice.push_back(p.earlierFixes ? "The safe fixes have been tried. What's left erases data: Format Data (copy your files first), or "
                                            "flash the stock firmware - from a computer with the TWRP v2 PC rescue tool if needed."
                                          : "No safe automatic fix applies. Format Data (copy your files first), or flash the ROM / stock "
                                            "firmware again - from a computer with the TWRP v2 PC rescue tool if needed.");

    // ---- one line for the top of the screen
    if (p.bootFailed) {
        std::string what;
        for (auto& f : r.findings)
            if (f.severity >= 1) { what = f.title; break; }
        if (what.empty() && !r.androidReason.empty()) what = "Android reported " + r.androidReason;
        if (what.empty() && userSaysBootFails) what = "no log was kept, so the usual bootloop fixes are planned";
        p.cause = what;
        p.headline = "The last boot failed" + (what.empty() ? std::string() : ": " + what);
    } else {
        int bad = 0;
        for (auto& c : p.checks) bad += c.level >= 2;
        p.headline = bad ? "Found " + std::to_string(bad) + " problem(s)" : "No failed boot recorded, and everything checked looks fine";
    }
    return p;
}

bool apply(const Plan& p, const Options& o, Task& t) {
    std::vector<const Step*> todo;
    for (auto& s : p.steps) {
        bool chosen = o.only.empty() ? s.recommended : o.only.count(s.id) > 0;
        if (!chosen) continue;
        if (s.id == "switch_slot" && o.only.empty() && !o.allowSlotSwitch) {
            t.print("Not switching slots (allow it with --allow-slot-switch): %s", s.title.c_str());
            continue;
        }
        if (s.risk == Risk::ErasesData && !o.allowFormatData) {
            t.print("Skipped - erases data (allow it with --allow-format-data): %s", s.title.c_str());
            continue;
        }
        todo.push_back(&s);
    }
    t.print("%s", p.headline.c_str());
    if (todo.empty()) {
        t.print("Nothing to repair.");
        for (auto& a : p.advice) t.print("%s", a.c_str());
        return false;
    }
    bool allOk = true;
    const std::string slot = device::info().slot;
    Partitions& parts = Partitions::get();
    for (size_t i = 0; i < todo.size(); i++) {
        const Step& s = *todo[i];
        t.setRange(float(i) / todo.size(), 1.0f / todo.size());
        t.setSubProgress(0);
        t.setStatus(s.title);
        t.print("[%zu/%zu] %s", i + 1, todo.size(), s.title.c_str());
        bool ok = false;
        std::string err;
        history::By by(history::kByAutoRepair);  // the wipes / flashes below are ours, not "the last change"
        if (s.id == "disable_modules") {
            int n = bootimg::disableAllModules();
            t.print("  %d module(s) disabled", n);
            ok = true;
        } else if (s.id == "wipe_caches") {
            ok = parts.wipeDalvik(t);
            Partition* c = parts.find("/cache");
            if (c && c->present) ok = parts.wipe(*c, t) && ok;
        } else if (s.id == "disable_verity") {
            ok = avb::disableVerification(false, t);
        } else if (s.id == "restore_vbmeta") {
            std::string dev = avb::vbmetaDevice(slot), hdr;
            if (util::readFile(s.file, hdr, 1 << 20) && !hdr.empty()) {
                int fd = ::open(dev.c_str(), O_WRONLY | O_CLOEXEC);
                ok = fd >= 0 && pwrite(fd, hdr.data(), hdr.size(), 0) == (ssize_t)hdr.size() && fsync(fd) == 0;
                if (fd >= 0) close(fd);
                ok = ok && avb::read(dev).ok;
            }
            if (!ok) t.print("  could not restore %s", dev.c_str());
        } else if (s.id == "restore_super") {
            ok = lp::restoreMetadataRegion(lp::superDevice(), s.file, err);
            if (!ok) t.print("  %s", err.c_str());
        } else if (util::startsWith(s.id, "restore_")) {
            std::string dev = slotDev(s.id.substr(8), slot);
            if (!dev.empty() && Sha256::hexOfFile(dev) == Sha256::hexOfFile(s.file)) {
                t.print("  already the original image");
                ok = true;
            } else {
                img::FlashOptions fo;
                fo.verify = true;
                ok = !dev.empty() && img::flash(s.file, "", dev, fo, t);
            }
        } else if (s.id == "switch_slot") {
            std::string msg;
            ok = device::switchSlot(msg);
            t.print("  %s", msg.c_str());
        } else if (s.id == "repair_data_fs") {
            Partition* d = parts.find("/data");
            ok = d && parts.repair(*d, t);
            if (d) parts.mount(*d);
        } else if (s.id == "format_data") {
            Partition* d = parts.find("/data");
            ok = d && parts.format(*d, t);
        }
        {
            history::By none(nullptr);
            history::record(kAction, s.title + " [" + s.id + "]", ok);
        }
        t.print("  %s", ok ? "done" : "FAILED");
        allOk = allOk && ok;
    }
    t.setRange(0, 1);
    t.setProgress(1);
    t.print(allOk ? "Repairs done. If the phone still doesn't start, come back here: the next run tries the next fix."
                  : "Some repairs failed - see above.");
    if (o.reboot && allOk) {
        t.print("Rebooting to system in 3 seconds...");
        sleep(3);
        device::reboot("");
    }
    return allOk;
}

bool autoRunAllowed() {
    int64_t now = (int64_t)time(nullptr);
    int n = 0;
    for (auto& v : util::split(Settings::get().str("autofix_runs"), ','))
        if (now - atoll(v.c_str()) < 86400) n++;
    return n < 3;
}

void noteAutoRun() {
    int64_t now = (int64_t)time(nullptr);
    std::string keep;
    for (auto& v : util::split(Settings::get().str("autofix_runs"), ','))
        if (now - atoll(v.c_str()) < 86400) keep += v + ",";
    Settings::get().set("autofix_runs", keep + std::to_string(now));
    Settings::get().save();
}

}  // namespace tw::autofix
