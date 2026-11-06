#include "ui/screens/screens.h"

#include <cmath>
#include <deque>

#include "core/log.h"
#include "core/util.h"
#include "core/settings.h"
#include "platform/device.h"
#include "sys/partitions.h"

namespace tw {

namespace {

class AdvancedScreen : public Screen {
public:
    std::string title() const override { return "Advanced"; }
    std::string subtitle() const override { return "Tools for power users"; }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("adv_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        struct Item { Icon icon; const char* title; const char* sub; Color tint; int code; };
        const auto& info = device::info();
        std::vector<std::pair<const char*, std::vector<Item>>> groups = {
            {"Partitions & boot",
             {{Icon::Usb, "Fastboot mode (fastbootd)", "fastboot flash from a computer, super resized for you", th.info, 27},
              {Icon::Download, "Install FMRRE permanently", "Keep it after fastboot boot - stock is saved first", th.accent, 26},
              {Icon::Key, "Root", "Magisk / KernelSU / APatch, stock backup, modules", th.ok, 25},
              {Icon::Storage, "Super partitions", "Resize / delete dynamic partitions (system, product...)", th.accent, 20},
              {Icon::Phone, "GSI wizard", "Flash a Generic System Image the right way", th.ok, 21},
              {Icon::Bolt, "Verified boot (vbmeta)", "Disable / enable dm-verity and AVB", th.warn, 22},
              {Icon::Wrench, "File system tools", "Repair, resize, change file system", th.info, 23},
              {Icon::Search, "Find & flash a file", "Manual path, diagnostics, dd command", th.accent2, 24}}},
            {"Diagnostics",
             {{Icon::Wrench, "Auto repair", "Doesn't start? Finds the problem and fixes it", th.danger, 29},
              {Icon::Shield, "Bootloop rescue", "Why the last boot failed, with one-tap fixes", th.warn, 28},
              {Icon::Timer, "Flash history", "Every flash, wipe, restore and repair, with the time", th.accent2, 30},
              {Icon::Phone, "Device info", "Model, Android, slot, encryption, CPU", th.accent, 12},
              {Icon::Chart, "System monitor", "Live CPU, memory, battery & thermals", th.accent, 1},
              {Icon::Storage, "Partition info", "Block devices, sizes and flags", th.info, 2},
              {Icon::Touch, "Input & touch test", "Check keys and touch calibration", th.ok, 3},
              {Icon::Image, "Display test", "Dead pixels, burn-in, colour uniformity", th.accent2, 13}}},
            {"Tools",
             {{Icon::Folder, "File manager", "Browse, copy, move, chmod", th.warn, 4},
              {Icon::Terminal, "Terminal", "Run shell commands as root", rgb(0x94A3B8), 5},
              {Icon::Download, "Copy log to storage", "Save recovery.log for bug reports", th.accent2, 6},
              {Icon::Camera, "Take screenshot", "Saved to Pictures/Screenshots", th.accent, 7},
              {Icon::Shield, "Fix storage contexts", "restorecon on internal storage", th.info, 8},
              {Icon::Usb, "Restart ADB", "Restart the adbd service", th.ok, 9},
              {Icon::Refresh, "Reload partitions", "Re-read fstab and block devices", th.textDim, 10}}},
        };
        if (info.ab) groups[2].second.insert(groups[2].second.begin(), {Icon::Swap, "Switch boot slot",
                                             "Make the other A/B slot active", th.danger, 11});
        int k = 0;
        for (auto& g : groups) {
            ui.sectionHeader(Rect(x, y, w, ui.dp(24)), g.first);
            y += ui.dp(30);
            Rect card(x, y, w, ui.dp(70) * g.second.size() + ui.dp(8));
            ui.card(card);
            float iy = y + ui.dp(4);
            for (auto& it : g.second) {
                if (ui.listItem(ui.id("adv", k++), Rect(x + ui.dp(4), iy, w - ui.dp(8), ui.dp(66)), it.icon, it.title, it.sub, "",
                                it.tint) & Clicked)
                    act(app, it.code);
                iy += ui.dp(70);
            }
            y += card.h + ui.dp(18);
        }
        ui.endScroll(y);
    }

