// Root center: install / remove Magisk, KernelSU or APatch safely and fix module bootloops.
#include "ui/screens/screens.h"

#include "core/util.h"
#include "platform/device.h"
#include "sys/bootimg.h"
#include "sys/imagesrc.h"
#include "sys/installer.h"
#include "sys/partitions.h"
#include "sys/pathcheck.h"

namespace tw {

namespace {

class RootScreen : public Screen {
public:
    std::string title() const override { return "Root"; }
    std::string subtitle() const override { return bootimg::rootName(status_.kind); }
    void onShow(App&) override { reload(); }
    void reload() {
        target_ = bootimg::rootTarget();
        status_ = bootimg::detectRoot();
        mods_ = bootimg::modules();
        backups_ = bootimg::stockBackups();
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("root_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;

        // status hero
        Rect hero(x, y, w, ui.dp(150));
        bool rooted = status_.kind != bootimg::RootKind::None;
        Color tint = rooted ? th.ok : th.accent;
        ui.card(hero, ui.dp(24));
        c.radialGlowRR(hero.r() - ui.dp(30), hero.y + ui.dp(20), hero.w * 0.7f, withAlpha(tint, 0.25f), hero, ui.dp(24));
        c.fillCircle(x + ui.dp(40), y + ui.dp(44), ui.dp(24), withAlpha(tint, 0.18f));
        icons::drawCentered(c, rooted ? Icon::Shield : Icon::Key, x + ui.dp(40), y + ui.dp(44), ui.dp(24), tint);
        font::drawIn(c, Face::Bold, ui.dp(20), Rect(x + ui.dp(76), y + ui.dp(20), w - ui.dp(90), ui.dp(28)), bootimg::rootName(status_.kind), th.text);
        font::drawIn(c, Face::Regular, ui.dp(12.5f), Rect(x + ui.dp(76), y + ui.dp(48), w - ui.dp(90), ui.dp(20)),
                     !status_.dataReadable ? "Data is encrypted - root files can't be checked" : status_.detail.empty() ? "No root manager data in /data/adb" : status_.detail,
                     th.textDim);
        std::string tl = "Ramdisk target: " + target_.name + device::info().slot;
        if (target_.info.ok)
            tl += "  \xE2\x80\xA2  header v" + std::to_string(target_.info.headerVersion) +
                  (target_.info.patchLevel.empty() ? "" : "  \xE2\x80\xA2  patch " + target_.info.patchLevel);
        font::drawIn(c, Face::SemiBold, ui.dp(12.5f), Rect(x + ui.dp(20), y + ui.dp(88), w - ui.dp(40), ui.dp(20)), tl, th.text);
        ui.textBlock(x + ui.dp(20), y + ui.dp(110), w - ui.dp(40), target_.why, ui.dp(11.5f), th.textFaint);
        y += hero.h + ui.dp(18);

        // install
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Install / remove");
        y += ui.dp(30);
        struct A { Icon icon; std::string t; std::string s; int code; Color col; };
        std::vector<A> acts = {
            {Icon::Install, "Install root from zip or APK", "Magisk APK, KernelSU / APatch zip - stock " + target_.name + " is backed up first", 1, th.accent},
            {Icon::Bolt, "Flash a patched image", "magisk_patched-*.img / kernelsu / apatch -> " + target_.name + device::info().slot, 2, th.warn},
            {Icon::Backup, "Back up stock " + target_.name, backups_.empty() ? "No stock backup yet" : std::to_string(backups_.size()) + " backup(s) in FMRRE/root", 3, th.info},
            {Icon::Restore, "Unroot: restore stock " + target_.name, "Flash a saved stock image back", 4, th.danger},
        };
        Rect ac(x, y, w, ui.dp(72) * acts.size() + ui.dp(8));
        ui.card(ac);
        for (size_t i = 0; i < acts.size(); i++) {
            if (!(ui.listItem(ui.id("ra", (int)i), Rect(x + ui.dp(4), y + ui.dp(4) + i * ui.dp(72), w - ui.dp(8), ui.dp(68)), acts[i].icon, acts[i].t,
                              acts[i].s, "", acts[i].col) & Clicked))
                continue;
            act(app, acts[i].code);
        }
        y += ac.h + ui.dp(18);

        // modules
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Modules (" + std::to_string(mods_.size()) + ")");
        y += ui.dp(30);
        if (mods_.empty()) {
            ui.label(Rect(x, y, w, ui.dp(22)), status_.dataReadable ? "No modules installed" : "Can't read /data/adb (encrypted)", ui.dp(13), th.textDim);
            y += ui.dp(36);
        } else {
            if (ui.button(ui.id("mod_safe"), Rect(x, y, w, ui.dp(50)), "Bootloop? Disable all modules", Icon::Shield, BtnStyle::Tonal)) {
                int n = bootimg::disableAllModules();
                app.toast(std::to_string(n) + " module(s) disabled - reboot to test", Icon::Shield, th.ok);
                reload();
            }
            y += ui.dp(62);
            Rect mc(x, y, w, ui.dp(84) * mods_.size() + ui.dp(8));
            ui.card(mc);
            for (size_t i = 0; i < mods_.size(); i++) {
                auto& m = mods_[i];
                Rect r(x + ui.dp(4), y + ui.dp(4) + i * ui.dp(84), w - ui.dp(8), ui.dp(80));
                bool on = !m.disabled;
                std::string sub = (m.version.empty() ? "" : m.version + "  \xE2\x80\xA2  ") + (m.author.empty() ? m.id : m.author) +
                                  (m.removing ? "  \xE2\x80\xA2  removed on next boot" : "");
                if (ui.toggleRow(ui.id("mod", (int)i), Rect(r.x, r.y, r.w - ui.dp(48), r.h), m.removing ? Icon::Trash : Icon::Zip, m.name, sub, on)) {
                    bootimg::setModuleDisabled(m, !on);
                    reload();
                    break;
                }
                if (ui.iconButton(ui.id("mod_rm", (int)i), Rect(r.r() - ui.dp(44), r.cy() - ui.dp(20), ui.dp(40), ui.dp(40)),
                                  m.removing ? Icon::Refresh : Icon::Trash, m.removing ? th.ok : th.danger)) {
                    bootimg::setModuleRemove(m, !m.removing);
                    app.toast(m.removing ? "Removal cancelled" : m.name + " will be removed on next boot", Icon::Trash);
                    reload();
                    break;
                }
            }
            y += mc.h + ui.dp(12);
        }
        ui.textBlock(x, y, w, "FMRRE doesn't ship its own su: it installs and manages the proven root solutions above. "
                              "A stock copy of " + target_.name + " is always kept so you can unroot or recover from a bad patch.",
                     ui.dp(12), th.textFaint);
        y += ui.dp(60);
        ui.endScroll(y);
    }

    void act(App& app, int code) {
        bootimg::RootTarget tgt = target_;
        switch (code) {
            case 1:
                app.push(makeFilesScreen("", {".zip", ".apk"}, [tgt](App& a, const std::string& p) {
                    a.pop();
                    installer::ZipInfo zi = installer::inspectZip(p);
                    if (!zi.hasUpdateBinary) {
                        a.toast("No installer inside - patch the image in the root app and use 'Flash a patched image'", Icon::Warning, a.ui.th.warn);
                        return;
                    }
                    a.runTask("Installing " + util::basename(p), Icon::Install, [p, tgt](Task& t) {
                        t.setRange(0, 0.2f);
                        if (!bootimg::backupStock(tgt, t)) { t.print("Not continuing without a stock backup."); return false; }
                        t.setRange(0.2f, 0.8f);
                        return installer::installZip(p, installer::optionsFromSettings(), t);
                    });
                }, "Root installer (zip / APK)"));
                break;
            case 2:
                app.push(makeFilesScreen("", img::imageExtensions(), [tgt](App& a, const std::string& p) {
                    a.pop();
                    a.runTask("Flashing " + util::basename(p) + " to " + tgt.name, Icon::Bolt, [p, tgt](Task& t) {
                        t.setRange(0, 0.3f);
                        if (!bootimg::backupStock(tgt, t)) { t.print("Not continuing without a stock backup."); return false; }
                        bootimg::Info pi = bootimg::read(p);
                        if (!pi.ok) { t.print("%s is not a boot image: %s", util::basename(p).c_str(), pi.error.c_str()); return false; }
                        if (tgt.info.ok && pi.headerVersion != tgt.info.headerVersion)
                            t.print("Warning: header v%u, but %s is v%u - is this for the right partition?", pi.headerVersion, tgt.name.c_str(),
                                    tgt.info.headerVersion);
                        t.setRange(0.3f, 0.7f);
                        img::FlashOptions o;
                        o.verify = true;
                        return img::flash(p, "", tgt.dev, o, t);
                    });
                }, "Patched image"));
                break;
            case 3:
                app.runTask("Backing up stock " + tgt.name, Icon::Backup, [tgt](Task& t) { return bootimg::backupStock(tgt, t); });
                break;
            case 4:
                util::mkdirs(bootimg::backupDir());
                app.push(makeFilesScreen(bootimg::backupDir(), {".img"}, [tgt](App& a, const std::string& p) {
                    a.pop();
                    a.confirm("Restore " + util::basename(p) + "?", "Flashes it to " + tgt.name + device::info().slot +
                              ". Root is removed; your apps and data stay.", "Swipe to unroot", [&a, p, tgt]() {
                        a.runTask("Restoring stock " + tgt.name, Icon::Restore, [p, tgt](Task& t) {
                            img::FlashOptions o;
                            o.verify = true;
                            return img::flash(p, "", tgt.dev, o, t);
                        });
                    }, true, Icon::Restore);
                }, "Stock backup"));
                break;
        }
    }

private:
    bootimg::RootTarget target_;
    bootimg::RootStatus status_;
    std::vector<bootimg::Module> mods_;
    std::vector<std::string> backups_;
};

}  // namespace

std::unique_ptr<Screen> makeRootScreen() { return std::make_unique<RootScreen>(); }

}  // namespace tw
