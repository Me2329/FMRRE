// "Can't find your file?" - manual path entry with diagnostics and a generated dd command.
#include "ui/screens/screens.h"

#include "core/settings.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/imagesrc.h"
#include "sys/partitions.h"
#include "sys/pathcheck.h"
#include "sys/super.h"

namespace tw {

namespace {

class ManualFlashScreen : public Screen {
public:
    explicit ManualFlashScreen(const std::string& path) {
        path_ = path.empty() ? "/sdcard/Download/" : pathcheck::pretty(path);
        targets_ = flashTargets();
        recheck();
    }
    std::string title() const override { return "Find & flash a file"; }
    std::string subtitle() const override { return report_.usable() ? "File found" : "Type the path of your image"; }
    void onShow(App&) override { if (dirty_) { dirty_ = false; recheck(); } }

    void setPath(const std::string& p) {
        path_ = p;
        dirty_ = true;
        recheck();
    }

    void recheck() {
        report_ = pathcheck::check(path_);
        info_ = img::Info();
        if (report_.usable()) {
            info_ = img::inspect(report_.resolved);
            int g = info_.ok ? guessTarget(targets_, info_) : -1;
            if (g >= 0 && !userPicked_) sel_ = g;
        }
        cmd_ = {};
        if (report_.usable() && dev().size()) {
            img::Info i = info_.ok ? info_ : img::Info();
            i.path = report_.resolved;
            cmd_ = img::ddCommand(i, dev());
        }
    }

    std::string dev() const {
        if (!customDev_.empty()) return customDev_;
        return sel_ >= 0 && sel_ < (int)targets_.size() ? targets_[sel_].dev : "";
    }

