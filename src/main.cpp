// FMRRE - a modern Android custom recovery.
//
// On a device this binary is started by init from the recovery ramdisk
// (service recovery /system/bin/recovery). It draws directly to DRM/fbdev,
// reads /dev/input, and performs flashing, backups and wipes itself.
//
// Desktop development:
//   fmrre --sim                 X11 window (if built with X11) on a sandboxed fake device
//   fmrre --sim --headless --script demo.txt --out shots/

#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

#include "core/log.h"
#include "core/settings.h"
#include "core/task.h"
#include "core/util.h"
#include "gfx/png.h"
#include "platform/device.h"
#include "platform/display.h"
#include "platform/input.h"
#include "sim.h"
#include "sys/autofix.h"
#include "sys/mtp.h"
#include "sys/ors.h"
#include "sys/pathcheck.h"
#include "sys/partitions.h"
#include "ui/app.h"
#include "ui/screens/screens.h"

using namespace tw;

static void usage() {
    printf("FMRRE recovery\n"
           "  --sim               run against a sandboxed fake device (desktop)\n"
           "  --sim-root DIR      sandbox directory (default ./sim_root)\n"
           "  --size WxH          simulator resolution (default 540x1170)\n"
           "  --headless          no window; use with --script\n"
           "  --script FILE       automation: tap X Y | swipe X1 Y1 X2 Y2 MS | wait MS | key NAME | text STR | shot FILE\n"
           "  --out DIR           where 'shot' files are written\n"
           "  fmrre <command>     command line mode (also when run as 'fmrre'; 'twrp' works too): fmrre help\n");
}

static Key keyByName(const std::string& n) {
    if (n == "back") return Key::Back;
    if (n == "home") return Key::Home;
    if (n == "volup") return Key::VolUp;
    if (n == "voldown") return Key::VolDown;
    if (n == "power") return Key::Power;
    return Key::None;
}

