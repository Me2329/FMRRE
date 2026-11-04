// Bootloop rescue: what the previous boot left behind, in plain words, with a fix next to each cause.
#include "ui/screens/screens.h"

#include "core/i18n.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/bootimg.h"
#include "sys/pathcheck.h"
#include "sys/rescue.h"

namespace tw {

namespace {

float textH(const std::string& s, float size, float w, Face f = Face::Regular) {
    return font::wrap(f, size, s, w).size() * font::lineHeight(f, size);
}

std::string prettyReason(const std::string& r) {
    if (r.empty()) return "not reported";
    std::string l = util::toLower(r);
    if (l.find("kernel_panic") != std::string::npos) return r + "  (kernel crash)";
    if (l.find("watchdog") != std::string::npos) return r + "  (system froze)";
    if (l.find("rescueparty") != std::string::npos) return r + "  (Android gave up)";
    return r;
}

class RescueScreen : public Screen {
public:
    RescueScreen() { refresh(); }
    std::string title() const override { return "Bootloop rescue"; }
    std::string subtitle() const override { return "Why the last boot failed, and what to do"; }

    void refresh() { r_ = rescue::analyze(); }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("rs_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;

        // ---- don't know what's wrong? one tap
        if (ui.button(ui.id("rs_auto"), Rect(x, y, w, ui.dp(52)), "Auto repair - find and fix it for me", Icon::Wrench))
            app.push(makeAutoRepairScreen());
        y += ui.dp(66);

        // ---- last boot
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Last boot");
        y += ui.dp(30);
        std::vector<std::pair<std::string, std::string>> rows = {
            {"Boot reason", prettyReason(r_.bootReason)},
            {"Kernel log", r_.logSource.empty() ? "none kept" : util::basename(r_.logSource) + " (" + util::humanSize(r_.log.size()) + ")"},
        };
        if (!r_.androidReason.empty()) rows.push_back({"Android said", r_.androidReason});
        float slotH = textH(r_.slotInfo, ui.dp(12.5f), w - ui.dp(32));
        Rect lc(x, y, w, ui.dp(26) * rows.size() + slotH + ui.dp(30));
        ui.card(lc);
        float ly = y + ui.dp(12);
        for (auto& row : rows) {
            font::drawIn(c, Face::Regular, ui.dp(13), Rect(x + ui.dp(16), ly, w * 0.4f, ui.dp(20)), row.first, th.textDim);
            font::drawIn(c, Face::SemiBold, ui.dp(13), Rect(x + w * 0.4f, ly, w * 0.6f - ui.dp(16), ui.dp(20)), row.second, th.text, AlignRight);
            ly += ui.dp(26);
        }
        ui.textBlock(x + ui.dp(16), ly + ui.dp(2), w - ui.dp(32), r_.slotInfo, ui.dp(12.5f), th.textDim);
        y += lc.h + ui.dp(16);

        // ---- what was changed last (FMRRE/history.txt)
        if (!r_.changes.empty()) {
            ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Recent changes");
            y += ui.dp(30);
            Rect hc(x, y, w, ui.dp(30) * r_.changes.size() + ui.dp(16));
            ui.card(hc);
            float hy = y + ui.dp(8);
            {
                i18n::NoTranslate raw;  // file and partition names
                for (auto& ch : r_.changes) {
                    font::drawIn(c, Face::Regular, ui.dp(12.5f), Rect(x + ui.dp(16), hy, w - ui.dp(32), ui.dp(24)), ch,
                                 ch.find("(failed)") != std::string::npos ? th.danger : th.textDim);
                    hy += ui.dp(30);
                }
            }
            y += hc.h + ui.dp(16);
        }

        // ---- findings
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), r_.findings.empty() ? "Nothing suspicious found" : "What went wrong");
        y += ui.dp(30);
        if (r_.findings.empty()) {
            float h = ui.textBlock(x + ui.dp(4), y, w - ui.dp(8),
                                   "The previous boot's log shows none of the known failure patterns. Open it below, or save the logs and "
                                   "ask for help with them.",
                                   ui.dp(13), th.textDim);
            y += h + ui.dp(16);
        }
        int k = 0;
        for (auto& f : r_.findings) {
            Color col = f.severity >= 2 ? th.danger : f.severity == 1 ? th.warn : th.info;
            float tw = w - ui.dp(64);
            float h1 = textH(f.title, ui.dp(15), tw, Face::SemiBold), h2 = textH(f.explanation, ui.dp(12.5f), tw);
            std::string ev = f.evidence.substr(0, 220);
            float h3 = ev.empty() ? 0 : textH(ev, ui.dp(11), tw, Face::Mono) + ui.dp(8);
            bool fixable = f.fix != rescue::Fix::None;
            Rect fc(x, y, w, ui.dp(28) + h1 + h2 + h3 + (fixable ? ui.dp(56) : 0));
            ui.card(fc);
            c.fillRoundRect(Rect(x, y + ui.dp(14), ui.dp(4), fc.h - ui.dp(28)), ui.dp(2), col);
            icons::drawCentered(c, f.severity ? Icon::Warning : Icon::Info, x + ui.dp(30), y + ui.dp(26), ui.dp(18), col);
            float ty = y + ui.dp(14);
            ty += ui.textBlock(x + ui.dp(48), ty, tw, f.title, ui.dp(15), th.text, Face::SemiBold) + ui.dp(4);
            ty += ui.textBlock(x + ui.dp(48), ty, tw, f.explanation, ui.dp(12.5f), th.textDim);
            if (!ev.empty()) ty += ui.dp(6) + ui.textBlock(x + ui.dp(48), ty + ui.dp(6), tw, ev, ui.dp(11), th.textFaint, Face::Mono);
            if (fixable &&
                ui.button(ui.id("rs_fix", k), Rect(x + ui.dp(48), fc.b() - ui.dp(50), std::min(tw, ui.dp(230)), ui.dp(40)),
                          rescue::fixLabel(f.fix), Icon::Wrench, f.severity >= 2 ? BtnStyle::Filled : BtnStyle::Tonal))
                apply(app, f.fix);
            y += fc.h + ui.dp(10);
            k++;
        }
        y += ui.dp(8);