    void act(App& app, int code) {
        Theme& th = app.ui.th;
        switch (code) {
            case 1: app.push(makeMonitorScreen()); break;
            case 20: app.push(makeSuperScreen()); break;
            case 21: app.push(makeGsiScreen()); break;
            case 22: app.push(makeVbmetaScreen()); break;
            case 23: app.push(makeFsToolsScreen("")); break;
            case 24: app.push(makeManualFlashScreen()); break;
            case 25: app.push(makeRootScreen()); break;
            case 26: app.push(makeInstallRecoveryScreen()); break;
            case 27: app.push(makeFastbootScreen(true)); break;
            case 28: app.push(makeRescueScreen()); break;
            case 30: app.push(makeHistoryScreen()); break;
            case 29: app.push(makeAutoRepairScreen()); break;
            case 2: app.push(makePartInfoScreen()); break;
            case 3: app.push(makeInputTestScreen()); break;
            case 12: app.push(makeDeviceInfoScreen()); break;
            case 13: app.push(makeDisplayTestScreen()); break;
            case 4: app.push(makeFilesScreen()); break;
            case 5: app.push(makeTerminalScreen()); break;
            case 6: {
                std::string dst = Partitions::get().storagePath() + "/recovery_" + util::dateStamp() + ".log";
                if (util::copyFile(log::filePath(), dst)) app.toast("Saved " + util::basename(dst), Icon::Check, th.ok);
                else app.toast("Couldn't copy the log", Icon::Warning, th.danger);
                break;
            }
            case 7: app.screenshot(); break;
            case 8:
                app.runTask("Fixing contexts", Icon::Shield, [](Task& t) {
                    std::string p = Partitions::get().storagePath();
                    t.print("restorecon -RF %s", p.c_str());
                    int rc = util::run("restorecon -RF " + util::shellQuote(p), [&](const std::string& l) { t.print("%s", l.c_str()); });
                    return rc == 0;
                });
                break;
            case 9:
                device::setProp("ctl.restart", "adbd");
                app.toast("adbd restarted", Icon::Usb);
                break;
            case 10:
                Partitions::get().load();
                app.toast("Partitions reloaded", Icon::Refresh);
                break;
            case 11: {
                const auto& info = device::info();
                std::string cur(1, (char)toupper(info.slot.back()));
                std::string other = cur == "A" ? "B" : "A";
                app.confirm("Switch to slot " + other + "?",
                            "Current slot is " + cur + ". The device will boot from slot " + other + " next time.",
                            "Swipe to switch", [&app]() {
                                std::string msg;
                                bool ok = device::switchSlot(msg);
                                app.toast(msg, ok ? Icon::Swap : Icon::Warning, ok ? app.ui.th.ok : app.ui.th.danger);
                            });
                break;
            }
        }
    }
};

class MonitorScreen : public Screen {
public:
    std::string title() const override { return "System monitor"; }
    std::string subtitle() const override { return "Uptime " + util::humanDuration(device::uptimeSec() * 1000); }

    void sample() {
        cpu_ = device::cpu();
        mem_ = device::memory();
        bat_ = device::battery();
        zones_ = device::thermals();
        hist_.push_back(cpu_.usage);
        if (hist_.size() > 60) hist_.pop_front();
        last_ = util::nowMs();
    }

