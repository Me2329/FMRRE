#pragma once
#include <cstdint>
#include <string>

namespace tw {

class Task;

// Android Verified Boot: the vbmeta header flags. Setting both flags is what
// `fastboot --disable-verity --disable-verification flash vbmeta` does, and most
// GSIs (and custom kernels) need it to boot. Only works with an unlocked bootloader.
namespace avb {

constexpr uint32_t kDisableVerity = 1, kDisableVerification = 2;

struct Info {
    bool ok = false;
    std::string error;
    uint32_t flags = 0;
    uint64_t rollbackIndex = 0;
    uint32_t versionMajor = 0, versionMinor = 0;
    std::string release;  // e.g. "avbtool 1.3.0"
};

Info read(const std::string& dev);
bool setFlags(const std::string& dev, uint32_t flags, std::string& err);

// Patch vbmeta of the current slot (or both). The original header is saved to storage first.
bool disableVerification(bool bothSlots, Task& t);
// Restore the flags to 0 (verification enabled).
bool enableVerification(bool bothSlots, Task& t);

std::string vbmetaDevice(const std::string& slot);  // "" if the device has none

}  // namespace avb
}  // namespace tw
