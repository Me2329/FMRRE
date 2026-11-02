#include "sys/bootimg.h"

#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "core/sha256.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"

namespace tw::bootimg {

namespace {

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// os_version: 7 bits each A.B.C, then 7 bits year-2000 and 4 bits month.
void decodeOs(uint32_t v, Info& i) {
    if (!v) return;
    uint32_t ver = v >> 11, lvl = v & 0x7ff;
    i.osVersion = std::to_string((ver >> 14) & 0x7f) + "." + std::to_string((ver >> 7) & 0x7f) + "." + std::to_string(ver & 0x7f);
    char b[16];
    snprintf(b, sizeof(b), "%04u-%02u", 2000 + (lvl >> 4), lvl & 0xf);
    i.patchLevel = b;
}

std::string slotDev(const std::string& name) {
    std::string by = Partitions::byNameDir(), slot = device::info().slot;
    for (auto c : {name + slot, name})
        if (util::exists(by + "/" + c)) return by + "/" + c;
    return "";
}

std::string adb() { return device::path("/data/adb"); }

}  // namespace

Info read(const std::string& dev) {
    Info i;
    int fd = open(dev.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { i.error = "cannot open " + dev; return i; }
    uint8_t h[1660] = {};
    ssize_t n = pread(fd, h, sizeof(h), 0);
    close(fd);
    if (n < 64 || memcmp(h, "ANDROID!", 8) != 0) { i.error = "no Android boot image header"; return i; }
    i.headerVersion = le32(h + 40);
    if (i.headerVersion >= 3) {
        i.kernelSize = le32(h + 8);
        i.ramdiskSize = le32(h + 12);
        decodeOs(le32(h + 16), i);
        i.cmdline.assign((const char*)h + 44, strnlen((const char*)h + 44, 1536));
    } else {
        i.kernelSize = le32(h + 8);
        i.ramdiskSize = le32(h + 16);
        i.pageSize = le32(h + 36);
        decodeOs(le32(h + 44), i);
        i.cmdline.assign((const char*)h + 64, strnlen((const char*)h + 64, 512));
    }
    i.ok = true;
    return i;
}

RootTarget rootTarget() {
    RootTarget t;
    std::string ib = slotDev("init_boot");
    if (!ib.empty()) {
        Info i = read(ib);
        if (i.ok && i.ramdiskSize > 0) {
            t = {"init_boot", ib, i, "This device has init_boot (Android 13+ launch): the generic ramdisk lives there."};
            return t;
        }
    }
    std::string b = slotDev("boot");
    t.name = "boot";
    t.dev = b;
    t.info = b.empty() ? Info() : read(b);
    if (b.empty()) t.why = "No boot partition found.";
    else if (!t.info.ok) t.why = "boot has no valid header (" + t.info.error + ").";
    else if (t.info.ramdiskSize == 0)
        t.why = "boot has no ramdisk (header v" + std::to_string(t.info.headerVersion) +
                "). On this device root is installed by patching boot anyway (Magisk handles it) or via vendor_boot.";
    else
        t.why = "The ramdisk is in boot.";
    return t;
}

const char* rootName(RootKind k) {
    switch (k) {
        case RootKind::Magisk: return "Magisk";
        case RootKind::KernelSU: return "KernelSU";
        case RootKind::APatch: return "APatch";
        case RootKind::Unknown: return "Unknown root";
        default: return "Not rooted";
    }
}

RootStatus detectRoot() {
    RootStatus s;
    if (Partitions::get().dataEncrypted()) s.dataReadable = false;
    std::string a = adb();
    if (util::isDir(a + "/magisk") || util::exists(a + "/magisk.db")) {
        s.kind = RootKind::Magisk;
        std::string v = util::readLine(a + "/magisk/util_functions.sh");
        std::string data;
        if (util::readFile(a + "/magisk/util_functions.sh", data, 64 * 1024)) {
            auto p = data.find("MAGISK_VER='");
            if (p != std::string::npos) v = data.substr(p + 12, data.find('\'', p + 12) - p - 12);
            else v.clear();
        }
        s.detail = v.empty() ? "/data/adb/magisk" : "Magisk " + v;
    } else if (util::isDir(a + "/ksu") || util::exists(a + "/ksud")) {
        s.kind = RootKind::KernelSU;
        s.detail = "/data/adb/ksu";
    } else if (util::isDir(a + "/ap") || util::exists(a + "/apd")) {
        s.kind = RootKind::APatch;
        s.detail = "/data/adb/ap";
    } else if (util::isDir(a + "/modules")) {
        s.kind = RootKind::Unknown;
        s.detail = "/data/adb/modules exists";
    }
    return s;
}

std::vector<Module> modules() {
    std::vector<Module> out;
    std::string root = adb() + "/modules";
    for (auto& e : util::listDir(root)) {
        if (!e.dir) continue;
        Module m;
        m.id = e.name;
        m.dir = root + "/" + e.name;
        std::string prop;
        util::readFile(m.dir + "/module.prop", prop, 16 * 1024);
        for (auto& l : util::split(prop, '\n')) {
            auto eq = l.find('=');
            if (eq == std::string::npos) continue;
            std::string k = util::trim(l.substr(0, eq)), v = util::trim(l.substr(eq + 1));
            if (k == "name") m.name = v;
            else if (k == "version") m.version = v;
            else if (k == "author") m.author = v;
            else if (k == "description") m.description = v;
        }
        if (m.name.empty()) m.name = m.id;
        m.disabled = util::exists(m.dir + "/disable");
        m.removing = util::exists(m.dir + "/remove");
        out.push_back(m);
    }
    return out;
}

bool setModuleDisabled(const Module& m, bool d) {
    std::string f = m.dir + "/disable";
    return d ? util::writeFile(f, "") : (unlink(f.c_str()) == 0 || !util::exists(f));
}

bool setModuleRemove(const Module& m, bool r) {
    std::string f = m.dir + "/remove";
    return r ? util::writeFile(f, "") : (unlink(f.c_str()) == 0 || !util::exists(f));
}

int disableAllModules() {
    int n = 0;
    for (auto& m : modules())
        if (!m.disabled && setModuleDisabled(m, true)) n++;
    return n;
}

std::string backupDir() { return Partitions::get().storagePath() + "/TWRP/root"; }

std::vector<std::string> stockBackups() {
    std::vector<std::string> v;
    for (auto& e : util::listDir(backupDir()))
        if (!e.dir && util::endsWith(e.name, ".img")) v.push_back(backupDir() + "/" + e.name);
    return v;
}

bool backupStock(const RootTarget& t, Task& task, std::string* outFile) {
    if (t.dev.empty()) { task.print("No %s partition to back up", t.name.c_str()); return false; }
    std::string tag = t.info.patchLevel.empty() ? util::dateStamp() : t.info.patchLevel;
    std::string f = backupDir() + "/" + t.name + device::info().slot + "_stock_" + tag + ".img";
    task.setStatus("Backing up stock " + t.name);
    std::string want = Sha256::hexOfFile(t.dev);
    for (auto& b : stockBackups())
        if (util::fileSize(b) == Partitions::blockSize(t.dev) && Sha256::hexOfFile(b) == want) {
            task.print("Stock %s already backed up: %s", t.name.c_str(), util::basename(b).c_str());
            if (outFile) *outFile = b;
            return true;
        }
    util::mkdirs(backupDir());
    if (!util::copyFile(t.dev, f, [&](uint64_t d, uint64_t tot) { if (tot) task.setSubProgress(float(d) / tot); })) {
        task.print("Backup of %s failed", t.name.c_str());
        return false;
    }
    util::writeFile(f + ".sha256", want + "  " + util::basename(f) + "\n");
    task.print("Saved stock %s to %s", t.name.c_str(), f.c_str());
    if (outFile) *outFile = f;
    return true;
}

}  // namespace bootimg
