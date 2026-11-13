#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace tw {

class Task;

// fastbootd built into the recovery: the userspace fastboot that `fastboot reboot fastboot`
// (or Reboot > Fastbootd) lands in. It flashes logical partitions in super (the reason GSIs
// must be flashed from fastbootd and not the bootloader), resizes/creates/deletes them, sets
// the active slot, erases, fetches, and adds `fastboot oem fmrre <command>` (or `oem twrp`) to drive FMRRE.
// Transports: USB through FunctionFS (like AOSP's fastbootd), and TCP (fastboot -s tcp:HOST),
// which the simulator and emulators use.
namespace fastboot {

struct Options {
    bool usb = true;   // FunctionFS gadget (real devices)
    int tcpPort = 0;   // also/instead accept `fastboot -s tcp:<ip>:<port>`; 0 = off
    std::string tcpBind = "0.0.0.0";
};

// Live state for the UI.
struct State {
    std::mutex mu;
    std::string transport;          // "USB" / "TCP 127.0.0.1:5554"
    std::string peer;               // who is connected
    bool connected = false;
    std::string command;            // last command
    std::string result;             // OKAY / FAIL message of it
    uint64_t done = 0, total = 0;   // current download / flash
    std::vector<std::string> history;
    int commands = 0;
};
State& state();

// Runs until the computer reboots the phone (rebootTo = target, "" = system), asks for
// recovery (rebootTo = "recovery": just leave fastboot mode), or the task is cancelled.
bool run(const Options& o, Task& t, std::string& rebootTo);

// Value of a getvar for the tests/UI ("" + false if unknown).
bool getvar(const std::string& name, std::string& value);
uint64_t maxDownloadSize();

}  // namespace fastboot
}  // namespace tw
