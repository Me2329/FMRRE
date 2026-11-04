#include "ui/screens/screens.h"

#include <unistd.h>

#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/avb.h"
#include "sys/bootimg.h"
#include "sys/imagesrc.h"
#include "sys/installer.h"
#include "sys/partitions.h"
#include "sys/pathcheck.h"
#include "sys/payload.h"
#include "sys/sideload.h"
#include "sys/super.h"

namespace tw {

std::vector<FlashTarget> flashTargets() {
    std::vector<FlashTarget> v;
    auto have = [&](const std::string& dev) {
        for (auto& t : v)
            if (t.dev == dev) return true;
        return false;
    };
    lp::Metadata m;
    std::string err;
    bool dyn = lp::read(lp::superDevice(), lp::currentSlotIndex(), m, err);
    for (auto& p : Partitions::get().all()) {
        if (!p.present || p.removable || p.mountPoint == "/data" || p.mountPoint == "/metadata") continue;
        FlashTarget t;
        t.label = p.displayName;
        t.dev = p.blockDev;
        t.size = Partitions::blockSize(p.blockDev);
        std::string base = util::basename(p.blockDev);
        if (dyn && m.find(base)) {
            t.logical = true;
            t.lpName = base;
            t.size = m.find(base)->bytes();
        }
        v.push_back(t);
    }
    if (dyn) {
        const std::string slot = device::info().slot;
        for (auto& p : m.parts) {
            bool otherSlot = !slot.empty() && (util::endsWith(p.name, "_a") || util::endsWith(p.name, "_b")) && !util::endsWith(p.name, slot);
            if (otherSlot) continue;
            std::string dev = lp::mappedPath(p.name);
            if (dev.empty() || have(dev)) continue;
            v.push_back({p.name, dev, p.name, p.bytes(), true});
        }
    }
    return v;
}

int guessTarget(const std::vector<FlashTarget>& ts, const img::Info& info) {
    auto findName = [&](const std::string& n) {
        for (size_t i = 0; i < ts.size(); i++) {
            std::string b = util::basename(ts[i].dev);
            std::string slot = device::info().slot;
            if (b == n || b == n + slot || (ts[i].logical && ts[i].lpName == n + slot)) return (int)i;
        }
        return -1;
    };
    std::string name = util::toLower(util::basename(info.path) + " " + info.zipEntry);
    if (info.looksLikeGsi) { int i = findName("system"); if (i >= 0) return i; }
    switch (info.kind) {
        case img::Kind::Vbmeta: return name.find("vbmeta_system") != std::string::npos ? findName("vbmeta_system") : findName("vbmeta");
        case img::Kind::VendorBoot: return findName("vendor_boot");
        case img::Kind::Dtbo: return findName("dtbo");
        case img::Kind::Super: return findName("super");
        case img::Kind::Boot:
            if (name.find("init_boot") != std::string::npos) return findName("init_boot");
            if (name.find("magisk_patched") != std::string::npos || name.find("kernelsu") != std::string::npos ||
                name.find("apatch") != std::string::npos)
                return findName(bootimg::rootTarget().name);
            if (name.find("recovery") != std::string::npos || name.find("twrp") != std::string::npos ||
                name.find("fmrre") != std::string::npos) {
                int r = findName("recovery");
                if (r >= 0) return r;
            }
            return findName("boot");
        default: break;
    }
    int best = -1;
    size_t bestLen = 0;
    for (size_t i = 0; i < ts.size(); i++) {
        std::string b = util::basename(ts[i].dev);
        const std::string slot = device::info().slot;
        if (!slot.empty() && util::endsWith(b, slot)) b = b.substr(0, b.size() - slot.size());
        if (b.size() > 2 && name.find(b) != std::string::npos && b.size() > bestLen) { best = (int)i; bestLen = b.size(); }
    }
    return best;
}

namespace {

// The zip queue survives leaving the screen, like TWRP's "Add more zips".
std::vector<std::string> g_queue;

}  // namespace

void queueZip(const std::string& path) { g_queue.push_back(path); }

namespace {

void pickImage(App& app) {
    app.push(makeFilesScreen("", img::imageExtensions(),
                             [](App& a, const std::string& p) { a.pop(); a.push(makeFlashImageScreen(p)); }, "Select image",
                             [](App& a) { a.pop(); a.push(makeManualFlashScreen()); }));
}

class InstallScreen : public Screen {
public:
    std::string title() const override { return "Install"; }
    std::string subtitle() const override {
        switch (mode_) {
            case 1: return "Raw, sparse, .xz, .gz, .zst or zipped images";
            case 2: return "Generic System Images, done properly";
            case 3: return "ADB sideload from a computer";
            default: return g_queue.empty() ? "Flash zips or images" : std::to_string(g_queue.size()) + " zip(s) queued";
        }
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        float pad = ui.dp(16);
        Rect bottom = area.cutBottom(ui.dp(88));
        ui.beginScroll(ui.id("inst_scroll"), area);
        float y = area.y;
        float x = area.x + pad, w = area.w - pad * 2;

        // Mode tabs (icons are dropped when the screen is too narrow for them)
        struct Tab { const char* label; Icon icon; };
        Tab tabs[] = {{"Zip", Icon::Zip}, {"Image", Icon::Image}, {"GSI", Icon::Phone}, {"Sideload", Icon::Usb}};
        float gap = ui.dp(6), tabW = (w - gap * 3) / 4;
        for (int i = 0; i < 4; i++) {
            Icon ic = ui.chipWidth(tabs[i].label, tabs[i].icon) <= tabW ? tabs[i].icon : Icon::None;
            if (ui.chip(ui.id("tab", i), Rect(x + i * (tabW + gap), y, tabW, ui.dp(42)), tabs[i].label, mode_ == i, ic)) mode_ = i;
        }
        y += ui.dp(58);
        Settings& s = Settings::get();

        if (mode_ == 3) {
            Rect info(x, y, w, ui.dp(190));
            ui.card(info);
            icons::drawCentered(*ui.c, Icon::Usb, info.x + ui.dp(40), info.y + ui.dp(44), ui.dp(32), th.info);
            ui.label(Rect(info.x + ui.dp(74), info.y + ui.dp(20), info.w - ui.dp(90), ui.dp(26)), "ADB sideload", ui.dp(16), th.text,
                     Face::SemiBold);
            ui.textBlock(info.x + ui.dp(74), info.y + ui.dp(48), info.w - ui.dp(94),
                         "Push a zip over USB without copying it to storage first. Run adb sideload <file.zip> on your computer "
                         "and the package is received and flashed here.",
                         ui.dp(13.5f), th.textDim);
            Rect code(info.x + ui.dp(16), info.b() - ui.dp(44), info.w - ui.dp(32), ui.dp(32));
            ui.c->fillRoundRect(code, ui.dp(10), th.surface2);
            font::drawIn(*ui.c, Face::Mono, ui.dp(13), code.inset(ui.dp(12), ui.dp(8)), "adb sideload rom.zip", th.text);
            y += ui.dp(206);
            Rect optc(x, y, w, ui.dp(68) * 2 + ui.dp(8));
            ui.card(optc);
            bool verify = s.flag("zip_verify", true), reboot = s.flag("zip_reboot", false);
            if (ui.toggleRow(ui.id("sl_v"), Rect(x + ui.dp(4), y + ui.dp(4), w - ui.dp(8), ui.dp(64)), Icon::Shield,
                             "Verify zip integrity", "CRC-check every entry before flashing", verify))
                s.setFlag("zip_verify", verify), s.save();
            if (ui.toggleRow(ui.id("sl_r"), Rect(x + ui.dp(4), y + ui.dp(72), w - ui.dp(8), ui.dp(64)), Icon::Reboot,
                             "Reboot when done", "Automatically boot to system on success", reboot))
                s.setFlag("zip_reboot", reboot), s.save();
            y += optc.h + ui.dp(16);
            ui.endScroll(y);
            Rect sb = bottom.inset(pad, ui.dp(14));
            if (ui.slideToConfirm(ui.id("sl_go"), sb, "Swipe to start sideload", th.info)) {
                installer::InstallOptions o;
                o.verifyZip = s.flag("zip_verify", true);
                bool rb = s.flag("zip_reboot", false);
                app.runTask("ADB sideload", Icon::Usb, [o, rb](Task& t) {
                    t.setRange(0, 0.7f);
                    std::string pkg = sideload::receive(t);
                    if (pkg.empty()) return false;
                    t.setRange(0.7f, 0.3f);
                    bool ok = installer::installZip(pkg, o, t);
                    unlink(pkg.c_str());
                    if (ok && rb) {
                        t.print("Rebooting in 3 seconds...");
                        sleep(3);
                        device::reboot("");
                    }
                    return ok;
                });
            }
            return;
        }

        if (mode_ == 2) {
            Rect info(x, y, w, ui.dp(236));
            ui.card(info);
            Canvas& c = *ui.c;
            c.fillCircle(info.x + ui.dp(40), info.y + ui.dp(42), ui.dp(24), th.accentSoft);
            icons::drawCentered(c, Icon::Phone, info.x + ui.dp(40), info.y + ui.dp(42), ui.dp(24), th.accent);
            ui.label(Rect(info.x + ui.dp(76), info.y + ui.dp(22), info.w - ui.dp(90), ui.dp(26)), "GSI wizard", ui.dp(17), th.text, Face::SemiBold);
            ui.label(Rect(info.x + ui.dp(76), info.y + ui.dp(46), info.w - ui.dp(90), ui.dp(20)), "Flash any Treble GSI in one swipe", ui.dp(13),
                     th.textDim);
            const char* steps[] = {"Reads .img, .img.xz, .img.gz, .zst and zipped GSIs",
                                   "Checks it fits; grows system in super if needed",
                                   "Remaps system read-write (why plain dd fails)",
                                   "Disables AVB verification in vbmeta",
                                   "Optional factory reset, verified write"};
            float sy = info.y + ui.dp(84);
            for (auto st : steps) {
                icons::drawCentered(c, Icon::Check, info.x + ui.dp(30), sy + ui.dp(9), ui.dp(14), th.ok);
                ui.label(Rect(info.x + ui.dp(48), sy, info.w - ui.dp(60), ui.dp(20)), st, ui.dp(13), th.text);
                sy += ui.dp(28);
            }
            y += info.h + ui.dp(16);
            if (ui.button(ui.id("gsi_go"), Rect(x, y, w, ui.dp(54)), "Choose GSI", Icon::Folder)) {
                app.push(makeFilesScreen("", {".img", ".img.xz", ".img.gz", ".img.zst", ".img.lz4", ".xz", ".gz", ".zip"},
                                         [](App& a, const std::string& p) { a.pop(); a.push(makeGsiScreen(p)); }, "Select GSI",
                                         [](App& a) { a.pop(); a.push(makeManualFlashScreen()); }));
            }
            y += ui.dp(64);
            if (ui.button(ui.id("gsi_super"), Rect(x, y, w, ui.dp(50)), "Super partitions", Icon::Storage, BtnStyle::Tonal))
                app.push(makeSuperScreen());
            y += ui.dp(66);
            ui.endScroll(y);
            ui.label(bottom.inset(pad, ui.dp(20)), "Unlocked bootloader required", ui.dp(13), th.textFaint, Face::Regular, AlignCenter);
            return;
        }

        if (mode_ == 1) {
            Rect info(x, y, w, ui.dp(132));
            ui.card(info);
            icons::drawCentered(*ui.c, Icon::Image, info.x + ui.dp(36), info.y + ui.dp(36), ui.dp(30), th.accent);
            ui.textBlock(info.x + ui.dp(64), info.y + ui.dp(20), info.w - ui.dp(84),
                         "boot, init_boot, vendor_boot, dtbo, vbmeta, recovery, system, super... Raw and sparse images, also "
                         "compressed (.xz .gz .zst .lz4) or inside a .zip - streamed, no temp files.",
                         ui.dp(13.5f), th.textDim);
            y += ui.dp(148);
            if (ui.button(ui.id("pick_img"), Rect(x, y, w, ui.dp(54)), "Choose image", Icon::Folder)) pickImage(app);
            y += ui.dp(64);
            if (ui.button(ui.id("cant_find"), Rect(x, y, w, ui.dp(50)), "Can't find your file?", Icon::Search, BtnStyle::Tonal))
                app.push(makeManualFlashScreen());
            y += ui.dp(58);
            ui.label(Rect(x, y, w, ui.dp(36)), "Type the path, see why it's hidden, get a ready dd command", ui.dp(12.5f), th.textFaint,
                     Face::Regular, AlignCenter);
            y += ui.dp(40);
            ui.endScroll(y);
            ui.label(bottom.inset(pad, ui.dp(20)), "Images are flashed from the next screen", ui.dp(13), th.textFaint, Face::Regular,
                     AlignCenter);
            return;
        }

        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Queue");
        y += ui.dp(30);
        if (g_queue.empty()) {
            Rect e(x, y, w, ui.dp(150));
            ui.card(e);
            ui.c->fillCircle(e.cx(), e.y + ui.dp(48), ui.dp(28), th.accentSoft);
            icons::drawCentered(*ui.c, Icon::Zip, e.cx(), e.y + ui.dp(48), ui.dp(28), th.accent);
            ui.label(Rect(e.x, e.y + ui.dp(86), e.w, ui.dp(24)), "No zips queued", ui.dp(16), th.text, Face::SemiBold, AlignCenter);
            ui.label(Rect(e.x, e.y + ui.dp(110), e.w, ui.dp(20)), "Add ROMs, kernels, Magisk, GApps...", ui.dp(13), th.textDim,
                     Face::Regular, AlignCenter);
            y += ui.dp(162);
        } else {
            Rect card(x, y, w, ui.dp(66) * g_queue.size() + ui.dp(8));
            ui.card(card);
            float iy = y + ui.dp(4);
            for (size_t i = 0; i < g_queue.size(); i++) {
                Rect row(x + ui.dp(4), iy, w - ui.dp(8), ui.dp(62));
                std::string sz = util::humanSize(util::fileSize(g_queue[i]));
                ui.listItem(ui.id("q", (int)i), row.cutLeft(row.w - ui.dp(52)), Icon::Zip, std::to_string(i + 1) + ". " + util::basename(g_queue[i]),
                            sz + "  \xE2\x80\xA2  " + pathcheck::pretty(util::dirname(g_queue[i])), "", th.accent, false);
                if (ui.iconButton(ui.id("qdel", (int)i), Rect(row.r() - ui.dp(48), row.cy() - ui.dp(20), ui.dp(40), ui.dp(40)), Icon::Close,
                                  th.textDim)) {
                    g_queue.erase(g_queue.begin() + i);
                    break;
                }
                iy += ui.dp(66);
            }
            y += card.h + ui.dp(12);
        }
        float bw = (w - ui.dp(10)) / 2;
        if (ui.button(ui.id("add"), Rect(x, y, g_queue.empty() ? w : bw, ui.dp(52)), g_queue.empty() ? "Select zip" : "Add more", Icon::Plus,
                      g_queue.empty() ? BtnStyle::Filled : BtnStyle::Tonal)) {
            app.push(makeFilesScreen("", {".zip"},
                                     [](App& a, const std::string& p) {
                                         a.pop();
                                         if (payload::isOtaZip(p)) {  // A/B ROM / OTA: own screen (slot, super, verity)
                                             a.push(makePayloadScreen(p));
                                             return;
                                         }
                                         if (g_queue.size() < 10) g_queue.push_back(p);
                                         a.toast("Added " + util::basename(p), Icon::Zip);
                                     },
                                     "Select zip",
                                     [](App& a) {
                                         a.push(makeTextInputScreen("Zip path", "/sdcard/Download/rom.zip", "/sdcard/Download/",
                                                                    [](App& b, const std::string& p) {
                                                                        auto r = pathcheck::check(p);
                                                                        if (!r.usable()) {
                                                                            b.toast(r.findings.empty() ? "File not found" : r.findings.back().text,
                                                                                    Icon::Warning, b.ui.th.danger);
                                                                            return;
                                                                        }
                                                                        b.pop();
                                                                        if (payload::isOtaZip(r.resolved)) {
                                                                            b.push(makePayloadScreen(r.resolved));
                                                                            return;
                                                                        }
                                                                        g_queue.push_back(r.resolved);
                                                                        b.toast("Added " + util::basename(r.resolved), Icon::Zip);
                                                                    }));
                                     }));
        }
        if (!g_queue.empty() && ui.button(ui.id("clear"), Rect(x + bw + ui.dp(10), y, bw, ui.dp(52)), "Clear", Icon::Trash, BtnStyle::Outline))
            g_queue.clear();
        y += ui.dp(68);

        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Options");
        y += ui.dp(30);
        Rect opt(x, y, w, ui.dp(68) * 3 + ui.dp(8));
        ui.card(opt);
        float oy = y + ui.dp(4);
        bool verify = s.flag("zip_verify", true), wipeAfter = s.flag("zip_wipe_cache", false), reboot = s.flag("zip_reboot", false);
        if (ui.toggleRow(ui.id("o1"), Rect(x + ui.dp(4), oy, w - ui.dp(8), ui.dp(64)), Icon::Shield, "Verify zip integrity",
                         "CRC-check every entry before flashing", verify)) s.setFlag("zip_verify", verify), s.save();
        oy += ui.dp(68);
        if (ui.toggleRow(ui.id("o2"), Rect(x + ui.dp(4), oy, w - ui.dp(8), ui.dp(64)), Icon::Wipe, "Wipe cache & dalvik after",
                         "Recommended after ROM updates", wipeAfter)) s.setFlag("zip_wipe_cache", wipeAfter), s.save();
        oy += ui.dp(68);
        if (ui.toggleRow(ui.id("o3"), Rect(x + ui.dp(4), oy, w - ui.dp(8), ui.dp(64)), Icon::Reboot, "Reboot when done",
                         "Automatically boot to system on success", reboot)) s.setFlag("zip_reboot", reboot), s.save();
        y += opt.h + ui.dp(16);
        ui.endScroll(y);

        Rect sb = bottom.inset(pad, ui.dp(14));
        if (g_queue.empty()) {
            ui.c->fillRoundRect(sb, sb.h / 2, th.surface2);
            ui.label(sb, "Queue a zip to flash", ui.dp(15), th.textFaint, Face::SemiBold, AlignCenter);
        } else if (ui.slideToConfirm(ui.id("flash"), sb, g_queue.size() > 1 ? "Swipe to flash all" : "Swipe to flash", th.accent)) {
            auto queue = g_queue;
            g_queue.clear();
            installer::InstallOptions o;
            o.verifyZip = s.flag("zip_verify", true);
            o.wipeCacheAfter = s.flag("zip_wipe_cache", false);
            bool rb = s.flag("zip_reboot", false);
            app.runTask(queue.size() > 1 ? "Flashing " + std::to_string(queue.size()) + " zips" : "Flashing " + util::basename(queue[0]),
                        Icon::Install, [queue, o, rb](Task& t) {
                            bool ok = true;
                            for (size_t i = 0; i < queue.size() && ok; i++) {
                                t.setRange(float(i) / queue.size(), 1.0f / queue.size());
                                t.setSubProgress(0);
                                ok = installer::installZip(queue[i], o, t);
                            }
                            if (ok && rb) {
                                t.print("Rebooting in 3 seconds...");
                                sleep(3);
                                device::reboot("");
                            }
                            return ok;
                        });
        }
    }

private:
    int mode_ = 0;
};

class FlashImageScreen : public Screen {
public:
    FlashImageScreen(std::string img, std::string entry) : img_(std::move(img)), entry_(std::move(entry)) { reload(); }
    void reload() {
        info_ = img::inspect(img_, entry_);
        entry_ = info_.zipEntry;
        targets_ = flashTargets();
        sel_ = guessTarget(targets_, info_);
    }
    void onShow(App&) override { targets_ = flashTargets(); }
    std::string title() const override { return "Flash image"; }
    std::string subtitle() const override { return util::basename(img_); }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        Rect bottom = area.cutBottom(ui.dp(88));
        ui.beginScroll(ui.id("fi_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;

        Rect info(x, y, w, ui.dp(92));
        ui.card(info);
        icons::drawCentered(c, info_.ok ? Icon::Chip : Icon::Warning, info.x + ui.dp(36), info.cy(), ui.dp(28), info_.ok ? th.accent : th.danger);
        ui.label(Rect(info.x + ui.dp(66), info.y + ui.dp(16), info.w - ui.dp(80), ui.dp(24)), util::basename(img_), ui.dp(15.5f), th.text,
                 Face::SemiBold);
        ui.textBlock(info.x + ui.dp(66), info.y + ui.dp(44), info.w - ui.dp(80),
                     info_.ok ? info_.describe() : info_.error, ui.dp(12.5f), info_.ok ? th.textDim : th.danger);
        y += info.h + ui.dp(12);

        if (info_.zipImages.size() > 1) {
            ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Image inside the zip");
            y += ui.dp(30);
            float cx = x;
            for (size_t i = 0; i < info_.zipImages.size() && i < 12; i++) {
                std::string n = util::basename(info_.zipImages[i]);
                float cw = std::min(w, ui.chipWidth(n));
                if (cx + cw > x + w) { cx = x; y += ui.dp(44); }
                if (ui.chip(ui.id("ze", (int)i), Rect(cx, y, cw, ui.dp(36)), n, info_.zipImages[i] == entry_)) {
                    entry_ = info_.zipImages[i];
                    reload();
                }
                cx += cw + ui.dp(8);
            }
            y += ui.dp(52);
        }

        if (info_.looksLikeGsi) {
            Rect g(x, y, w, ui.dp(74));
            c.fillRoundRect(g, ui.dp(18), th.accentSoft);
            if (ui.listItem(ui.id("fi_gsi"), g.inset(ui.dp(2)), Icon::Phone, "This looks like a GSI",
                            "Use the GSI wizard: resizes system, disables verity", "", th.accent) & Clicked)
                app.push(makeGsiScreen(img_));
            y += g.h + ui.dp(12);
        }

        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Target partition");
        y += ui.dp(30);
        Rect card(x, y, w, ui.dp(62) * targets_.size() + ui.dp(8));
        ui.card(card);
        float iy = y + ui.dp(4);
        for (size_t i = 0; i < targets_.size(); i++) {
            auto& t = targets_[i];
            bool on = sel_ == (int)i;
            std::string sub = pathcheck::pretty(t.dev) + (t.size ? "  \xE2\x80\xA2  " + util::humanSize(t.size) : "");
            std::string trail;
            if (info_.imageSize && t.size) trail = info_.imageSize <= t.size ? "fits" : (t.logical ? "resize" : "too big");
            if (ui.checkRow(ui.id("tp", (int)i), Rect(x + ui.dp(4), iy, w - ui.dp(8), ui.dp(58)), t.label + (t.logical ? "  (dynamic)" : ""),
                            sub, on, trail))
                sel_ = (int)i;
            iy += ui.dp(62);
        }
        y += card.h + ui.dp(16);

        // Options
        Settings& s = Settings::get();
        const FlashTarget* tp = sel_ >= 0 && sel_ < (int)targets_.size() ? &targets_[sel_] : nullptr;
        bool isVbmeta = tp && util::basename(tp->dev).find("vbmeta") == 0;
        bool compressed = info_.comp != img::Compression::None;
        int nOpt = 1 + (compressed ? 1 : 0) + (isVbmeta ? 1 : 0);
        Rect opt(x, y, w, ui.dp(68) * nOpt + ui.dp(8));
        ui.card(opt);
        float oy = y + ui.dp(4);
        bool verify = s.flag("img_verify", true);
        if (ui.toggleRow(ui.id("fo_v"), Rect(x + ui.dp(4), oy, w - ui.dp(8), ui.dp(64)), Icon::Shield, "Verify after flashing",
                         "Read back and compare SHA-256", verify)) s.setFlag("img_verify", verify), s.save();
        oy += ui.dp(68);
        if (compressed) {
            if (ui.toggleRow(ui.id("fo_p"), Rect(x + ui.dp(4), oy, w - ui.dp(8), ui.dp(64)), Icon::Search, "Check archive first",
                             "Decompress once before writing - slower, safest", precheck_)) {}
            oy += ui.dp(68);
        }
        if (isVbmeta) {
            ui.toggleRow(ui.id("fo_avb"), Rect(x + ui.dp(4), oy, w - ui.dp(8), ui.dp(64)), Icon::Bolt, "Disable verity & verification",
                         "Same as fastboot --disable-verity --disable-verification", disableAvb_);
            oy += ui.dp(68);
        }
        y += opt.h + ui.dp(12);
        if (ui.button(ui.id("fi_dd"), Rect(x, y, w, ui.dp(48)), "Show the dd command instead", Icon::Terminal, BtnStyle::Outline))
            app.push(makeManualFlashScreen(img_));
        y += ui.dp(64);
        ui.endScroll(y);

        Rect sb = bottom.inset(pad, ui.dp(14));
        bool tooBig = tp && info_.imageSize && tp->size && info_.imageSize > tp->size;
        lp::Metadata m;
        std::string err;
        uint64_t grow = 0;
        bool canGrow = false;
        if (tooBig && tp->logical && lp::read(lp::superDevice(), lp::currentSlotIndex(), m, err)) {
            grow = info_.imageSize - tp->size;
            canGrow = m.maxGrowBytes(tp->lpName) >= grow;
        }
        if (!info_.ok || !tp) {
            c.fillRoundRect(sb, sb.h / 2, th.surface2);
            ui.label(sb, !info_.ok ? "Can't read this image" : "Select a partition", ui.dp(15), th.textFaint, Face::SemiBold, AlignCenter);
        } else if (tooBig && !canGrow) {
            c.fillRoundRect(sb, sb.h / 2, withAlpha(th.danger, 0.15f));
            ui.label(sb, tp->logical ? "Not enough space in super - see GSI wizard" : "Image is bigger than the partition", ui.dp(14), th.danger,
                     Face::SemiBold, AlignCenter);
        } else if (ui.slideToConfirm(ui.id("fi_go"), sb, tooBig ? "Swipe to resize & flash " + tp->label : "Swipe to flash " + tp->label,
                                     th.warn)) {
            std::string img = img_, entry = entry_, dev = tp->dev, lpName = tp->lpName;
            uint64_t need = info_.imageSize;
            bool resize = tooBig;
            img::FlashOptions o;
            o.verify = s.flag("img_verify", true);
            o.precheck = precheck_;
            bool avbOff = isVbmeta && disableAvb_;
            app.runTask("Flashing " + tp->label, Icon::Bolt, [=](Task& t) {
                for (auto& p : Partitions::get().all())
                    if (p.blockDev == dev && p.mounted) Partitions::get().unmount(p);
                img::FlashOptions fo = o;
                if (resize) {
                    t.setRange(0, 0.05f);
                    if (!lp::resizePartition(lpName, need, t)) return false;
                    t.setRange(0.05f, 0.95f);
                }
                if (!lpName.empty()) {
                    lp::Metadata mm;
                    std::string e;
                    if (lp::read(lp::superDevice(), lp::currentSlotIndex(), mm, e) && mm.find(lpName)) fo.limit = mm.find(lpName)->bytes();
                }
                if (!img::flash(img, entry, dev, fo, t)) return false;
                if (avbOff) {
                    std::string e;
                    avb::Info ai = avb::read(dev);
                    if (ai.ok && avb::setFlags(dev, ai.flags | avb::kDisableVerity | avb::kDisableVerification, e))
                        t.print("vbmeta flags set to %u (verity and verification disabled)", ai.flags | 3);
                    else
                        t.print("Could not patch vbmeta flags: %s", ai.ok ? e.c_str() : ai.error.c_str());
                }
                return true;
            });
        }
    }

private:
    std::string img_, entry_;
    img::Info info_;
    std::vector<FlashTarget> targets_;
    int sel_ = -1;
    bool precheck_ = false, disableAvb_ = true;
};

}  // namespace

std::unique_ptr<Screen> makeInstallScreen() { return std::make_unique<InstallScreen>(); }
std::unique_ptr<Screen> makeFlashImageScreen(const std::string& img, const std::string& entry) {
    return std::make_unique<FlashImageScreen>(img, entry);
}

}  // namespace tw
