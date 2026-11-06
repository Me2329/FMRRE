#include "ui/screens/screens.h"

#include <cmath>

#include "core/task.h"
#include "core/i18n.h"
#include "core/util.h"
#include "platform/device.h"

namespace tw {

namespace {

class OperationScreen : public Screen {
public:
    explicit OperationScreen(Icon icon) : icon_(icon) {}
    std::string title() const override { return Task::get().title(); }
    std::string subtitle() const override {
        Task& t = Task::get();
        if (t.running()) return "In progress  \xE2\x80\xA2  please don't reboot";
        return t.success() ? "Completed successfully" : "Finished with errors";
    }
    bool isOperation() const override { return true; }
    bool onBack(App& app) override {
        if (Task::get().running()) {
            app.toast("Please wait for the operation to finish", Icon::Timer);
            return true;
        }
        return false;
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        Task& t = Task::get();
        bool running = t.running();
        bool ok = t.success();
        float pad = ui.dp(16);
        float frac = t.progress();
        shown_ += (frac - shown_) * std::min(1.f, ui.dt * 8);
        if (std::fabs(frac - shown_) > 0.001f) ui.keepAnimating();
        if (running) ui.keepAnimating();

        // Progress ring
        float ringR = ui.dp(78);
        float cx = area.cx(), cy = area.y + ui.dp(20) + ringR + ui.dp(10);
        Color col = running ? th.accent : (ok ? th.ok : th.danger);
        c.radialGlow(cx, cy, ringR * 2.2f, withAlpha(col, 0.18f));
        ui.ring(cx, cy, ringR, ui.dp(12), running ? shown_ : 1.0f, col);
        if (running) {
            // orbiting dot
            float a = (ui.now % 2000) / 2000.0f * 6.2832f;
            c.fillCircle(cx + std::sin(a) * (ringR + ui.dp(18)), cy - std::cos(a) * (ringR + ui.dp(18)), ui.dp(4), withAlpha(col, 0.8f));
            char pct[16];
            snprintf(pct, sizeof(pct), "%d%%", int(shown_ * 100 + 0.5f));
            font::drawIn(c, Face::Bold, ui.dp(40), Rect(cx - ringR, cy - ui.dp(30), ringR * 2, ui.dp(48)), pct, th.text, AlignCenter);
            icons::drawCentered(c, icon_, cx, cy + ui.dp(30), ui.dp(22), th.textDim);
        } else {
            if (doneAt_ == 0) doneAt_ = ui.now;
            float e = easeOutBack(std::min(1.f, (ui.now - doneAt_) / 450.0f));
            if (e < 1) ui.keepAnimating();
            c.fillCircle(cx, cy, ringR * 0.62f * e, withAlpha(col, 0.18f));
            icons::drawCentered(c, ok ? Icon::Check : Icon::Close, cx, cy, ui.dp(64) * e, col);
        }
        float y = cy + ringR + ui.dp(30);
        std::string status = running ? t.status() : (ok ? "Success" : "Failed");
        font::drawIn(c, Face::Bold, ui.dp(20), Rect(area.x + pad, y, area.w - pad * 2, ui.dp(28)), status, th.text, AlignCenter);
        y += ui.dp(30);
        int64_t el = (running ? util::nowMs() : t.endedMs()) - t.startedMs();
        std::string meta = util::humanDuration(el);
        if (running && shown_ > 0.03f) {
            int64_t eta = int64_t(el / shown_ * (1 - shown_));
            meta += "  \xE2\x80\xA2  ~" + util::humanDuration(eta) + " left";
        }
        std::string det = t.detail();
        if (!det.empty() && running) meta += "  \xE2\x80\xA2  " + det;
        font::drawIn(c, Face::Regular, ui.dp(13.5f), Rect(area.x + pad, y, area.w - pad * 2, ui.dp(20)), meta, th.textDim, AlignCenter);
        y += ui.dp(30);

        // Buttons
        Rect btns = area.cutBottom(running ? ui.dp(0) : ui.dp(76));
        // Log card
        Rect logR(area.x + pad, y, area.w - pad * 2, area.b() - y - ui.dp(8));
        if (logR.h > ui.dp(60)) {
            ui.card(logR, ui.dp(18), th.light ? rgb(0x0F172A) : mix(th.bgBottom, rgb(0x000000), 0.45f), false);
            Rect body = logR.inset(ui.dp(12));
            float fs = ui.dp(12.5f);
            float lh = font::lineHeight(Face::Mono, fs);
            i18n::NoTranslate raw;  // command output, shown as it is
            auto lines = t.lines(300);
            std::vector<std::string> wrapped;
            for (auto& l : lines)
                for (auto& w : font::wrap(Face::Mono, fs, l, body.w)) wrapped.push_back(w);
            int maxLines = std::max(1, int(body.h / lh));
            size_t start = wrapped.size() > (size_t)maxLines ? wrapped.size() - maxLines : 0;
            c.pushClip(body);
            float ly = body.y;
            for (size_t i = start; i < wrapped.size(); i++) {
                font::draw(c, Face::Mono, fs, body.x, ly, wrapped[i], rgb(0xE2E8F0));
                ly += lh;
            }
            c.popClip();
        }
        if (!running) {
            Rect b = btns.inset(pad, ui.dp(12));
            float bw = (b.w - ui.dp(10)) / 2;
            if (ui.button(ui.id("op_home"), Rect(b.x, b.y, bw, b.h), "Home", Icon::Home, BtnStyle::Tonal)) app.home();
            if (ok) {
                if (ui.button(ui.id("op_reboot"), Rect(b.x + bw + ui.dp(10), b.y, bw, b.h), "Reboot", Icon::Reboot))
                    app.confirm("Reboot to system?", "Your device will restart now.", "Swipe to reboot", []() { device::reboot(""); },
                                false, Icon::Reboot);
            } else if (ui.button(ui.id("op_back"), Rect(b.x + bw + ui.dp(10), b.y, bw, b.h), "Back", Icon::Back, BtnStyle::Outline)) {
                app.pop();
            }
        }
    }

private:
    Icon icon_;
    float shown_ = 0;
    int64_t doneAt_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeOperationScreen(Icon icon) { return std::make_unique<OperationScreen>(icon); }

}  // namespace tw
