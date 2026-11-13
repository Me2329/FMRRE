#pragma once
#include <string>
#include <vector>

namespace tw {

// Keeps the logs where a person can get at them without adb - the first thing needed when FMRRE meets a new phone
// (a stock recovery ramdisk has no adb shell, and /tmp is gone at the next reboot):
//   /cache/recovery/last_log + last_kmsg      the stock recovery's "View recovery logs" lists them
//   <card or USB disk>/FMRRE/logs/            recovery.log, kernel.log, selinux.txt - a microSD goes into a PC
//   <internal storage>/FMRRE/logs/            when it is readable
namespace logsave {

enum class Removable { No, IfMounted, Mount };

std::string kernelLog(size_t maxBytes = 1 << 20);  // the kernel's ring buffer (dmesg)
// What SELinux refused FMRRE ("avc: denied" lines about the recovery process), each kind once, readable.
std::vector<std::string> selinuxDenials(const std::string& kernelLog);
std::string selinuxMode();  // "enforcing", "permissive", or "" when the kernel has no SELinux
std::string selinuxReport(const std::string& kernelLog);

// Writes the logs out now and returns the folders written (as the user knows them).
std::vector<std::string> save(const char* why, Removable removable);
// Cheap to call often (the UI's idle hook): saves once shortly after start, then when the log has grown.
void tick();

}  // namespace logsave

}  // namespace tw