    void draw(Ui& ui, App&, Rect area) override {
        if (util::nowMs() - last_ > 700) sample();
        ui.ambient();
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("mon_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;

        // Rings row
        float gap = ui.dp(10);
        float cw = (w - gap * 2) / 3;
        struct R { const char* label; float frac; std::string val; Color col; };
        float memFrac = mem_.total ? 1 - float(mem_.avail) / mem_.total : 0;
        R rings[3] = {{"CPU", cpu_.usage, std::to_string(int(cpu_.usage * 100)) + "%", th.accent},
                      {"RAM", memFrac, std::to_string(int(memFrac * 100)) + "%", th.info},
                      {"Battery", bat_.percent / 100.f, bat_.percent >= 0 ? std::to_string(bat_.percent) + "%" : "n/a", th.ok}};
        for (int i = 0; i < 3; i++) {
            Rect r(x + i * (cw + gap), y, cw, cw + ui.dp(24));
            ui.card(r, ui.dp(20));
            float rr = cw * 0.3f;
            float shown = ui.anim(ui.id("ring", i), rings[i].frac, 6);
            ui.ring(r.cx(), r.y + cw * 0.48f, rr, ui.dp(8), shown, rings[i].col);
            font::drawIn(c, Face::Bold, ui.dp(16), Rect(r.x, r.y + cw * 0.48f - ui.dp(12), r.w, ui.dp(24)), rings[i].val, th.text, AlignCenter);
            font::drawIn(c, Face::SemiBold, ui.dp(12), Rect(r.x, r.b() - ui.dp(34), r.w, ui.dp(20)), rings[i].label, th.textDim, AlignCenter);
        }
        y += cw + ui.dp(24) + ui.dp(14);

        // CPU history graph
        Rect g(x, y, w, ui.dp(170));
        ui.card(g, ui.dp(20));
        font::drawIn(c, Face::SemiBold, ui.dp(14), Rect(g.x + ui.dp(16), g.y + ui.dp(10), g.w, ui.dp(22)),
                     "CPU load  \xE2\x80\xA2  " + std::to_string(cpu_.cores) + " cores", th.text);
        Rect plot(g.x + ui.dp(16), g.y + ui.dp(42), g.w - ui.dp(32), g.h - ui.dp(58));
        for (int k = 0; k <= 4; k++) {
            float ly = plot.y + plot.h * k / 4;
            c.fillRect(Rect(plot.x, ly, plot.w, std::max(1.f, ui.dp(0.6f))), withAlpha(th.textFaint, 0.25f));
        }
        if (hist_.size() > 1) {
            float step = plot.w / 59;
            float x0 = plot.r() - step * (hist_.size() - 1);
            for (size_t k = 1; k < hist_.size(); k++) {
                float ax = x0 + step * (k - 1), bx = x0 + step * k;
                float ay = plot.b() - plot.h * hist_[k - 1], by = plot.b() - plot.h * hist_[k];
                // area fill
                c.fillRect(Rect(ax, std::min(ay, by), bx - ax + 1, plot.b() - std::min(ay, by)), withAlpha(th.accent, 0.10f));
                c.line(ax, ay, bx, by, ui.dp(2.2f), th.accent);
            }
        }
        y += g.h + ui.dp(14);

        // Per-core frequencies
        if (!cpu_.freqMhz.empty()) {
            int maxF = 1;
            for (int f : cpu_.freqMhz) maxF = std::max(maxF, f);
            Rect fc(x, y, w, ui.dp(44) + ui.dp(26) * cpu_.freqMhz.size());
            ui.card(fc, ui.dp(20));
            font::drawIn(c, Face::SemiBold, ui.dp(14), Rect(fc.x + ui.dp(16), fc.y + ui.dp(10), fc.w, ui.dp(22)), "Core frequencies", th.text);
            float fy = fc.y + ui.dp(42);
            for (size_t k = 0; k < cpu_.freqMhz.size(); k++) {
                font::drawIn(c, Face::Mono, ui.dp(11.5f), Rect(fc.x + ui.dp(16), fy, ui.dp(50), ui.dp(18)), "cpu" + std::to_string(k), th.textDim);
                ui.usageBar(Rect(fc.x + ui.dp(70), fy + ui.dp(6), fc.w - ui.dp(170), ui.dp(6)), float(cpu_.freqMhz[k]) / maxF, th.accent);
                font::drawIn(c, Face::Mono, ui.dp(11.5f), Rect(fc.r() - ui.dp(96), fy, ui.dp(80), ui.dp(18)),
                             std::to_string(cpu_.freqMhz[k]) + " MHz", th.text, AlignRight);
                fy += ui.dp(26);
            }
            y += fc.h + ui.dp(14);
        }

        // Battery details
        Rect bc(x, y, w, ui.dp(150));
        ui.card(bc, ui.dp(20));
        font::drawIn(c, Face::SemiBold, ui.dp(14), Rect(bc.x + ui.dp(16), bc.y + ui.dp(10), bc.w, ui.dp(22)), "Battery", th.text);
        char buf[64];
        std::vector<std::pair<std::string, std::string>> kv = {
            {"Status", bat_.status.empty() ? "unknown" : bat_.status},
            {"Health", bat_.health.empty() ? "unknown" : bat_.health},
        };
        snprintf(buf, sizeof(buf), "%d mV  \xE2\x80\xA2  %d mA", bat_.voltageMv, bat_.currentMa);
        kv.push_back({"Voltage / current", buf});
        if (bat_.tempC > -100) { snprintf(buf, sizeof(buf), "%.1f \xC2\xB0""C", bat_.tempC); kv.push_back({"Temperature", buf}); }
        float ky = bc.y + ui.dp(40);
        for (auto& [k, v] : kv) {
            font::drawIn(c, Face::Regular, ui.dp(13), Rect(bc.x + ui.dp(16), ky, bc.w / 2, ui.dp(22)), k, th.textDim);
            font::drawIn(c, Face::SemiBold, ui.dp(13), Rect(bc.x + bc.w / 2, ky, bc.w / 2 - ui.dp(16), ui.dp(22)), v, th.text, AlignRight);
            ky += ui.dp(26);
        }
        y += bc.h + ui.dp(14);

        // Memory
        Rect mc(x, y, w, ui.dp(96));
        ui.card(mc, ui.dp(20));
        font::drawIn(c, Face::SemiBold, ui.dp(14), Rect(mc.x + ui.dp(16), mc.y + ui.dp(10), mc.w, ui.dp(22)), "Memory", th.text);
        ui.usageBar(Rect(mc.x + ui.dp(16), mc.y + ui.dp(44), mc.w - ui.dp(32), ui.dp(8)), memFrac, th.info);
        font::drawIn(c, Face::Regular, ui.dp(12.5f), Rect(mc.x + ui.dp(16), mc.y + ui.dp(60), mc.w - ui.dp(32), ui.dp(20)),
                     util::humanSize(mem_.total - mem_.avail) + " used of " + util::humanSize(mem_.total), th.textDim);
        y += mc.h + ui.dp(14);

        // Thermal zones
        if (!zones_.empty()) {
            size_t n = std::min<size_t>(zones_.size(), 24);
            Rect tc(x, y, w, ui.dp(44) + ui.dp(26) * n);
            ui.card(tc, ui.dp(20));
            font::drawIn(c, Face::SemiBold, ui.dp(14), Rect(tc.x + ui.dp(16), tc.y + ui.dp(10), tc.w, ui.dp(22)), "Thermal zones", th.text);
            float ty = tc.y + ui.dp(42);
            for (size_t k = 0; k < n; k++) {
                auto& z = zones_[k];
                Color col = z.c > 70 ? th.danger : (z.c > 50 ? th.warn : th.ok);
                c.fillCircle(tc.x + ui.dp(22), ty + ui.dp(9), ui.dp(4), col);
                font::drawIn(c, Face::Regular, ui.dp(12.5f), Rect(tc.x + ui.dp(34), ty, tc.w * 0.6f, ui.dp(18)), z.type, th.text);
                snprintf(buf, sizeof(buf), "%.1f \xC2\xB0""C", z.c);
                font::drawIn(c, Face::SemiBold, ui.dp(12.5f), Rect(tc.r() - ui.dp(96), ty, ui.dp(80), ui.dp(18)), buf, col, AlignRight);
                ty += ui.dp(26);
            }
            y += tc.h + ui.dp(14);
        }
        ui.endScroll(y);
    }

private:
    device::CpuStat cpu_;
    device::Mem mem_;
    device::Battery bat_;
    std::vector<device::ThermalZone> zones_;
    std::deque<float> hist_;
    int64_t last_ = 0;
};

class PartInfoScreen : public Screen {
public:
    std::string title() const override { return "Partition info"; }
    std::string subtitle() const override { return std::to_string(Partitions::get().all().size()) + " partitions"; }
    void onShow(App&) override { Partitions::get().refresh(); }

