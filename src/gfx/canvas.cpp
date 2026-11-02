#include "gfx/canvas.h"

#include <cmath>
#include <cstring>
#include <unordered_map>

namespace tw {

namespace {
// Alpha masks for expensive-but-repeated shapes (soft shadows, corner glows).
struct AlphaMask {
    int w = 0, h = 0;
    std::vector<uint8_t> a;
    // Non-zero runs per row: runs[rowStart[y] .. rowStart[y+1]) as (x0, x1) pairs.
    std::vector<int> rowStart;
    std::vector<std::pair<int, int>> runs;
    void buildRuns() {
        rowStart.assign(h + 1, 0);
        runs.clear();
        for (int y = 0; y < h; y++) {
            rowStart[y] = (int)runs.size();
            const uint8_t* r = &a[size_t(y) * w];
            int x = 0;
            while (x < w) {
                while (x < w && !r[x]) x++;
                if (x >= w) break;
                int s0 = x;
                while (x < w && r[x]) x++;
                runs.push_back({s0, x});
            }
        }
        rowStart[h] = (int)runs.size();
    }
};
std::unordered_map<uint64_t, AlphaMask> g_masks;

uint64_t maskKey(std::initializer_list<int> v) {
    uint64_t h = 1469598103934665603ull;
    for (int x : v) { h ^= (uint64_t)(uint32_t)x; h *= 1099511628211ull; }
    return h;
}

AlphaMask& maskSlot(uint64_t key, bool& fresh) {
    auto it = g_masks.find(key);
    if (it != g_masks.end()) { fresh = false; return it->second; }
    if (g_masks.size() > 160) g_masks.clear();
    fresh = true;
    return g_masks[key];
}
}  // namespace

static inline int chR(Color c) { return (c >> 16) & 255; }
static inline int chG(Color c) { return (c >> 8) & 255; }
static inline int chB(Color c) { return c & 255; }
static inline int chA(Color c) { return (c >> 24) & 255; }

Color mix(Color a, Color b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    auto m = [t](int x, int y) { return int(x + (y - x) * t + 0.5f); };
    return rgba(m(chR(a), chR(b)), m(chG(a), chG(b)), m(chB(a), chB(b)), m(chA(a), chA(b)));
}
Color lighten(Color c, float t) { return mix(c, (c & 0xFF000000u) | 0xFFFFFF, t); }
Color darken(Color c, float t) { return mix(c, c & 0xFF000000u, t); }

void Canvas::resize(int w, int h) {
    w_ = w;
    h_ = h;
    px_.assign(size_t(w) * h, 0xFF000000u);
    clips_.clear();
    clips_.push_back({0, 0, w, h});
}

void Canvas::pushClip(const Rect& r) {
    IRect c = clipFor(r.x, r.y, r.r(), r.b());
    clips_.push_back(c);
}

void Canvas::popClip() {
    if (clips_.size() > 1) clips_.pop_back();
}

Rect Canvas::clipLocal() const {
    const IRect& c = clips_.back();
    return {c.x0 - ox_, c.y0 - oy_, float(c.x1 - c.x0), float(c.y1 - c.y0)};
}

Canvas::IRect Canvas::clipFor(float x0, float y0, float x1, float y1) const {
    const IRect& c = clips_.back();
    IRect r;
    r.x0 = std::max(c.x0, (int)std::floor(x0 + ox_));
    r.y0 = std::max(c.y0, (int)std::floor(y0 + oy_));
    r.x1 = std::min(c.x1, (int)std::ceil(x1 + ox_));
    r.y1 = std::min(c.y1, (int)std::ceil(y1 + oy_));
    return r;
}

inline void Canvas::blend(uint32_t& d, Color c, int cov) {
    int a = (chA(c) * cov) >> 8;
    if (alpha256_ < 256) a = (a * alpha256_) >> 8;
    if (a <= 0) return;
    if (a >= 255) { d = c | 0xFF000000u; return; }
    // Two channels per multiply (0x00RR00BB and 0x0000GG00).
    uint32_t a2 = a + (a >> 7), ia2 = 256 - a2;
    uint32_t rb = (((c & 0xFF00FFu) * a2 + (d & 0xFF00FFu) * ia2) >> 8) & 0xFF00FFu;
    uint32_t g = (((c & 0x00FF00u) * a2 + (d & 0x00FF00u) * ia2) >> 8) & 0x00FF00u;
    d = 0xFF000000u | rb | g;
}

void Canvas::clear(Color c) {
    const IRect& r = clips_.back();
    for (int y = r.y0; y < r.y1; y++) std::fill(&px_[size_t(y) * w_ + r.x0], &px_[size_t(y) * w_ + r.x1], c | 0xFF000000u);
}

void Canvas::fillRect(const Rect& rc, Color c) {
    IRect r = clipFor(rc.x, rc.y, rc.r(), rc.b());
    if ((c & 0x00FFFFFFu) == 0 && chA(c) < 255) {
        // Black scrim: a pure multiply (much faster than a generic blend).
        int k = 256 - ((chA(c) * alpha256_) >> 8);
        for (int y = r.y0; y < r.y1; y++) {
            uint32_t* row = &px_[size_t(y) * w_];
            for (int x = r.x0; x < r.x1; x++) {
                uint32_t p = row[x];
                row[x] = 0xFF000000u | ((((p & 0xFF00FFu) * k) >> 8) & 0xFF00FFu) | ((((p & 0x00FF00u) * k) >> 8) & 0x00FF00u);
            }
        }
        return;
    }
    if (chA(c) == 255 && alpha_ >= 1.0f) {
        for (int y = r.y0; y < r.y1; y++) std::fill(&px_[size_t(y) * w_ + r.x0], &px_[size_t(y) * w_ + r.x1], c);
        return;
    }
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t* row = &px_[size_t(y) * w_];
        for (int x = r.x0; x < r.x1; x++) blend(row[x], c, 256);
    }
}

