#pragma once
#include <cstdint>
#include <string>

namespace tw {

class Task;

// MTP: the phone's storage shows up on a computer (Windows Explorer, macOS with Android File Transfer /
// OpenMTP, Linux file managers through libmtp) while FMRRE runs, to copy ROMs and backups both ways.
// USB through FunctionFS (added next to adb, which keeps working); TCP in the simulator, for the tests.
namespace mtp {

struct Options {
    bool usb = true;
    int tcpPort = 0;  // also/instead serve MTP over TCP (simulator, tests); 0 = off
    std::string tcpBind = "127.0.0.1";
};

struct Status {
    bool running = false, connected = false;
    std::string transport, peer, error;
    uint64_t bytesIn = 0, bytesOut = 0;
    int filesIn = 0, filesOut = 0;
};
Status status();

// In the background (the Mount screen's MTP switch). false + err when it can't start.
bool start(const Options& o, std::string& err);
void stop();
bool running();
// In the foreground (`fmrre mtp`): serves until the task is cancelled.
bool run(const Options& o, Task& t);

}  // namespace mtp
}  // namespace tw