static int runScript(App& app, const std::string& file, const std::string& outDir) {
    std::ifstream in(file);
    if (!in) { fprintf(stderr, "cannot open script %s\n", file.c_str()); return 1; }
    util::mkdirs(outDir);
    auto frames = [&](int ms) {
        int64_t end = util::nowMs() + ms;
        do {
            app.frame({});
            usleep(16000);
        } while (util::nowMs() < end);
    };
    std::string line;
    while (std::getline(in, line)) {
        line = util::trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;
        if (cmd == "wait") {
            int ms; ss >> ms; frames(ms);
        } else if (cmd == "tap") {
            int x, y; ss >> x >> y;
            app.frame({{EvType::TouchDown, x, y}});
            frames(50);
            app.frame({{EvType::TouchUp, x, y}});
            frames(30);
        } else if (cmd == "longpress") {
            int x, y; ss >> x >> y;
            app.frame({{EvType::TouchDown, x, y}});
            frames(700);
            app.frame({{EvType::TouchUp, x, y}});
        } else if (cmd == "swipe") {
            int x1, y1, x2, y2, ms; ss >> x1 >> y1 >> x2 >> y2 >> ms;
            app.frame({{EvType::TouchDown, x1, y1}});
            int steps = std::max(2, ms / 16);
            for (int i = 1; i <= steps; i++) {
                int x = x1 + (x2 - x1) * i / steps, y = y1 + (y2 - y1) * i / steps;
                app.frame({{EvType::TouchMove, x, y}});
                usleep(16000);
            }
            app.frame({{EvType::TouchUp, x2, y2}});
        } else if (cmd == "key") {
            std::string n; ss >> n;
            InputEvent e{EvType::KeyDown};
            e.key = keyByName(n);
            app.frame({e});
            e.type = EvType::KeyUp;
            app.frame({e});
        } else if (cmd == "text") {
            std::string rest; std::getline(ss, rest);
            InputEvent e{EvType::Text};
            e.text = util::trim(rest);
            app.frame({e});
        } else if (cmd == "stats") {
            // print and reset render statistics
            if (app.frameCount) printf("  [%s] frames: %d, avg render %.2f ms\n", util::trim(line.substr(5)).c_str(), app.frameCount,
                                       app.frameMsTotal / app.frameCount);
            app.frameCount = 0;
            app.frameMsTotal = 0;
        } else if (cmd == "open") {
            // open <screen> [path]: push a screen directly (screenshots and UI tests)
            std::string name, arg;
            ss >> name;
            std::getline(ss, arg);
            std::string raw = util::trim(arg);
            arg = pathcheck::resolve(raw);
            std::unique_ptr<Screen> sc;
            if (name == "install") {
                if (!raw.empty()) queueZip(arg);
                sc = makeInstallScreen();
            } else if (name == "monitor") sc = makeMonitorScreen();
            else if (name == "terminal") sc = makeTerminalScreen(raw);  // a command, not a path
            else if (name == "payload") sc = makePayloadScreen(arg);
            else if (name == "signature") sc = makeSignatureScreen(arg);
            else if (name == "history") sc = makeHistoryScreen();
            else if (name == "gsi") sc = makeGsiScreen(arg);
            else if (name == "super") sc = makeSuperScreen();
            else if (name == "flash") sc = makeFlashImageScreen(arg);
            else if (name == "files") sc = makeFilesScreen(arg);
            else if (name == "root") sc = makeRootScreen();
            else if (name == "advanced") sc = makeAdvancedScreen();
            else if (name == "fastboot") sc = makeFastbootScreen(arg != "--no-start");
            else if (name == "rescue") sc = makeRescueScreen();
            else if (name == "autofix") sc = makeAutoRepairScreen(arg == "--auto", arg == "--prompt");
            else if (name == "manual") sc = makeManualFlashScreen(arg);
            else if (name == "about") sc = makeAboutScreen();
            else if (name == "mount") sc = makeMountScreen();
            else if (name == "settings") sc = makeSettingsScreen();
            else if (name == "detail") sc = makeDetailScreen(raw == "first");
            else if (name == "deviceinfo") sc = makeDeviceInfoScreen();
            else if (name == "displaytest") sc = makeDisplayTestScreen();
            else if (name == "samsungap") sc = makeSamsungApScreen(arg);
            else if (name == "inputtest") sc = makeInputTestScreen();
            else if (name == "backup") sc = makeBackupScreen();
            else if (name == "restore") sc = makeRestoreScreen();
            if (sc) app.push(std::move(sc));
            else fprintf(stderr, "open: unknown screen %s\n", name.c_str());
            frames(450);
        } else if (cmd == "waittask") {
            while (Task::get().running()) frames(100);
            frames(400);
        } else if (cmd == "shot") {
            std::string f; ss >> f;
            app.frame({});
            savePng(app.canvas, util::joinPath(outDir, f));
            printf("shot %s\n", f.c_str());
        } else {
            fprintf(stderr, "unknown script command: %s\n", cmd.c_str());
        }
    }
    Task::get().join();
    if (app.frameCount) printf("frames: %d, avg render %.2f ms\n", app.frameCount, app.frameMsTotal / app.frameCount);
    return 0;
}

