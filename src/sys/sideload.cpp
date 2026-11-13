#include "sys/sideload.h"

#include <memory>
#include <unistd.h>

#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/adbd.h"
#include "sys/mtp.h"

namespace tw::sideload {

namespace {
std::unique_ptr<adbd::Sideload> g_session;  // open from receive() until finish(): the computer waits for the result
}

std::string receive(Task& t, int tcpPort) {
    g_session.reset();
    if (mtp::running()) {  // sideload needs the USB gadget to itself
        t.print("Turning MTP off for sideload");
        mtp::stop();
    }
    adbd::Options o;
    if (device::isSim() || tcpPort) {  // the simulator: real adb over TCP (adb connect 127.0.0.1:PORT)
        o.usb = false;
        o.tcpPort = tcpPort ? tcpPort : 5559;
    }
    t.print("ADB sideload - on your computer run:");
    t.print("    adb sideload <package.zip>");
    g_session = std::make_unique<adbd::Sideload>();
    std::string path = g_session->receive(o, t);
    if (path.empty() && g_session->reboot != "-") {
        std::string target = g_session->reboot;
        g_session.reset();
        t.print("Rebooting%s...", target.empty() ? "" : (" to " + target).c_str());
        device::reboot(target);
    }
    return path;
}

bool streamed() { return g_session && g_session->streamed(); }

void finish(bool installed) {
    if (!g_session) return;
    g_session->finish(installed);
    g_session.reset();
}

}  // namespace tw::sideload
