#include "ui/screens/screens.h"

#include <cmath>

#include "core/settings.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"

namespace tw {

namespace {

class HomeScreen : public Screen {
public:
    std::string title() const override { return "TWRP v2"; }
    bool hasHeader() const override { return false; }

    void onShow(App&) override { refresh(); }

    void refresh() {
        Partitions::get().refresh();
        lastRefresh_ = util::nowMs();
        Partition* d = Partitions::get().find("/data");
        dataUsed_ = d ? d->used : 0;
        dataSize_ = d ? d->size : 0;
        dataMounted_ = d && d->mounted;
        encrypted_ = Partitions::get().dataEncrypted();
    }

    void draw(Ui& ui, App& app, Rect area) override {
        if (util::nowMs() - lastRefresh_ > 5000) refresh();
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);

        // Top bar: logo + title + quick actions
        Rect top = area.cutTop(ui.dp(70));
        Rect logo(top.x + pad, top.cy() - ui.dp(22), ui.dp(44), ui.dp(44));
        c.fillRoundRect(logo, ui.dp(14), th.accent);
        c.radialGlowRR(logo.r(), logo.b(), ui.dp(50), withAlpha(th.accent2, 0.9f), logo, ui.dp(14));
        font::drawIn(c, Face::Bold, ui.dp(17), logo.offset(0, -ui.dp(1)), "TW", th.onAccent, AlignCenter);
        float tx = logo.r() + ui.dp(12);
        font::drawIn(c, Face::Bold, ui.dp(24), Rect(tx, top.cy() - ui.dp(23), ui.dp(200), ui.dp(30)), "TWRP v2", th.text);
        font::drawIn(c, Face::Regular, ui.dp(12.5f), Rect(tx, top.cy() + ui.dp(5), ui.dp(200), ui.dp(18)),
                     "Custom recovery 2.0.0", th.textDim);
        float bx = top.r() - pad - ui.dp(44);
        if (ui.iconButton(ui.id("q_power"), Rect(bx, top.cy() - ui.dp(22), ui.dp(44), ui.dp(44)), Icon::Power, th.text, true))
            app.push(makeRebootScreen());
        bx -= ui.dp(52);
        if (ui.iconButton(ui.id("q_shot"), Rect(bx, top.cy() - ui.dp(22), ui.dp(44), ui.dp(44)), Icon::Camera, th.text, true))
            app.screenshot();
        if (!Settings::get().str("pin_hash").empty()) {
            bx -= ui.dp(52);
            if (ui.iconButton(ui.id("q_lock"), Rect(bx, top.cy() - ui.dp(22), ui.dp(44), ui.dp(44)), Icon::Lock, th.text, true))
                app.lockNow();
        }

        Id sid = ui.id("home_scroll");
        ui.beginScroll(sid, area);
        float y = area.y + ui.dp(4);
        Rect inner(area.x + pad, y, area.w - pad * 2, 0);

        // Hero device card
        const auto& info = device::info();
        Rect hero(inner.x, y, inner.w, ui.dp(176));
        float rad = ui.dp(26);
        c.shadow(hero, rad, ui.dp(22), withAlpha(th.accent, th.light ? 0.25f : 0.22f), ui.dp(6));
        c.fillRoundRect(hero, rad, th.light ? darken(th.accent, 0.1f) : mix(darken(th.accent, 0.62f), th.surface, 0.25f));
        const float ph = 0;  // static: lets the recovery idle at 0% CPU on the home screen
        c.radialGlowRR(hero.r() - ui.dp(40) + ph * ui.dp(12), hero.y + ui.dp(20), hero.w * 0.8f, withAlpha(th.accent, 0.55f), hero, rad);
        c.radialGlowRR(hero.x + ui.dp(30), hero.b() + ui.dp(10) - ph * ui.dp(10), hero.w * 0.6f, withAlpha(th.accent2, 0.35f), hero, rad);
        c.pushClip(hero);
        for (int k = 0; k < 3; k++)
            c.strokeCircle(hero.r() - ui.dp(30), hero.y + ui.dp(40), ui.dp(60 + k * 34.f), ui.dp(1.2f), rgba(255, 255, 255, 22 - k * 5));
        c.popClip();
        Color heroText = rgb(0xFFFFFF);
        Color heroDim = rgba(255, 255, 255, 190);
        float hx = hero.x + ui.dp(20);
        std::string brand = info.brand.empty() ? info.manufacturer : info.brand;
        for (auto& ch : brand) ch = (char)toupper((unsigned char)ch);
        font::drawIn(c, Face::Bold, ui.dp(12), Rect(hx, hero.y + ui.dp(18), hero.w - ui.dp(40), ui.dp(18)),
                     brand.empty() ? "ANDROID" : brand, heroDim);
        font::drawIn(c, Face::Bold, ui.dp(26), Rect(hx, hero.y + ui.dp(38), hero.w - ui.dp(40), ui.dp(34)), info.model, heroText);
        std::string line = info.device + "  \xE2\x80\xA2  Android " + info.android;
        if (info.ab) line += "  \xE2\x80\xA2  Slot " + std::string(1, (char)toupper(info.slot.back()));
        font::drawIn(c, Face::Regular, ui.dp(13.5f), Rect(hx, hero.y + ui.dp(74), hero.w - ui.dp(40), ui.dp(20)), line, heroDim);
        // storage bar
        float frac = dataSize_ ? float(dataUsed_) / dataSize_ : 0;
        Rect bar(hx, hero.b() - ui.dp(46), hero.w - ui.dp(40), ui.dp(8));
        c.fillRoundRect(bar, ui.dp(4), rgba(255, 255, 255, 40));
        if (frac > 0) c.fillRoundRect(Rect(bar.x, bar.y, std::max(bar.h, bar.w * frac), bar.h), ui.dp(4), heroText);
        std::string st = encrypted_ ? "Data is encrypted \xE2\x80\xA2 internal storage unavailable"
                         : dataMounted_ ? ("Internal storage  " + util::humanSize(dataUsed_) + " / " + util::humanSize(dataSize_))
                                        : "Data not mounted";
        font::drawIn(c, Face::SemiBold, ui.dp(12.5f), Rect(hx, hero.b() - ui.dp(32), hero.w - ui.dp(40), ui.dp(20)), st, heroText);
        if (dataSize_ && !encrypted_) {
            char pct[16];
            snprintf(pct, sizeof(pct), "%d%%", int(frac * 100 + 0.5f));
            font::drawIn(c, Face::Bold, ui.dp(12.5f), Rect(hx, hero.b() - ui.dp(32), hero.w - ui.dp(40), ui.dp(20)), pct, heroText,
                         AlignRight);
        }
        int heroRes = ui.clickable(ui.id("hero"), hero);
        ui.drawPressFx(ui.id("hero"), hero, rad, rgb(0xFFFFFF));
        if (heroRes & Clicked) app.push(makeAboutScreen());
        y = hero.b() + ui.dp(20);

