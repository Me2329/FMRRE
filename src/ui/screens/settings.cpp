#include "ui/screens/screens.h"

#include <cmath>

#include "core/i18n.h"
#include "core/settings.h"
#include "core/sha256.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"

#ifndef TW_BUILD_DATE
#define TW_BUILD_DATE "dev"
#endif

namespace tw {

namespace {

class SettingsScreen : public Screen {
public:
    std::string title() const override { return "Settings"; }
    std::string subtitle() const override { return "Make it yours"; }
    void onShow(App&) override {
        int mx = device::brightnessMax();
        bright_ = mx > 0 ? float(device::brightness()) / mx : 1;
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        Settings& s = Settings::get();
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("set_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        bool changed = false;
        auto rowRect = [&](float& yy) {
            Rect r(x + ui.dp(4), yy, w - ui.dp(8), ui.dp(64));
            yy += ui.dp(68);
            return r;
        };

        // ---- Language
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Language");
        y += ui.dp(30);
        {
            Rect lc(x, y, w, ui.dp(64));
            ui.card(lc);
            float lx = lc.x + ui.dp(14);
            std::string cur = s.str("lang", "en");
            int li = 0;
            for (auto& L : i18n::languages()) {
                float cw = ui.chipWidth(L.name) + ui.dp(10);
                if (ui.chip(ui.id("lang", li++), Rect(lx, lc.y + ui.dp(13), cw, ui.dp(38)), L.name, cur == L.code)) {
                    s.set("lang", L.code);
                    changed = true;
                }
                lx += cw + ui.dp(8);
            }
            y += lc.h + ui.dp(18);
        }

        // ---- Appearance
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Appearance");
        y += ui.dp(30);
        Rect ap(x, y, w, ui.dp(286));
        ui.card(ap);
        font::drawIn(c, Face::SemiBold, ui.dp(15), Rect(ap.x + ui.dp(18), ap.y + ui.dp(14), w, ui.dp(22)), "Accent colour", th.text);
        int acc = s.integer("accent", 0);
        float sw = (ap.w - ui.dp(36)) / kAccentCount;
        for (int i = 0; i < kAccentCount; i++) {
            float cx = ap.x + ui.dp(18) + sw * i + sw / 2, cy = ap.y + ui.dp(64);
            float r = std::min(sw * 0.38f, ui.dp(17));
            Rect hit(cx - sw / 2, cy - ui.dp(24), sw, ui.dp(48));
            Id id = ui.id("acc", i);
            int res = ui.clickable(id, hit);
            float sel = ui.anim(id ^ 0x55ull, acc == i ? 1.f : 0.f, 14);
            if (sel > 0.01f) c.strokeCircle(cx, cy, r + ui.dp(5), ui.dp(2.5f) * sel, kAccents[i].a);
            c.fillCircle(cx, cy, r, kAccents[i].a);
            c.pushClip(Rect(cx, cy - r, r, r * 2));
            c.fillCircle(cx, cy, r, kAccents[i].b);
            c.popClip();
            ui.drawFocusRing(id, hit, ui.dp(12));
            if (res & Clicked) { s.set("accent", i); changed = true; }
        }
        font::drawIn(c, Face::Regular, ui.dp(12), Rect(ap.x + ui.dp(18), ap.y + ui.dp(90), w, ui.dp(18)), kAccents[acc].name, th.textDim);
        font::drawIn(c, Face::SemiBold, ui.dp(15), Rect(ap.x + ui.dp(18), ap.y + ui.dp(118), w, ui.dp(22)), "Background style", th.text);
        int style = s.integer("style", 0);
        float cxp = ap.x + ui.dp(18);
        float cyp = ap.y + ui.dp(148);
        for (int i = 0; i < kStyleCount; i++) {
            float cw = ui.chipWidth(kStyles[i]);
            if (cxp + cw > ap.r() - ui.dp(12)) { cxp = ap.x + ui.dp(18); cyp += ui.dp(46); }
            if (ui.chip(ui.id("style", i), Rect(cxp, cyp, cw, ui.dp(38)), kStyles[i], style == i)) { s.set("style", i); changed = true; }
            cxp += cw + ui.dp(8);
        }
        float yy = ap.y + ui.dp(204);
        bool anim = s.flag("animations", device::animationsByDefault());
        if (ui.toggleRow(ui.id("anim"), Rect(ap.x + ui.dp(4), yy, ap.w - ui.dp(8), ui.dp(64)), Icon::Bolt, "Animations",
                         "Transitions, ripples and motion", anim)) {
            s.setFlag("animations", anim);
            changed = true;
        }
        y += ap.h + ui.dp(10);
        Rect sp(x, y, w, ui.dp(82));
        ui.card(sp);
        float speed = (s.integer("anim_speed", 100) - 50) / 150.0f;  // 0.5x..2x
        char sbuf[32];
        snprintf(sbuf, sizeof(sbuf), "Animation speed  %.1fx", s.integer("anim_speed", 100) / 100.0);
        font::drawIn(c, Face::SemiBold, ui.dp(15), Rect(sp.x + ui.dp(18), sp.y + ui.dp(12), w, ui.dp(22)), sbuf, th.text);
        if (ui.slider(ui.id("aspeed"), Rect(sp.x + ui.dp(22), sp.y + ui.dp(52), sp.w - ui.dp(44), ui.dp(20)), speed)) {
            s.set("anim_speed", int(50 + speed * 150 + 0.5f) / 10 * 10);
            changed = true;
        }
        y += sp.h + ui.dp(12);
        Rect dq(x, y, w, ui.dp(70));
        int det = s.integer("detail", -1);
        ui.card(dq, ui.dp(18));
        if (ui.listItem(ui.id("detail"), dq.inset(ui.dp(4)), Icon::Eye, "Display quality",
                        det == 0 ? "Low - no shadows or animations (faster)" : "High - shadows, glows, animations",
                        det == 0 ? "Low" : "High", th.accent) & Clicked)
            app.push(makeDetailScreen(false));
        y += dq.h + ui.dp(18);

        // ---- Display
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Display");
        y += ui.dp(30);
        Rect dc(x, y, w, ui.dp(176) + ui.dp(68) * 4);
        ui.card(dc);
        icons::drawCentered(c, Icon::Sun, dc.x + ui.dp(28), dc.y + ui.dp(28), ui.dp(20), th.textDim);
        font::drawIn(c, Face::SemiBold, ui.dp(15), Rect(dc.x + ui.dp(50), dc.y + ui.dp(16), w, ui.dp(22)),
                     "Brightness  " + std::to_string(int(bright_ * 100)) + "%", th.text);
        if (ui.slider(ui.id("bright"), Rect(dc.x + ui.dp(22), dc.y + ui.dp(58), dc.w - ui.dp(44), ui.dp(20)), bright_)) {
            bright_ = std::max(bright_, 0.03f);
            device::setBrightness(int(bright_ * device::brightnessMax()));
            s.set("brightness", int(bright_ * 100));
        }
        font::drawIn(c, Face::SemiBold, ui.dp(15), Rect(dc.x + ui.dp(18), dc.y + ui.dp(92), w, ui.dp(22)), "Screen timeout", th.text);
        static const int tos[] = {0, 30, 60, 120, 300, 600};
        static const char* tnames[] = {"Off", "30s", "1m", "2m", "5m", "10m"};
        int to = s.integer("screen_timeout", 120);
        float tx = dc.x + ui.dp(18);
        float tcw = (dc.w - ui.dp(36) - ui.dp(5) * 5) / 6;
        for (int i = 0; i < 6; i++) {
            if (ui.chip(ui.id("to", i), Rect(tx, dc.y + ui.dp(122), tcw, ui.dp(36)), tnames[i], to == tos[i])) s.set("screen_timeout", tos[i]);
            tx += tcw + ui.dp(5);
        }
        yy = dc.y + ui.dp(172);
        bool h24 = s.flag("clock24", true), fh = s.flag("fahrenheit"), rot = s.flag("rotate180"), rb = s.flag("swap_rb");
        if (ui.toggleRow(ui.id("h24"), rowRect(yy), Icon::Timer, "24-hour clock", "", h24)) s.setFlag("clock24", h24);
        if (ui.toggleRow(ui.id("fh"), rowRect(yy), Icon::Thermo, "Fahrenheit", "Temperature unit in the status bar", fh)) s.setFlag("fahrenheit", fh);
        if (ui.toggleRow(ui.id("rot"), rowRect(yy), Icon::Refresh, "Rotate 180\xC2\xB0", "For upside-down panels", rot)) {
            s.setFlag("rotate180", rot);
            changed = true;
        }
        if (ui.toggleRow(ui.id("swaprb"), rowRect(yy), Icon::Eye, "Swap red and blue", "If red shows as blue", rb)) {
            s.setFlag("swap_rb", rb);
            changed = true;
        }
        y += dc.h + ui.dp(10);
        Rect ic(x, y, w, ui.dp(82));
        ui.card(ic);
        int inset = s.integer("inset_top", device::isSim() ? 0 : 24);
        font::drawIn(c, Face::SemiBold, ui.dp(15), Rect(ic.x + ui.dp(18), ic.y + ui.dp(12), w, ui.dp(22)),
                     "Status bar offset (notch)  " + std::to_string(inset) + "dp", th.text);
        float iv = inset / 64.0f;
        if (ui.slider(ui.id("inset"), Rect(ic.x + ui.dp(22), ic.y + ui.dp(52), ic.w - ui.dp(44), ui.dp(20)), iv)) {
            s.set("inset_top", int(iv * 64 + 0.5f));
            changed = true;
        }
        y += ic.h + ui.dp(18);

        // ---- Behaviour & security
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Security & feedback");
        y += ui.dp(30);
        bool hasPin = !s.str("pin_hash").empty();
        Rect sc(x, y, w, ui.dp(68) * 5 + ui.dp(8));
        ui.card(sc);
        yy = sc.y + ui.dp(4);
        bool vib = s.flag("vibrate", true), lob = s.flag("lock_on_blank", true), afx = s.flag("autofix_auto");
        bool req = s.flag("zip_sig_require");
        if (ui.toggleRow(ui.id("vib"), rowRect(yy), Icon::Vibrate, "Haptic feedback", "Vibrate on touch", vib)) s.setFlag("vibrate", vib);
        int pr = ui.listItem(ui.id("pin"), rowRect(yy), Icon::Lock, hasPin ? "Remove PIN lock" : "Set PIN lock",
                             hasPin ? "Recovery asks for the PIN on boot" : "Protect this recovery with a PIN", "", hasPin ? th.danger : th.accent);
        if (pr & Clicked) {
            if (hasPin) {
                s.set("pin_hash", "");
                s.save();
                app.toast("PIN removed", Icon::Lock);
            } else {
                app.push(makeTextInputScreen("New PIN", "4-12 digits", "", [](App& a, const std::string& pin) {
                    bool digits = pin.size() >= 4 && pin.size() <= 12 && pin.find_first_not_of("0123456789") == std::string::npos;
                    if (!digits) { a.toast("PIN must be 4-12 digits", Icon::Warning, a.ui.th.danger); return; }
                    Settings::get().set("pin_hash", Sha256::hexOf(pin + kPinSalt));
                    Settings::get().save();
                    a.toast("PIN set", Icon::Lock, a.ui.th.ok);
                }, true));
            }
        }
        if (ui.toggleRow(ui.id("lob"), rowRect(yy), Icon::Moon, "Lock when screen turns off", hasPin ? "" : "Needs a PIN", lob))
            s.setFlag("lock_on_blank", lob);
        if (ui.toggleRow(ui.id("afx"), rowRect(yy), Icon::Wrench, "Repair automatically after a failed boot",
                         "Runs the safe fixes after a 15 s countdown, then reboots. Never erases data", afx))
            s.setFlag("autofix_auto", afx);
        if (ui.toggleRow(ui.id("sigreq"), rowRect(yy), Icon::Key, "Only install zips signed with a trusted key",
                         "The installed system's keys, this recovery's and FMRRE/keys - like a stock recovery", req))
            s.setFlag("zip_sig_require", req);
        y += sc.h + ui.dp(18);

        // ---- Touch calibration (handy when porting)
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Touch calibration");
        y += ui.dp(30);
        Rect tcR(x, y, w, ui.dp(68) * 3 + ui.dp(8));
        ui.card(tcR);
        yy = tcR.y + ui.dp(4);
        bool tsw = s.flag("touch_swap"), tfx = s.flag("touch_flipx"), tfy = s.flag("touch_flipy");
        if (ui.toggleRow(ui.id("tsw"), rowRect(yy), Icon::Swap, "Swap X / Y", "", tsw)) { s.setFlag("touch_swap", tsw); changed = true; }
        if (ui.toggleRow(ui.id("tfx"), rowRect(yy), Icon::Forward, "Flip X", "", tfx)) { s.setFlag("touch_flipx", tfx); changed = true; }
        if (ui.toggleRow(ui.id("tfy"), rowRect(yy), Icon::Down, "Flip Y", "", tfy)) { s.setFlag("touch_flipy", tfy); changed = true; }
        y += tcR.h + ui.dp(18);

        Rect ab(x, y, w, ui.dp(72));
        ui.card(ab);
        if (ui.listItem(ui.id("about"), ab.inset(ui.dp(4)), Icon::Info, "About FMRRE", "Version, device & credits", "", th.accent) & Clicked)
            app.push(makeAboutScreen());
        y += ab.h + ui.dp(12);
        if (ui.button(ui.id("reset"), Rect(x, y, w, ui.dp(50)), "Reset appearance", Icon::Refresh, BtnStyle::Outline)) {
            s.set("accent", 0);
            s.set("style", 0);
            s.setFlag("animations", device::animationsByDefault());
            s.set("anim_speed", 100);
            changed = true;
        }
        y += ui.dp(66);
        ui.endScroll(y);

        if (changed) app.applySettings();
        // Persist every change made on this screen (cheap: tiny file).
        static int64_t lastSave = 0;
        if (ui.justReleased() && ui.now - lastSave > 300) {
            s.save();
            lastSave = ui.now;
        }
    }

private:
    float bright_ = 1;
};

std::string bootloaderText() {
    int u = device::bootloaderUnlocked();
    return u == 1 ? "Unlocked" : u == 0 ? "Locked" : "not reported";
}

class AboutScreen : public Screen {
public:
    std::string title() const override { return "About"; }

