#include "sys/logsave.h"

#include "core/log.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"
#include "sys/pathcheck.h"

#include <mutex>
#include <set>
#include <sys/klog.h>
#include <unistd.h>

namespace tw {
namespace logsave {

namespace {

std::mutex g_mu;
bool g_rotated = false;  // older sessions' files are shifted once per run, then this run's are rewritten in place

// last_log -> last_log.1 -> ... -> last_log.4 (what AOSP's recovery does, so the stock log viewer finds them)
void rotate(const std::string& f) {
    unlink((f + ".4").c_str());
    for (int i = 3; i >= 1; i--) rename((f + "." + std::to_string(i)).c_str(), (f + "." + std::to_string(i + 1)).c_str());
    rename(f.c_str(), (f + ".1").c_str());
}

// One value out of an audit line: name="brightness" -> brightness, tclass=file -> file
std::string field(const std::string& line, const std::string& key) {
    size_t p = line.find(" " + key + "=");
    if (p == std::string::npos) return "";
    p += key.size() + 2;
    if (p < line.size() && line[p] == '"') {
        size_t e = line.find('"', p + 1);
        return e == std::string::npos ? "" : line.substr(p + 1, e - p - 1);
    }
    size_t e = line.find(' ', p);
    return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

}  // namespace

std::string kernelLog(size_t maxBytes) {
    if (device::isSim()) {  // the sandbox plays a kernel log from a file
        std::string s;
        util::readFile(device::path("/proc/kmsg.txt"), s, maxBytes);
        return s;
    }
    int size = klogctl(10 /* SYSLOG_ACTION_SIZE_BUFFER */, nullptr, 0);
    if (size <= 0) size = 256 << 10;
    size = (int)std::min<size_t>(size_t(size), maxBytes);
    std::string buf(size_t(size), '\0');
    int got = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, &buf[0], size);
    if (got <= 0) return "";
    buf.resize(size_t(got));
    return buf;
}

std::string selinuxMode() {
    std::string v = util::trim(util::readLine(device::path("/sys/fs/selinux/enforce")));
    return v == "1" ? "enforcing" : v == "0" ? "permissive" : "";
}

std::vector<std::string> selinuxDenials(const std::string& kmsg) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (auto& line : util::split(kmsg, '\n')) {
        size_t avc = line.find("avc:");
        if (avc == std::string::npos || line.find("denied", avc) == std::string::npos) continue;
        // Ours: the process is the recovery program (FMRRE takes its place and its SELinux domain).
        std::string comm = field(line, "comm"), scon = field(line, "scontext");
        if (comm != "recovery" && comm != "fmrre" && scon.find(":recovery:") == std::string::npos) continue;
        size_t b0 = line.find('{', avc), b1 = line.find('}', avc);
        std::string perms = b0 != std::string::npos && b1 != std::string::npos && b1 > b0 ? util::trim(line.substr(b0 + 1, b1 - b0 - 1)) : "?";
        std::string what = field(line, "path");
        if (what.empty()) what = field(line, "name");
        std::string tcon = field(line, "tcontext"), tclass = field(line, "tclass");
        // u:object_r:sysfs_leds:s0 -> sysfs_leds
        auto parts = util::split(tcon, ':');
        std::string type = parts.size() >= 3 ? parts[2] : tcon;
        std::string s = perms + " " + tclass + (what.empty() ? "" : " " + what) + "  (" + type + ")";
        if (field(line, "permissive") == "1") s += "  [allowed: permissive]";
        if (seen.insert(s).second && out.size() < 60) out.push_back(s);
    }
    return out;
}

std::string selinuxReport(const std::string& kmsg) {
    std::string mode = selinuxMode();
    auto den = selinuxDenials(kmsg);
    std::string s = "SELinux: " + (mode.empty() ? std::string("not in this kernel") : mode) + "\n";
    if (den.empty()) return s + "Nothing was refused to FMRRE.\n";
    s += std::to_string(den.size()) + " kind(s) of action refused to FMRRE (permission, class, target, SELinux type):\n";
    for (auto& d : den) s += "  " + d + "\n";
    return s;
}

std::vector<std::string> save(const char* why, Removable removable) {
    std::lock_guard<std::mutex> lock(g_mu);
    std::vector<std::string> where;
    std::string log, kmsg = kernelLog();
    util::readFile(log::filePath(), log, 8 << 20);
    std::string report = selinuxReport(kmsg);
    std::string tail = std::string("\n--- saved: ") + why + " ---\n" + report;
    Partitions& P = Partitions::get();
    bool rotateNow = !g_rotated;

    // 1. /cache/recovery: where AOSP's recovery keeps its logs, and where a stock recovery shows them from.
    if (Partition* c = P.find("/cache")) {
        if (!c->isImage() && c->present && P.mount(*c)) {
            std::string dir = c->contentPath() + "/recovery";
            if (util::mkdirs(dir)) {
                if (rotateNow) {
                    rotate(dir + "/last_log");
                    rotate(dir + "/last_kmsg");
                }
                bool ok = util::writeFile(dir + "/last_log", log + tail);
                if (!kmsg.empty()) util::writeFile(dir + "/last_kmsg", kmsg);
                if (ok) where.push_back("/cache/recovery");
            }
        }
    }
    // 2. A card or USB disk, and readable internal storage: plain files with plain names.
    auto writeTo = [&](const std::string& base, const std::string& shown) {
        std::string dir = base + "/FMRRE/logs";
        if (!util::mkdirs(dir)) return;
        if (rotateNow) rename((dir + "/recovery.log").c_str(), (dir + "/recovery.previous.log").c_str());
        if (!util::writeFile(dir + "/recovery.log", log + tail)) return;
        if (!kmsg.empty()) util::writeFile(dir + "/kernel.log", kmsg);
        util::writeFile(dir + "/selinux.txt", report);
        where.push_back(shown + "/FMRRE/logs");
    };
    if (removable != Removable::No)
        for (auto& p : P.all()) {
            if (!p.removable || !p.storage || !p.present) continue;
            if (removable == Removable::IfMounted ? !P.isMounted(p) : !P.mount(p)) continue;
            writeTo(p.contentPath(), p.displayName);
        }
    Partition* d = P.find("/data");
    std::string media = device::path("/data/media/0");
    if (removable != Removable::No && d && P.isMounted(*d) && !P.dataEncrypted() && util::isDir(media)) writeTo(media, "/sdcard");
    if (!where.empty()) g_rotated = true;
    return where;
}

void tick() {
    static int64_t start = util::nowMs(), last = 0;
    static uint64_t savedGen = 0;
    static int saves = 0;
    if (device::isSim() || Task::get().running()) return;  // never under an operation: it may be formatting /cache
    int64_t now = util::nowMs();
    // The first save comes a few seconds in (the start-up log is what counts if the phone then hangs), with the
    // card; later ones follow the log, at most once a minute, and leave an unmounted card alone.
    if (saves == 0 ? now - start < 4000 : (now - last < 60000 || log::generation() == savedGen)) return;
    auto where = save(saves == 0 ? "start" : "periodic", saves == 0 ? Removable::Mount : Removable::IfMounted);
    if (saves == 0 && !where.empty()) {
        std::string all;
        for (auto& w : where) all += (all.empty() ? "" : ", ") + w;
        LOGI("Logs are kept in %s", all.c_str());
    }
    savedGen = log::generation();
    last = now;
    saves++;
}

}  // namespace logsave
}  // namespace tw