        // Main action grid
        struct T { Icon icon; const char* title; const char* sub; Color tint; int action; };
        T tiles[] = {
            {Icon::Install, "Install", "Zips & images", th.accent, 0},
            {Icon::Wipe, "Wipe", "Factory reset", th.danger, 1},
            {Icon::Backup, "Backup", "Save partitions", th.info, 2},
            {Icon::Restore, "Restore", "Bring it back", th.ok, 3},
            {Icon::Mount, "Mount", "Partitions", th.warn, 4},
            {Icon::Folder, "Files", "Browse & manage", th.accent2, 5},
            {Icon::Terminal, "Terminal", "Root shell", rgb(0x94A3B8), 6},
            {Icon::Advanced, "Advanced", "Power tools", rgb(0xC084FC), 7},
            {Icon::Phone, "GSI", "Flash system images", rgb(0x2DD4BF), 8},
            {Icon::Key, "Root", "Magisk & modules", rgb(0xFBBF24), 9},
        };
        const int nTiles = int(sizeof(tiles) / sizeof(tiles[0]));
        float gap = ui.dp(12);
        float tw = (inner.w - gap) / 2, tH = ui.dp(124);
        for (int i = 0; i < nTiles; i++) {
            Rect r(inner.x + (i % 2) * (tw + gap), y + (i / 2) * (tH + gap), tw, tH);
            // staggered entrance
            float appear = ui.anim(ui.id("tile_in", i), 1.f, 9.f - i * 0.6f);
            if (appear < 0.999f) r = r.offset(0, (1 - appear) * ui.dp(30));
            if (ui.tile(ui.id("tile", i), r, tiles[i].icon, tiles[i].title, tiles[i].sub, tiles[i].tint)) {
                switch (tiles[i].action) {
                    case 0: app.push(makeInstallScreen()); break;
                    case 1: app.push(makeWipeScreen()); break;
                    case 2: app.push(makeBackupScreen()); break;
                    case 3: app.push(makeRestoreScreen()); break;
                    case 4: app.push(makeMountScreen()); break;
                    case 5: app.push(makeFilesScreen()); break;
                    case 6: app.push(makeTerminalScreen()); break;
                    case 7: app.push(makeAdvancedScreen()); break;
                    case 8: app.push(makeGsiScreen()); break;
                    case 9: app.push(makeRootScreen()); break;
                }
            }
        }
        y += ((nTiles + 1) / 2) * (tH + gap) + ui.dp(4);

        // Wide rows: Settings & Reboot
        Rect sRow(inner.x, y, inner.w, ui.dp(72));
        ui.card(sRow, ui.dp(22));
        if (ui.listItem(ui.id("set"), sRow.inset(ui.dp(4)), Icon::Settings, "Settings", "Themes, display, security & more", "",
                        th.accent) & Clicked)
            app.push(makeSettingsScreen());
        y += ui.dp(84);
        Rect rRow(inner.x, y, inner.w, ui.dp(72));
        ui.card(rRow, ui.dp(22));
        if (ui.listItem(ui.id("reb"), rRow.inset(ui.dp(4)), Icon::Power, "Reboot", "System, bootloader, fastbootd, EDL...", "",
                        th.danger) & Clicked)
            app.push(makeRebootScreen());
        y += ui.dp(84);
        ui.endScroll(y + ui.dp(8));
    }

private:
    int64_t lastRefresh_ = 0;
    uint64_t dataUsed_ = 0, dataSize_ = 0;
    bool dataMounted_ = false, encrypted_ = false;
};

}  // namespace

std::unique_ptr<Screen> makeHomeScreen() { return std::make_unique<HomeScreen>(); }

}  // namespace tw
