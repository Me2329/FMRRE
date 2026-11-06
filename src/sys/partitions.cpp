#include "sys/partitions.h"
#include "sys/history.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <strings.h>
#include <unistd.h>

namespace tw {

static const struct { const char* name; const char* display; } kNames[] = {
    {"system", "System"}, {"system_root", "System"}, {"vendor", "Vendor"}, {"product", "Product"},
    {"system_ext", "System Ext"}, {"odm", "ODM"}, {"data", "Data"}, {"cache", "Cache"}, {"metadata", "Metadata"},
    {"persist", "Persist"}, {"efs", "EFS"}, {"sec_efs", "EFS (sec)"}, {"boot", "Boot"}, {"init_boot", "Init Boot"},
    {"vendor_boot", "Vendor Boot"}, {"recovery", "Recovery"}, {"dtbo", "DTBO"}, {"vbmeta", "VBMeta"},
    {"vbmeta_system", "VBMeta System"}, {"vbmeta_vendor", "VBMeta Vendor"}, {"modem", "Modem"}, {"radio", "Radio"},
    {"modemst1", "EFS modemst1"}, {"modemst2", "EFS modemst2"}, {"fsg", "EFS fsg"}, {"fsc", "EFS fsc"},
    {"misc", "Misc"}, {"logo", "Logo"}, {"splash", "Splash"}, {"super", "Super"}, {"external_sd", "MicroSD"},
    {"sdcard1", "MicroSD"}, {"usb_otg", "USB OTG"}, {"usbotg", "USB OTG"},
};

static std::string displayFor(const std::string& name) {
    for (auto& n : kNames)
        if (name == n.name) return n.display;
    std::string s = name;
    if (!s.empty()) s[0] = (char)toupper(s[0]);
    return s;
}

std::string Partition::contentPath() const { return device::path(mountPoint); }

Partitions& Partitions::get() {
    static Partitions p;
    return p;
}

std::string Partitions::byNameDir() {
    if (device::isSim()) return device::path("/dev/block/by-name");
    for (auto d : {"/dev/block/by-name", "/dev/block/bootdevice/by-name"})
        if (util::isDir(d)) return d;
    for (auto& e : util::listDir("/dev/block/platform")) {
        std::string p = "/dev/block/platform/" + e.name + "/by-name";
        if (util::isDir(p)) return p;
        for (auto& e2 : util::listDir("/dev/block/platform/" + e.name)) {
            p = "/dev/block/platform/" + e.name + "/" + e2.name + "/by-name";
            if (util::isDir(p)) return p;
        }
    }
    return "/dev/block/by-name";
}

std::string Partitions::byName(const std::string& name) {
    std::string dir = byNameDir(), exact = dir + "/" + name;
    if (util::exists(exact)) return exact;
    // Samsung names partitions in capitals (BOOT, RECOVERY, RADIO): match without case.
    for (auto& e : util::listDir(dir))
        if (strcasecmp(e.name.c_str(), name.c_str()) == 0) return dir + "/" + e.name;
    return "";
}

uint64_t Partitions::blockSize(const std::string& dev) {
    int fd = open(dev.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    uint64_t sz = 0;
    if (ioctl(fd, BLKGETSIZE64, &sz) != 0) {
        off_t e = lseek(fd, 0, SEEK_END);
        sz = e > 0 ? e : 0;
    }
    close(fd);
    return sz;
}

// /dev/block/sda5 -> "sda", /dev/block/mmcblk0p1 -> "mmcblk0" (symlinks such as by-name/ are followed first).
static std::string diskOf(const std::string& dev) {
    char real[PATH_MAX];
    std::string b = util::basename(realpath(dev.c_str(), real) ? std::string(real) : dev);
    if (util::startsWith(b, "mmcblk")) {
        size_t p = b.find('p', 6);
        return p == std::string::npos ? b : b.substr(0, p);
    }
    if (util::startsWith(b, "sd")) {
        while (b.size() > 3 && isdigit((unsigned char)b.back())) b.pop_back();
        return b;
    }
    return "";
}

const char* Partitions::diskKind(const std::string& dev) {
    std::string disk = diskOf(dev);
    if (disk.empty()) return "unknown";
    std::string sys = device::path("/sys/block/" + disk);
    if (!util::exists(sys)) return "unknown";
    if (util::startsWith(disk, "mmcblk")) {
        // The MMC core says what the card is: "SD" for a microSD, "MMC" for the phone's own eMMC.
        std::string type = util::trim(util::readLine(sys + "/device/type"));
        return type == "SD" ? "sd" : type.empty() ? "unknown" : "internal";
    }
    // sd*: on UFS phones sda..sdf are the phone's own storage (LUNs). A USB disk's sysfs path runs through the USB
    // host controller (.../usb1/1-1/...).
    char real[PATH_MAX];
    std::string path = realpath(sys.c_str(), real) ? std::string(real) : "";
    if (path.empty()) return util::trim(util::readLine(sys + "/removable")) == "1" ? "usb" : "unknown";
    return path.find("/usb") != std::string::npos ? "usb" : "internal";
}

static bool isFooterKeyLoc(const std::string& k, const std::string& v) {
    return (k == "encryptable" || k == "forceencrypt" || k == "forcefdeorfbe") && v == "footer";
}

static std::string resolveDev(std::string dev, bool slotSelect, bool logical) {
    const std::string slot = device::info().slot;
    if (logical) {
        std::string base = util::basename(dev);
        std::string m1 = device::path("/dev/block/mapper/" + base + slot);
        std::string m2 = device::path("/dev/block/mapper/" + base);
        return util::exists(m1) ? m1 : m2;
    }
    if (!util::startsWith(dev, "/")) dev = Partitions::byNameDir() + "/" + dev;
    if (slotSelect && !slot.empty() && !util::endsWith(dev, slot)) dev += slot;
    if (device::isSim() && util::startsWith(dev, "/dev/") && !util::startsWith(dev, device::simRoot()))
        dev = device::path(dev);
    return dev;
}

// TWRP format:  /data  f2fs  /dev/block/by-name/userdata  flags=display="Data";backup=1;wipeingui
void Partitions::addFromTwrpLine(const std::vector<std::string>& f) {
    Partition p;
    p.mountPoint = f[0];
    p.fsType = f[1];
    std::string dev = f[2];
    std::string flags;
    for (size_t i = 3; i < f.size(); i++) {
        if (util::startsWith(f[i], "flags=")) flags = f[i].substr(6);
        else if (flags.empty() && f[i][0] == '/') continue;  // secondary block device: ignored
        else if (!flags.empty()) flags += " " + f[i];
    }
    bool slotSel = false, footer = false;
    for (auto& fl : util::split(flags, ';')) {
        auto eq = fl.find('=');
        std::string k = util::trim(fl.substr(0, eq));
        std::string v = eq == std::string::npos ? "" : util::trim(fl.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (k == "display") p.displayName = v;
        else if (k == "backup") p.backup = v == "1" || v == "true";
        else if (k == "wipeingui" || k == "wipeduringfactoryreset") p.wipe = true;
        else if (k == "flashimg") p.flashImg = v == "1" || v == "true";
        else if (k == "removable") p.removable = true;
        else if (k == "storage") p.storage = true;
        else if (k == "slotselect") slotSel = true;
        else if (k == "logical") p.logical = true;
        else if (k == "fsflags") p.mountOpts = v;
        else if (k == "length") p.length = strtoll(v.c_str(), nullptr, 0);
        else if (isFooterKeyLoc(k, v)) footer = true;
    }
    if (footer && p.length == 0) p.length = -16384;  // AOSP's CRYPT_FOOTER_OFFSET
    p.blockDev = resolveDev(dev, slotSel, p.logical);
    finalize(p);
}

// AOSP format:  <dev> <mnt> <type> <mnt_flags> <fs_mgr_flags>
void Partitions::addFromAospLine(const std::vector<std::string>& f) {
    if (f.size() < 3) return;
    Partition p;
    std::string dev = f[0];
    p.mountPoint = f[1];
    p.fsType = f[2];
    if (f.size() > 3) p.mountOpts = f[3];
    std::string mgr = f.size() > 4 ? f[4] : "";
    if (p.mountPoint == "auto" || p.fsType == "auto") {
        // vold-managed removable storage
        if (mgr.find("voldmanaged=sdcard") != std::string::npos) {
            p.mountPoint = "/external_sd";
            p.fsType = "auto";
            p.removable = true;
            p.storage = true;
            p.displayName = "MicroSD";
            finalize(p);  // the card's device is found by rescanRemovable() (mmcblk0 on UFS phones, mmcblk1 on eMMC)
        } else if (mgr.find("voldmanaged=usb") != std::string::npos) {
            p.mountPoint = "/usb_otg";
            p.fsType = "auto";
            p.removable = true;
            p.storage = true;
            p.displayName = "USB OTG";
            finalize(p);  // never a fixed sda1: on UFS phones that is the phone's own storage
        }
        return;
    }
    if (p.mountPoint == "/") p.mountPoint = "/system_root";
    bool slotSel = mgr.find("slotselect") != std::string::npos;
    p.logical = mgr.find("logical") != std::string::npos;
    bool footer = false;
    for (auto& o : util::split(mgr, ',')) {
        auto eq = o.find('=');
        std::string k = o.substr(0, eq), v = eq == std::string::npos ? "" : o.substr(eq + 1);
        if (k == "length") p.length = strtoll(v.c_str(), nullptr, 0);
        else if (isFooterKeyLoc(k, v)) footer = true;
    }
    if (footer && p.length == 0) p.length = -16384;
    if (find(p.mountPoint)) return;  // first entry wins (fstabs often list ext4+f2fs variants)
    p.blockDev = resolveDev(dev, slotSel, p.logical);
    if (p.mountPoint == "/data" || p.mountPoint == "/cache" || p.mountPoint == "/metadata") p.wipe = true;
    finalize(p);
}

void Partitions::finalize(Partition& p) {
    std::string mp = p.mountPoint;
    p.name = util::basename(mp);
    if (p.name == "system_root") p.name = "system";
    if (p.displayName.empty()) p.displayName = displayFor(util::basename(mp));
    if (p.fsType == "erofs" || p.fsType == "squashfs") p.readOnly = true;
    static const char* backupDefault[] = {"/data", "/boot", "/init_boot", "/vendor_boot", "/dtbo", "/efs", "/persist",
                                          "/recovery", "/modemst1", "/modemst2", "/system_root", "/system", "/vendor"};
    if (!p.backup)
        for (auto b : backupDefault)
            if (mp == b) p.backup = true;
    if (p.isImage()) p.flashImg = true;
    if (mp == "/data" || mp == "/cache" || mp == "/system" || mp == "/system_root" || mp == "/vendor") p.wipe = true;
    if (mp == "/data" || mp == "/sdcard" || p.removable) p.storage = true;
    if (mp == "/misc" || mp == "/super") p.wipe = false;
    parts_.push_back(p);
}

void Partitions::addImagesFromByName() {
    // Supplement AOSP fstabs (which omit raw partitions) with well-known images.
    const std::string slot = device::info().slot;
    static const char* images[] = {"boot", "init_boot", "vendor_boot", "dtbo", "recovery", "vbmeta", "vbmeta_system",
                                   "modemst1", "modemst2", "persist", "logo", "splash"};
    for (auto img : images) {
        std::string mp = std::string("/") + img;
        if (find(mp)) continue;
        std::string dev = slot.empty() ? "" : byName(img + slot);
        if (dev.empty()) dev = byName(img);
        if (dev.empty()) continue;
        Partition p;
        p.mountPoint = mp;
        p.fsType = std::string(img) == "persist" ? "ext4" : "emmc";
        p.blockDev = dev;
        p.backup = std::string(img) != "vbmeta" && std::string(img) != "vbmeta_system" &&
                   std::string(img) != "logo" && std::string(img) != "splash";
        finalize(p);
    }
}

void Partitions::load() {
    parts_.clear();
    std::vector<std::string> cands = {"/etc/twrp.fstab", "/system/etc/twrp.fstab", "/etc/recovery.fstab", "/system/etc/recovery.fstab"};
    std::string hw = device::prop("ro.hardware");
    std::string plat = device::prop("ro.boot.hardware");
    for (auto& h : {hw, plat})
        if (!h.empty()) {
            cands.push_back("/vendor/etc/fstab." + h);
            cands.push_back("/fstab." + h);
        }
    std::string data;
    for (auto& c : cands) {
        std::string p = device::path(c);
        if (util::readFile(p, data)) {
            fstabPath_ = p;
            break;
        }
    }
    if (fstabPath_.empty()) LOGE("No fstab found! Partition features will be limited.");
    else LOGI("fstab: %s", fstabPath_.c_str());

    for (auto& raw : util::split(data, '\n')) {
        std::string line = util::trim(raw);
        if (line.empty() || line[0] == '#') continue;
        auto f = util::splitWs(line);
        if (f.size() < 3) continue;
        // AOSP lines have the mount point second; TWRP lines have the fs type second. "auto" second is AOSP only for
        // vold's sysfs patterns (/devices/...): "/usb_otg auto /dev/block/sdg1 flags=..." is a TWRP line.
        bool aosp = f[1][0] == '/' || (f[1] == "auto" && (util::startsWith(f[0], "/devices/") || util::startsWith(f[0], "/sys/")));
        if (aosp) addFromAospLine(f);
        else if (f[0][0] == '/') addFromTwrpLine(f);
    }
    addImagesFromByName();
    if (!device::isSim()) {
        std::string keep = storage_;
        rescanRemovable();
        storage_ = keep;
    }
    // Order: data first, then filesystems, then images.
    std::stable_sort(parts_.begin(), parts_.end(), [](const Partition& a, const Partition& b) {
        auto rank = [](const Partition& p) {
            if (p.mountPoint == "/data") return 0;
            if (p.removable) return 3;
            return p.isImage() ? 2 : 1;
        };
        return rank(a) < rank(b);
    });
    std::string st = Settings::get().str("storage", "/data");
    storage_ = find(st) ? st : "/data";
    refresh();
    for (auto& p : parts_)
        LOGI("  %-14s %-6s %s%s", p.mountPoint.c_str(), p.fsType.c_str(), p.blockDev.c_str(), p.present ? "" : " (missing)");
}

Partition* Partitions::find(const std::string& key) {
    for (auto& p : parts_)
        if (p.mountPoint == key || p.name == key) return &p;
    return nullptr;
}

bool Partitions::isMounted(const Partition& p) const {
    if (p.isImage()) return false;
    if (device::isSim())
        return std::find(simMounted_.begin(), simMounted_.end(), p.mountPoint) != simMounted_.end();
    std::string data;
    if (!util::readFile("/proc/mounts", data)) return false;
    for (auto& line : util::split(data, '\n')) {
        auto f = util::splitWs(line);
        if (f.size() >= 2 && f[1] == p.mountPoint) return true;
    }
    return false;
}

void Partitions::refresh() {
    for (auto& p : parts_) {
        p.present = util::exists(p.blockDev);
        p.mounted = isMounted(p);
        p.size = p.used = p.avail = 0;
        if (p.mounted) {
            struct statvfs sv;
            if (statvfs(p.contentPath().c_str(), &sv) == 0) {
                p.size = uint64_t(sv.f_blocks) * sv.f_frsize;
                p.avail = uint64_t(sv.f_bavail) * sv.f_frsize;
                p.used = p.size - uint64_t(sv.f_bfree) * sv.f_frsize;
            }
            if (device::isSim()) {
                // Sandbox dirs share the host filesystem: report the real content size instead.
                p.used = util::dirSize(p.contentPath());
                uint64_t fake = p.mountPoint == "/data" ? (128ull << 30) : (4ull << 30);
                p.size = std::max(fake, p.used);
                p.avail = p.size - p.used;
            }
        } else if (p.present) {
            p.size = blockSize(p.blockDev);
        }
    }
}

bool Partitions::mount(Partition& p, std::string* err) {
    if (p.isImage()) { if (err) *err = "raw image partitions cannot be mounted"; return false; }
    if (p.removable && std::string(diskKind(p.blockDev)) == "internal") {  // never put backups on the phone's own LUNs
        if (err) *err = p.blockDev + " is the phone's own storage, not a removable disk";
        return false;
    }
    if (isMounted(p)) return true;
    if (device::isSim()) {
        util::mkdirs(p.contentPath());
        simMounted_.push_back(p.mountPoint);
        p.mounted = true;
        LOGI("Mounted %s (sim)", p.mountPoint.c_str());
        return true;
    }
    util::mkdirs(p.mountPoint);
    std::vector<std::string> types;
    if (p.fsType == "auto" || p.removable) types = {"vfat", "exfat", "sdfat", "texfat", "ntfs3", "ntfs", "ext4", "f2fs"};
    else types = {p.fsType};
    if (p.removable && p.fsType != "auto")
        types.insert(types.begin(), p.fsType);
    unsigned long flags = MS_NOATIME | MS_NODEV | MS_NODIRATIME;
    bool roFs = p.readOnly || ((p.mountPoint == "/system_root" || p.mountPoint == "/system" ||
                                p.mountPoint == "/vendor" || p.mountPoint == "/product" ||
                                p.mountPoint == "/system_ext" || p.mountPoint == "/odm") &&
                               !Settings::get().flag("mount_system_rw"));
    if (roFs) flags |= MS_RDONLY;
    for (auto& t : types) {
        const char* opts = t == "vfat" ? "utf8" : (t == "exfat" || t == "sdfat" || t == "texfat") ? "iocharset=utf8" : nullptr;
        bool ok = ::mount(p.blockDev.c_str(), p.mountPoint.c_str(), t.c_str(), flags, opts) == 0;
        if (!ok && opts) ok = ::mount(p.blockDev.c_str(), p.mountPoint.c_str(), t.c_str(), flags, nullptr) == 0;
        if (ok) {
            LOGI("Mounted %s (%s%s)", p.mountPoint.c_str(), t.c_str(), roFs ? ", ro" : "");
            p.mounted = true;
            if (p.fsType == "auto") p.fsType = t;
            return true;
        }
        if (errno == EROFS || errno == EACCES) {
            if (::mount(p.blockDev.c_str(), p.mountPoint.c_str(), t.c_str(), flags | MS_RDONLY, opts) == 0) {
                LOGI("Mounted %s read-only", p.mountPoint.c_str());
                p.mounted = true;
                return true;
            }
        }
    }
    std::string e = strerror(errno);
    if (p.removable || p.fsType == "exfat" || p.fsType == "ntfs") {
        // Kernels without exFAT/NTFS drivers: FUSE helpers shipped in some ramdisks.
        std::string dev = util::shellQuote(p.blockDev), mp = util::shellQuote(p.mountPoint);
        for (auto& cmd : {"mount.exfat " + dev + " " + mp, "mount.exfat-fuse " + dev + " " + mp, "ntfs-3g " + dev + " " + mp,
                          "mount.ntfs " + dev + " " + mp})
            if (util::haveBinary(util::splitWs(cmd)[0]) && util::run(cmd + " 2>/dev/null") == 0) {
                LOGI("Mounted %s via %s", p.mountPoint.c_str(), util::splitWs(cmd)[0].c_str());
                p.mounted = true;
                return true;
            }
    }
    LOGE("Failed to mount %s: %s", p.mountPoint.c_str(), e.c_str());
    if (err) *err = e;
    return false;
}

bool Partitions::unmount(Partition& p, std::string* err) {
    if (!isMounted(p)) return true;
    if (device::isSim()) {
        simMounted_.erase(std::remove(simMounted_.begin(), simMounted_.end(), p.mountPoint), simMounted_.end());
        p.mounted = false;
        LOGI("Unmounted %s (sim)", p.mountPoint.c_str());
        return true;
    }
    sync();
    if (umount2(p.mountPoint.c_str(), 0) != 0 && umount2(p.mountPoint.c_str(), MNT_DETACH) != 0) {
        std::string e = strerror(errno);
        LOGE("Failed to unmount %s: %s", p.mountPoint.c_str(), e.c_str());
        if (err) *err = e;
        return false;
    }
    p.mounted = false;
    LOGI("Unmounted %s", p.mountPoint.c_str());
    return true;
}

void Partitions::mountAllStorage() {
    for (auto& p : parts_)
        if ((p.mountPoint == "/data" || p.mountPoint == "/cache" || p.mountPoint == "/metadata") && p.present) mount(p);
}

void Partitions::unmountAll() {
    for (auto& p : parts_) unmount(p);
}

bool Partitions::wipe(Partition& p, Task& t) {
    bool ok_ = [&]() -> bool {
        if (p.isImage()) {
            t.print("Erasing %s (zero-fill)...", p.displayName.c_str());
            int fd = open(p.blockDev.c_str(), O_WRONLY | O_CLOEXEC);
            if (fd < 0) { t.print("  cannot open %s", p.blockDev.c_str()); return false; }
            uint64_t sz = blockSize(p.blockDev);
            std::vector<char> zero(1 << 20, 0);
            uint64_t done = 0;
            while (done < sz) {
                size_t n = std::min<uint64_t>(zero.size(), sz - done);
                if (write(fd, zero.data(), n) != (ssize_t)n) break;
                done += n;
                t.setSubProgress(float(done) / sz);
            }
            fsync(fd);
            close(fd);
            return done == sz;
        }
        t.print("Wiping %s...", p.displayName.c_str());
        if (!mount(p)) {
            t.print("  %s can't be mounted - formatting instead", p.displayName.c_str());
            return format(p, t);
        }
        std::vector<std::string> keep;
        if (p.mountPoint == "/data") keep = {"media"};  // never wipe internal storage on a factory reset
        if (p.mountPoint == "/data") t.print("  (internal storage /data/media is preserved)");
        bool ok = util::removeTree(p.contentPath(), keep.empty() ? std::vector<std::string>{"lost+found"} : std::vector<std::string>{"media", "lost+found"});
        t.print(ok ? "  done" : "  some files could not be removed");
        return ok;
    }();
    history::record("Wipe", p.displayName, ok_);
    return ok_;
}

std::string Partitions::mkfsCommand(const Partition& p, const std::string& fs, uint64_t whole, bool haveMke2fs) {
    std::string dev = util::shellQuote(p.blockDev);
    // fstab length=: a negative value keeps that many bytes free at the end of the partition (where full-disk
    // encryption keeps its key - a filesystem over it makes Android's encryption fail on the next boot).
    uint64_t bytes = 0;
    if (p.length < 0 && whole > uint64_t(-p.length)) bytes = whole - uint64_t(-p.length);
    if (p.length > 0 && (whole == 0 || uint64_t(p.length) <= whole)) bytes = uint64_t(p.length);
    if (fs == "ext4")
        return haveMke2fs ? "mke2fs -F -t ext4 -b 4096 " + dev + (bytes ? " " + std::to_string(bytes / 4096) : "")
                          : "make_ext4fs " + (bytes ? "-l " + std::to_string(bytes / 4096 * 4096) + " " : std::string()) + dev;
    if (fs == "f2fs") {
        std::string sectors = bytes ? " " + std::to_string(bytes / 512) : "";
        return "make_f2fs -f -g android " + dev + sectors + " || mkfs.f2fs -f " + dev + sectors;
    }
    if (fs == "vfat") return "mkfs.fat " + dev + " || newfs_msdos -F 32 " + dev;
    if (fs == "exfat") return "mkfs.exfat " + dev;
    return "";
}

bool Partitions::format(Partition& p, Task& t, const std::string& newFs) {
    bool ok_ = [&]() -> bool {
        std::string fs = newFs.empty() ? p.fsType : newFs;
        t.print("Formatting %s as %s...", p.displayName.c_str(), fs.c_str());
        if (p.removable && std::string(diskKind(p.blockDev)) == "internal") {
            t.print("  refused: %s is the phone's own storage, not a removable disk", p.blockDev.c_str());
            return false;
        }
        if (device::isSim()) {
            util::removeTree(p.contentPath(), {"__keep__"});
            util::mkdirs(p.contentPath());
            if (!newFs.empty()) p.fsType = newFs;
            t.print("  formatted (simulated)");
            return true;
        }
        unmount(p);
        uint64_t whole = blockSize(p.blockDev);
        std::string cmd = mkfsCommand(p, fs, whole, util::haveBinary("mke2fs"));
        if (cmd.empty()) {
            t.print("  unsupported filesystem '%s'", fs.c_str());
            return false;
        }
        if (p.length < 0 && whole > uint64_t(-p.length))
            t.print("  %lld bytes at the end are kept free for the encryption footer", (long long)-p.length);
        int rc = util::run(cmd, [&](const std::string& l) { t.print("  %s", l.c_str()); });
        if (rc != 0) {
            t.print("  mkfs failed (exit %d)", rc);
            return false;
        }
        if (!newFs.empty()) p.fsType = newFs;
        bool ok = mount(p);
        if (ok && p.mountPoint == "/data") util::mkdirs("/data/media/0", 0770);
        t.print("  done");
        return ok;
    }();
    history::record("Format", p.displayName + (newFs.empty() ? "" : " as " + newFs), ok_);
    return ok_;
}

namespace {
int runFs(const std::string& cmd, Task& t) {
    t.print("$ %s", cmd.c_str());
    return util::run(cmd, [&](const std::string& l) { t.print("  %s", l.c_str()); });
}
}  // namespace

bool Partitions::repair(Partition& p, Task& t) {
    if (p.isImage()) { t.print("%s is a raw image, not a filesystem", p.displayName.c_str()); return false; }
    t.setStatus("Checking " + p.displayName);
    if (device::isSim()) { t.print("[simulator] fsck %s (%s): clean", p.blockDev.c_str(), p.fsType.c_str()); return true; }
    if (!unmount(p)) { t.print("Cannot unmount %s - is something using it?", p.mountPoint.c_str()); return false; }
    std::string dev = util::shellQuote(p.blockDev);
    int rc;
    if (p.fsType == "ext4" || p.fsType == "ext3" || p.fsType == "ext2") {
        rc = runFs("e2fsck -fy " + dev, t);
        rc = rc >= 0 && rc < 4 ? 0 : rc;  // 1/2 = errors corrected
    } else if (p.fsType == "f2fs") {
        rc = runFs("fsck.f2fs -f -a " + dev, t);
    } else if (p.fsType == "vfat") {
        rc = runFs("fsck_msdos -y " + dev + " || fsck.fat -a " + dev, t);
    } else if (p.fsType == "exfat") {
        rc = runFs("fsck.exfat -y " + dev + " || exfatfsck " + dev, t);
    } else if (p.fsType == "ntfs") {
        rc = runFs("ntfsfix " + dev, t);
    } else {
        t.print("No checker known for %s", p.fsType.c_str());
        return false;
    }
    t.print(rc == 0 ? "Filesystem is clean (or was repaired)." : "fsck reported errors (exit %d)", rc);
    mount(p);
    return rc == 0;
}

bool Partitions::resizeFs(Partition& p, Task& t) {
    if (p.isImage()) return false;
    t.setStatus("Resizing " + p.displayName);
    if (device::isSim()) { t.print("[simulator] resize filesystem on %s to fill the partition", p.blockDev.c_str()); return true; }
    if (!unmount(p)) { t.print("Cannot unmount %s", p.mountPoint.c_str()); return false; }
    std::string dev = util::shellQuote(p.blockDev);
    int rc;
    if (p.fsType == "ext4" || p.fsType == "ext3" || p.fsType == "ext2") {
        runFs("e2fsck -fy " + dev, t);
        rc = runFs("resize2fs " + dev, t);
    } else if (p.fsType == "f2fs") {
        rc = runFs("resize.f2fs " + dev, t);
    } else {
        t.print("Resizing %s is not supported", p.fsType.c_str());
        return false;
    }
    mount(p);
    t.print(rc == 0 ? "Filesystem now fills the partition." : "Resize failed (exit %d)", rc);
    return rc == 0;
}

std::string Partitions::fsInfo(Partition& p) {
    std::string out = "Device: " + p.blockDev + "\nSize: " + util::humanSize(blockSize(p.blockDev)) + "\nType (fstab): " + p.fsType + "\n";
    if (device::isSim()) return out + "[simulator]\n";
    std::string o;
    util::runCapture("blkid " + util::shellQuote(p.blockDev) + " 2>/dev/null", o);
    out += o;
    if (p.fsType.rfind("ext", 0) == 0) {
        util::runCapture("dumpe2fs -h " + util::shellQuote(p.blockDev) + " 2>/dev/null | head -n 40", o);
        out += o;
    }
    return out;
}

int Partitions::rescanRemovable() {
    // Find the real USB disk and microSD card from sysfs - never by a fixed name: on UFS phones sda..sdf are the
    // phone's own storage, and mmcblk0 is the SD card there but the internal eMMC on older phones.
    std::string usbDev, sdDev;
    for (auto& e : util::listDir(device::path("/sys/block"))) {
        std::string base = device::path("/dev/block/" + e.name);
        if (usbDev.empty() && util::startsWith(e.name, "sd") && std::string(diskKind(base)) == "usb")
            usbDev = util::exists(base + "1") ? base + "1" : base;
        if (sdDev.empty() && util::startsWith(e.name, "mmcblk") && e.name.find("boot") == std::string::npos &&
            e.name.find("rpmb") == std::string::npos && std::string(diskKind(base)) == "sd")
            sdDev = util::exists(base + "p1") ? base + "p1" : base;
    }
    bool haveUsb = false, haveSd = false;
    int found = 0;
    for (auto& p : parts_) {
        if (!p.removable) continue;
        bool usb = p.mountPoint.find("usb") != std::string::npos || p.mountPoint.find("otg") != std::string::npos;
        (usb ? haveUsb : haveSd) = true;
        std::string kind = p.blockDev.empty() ? "unknown" : diskKind(p.blockDev);
        if (kind == "internal") {
            LOGE("%s: %s is the phone's own storage, not a removable disk - not using it", p.mountPoint.c_str(),
                 p.blockDev.c_str());
            p.blockDev.clear();
        }
        // A verified card / USB disk beats an fstab device that isn't one (or isn't there).
        const std::string& real = usb ? usbDev : sdDev;
        if (!real.empty() && (kind != (usb ? "usb" : "sd") || !util::exists(p.blockDev))) p.blockDev = real;
        if (!p.blockDev.empty() && util::exists(p.blockDev)) found++;
    }
    auto add = [&](const char* mp, const char* name, const std::string& dev) {
        Partition p;
        p.mountPoint = mp;
        p.fsType = "auto";
        p.removable = p.storage = true;
        p.displayName = name;
        p.blockDev = dev;
        finalize(p);
        found++;
    };
    if (!haveUsb && !usbDev.empty()) add("/usb_otg", "USB OTG", usbDev);
    if (!haveSd && !sdDev.empty()) add("/external_sd", "MicroSD", sdDev);
    refresh();
    return found;
}

bool Partitions::wipeDalvik(Task& t) {
    t.print("Wiping Dalvik / ART cache...");
    Partition* d = find("/data");
    if (d) mount(*d);
    Partition* c = find("/cache");
    if (c) mount(*c);
    for (auto dir : {"/data/dalvik-cache", "/cache/dalvik-cache", "/data/resource-cache"}) {
        std::string p = device::path(dir);
        if (util::exists(p)) {
            util::removeTree(p);
            t.print("  removed %s", dir);
        }
    }
    return true;
}

std::vector<Partition*> Partitions::storages() {
    std::vector<Partition*> v;
    for (auto& p : parts_)
        if (p.storage && p.present) v.push_back(&p);
    return v;
}

std::string Partitions::storagePath() {
    Partition* p = find(storage_);
    if (!p) return device::path("/tmp");
    mount(*p);
    if (p->mountPoint == "/data") {
        std::string media = device::path("/data/media/0");
        if (util::isDir(media)) return media;
        // File-based encryption without decryption: don't litter /data, use RAM.
        if (dataEncrypted()) return device::path("/tmp");
        if (util::isDir(device::path("/data/media"))) return device::path("/data/media");
    }
    return p->contentPath();
}

std::string Partitions::appDir() { return storagePath() + "/FMRRE"; }

std::string Partitions::legacyDir() { return storagePath() + "/TWRP"; }

bool Partitions::dataEncrypted() {
    Partition* d = find("/data");
    if (!d || !isMounted(*d)) return false;
    if (util::isDir(device::path("/data/media/0"))) return false;
    // Encrypted directories exist but their names are scrambled (no "0").
    return util::isDir(device::path("/data/media")) || device::prop("ro.crypto.state") == "encrypted";
}

void Partitions::setStorage(const std::string& mp) {
    storage_ = mp;
    Settings::get().set("storage", mp);
    Settings::get().save();
}

}  // namespace tw