void Canvas::gradientV(const Rect& rc, Color top, Color bottom) {
    IRect r = clipFor(rc.x, rc.y, rc.r(), rc.b());
    for (int y = r.y0; y < r.y1; y++) {
        float t = rc.h > 0 ? ((y - oy_) - rc.y) / rc.h : 0;
        Color c = mix(top, bottom, t);
        uint32_t* row = &px_[size_t(y) * w_];
        for (int x = r.x0; x < r.x1; x++) blend(row[x], c, 256);
    }
}

void Canvas::gradientH(const Rect& rc, Color left, Color right) {
    IRect r = clipFor(rc.x, rc.y, rc.r(), rc.b());
    for (int x = r.x0; x < r.x1; x++) {
        float t = rc.w > 0 ? ((x - ox_) - rc.x) / rc.w : 0;
        Color c = mix(left, right, t);
        for (int y = r.y0; y < r.y1; y++) blend(px_[size_t(y) * w_ + x], c, 256);
    }
}

template <typename F>
void Canvas::fillSdf(float x0, float y0, float x1, float y1, Color c, float feather, F sdf) {
    IRect r = clipFor(x0 - feather, y0 - feather, x1 + feather, y1 + feather);
    float inv = 1.0f / std::max(feather, 0.0001f);
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t* row = &px_[size_t(y) * w_];
        float py = y + 0.5f - oy_;
        for (int x = r.x0; x < r.x1; x++) {
            float d = sdf(x + 0.5f - ox_, py);
            float cov = 0.5f - d * inv;
            if (cov <= 0) continue;
            blend(row[x], c, cov >= 1 ? 256 : int(cov * 256));
        }
    }
}

static inline float rrSdf(float px, float py, float cx, float cy, float hw, float hh, float rad) {
    float qx = std::fabs(px - cx) - (hw - rad);
    float qy = std::fabs(py - cy) - (hh - rad);
    float mx = std::max(qx, 0.f), my = std::max(qy, 0.f);
    return std::sqrt(mx * mx + my * my) + std::min(std::max(qx, qy), 0.f) - rad;
}

