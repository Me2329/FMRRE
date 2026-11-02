#pragma once
#include <atomic>
#include <string>

namespace tw {

class Task;

namespace sideload {

// Receive a flashable zip over ADB sideload and return a path to it.
//
// On a device this flips USB to the "sideload" function (so `adb sideload FILE`
// on the host picks the device up) and either spawns minadbd/adbd or polls
// well-known drop points (/sideload/package.zip, /tmp/update.zip,
// /cache/sideload.zip). In simulator mode it fakes a transfer with the sample
// zip baked into the sandbox, so the full UI flow can be exercised on a desktop.
//
// Writes human-readable progress into `t`. Returns the local path of the
// received package on success, or an empty string on failure or cancel. The
// caller is responsible for installing it (and then removing it from `/tmp`).
std::string receive(Task& t);

}  // namespace sideload
}  // namespace tw
