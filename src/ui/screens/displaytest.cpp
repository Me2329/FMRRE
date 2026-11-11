// Display test: first three named colour bands (does the panel show red where FMRRE draws red? - some display
// drivers swap red and blue, fixed with one button), then full-screen solid colours, a smooth gradient and a fine
// grid, to check a panel for dead or stuck pixels, burn-in (AMOLED), uniformity and touch. Tap to step through;
// back to leave. Useful on a used phone, and on a phone FMRRE has never run on.
#include "ui/screens/screens.h"

#include "core/i18n.h"
#include "core/settings.h"
#include "ui/app.h"

namespace tw {

namespace {

class DisplayTestScreen : public Screen {
public:
    std::string title() const override { return "Display test"; }
    std::string subtitle() const override { return "Tap to change - look for dead pixels and burn-in"; }
    bool hasHeader() const override { return false; }  // cover the whole viewport, no header

    void draw(Ui& ui, App& app, Rect area) override {
        Canvas& c = *ui.c;
        Rect full(area.x, 0 - c.oy(), area.w, (float)c.height());  // the whole panel in this screen's coordinate space
        Rect swapBtn(full.x + ui.dp(36), full.y + full.h * 0.5f + ui.dp(18), full.w - ui.dp(72), ui.dp(52));
        for (auto& e : ui.events()) {
            if (e.type != EvType::TouchUp) continue;
            if (step_ == 0 && swapBtn.contains(e.x - c.ox(), e.y - c.oy())) continue;  // the button, not "next"
            step_ = (step_ + 1) % kSteps;
        }

        struct S { const char* name; Color col; };
        static const S solids[] = {{"White", rgb(0xFFFFFF)}, {"Black", rgb(0x000000)}, {"Red", rgb(0xFF0000)},
                                   {"Green", rgb(0x00FF00)}, {"Blue", rgb(0x0000FF)}, {"Grey 50%", rgb(0x808080)}};
        int nSolid = 6;
        if (step_ == 0) {  // each band must show the colour it names
            static const S bands[] = {{"RED", rgb(0xFF0000)}, {"GREEN", rgb(0x00FF00)}, {"BLUE", rgb(0x0000FF)}};
            float bh = full.h / 3;
            for (int i = 0; i < 3; i++) {
                Rect b(full.x, full.y + bh * i, full.w, bh);
                c.fillRect(b, bands[i].col);
                i18n::NoTranslate raw;  // the names stay in English, like the colours they stand for
                font::drawIn(c, Face::Bold, ui.dp(26), Rect(b.x, b.y + b.h * 0.5f - ui.dp(i == 1 ? 58 : 18), b.w, ui.dp(34)), bands[i].name,
                             i == 1 ? rgb(0x000000) : rgb(0xFFFFFF), AlignCenter);
            }
            // A dark panel behind the button and its hint: readable on any band, and with the colours swapped.
            c.fillRoundRect(Rect(swapBtn.x - ui.dp(12), swapBtn.y - ui.dp(12), swapBtn.w + ui.dp(24), swapBtn.h + ui.dp(56)), ui.dp(22),
                            rgb(0x0B0F17));
            if (ui.button(ui.id("dt_swap"), swapBtn, "Names don't match? Swap red / blue", Icon::Refresh, BtnStyle::Tonal)) {
                Settings& st = Settings::get();
                st.setFlag("swap_rb", !st.flag("swap_rb"));
                st.save();
                app.applySettings();
            }
            font::drawIn(c, Face::Regular, ui.dp(12.5f), Rect(full.x, swapBtn.b() + ui.dp(10), full.w, ui.dp(20)),
                         "Tap elsewhere to continue, Back to leave", withAlpha(rgb(0xFFFFFF), 0.7f), AlignCenter);
        } else if (step_ <= nSolid) {
            c.fillRect(full, solids[step_ - 1].col);
            label(c, ui, full, solids[step_ - 1].name, solids[step_ - 1].col);
        } else if (step_ == nSolid + 1) {  // horizontal greyscale gradient - banding / colour-cast check
            c.gradientH(full, rgb(0x000000), rgb(0xFFFFFF));
            label(c, ui, full, "Gradient", rgb(0x808080));
        } else {  // fine grid on dark - geometry / stuck pixels
            c.fillRect(full, rgb(0x101014));
            Color line = rgb(0x303038);
            for (float gx = 0; gx <= full.w; gx += ui.dp(24)) c.fillRect(Rect(gx, 0, 1, full.h), line);
            for (float gy = 0; gy <= full.h; gy += ui.dp(24)) c.fillRect(Rect(0, gy, full.w, 1), line);
            label(c, ui, full, "Grid", rgb(0x101014));
        }
        ui.keepAnimating();  // keep polling for taps
    }

private:
    // A legible caption and a step counter, in a colour that contrasts with the background.
    static void label(Canvas& c, Ui& ui, const Rect& full, const std::string& name, Color bg) {
        bool darkBg = ((bg & 0xFF) + ((bg >> 8) & 0xFF) + ((bg >> 16) & 0xFF)) < 384;
        Color fg = darkBg ? rgb(0xFFFFFF) : rgb(0x000000);
        float cy = full.y + full.h * 0.5f;  // centre, clear of the status bar and the bottom nav
        {
            i18n::NoTranslate raw;  // colour names stay in English
            font::drawIn(c, Face::SemiBold, ui.dp(16), Rect(full.x, cy - ui.dp(16), full.w, ui.dp(24)), name, withAlpha(fg, 0.8f), AlignCenter);
        }
        font::drawIn(c, Face::Regular, ui.dp(12.5f), Rect(full.x, cy + ui.dp(10), full.w, ui.dp(20)),
                     "Tap to change, Back to leave", withAlpha(fg, 0.5f), AlignCenter);
    }
    static constexpr int kSteps = 9;  // colour order + 6 solids + gradient + grid
    int step_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeDisplayTestScreen() { return std::make_unique<DisplayTestScreen>(); }

}  // namespace tw
