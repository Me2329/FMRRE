#pragma once
#include <string>

namespace tw {

// FMRRE's own adb daemon for everything but sideload: `adb shell`, `adb pull`, `adb push`, `adb reboot`.
// A stock recovery ramdisk has no adbd for these (user builds start it for sideload only) and usually no shell
// either, so a phone running FMRRE in its stock ramdisk was silent to a computer - and the PC tools, which drive
// FMRRE with `adb shell fmrre ...`, had nothing to talk to. With this the phone answers adb whatever ROM it has.
//
//   shell      a command or an interactive session; exit codes (shell protocol v2). With the ramdisk's sh when
//              there is one; without, the command is run directly - `fmrre ...` always works.
//   sync       pull and push (files and folders); /sdcard is the phone's internal storage, as in Android
//   reboot     adb reboot [recovery|bootloader|download|...]
//
// No authentication, like every recovery - but while FMRRE's PIN lock is on, shell and file transfer are refused.
// Over USB through FunctionFS (the adb function, as the sideload receiver uses it), or TCP for the simulator and tests.
namespace adbserver {

struct Options {
    bool usb = true;
    int tcpPort = 0;  // instead of USB
    std::string tcpBind = "127.0.0.1";
};

bool start(const Options& o);  // in the background; false when it is already running
void stop();
bool running();
void setLocked(bool locked);   // FMRRE's PIN lock
void pause();                  // the sideload receiver takes the USB link for itself...
void resume();                 // ...and gives it back
std::string status();          // one line for the UI and the log
int serveForeground(const Options& o);  // `fmrre adbd --tcp PORT`: until killed (tests)

}  // namespace adbserver

}  // namespace tw
