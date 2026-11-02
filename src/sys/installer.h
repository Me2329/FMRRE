#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tw {

class Task;

namespace installer {

struct ZipInfo {
    bool ok = false;
    std::string error;
    int entries = 0;
    uint64_t uncompressed = 0;
    bool hasUpdateBinary = false;
    bool hasUpdaterScript = false;
    bool isMagiskModule = false;     // module.prop at root
    bool hasPayload = false;         // payload.bin (full OTA, A/B)
    std::string kind;                // human readable guess
    std::vector<std::string> names;  // first entries
};

ZipInfo inspectZip(const std::string& path);
bool testZip(const std::string& path, Task& t);  // CRC-check all entries

struct InstallOptions {
    bool verifyZip = true;
    bool wipeCacheAfter = false;
};
bool installZip(const std::string& path, const InstallOptions& o, Task& t);

// Flash a raw or Android-sparse image to a block device.
bool flashImage(const std::string& image, const std::string& blockDev, Task& t);
bool isSparseImage(const std::string& path);

}  // namespace installer
}  // namespace tw
