// Auto repair: "my phone doesn't start and I don't know why". Checks everything, says what is wrong
// in plain words, and fixes it - safest fix first, nothing erased unless the user picks Format Data.
#include "ui/screens/screens.h"

#include <cmath>

#include "core/i18n.h"
#include "core/settings.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/autofix.h"

namespace tw {

namespace {

constexpr float kAutoSeconds = 15.f;

float textH(const std::string& s, float size, float w, Face f = Face::Regular) {
    return font::wrap(f, size, s, w).size() * font::lineHeight(f, size);
}

class AutoRepairScreen : public Screen {
public:
    AutoRepairScreen(bool autostart, bool prompt) : autostart_(autostart), prompt_(prompt) {
        reboot_ = Settings::get().flag("autofix_reboot", true);
        refresh();
        // Unattended only for a clear failure, with something to do, and not in a loop.
        if (autostart_ && (!plan_.bootFailed || selectedCount() == 0 || !autofix::autoRunAllowed())) autostart_ = false;
    }
    std::string title() const override { return "Auto repair"; }
    std::string subtitle() const override { return "Finds out why the phone doesn't start, and fixes it"; }
    void onShow(App&) override {
        if (shown_++) refresh();  // back from the repair: show what is left
    }

    void refresh() {
        plan_ = autofix::diagnose(userSays_);
        sel_.clear();
        for (auto& s : plan_.steps) sel_.push_back(s.recommended && s.risk != autofix::Risk::ErasesData);
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        Rect bottom = area.cutBottom(ui.dp(autostart_ ? 132 : 88));
        ui.beginScroll(ui.id("af_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;

        // ---- verdict
        bool failed = plan_.bootFailed;
        int problems = 0;
        for (auto& ch : plan_.checks) problems += ch.level >= 2;
        Color tint = failed || problems ? th.danger : th.ok;
        std::string sub = prompt_ ? "Android couldn't start and asked for a factory reset. Try the fixes below first - nothing is erased unless you "
                                    "choose Format Data."
                          : failed ? "Here is what was found, and what will be done about it."
                                   : "If the phone still doesn't start, tell FMRRE below - many phones keep no log of a failed boot.";
        float tw = w - ui.dp(84);
        // Built from parts so each one is translated ("{*}" patterns don't translate what they capture).
        std::string head = failed ? i18n::tr("The last boot failed") + (plan_.cause.empty() ? std::string() : ": " + i18n::tr(plan_.cause))
                                  : plan_.headline;
        float hh = textH(head, ui.dp(16), tw, Face::SemiBold), sh = textH(sub, ui.dp(12.5f), tw);
        std::string lc = plan_.lastChange.empty() ? "" : i18n::tr("Last change:") + " " + plan_.lastChange;
        float lh = lc.empty() ? 0 : ui.dp(22);
        Rect vc(x, y, w, std::max(ui.dp(84), ui.dp(30) + hh + sh + lh + ui.dp(8)));
        ui.card(vc);
        c.fillCircle(x + ui.dp(38), y + ui.dp(42), ui.dp(24), withAlpha(tint, 0.15f));
        icons::drawCentered(c, failed || problems ? Icon::Warning : Icon::Check, x + ui.dp(38), y + ui.dp(42), ui.dp(24), tint);
        float ty = y + ui.dp(16);
        ty += ui.textBlock(x + ui.dp(72), ty, tw, head, ui.dp(16), th.text, Face::SemiBold) + ui.dp(4);
        ty += ui.textBlock(x + ui.dp(72), ty, tw, sub, ui.dp(12.5f), th.textDim);
        if (!lc.empty()) {
            i18n::NoTranslate raw;  // file names
            font::drawIn(c, Face::Regular, ui.dp(12), Rect(x + ui.dp(72), ty + ui.dp(4), tw, ui.dp(18)), lc, th.textFaint);
        }
        y += vc.h + ui.dp(12);

        if (userSays_ || !failed) {  // no failure on record: let the user say so
            Rect uc(x, y, w, ui.dp(76));
            ui.card(uc);
            bool v = userSays_;
            if (ui.toggleRow(ui.id("af_says"), Rect(x + ui.dp(4), y + ui.dp(4), w - ui.dp(8), ui.dp(68)), Icon::Phone,
                             "My phone doesn't start (bootloop)", "Plan the usual bootloop fixes even without a log", v)) {
                userSays_ = v;
                refresh();
            }
            y += uc.h + ui.dp(16);
        }

        // ---- checks
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Checked");
        y += ui.dp(30);
        Rect cc(x, y, w, ui.dp(34) * plan_.checks.size() + ui.dp(14));
        ui.card(cc);
        float cy = y + ui.dp(7);
        for (auto& ch : plan_.checks) {
            Color col = ch.level >= 2 ? th.danger : ch.level == 1 ? th.warn : th.ok;
            icons::drawCentered(c, ch.level ? Icon::Warning : Icon::Check, x + ui.dp(26), cy + ui.dp(17), ui.dp(15), col);
            font::drawIn(c, Face::Regular, ui.dp(13), Rect(x + ui.dp(44), cy + ui.dp(7), w * 0.56f - ui.dp(44), ui.dp(20)), ch.title, th.text);
            font::drawIn(c, Face::SemiBold, ui.dp(12.5f), Rect(x + w * 0.56f, cy + ui.dp(7), w * 0.44f - ui.dp(16), ui.dp(20)), ch.status,
                         ch.level ? col : th.textDim, AlignRight);
            cy += ui.dp(34);
        }
        y += cc.h + ui.dp(16);

        // ---- the plan
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), plan_.steps.empty() ? "Nothing to repair" : "Repair plan (safest first)");
        y += ui.dp(30);
        if (plan_.steps.empty()) {
            y += ui.textBlock(x + ui.dp(4), y, w - ui.dp(8),
                              failed ? "No automatic fix applies. See the advice below, or open Bootloop rescue for the full logs."
                                     : "Everything checked looks fine. If the phone still doesn't start, switch on \"My phone doesn't start\" above.",
                              ui.dp(13), th.textDim) + ui.dp(16);
        }
        for (size_t i = 0; i < plan_.steps.size(); i++) {
            const autofix::Step& s = plan_.steps[i];
            float stw = w - ui.dp(70);
            float h1 = textH(s.title, ui.dp(14.5f), stw, Face::SemiBold), h2 = textH(s.why, ui.dp(12.5f), stw);
            float h3 = s.note.empty() ? 0 : textH(s.note, ui.dp(12.5f), stw) + ui.dp(4);
            float h4 = s.file.empty() ? 0 : ui.dp(20);
            Rect sc(x, y, w, ui.dp(58) + h1 + h2 + h3 + h4);
            ui.card(sc);
            bool on = sel_[i];
            int res = ui.clickable(ui.id("af_step", (int)i), sc);
            ui.drawPressFx(ui.id("af_step", (int)i), sc, ui.dp(18), th.text);
            if (res & Clicked) sel_[i] = on = !on;
            ui.checkboxVisual(ui.id("af_cb", (int)i), Rect(x + ui.dp(16), y + ui.dp(16), ui.dp(24), ui.dp(24)), on);
            float sy = y + ui.dp(14);
            sy += ui.textBlock(x + ui.dp(54), sy, stw, s.title, ui.dp(14.5f), th.text, Face::SemiBold) + ui.dp(3);
            sy += ui.textBlock(x + ui.dp(54), sy, stw, s.why, ui.dp(12.5f), th.textDim);
            if (!s.note.empty()) sy += ui.dp(4) + ui.textBlock(x + ui.dp(54), sy + ui.dp(4), stw, s.note, ui.dp(12.5f), th.warn);
            if (!s.file.empty()) {
                i18n::NoTranslate raw;
                font::drawIn(c, Face::Mono, ui.dp(11.5f), Rect(x + ui.dp(54), sy + ui.dp(2), stw, ui.dp(18)), util::basename(s.file), th.textFaint);
                sy += ui.dp(20);
            }
            sy += ui.dp(8);
            Color rc = s.risk == autofix::Risk::Safe ? th.ok : s.risk == autofix::Risk::Undoable ? th.info : th.danger;
            float bx = x + ui.dp(54);
            ui.badge(bx, sy + ui.dp(10), autofix::riskName(s.risk), rc);
            bx += font::measure(Face::SemiBold, ui.dp(11.5f), autofix::riskName(s.risk)) + ui.dp(22);
            if (s.recommended) {
                ui.badge(bx, sy + ui.dp(10), "Recommended", th.accent);
                bx += font::measure(Face::SemiBold, ui.dp(11.5f), "Recommended") + ui.dp(22);
            }
            if (s.triedBefore) ui.badge(bx, sy + ui.dp(10), "Tried already", th.warn);
            y += sc.h + ui.dp(10);
        }
        for (auto& a : plan_.advice) {
            float h = textH(a, ui.dp(12.5f), w - ui.dp(56));
            Rect nr(x, y, w, h + ui.dp(20));
            c.fillRoundRect(nr, ui.dp(14), withAlpha(th.info, 0.10f));
            icons::drawCentered(c, Icon::Info, x + ui.dp(22), y + ui.dp(20), ui.dp(16), th.info);
            ui.textBlock(x + ui.dp(40), y + ui.dp(10), w - ui.dp(56), a, ui.dp(12.5f), th.text);
            y += nr.h + ui.dp(8);
        }
        y += ui.dp(8);

        // ---- options
        Rect oc(x, y, w, ui.dp(72) * 2 + ui.dp(8));
        ui.card(oc);
        bool rb = reboot_;
        if (ui.toggleRow(ui.id("af_rb"), Rect(x + ui.dp(4), y + ui.dp(4), w - ui.dp(8), ui.dp(68)), Icon::Reboot, "Reboot when done",
                         "Start Android right after the repair", rb)) {
            reboot_ = rb;
            Settings::get().setFlag("autofix_reboot", rb);
            Settings::get().save();
        }
        if (ui.listItem(ui.id("af_rescue"), Rect(x + ui.dp(4), y + ui.dp(76), w - ui.dp(8), ui.dp(66)), Icon::Eye, "Details and logs",
                        "Bootloop rescue: the previous boot's logs, explained", "", th.accent) & Clicked)
            app.push(makeRescueScreen());
        y += oc.h + ui.dp(12);
        if (prompt_ || failed) {
            if (ui.button(ui.id("af_try"), Rect(x, y, w, ui.dp(48)), "Just try starting Android again", Icon::Reboot, BtnStyle::Tonal))
                device::reboot("");
            y += ui.dp(60);
        }
        ui.endScroll(y + ui.dp(8));

        // ---- bottom: countdown or swipe
        int n = selectedCount();
        bool erases = false;
        for (size_t i = 0; i < plan_.steps.size(); i++) erases |= sel_[i] && plan_.steps[i].risk == autofix::Risk::ErasesData;
        if (autostart_ && !started_) {
            if (t0_ == 0) t0_ = ui.now;
            float left = kAutoSeconds - (ui.now - t0_) / 1000.f;
            if (left <= 0) { start(app, true); return; }
            ui.keepAnimating();
            Rect b = bottom.inset(pad, ui.dp(12));
            ui.progressBar(Rect(b.x, b.y, b.w, ui.dp(6)), 1.f - left / kAutoSeconds, th.accent);
            ui.label(Rect(b.x, b.y + ui.dp(14), b.w, ui.dp(22)),
                     "Repairing automatically in " + std::to_string((int)std::ceil(left)) + " s", ui.dp(14), th.text, Face::SemiBold, AlignCenter);
            float bw = (b.w - ui.dp(10)) / 2;
            Rect br(b.x, b.b() - ui.dp(52), bw, ui.dp(52));
            if (ui.button(ui.id("af_cancel"), br, "Cancel", Icon::Close, BtnStyle::Tonal)) autostart_ = false;
            if (ui.button(ui.id("af_now"), Rect(br.r() + ui.dp(10), br.y, bw, br.h), "Repair now", Icon::Wrench)) start(app, true);
            return;
        }
        Rect sb = bottom.inset(pad, ui.dp(14));
        if (n == 0) {
            c.fillRoundRect(sb, sb.h / 2, th.surface2);
            ui.label(sb, plan_.steps.empty() ? "Nothing to repair" : "Select fixes above", ui.dp(14), th.textFaint, Face::SemiBold, AlignCenter);
        } else if (ui.slideToConfirm(ui.id("af_go"), sb,
                                     erases ? "Swipe to repair and ERASE data" : n == 1 ? "Swipe to repair" : "Swipe to run " + std::to_string(n) + " fixes",
                                     erases ? th.danger : th.accent)) {
            start(app, false);
        }
    }

private:
    int selectedCount() const {
        int n = 0;
        for (bool b : sel_) n += b;
        return n;
    }

    void start(App& app, bool unattended) {
        started_ = true;
        autostart_ = false;
        autofix::Options o;
        for (size_t i = 0; i < plan_.steps.size(); i++)
            if (sel_[i]) o.only.insert(plan_.steps[i].id);
        if (o.only.empty()) return;
        o.allowSlotSwitch = true;  // chosen (or recommended for an unattended run) on this screen
        o.allowFormatData = !unattended && o.only.count("format_data");
        o.reboot = reboot_ || unattended;
        if (unattended) autofix::noteAutoRun();
        autofix::Plan p = plan_;
        app.runTask("Auto repair", Icon::Wrench, [p, o](Task& t) { return autofix::apply(p, o, t); });
        started_ = false;
    }

    autofix::Plan plan_;
    std::vector<bool> sel_;
    bool autostart_ = false, prompt_ = false, userSays_ = false, reboot_ = true, started_ = false;
    int shown_ = 0;
    int64_t t0_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeAutoRepairScreen(bool autostart, bool prompt) { return std::make_unique<AutoRepairScreen>(autostart, prompt); }

}  // namespace tw
