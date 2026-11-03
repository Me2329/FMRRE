#include "platform/device.h"
#include "core/log.h"
#include "core/util.h"

#include <cmath>
#include <map>
#include <mutex>
#include <linux/reboot.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace tw::device {

static bool g_sim = false;
static std::string g_simRoot;
static std::map<std::string, std::string> g_props;
static std::mutex g_propMu;
static Info g_info;

static void loadPropFile(const std::string& p) {
    std::string data;
    if (!util::readFile(p, data)) return;
    for (auto& line : util::split(data, '\n')) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        g_props.emplace(util::trim(line.substr(0, eq)), util::trim(line.substr(eq + 1)));
    }
}

static void loadProps() {
    std::lock_guard<std::mutex> lk(g_propMu);
    g_props.clear();
    if (g_sim) {
        loadPropFile(g_simRoot + "/sim.prop");
        return;
    }
    // Live properties from init via getprop (works from a static binary too).
    std::string out;
    if (util::runCapture("getprop", out) == 0) {
        for (auto& line : util::split(out, '\n')) {
            // [key]: [value]
            auto a = line.find("]: [");
            if (line.size() < 6 || line[0] != '[' || a == std::string::npos) continue;
            std::string k = line.substr(1, a - 1);
            std::string v = line.substr(a + 4);
            if (!v.empty() && v.back() == ']') v.pop_back();
            g_props[k] = v;
        }
    }
    for (auto f : {"/prop.default", "/default.prop", "/system/build.prop", "/vendor/build.prop"}) loadPropFile(f);
}

void init(bool sim, const std::string& simRoot) {
    g_sim = sim;
    g_simRoot = simRoot;
    loadProps();
    Info& i = g_info;
    auto first = [](std::initializer_list<const char*> keys, const char* def) {
        for (auto k : keys) {
            std::string v = prop(k);
            if (!v.empty()) return v;
        }
        return std::string(def);
    };
    i.model = first({"ro.product.model", "ro.product.vendor.model", "ro.product.system.model"}, "Android device");
    i.device = first({"ro.product.device", "ro.product.vendor.device", "ro.build.product"}, "unknown");
    i.brand = first({"ro.product.brand", "ro.product.vendor.brand"}, "");
    i.manufacturer = first({"ro.product.manufacturer", "ro.product.vendor.manufacturer"}, "");
    i.android = first({"ro.build.version.release", "ro.vendor.build.version.release"}, "?");
    i.sdk = first({"ro.build.version.sdk", "ro.vendor.build.version.sdk"}, "?");
    i.slot = first({"ro.boot.slot_suffix"}, "");
    if (i.slot.empty()) {
        std::string s = prop("ro.boot.slot");
        if (!s.empty()) i.slot = "_" + s;
    }
    i.ab = !i.slot.empty();
    i.serial = first({"ro.serialno", "ro.boot.serialno"}, "0123456789");
    i.fingerprint = first({"ro.build.fingerprint", "ro.vendor.build.fingerprint"}, "");
    i.patch = first({"ro.build.version.security_patch", "ro.vendor.build.security_patch"}, "");
    i.platform = first({"ro.board.platform", "ro.hardware"}, "");
    struct utsname u;
    if (g_sim) i.kernel = prop("sim.kernel", "5.15.137-android14");
    else if (uname(&u) == 0) i.kernel = u.release;
    LOGI("device: %s (%s) Android %s, slot '%s', kernel %s", i.model.c_str(), i.device.c_str(), i.android.c_str(),
         i.slot.c_str(), i.kernel.c_str());
}

bool isSim() { return g_sim; }
std::string simRoot() { return g_simRoot; }
std::string path(const std::string& p) { return g_sim ? g_simRoot + p : p; }

std::string prop(const std::string& key, const std::string& def) {
    std::lock_guard<std::mutex> lk(g_propMu);
    auto it = g_props.find(key);
    return it == g_props.end() || it->second.empty() ? def : it->second;
}

bool setProp(const std::string& key, const std::string& value) {
    {
        std::lock_guard<std::mutex> lk(g_propMu);
        g_props[key] = value;
    }
    if (g_sim) return true;
    std::string out;
    return util::runCapture("setprop " + util::shellQuote(key) + " " + util::shellQuote(value), out) == 0;
}

const Info& info() { return g_info; }

const char* buildAbi() {
#if defined(__aarch64__)
    return "arm64-v8a";
#elif defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7
    return "armeabi-v7a";
#elif defined(__arm__)
    return "armeabi";
#elif defined(__x86_64__)
    return "x86_64";
#elif defined(__i386__)
    return "x86";
#elif defined(__riscv) && __riscv_xlen == 64
    return "riscv64";
#elif defined(__mips__) && !defined(__mips64)
    return "mips";
#else
    return "unknown";
#endif
}

