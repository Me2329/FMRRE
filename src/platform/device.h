#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tw::device {

// Simulator mode runs everything against a sandbox directory instead of the
// real block devices, so the recovery can be developed on a desktop.
void init(bool sim, const std::string& simRoot);
bool isSim();
std::string simRoot();
// Translate an on-device absolute path to where it lives (sandbox in sim mode).
std::string path(const std::string& p);

std::string prop(const std::string& key, const std::string& def = "");
bool setProp(const std::string& key, const std::string& value);

struct Info {
    std::string model, device, brand, manufacturer, android, sdk, slot, serial, kernel, fingerprint, patch, platform;
    bool ab = false;
};
const Info& info();

// The Android ABI this binary was built for (arm64-v8a, armeabi-v7a, armeabi, x86, x86_64, riscv64, mips).
const char* buildAbi();
// What the kernel reports (uname -m): aarch64, armv8l (32-bit mode on a 64-bit CPU), armv7l, i686, x86_64, ...
std::string machine();
// Default for the "Animations" setting: off on soft-float builds (armeabi), where the software renderer is slow.
bool animationsByDefault();
// Non-empty when a better build exists for this CPU (e.g. a 32-bit build running on a 64-bit kernel).
std::string abiAdvice();

struct Battery {
    int percent = -1;
    std::string status;  // Charging / Discharging / Full
    float tempC = -1000;
    int voltageMv = 0;
    int currentMa = 0;
    std::string health;
    bool charging = false;
};
Battery battery();

struct ThermalZone { std::string type; float c; };
std::vector<ThermalZone> thermals();
float cpuTempC();

struct CpuStat { float usage = 0; std::vector<int> freqMhz; int cores = 0; };
CpuStat cpu();  // usage since previous call
struct Mem { uint64_t total = 0, avail = 0, swapTotal = 0, swapFree = 0; };
Mem memory();
int64_t uptimeSec();

int brightnessMax();
int brightness();
void setBrightness(int v);
void vibrate(int ms);

// target: "" (system), recovery, bootloader, fastboot, edl, download, poweroff
void reboot(const std::string& target);
// Runs right before a reboot or power-off (the logs are saved to storage there: /tmp does not survive it).
void setBeforeReboot(std::function<void()> fn);
bool switchSlot(std::string& msg);
// Bootloader lock state from the boot properties (ro.boot.flash.locked, vbmeta.device_state,
// verifiedbootstate): 1 unlocked, 0 locked, -1 not reported. `how` gets the evidence ("verifiedbootstate=orange").
int bootloaderUnlocked(std::string* how = nullptr);
// Make slot "_a" / "_b" active for the next boot (boot control HAL via bootctl).
bool setActiveSlot(const std::string& suffix, std::string& msg);

}  // namespace tw::device