void Canvas::fillRoundRect(const Rect& rc, float rad, Color c, float feather, bool hollow) {
    if (rc.w <= 0 || rc.h <= 0) return;
    rad = std::min(rad, std::min(rc.w, rc.h) / 2);
    float cx = rc.cx(), cy = rc.cy(), hw = rc.w / 2, hh = rc.h / 2;
    IRect r = clipFor(rc.x - feather, rc.y - feather, rc.r() + feather, rc.b() + feather);
    float inv = 1.0f / std::max(feather, 0.0001f);
    // Interior span (in local x) where coverage only depends on y.
    float spanL = rc.x + rad + feather + 1, spanR = rc.r() - rad - feather - 1;
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t* row = &px_[size_t(y) * w_];
        float py = y + 0.5f - oy_;
        int sx0 = std::max(r.x0, (int)std::ceil(spanL + ox_));
        int sx1 = std::min(r.x1, (int)std::floor(spanR + ox_));
        int midCov = 0;
        if (sx1 > sx0) {
            float d = rrSdf(cx, py, cx, cy, hw, hh, rad);
            float cov = 0.5f - d * inv;
            midCov = cov <= 0 ? 0 : (cov >= 1 ? 256 : int(cov * 256));
        } else {
            sx0 = sx1 = r.x1;
        }
        for (int x = r.x0; x < sx0; x++) {
            float d = rrSdf(x + 0.5f - ox_, py, cx, cy, hw, hh, rad);
            float cov = 0.5f - d * inv;
            if (cov > 0) blend(row[x], c, cov >= 1 ? 256 : int(cov * 256));
        }
        if (midCov > 0 && !(hollow && midCov >= 256))
            for (int x = sx0; x < sx1; x++) blend(row[x], c, midCov);
        for (int x = std::max(sx1, sx0); x < r.x1; x++) {
            float d = rrSdf(x + 0.5f - ox_, py, cx, cy, hw, hh, rad);
            float cov = 0.5f - d * inv;
            if (cov > 0) blend(row[x], c, cov >= 1 ? 256 : int(cov * 256));
        }
    }
}

void Canvas::strokeRoundRect(const Rect& rc, float rad, float width, Color c) {
    rad = std::min(rad, std::min(rc.w, rc.h) / 2);
    float cx = rc.cx(), cy = rc.cy(), hw = rc.w / 2, hh = rc.h / 2, half = width / 2;
    auto f = [&](float px, float py) { return std::fabs(rrSdf(px, py, cx, cy, hw, hh, rad)) - half; };
    // Only the outline band is evaluated: 4 corner boxes + 4 thin edge strips (disjoint clips).
    float k = std::ceil(rad + width + 2);
    if (rc.w < 2 * k + 2 || rc.h < 2 * k + 2) {
        fillSdf(rc.x, rc.y, rc.r(), rc.b(), c, 1, f);
        return;
    }
    float x0 = std::floor(rc.x) - 2, y0 = std::floor(rc.y) - 2, x1 = std::ceil(rc.r()) + 2, y1 = std::ceil(rc.b()) + 2;
    float ix0 = std::floor(rc.x + k), iy0 = std::floor(rc.y + k), ix1 = std::floor(rc.r() - k), iy1 = std::floor(rc.b() - k);
    float t = width + 3;  // strip thickness
    Rect parts[8] = {
        {x0, y0, ix0 - x0, iy0 - y0}, {ix1, y0, x1 - ix1, iy0 - y0},          // top corners
        {x0, iy1, ix0 - x0, y1 - iy1}, {ix1, iy1, x1 - ix1, y1 - iy1},        // bottom corners
        {ix0, y0, ix1 - ix0, t + 2}, {ix0, y1 - t - 2, ix1 - ix0, t + 2},     // top / bottom edges
        {x0, iy0, t + 2, iy1 - iy0}, {x1 - t - 2, iy0, t + 2, iy1 - iy0},     // left / right edges
    };
    for (auto& p : parts) {
        pushClip(p);
        fillSdf(p.x, p.y, p.r(), p.b(), c, 0.0001f + 1, f);
        popClip();
    }
}