bool animationsByDefault() { return std::string(buildAbi()) != "armeabi"; }

std::string machine() {
    struct utsname u;
    return uname(&u) == 0 ? u.machine : "";
}

std::string abiAdvice() {
    std::string m = machine(), b = buildAbi();
    bool kernel64 = m == "aarch64" || m == "armv8l" || m == "armv8b" || m == "x86_64";
    if (kernel64 && (b == "armeabi-v7a" || b == "armeabi")) return "64-bit CPU: the arm64 build is faster and works where 32-bit apps are not supported";
    if (m == "x86_64" && b == "x86") return "64-bit CPU: the x86_64 build is faster";
    if ((m == "armv7l" || m == "armv8l") && b == "armeabi") return "ARMv7 CPU: the armeabi-v7a build is much faster (hardware floating point)";
    return "";
}

static std::string psDir() {
    std::string base = path("/sys/class/power_supply");
    for (auto n : {"battery", "Battery", "BAT0", "bms"})
        if (util::exists(base + "/" + n + "/capacity")) return base + "/" + n;
    for (auto& e : util::listDir(base))
        if (util::exists(base + "/" + e.name + "/capacity")) return base + "/" + e.name;
    return "";
}

Battery battery() {
    Battery b;
    if (g_sim) {
        // A gently moving fake battery for the simulator.
        int64_t t = util::nowMs() / 1000;
        b.percent = 78 - int((t / 60) % 10);
        b.status = "Charging";
        b.charging = true;
        b.tempC = 31.5f;
        b.voltageMv = 4123;
        b.currentMa = 980;
        b.health = "Good";
        return b;
    }
    static std::string dir = psDir();
    if (dir.empty()) return b;
    auto num = [](const std::string& p, long def) {
        std::string s = util::readLine(p);
        try { return s.empty() ? def : std::stol(s); } catch (...) { return def; }
    };
    b.percent = (int)num(dir + "/capacity", -1);
    b.status = util::readLine(dir + "/status");
    b.charging = b.status == "Charging" || b.status == "Full";
    long t = num(dir + "/temp", -10000);
    if (t > -10000) b.tempC = t / 10.0f;
    b.voltageMv = int(num(dir + "/voltage_now", 0) / 1000);
    long cur = num(dir + "/current_now", 0);
    b.currentMa = int(std::labs(cur) > 20000 ? cur / 1000 : cur);
    b.health = util::readLine(dir + "/health");
    return b;
}

std::vector<ThermalZone> thermals() {
    std::vector<ThermalZone> v;
    if (g_sim) {
        float wob = std::sin(util::nowMs() / 3000.0f);
        v.push_back({"cpu-0-0", 41.2f + wob * 2});
        v.push_back({"cpu-1-0", 43.8f + wob * 2.5f});
        v.push_back({"gpu", 38.1f + wob});
        v.push_back({"battery", 31.5f});
        v.push_back({"skin", 33.0f});
        return v;
    }
    for (int i = 0; i < 80; i++) {
        std::string base = "/sys/class/thermal/thermal_zone" + std::to_string(i);
        if (!util::exists(base)) break;
        std::string ty = util::readLine(base + "/type");
        std::string t = util::readLine(base + "/temp");
        if (t.empty()) continue;
        long raw;
        try { raw = std::stol(t); } catch (...) { continue; }
        float c = std::labs(raw) > 1000 ? raw / 1000.0f : float(raw);
        if (c < -30 || c > 150) continue;
        v.push_back({ty, c});
    }
    return v;
}

float cpuTempC() {
    auto z = thermals();
    float best = -1000;
    for (auto& t : z) {
        std::string ty = util::toLower(t.type);
        if (ty.find("cpu") != std::string::npos || ty.find("tsens") != std::string::npos ||
            ty.find("soc") != std::string::npos || ty.find("ap") == 0)
            best = std::max(best, t.c);
    }
    if (best < -100 && !z.empty()) best = z[0].c;
    return best;
}

CpuStat cpu() {
    static uint64_t lastIdle = 0, lastTotal = 0;
    CpuStat s;
    s.cores = (int)sysconf(_SC_NPROCESSORS_CONF);
    std::string data;
    if (util::readFile("/proc/stat", data, 4096)) {
        auto line = data.substr(0, data.find('\n'));
        auto f = util::splitWs(line);
        if (f.size() >= 5 && f[0] == "cpu") {
            uint64_t total = 0, idle = 0;
            for (size_t i = 1; i < f.size(); i++) {
                uint64_t v = std::stoull(f[i]);
                total += v;
                if (i == 4 || i == 5) idle += v;
            }
            if (lastTotal && total > lastTotal) s.usage = 1.0f - float(idle - lastIdle) / float(total - lastTotal);
            lastIdle = idle;
            lastTotal = total;
        }
    }
    for (int i = 0; i < s.cores && i < 16; i++) {
        std::string f = util::readLine("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_cur_freq");
        s.freqMhz.push_back(f.empty() ? 0 : int(std::stol(f) / 1000));
    }
    return s;
}

