#include "ui/screens/screens.h"

#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"

namespace tw {

namespace {

class RebootScreen : public Screen {
public:
    std::string title() const override { return "Reboot"; }
    std::string subtitle() const override { return "Choose where to go next"; }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("rb_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        const auto& info = device::info();
        if (info.ab) {
            Rect sc(x, y, w, ui.dp(84));
            ui.card(sc, ui.dp(22));
            std::string cur(1, (char)toupper(info.slot.back()));
            if (ui.listItem(ui.id("slot"), sc.inset(ui.dp(4)), Icon::Swap, "Active slot: " + cur, "Tap to switch to the other slot", "",
                            th.warn) & Clicked) {
                std::string other = cur == "A" ? "B" : "A";
                app.confirm("Switch to slot " + other + "?", "Only do this if slot " + other + " has a working system.",
                            "Swipe to switch", [&app]() {
                                std::string msg;
                                bool ok = device::switchSlot(msg);
                                app.toast(msg, ok ? Icon::Swap : Icon::Warning, ok ? app.ui.th.ok : app.ui.th.danger);
                            });
            }
            y += sc.h + ui.dp(16);
        }
        struct T { Icon icon; const char* title; const char* sub; const char* target; Color tint; };
        T targets[] = {
            {Icon::Phone, "System", "Boot Android", "", th.ok},
            {Icon::Refresh, "Recovery", "Restart TWRP v2", "recovery", th.accent},
            {Icon::Bolt, "Bootloader", "Fastboot mode", "bootloader", th.warn},
            {Icon::Usb, "Fastbootd", "Userspace fastboot", "fastboot", th.info},
            {Icon::Download, "Download", "Samsung Odin mode", "download", th.accent2},
            {Icon::Chip, "EDL", "Qualcomm emergency mode", "edl", th.danger},
            {Icon::Power, "Power off", "Shut the device down", "poweroff", th.textDim},
        };
        float gap = ui.dp(12);
        float tw = (w - gap) / 2, tH = ui.dp(120);
        for (int i = 0; i < 7; i++) {
            Rect r(x + (i % 2) * (tw + gap), y + (i / 2) * (tH + gap), i == 6 ? w : tw, tH);
            if (ui.tile(ui.id("rt", i), r, targets[i].icon, targets[i].title, targets[i].sub, targets[i].tint)) {
                std::string tgt = targets[i].target;
                std::string title = std::string(i == 6 ? "Power off" : "Reboot to ") + (i == 6 ? "?" : std::string(targets[i].title) + "?");
                std::string msg = Task::get().running() ? "An operation is still running!" : "Unmounts partitions and restarts now.";
                app.confirm(title, msg, i == 6 ? "Swipe to power off" : "Swipe to reboot", [tgt, &app]() {
                    app.toast("See you on the other side", Icon::Reboot);
                    Partitions::get().unmountAll();
                    device::reboot(tgt);
                }, i >= 5, targets[i].icon);
            }
        }
        y += 4 * (tH + gap);
        ui.endScroll(y);
    }
};

}  // namespace

std::unique_ptr<Screen> makeRebootScreen() { return std::make_unique<RebootScreen>(); }

}  // namespace tw