void Canvas::shadow(const Rect& rc, float rad, float blur, Color c, float dy, bool hollow) {
    int w = (int)std::lround(rc.w), h = (int)std::lround(rc.h);
    if (w <= 0 || h <= 0) return;
    float r2 = std::min(rad + blur * 0.25f, std::min((float)w, (float)h) / 2);
    int pad = (int)std::ceil(blur) + 1;
    bool fresh;
    AlphaMask& m = maskSlot(maskKey({1, w, h, (int)(r2 * 4), (int)(blur * 4), hollow}), fresh);
    if (fresh) {
        m.w = w + 2 * pad;
        m.h = h + 2 * pad;
        m.a.assign(size_t(m.w) * m.h, 0);
        float cx = m.w / 2.0f, cy = m.h / 2.0f, inv = 1.0f / std::max(blur, 0.001f);
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                float d = rrSdf(x + 0.5f, y + 0.5f, cx, cy, w / 2.0f, h / 2.0f, r2);
                float cov = 0.5f - d * inv;
                if (cov <= 0) continue;
                if (hollow && cov >= 1) continue;
                m.a[size_t(y) * m.w + x] = (uint8_t)(std::min(cov, 1.f) * 255);
            }
    }
    if (fresh) m.buildRuns();
    drawRuns(m.a.data(), m.w, m.h, m.rowStart.data(), m.runs.data(), std::round(rc.x) - pad, std::round(rc.y + dy) - pad, c);
}

template <typename F>
void Canvas::fillRing(float cx, float cy, float ri, float ro, Color c, F sdf) {
    IRect r = clipFor(cx - ro, cy - ro, cx + ro, cy + ro);
    for (int y = r.y0; y < r.y1; y++) {
        float py = y + 0.5f - oy_;
        float dy = py - cy;
        if (std::fabs(dy) > ro) continue;
        float xo = std::sqrt(ro * ro - dy * dy);
        float xi = ri > std::fabs(dy) ? std::sqrt(ri * ri - dy * dy) : 0.f;
        uint32_t* row = &px_[size_t(y) * w_];
        // two spans: left and right of the hole
        float spans[2][2] = {{cx - xo, cx - xi}, {cx + xi, cx + xo}};
        int lastEnd = -1;
        for (auto& sp : spans) {
            int a = std::max(r.x0, (int)std::floor(sp[0] + ox_));
            int b = std::min(r.x1, (int)std::ceil(sp[1] + ox_) + 1);
            a = std::max(a, lastEnd);
            for (int x = a; x < b; x++) {
                float cov = 0.5f - sdf(x + 0.5f - ox_, py);
                if (cov > 0) blend(row[x], c, cov >= 1 ? 256 : int(cov * 256));
            }
            lastEnd = std::max(lastEnd, b);
        }
    }
}

void Canvas::fillCircle(float cx, float cy, float rad, Color c, float feather) {
    fillSdf(cx - rad, cy - rad, cx + rad, cy + rad, c, feather, [&](float px, float py) {
        float dx = px - cx, dy = py - cy;
        return std::sqrt(dx * dx + dy * dy) - rad;
    });
}

void Canvas::strokeCircle(float cx, float cy, float rad, float width, Color c) {
    float h = width / 2;
    fillRing(cx, cy, std::max(0.f, rad - h - 1), rad + h + 1, c, [&](float px, float py) {
        float dx = px - cx, dy = py - cy;
        return std::fabs(std::sqrt(dx * dx + dy * dy) - rad) - h;
    });
}

