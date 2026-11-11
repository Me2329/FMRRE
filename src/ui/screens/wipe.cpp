#include "ui/screens/screens.h"

#include <set>

#include "core/util.h"
#include "sys/partitions.h"

namespace tw {

namespace {

class WipeScreen : public Screen {
public:
    std::string title() const override { return "Wipe"; }
    std::string subtitle() const override { return "Factory reset & advanced wipe"; }
    void onShow(App&) override { Partitions::get().refresh(); }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("wipe_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;

        // Factory reset hero
        Rect hero(x, y, w, ui.dp(214));
        ui.card(hero, ui.dp(24));
        c.radialGlowRR(hero.r(), hero.y, hero.w * 0.7f, withAlpha(th.danger, 0.18f), hero, ui.dp(24));
        c.fillCircle(hero.x + ui.dp(42), hero.y + ui.dp(44), ui.dp(24), withAlpha(th.danger, 0.16f));
        icons::drawCentered(c, Icon::Wipe, hero.x + ui.dp(42), hero.y + ui.dp(44), ui.dp(26), th.danger);
        ui.label(Rect(hero.x + ui.dp(78), hero.y + ui.dp(22), hero.w - ui.dp(90), ui.dp(26)), "Factory reset", ui.dp(19), th.text, Face::Bold);
        ui.label(Rect(hero.x + ui.dp(78), hero.y + ui.dp(48), hero.w - ui.dp(90), ui.dp(20)), "Data, Cache and ART/Dalvik cache", ui.dp(13),
                 th.textDim);
        // Encrypted data can't be read here: a reset that keeps internal storage is impossible, and FMRRE never
        // formats as a side effect - Format Data (below) is the explicit way.
        const bool enc = Partitions::get().dataEncrypted();
        ui.textBlock(hero.x + ui.dp(20), hero.y + ui.dp(84), hero.w - ui.dp(40),
                     enc ? "Data is encrypted and can't be read here, so a reset can't keep your files. Use Format Data below, or reset "
                           "from Android's Settings."
                         : "Apps and settings are removed. Internal storage (photos, downloads, backups) is kept.",
                     ui.dp(13.5f), enc ? th.warn : th.textDim);
        Rect slide(hero.x + ui.dp(16), hero.b() - ui.dp(74), hero.w - ui.dp(32), ui.dp(58));
        if (enc) {
            ui.c->fillRoundRect(slide, slide.h / 2, th.surface2);
            ui.label(slide, "Not available: data is encrypted", ui.dp(14.5f), th.textFaint, Face::SemiBold, AlignCenter);
        } else if (ui.slideToConfirm(ui.id("factory"), slide, "Swipe to factory reset", th.danger)) {
            app.runTask("Factory reset", Icon::Wipe, [](Task& t) {
                auto& P = Partitions::get();
                bool ok = true;
                const char* parts[] = {"/data", "/cache"};
                for (int i = 0; i < 2; i++) {
                    t.setRange(i * 0.45f, 0.45f);
                    if (Partition* p = P.find(parts[i])) ok = P.wipe(*p, t) && ok;
                }
                t.setRange(0.9f, 0.1f);
                P.wipeDalvik(t);
                t.print(ok ? "Factory reset complete." : "Factory reset finished with errors.");
                return ok;
            });
        }
        y += hero.h + ui.dp(18);

        // Format data
        Rect fd(x, y, w, ui.dp(76));
        ui.card(fd, ui.dp(22));
        if (ui.listItem(ui.id("fmt"), fd.inset(ui.dp(4)), Icon::Warning, "Format Data",
                        "Erase everything incl. internal storage & encryption", "", th.danger) & Clicked)
            app.push(makeFormatDataScreen());
        y += fd.h + ui.dp(10);
        Rect dv(x, y, w, ui.dp(76));
        ui.card(dv, ui.dp(22));
        if (ui.listItem(ui.id("dalvik"), dv.inset(ui.dp(4)), Icon::Refresh, "Wipe ART / Dalvik cache",
                        "Fixes boot loops after updates", "", th.accent) & Clicked)
            app.confirm("Wipe ART cache?", "Apps will be re-optimised on the next boot.", "Swipe to wipe",
                        [&app]() { app.runTask("Wiping ART cache", Icon::Refresh, [](Task& t) { return Partitions::get().wipeDalvik(t); }); },
                        false, Icon::Refresh);
        y += dv.h + ui.dp(22);

        // Advanced wipe
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Advanced wipe");
        y += ui.dp(30);
        std::vector<Partition*> list;
        for (auto& p : Partitions::get().all())
            if (p.wipe && p.present) list.push_back(&p);
        Rect card(x, y, w, ui.dp(62) * list.size() + ui.dp(8));
        ui.card(card);
        float iy = y + ui.dp(4);
        for (auto* p : list) {
            bool on = sel_.count(p->mountPoint) > 0;
            std::string sub = p->fsType + "  \xE2\x80\xA2  " + (p->mounted ? util::humanSize(p->used) + " used" : "not mounted");
            if (ui.checkRow(ui.id(p->mountPoint), Rect(x + ui.dp(4), iy, w - ui.dp(8), ui.dp(58)), p->displayName, sub, on)) {
                if (on) sel_.insert(p->mountPoint); else sel_.erase(p->mountPoint);
            }
            iy += ui.dp(62);
        }
        y += card.h + ui.dp(14);
        if (!sel_.empty()) {
            if (ui.button(ui.id("wipe_sel"), Rect(x, y, w, ui.dp(54)), "Wipe " + std::to_string(sel_.size()) + " partition(s)", Icon::Trash,
                          BtnStyle::Danger)) {
                std::vector<std::string> parts(sel_.begin(), sel_.end());
                std::string names;
                for (auto& mp : parts) names += (names.empty() ? "" : ", ") + Partitions::get().find(mp)->displayName;
                app.confirm("Wipe selected?", names, "Swipe to wipe", [parts, this, &app]() {
                    sel_.clear();
                    app.runTask("Advanced wipe", Icon::Wipe, [parts](Task& t) {
                        bool ok = true;
                        for (size_t i = 0; i < parts.size(); i++) {
                            t.setRange(float(i) / parts.size(), 1.0f / parts.size());
                            if (Partition* p = Partitions::get().find(parts[i])) ok = Partitions::get().wipe(*p, t) && ok;
                        }
                        return ok;
                    });
                });
            }
            y += ui.dp(66);
        }
        ui.endScroll(y + ui.dp(8));
    }

private:
    std::set<std::string> sel_;
};

class FormatDataScreen : public Screen {
public:
    std::string title() const override { return "Format Data"; }
    std::string subtitle() const override { return "Type \"yes\" to continue"; }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        float pad = ui.dp(16);
        Rect kb = area.cutBottom(ui.keyboardHeight());
        float x = area.x + pad, w = area.w - pad * 2, y = area.y + ui.dp(4);
        Rect warn(x, y, w, ui.dp(132));
        ui.card(warn, ui.dp(22), withAlpha(th.danger, 0.12f), false);
        icons::drawCentered(*ui.c, Icon::Warning, warn.x + ui.dp(32), warn.y + ui.dp(34), ui.dp(28), th.danger);
        ui.label(Rect(warn.x + ui.dp(60), warn.y + ui.dp(20), warn.w - ui.dp(70), ui.dp(26)), "This erases EVERYTHING", ui.dp(17),
                 th.danger, Face::Bold);
        ui.textBlock(warn.x + ui.dp(20), warn.y + ui.dp(60), warn.w - ui.dp(40),
                     "Apps, settings, internal storage (photos, music, backups) and file-based encryption are removed.",
                     ui.dp(13.5f), th.text);
        y += warn.h + ui.dp(16);
        bool ok = util::toLower(util::trim(text_)) == "yes";
        ui.textField(ui.id("fd_field"), Rect(x, y, w, ui.dp(56)), text_, "Type yes", true);
        y += ui.dp(70);
        Rect btn(x, y, w, ui.dp(54));
        bool enter = ui.keyboard(ui.id("fd_kb"), kb, text_);
        if (ok) {
            if (ui.button(ui.id("fd_go"), btn, "Format Data now", Icon::Warning, BtnStyle::Danger) || enter) {
                app.pop();
                app.runTask("Format Data", Icon::Warning, [](Task& t) {
                    auto& P = Partitions::get();
                    Partition* d = P.find("/data");
                    if (!d) { t.print("No /data partition in fstab"); return false; }
                    bool r = true;
                    if (Partition* m = P.find("/metadata")) {
                        t.setRange(0, 0.2f);
                        r = P.format(*m, t) && r;
                    }
                    t.setRange(0.2f, 0.8f);
                    r = P.format(*d, t) && r;
                    t.print(r ? "Data formatted. You may need to reboot recovery to use internal storage." : "Format failed.");
                    return r;
                });
            }
        } else {
            ui.c->fillRoundRect(btn, ui.dp(16), th.surface2);
            ui.label(btn, "Type yes to enable", ui.dp(15), th.textFaint, Face::SemiBold, AlignCenter);
        }
    }

private:
    std::string text_;
};

}  // namespace

std::unique_ptr<Screen> makeWipeScreen() { return std::make_unique<WipeScreen>(); }
std::unique_ptr<Screen> makeFormatDataScreen() { return std::make_unique<FormatDataScreen>(); }

}  // namespace tw
