#include "sys/ors.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "core/log.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/avb.h"
#include "sys/backup.h"
#include "sys/bootimg.h"
#include "sys/imagesrc.h"
#include "sys/installer.h"
#include "sys/partitions.h"
#include "sys/pathcheck.h"
#include "sys/sideload.h"
#include "sys/super.h"

namespace tw::ors {

namespace {

constexpr const char* kOrsFiles[] = {"/cache/recovery/openrecoveryscript", "/tmp/openrecoveryscript"};
constexpr const char* kAospCommand = "/cache/recovery/command";

// bootloader_message (bootable/recovery/bootloader_message): command[32] status[32] recovery[768]
constexpr size_t kBcbCommand = 0, kBcbRecovery = 64, kBcbRecoveryLen = 768, kBcbClearLen = 832;

std::string miscDevice() {
    std::string p = Partitions::byNameDir() + "/misc";
    return util::exists(p) ? p : "";
}

bool readBcb(std::vector<std::string>& args) {
    std::string dev = miscDevice();
    if (dev.empty()) return false;
    int fd = open(dev.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[kBcbClearLen] = {};
    bool ok = pread(fd, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf);
    close(fd);
    if (!ok || strncmp(buf + kBcbCommand, "boot-recovery", 13) != 0) return false;
    std::string rec(buf + kBcbRecovery, strnlen(buf + kBcbRecovery, kBcbRecoveryLen));
    auto lines = util::split(rec, '\n');
    for (size_t i = 0; i < lines.size(); i++)
        if (!(i == 0 && lines[i] == "recovery") && util::startsWith(lines[i], "--")) args.push_back(lines[i]);
    return true;
}

void clearBcb() {
    std::string dev = miscDevice();
    if (dev.empty()) return;
    int fd = open(dev.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    char zero[kBcbClearLen] = {};
    if (pwrite(fd, zero, sizeof(zero), 0) == (ssize_t)sizeof(zero)) fsync(fd);
    close(fd);
    LOGI("ors: cleared the bootloader message");
}

std::string slot() { return device::info().slot; }

std::string resolveTarget(const std::string& name) {
    if (util::startsWith(name, "/dev/")) return device::path(name);
    std::string n = name[0] == '/' ? name.substr(1) : name;
    if (Partition* p = Partitions::get().find("/" + n)) return p->blockDev;
    if (n == "system")
        if (Partition* p = Partitions::get().find("/system_root")) return p->blockDev;
    for (auto c : {n, n + slot()}) {
        std::string m = lp::mappedPath(c);
        if (!m.empty()) return m;
    }
    std::string by = Partitions::byNameDir();
    for (auto c : {n + slot(), n})
        if (util::exists(by + "/" + c)) return by + "/" + c;
    return "";
}

std::vector<std::string> mountPointsForLetters(const std::string& letters) {
    std::vector<std::string> mps;
    auto add = [&](std::initializer_list<const char*> c) {
        for (auto mp : c)
            if (Partition* p = Partitions::get().find(mp)) { mps.push_back(p->mountPoint); return; }
    };
    for (char ch : letters) {
        switch (toupper((unsigned char)ch)) {
            case 'S': add({"/system_root", "/system"}); break;
            case 'D': add({"/data"}); break;
            case 'C': add({"/cache"}); break;
            case 'R': add({"/recovery"}); break;
            case 'B': add({"/boot"}); break;
            case 'I': add({"/init_boot"}); break;
            case 'V': add({"/vendor"}); break;
            case 'E': add({"/efs"}); break;
            case 'P': add({"/persist"}); break;
            default: break;  // O (compress), M (no digest) and unknown letters are options
        }
    }
    return mps;
}

bool doWipe(const std::string& what, Task& t) {
    if (what == "dalvik") return Partitions::get().wipeDalvik(t);
    if (what == "data") {
        bool ok = true;
        for (auto mp : {"/data", "/cache"})
            if (Partition* p = Partitions::get().find(mp)) ok &= Partitions::get().wipe(*p, t);
        return Partitions::get().wipeDalvik(t) && ok;
    }
    Partition* p = Partitions::get().find("/" + what);
    if (!p && what == "system") p = Partitions::get().find("/system_root");
    if (!p) { t.print("wipe: unknown partition '%s'", what.c_str()); return false; }
    return Partitions::get().wipe(*p, t);
}

bool doGsi(const std::string& path, Task& t) {
    img::Info info = img::inspect(path);
    if (!info.ok) { t.print("gsi: %s", info.error.c_str()); return false; }
    std::string sys = "system" + slot(), dev;
    img::FlashOptions o;
    o.verify = true;
    lp::Metadata m;
    std::string e;
    if (lp::read(lp::superDevice(), lp::currentSlotIndex(), m, e) && m.find(sys)) {
        if (info.imageSize > m.find(sys)->bytes() && !lp::resizePartition(sys, info.imageSize, t)) return false;
        if (!lp::read(lp::superDevice(), lp::currentSlotIndex(), m, e)) { t.print("%s", e.c_str()); return false; }
        if (lp::mappedPath(sys).empty() && !lp::map(m, sys, true, e)) { t.print("map %s: %s", sys.c_str(), e.c_str()); return false; }
        dev = lp::mappedPath(sys);
        o.limit = m.find(sys)->bytes();
    } else {
        dev = resolveTarget("system");
    }
    if (dev.empty()) { t.print("gsi: no system partition"); return false; }
    if (!img::flash(path, "", dev, o, t)) return false;
    avb::disableVerification(false, t);
    return true;
}

std::string pending(const std::string& f) {
    std::string data;
    util::readFile(device::path(f), data, 1 << 20);
    return data;
}

}  // namespace

std::vector<std::string> fromAospArgs(const std::vector<std::string>& args) {
    std::vector<std::string> out;
    for (auto& a : args) {
        std::string v = a.find('=') == std::string::npos ? "" : a.substr(a.find('=') + 1);
        if (util::startsWith(a, "--update_package=")) {
            if (util::startsWith(v, "@")) out.push_back("print Block-map OTA packages (" + v + ") are not supported - flash the zip from storage.");
            else out.push_back("install " + v);
        } else if (a == "--wipe_data" || a == "--prompt_and_wipe_data") {
            out.push_back("wipe data");
        } else if (a == "--wipe_cache") {
            out.push_back("wipe cache");
        } else if (a == "--sideload" || a == "--sideload_auto_reboot") {
            out.push_back("sideload");
            if (a == "--sideload_auto_reboot") out.push_back("reboot");
        } else if (a == "--shutdown_after") {
            out.push_back("reboot poweroff");
        }
    }
    return out;
}

bool takePending(Script& out) {
    for (auto f : kOrsFiles) {
        std::string d = pending(f);
        if (d.empty()) continue;
        unlink(device::path(f).c_str());
        out.source = f;
        for (auto& l : util::split(d, '\n'))
            if (!util::trim(l).empty() && util::trim(l)[0] != '#') out.lines.push_back(util::trim(l));
        clearBcb();
        return !out.lines.empty();
    }
    std::vector<std::string> args;
    std::string cmd = pending(kAospCommand);
    if (!cmd.empty()) {
        for (auto& l : util::split(cmd, '\n')) args.push_back(util::trim(l));
        unlink(device::path(kAospCommand).c_str());
        out.source = kAospCommand;
    }
    if (readBcb(args) && out.source.empty()) out.source = "bootloader message (misc)";
    clearBcb();
    out.lines = fromAospArgs(args);
    return !out.lines.empty();
}

std::string help() {
    return "Commands (OpenRecoveryScript + TWRP v2):\n"
           "  install <zip>             flash a zip\n"
           "  flash <image> <partition> flash any image (.img .xz .gz .zst .zip, sparse)\n"
           "  gsi <image>               resize system, flash GSI, disable AVB\n"
           "  recovery <image>          install TWRP v2 permanently (stock recovery/boot saved first)\n"
           "  wipe <data|cache|dalvik|system|...>\n"
           "  format data\n"
           "  backup <SDCRBIVEP>[O][M] [name]   O=compress M=no digest\n"
           "  restore <name> [letters]\n"
           "  mount|unmount <mountpoint>\n"
           "  mkdir <path>\n"
           "  cmd <shell command>\n"
           "  sideload\n"
           "  lp resize <name> <MB> | lp delete <name> | lp list\n"
           "  vbmeta disable|enable|status\n"
           "  root status|backup|modules-off   modules-off fixes module bootloops\n"
           "  check <path>              why can't I see this file?\n"
           "  dd <image> <partition>    print the equivalent dd command\n"
           "  print <text>, sleep <seconds>\n"
           "  reboot [recovery|bootloader|fastboot|edl|download|poweroff]\n";
}

bool run(const std::vector<std::string>& lines, Task& t) {
    std::string rebootTo;
    bool reboot = false;
    for (size_t i = 0; i < lines.size(); i++) {
        std::string line = util::trim(lines[i]);
        if (line.empty() || line[0] == '#') continue;
        t.setRange(float(i) / lines.size(), 1.0f / lines.size());
        t.setSubProgress(0);
        auto sp = line.find(' ');
        std::string cmd = util::toLower(line.substr(0, sp)), rest = sp == std::string::npos ? "" : util::trim(line.substr(sp + 1));
        auto a = util::splitWs(rest);
        t.print("> %s", line.c_str());
        t.setStatus(line);
        bool ok = true;
        if (cmd == "install") {
            std::string p = pathcheck::resolve(rest);
            if (img::isImageName(p) && !util::endsWith(util::toLower(p), ".zip")) {
                t.print("'%s' is an image: use 'flash <image> <partition>'", rest.c_str());
                ok = false;
            } else {
                installer::InstallOptions o;
                ok = installer::installZip(p, o, t);
            }
        } else if (cmd == "flash") {
            if (a.size() < 2) { t.print("usage: flash <image> <partition>"); ok = false; }
            else {
                std::string dev = resolveTarget(a[1]);
                if (dev.empty()) { t.print("unknown partition %s", a[1].c_str()); ok = false; }
                else {
                    img::FlashOptions o;
                    o.verify = true;
                    ok = img::flash(pathcheck::resolve(a[0]), "", dev, o, t);
                }
            }
        } else if (cmd == "gsi") {
            ok = doGsi(pathcheck::resolve(rest), t);
        } else if (cmd == "wipe") {
            ok = doWipe(util::toLower(rest), t);
        } else if (cmd == "format") {
            Partition* p = Partitions::get().find("/" + (rest.empty() ? std::string("data") : rest));
            ok = p && Partitions::get().format(*p, t);
            if (!p) t.print("format: unknown partition");
        } else if (cmd == "backup") {
            backup::Options o;
            std::string letters = a.empty() ? "SDB" : a[0];
            o.parts = mountPointsForLetters(letters);
            o.compress = letters.find_first_of("Oo") != std::string::npos;
            o.digest = letters.find_first_of("Mm") == std::string::npos;
            o.name = a.size() > 1 ? a[1] : util::dateStamp();
            ok = !o.parts.empty() && backup::create(o, t);
        } else if (cmd == "restore") {
            ok = false;
            for (auto& s : backup::list())
                if (!a.empty() && (s.name == a[0] || util::basename(s.path) == a[0])) {
                    std::vector<std::string> names;
                    if (a.size() > 1)
                        for (auto& mp : mountPointsForLetters(a[1])) names.push_back(Partitions::get().find(mp)->name);
                    else
                        for (auto& it : s.items) names.push_back(it.part);
                    ok = backup::restore(s, names, true, t);
                    break;
                }
            if (!ok && !a.empty()) t.print("restore of '%s' failed or backup not found", a[0].c_str());
        } else if (cmd == "mount" || cmd == "unmount" || cmd == "umount") {
            Partition* p = Partitions::get().find(rest);
            if (!p) { t.print("unknown mount point %s", rest.c_str()); ok = false; }
            else ok = cmd == "mount" ? Partitions::get().mount(*p) : Partitions::get().unmount(*p);
        } else if (cmd == "mkdir") {
            ok = util::mkdirs(pathcheck::resolve(rest));
        } else if (cmd == "cmd") {
            int rc = util::run(rest, [&](const std::string& l) { t.print("%s", l.c_str()); });
            ok = rc == 0;
        } else if (cmd == "print") {
            t.print("%s", rest.c_str());
        } else if (cmd == "sleep") {
            sleep((unsigned)std::max(0, atoi(rest.c_str())));
        } else if (cmd == "sideload") {
            std::string pkg = sideload::receive(t);
            installer::InstallOptions o;
            ok = !pkg.empty() && installer::installZip(pkg, o, t);
        } else if (cmd == "lp") {
            if (a.size() >= 3 && a[0] == "resize") ok = lp::resizePartition(a[1], uint64_t(atof(a[2].c_str()) * (1 << 20)), t);
            else if (a.size() >= 2 && a[0] == "delete") ok = lp::deletePartition(a[1], t);
            else if (a.size() >= 1 && a[0] == "free-other-slot") ok = lp::freeOtherSlot(t);
            else {
                lp::Metadata m;
                std::string e;
                ok = lp::read(lp::superDevice(), lp::currentSlotIndex(), m, e);
                if (!ok) t.print("%s", e.c_str());
                for (auto& p : m.parts) t.print("%-20s %10s  %zu extent(s)", p.name.c_str(), util::humanSize(p.bytes()).c_str(), p.extents.size());
                if (ok) t.print("free: %s of %s", util::humanSize(m.freeBytes()).c_str(), util::humanSize(m.totalBytes()).c_str());
            }
        } else if (cmd == "vbmeta") {
            if (rest == "disable") ok = avb::disableVerification(true, t);
            else if (rest == "enable") ok = avb::enableVerification(true, t);
            else {
                avb::Info i = avb::read(avb::vbmetaDevice(slot()));
                t.print(i.ok ? "vbmeta%s flags=%u" : "vbmeta%s: unavailable", slot().c_str(), i.flags);
            }
        } else if (cmd == "recovery") {
            if (rest.empty()) { t.print("usage: recovery <twrpv2-recovery.img>"); ok = false; }
            else ok = bootimg::installRecovery(pathcheck::resolve(rest), t);
        } else if (cmd == "root") {
            if (rest == "modules-off") {
                t.print("Disabled %d module(s)", bootimg::disableAllModules());
            } else if (rest == "backup") {
                ok = bootimg::backupStock(bootimg::rootTarget(), t);
            } else {
                auto st = bootimg::detectRoot();
                auto tg = bootimg::rootTarget();
                t.print("%s%s", bootimg::rootName(st.kind), st.detail.empty() ? "" : (" (" + st.detail + ")").c_str());
                t.print("ramdisk target: %s%s", tg.name.c_str(), device::info().slot.c_str());
                for (auto& m : bootimg::modules()) t.print("  module %-24s %s%s", m.id.c_str(), m.disabled ? "disabled" : "enabled", m.removing ? ", removing" : "");
            }
        } else if (cmd == "check") {
            auto r = pathcheck::check(rest);
            t.print("%s -> %s", r.input.c_str(), r.resolved.c_str());
            for (auto& f : r.findings) t.print("  %s", f.text.c_str());
            for (auto& s : r.suggestions) t.print("  maybe: %s", s.c_str());
            ok = r.usable();
        } else if (cmd == "dd") {
            if (a.size() < 2) { t.print("usage: dd <image> <partition>"); ok = false; }
            else {
                auto r = pathcheck::check(a[0]);
                std::string dev = resolveTarget(a[1]);
                if (!r.usable() || dev.empty()) { t.print("file or partition not found (try: check %s)", a[0].c_str()); ok = false; }
                else {
                    img::Info i = img::inspect(r.resolved);
                    i.path = r.resolved;
                    auto d = img::ddCommand(i, dev);
                    for (auto& l : d.lines) t.print("%s", l.c_str());
                    for (auto& n : d.notes) t.print("# %s", n.c_str());
                }
            }
        } else if (cmd == "reboot") {
            reboot = true;
            rebootTo = rest == "system" ? "" : rest;
        } else if (cmd == "set" || cmd == "remountrw" || cmd == "fixperms") {
            t.print("(ignored: %s)", cmd.c_str());
        } else if (cmd == "fixcontexts") {
            ok = util::run("restorecon -RF " + util::shellQuote(Partitions::get().storagePath())) == 0;
        } else if (cmd == "decrypt") {
            t.print("decrypt: not supported yet (no FBE/FDE decryption in TWRP v2)");
            ok = false;
        } else if (cmd == "help") {
            for (auto& l : util::split(help(), '\n')) t.print("%s", l.c_str());
        } else {
            t.print("Unknown command '%s' (try: help)", cmd.c_str());
            ok = false;
        }
        if (!ok) {
            t.print("Script stopped: '%s' failed.", line.c_str());
            return false;
        }
    }
    t.setRange(0, 1);
    t.setProgress(1);
    if (reboot) {
        t.print("Rebooting%s in 3 seconds...", rebootTo.empty() ? "" : (" to " + rebootTo).c_str());
        sleep(3);
        device::reboot(rebootTo);
    }
    return true;
}

// ---- CLI --------------------------------------------------------------------

std::string queueDir() { return device::path("/tmp/twrpv2/cli"); }
std::string uiPidFile() { return device::path("/tmp/twrpv2/ui.pid"); }

int cliMain(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help") {
        printf("usage: twrp <command> [args]\n%s", help().c_str());
        return args.empty() ? 1 : 0;
    }
    std::string line;
    for (auto& a : args) line += (line.empty() ? "" : " ") + a;
    std::string pid = util::readLine(uiPidFile());
    bool uiAlive = !pid.empty() && kill(atoi(pid.c_str()), 0) == 0;
    bool readOnly = (args[0] == "root" && (args.size() == 1 || args[1] == "status")) || args[0] == "check" || args[0] == "dd" || args[0] == "help" || (args[0] == "lp" && (args.size() == 1 || args[1] == "list")) ||
                    (args[0] == "vbmeta" && (args.size() == 1 || args[1] == "status"));
    if (uiAlive && !readOnly) {
        util::mkdirs(queueDir());
        std::string id = std::to_string(getpid()) + "_" + std::to_string(util::nowMs());
        std::string base = queueDir() + "/" + id;
        util::writeFile(base + ".tmp", line + "\n");
        rename((base + ".tmp").c_str(), (base + ".cmd").c_str());
        fprintf(stderr, "Queued for the recovery UI: %s\n", line.c_str());
        std::string done;
        for (int64_t start = util::nowMs(); !util::readFile(base + ".done", done); usleep(250 * 1000)) {
            if (util::nowMs() - start > 6 * 3600 * 1000) { fprintf(stderr, "timed out\n"); return 2; }
        }
        unlink((base + ".done").c_str());
        auto nl = done.find('\n');
        int rc = atoi(done.substr(0, nl).c_str());
        if (nl != std::string::npos) fputs(done.c_str() + nl + 1, stdout);
        return rc;
    }
    Task& t = Task::get();
    t.start("twrp " + line, [line](Task& tt) { return run({line}, tt); });
    size_t cursor = 0;
    auto flush = [&]() {
        for (auto& l : t.linesSince(cursor)) printf("%s\n", l.c_str());
        fflush(stdout);
    };
    while (t.running()) {
        flush();
        usleep(100 * 1000);
    }
    t.join();
    flush();
    return t.success() ? 0 : 1;
}

}  // namespace tw::ors