void Canvas::strokeArc(float cx, float cy, float rad, float width, float a0, float sweep, Color c) {
    if (sweep <= 0) return;
    const float TAU = 6.28318530718f;
    if (sweep >= TAU - 1e-4f) { strokeCircle(cx, cy, rad, width, c); return; }
    float h = width / 2;
    a0 = std::fmod(a0, TAU);
    if (a0 < 0) a0 += TAU;
    float e0x = cx + std::sin(a0) * rad, e0y = cy - std::cos(a0) * rad;
    float e1x = cx + std::sin(a0 + sweep) * rad, e1y = cy - std::cos(a0 + sweep) * rad;
    fillRing(cx, cy, std::max(0.f, rad - h - 1), rad + h + 1, c, [&](float px, float py) {
        float dx = px - cx, dy = py - cy;
        float th = std::atan2(dx, -dy);
        if (th < 0) th += TAU;
        float rel = th - a0;
        if (rel < 0) rel += TAU;
        if (rel <= sweep) return std::fabs(std::sqrt(dx * dx + dy * dy) - rad) - h;
        float d0 = std::hypot(px - e0x, py - e0y), d1 = std::hypot(px - e1x, py - e1y);
        return std::min(d0, d1) - h;
    });
}

void Canvas::line(float x0, float y0, float x1, float y1, float width, Color c) {
    float h = width / 2;
    float bx = x1 - x0, by = y1 - y0;
    float len2 = bx * bx + by * by;
    fillSdf(std::min(x0, x1) - h, std::min(y0, y1) - h, std::max(x0, x1) + h, std::max(y0, y1) + h, c, 1,
            [&](float px, float py) {
                float ax = px - x0, ay = py - y0;
                float t = len2 > 0 ? std::clamp((ax * bx + ay * by) / len2, 0.f, 1.f) : 0.f;
                float dx = ax - bx * t, dy = ay - by * t;
                return std::sqrt(dx * dx + dy * dy) - h;
            });
}

void Canvas::fillCircleInRR(float cx, float cy, float rad, const Rect& rr, float rrRadius, Color c) {
    rrRadius = std::min(rrRadius, std::min(rr.w, rr.h) / 2);
    Rect b = rr.intersect(Rect(cx - rad, cy - rad, 2 * rad, 2 * rad));
    if (b.w <= 0 || b.h <= 0) return;
    float rcx = rr.cx(), rcy = rr.cy(), hw = rr.w / 2, hh = rr.h / 2;
    fillSdf(b.x, b.y, b.r(), b.b(), c, 1, [&](float px, float py) {
        float d1 = std::hypot(px - cx, py - cy) - rad;
        float d2 = rrSdf(px, py, rcx, rcy, hw, hh, rrRadius);
        return std::max(d1, d2);
    });
}

void Canvas::radialGlow(float cx, float cy, float rad, Color c) {
    IRect r = clipFor(cx - rad, cy - rad, cx + rad, cy + rad);
    float inv = 1.0f / rad;
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t* row = &px_[size_t(y) * w_];
        float dy = (y + 0.5f - oy_ - cy) * inv;
        for (int x = r.x0; x < r.x1; x++) {
            float dx = (x + 0.5f - ox_ - cx) * inv;
            float d2 = dx * dx + dy * dy;
            if (d2 >= 1) continue;
            float t = 1 - d2;
            blend(row[x], c, int(t * t * 256));
        }
    }
}

void Canvas::radialGlowRR(float cx, float cy, float rad, Color c, const Rect& rr, float rrRadius) {
    int w = (int)std::lround(rr.w), h = (int)std::lround(rr.h);
    if (w <= 0 || h <= 0) return;
    rrRadius = std::min(rrRadius, std::min((float)w, (float)h) / 2);
    int ox = (int)std::lround(cx - rr.x), oy = (int)std::lround(cy - rr.y);
    bool fresh;
    AlphaMask& m = maskSlot(maskKey({2, w, h, (int)(rrRadius * 4), ox, oy, (int)rad}), fresh);
    if (fresh) {
        m.w = w;
        m.h = h;
        m.a.assign(size_t(w) * h, 0);
        float inv = 1.0f / rad;
        for (int y = 0; y < h; y++) {
            float py = y + 0.5f;
            float dy = (py - oy) * inv;
            for (int x = 0; x < w; x++) {
                float px = x + 0.5f;
                float dx = (px - ox) * inv;
                float d2 = dx * dx + dy * dy;
                if (d2 >= 1) continue;
                float cov = 0.5f - rrSdf(px, py, w / 2.0f, h / 2.0f, w / 2.0f, h / 2.0f, rrRadius);
                if (cov <= 0) continue;
                float t = 1 - d2;
                m.a[size_t(y) * w + x] = (uint8_t)(t * t * std::min(cov, 1.f) * 255);
            }
        }
    }
    if (fresh) m.buildRuns();
    drawRuns(m.a.data(), m.w, m.h, m.rowStart.data(), m.runs.data(), std::round(rr.x), std::round(rr.y), c);
}

