// Display test: full-screen solid colours, a smooth gradient and a fine grid, to check a panel for dead or stuck
// pixels, burn-in (AMOLED), uniformity and touch. Tap to step through; back to leave. Useful on a used phone.
#include "ui/screens/screens.h"

#include "core/i18n.h"

namespace tw {

namespace {

class DisplayTestScreen : public Screen {
public:
    std::string title() const override { return "Display test"; }
    std::string subtitle() const override { return "Tap to change - look for dead pixels and burn-in"; }
    bool hasHeader() const override { return false; }  // cover the whole viewport, no header

    void draw(Ui& ui, App& app, Rect area) override {
        (void)app;
        Canvas& c = *ui.c;
        Rect full(area.x, 0 - c.oy(), area.w, (float)c.height());  // the whole panel in this screen's coordinate space
        for (auto& e : ui.events())
            if (e.type == EvType::TouchUp) step_ = (step_ + 1) % kSteps;

        struct S { const char* name; Color col; };
        static const S solids[] = {{"White", rgb(0xFFFFFF)}, {"Black", rgb(0x000000)}, {"Red", rgb(0xFF0000)},
                                   {"Green", rgb(0x00FF00)}, {"Blue", rgb(0x0000FF)}, {"Grey 50%", rgb(0x808080)}};
        int nSolid = 6;
        if (step_ < nSolid) {
            c.fillRect(full, solids[step_].col);
            label(c, ui, full, solids[step_].name, solids[step_].col);
        } else if (step_ == nSolid) {  // horizontal greyscale gradient - banding / colour-cast check
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
    static constexpr int kSteps = 8;  // 6 solids + gradient + grid
    int step_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeDisplayTestScreen() { return std::make_unique<DisplayTestScreen>(); }

}  // namespace tw
