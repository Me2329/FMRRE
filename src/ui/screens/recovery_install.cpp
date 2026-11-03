// "Install TWRP v2 permanently" - for when it was started with `fastboot boot` (or on a phone
// whose bootloader can't do that), plus restoring the stock recovery saved on the way.
#include "ui/screens/screens.h"

#include "core/util.h"
#include "platform/device.h"
#include "sys/bootimg.h"
#include "sys/imagesrc.h"
#include "sys/partitions.h"
#include "sys/pathcheck.h"

namespace tw {

namespace {

class InstallRecoveryScreen : public Screen {
public:
    std::string title() const override { return "Install TWRP v2"; }
    std::string subtitle() const override { return "Make the recovery permanent"; }
    void onShow(App&) override { reload(); }
    void reload() {
        target_ = bootimg::recoveryTarget("");
        stock_ = bootimg::imagesIn(bootimg::stockDir());
    }

    void pickAndInstall(App& app) {
        app.push(makeFilesScreen(Partitions::get().storagePath() + "/Download", {".img"}, [](App& a, const std::string& p) {
            a.pop();
            bootimg::RootTarget t = bootimg::recoveryTarget(p);
            if (t.dev.empty()) { a.toast(t.why, Icon::Warning, a.ui.th.danger); return; }
            std::string msg = util::basename(p) + " goes into " + t.name + device::info().slot + ". The current " + t.name +
                              " is saved to TWRP/stock first, so this can be undone.";
            if (t.name == "boot") msg += "\n\nThis phone has no recovery partition: only continue if the image was built from THIS phone's boot.img.";
            a.confirm("Install TWRP v2?", msg, "Swipe to install", [&a, p]() {
                a.runTask("Installing TWRP v2", Icon::Download, [p](Task& tk) { return bootimg::installRecovery(p, tk); });
            }, t.name == "boot", Icon::Download);
        }, "TWRP v2 image", [](App& a) { a.pop(); a.push(makeManualFlashScreen()); }));
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("ri_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;

        float whyH = font::wrap(Face::Regular, ui.dp(12.5f), target_.why, w - ui.dp(40)).size() * font::lineHeight(Face::Regular, ui.dp(12.5f));
        Rect hero(x, y, w, ui.dp(100) + whyH);
        ui.card(hero, ui.dp(24));
        c.radialGlowRR(hero.r() - ui.dp(30), hero.y + ui.dp(20), hero.w * 0.7f, withAlpha(th.accent, 0.22f), hero, ui.dp(24));
        c.fillCircle(x + ui.dp(40), y + ui.dp(44), ui.dp(24), th.accentSoft);
        icons::drawCentered(c, Icon::Download, x + ui.dp(40), y + ui.dp(44), ui.dp(24), th.accent);
        font::drawIn(c, Face::Bold, ui.dp(18), Rect(x + ui.dp(76), y + ui.dp(22), w - ui.dp(90), ui.dp(26)),
                     "Target: " + target_.name + device::info().slot, th.text);
        font::drawIn(c, Face::Regular, ui.dp(12.5f), Rect(x + ui.dp(76), y + ui.dp(48), w - ui.dp(90), ui.dp(20)),
                     target_.dev.empty() ? "not found" : pathcheck::pretty(target_.dev) + "  \xE2\x80\xA2  " +
                     util::humanSize(Partitions::blockSize(target_.dev)), th.textDim);
        ui.textBlock(x + ui.dp(20), y + ui.dp(84), w - ui.dp(40), target_.why, ui.dp(12.5f), target_.name == "boot" ? th.warn : th.textDim);
        y += hero.h + ui.dp(14);

        if (ui.button(ui.id("ri_pick"), Rect(x, y, w, ui.dp(54)), "Choose TWRP v2 image", Icon::Folder)) pickAndInstall(app);
        y += ui.dp(66);
        ui.textBlock(x + ui.dp(4), y, w - ui.dp(8),
                     "Copy the image you started TWRP v2 with to the phone first, e.g.  adb push twrpv2-recovery.img /sdcard/Download/ "
                     "(or /tmp/ if storage is encrypted).",
                     ui.dp(12.5f), th.textFaint);
        y += ui.dp(52);

        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "From a computer");
        y += ui.dp(30);
        struct H { Icon icon; const char* t; const char* s; };
        H how[] = {{Icon::Bolt, "Fastboot (Pixel, Xiaomi, OnePlus, Motorola...)", "scripts/install.sh   (test boot, nothing written)\nscripts/install.sh --permanent"},
                   {Icon::Phone, "Samsung - Download mode", "scripts/install.sh   (Heimdall on Linux/macOS)\nWindows: install.ps1 -Samsung, then Odin AP slot"}};
        for (auto& hh : how) {
            Rect r(x, y, w, ui.dp(92));
            ui.card(r, ui.dp(18));
            icons::drawCentered(c, hh.icon, x + ui.dp(28), y + ui.dp(28), ui.dp(20), th.accent);
            font::drawIn(c, Face::SemiBold, ui.dp(13.5f), Rect(x + ui.dp(52), y + ui.dp(16), w - ui.dp(64), ui.dp(22)), hh.t, th.text);
            float ly = y + ui.dp(42);
            for (auto& l : util::split(hh.s, '\n')) {
                font::drawIn(c, Face::Mono, ui.dp(11.5f), Rect(x + ui.dp(52), ly, w - ui.dp(64), ui.dp(18)), l, th.textDim);
                ly += ui.dp(20);
            }
            y += r.h + ui.dp(10);
        }
        y += ui.dp(8);

        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Stock backups (TWRP/stock)");
        y += ui.dp(30);
        if (stock_.empty()) {
            ui.label(Rect(x, y, w, ui.dp(22)), "None yet - one is made automatically before installing.", ui.dp(13), th.textDim);
            y += ui.dp(34);
        } else {
            Rect sc(x, y, w, ui.dp(66) * stock_.size() + ui.dp(8));
            ui.card(sc);
            for (size_t i = 0; i < stock_.size(); i++) {
                std::string f = stock_[i];
                if (!(ui.listItem(ui.id("ri_stock", (int)i), Rect(x + ui.dp(4), y + ui.dp(4) + i * ui.dp(66), w - ui.dp(8), ui.dp(62)),
                                  Icon::Restore, util::basename(f), util::humanSize(util::fileSize(f)) + "  \xE2\x80\xA2  tap to restore", "",
                                  th.warn) & Clicked))
                    continue;
                bootimg::RootTarget t = bootimg::recoveryTarget(f);
                app.confirm("Restore " + util::basename(f) + "?", "Flashes it back to " + t.name + device::info().slot +
                            " (removes TWRP v2 from there).", "Swipe to restore", [&app, f, t]() {
                    app.runTask("Restoring stock " + t.name, Icon::Restore, [f, t](Task& tk) {
                        img::FlashOptions o;
                        o.verify = true;
                        return img::flash(f, "", t.dev, o, tk);
                    });
                }, true, Icon::Restore);
            }
            y += sc.h + ui.dp(12);
        }
        ui.endScroll(y + ui.dp(8));
    }

private:
    bootimg::RootTarget target_;
    std::vector<std::string> stock_;
};

}  // namespace

std::unique_ptr<Screen> makeInstallRecoveryScreen() { return std::make_unique<InstallRecoveryScreen>(); }

}  // namespace tw
