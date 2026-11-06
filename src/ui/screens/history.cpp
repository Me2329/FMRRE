// The flash history (FMRRE/history.txt): what was flashed, wiped, restored and repaired - newest first - so
// "what did I change, and when?" is one tap away when the phone misbehaves.
#include "ui/screens/screens.h"

#include <ctime>

#include "core/i18n.h"
#include "core/util.h"
#include "sys/history.h"
#include "sys/installer.h"
#include "sys/pathcheck.h"

namespace tw {

namespace {

Icon iconFor(const std::string& a) {
    if (util::startsWith(a, "Install zip")) return Icon::Zip;
    if (a == "A/B OTA") return Icon::Install;
    if (a == "Flash image" || util::startsWith(a, "fastboot")) return Icon::Bolt;
    if (a == "Wipe" || a == "Format") return Icon::Wipe;
    if (a == "Restore") return Icon::Restore;
    if (a == "vbmeta") return Icon::Shield;
    if (a == "Auto repair") return Icon::Wrench;
    if (a == "Install FMRRE") return Icon::Download;
    return Icon::Info;
}

std::string when(int64_t t) {
    time_t tt = time_t(t);
    struct tm tm;
    localtime_r(&tt, &tm);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
    return b;
}

class HistoryScreen : public Screen {
public:
    HistoryScreen() { entries_ = history::recent(120); }
    std::string title() const override { return "Flash history"; }
    std::string subtitle() const override { return "What was flashed, wiped, restored and repaired"; }
    void onShow(App&) override { entries_ = history::recent(120); }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("hist_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        if (entries_.empty()) {
            Rect e(x, y, w, ui.dp(150));
            ui.card(e);
            ui.c->fillCircle(e.cx(), e.y + ui.dp(48), ui.dp(28), th.accentSoft);
            icons::drawCentered(*ui.c, Icon::Timer, e.cx(), e.y + ui.dp(48), ui.dp(28), th.accent);
            ui.label(Rect(e.x, e.y + ui.dp(86), e.w, ui.dp(24)), "Nothing yet", ui.dp(16), th.text, Face::SemiBold, AlignCenter);
            ui.label(Rect(e.x + ui.dp(12), e.y + ui.dp(110), e.w - ui.dp(24), ui.dp(20)),
                     "Every zip, image, OTA, wipe, restore and repair is noted here.", ui.dp(12.5f), th.textDim, Face::Regular, AlignCenter);
            y += e.h + ui.dp(12);
        } else {
            Rect card(x, y, w, ui.dp(70) * entries_.size() + ui.dp(8));
            ui.card(card);
            float iy = y + ui.dp(4);
            for (size_t i = 0; i < entries_.size(); i++) {
                const history::Entry& e = entries_[i];
                bool romSwitch = e.detail.find(installer::kRomSwitchNote) != std::string::npos;
                Color tint = !e.ok ? th.danger : romSwitch ? th.warn : th.accent;
                std::string detail = e.detail;
                if (!detail.empty() && detail.back() == ']' && detail.rfind(" [") != std::string::npos) detail.erase(detail.rfind(" ["));
                int f;
                {
                    i18n::NoTranslate raw;  // file and partition names stay as they are
                    f = ui.listItem(ui.id("he", int(i)), Rect(x + ui.dp(4), iy, w - ui.dp(8), ui.dp(66)), iconFor(e.action),
                                    e.action + (e.ok ? "" : "  \xE2\x80\xA2  failed"), detail, history::ago(e.time), tint, false);
                }
                if (f & Clicked) {
                    Dialog d;
                    d.title = e.action;
                    d.message = e.detail + "\n\n" + when(e.time) + (e.ok ? "" : "  -  failed");
                    d.icon = iconFor(e.action);
                    d.tint = tint;
                    d.buttons = {"OK"};
                    app.dialog(std::move(d));
                }
                iy += ui.dp(70);
            }
            y += card.h + ui.dp(12);
        }
        {
            i18n::NoTranslate raw;
            ui.label(Rect(x, y, w, ui.dp(20)), pathcheck::pretty(history::path()), ui.dp(12), th.textFaint, Face::Regular, AlignCenter);
        }
        y += ui.dp(36);
        ui.endScroll(y);
    }

private:
    std::vector<history::Entry> entries_;
};

}  // namespace

std::unique_ptr<Screen> makeHistoryScreen() { return std::make_unique<HistoryScreen>(); }

}  // namespace tw
