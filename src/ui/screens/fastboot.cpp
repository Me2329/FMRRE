// Fastboot mode: TWRP v2's own fastbootd. Started from Advanced, by `fastboot reboot fastboot` /
// `adb reboot fastboot` (the bootloader message says --fastboot), or `twrp fastboot`.
#include "ui/screens/screens.h"

#include <cmath>

#include "core/settings.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/fastboot.h"

namespace tw {

namespace {

class FastbootScreen : public Screen {
public:
    explicit FastbootScreen(bool autostart) : autostart_(autostart) {}
    std::string title() const override { return "Fastboot mode"; }
    std::string subtitle() const override {
        if (!mine()) return "fastbootd built into TWRP v2";
        auto& s = fastboot::state();
        std::lock_guard<std::mutex> l(s.mu);
        return s.transport.empty() ? "Starting..." : s.transport;
    }
    bool onBack(App& app) override {
        if (mine() && Task::get().running()) {
            Task::get().requestCancel();
            app.toast("Leaving fastboot mode", Icon::Usb);
            return true;
        }
        return false;
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        if (autostart_ && !started_) start(app);
        bool running = mine() && Task::get().running();
        if (running) ui.keepAnimating();
        fastboot::State snap;
        {
            auto& s = fastboot::state();
            std::lock_guard<std::mutex> l(s.mu);
            snap.transport = s.transport;
            snap.peer = s.peer;
            snap.connected = s.connected;
            snap.command = s.command;
            snap.result = s.result;
            snap.done = s.done;
            snap.total = s.total;
            snap.history = s.history;
            snap.commands = s.commands;
        }
        float pad = ui.dp(16);
        Rect bottom = area.cutBottom(ui.dp(88));
        ui.beginScroll(ui.id("fb_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;

        // ---- status
        Rect sc(x, y, w, ui.dp(150));
        ui.card(sc);
        Color col = !running ? th.textDim : snap.connected ? th.ok : th.accent;
        float cx = x + ui.dp(58), cy = sc.y + ui.dp(62);
        if (running) {
            float pulse = 0.5f + 0.5f * std::sin((ui.now % 1600) / 1600.0f * 6.2832f);
            c.radialGlow(cx, cy, ui.dp(58) + pulse * ui.dp(10), withAlpha(col, 0.20f));
        }
        c.fillCircle(cx, cy, ui.dp(34), withAlpha(col, 0.16f));
        icons::drawCentered(c, Icon::Usb, cx, cy, ui.dp(34), col);
        std::string head = !running ? (started_ ? "Fastboot mode stopped" : "Not running")
                           : snap.connected ? "Connected" : "Waiting for a computer";
        ui.label(Rect(x + ui.dp(110), sc.y + ui.dp(28), w - ui.dp(124), ui.dp(26)), head, ui.dp(18), th.text, Face::SemiBold);
        std::string sub = !running ? "Flash partitions from a computer with fastboot"
                          : snap.connected ? snap.peer + "  \xE2\x80\xA2  " + std::to_string(snap.commands) + " commands"
                          : snap.transport == "USB" ? "Plug in the USB cable, then run: fastboot devices" : "fastboot -s tcp:<ip> devices";
        ui.textBlock(x + ui.dp(110), sc.y + ui.dp(58), w - ui.dp(124), sub, ui.dp(12.5f), th.textDim);
        if (running && snap.total) {
            Rect pb(x + ui.dp(16), sc.y + ui.dp(116), w - ui.dp(32), ui.dp(8));
            ui.progressBar(pb, float(double(snap.done) / double(snap.total)));
            ui.label(Rect(x + ui.dp(16), sc.y + ui.dp(126), w - ui.dp(32), ui.dp(18)),
                     util::humanSize(snap.done) + " / " + util::humanSize(snap.total), ui.dp(11), th.textFaint, Face::Regular, AlignRight);
        } else if (!snap.command.empty()) {
            ui.label(Rect(x + ui.dp(16), sc.y + ui.dp(114), w - ui.dp(32), ui.dp(20)), "> " + snap.command, ui.dp(12), th.textFaint, Face::Mono);
        }
        y += sc.h + ui.dp(16);

        // ---- recent commands
        if (!snap.history.empty()) {
            ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Commands");
            y += ui.dp(30);
            size_t n = std::min<size_t>(snap.history.size(), 8);
            Rect hc(x, y, w, ui.dp(26) * n + ui.dp(16));
            ui.card(hc);
            float hy = y + ui.dp(8);
            for (size_t i = snap.history.size() - n; i < snap.history.size(); i++) {
                bool last = i + 1 == snap.history.size();
                Color hcol = last && util::startsWith(snap.result, "FAIL") ? th.danger : last ? th.text : th.textDim;
                font::drawIn(c, Face::Mono, ui.dp(12), Rect(x + ui.dp(16), hy, w - ui.dp(32), ui.dp(22)), snap.history[i], hcol);
                hy += ui.dp(26);
            }
            y += hc.h + ui.dp(8);
            if (util::startsWith(snap.result, "FAIL")) {
                float h = ui.textBlock(x + ui.dp(4), y, w - ui.dp(8), snap.result.substr(4), ui.dp(12.5f), th.danger);
                y += h + ui.dp(8);
            }
            y += ui.dp(8);
        }

        // ---- how to
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "On the computer");
        y += ui.dp(30);
        const char* cmds[] = {"fastboot devices", "fastboot flash system system.img", "fastboot flash boot boot.img",
                              "fastboot getvar all", "fastboot oem twrp vbmeta disable", "fastboot reboot recovery   (back here)"};
        Rect cc(x, y, w, ui.dp(26) * 6 + ui.dp(16));
        ui.card(cc, -1, th.surface2, false);
        float ty = y + ui.dp(8);
        for (auto s : cmds) {
            font::drawIn(c, Face::Mono, ui.dp(12), Rect(x + ui.dp(16), ty, w - ui.dp(32), ui.dp(22)), s, th.textDim);
            ty += ui.dp(26);
        }
        y += cc.h + ui.dp(12);
        float h = ui.textBlock(x + ui.dp(4), y, w - ui.dp(8),
                               "Logical partitions (system, vendor, product...) are resized in super automatically, like Android's "
                               "fastbootd: this is where GSIs flash with plain fastboot. `fastboot oem twrp <command>` runs any TWRP v2 "
                               "command (twrp help).",
                               ui.dp(12), th.textFaint);
        y += h + ui.dp(12);
        if (!device::isSim() && !running) {
            Rect tc(x, y, w, ui.dp(76));
            ui.card(tc);
            if (ui.toggleRow(ui.id("fb_tcp"), tc.inset(ui.dp(4)), Icon::Swap, "Over the network (TCP 5554)",
                             "For emulators and USB networking: fastboot -s tcp:<ip>", tcp_))
                Settings::get().setFlag("fastboot_tcp", tcp_), Settings::get().save();
            y += tc.h + ui.dp(12);
        }
        ui.endScroll(y);

        Rect b = bottom.inset(pad, ui.dp(16));
        if (running) {
            if (ui.button(ui.id("fb_stop"), b, "Exit fastboot mode", Icon::Close, BtnStyle::Tonal)) Task::get().requestCancel();
        } else if (ui.button(ui.id("fb_start"), b, started_ ? "Start again" : "Start fastboot mode", Icon::Usb)) {
            start(app);
        }
    }

private:
    bool mine() const { return started_ && Task::get().title() == kTitle; }

    void start(App& app) {
        started_ = true;
        if (Task::get().running()) {
            app.toast("Another operation is still running", Icon::Warning, app.ui.th.warn);
            started_ = false;
            autostart_ = false;
            return;
        }
        fastboot::Options o;
        tcp_ = Settings::get().flag("fastboot_tcp", false);
        if (device::isSim()) {
            o.usb = false;
            o.tcpPort = 5554;
            o.tcpBind = "127.0.0.1";
        } else if (tcp_) {
            o.usb = false;
            o.tcpPort = 5554;
        }
        Task::get().start(kTitle, [o](Task& t) {
            std::string target;
            return fastboot::run(o, t, target);
        });
    }

    static constexpr const char* kTitle = "Fastboot mode";
    bool autostart_ = false, started_ = false, tcp_ = false;
};

}  // namespace

std::unique_ptr<Screen> makeFastbootScreen(bool autostart) { return std::make_unique<FastbootScreen>(autostart); }

}  // namespace tw