        // ---- logs
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Logs");
        y += ui.dp(30);
        bool haveLogcat = !r_.logcat.empty();
        Rect gc(x, y, w, ui.dp(70) * (3 + haveLogcat) + ui.dp(8));
        ui.card(gc);
        float gy = y + ui.dp(4);
        if (ui.listItem(ui.id("rs_view"), Rect(x + ui.dp(4), gy, w - ui.dp(8), ui.dp(66)), Icon::Eye, "Previous boot's kernel log",
                        r_.logSource.empty() ? "Not available on this phone" : pathcheck::pretty(r_.logSource), "", th.accent) & Clicked) {
            if (!r_.logSource.empty()) app.push(makeTextViewerScreen(r_.logSource));
        }
        gy += ui.dp(70);
        if (haveLogcat) {
            if (ui.listItem(ui.id("rs_logcat"), Rect(x + ui.dp(4), gy, w - ui.dp(8), ui.dp(66)), Icon::Terminal, "Previous boot's Android log",
                            "Warnings, errors and crashes (from pstore)", "", th.warn) & Clicked) {
                std::string f = device::path("/tmp/logcat-previous-boot.txt");
                util::writeFile(f, r_.logcat);
                app.push(makeTextViewerScreen(f));
            }
            gy += ui.dp(70);
        }
        if (ui.listItem(ui.id("rs_save"), Rect(x + ui.dp(4), gy, w - ui.dp(8), ui.dp(66)), Icon::Download, "Save logs for a bug report",
                        "Kernel logs, recovery.log and this diagnosis to FMRRE/logs", "", th.ok) & Clicked) {
            std::string d = rescue::saveLogs(r_);
            app.toast(d.empty() ? "Could not save the logs" : "Saved to " + pathcheck::pretty(d), d.empty() ? Icon::Warning : Icon::Check);
        }
        gy += ui.dp(70);
        if (ui.listItem(ui.id("rs_refresh"), Rect(x + ui.dp(4), gy, w - ui.dp(8), ui.dp(66)), Icon::Refresh, "Check again", "Re-read the logs",
                        "", th.textDim) & Clicked) {
            refresh();
            app.toast("Checked again", Icon::Refresh);
        }
        y += gc.h + ui.dp(20);
        ui.endScroll(y);
    }

private:
    void apply(App& app, rescue::Fix f) {
        switch (f) {
            case rescue::Fix::DisableModules:
                app.confirm("Disable all modules?", "Every Magisk / KernelSU module is disabled (not removed). Turn them back on in the Magisk app.",
                            "Swipe to disable", [&app, this] {
                                int n = bootimg::disableAllModules();
                                app.toast("Disabled " + std::to_string(n) + " module(s)", Icon::Check);
                                refresh();
                            }, false);
                break;
            case rescue::Fix::DisableVerity: app.push(makeVbmetaScreen()); break;
            case rescue::Fix::SwitchSlot:
                app.confirm("Boot the other slot?", "The other A/B slot becomes active for the next boot.", "Swipe to switch", [&app] {
                    std::string msg;
                    bool ok = device::switchSlot(msg);
                    app.toast(msg, ok ? Icon::Check : Icon::Warning, ok ? 0 : app.ui.th.danger);
                });
                break;
            case rescue::Fix::FormatData: app.push(makeFormatDataScreen()); break;
            case rescue::Fix::ReflashSystem: app.push(makeGsiScreen()); break;
            case rescue::Fix::RestoreBoot: app.push(makeRootScreen()); break;
            default: break;
        }
    }

    rescue::Report r_;
};

}  // namespace

std::unique_ptr<Screen> makeRescueScreen() { return std::make_unique<RescueScreen>(); }

}  // namespace tw
