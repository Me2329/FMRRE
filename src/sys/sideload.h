#pragma once
#include <atomic>
#include <string>

namespace tw {

class Task;

namespace sideload {

// Receive a flashable zip with `adb sideload` and return a path to it. FMRRE speaks the adb protocol itself
// (sys/adbd.h), so no minadbd is needed: over USB on a phone, over TCP in the simulator or with tcpPort
// (`adb connect 127.0.0.1:PORT` first). The package is stored on internal storage, /cache or /tmp - wherever
// it fits. Progress goes to `t`; "" on failure or cancel. The computer keeps waiting until finish().
std::string receive(Task& t, int tcpPort = 0);
// After installing it: `adb sideload` on the computer then exits with success or failure. The caller removes the file.
void finish(bool installed);

}  // namespace sideload
}  // namespace tw
