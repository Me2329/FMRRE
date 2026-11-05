#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tw {

class Task;

namespace backup {

struct Item {
    std::string part;      // partition name, e.g. "data"
    std::string file;      // file name inside the backup folder
    std::string type;      // "tar" or "img"
    bool compressed = false;
    uint64_t size = 0;     // bytes on disk
    std::string sha256;
    bool encrypted = false;  // AES-256 (core/crypt.h); the SHA-256 above is of the encrypted file
    // Classic TWRP splits big archives into data.ext4.win000, .win001 ... - each a whole tar archive with its own
    // .sha2: `file` / `sha256` are the first part, these the rest.
    std::vector<std::pair<std::string, std::string>> extra;  // file, sha256
};

struct Set {
    std::string path, name;
    int64_t mtime = 0;
    uint64_t size = 0;
    std::vector<Item> items;
    bool hasManifest = false;
};

struct Options {
    std::vector<std::string> parts;  // mount points
    std::string name;
    bool compress = true;
    bool digest = true;
    std::string password;  // non-empty: encrypt every file (AES-256-CTR + HMAC-SHA256, PBKDF2 key)
    // Split a filesystem backup into parts of about this many bytes (data.fs.win000, .win001 ...) so it fits on a
    // FAT32 card (single file < 4 GiB). 0 = one file. Set automatically for a FAT target; images are never split.
    uint64_t splitSize = 0;
};

std::string backupRoot();  // <storage>/FMRRE/BACKUPS/<serial> (restore also lists <storage>/TWRP/BACKUPS/<serial>)
std::vector<Set> list();
uint64_t estimateSize(const std::string& mountPoint);
bool create(const Options& o, Task& t);
bool restore(const Set& s, const std::vector<std::string>& parts, bool verifyFirst, Task& t, const std::string& password = "");
bool encrypted(const Set& s);
// Checks the password against every encrypted file's header (nothing is decrypted yet).
bool checkPassword(const Set& s, const std::string& password, std::string& err);
bool verify(const Set& s, Task& t);

}  // namespace backup
}  // namespace tw
