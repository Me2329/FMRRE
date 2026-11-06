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
    bool isRom = false;              // writes a whole system (payload.bin, system.new.dat, system.img, super.img)
    std::string kind;                // human readable guess
    std::vector<std::string> names;  // first entries
};

ZipInfo inspectZip(const std::string& path);
bool testZip(const std::string& path, Task& t);  // CRC-check all entries

struct InstallOptions {
    bool verifyZip = true;
    bool wipeCacheAfter = false;
    bool checkSignature = true;     // show whose signature it is; warn when a ROM is signed with another key
    bool requireSignature = false;  // refuse zips not signed with a trusted key (Android recovery's rule)
};
bool installZip(const std::string& path, const InstallOptions& o, Task& t);

// The signature step of an install (also used before A/B OTAs): prints the verdict, warns about a ROM signed
// with another key than the installed system's, refuses when requireSignature and it isn't trusted.
// note: for the flash history ("signed: LineageOS (...)"); empty when not checked.
bool checkSignature(const std::string& path, const InstallOptions& o, bool isRom, Task& t, std::string& note);
// Settings: zip_sig, zip_sig_require.
InstallOptions optionsFromSettings();
// What the flash history says after a ROM that is signed with another key than the system it replaced.
extern const char kRomSwitchNote[];

// Flash a raw or Android-sparse image to a block device.
bool flashImage(const std::string& image, const std::string& blockDev, Task& t);
bool isSparseImage(const std::string& path);

}  // namespace installer
}  // namespace tw
