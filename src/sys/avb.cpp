#include "sys/avb.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"

namespace tw::avb {

namespace {

uint64_t be(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

std::string otherSlot() { return device::info().slot == "_b" ? "_a" : "_b"; }

bool patch(bool bothSlots, bool disable, Task& t) {
    std::vector<std::string> slots = {device::info().slot};
    if (bothSlots && device::info().ab) slots.push_back(otherSlot());
    bool any = false, ok = true;
    for (auto& s : slots) {
        std::string dev = vbmetaDevice(s);
        if (dev.empty()) continue;
        Info i = read(dev);
        if (!i.ok) { t.print("  %s: %s", util::basename(dev).c_str(), i.error.c_str()); ok = false; continue; }
        uint32_t nf = disable ? (i.flags | kDisableVerity | kDisableVerification) : (i.flags & ~(kDisableVerity | kDisableVerification));
        if (nf == i.flags) {
            t.print("  %s: already %s (flags=%u)", util::basename(dev).c_str(), disable ? "disabled" : "enabled", i.flags);
            any = true;
            continue;
        }
        std::string head;
        int fd = open(dev.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            head.resize(256);
            if (pread(fd, &head[0], 256, 0) == 256) {
                std::string f = Partitions::get().storagePath() + "/TWRP/vbmeta/" + util::basename(dev) + "_header_" + util::dateStamp() + ".bin";
                util::mkdirs(util::dirname(f));
                util::writeFile(f, head);
                t.print("  saved original header to %s", f.c_str());
            }
            close(fd);
        }
        std::string err;
        if (!setFlags(dev, nf, err)) { t.print("  %s: %s", util::basename(dev).c_str(), err.c_str()); ok = false; continue; }
        t.print("  %s: flags %u -> %u", util::basename(dev).c_str(), i.flags, nf);
        any = true;
    }
    if (!any && ok) { t.print("No vbmeta partition found on this device."); return false; }
    return ok;
}

}  // namespace

std::string vbmetaDevice(const std::string& slot) {
    std::string by = Partitions::byNameDir();
    for (auto n : {"vbmeta" + slot, std::string("vbmeta")})
        if (util::exists(by + "/" + n)) return by + "/" + n;
    return "";
}

Info read(const std::string& dev) {
    Info i;
    int fd = open(dev.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { i.error = strerror(errno); return i; }
    uint8_t h[256];
    ssize_t n = pread(fd, h, sizeof(h), 0);
    close(fd);
    if (n != (ssize_t)sizeof(h) || memcmp(h, "AVB0", 4) != 0) { i.error = "no AVB0 vbmeta header"; return i; }
    i.versionMajor = (uint32_t)be(h + 4, 4);
    i.versionMinor = (uint32_t)be(h + 8, 4);
    i.rollbackIndex = be(h + 112, 8);
    i.flags = (uint32_t)be(h + 120, 4);
    i.release.assign((const char*)h + 128, strnlen((const char*)h + 128, 48));
    i.ok = true;
    return i;
}

bool setFlags(const std::string& dev, uint32_t flags, std::string& err) {
    Info i = read(dev);
    if (!i.ok) { err = i.error; return false; }
    int fd = open(dev.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) { err = strerror(errno); return false; }
    uint8_t b[4] = {uint8_t(flags >> 24), uint8_t(flags >> 16), uint8_t(flags >> 8), uint8_t(flags)};
    bool ok = pwrite(fd, b, 4, 120) == 4 && fsync(fd) == 0;
    close(fd);
    if (!ok) { err = strerror(errno); return false; }
    Info back = read(dev);
    if (!back.ok || back.flags != flags) { err = "read-back mismatch"; return false; }
    return true;
}

bool disableVerification(bool bothSlots, Task& t) {
    t.print("Disabling dm-verity and AVB verification...");
    return patch(bothSlots, true, t);
}

bool enableVerification(bool bothSlots, Task& t) {
    t.print("Re-enabling dm-verity and AVB verification...");
    return patch(bothSlots, false, t);
}

}  // namespace tw::avb