    void draw(Ui& ui, App&, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("about_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        Rect hero(x, y, w, ui.dp(150));
        ui.card(hero, ui.dp(24));
        c.radialGlowRR(hero.cx(), hero.y, hero.w * 0.7f, withAlpha(th.accent, 0.25f), hero, ui.dp(24));
        Rect logo(hero.cx() - ui.dp(30), hero.y + ui.dp(20), ui.dp(60), ui.dp(60));
        c.fillRoundRect(logo, ui.dp(18), th.accent);
        drawLogoText(c, logo, ui.dp(22), th.onAccent);
        font::drawIn(c, Face::Bold, ui.dp(20), Rect(hero.x, hero.y + ui.dp(88), hero.w, ui.dp(26)), "FMRRE  2.0.0", th.text, AlignCenter);
        font::drawIn(c, Face::Regular, ui.dp(12.5f), Rect(hero.x, hero.y + ui.dp(116), hero.w, ui.dp(20)),
                     std::string("FMR Recovery Environment  \xE2\x80\xA2  built ") + TW_BUILD_DATE, th.textDim, AlignCenter);
        y += hero.h + ui.dp(18);
        const auto& i = device::info();
        std::vector<std::pair<std::string, std::string>> kv = {
            {"Model", i.model}, {"Device", i.device}, {"Manufacturer", i.manufacturer}, {"Platform", i.platform},
            {"Android", i.android + " (SDK " + i.sdk + ")"}, {"Security patch", i.patch}, {"Kernel", i.kernel},
            {"Slot", i.ab ? std::string(1, (char)toupper(i.slot.back())) : "not A/B"}, {"Serial", i.serial},
            {"Bootloader", bootloaderText()},
            {"Display", std::to_string(c.width()) + " x " + std::to_string(c.height())},
            {"fstab", Partitions::get().fstabPath()},
            {"Build", std::string(device::buildAbi()) + "  \xE2\x80\xA2  CPU " + device::machine()},
            {"SHA-256", Sha256::backend()},
        };
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Device");
        y += ui.dp(30);
        Rect card(x, y, w, ui.dp(44) * kv.size() + ui.dp(12));
        ui.card(card);
        float ky = y + ui.dp(6);
        for (auto& [k, v] : kv) {
            font::drawIn(c, Face::Regular, ui.dp(13.5f), Rect(x + ui.dp(18), ky, w * 0.4f, ui.dp(44)), k, th.textDim);
            font::drawIn(c, Face::SemiBold, ui.dp(13.5f), Rect(x + w * 0.38f, ky, w * 0.62f - ui.dp(18), ui.dp(44)), v.empty() ? "-" : v,
                         th.text, AlignRight);
            ky += ui.dp(44);
        }
        y += card.h + ui.dp(12);
        if (!device::abiAdvice().empty()) {
            icons::drawCentered(c, Icon::Info, x + ui.dp(10), y + ui.dp(9), ui.dp(15), th.warn);
            y += ui.textBlock(x + ui.dp(26), y, w - ui.dp(26), device::abiAdvice(), ui.dp(12.5f), th.warn) + ui.dp(10);
        }
        y += ui.dp(6);
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Credits");
        y += ui.dp(30);
        Rect cr(x, y, w, ui.dp(150));
        ui.card(cr);
        float th2 = ui.textBlock(cr.x + ui.dp(18), cr.y + ui.dp(16), cr.w - ui.dp(36),
                                 "Inspired by the original TWRP. Fonts: Inter (SIL OFL 1.1), DejaVu Sans Mono. "
                                 "stb_truetype (public domain), miniz (MIT). Licensed under the Apache License 2.0.",
                                 ui.dp(13), th.textDim);
        (void)th2;
        y += cr.h + ui.dp(16);
        ui.endScroll(y);
    }
};

}  // namespace

std::unique_ptr<Screen> makeSettingsScreen() { return std::make_unique<SettingsScreen>(); }
std::unique_ptr<Screen> makeAboutScreen() { return std::make_unique<AboutScreen>(); }

}  // namespace tw
