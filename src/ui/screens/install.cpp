#include "ui/screens/screens.h"

#include <unistd.h>

#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/installer.h"
#include "sys/partitions.h"

namespace tw {

namespace {

// The zip queue survives leaving the screen, like TWRP's "Add more zips".
std::vector<std::string> g_queue;

class InstallScreen : public Screen {
public:
    std::string title() const override { return "Install"; }
    std::string subtitle() const override {
        return g_queue.empty() ? "Flash zips or images" : std::to_string(g_queue.size()) + " zip(s) queued";
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        float pad = ui.dp(16);
        // Bottom: swipe to flash
        Rect bottom = area.cutBottom(ui.dp(88));
        ui.beginScroll(ui.id("inst_scroll"), area);
        float y = area.y;
        float x = area.x + pad, w = area.w - pad * 2;

        // Mode tabs
        float tabW = (w - ui.dp(8)) / 2;
        if (ui.chip(ui.id("tab_zip"), Rect(x, y, tabW, ui.dp(42)), "Zip", mode_ == 0, Icon::Zip)) mode_ = 0;
        if (ui.chip(ui.id("tab_img"), Rect(x + tabW + ui.dp(8), y, tabW, ui.dp(42)), "Image", mode_ == 1, Icon::Image)) mode_ = 1;
        y += ui.dp(58);

        if (mode_ == 1) {
            Rect info(x, y, w, ui.dp(120));
            ui.card(info);
            icons::drawCentered(*ui.c, Icon::Image, info.x + ui.dp(36), info.y + ui.dp(36), ui.dp(30), th.accent);
            ui.textBlock(info.x + ui.dp(64), info.y + ui.dp(20), info.w - ui.dp(84),
                         "Pick a .img file (boot, init_boot, vendor_boot, dtbo, recovery, super...). Raw and Android sparse "
                         "images are supported.",
                         ui.dp(13.5f), th.textDim);
            y += ui.dp(136);
            if (ui.button(ui.id("pick_img"), Rect(x, y, w, ui.dp(54)), "Choose image", Icon::Folder)) {
                app.push(makeFilesScreen("", {".img", ".bin", ".mbn"},
                                         [](App& a, const std::string& p) { a.pop(); a.push(makeFlashImageScreen(p)); },
                                         "Select image"));
            }
            y += ui.dp(70);
            ui.endScroll(y);
            ui.label(bottom.inset(pad, ui.dp(20)), "Images are flashed from the next screen", ui.dp(13), th.textFaint,
                     Face::Regular, AlignCenter);
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
                int r = ui.listItem(ui.id("q", (int)i), row.inset(0, 0).cutLeft(row.w - ui.dp(52)), Icon::Zip,
                                    std::to_string(i + 1) + ". " + util::basename(g_queue[i]), sz + "  \xE2\x80\xA2  " +
                                    util::dirname(g_queue[i]), "", th.accent, false);
                (void)r;
                if (ui.iconButton(ui.id("qdel", (int)i), Rect(row.r() - ui.dp(48), row.cy() - ui.dp(20), ui.dp(40), ui.dp(40)),
                                  Icon::Close, th.textDim)) {
                    g_queue.erase(g_queue.begin() + i);
                    break;
                }
                iy += ui.dp(66);
            }
            y += card.h + ui.dp(12);
        }
        float bw = (w - ui.dp(10)) / 2;
        if (ui.button(ui.id("add"), Rect(x, y, g_queue.empty() ? w : bw, ui.dp(52)), g_queue.empty() ? "Select zip" : "Add more",
                      Icon::Plus, g_queue.empty() ? BtnStyle::Filled : BtnStyle::Tonal)) {
            app.push(makeFilesScreen("", {".zip"},
                                     [](App& a, const std::string& p) {
                                         if (g_queue.size() < 10) g_queue.push_back(p);
                                         a.pop();
                                         a.toast("Added " + util::basename(p), Icon::Zip);
                                     },
                                     "Select zip"));
        }
        if (!g_queue.empty() &&
            ui.button(ui.id("clear"), Rect(x + bw + ui.dp(10), y, bw, ui.dp(52)), "Clear", Icon::Trash, BtnStyle::Outline))
            g_queue.clear();
        y += ui.dp(68);

        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Options");
        y += ui.dp(30);
        Settings& s = Settings::get();
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