void Canvas::drawRuns(const uint8_t* mask, int mw, int mh, const int* rowStart, const std::pair<int, int>* runs, float fx,
                      float fy, Color c) {
    int x = (int)std::lround(fx + ox_), y = (int)std::lround(fy + oy_);
    const IRect& cl = clips_.back();
    int y0 = std::max(cl.y0, y), y1 = std::min(cl.y1, y + mh);
    for (int yy = y0; yy < y1; yy++) {
        int my = yy - y;
        const uint8_t* m = mask + size_t(my) * mw;
        uint32_t* row = &px_[size_t(yy) * w_];
        for (int k = rowStart[my]; k < rowStart[my + 1]; k++) {
            int a0 = std::max(cl.x0, x + runs[k].first), a1 = std::min(cl.x1, x + runs[k].second);
            for (int xx = a0; xx < a1; xx++) {
                int a = m[xx - x];
                blend(row[xx], c, a + (a >> 7));
            }
        }
    }
}

void Canvas::drawMask(const uint8_t* mask, int mw, int mh, int stride, float fx, float fy, Color c) {
    int x = (int)std::lround(fx + ox_), y = (int)std::lround(fy + oy_);
    const IRect& cl = clips_.back();
    int x0 = std::max(cl.x0, x), y0 = std::max(cl.y0, y);
    int x1 = std::min(cl.x1, x + mw), y1 = std::min(cl.y1, y + mh);
    for (int yy = y0; yy < y1; yy++) {
        const uint8_t* m = mask + size_t(yy - y) * stride - x;
        uint32_t* row = &px_[size_t(yy) * w_];
        for (int xx = x0; xx < x1; xx++) {
            int a = m[xx];
            if (a) blend(row[xx], c, a + (a >> 7));
        }
    }
}

void Canvas::blit(const Canvas& src, float fdx, float fdy, float opacity) {
    int dx = (int)std::lround(fdx + ox_), dy = (int)std::lround(fdy + oy_);
    const IRect& cl = clips_.back();
    int x0 = std::max(cl.x0, dx), y0 = std::max(cl.y0, dy);
    int x1 = std::min(cl.x1, dx + src.w_), y1 = std::min(cl.y1, dy + src.h_);
    if (x1 <= x0) return;
    int cov = int(std::clamp(opacity, 0.f, 1.f) * 256);
    for (int y = y0; y < y1; y++) {
        const uint32_t* s = &src.px_[size_t(y - dy) * src.w_ + (x0 - dx)];
        uint32_t* d = &px_[size_t(y) * w_ + x0];
        if (cov >= 256 && alpha_ >= 1.0f) {
            memcpy(d, s, size_t(x1 - x0) * 4);
        } else {
            for (int x = 0; x < x1 - x0; x++) blend(d[x], s[x] | 0xFF000000u, cov);
        }
    }
}

void Canvas::dim(float amount) {
    const IRect& r = clips_.back();
    int k = int((1 - std::clamp(amount, 0.f, 1.f)) * 256);
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t* row = &px_[size_t(y) * w_];
        for (int x = r.x0; x < r.x1; x++) {
            uint32_t p = row[x];
            uint32_t rb = (((p & 0xFF00FF) * k) >> 8) & 0xFF00FF;
            uint32_t g = (((p & 0x00FF00) * k) >> 8) & 0x00FF00;
            row[x] = 0xFF000000u | rb | g;
        }
    }
}

}  // namespace tw
