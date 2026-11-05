#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace tw {

class Task;

// The device side of the adb protocol - just what `adb sideload` needs, like AOSP's minadbd, so no minadbd is
// needed in the ramdisk: the connection (no authentication, as in recovery), the sideload-host service (the
// computer serves the package block by block, which recovery asks for), the old sideload service (the whole
// file pushed) and `adb reboot ...`. Other services are refused. Over USB through FunctionFS - the
// /dev/usb-ffs/adb function adbd uses, with adbd stopped meanwhile - or over TCP (the simulator, the tests:
// `adb connect 127.0.0.1:PORT`).
namespace adbd {

struct Options {
    bool usb = true;
    int tcpPort = 0;  // instead of USB
    std::string tcpBind = "127.0.0.1";
};

class Sideload {
public:
    Sideload();
    ~Sideload();  // closes the connection (as a failure, if finish() wasn't called) and gives USB back to adbd
    // Waits for `adb sideload`. The package is then read from the computer as it installs, through FUSE at
    // /sideload/package.zip (as in AOSP's recovery: nothing is stored, no free space needed; a block read twice
    // must not change). Without FUSE - or with the old protocol, which pushes the whole file - it is stored in the
    // first of internal storage, /cache, /tmp that has room. Returns its path, or "" (why: in t's log).
    std::string receive(const Options& o, Task& t);
    bool streamed() const;  // read from the computer as it installs (FUSE)
    // After installing: the computer is told (adb sideload exits 0, or reports the failure), then it is closed.
    void finish(bool installed);
    std::string reboot;  // the computer asked for `adb reboot <target>` instead ("" = system, "-" = none)

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace adbd
}  // namespace tw
