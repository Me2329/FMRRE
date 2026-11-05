// Display-quality picker. Shown once on the first boot (the `detail` setting hasn't been chosen yet) and reachable
// again from Settings. Low detail turns off soft shadows, background glows and animations - the heavy per-frame work
// at a phone's full resolution - so the UI stays smooth on older hardware (2017-era and before).
#include "ui/screens/screens.h"

#include "core/i18n.h"
#include "core/settings.h"
#include "platform/device.h"
#include "ui/app.h"

namespace tw {

namespace {

class DetailScreen : public Screen {
public:
    explicit DetailScreen(bool firstRun) : firstRun_(firstRun) {}
    std::string title() const override { return "Display quality"; }
    std::string subtitle() const override { return firstRun_ ? "Welcome - pick what runs best on this phone" : "How much the interface draws"; }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("dt_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        int cur = Settings::get().integer("detail", -1);

        Rect intro(x, y, w, ui.dp(72));
        ui.card(intro, ui.dp(18));
        ui.label(Rect(intro.x + ui.dp(16), intro.y + ui.dp(14), intro.w - ui.dp(32), ui.dp(22)),
                 "Older phones draw faster with less detail.", ui.dp(13), th.text, Face::SemiBold);
        ui.label(Rect(intro.x + ui.dp(16), intro.y + ui.dp(40), intro.w - ui.dp(32), ui.dp(22)),
                 "You can change this any time in Settings.", ui.dp(12.5f), th.textDim, Face::Regular);
        y += intro.h + ui.dp(14);

        auto choice = [&](const char* name, const char* sub, Icon icon, Color tint, int value) {
            Rect r(x, y, w, ui.dp(96));
            bool sel = cur == value;
            int res = ui.clickable(ui.id("dt", value), r);
            ui.card(r, ui.dp(20), sel ? th.accentSoft : 0);
            ui.c->fillCircle(r.x + ui.dp(44), r.cy(), ui.dp(26), withAlpha(tint, 0.16f));
            icons::drawCentered(*ui.c, icon, r.x + ui.dp(44), r.cy(), ui.dp(24), tint);
            ui.label(Rect(r.x + ui.dp(82), r.y + ui.dp(20), r.w - ui.dp(150), ui.dp(26)), name, ui.dp(17), th.text, Face::SemiBold);
            ui.label(Rect(r.x + ui.dp(82), r.y + ui.dp(46), r.w - ui.dp(120), ui.dp(40)), sub, ui.dp(12.5f), th.textDim, Face::Regular);
            if (sel) {
                ui.c->fillCircle(r.r() - ui.dp(30), r.cy(), ui.dp(13), tint);
                icons::drawCentered(*ui.c, Icon::Check, r.r() - ui.dp(30), r.cy(), ui.dp(13), th.onAccent);
            }
            y += r.h + ui.dp(12);
            return (res & Clicked) != 0;
        };

        bool pickHigh = choice("High detail", "Shadows, glows and animations", Icon::Eye, th.accent, 1);
        bool pickLow = choice("Low detail", "Flat and instant - best on slow phones", Icon::Bolt, th.warn, 0);
        if (pickHigh || pickLow) {
            Settings::get().set("detail", pickHigh ? 1 : 0);
            Settings::get().save();
            app.applySettings();
            app.vibrate();
            if (firstRun_) app.home();  // first boot: straight to the home screen once chosen
        }
        y += ui.dp(8);
        ui.endScroll(y);
    }

private:
    bool firstRun_;
};

}  // namespace

std::unique_ptr<Screen> makeDetailScreen(bool firstRun) { return std::make_unique<DetailScreen>(firstRun); }

}  // namespace tw
