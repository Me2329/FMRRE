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
};

std::string backupRoot();  // <storage>/TWRP/BACKUPS/<serial>
std::vector<Set> list();
uint64_t estimateSize(const std::string& mountPoint);
bool create(const Options& o, Task& t);
bool restore(const Set& s, const std::vector<std::string>& parts, bool verifyFirst, Task& t);
bool verify(const Set& s, Task& t);

}  // namespace backup
}  // namespace tw
