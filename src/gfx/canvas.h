#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>
#include <utility>

namespace tw {

using Color = uint32_t;  // 0xAARRGGBB

constexpr Color rgba(int r, int g, int b, int a = 255) {
    return (Color(a & 255) << 24) | (Color(r & 255) << 16) | (Color(g & 255) << 8) | Color(b & 255);
}
constexpr Color rgb(uint32_t hex) { return 0xFF000000u | hex; }
inline Color withAlpha(Color c, float a) {
    int na = int(((c >> 24) & 255) * std::clamp(a, 0.0f, 1.0f) + 0.5f);
    return (c & 0x00FFFFFFu) | (Color(na) << 24);
}
Color mix(Color a, Color b, float t);
Color lighten(Color c, float t);  // towards white
Color darken(Color c, float t);   // towards black

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    Rect() = default;
    Rect(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
    float r() const { return x + w; }
    float b() const { return y + h; }
    float cx() const { return x + w / 2; }
    float cy() const { return y + h / 2; }
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    Rect offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
    Rect intersect(const Rect& o) const {
        float nx = std::max(x, o.x), ny = std::max(y, o.y);
        float nr = std::min(r(), o.r()), nb = std::min(b(), o.b());
        return {nx, ny, std::max(0.f, nr - nx), std::max(0.f, nb - ny)};
    }
    // Layout helpers: slice a piece off and return it, shrinking *this.
    Rect cutTop(float a) { Rect o{x, y, w, a}; y += a; h -= a; return o; }
    Rect cutBottom(float a) { Rect o{x, b() - a, w, a}; h -= a; return o; }
    Rect cutLeft(float a) { Rect o{x, y, a, h}; x += a; w -= a; return o; }
    Rect cutRight(float a) { Rect o{r() - a, y, a, h}; w -= a; return o; }
};

// Software ARGB8888 render target with anti-aliased SDF primitives.
class Canvas {
public:
    Canvas() = default;
    Canvas(int w, int h) { resize(w, h); }
    void resize(int w, int h);
    int width() const { return w_; }
    int height() const { return h_; }
    uint32_t* pixels() { return px_.data(); }
    const uint32_t* pixels() const { return px_.data(); }

    // Transform & clipping
    void setOrigin(float ox, float oy) { ox_ = ox; oy_ = oy; }
    float ox() const { return ox_; }
    float oy() const { return oy_; }
    void pushClip(const Rect& r);  // r in local (translated) coords
    void popClip();
    Rect clipLocal() const;        // current clip in local coords
    void setAlpha(float a) { alpha_ = std::clamp(a, 0.f, 1.f); alpha256_ = int(alpha_ * 256); }
    float alpha() const { return alpha_; }
    // Low-detail mode (slow phones): soft shadows and live radial glows are skipped - the heaviest per-frame work at
    // high resolution. Cards keep their outline, so nothing becomes invisible.
    void setLowDetail(bool low) { lowDetail_ = low; }
    bool lowDetail() const { return lowDetail_; }

    void clear(Color c);
    void fillRect(const Rect& r, Color c);
    void gradientV(const Rect& r, Color top, Color bottom);
    void gradientH(const Rect& r, Color left, Color right);
    // hollow: skip the fully-covered interior (for shadows that sit under an opaque card).
    void fillRoundRect(const Rect& r, float radius, Color c, float feather = 1.0f, bool hollow = false);
    void strokeRoundRect(const Rect& r, float radius, float width, Color c);
    void shadow(const Rect& r, float radius, float blur, Color c, float dy = 0, bool hollow = true);
    void fillCircle(float cx, float cy, float rad, Color c, float feather = 1.0f);
    void strokeCircle(float cx, float cy, float rad, float width, Color c);
    // Arc starting at angle a0 (radians, 0 = 12 o'clock, clockwise), sweeping `sweep`.
    void strokeArc(float cx, float cy, float rad, float width, float a0, float sweep, Color c);
    void line(float x0, float y0, float x1, float y1, float width, Color c);
    // Circle clipped to a rounded rect (ripples).
    void fillCircleInRR(float cx, float cy, float rad, const Rect& rr, float rrRadius, Color c);
    void radialGlow(float cx, float cy, float rad, Color c);
    // Glow masked by a rounded rectangle (no square corners poking out).
    void radialGlowRR(float cx, float cy, float rad, Color c, const Rect& rr, float rrRadius);
    // 8-bit coverage mask blended in color c at local (x, y).
    void drawMask(const uint8_t* mask, int mw, int mh, int stride, float x, float y, Color c);
    // Copy another canvas (same format) at local offset with opacity.
    void blit(const Canvas& src, float dx, float dy, float opacity = 1.0f);
    void dim(float amount);  // darken whole clip area

private:
    struct IRect { int x0, y0, x1, y1; };
    IRect clipFor(float x0, float y0, float x1, float y1) const;
    inline void blend(uint32_t& d, Color c, int cov);  // cov 0..256
    void drawRuns(const uint8_t* mask, int mw, int mh, const int* rowStart, const std::pair<int, int>* runs, float x, float y,
                  Color c);
    template <typename F> void fillSdf(float x0, float y0, float x1, float y1, Color c, float feather, F sdf);
    // Evaluate sdf only inside the annulus ri..ro around (cx, cy).
    template <typename F> void fillRing(float cx, float cy, float ri, float ro, Color c, F sdf);

    int w_ = 0, h_ = 0;
    std::vector<uint32_t> px_;
    float ox_ = 0, oy_ = 0, alpha_ = 1;
    int alpha256_ = 256;
    bool lowDetail_ = false;
    std::vector<IRect> clips_;
};

}  // namespace tw
