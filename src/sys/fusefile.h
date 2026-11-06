#pragma once
// One read-only file served through the kernel's FUSE, its blocks fetched when something reads them: what
// `adb sideload` uses (like AOSP's fuse_sideload), so a package needs no room anywhere - the installer reads
// /sideload/package.zip and each block comes from the computer as it is needed.
//
// A block read again (the kernel dropped it from its cache) must be the same as the first time: every block's
// SHA-256 is kept, so a computer can't send one package to the signature check and another to the installer.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace tw::fusefile {

using FetchFn = std::function<bool(uint64_t block, std::string& data)>;  // that block's bytes; false: failed

class Server {
public:
    Server();
    ~Server();  // stop()
    // Mounts a FUSE filesystem on `dir` holding `name` (size bytes, in blocks of blockSize). false: no FUSE here
    // (no /dev/fuse, not allowed to mount) - err says why.
    bool start(const std::string& dir, const std::string& name, uint64_t size, uint32_t blockSize, FetchFn fetch,
               std::string& err);
    void stop();  // unmounts; reads still waiting fail
    std::string path() const;
    uint64_t fetched() const;     // bytes fetched (blocks read again count again)
    std::string problem() const;  // the first read that failed, and why ("" = none)

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace tw::fusefile