    void draw(Ui& ui, App&, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("pi_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        for (auto& p : Partitions::get().all()) {
            Rect r(x, y, w, ui.dp(112));
            ui.card(r, ui.dp(18));
            Color tint = p.isImage() ? th.danger : (p.mounted ? th.ok : th.accent);
            icons::drawCentered(c, p.isImage() ? Icon::Chip : Icon::Mount, r.x + ui.dp(28), r.y + ui.dp(28), ui.dp(22), tint);
            font::drawIn(c, Face::SemiBold, ui.dp(15.5f), Rect(r.x + ui.dp(50), r.y + ui.dp(14), r.w - ui.dp(150), ui.dp(24)), p.displayName, th.text);
            ui.badge(r.r() - ui.dp(16) - (font::measure(Face::SemiBold, ui.dp(11.5f), p.fsType) + ui.dp(14)), r.y + ui.dp(27), p.fsType, tint);
            font::drawIn(c, Face::Mono, ui.dp(11.5f), Rect(r.x + ui.dp(16), r.y + ui.dp(48), r.w - ui.dp(32), ui.dp(18)), p.blockDev, th.textDim);
            std::string flags;
            if (p.backup) flags += "backup ";
            if (p.wipe) flags += "wipe ";
            if (p.flashImg) flags += "flash ";
            if (p.logical) flags += "logical ";
            if (p.storage) flags += "storage ";
            if (p.removable) flags += "removable ";
            font::drawIn(c, Face::Regular, ui.dp(12), Rect(r.x + ui.dp(16), r.y + ui.dp(70), r.w - ui.dp(32), ui.dp(18)),
                         p.mountPoint + "  \xE2\x80\xA2  " + (p.size ? util::humanSize(p.size) : "?") + "  \xE2\x80\xA2  " + flags, th.textFaint);
            if (!p.present) font::drawIn(c, Face::SemiBold, ui.dp(12), Rect(r.x + ui.dp(16), r.y + ui.dp(88), r.w, ui.dp(18)),
                                         "block device not found", th.danger);
            y += r.h + ui.dp(10);
        }
        ui.endScroll(y);
    }
};

class InputTestScreen : public Screen {
public:
    std::string title() const override { return "Input test"; }
    std::string subtitle() const override { return "Press keys, draw on the screen"; }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        for (auto& e : ui.events()) {
            if (e.type == EvType::KeyDown || e.type == EvType::KeyUp) {
                static const char* names[] = {"none", "VOL+", "VOL-", "POWER", "BACK", "HOME", "MENU", "ENTER", "BKSP",
                                              "LEFT", "RIGHT", "UP", "DOWN", "ESC"};
                char buf[80];
                snprintf(buf, sizeof(buf), "%s %s (code %d)", names[(int)e.key], e.type == EvType::KeyDown ? "down" : "up", e.rawCode);
                keys_.push_front(buf);
                if (keys_.size() > 6) keys_.pop_back();
            } else if (e.type == EvType::TouchDown || e.type == EvType::TouchMove) {
                trail_.push_back({(float)e.x, (float)e.y - c.oy(), ui.now});
                lastX_ = e.x;
                lastY_ = e.y;
            }
        }
        while (!trail_.empty() && ui.now - trail_.front().t > 2500) trail_.pop_front();
        if (!trail_.empty()) ui.keepAnimating();
        Rect pad = area.inset(ui.dp(16), ui.dp(4));
        Rect info = pad.cutTop(ui.dp(190));
        ui.card(info, ui.dp(20));
        char buf[64];
        snprintf(buf, sizeof(buf), "Touch: %d, %d   Screen: %d x %d", lastX_, lastY_, c.width(), c.height());
        font::drawIn(c, Face::Mono, ui.dp(13), Rect(info.x + ui.dp(16), info.y + ui.dp(12), info.w, ui.dp(20)), buf, th.text);
        float ky = info.y + ui.dp(40);
        if (keys_.empty()) font::drawIn(c, Face::Regular, ui.dp(13), Rect(info.x + ui.dp(16), ky, info.w, ui.dp(20)),
                                        "Press any hardware key...", th.textDim);
        for (auto& k : keys_) {
            font::drawIn(c, Face::Mono, ui.dp(13), Rect(info.x + ui.dp(16), ky, info.w, ui.dp(20)), k, th.accent);
            ky += ui.dp(22);
        }
        // Touch calibration: if taps land in the wrong place or axis (common on an odd panel), toggle these and the
        // transform updates live - the drawing pad below shows the effect at once.
        Rect cal = pad.cutTop(ui.dp(66));
        ui.card(cal, ui.dp(18));
        font::drawIn(c, Face::Regular, ui.dp(12), Rect(cal.x + ui.dp(16), cal.y + ui.dp(10), cal.w - ui.dp(32), ui.dp(16)),
                     "Touch calibration (if taps land in the wrong place)", th.textDim);
        Settings& st = Settings::get();
        struct Cal { const char* lbl; const char* key; };
        Cal cals[] = {{"Swap X/Y", "touch_swap"}, {"Flip X", "touch_flipx"}, {"Flip Y", "touch_flipy"}};
        float cgap = ui.dp(8), ccw = (cal.w - ui.dp(32) - 2 * cgap) / 3;
        for (int i = 0; i < 3; i++) {
            bool on = st.flag(cals[i].key);
            if (ui.chip(ui.id("cal", i), Rect(cal.x + ui.dp(16) + i * (ccw + cgap), cal.y + ui.dp(30), ccw, ui.dp(30)), cals[i].lbl, on)) {
                st.setFlag(cals[i].key, !on);
                st.save();
                app.applySettings();
                app.vibrate();
            }
        }
        pad.cutTop(ui.dp(10));
        ui.card(pad, ui.dp(20), th.surface, false);
        // grid
        for (float gx = pad.x; gx < pad.r(); gx += ui.dp(40)) c.fillRect(Rect(gx, pad.y, 1, pad.h), withAlpha(th.textFaint, 0.15f));
        for (float gy = pad.y; gy < pad.b(); gy += ui.dp(40)) c.fillRect(Rect(pad.x, gy, pad.w, 1), withAlpha(th.textFaint, 0.15f));
        for (size_t i = 1; i < trail_.size(); i++) {
            float a = 1 - (ui.now - trail_[i].t) / 2500.0f;
            if (trail_[i].t - trail_[i - 1].t < 120)
                c.line(trail_[i - 1].x, trail_[i - 1].y, trail_[i].x, trail_[i].y, ui.dp(5), withAlpha(th.accent, a));
        }
        if (ui.touchDown()) {
            float tx = ui.touchX(), ty = ui.touchY() - c.oy();
            c.fillRect(Rect(pad.x, ty, pad.w, 1), withAlpha(th.accent2, 0.6f));
            c.fillRect(Rect(tx, pad.y, 1, pad.h), withAlpha(th.accent2, 0.6f));
            c.strokeCircle(tx, ty, ui.dp(26), ui.dp(2), th.accent2);
        }
    }

private:
    struct P { float x, y; int64_t t; };
    std::deque<P> trail_;
    std::deque<std::string> keys_;
    int lastX_ = 0, lastY_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeAdvancedScreen() { return std::make_unique<AdvancedScreen>(); }
std::unique_ptr<Screen> makeMonitorScreen() { return std::make_unique<MonitorScreen>(); }
std::unique_ptr<Screen> makePartInfoScreen() { return std::make_unique<PartInfoScreen>(); }
std::unique_ptr<Screen> makeInputTestScreen() { return std::make_unique<InputTestScreen>(); }

}  // namespace tw