Mem memory() {
    Mem m;
    std::string data;
    if (!util::readFile("/proc/meminfo", data)) return m;
    for (auto& line : util::split(data, '\n')) {
        auto f = util::splitWs(line);
        if (f.size() < 2) continue;
        uint64_t kb = std::stoull(f[1]) * 1024;
        if (f[0] == "MemTotal:") m.total = kb;
        else if (f[0] == "MemAvailable:") m.avail = kb;
        else if (f[0] == "SwapTotal:") m.swapTotal = kb;
        else if (f[0] == "SwapFree:") m.swapFree = kb;
    }
    return m;
}

int64_t uptimeSec() {
    std::string s = util::readLine("/proc/uptime");
    try { return (int64_t)std::stod(s); } catch (...) { return 0; }
}

static std::string blDir() {
    static std::string d = [] {
        if (g_sim) return std::string();
        std::string base = "/sys/class/backlight";
        for (auto& e : util::listDir(base))
            if (util::exists(base + "/" + e.name + "/brightness")) return base + "/" + e.name;
        if (util::exists("/sys/class/leds/lcd-backlight/brightness")) return std::string("/sys/class/leds/lcd-backlight");
        return std::string();
    }();
    return d;
}

static int g_simBrightness = 180;

int brightnessMax() {
    if (g_sim) return 255;
    std::string d = blDir();
    std::string s = d.empty() ? "" : util::readLine(d + "/max_brightness");
    try { return s.empty() ? 255 : std::stoi(s); } catch (...) { return 255; }
}

int brightness() {
    if (g_sim) return g_simBrightness;
    std::string d = blDir();
    std::string s = d.empty() ? "" : util::readLine(d + "/brightness");
    try { return s.empty() ? brightnessMax() : std::stoi(s); } catch (...) { return 0; }
}

void setBrightness(int v) {
    if (g_sim) { g_simBrightness = v; return; }
    std::string d = blDir();
    if (!d.empty()) util::writeFile(d + "/brightness", std::to_string(v));
}

void vibrate(int ms) {
    if (g_sim || ms <= 0) return;
    if (util::exists("/sys/class/leds/vibrator/activate")) {
        util::writeFile("/sys/class/leds/vibrator/duration", std::to_string(ms));
        util::writeFile("/sys/class/leds/vibrator/activate", "1");
    } else if (util::exists("/sys/class/timed_output/vibrator/enable")) {
        util::writeFile("/sys/class/timed_output/vibrator/enable", std::to_string(ms));
    }
}

void reboot(const std::string& target) {
    LOGI("Rebooting to '%s'", target.empty() ? "system" : target.c_str());
    if (g_sim) {
        LOGI("(simulator) reboot request ignored");
        return;
    }
    sync();
    std::string arg = target == "poweroff" ? "shutdown," : ("reboot," + target);
    // The Android way: ask init (works for every target incl. fastbootd).
    setProp("sys.powerctl", arg);
    sleep(5);
    // Fallback: direct syscall.
    if (target == "poweroff") {
        ::reboot(RB_POWER_OFF);
    } else {
        syscall(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, target.c_str());
        ::reboot(RB_AUTOBOOT);
    }
}

bool switchSlot(std::string& msg) {
    const Info& i = info();
    if (!i.ab) { msg = "This device is not A/B"; return false; }
    return setActiveSlot(i.slot == "_a" ? "_b" : "_a", msg);
}

bool setActiveSlot(const std::string& suffix, std::string& msg) {
    const Info& i = info();
    if (!i.ab) { msg = "This device is not A/B"; return false; }
    if (suffix != "_a" && suffix != "_b") { msg = "Unknown slot " + suffix; return false; }
    int target = suffix == "_b" ? 1 : 0;
    std::string name = target ? "B" : "A";
    if (g_sim) {
        g_info.slot = suffix;
        msg = "Active slot set to " + name;
        return true;
    }
    std::string out;
    int rc = util::runCapture("bootctl set-active-boot-slot " + std::to_string(target), out);
    if (rc != 0) {
        msg = "bootctl failed (" + util::trim(out) + "). From a computer: fastboot set_active " + std::string(target ? "b" : "a");
        return false;
    }
    msg = "Next boot will use slot " + name;
    return true;
}

}  // namespace tw::device
