// TWRP v2 - a modern Android custom recovery.
//
// On a device this binary is started by init from the recovery ramdisk
// (service recovery /system/bin/recovery). It draws directly to DRM/fbdev,
// reads /dev/input, and performs flashing, backups and wipes itself.
//
// Desktop development:
//   twrpv2 --sim                 X11 window (if built with X11) on a sandboxed fake device
//   twrpv2 --sim --headless --script demo.txt --out shots/

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
#include "sys/ors.h"
#include "sys/partitions.h"
#include "ui/app.h"
#include "ui/screens/screens.h"

using namespace tw;

static void usage() {
    printf("TWRP v2 recovery\n"
           "  --sim               run against a sandboxed fake device (desktop)\n"
           "  --sim-root DIR      sandbox directory (default ./sim_root)\n"
           "  --size WxH          simulator resolution (default 540x1170)\n"
           "  --headless          no window; use with --script\n"
           "  --script FILE       automation: tap X Y | swipe X1 Y1 X2 Y2 MS | wait MS | key NAME | text STR | shot FILE\n"
           "  --out DIR           where 'shot' files are written\n"
           "  twrp <command>      command line mode (also when run as 'twrp'): twrp help\n");
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
    bool sim = false, headless = false, cli = util::basename(argv[0]) == "twrp";
    std::string simRoot = "sim_root", script, outDir = "shots";
    std::vector<std::string> cliArgs;
    int sw = 540, sh = 1170;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (cli) { cliArgs.push_back(a); continue; }
        if (a == "twrp") { cli = true; continue; }
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
        log::init(simRoot + (cli ? "/tmp/twrp-cli.log" : "/tmp/recovery.log"));
    } else if (cli) {
        log::init("/tmp/twrp-cli.log");
    } else {
        util::mkdirs("/tmp");
        log::init("/tmp/recovery.log");
        setenv("PATH", "/system/bin:/sbin:/bin:/vendor/bin:/system/xbin", 1);
    }
    LOGI("TWRP v2 starting (%s)", sim ? "simulator" : "device");
    device::init(sim, simRoot);
    Partitions::get().load();
    Partitions::get().mountAllStorage();
    Partitions::get().refresh();

    // Settings live next to backups (like TWRP's .twrps) so they survive reboots.
    std::string cfgDir = Partitions::get().storagePath() + "/TWRP";
    if (!util::mkdirs(cfgDir)) cfgDir = device::path("/tmp");
    Settings::get().load(cfgDir + "/.twrpv2.conf");
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
        app.push(makeOrsScreen(pending));
    }
    if (headless) {
        if (script.empty()) {
            fprintf(stderr, "headless mode needs --script\n");
            return 1;
        }
        int rc = runScript(app, script, outDir);
        unlink(ors::uiPidFile().c_str());
        return rc;
    }
    app.run();
    Task::get().join();
    unlink(ors::uiPidFile().c_str());
    return 0;
}
