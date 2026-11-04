#include "sys/sideload.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "core/log.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/mtp.h"

namespace tw::sideload {

namespace {

constexpr const char* kDropPaths[] = {
    "/sideload/package.zip",  // minadbd FUSE mount
    "/tmp/update.zip",        // AOSP recovery convention
    "/cache/sideload.zip",
    "/data/local/tmp/sideload.zip",
};

bool stableFile(const std::string& p, uint64_t prevSize, uint64_t& outSize) {
    struct stat st{};
    if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    outSize = (uint64_t)st.st_size;
    return outSize > 0 && outSize == prevSize;
}

std::string findDrop(uint64_t& outSize) {
    for (const char* p : kDropPaths) {
        std::string path = device::path(p);
        struct stat st{};
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            outSize = (uint64_t)st.st_size;
            return path;
        }
    }
    return {};
}

// Prepare the host-visible drop directory for minadbd on a device.
void primeDevice(Task& t) {
    if (mtp::running()) {  // sideload needs the USB gadget to itself
        t.print("Turning MTP off for sideload");
        mtp::stop();
    }
    util::mkdirs("/tmp");
    util::mkdirs("/sideload");
    // Try spawning minadbd first (preferred, speaks the real sideload protocol).
    bool spawned = false;
    for (const char* bin : {"/sbin/minadbd", "/system/bin/minadbd", "/sbin/recovery"}) {
        if (!util::exists(bin)) continue;
        t.print("Starting %s", bin);
        std::string cmd = std::string(bin) + " --sideload > /tmp/minadbd.log 2>&1 &";
        if (util::run(cmd) == 0) { spawned = true; break; }
    }
    if (!spawned) {
        // Fall back: flip USB to sideload and hope for a push.
        t.print("minadbd not found; falling back to adbd sideload mode");
        device::setProp("sys.usb.config", "none");
        usleep(200 * 1000);
        device::setProp("sys.usb.config", "sideload");
        device::setProp("ctl.start", "adbd");
    }
}

void cleanupDevice() {
    util::run("pkill -f minadbd 2>/dev/null");
    device::setProp("sys.usb.config", "adb");
}

// Fake transfer for the simulator: pretend a 32 MB package streams in.
std::string simulateTransfer(Task& t) {
    std::string zip = device::path("/data/media/0/Download/LineageOS-22.1-20261028-nightly-husky.zip");
    uint64_t total = util::fileSize(zip);
    if (total == 0) {
        t.print("No demo package found in the sandbox");
        return {};
    }
    t.setStatus("Receiving package (simulated)");
    for (int i = 0; i <= 100 && !t.cancelled(); i += 2) {
        t.setSubProgress(i / 100.f);
        uint64_t got = total * i / 100;
        char buf[64];
        snprintf(buf, sizeof(buf), "%s / %s", util::humanSize(got).c_str(), util::humanSize(total).c_str());
        t.setDetail(buf);
        usleep(60 * 1000);
    }
    if (t.cancelled()) return {};
    std::string dst = device::path("/tmp/sideload.zip");
    util::mkdirs(util::dirname(dst));
    if (!util::copyFile(zip, dst)) return {};
    return dst;
}

}  // namespace

std::string receive(Task& t) {
    t.print("ADB sideload: waiting for host");
    t.print("On your computer run:");
    t.print("    adb sideload <package.zip>");

    if (device::isSim()) return simulateTransfer(t);

    primeDevice(t);
    t.setStatus("Waiting for host");

    uint64_t prevSize = 0, stableFor = 0;
    int64_t deadline = util::nowMs() + 10 * 60 * 1000;  // 10 min
    std::string drop;
    while (!t.cancelled() && util::nowMs() < deadline) {
        uint64_t sz = 0;
        drop = findDrop(sz);
        if (!drop.empty()) {
            char buf[96];
            snprintf(buf, sizeof(buf), "Receiving %s", util::humanSize(sz).c_str());
            t.setStatus(buf);
            // Wait until the file stops growing (minadbd closes it).
            if (sz == prevSize) {
                stableFor += 300;
                if (stableFor >= 1200) break;
            } else {
                stableFor = 0;
            }
            prevSize = sz;
        }
        usleep(300 * 1000);
    }
    cleanupDevice();
    if (t.cancelled() || drop.empty()) {
        if (drop.empty()) t.print("Timed out waiting for a sideload package");
        return {};
    }
    uint64_t finalSize = 0;
    stableFile(drop, prevSize, finalSize);
    t.print("Received %s (%s)", util::basename(drop).c_str(), util::humanSize(finalSize).c_str());
    return drop;
}

}  // namespace tw::sideload
