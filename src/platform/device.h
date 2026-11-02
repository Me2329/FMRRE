#pragma once
#include <cstdint>
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
bool switchSlot(std::string& msg);

}  // namespace tw::device