int main(int argc, char** argv) {
    // Run as `fmrre` / `twrp` (the links next to /system/bin/recovery): the command line. The desktop build is called
    // fmrre too, so there a leading option (--sim ...) means the recovery itself.
    std::string self = util::basename(argv[0]);
    bool sim = false, headless = false;
    bool cli = self == "twrp" || (self == "fmrre" && !(argc > 1 && util::startsWith(argv[1], "--")));
    std::string simRoot = "sim_root", script, outDir = "shots";
    std::vector<std::string> cliArgs;
    int sw = 540, sh = 1170;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (cli) { cliArgs.push_back(a); continue; }
        if (a == "fmrre" || a == "twrp") { cli = true; continue; }
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--sim") sim = true;
        else if (a == "--sim-root") simRoot = next();
        else if (a == "--headless") headless = true;
        else if (a == "--script") script = next();
        else if (a == "--out") outDir = next();
        else if (a == "--size") sscanf(next().c_str(), "%dx%d", &sw, &sh);
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        // Android's init may pass recovery arguments (e.g. --update_package); ignore unknown ones.
    }
    signal(SIGPIPE, SIG_IGN);

    if (cli) log::setConsole(false);
    if (sim) {
        char real[4096];
        util::mkdirs(simRoot);
        if (realpath(simRoot.c_str(), real)) simRoot = real;
        sim::ensureRoot(simRoot);
        log::init(simRoot + (cli ? "/tmp/fmrre-cli.log" : "/tmp/recovery.log"));
    } else if (cli) {
        log::init("/tmp/fmrre-cli.log");
    } else {
        util::mkdirs("/tmp");
        log::init("/tmp/recovery.log");
        setenv("PATH", "/system/bin:/sbin:/bin:/vendor/bin:/system/xbin", 1);
    }
    LOGI("FMRRE starting (%s, %s build, cpu %s)", sim ? "simulator" : "device", device::buildAbi(), device::machine().c_str());
    if (!device::abiAdvice().empty()) LOGW("%s", device::abiAdvice().c_str());
    device::init(sim, simRoot);
    Partitions::get().load();
    Partitions::get().mountAllStorage();
    Partitions::get().refresh();

    // Settings live next to backups (like TWRP's .twrps) so they survive reboots.
    std::string cfgDir = Partitions::get().appDir();
    if (!util::mkdirs(cfgDir)) cfgDir = device::path("/tmp");
    // Renamed from TWRP v2: take over its settings and flash history once.
    std::string legacy = Partitions::get().legacyDir();
    if (!util::exists(cfgDir + "/.fmrre.conf") && util::exists(legacy + "/.twrpv2.conf")) util::copyFile(legacy + "/.twrpv2.conf", cfgDir + "/.fmrre.conf");
    if (!util::exists(cfgDir + "/history.txt") && util::exists(legacy + "/history.txt")) util::copyFile(legacy + "/history.txt", cfgDir + "/history.txt");
    Settings::get().load(cfgDir + "/.fmrre.conf");
    if (cli) {
        int rc = ors::cliMain(cliArgs);
        Task::get().join();
        return rc;
    }

    std::unique_ptr<Display> display;
    std::unique_ptr<EvdevInput> input;
    InputQueue queue;
    if (sim) {
#ifdef TW_HAVE_X11
        if (!headless && getenv("DISPLAY")) {
            display = createX11Display(sw, sh);
            if (!display->init()) display.reset();
        }
#endif
        if (!display) {
            display = createHeadlessDisplay(sw, sh);
            display->init();
            headless = true;
        }
    } else {
        display = openHardwareDisplay();
        if (!display) {
            LOGE("No usable display (DRM and fbdev failed)");
            return 1;
        }
        input = std::make_unique<EvdevInput>();
        input->open(display->width(), display->height());
        int b = Settings::get().integer("brightness", -1);
        if (b > 0) device::setBrightness(std::max(1, device::brightnessMax() * b / 100));
        else if (device::brightness() == 0) device::setBrightness(device::brightnessMax() * 6 / 10);
    }
    LOGI("display: %s %dx%d", display->name(), display->width(), display->height());

    App app(display.get(), input.get(), &queue);
    app.idleHook = orsIdle;
    util::mkdirs(util::dirname(ors::uiPidFile()));
    util::writeFile(ors::uiPidFile(), std::to_string(getpid()) + "\n");
    ors::Script pending;
    if (ors::takePending(pending)) {
        LOGI("ors: %zu command(s) from %s", pending.lines.size(), pending.source.c_str());
        if (pending.lines.size() == 1 && pending.lines[0] == "fastboot") app.push(makeFastbootScreen(true));  // reboot fastboot
        else if (pending.lines.size() == 1 && pending.lines[0] == "autofix --prompt")  // Android gave up booting
            app.push(makeAutoRepairScreen(Settings::get().flag("autofix_auto"), true));
        else app.push(makeOrsScreen(pending));
    } else if (Settings::get().flag("autofix_auto") && autofix::lastBootFailed()) {
        app.push(makeAutoRepairScreen(true, false));  // "Repair automatically after a failed boot"
    }
    if (headless) {
        if (script.empty()) {
            fprintf(stderr, "headless mode needs --script\n");
            return 1;
        }
        int rc = runScript(app, script, outDir);
        mtp::stop();
        unlink(ors::uiPidFile().c_str());
        return rc;
    }
    app.run();
    Task::get().join();
    mtp::stop();
    unlink(ors::uiPidFile().c_str());
    return 0;
}
