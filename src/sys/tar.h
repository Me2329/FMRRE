#pragma once
// tar, built in: backups no longer depend on the ramdisk's tar, and SELinux labels always survive a backup and a
// restore (without them a restored /data or system often doesn't boot).
//
// Written: POSIX pax/ustar like GNU tar - regular files, directories, symlinks, hard links, devices, FIFOs - with
// owners, modes and times; extended attributes as pax records: the SELinux label as "RHT.security.selinux" (what
// toybox tar --selinux, GNU tar --selinux and TWRP read) and as "SCHILY.xattr.security.selinux", the others as
// "SCHILY.xattr.<name>" (GNU tar --xattrs);
// long names, long link targets and files of 8 GiB or more through pax as well.
// Read: all of that, plus GNU long names ('L' / 'K') and classic TWRP backups, whose names are absolute
// ("/data/app/..."): they are put back relative to the partition. Nothing is written outside the target folder:
// "..", absolute names of other partitions and paths through symlinks are refused.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tw::tar {

struct Stats {
    uint64_t files = 0, dirs = 0, links = 0, other = 0, bytes = 0;
    int labels = 0;         // SELinux labels stored / put back
    int xattrFailed = 0;    // extended attributes that couldn't be read or set
    int refused = 0;        // entries not extracted: outside the folder, damaged names
    int skipped = 0;        // sockets (tar can't hold them), files that vanished
    std::vector<std::string> notes;  // the first few problems, for the log
};

using Sink = std::function<bool(const void*, size_t)>;
// Archive everything under `root` as "./..." (like `cd root && tar -cf - .`), minus `exclude` (names relative to
// root, e.g. "media", "lost+found"). cancelled() is asked between files.
bool create(const std::string& root, const std::vector<std::string>& exclude, const Sink& out, Stats& st,
            std::string& err, const std::function<bool()>& cancelled = {});

// Extraction is fed the archive in pieces (the restore streams it out of gzip / decryption).
class Extractor {
public:
    // root: the target folder. absPrefixes: absolute names that mean root itself (a classic TWRP backup of /data
    // stores "/data/app/x": with "/data" it becomes root/app/x).
    Extractor(std::string root, std::vector<std::string> absPrefixes = {});
    ~Extractor();
    bool feed(const void* data, size_t n);  // false: damaged archive or a write error (error())
    bool finish();                          // the end: applies the directories' own times and modes
    // The next part of a split backup follows (each part is a whole archive with its own end blocks).
    bool nextArchive();
    const Stats& stats() const { return st_; }
    const std::string& error() const { return err_; }

private:
    struct Impl;
    Impl* d_;
    Stats st_;
    std::string err_;
};

}  // namespace tw::tar
