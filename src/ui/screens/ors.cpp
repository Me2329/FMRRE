// OpenRecoveryScript countdown screen and the `fmrre` CLI queue.
#include "ui/screens/screens.h"

#include <cmath>
#include <unistd.h>

#include "core/util.h"
#include "sys/ors.h"

namespace tw {

namespace {

class OrsScreen : public Screen {
public:
    explicit OrsScreen(ors::Script s) : s_(std::move(s)) {}
    std::string title() const override { return "OpenRecoveryScript"; }
    std::string subtitle() const override { return "From " + s_.source; }

    void start(App& app) {
        started_ = true;
        auto lines = s_.lines;
        app.pop();
        app.runTask("OpenRecoveryScript", Icon::Terminal, [lines](Task& t) { return ors::run(lines, t); });
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        if (t0_ == 0) t0_ = ui.now;
        float left = 10.f - (ui.now - t0_) / 1000.f;
        if (!started_ && left <= 0) { start(app); return; }
        ui.keepAnimating();
        float pad = ui.dp(16);
        Rect bottom = area.cutBottom(ui.dp(84));
        float cx = area.cx(), cy = area.y + ui.dp(90);
        ui.ring(cx, cy, ui.dp(58), ui.dp(9), std::max(0.f, left / 10.f), th.accent);
        font::drawIn(c, Face::Bold, ui.dp(36), Rect(cx - ui.dp(50), cy - ui.dp(24), ui.dp(100), ui.dp(44)), std::to_string((int)std::ceil(left)),
                     th.text, AlignCenter);
        float y = cy + ui.dp(76);
        ui.label(Rect(area.x, y, area.w, ui.dp(24)), "Running these commands automatically", ui.dp(15), th.text, Face::SemiBold, AlignCenter);
        y += ui.dp(36);
        Rect box(area.x + pad, y, area.w - pad * 2, bottom.y - y - ui.dp(8));
        ui.card(box, ui.dp(16), th.light ? rgb(0x0F172A) : mix(th.bgBottom, rgb(0x000000), 0.5f), false);
        float ly = box.y + ui.dp(12), lh = font::lineHeight(Face::Mono, ui.dp(12.5f));
        c.pushClip(box);
        for (auto& l : s_.lines) {
            font::draw(c, Face::Mono, ui.dp(12.5f), box.x + ui.dp(12), ly, "> " + l, rgb(0xE2E8F0));
            ly += lh;
        }
        c.popClip();
        Rect b = bottom.inset(pad, ui.dp(14));
        float bw = (b.w - ui.dp(10)) / 2;
        if (ui.button(ui.id("ors_cancel"), Rect(b.x, b.y, bw, b.h), "Cancel", Icon::Close, BtnStyle::Tonal)) {
            started_ = true;
            app.pop();
            app.toast("Script cancelled", Icon::Close);
        }
        if (ui.button(ui.id("ors_run"), Rect(b.x + bw + ui.dp(10), b.y, bw, b.h), "Run now", Icon::Forward) && !started_) start(app);
    }

private:
    ors::Script s_;
    int64_t t0_ = 0;
    bool started_ = false;
};

std::string g_active;  // CLI request currently running as the task

}  // namespace

std::unique_ptr<Screen> makeOrsScreen(const ors::Script& s) { return std::make_unique<OrsScreen>(s); }

void orsIdle(App& app) {
    Task& t = Task::get();
    if (!g_active.empty()) {
        if (t.running()) return;
        std::string out = std::string(t.success() ? "0" : "1") + "\n";
        for (auto& l : t.lines(500)) out += l + "\n";
        util::writeFile(g_active + ".done", out);
        unlink((g_active + ".run").c_str());
        g_active.clear();
        return;
    }
    if (t.running()) return;
    for (auto& e : util::listDir(ors::queueDir())) {
        if (!util::endsWith(e.name, ".cmd")) continue;
        std::string base = ors::queueDir() + "/" + e.name.substr(0, e.name.size() - 4), cmd;
        util::readFile(base + ".cmd", cmd);
        rename((base + ".cmd").c_str(), (base + ".run").c_str());
        cmd = util::trim(cmd);
        g_active = base;
        app.runTask("fmrre " + cmd, Icon::Terminal, [cmd](Task& tt) { return ors::run({cmd}, tt); });
        if (!t.running()) {  // refused (another task) - report and move on
            util::writeFile(base + ".done", "1\nAnother operation is running\n");
            g_active.clear();
        }
        break;
    }
}

}  // namespace tw