        // Swipe bar
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

// Choose the target partition for an image.
class FlashImageScreen : public Screen {
public:
    explicit FlashImageScreen(std::string img) : img_(std::move(img)) {
        sparse_ = installer::isSparseImage(img_);
        // Guess the target from the file name.
        std::string b = util::toLower(util::basename(img_));
        for (auto& p : Partitions::get().all()) {
            if (!(p.flashImg || p.isImage() || p.mountPoint == "/system_root" || p.mountPoint == "/vendor")) continue;
            if (b.find(p.name) != std::string::npos && (sel_.empty() || p.name.size() > sel_.size())) sel_ = p.mountPoint;
        }
    }
    std::string title() const override { return "Flash image"; }
    std::string subtitle() const override { return util::basename(img_); }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        float pad = ui.dp(16);
        Rect bottom = area.cutBottom(ui.dp(88));
        ui.beginScroll(ui.id("fi_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        Rect info(x, y, w, ui.dp(84));
        ui.card(info);
        icons::drawCentered(*ui.c, Icon::Image, info.x + ui.dp(36), info.cy(), ui.dp(30), th.accent);
        ui.label(Rect(info.x + ui.dp(66), info.y + ui.dp(16), info.w - ui.dp(80), ui.dp(24)), util::basename(img_), ui.dp(16),
                 th.text, Face::SemiBold);
        ui.label(Rect(info.x + ui.dp(66), info.y + ui.dp(42), info.w - ui.dp(80), ui.dp(20)),
                 util::humanSize(util::fileSize(img_)) + (sparse_ ? "  \xE2\x80\xA2  Android sparse image" : "  \xE2\x80\xA2  raw image"),
                 ui.dp(13), th.textDim);
        y += ui.dp(100);
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Target partition");
        y += ui.dp(30);
        auto& parts = Partitions::get().all();
        int n = 0;
        for (auto& p : parts)
            if (p.flashImg || p.isImage() || !p.isImage()) n++;
        Rect card(x, y, w, ui.dp(62) * n + ui.dp(8));
        ui.card(card);
        float iy = y + ui.dp(4);
        for (size_t i = 0; i < parts.size(); i++) {
            auto& p = parts[i];
            bool on = sel_ == p.mountPoint;
            std::string sub = p.blockDev + (p.size ? "  \xE2\x80\xA2  " + util::humanSize(p.size) : "");
            if (ui.checkRow(ui.id("tp", (int)i), Rect(x + ui.dp(4), iy, w - ui.dp(8), ui.dp(58)), p.displayName, sub, on))
                sel_ = p.mountPoint;
            iy += ui.dp(62);
        }
        y += card.h + ui.dp(16);
        ui.endScroll(y);
        Rect sb = bottom.inset(pad, ui.dp(14));
        Partition* tp = Partitions::get().find(sel_);
        if (!tp) {
            ui.c->fillRoundRect(sb, sb.h / 2, th.surface2);
            ui.label(sb, "Select a partition", ui.dp(15), th.textFaint, Face::SemiBold, AlignCenter);
        } else if (ui.slideToConfirm(ui.id("fi_go"), sb, "Swipe to flash " + tp->displayName, th.warn)) {
            std::string img = img_, dev = tp->blockDev, mp = tp->mountPoint;
            app.runTask("Flashing " + tp->displayName, Icon::Bolt, [img, dev, mp](Task& t) {
                if (Partition* p = Partitions::get().find(mp)) Partitions::get().unmount(*p);
                return installer::flashImage(img, dev, t);
            });
        }
    }

private:
    std::string img_, sel_;
    bool sparse_ = false;
};

}  // namespace

std::unique_ptr<Screen> makeInstallScreen() { return std::make_unique<InstallScreen>(); }
std::unique_ptr<Screen> makeFlashImageScreen(const std::string& img) { return std::make_unique<FlashImageScreen>(img); }

}  // namespace tw