    void editPath(App& app, const std::string& initial) {
        app.push(makeTextInputScreen("File path", "/sdcard/Download/system.img", initial,
                                     [this](App&, const std::string& p) { setPath(p); }));
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        bool ready = report_.usable() && !dev().empty();
        Rect bottom = area.cutBottom(ready ? ui.dp(88) : 0);
        ui.beginScroll(ui.id("mf_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;

        // ---- path field
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "1  \xE2\x80\xA2  Where is the file?");
        y += ui.dp(30);
        Rect f(x, y, w, ui.dp(58));
        int fr = ui.clickable(ui.id("mf_field"), f);
        c.fillRoundRect(f, ui.dp(16), th.surface);
        c.strokeRoundRect(f, ui.dp(16), std::max(1.f, ui.dp(1.2f)), report_.usable() ? th.ok : th.outline);
        ui.drawPressFx(ui.id("mf_field"), f, ui.dp(16), th.accent);
        icons::drawCentered(c, Icon::Edit, f.r() - ui.dp(26), f.cy(), ui.dp(18), th.textDim);
        std::string shown = path_;
        float fs = ui.dp(13.5f);
        while (shown.size() > 4 && font::measure(Face::Mono, fs, shown) > f.w - ui.dp(64)) shown = "\xE2\x80\xA6" + shown.substr(4);
        font::drawIn(c, Face::Mono, fs, Rect(f.x + ui.dp(14), f.y, f.w - ui.dp(56), f.h), shown, th.text);
        if (fr & Clicked) editPath(app, path_);
        y += f.h + ui.dp(10);

        // quick prefixes
        std::vector<std::pair<std::string, std::string>> q = {{"Download", "/sdcard/Download/"}, {"sdcard", "/sdcard/"}, {"tmp", "/tmp/"}};
        for (auto* p : Partitions::get().storages())
            if (p->removable && p->present) q.push_back({p->displayName, pathcheck::pretty(p->contentPath()) + "/"});
        float qx = x;
        for (size_t i = 0; i < q.size(); i++) {
            float cw = ui.chipWidth(q[i].first);
            if (qx + cw > x + w) break;
            if (ui.chip(ui.id("mf_q", (int)i), Rect(qx, y, cw, ui.dp(34)), q[i].first, false)) editPath(app, q[i].second);
            qx += cw + ui.dp(8);
        }
        y += ui.dp(44);
        if (ui.button(ui.id("mf_browse"), Rect(x, y, w, ui.dp(46)), "Browse all files", Icon::Folder, BtnStyle::Tonal)) {
            app.push(makeFilesScreen("", {}, [this](App& a, const std::string& p) {
                setPath(pathcheck::pretty(p));
                userPicked_ = false;
                a.pop();
            }, "Pick any file"));
        }
        y += ui.dp(60);

        // ---- diagnostics
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "2  \xE2\x80\xA2  Check");
        y += ui.dp(30);
        float fsz = ui.dp(13);
        float lh = font::lineHeight(Face::Regular, fsz);
        float h = ui.dp(16);
        std::vector<std::vector<std::string>> wrapped;
        for (auto& fd : report_.findings) {
            wrapped.push_back(font::wrap(Face::Regular, fsz, fd.text, w - ui.dp(60)));
            h += wrapped.back().size() * lh + ui.dp(10);
        }
        bool showResolved = !device::isSim() && !report_.resolved.empty() && report_.resolved != report_.input;
        if (showResolved) h += ui.dp(24);
        Rect cc(x, y, w, h);
        ui.card(cc, ui.dp(18));
        float cy = y + ui.dp(10);
        for (size_t i = 0; i < report_.findings.size(); i++) {
            auto lv = report_.findings[i].level;
            Icon ic = lv == pathcheck::Level::Ok ? Icon::Check : lv == pathcheck::Level::Error ? Icon::Close
                     : lv == pathcheck::Level::Warn ? Icon::Warning : Icon::Info;
            Color col = lv == pathcheck::Level::Ok ? th.ok : lv == pathcheck::Level::Error ? th.danger : lv == pathcheck::Level::Warn ? th.warn : th.info;
            icons::drawCentered(c, ic, x + ui.dp(26), cy + lh / 2, ui.dp(16), col);
            float ly = cy;
            for (auto& l : wrapped[i]) {
                font::draw(c, Face::Regular, fsz, x + ui.dp(46), ly, l, th.text);
                ly += lh;
            }
            cy = ly + ui.dp(10);
        }
        if (showResolved)
            font::drawIn(c, Face::Mono, ui.dp(11.5f), Rect(x + ui.dp(16), cy - ui.dp(2), w - ui.dp(32), ui.dp(20)),
                         "= " + report_.resolved, th.textFaint);
        y += h + ui.dp(10);
        if (!report_.suggestions.empty()) {
            size_t n = std::min<size_t>(report_.suggestions.size(), 8);
            Rect sc(x, y, w, ui.dp(60) * n + ui.dp(8));
            ui.card(sc, ui.dp(18));
            for (size_t i = 0; i < n; i++) {
                const std::string& s = report_.suggestions[i];
                if (ui.listItem(ui.id("mf_sug", (int)i), Rect(x + ui.dp(4), y + ui.dp(4) + i * ui.dp(60), w - ui.dp(8), ui.dp(56)),
                                iconForFile(util::basename(s), false), util::basename(s), (i == 0 ? "Best match  \xE2\x80\xA2  " : "") +
                                util::humanSize(util::fileSize(s)), "", th.accent) & Clicked) {
                    setPath(pathcheck::pretty(s));
                    break;
                }
            }
            y += sc.h + ui.dp(12);
        }

        if (report_.usable()) {
            // ---- image + target
            Rect ic(x, y, w, ui.dp(64));
            ui.card(ic, ui.dp(18));
            icons::drawCentered(c, Icon::Chip, x + ui.dp(30), ic.cy(), ui.dp(22), th.accent);
            ui.label(Rect(x + ui.dp(56), ic.y + ui.dp(10), w - ui.dp(70), ui.dp(22)), "Content", ui.dp(12), th.textDim);
            ui.label(Rect(x + ui.dp(56), ic.y + ui.dp(30), w - ui.dp(70), ui.dp(22)),
                     info_.ok ? info_.describe() : "unrecognised - treated as raw data", ui.dp(13.5f), th.text, Face::SemiBold);
            y += ic.h + ui.dp(18);
            ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "3  \xE2\x80\xA2  Write it to");
            y += ui.dp(30);
            Rect tc(x, y, w, ui.dp(58) * targets_.size() + ui.dp(66));
            ui.card(tc, ui.dp(18));
            float ty = y + ui.dp(4);
            for (size_t i = 0; i < targets_.size(); i++) {
                auto& t = targets_[i];
                bool on = customDev_.empty() && sel_ == (int)i;
                std::string trail;
                if (info_.imageSize && t.size) trail = info_.imageSize <= t.size ? "fits" : "too small";
                if (ui.checkRow(ui.id("mf_t", (int)i), Rect(x + ui.dp(4), ty, w - ui.dp(8), ui.dp(54)), t.label,
                                pathcheck::pretty(t.dev) + (t.size ? "  \xE2\x80\xA2  " + util::humanSize(t.size) : ""), on, trail)) {
                    sel_ = (int)i;
                    customDev_.clear();
                    userPicked_ = true;
                    recheck();
                }
                ty += ui.dp(58);
            }
            if (ui.listItem(ui.id("mf_custom"), Rect(x + ui.dp(4), ty, w - ui.dp(8), ui.dp(56)), Icon::Edit, "Other block device",
                            customDev_.empty() ? "Type a /dev/block/... path" : customDev_, "", th.textDim) & Clicked) {
                app.push(makeTextInputScreen("Block device", "/dev/block/by-name/...", customDev_.empty() ? Partitions::byNameDir() + "/" : customDev_,
                                             [this](App& a, const std::string& d) {
                                                 if (!util::exists(d)) { a.toast(d + " doesn't exist", Icon::Warning, a.ui.th.danger); return; }
                                                 customDev_ = d;
                                                 userPicked_ = true;
                                                 recheck();
                                             }));
            }
            y += tc.h + ui.dp(18);

            // ---- generated command
            if (!cmd_.lines.empty()) {
                ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "4  \xE2\x80\xA2  Command for the recovery terminal");
                y += ui.dp(30);
                float mfs = ui.dp(12.5f), mlh = font::lineHeight(Face::Mono, mfs);
                std::vector<std::string> ml;
                for (auto& l : cmd_.lines)
                    for (auto& wl : font::wrap(Face::Mono, mfs, "$ " + l, w - ui.dp(28))) ml.push_back(wl);
                Rect code(x, y, w, ml.size() * mlh + ui.dp(24));
                ui.card(code, ui.dp(16), th.light ? rgb(0x0F172A) : mix(th.bgBottom, rgb(0x000000), 0.5f), false);
                float ly = y + ui.dp(12);
                for (auto& l : ml) {
                    font::draw(c, Face::Mono, mfs, x + ui.dp(14), ly, l, util::startsWith(l, "$ ") ? rgb(0x7DD3FC) : rgb(0xE2E8F0));
                    ly += mlh;
                }
                y += code.h + ui.dp(10);
                for (auto& n : cmd_.notes) {
                    float th2 = ui.textBlock(x + ui.dp(22), y, w - ui.dp(22), n, ui.dp(12.5f), n.find("WARNING") == 0 ? th.danger : th.textDim);
                    icons::drawCentered(c, Icon::Info, x + ui.dp(8), y + ui.dp(8), ui.dp(13), th.textDim);
                    y += th2 + ui.dp(8);
                }
                y += ui.dp(6);
                float bw = (w - ui.dp(10)) / 2;
                if (ui.button(ui.id("mf_term"), Rect(x, y, bw, ui.dp(50)), "Open in terminal", Icon::Terminal)) {
                    std::string joined;
                    for (auto& l : cmd_.lines) joined += (joined.empty() ? "" : " && ") + l;
                    app.push(makeTerminalScreen("", joined));
                }
                if (ui.button(ui.id("mf_save"), Rect(x + bw + ui.dp(10), y, bw, ui.dp(50)), "Save script", Icon::Download, BtnStyle::Tonal)) {
                    std::string f = Partitions::get().appDir() + "/scripts/flash_" + util::basename(report_.resolved) + ".sh";
                    std::string body = "#!/system/bin/sh\n# Generated by FMRRE\nset -e\n";
                    for (auto& n : cmd_.notes) body += "# " + n + "\n";
                    for (auto& l : cmd_.lines) body += l + "\n";
                    util::mkdirs(util::dirname(f));
                    if (util::writeFile(f, body)) app.toast("Saved " + pathcheck::pretty(f), Icon::Check, th.ok);
                    else app.toast("Couldn't save the script", Icon::Warning, th.danger);
                }
                y += ui.dp(66);
            }
        }
        ui.endScroll(y + ui.dp(8));

