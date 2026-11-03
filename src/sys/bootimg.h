#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tw {

class Task;

// Android boot image headers (v0-v4) and the root helpers built on them.
namespace bootimg {

struct Info {
    bool ok = false;
    std::string error;
    uint32_t headerVersion = 0;
    uint32_t kernelSize = 0, ramdiskSize = 0, pageSize = 4096;
    std::string osVersion, patchLevel;  // "15.0.0", "2026-10"
    std::string cmdline;
};
Info read(const std::string& dev);

// Where the generic ramdisk lives: init_boot on devices launched with Android 13+, boot otherwise.
struct RootTarget {
    std::string name;  // "init_boot" or "boot"
    std::string dev;
    Info info;
    std::string why;
};
RootTarget rootTarget();

enum class RootKind { None, Magisk, KernelSU, APatch, Unknown };
struct RootStatus {
    RootKind kind = RootKind::None;
    std::string detail;  // version / where it was detected
    bool dataReadable = true;
};
RootStatus detectRoot();
const char* rootName(RootKind k);

struct Module {
    std::string id, name, version, author, description, dir;
    bool disabled = false, removing = false;
};
std::vector<Module> modules();  // /data/adb/modules (Magisk, KernelSU and APatch share the layout)
bool setModuleDisabled(const Module& m, bool disabled);
bool setModuleRemove(const Module& m, bool remove);
int disableAllModules();  // the classic "bootloop after installing a module" fix

std::string backupDir();  // <storage>/TWRP/root
std::string stockDir();   // <storage>/TWRP/stock  (recovery / boot saved before installing TWRP v2)

// Where a recovery image belongs: recovery(_slot) if the phone has that partition, vendor_boot for
// vendor_boot images, otherwise boot (A/B phones that keep the recovery ramdisk in boot).
RootTarget recoveryTarget(const std::string& image);
// Copy the stock target image to storage before rooting (skipped if an identical copy exists).
bool backupStock(const RootTarget& t, Task& task, std::string* outFile = nullptr);
std::vector<std::string> stockBackups();
// Same, into any folder (skipped if an identical copy is already there).
bool backupPartition(const RootTarget& t, const std::string& dir, Task& task, std::string* outFile = nullptr);
std::vector<std::string> imagesIn(const std::string& dir);
// Back up the target, flash `image`, verify. Used by the UI and `twrp recovery <image>`.
bool installRecovery(const std::string& image, Task& task);

}  // namespace bootimg
}  // namespace tw