        if (ready) {
            Rect sb = bottom.inset(pad, ui.dp(14));
            std::string target = customDev_.empty() ? targets_[sel_].label : util::basename(customDev_);
            if (ui.slideToConfirm(ui.id("mf_go"), sb, "Swipe to flash with FMRRE", th.warn)) {
                std::string src = report_.resolved, d = dev();
                std::string lpName = customDev_.empty() ? targets_[sel_].lpName : "";
                bool verify = Settings::get().flag("img_verify", true);
                app.runTask("Flashing " + target, Icon::Bolt, [src, d, lpName, verify](Task& t) {
                    for (auto& p : Partitions::get().all())
                        if (p.blockDev == d && p.mounted) Partitions::get().unmount(p);
                    img::FlashOptions o;
                    o.verify = verify;
                    if (!lpName.empty()) {
                        lp::Metadata m;
                        std::string e;
                        if (lp::read(lp::superDevice(), lp::currentSlotIndex(), m, e) && m.find(lpName)) o.limit = m.find(lpName)->bytes();
                    }
                    return img::flash(src, "", d, o, t);
                });
            }
        }
    }

private:
    std::string path_, customDev_;
    pathcheck::Report report_;
    img::Info info_;
    std::vector<FlashTarget> targets_;
    img::DdCommand cmd_;
    int sel_ = -1;
    bool userPicked_ = false, dirty_ = false;
};

}  // namespace

std::unique_ptr<Screen> makeManualFlashScreen(const std::string& path) { return std::make_unique<ManualFlashScreen>(path); }

}  // namespace tw
