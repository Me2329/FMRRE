#include "gfx/icons.h"

#include <cmath>
#include <map>
#include <vector>

namespace tw::icons {

namespace {

enum class Op { Seg, Disc, Ring, Arc, Box, BoxStroke, Poly };

struct Shape {
    Op op;
    float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;  // parameters (meaning depends on op)
    float w = 2;                                      // stroke width
    bool sub = false;                                 // subtract from union
    std::vector<float> pts;                           // polygon xy pairs
};

using Def = std::vector<Shape>;

Shape S(float x1, float y1, float x2, float y2, float w = 2) { return {Op::Seg, x1, y1, x2, y2, 0, 0, w}; }
Shape D(float cx, float cy, float r) { return {Op::Disc, cx, cy, r}; }
Shape R(float cx, float cy, float r, float w = 2) { return {Op::Ring, cx, cy, r, 0, 0, 0, w}; }
// Arc: start angle and sweep in degrees (0 = 12 o'clock, clockwise).
Shape A(float cx, float cy, float r, float w, float a0, float sweep) {
    return {Op::Arc, cx, cy, r, a0 * 0.0174532925f, sweep * 0.0174532925f, 0, w};
}
Shape B(float x, float y, float bw, float bh, float rad) { return {Op::Box, x, y, bw, bh, rad}; }
Shape BS(float x, float y, float bw, float bh, float rad, float w = 2) { return {Op::BoxStroke, x, y, bw, bh, rad, 0, w}; }
Shape P(std::vector<float> pts) { Shape s{Op::Poly}; s.pts = std::move(pts); return s; }
Shape sub(Shape s) { s.sub = true; return s; }

float sdSeg(float px, float py, const Shape& s) {
    float ax = px - s.a, ay = py - s.b, bx = s.c - s.a, by = s.d - s.b;
    float l2 = bx * bx + by * by;
    float t = l2 > 0 ? std::fmin(std::fmax((ax * bx + ay * by) / l2, 0.f), 1.f) : 0.f;
    return std::hypot(ax - bx * t, ay - by * t) - s.w / 2;
}

float sdBox(float px, float py, float x, float y, float w, float h, float r) {
    float cx = x + w / 2, cy = y + h / 2;
    float qx = std::fabs(px - cx) - (w / 2 - r), qy = std::fabs(py - cy) - (h / 2 - r);
    return std::hypot(std::fmax(qx, 0.f), std::fmax(qy, 0.f)) + std::fmin(std::fmax(qx, qy), 0.f) - r;
}

float sdPoly(float px, float py, const std::vector<float>& v) {
    size_t n = v.size() / 2;
    float d = (px - v[0]) * (px - v[0]) + (py - v[1]) * (py - v[1]);
    float s = 1;
    for (size_t i = 0, j = n - 1; i < n; j = i, i++) {
        float ex = v[j * 2] - v[i * 2], ey = v[j * 2 + 1] - v[i * 2 + 1];
        float wx = px - v[i * 2], wy = py - v[i * 2 + 1];
        float t = std::fmin(std::fmax((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.f), 1.f);
        float bx = wx - ex * t, by = wy - ey * t;
        d = std::fmin(d, bx * bx + by * by);
        bool c1 = py >= v[i * 2 + 1], c2 = py < v[j * 2 + 1], c3 = ex * wy > ey * wx;
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
    }
    return s * std::sqrt(d);
}

float sdShape(float px, float py, const Shape& s) {
    switch (s.op) {
        case Op::Seg: return sdSeg(px, py, s);
        case Op::Disc: return std::hypot(px - s.a, py - s.b) - s.c;
        case Op::Ring: return std::fabs(std::hypot(px - s.a, py - s.b) - s.c) - s.w / 2;
        case Op::Arc: {
            const float TAU = 6.28318530718f;
            float dx = px - s.a, dy = py - s.b;
            float th = std::atan2(dx, -dy);
            if (th < 0) th += TAU;
            float rel = th - s.d;
            while (rel < 0) rel += TAU;
            while (rel >= TAU) rel -= TAU;
            if (rel <= s.e) return std::fabs(std::hypot(dx, dy) - s.c) - s.w / 2;
            float e0x = s.a + std::sin(s.d) * s.c, e0y = s.b - std::cos(s.d) * s.c;
            float e1x = s.a + std::sin(s.d + s.e) * s.c, e1y = s.b - std::cos(s.d + s.e) * s.c;
            return std::fmin(std::hypot(px - e0x, py - e0y), std::hypot(px - e1x, py - e1y)) - s.w / 2;
        }
        case Op::Box: return sdBox(px, py, s.a, s.b, s.c, s.d, s.e);
        case Op::BoxStroke: return std::fabs(sdBox(px, py, s.a, s.b, s.c, s.d, s.e)) - s.w / 2;
        case Op::Poly: return sdPoly(px, py, s.pts);
    }
    return 1e9f;
}

Def define(Icon i) {
    switch (i) {
        case Icon::Install:
        case Icon::Download:
            return {S(12, 3, 12, 14), S(7, 9.5, 12, 14.5), S(17, 9.5, 12, 14.5), S(4, 15, 4, 19.5), S(4, 19.5, 20, 19.5),
                    S(20, 19.5, 20, 15)};
        case Icon::Wipe:
            return {S(17, 2.5, 13, 10.5, 2.2), P({6.5, 11, 17, 11, 18, 15, 5.5, 15}), S(7, 15.5, 5, 21, 1.8),
                    S(10, 15.5, 9.5, 21, 1.8), S(13.5, 15.5, 14, 21, 1.8), S(16.5, 15.5, 18.5, 21, 1.8)};
        case Icon::Backup:
            return {D(8, 14.5, 4.2), D(13, 11, 5.8), D(17.3, 15, 3.8), B(3.8, 14.5, 17.4, 6, 3),
                    sub(S(12, 19, 12, 12.5, 2)), sub(S(9, 15, 12, 12, 2)), sub(S(15, 15, 12, 12, 2))};
        case Icon::Restore:
            return {A(13, 12, 8, 2, 270, 300), P({1.5, 10.5, 8.5, 10.5, 5, 15}), S(13, 7.5, 13, 12.5),
                    S(13, 12.5, 16, 14.5)};
        case Icon::Mount:
            return {BS(3, 13, 18, 7, 2), D(16.5, 16.5, 1.3), S(6.5, 16.5, 11, 16.5), S(4.5, 13, 7.5, 5), S(19.5, 13, 16.5, 5),
                    S(7.5, 5, 16.5, 5)};
        case Icon::Folder:
            return {B(2, 7, 20, 13.5, 2), B(2, 4, 8.5, 5, 1.5), P({8, 4, 10.5, 4, 12.5, 7.5, 8, 7.5})};
        case Icon::File:
            return {BS(5, 2.5, 14, 19, 2), S(8.5, 8, 15.5, 8, 1.8), S(8.5, 12, 15.5, 12, 1.8), S(8.5, 16, 13, 16, 1.8)};
        case Icon::Zip:
            return {BS(5, 2.5, 14, 19, 2), B(10, 3.5, 2, 2, 0.3), B(12, 5.5, 2, 2, 0.3), B(10, 7.5, 2, 2, 0.3),
                    B(12, 9.5, 2, 2, 0.3), B(9.8, 12, 4.4, 5.5, 1), sub(B(11, 14.6, 2, 1.6, 0.3))};
        case Icon::Image:
            return {BS(3, 4, 18, 16, 2), D(8.5, 9, 1.9), P({5, 18, 10, 12, 13, 15, 15.5, 12.5, 19, 18})};
        case Icon::Terminal:
            return {BS(2.5, 4, 19, 16, 2.5), S(6.5, 9.5, 9.5, 12), S(9.5, 12, 6.5, 14.5), S(12, 15, 17, 15)};
        case Icon::Advanced:
            return {S(4, 6, 20, 6), S(4, 12, 20, 12), S(4, 18, 20, 18), D(9, 6, 2.7), D(15, 12, 2.7), D(7.5, 18, 2.7)};
        case Icon::Settings: {
            Def d{D(12, 12, 7.2)};
            for (int k = 0; k < 8; k++) {
                float a = k * 3.14159265f / 4;
                d.push_back(S(12 + std::cos(a) * 6.5f, 12 + std::sin(a) * 6.5f, 12 + std::cos(a) * 9.6f,
                              12 + std::sin(a) * 9.6f, 3.4f));
            }
            d.push_back(sub(D(12, 12, 3.2)));
            return d;
        }
        case Icon::Power: return {A(12, 13, 7.8, 2.2, 35, 290), S(12, 3, 12, 11, 2.2)};
        case Icon::Reboot:
        case Icon::Refresh:
            return {A(12, 12, 7.5, 2.2, 45, 295), P({9.6, 1.4, 15, 4.8, 9.6, 8.4})};
        case Icon::Back: return {S(15, 5, 8, 12, 2.4), S(8, 12, 15, 19, 2.4)};
        case Icon::Forward: return {S(9, 5, 16, 12, 2.4), S(16, 12, 9, 19, 2.4)};
        case Icon::Home:
            return {S(3, 11.5, 12, 3.5), S(12, 3.5, 21, 11.5), S(5.5, 10, 5.5, 20), S(5.5, 20, 18.5, 20),
                    S(18.5, 20, 18.5, 10), B(10, 13.5, 4, 6.5, 1)};
        case Icon::Console:
            return {BS(3, 4, 18, 16, 2), S(7, 9, 17, 9, 1.8), S(7, 12.5, 17, 12.5, 1.8), S(7, 16, 12.5, 16, 1.8)};
        case Icon::Check: return {S(5, 12.5, 10, 17.5, 2.6), S(10, 17.5, 19, 7, 2.6)};
        case Icon::Close: return {S(6, 6, 18, 18, 2.4), S(18, 6, 6, 18, 2.4)};
        case Icon::Lock:
            return {B(5, 10.5, 14, 11, 2.5), A(12, 8.5, 4, 2, 270, 180), S(8, 8.5, 8, 10.5), S(16, 8.5, 16, 10.5),
                    sub(D(12, 15, 1.6)), sub(S(12, 15, 12, 18, 1.4))};
        case Icon::Battery: return {BS(2.5, 7, 17, 10, 2.2), B(20.2, 10, 2, 4, 0.8)};
        case Icon::Thermo:
            return {D(12, 17.5, 3.9), BS(9.8, 2.8, 4.4, 12.5, 2.2, 1.8), S(12, 8, 12, 16, 2)};
        case Icon::Cpu:
        case Icon::Chip: {
            Def d{BS(6, 6, 12, 12, 2), B(9.5, 9.5, 5, 5, 1)};
            for (float p : {9.f, 12.f, 15.f}) {
                d.push_back(S(p, 2.5, p, 5, 1.8));
                d.push_back(S(p, 19, p, 21.5, 1.8));
                d.push_back(S(2.5, p, 5, p, 1.8));
                d.push_back(S(19, p, 21.5, p, 1.8));
            }
            return d;
        }
        case Icon::Memory:
            return {BS(2.5, 6.5, 19, 10, 1.5), B(5.5, 9.5, 3, 4, 0.5), B(10.5, 9.5, 3, 4, 0.5), B(15.5, 9.5, 3, 4, 0.5),
                    S(6, 17, 6, 20, 1.8), S(10, 17, 10, 20, 1.8), S(14, 17, 14, 20, 1.8), S(18, 17, 18, 20, 1.8)};
        case Icon::Sd:
            return {P({8, 2.5, 18, 2.5, 18, 21.5, 6, 21.5, 6, 6.5}), sub(S(10, 5.5, 10, 8.5, 1.5)),
                    sub(S(12.6, 5.5, 12.6, 8.5, 1.5)), sub(S(15.2, 5.5, 15.2, 8.5, 1.5))};
        case Icon::Usb:
            return {S(12, 5, 12, 18.5), D(12, 19.5, 2.4), S(12, 14, 7.5, 11, 1.8), S(7.5, 11, 7.5, 8, 1.8),
                    B(6, 5.5, 3, 3, 0.4), S(12, 15.5, 16.5, 12.5, 1.8), S(16.5, 12.5, 16.5, 9.5, 1.8), D(16.5, 8.5, 1.6),
                    P({9.5, 6, 14.5, 6, 12, 2})};
        case Icon::Up: return {S(12, 19, 12, 5), S(6, 11, 12, 5), S(18, 11, 12, 5)};
        case Icon::Down: return {S(12, 5, 12, 19), S(6, 13, 12, 19), S(18, 13, 12, 19)};
        case Icon::Sort: return {S(4, 7, 20, 7), S(4, 12, 15, 12), S(4, 17, 10, 17)};
        case Icon::Plus: return {S(12, 5, 12, 19, 2.2), S(5, 12, 19, 12, 2.2)};
        case Icon::Trash:
            return {S(4, 6.5, 20, 6.5), S(9.5, 3.5, 14.5, 3.5), S(6, 6.5, 7, 20.5), S(7, 20.5, 17, 20.5), S(17, 20.5, 18, 6.5),
                    S(10, 10, 10, 17, 1.8), S(14, 10, 14, 17, 1.8)};
        case Icon::Copy: return {BS(8.5, 7.5, 11.5, 13.5, 2), S(4.5, 15.5, 4.5, 4), S(4.5, 4, 15, 4)};
        case Icon::Move: return {S(3, 12, 15, 12), S(11, 8, 15, 12), S(15, 12, 11, 16), S(19.5, 4, 19.5, 20)};
        case Icon::Edit:
            return {S(6.5, 17.5, 15.5, 8.5, 3.4), P({3.2, 20.8, 4.4, 16.2, 7.8, 19.6}), S(17.5, 6.5, 18.6, 5.4, 3.4)};
        case Icon::Info: return {R(12, 12, 9), S(12, 11, 12, 16.5, 2.2), D(12, 7.8, 1.3)};
        case Icon::Warning:
            return {S(12, 3.5, 21, 19.5), S(21, 19.5, 3, 19.5), S(3, 19.5, 12, 3.5), S(12, 9.5, 12, 14, 2.2),
                    D(12, 16.8, 1.2)};
        case Icon::Sun: {
            Def d{D(12, 12, 4.2)};
            for (int k = 0; k < 8; k++) {
                float a = k * 3.14159265f / 4;
                d.push_back(S(12 + std::cos(a) * 7, 12 + std::sin(a) * 7, 12 + std::cos(a) * 9.5f,
                              12 + std::sin(a) * 9.5f, 2));
            }
            return d;
        }
        case Icon::Palette:
            return {D(12, 12, 9.5), sub(D(7.5, 10, 1.7)), sub(D(11, 6.5, 1.7)), sub(D(15.5, 8, 1.7)),
                    sub(D(15, 15.5, 2.6))};
        case Icon::Timer:
            return {R(12, 13.5, 7.5), S(12, 13.5, 12, 9.5), S(9.5, 2.5, 14.5, 2.5), S(18.2, 6.2, 19.5, 4.9)};
        case Icon::Vibrate:
            return {BS(8, 4, 8, 16, 1.5), S(4.5, 8, 4.5, 16, 1.8), S(19.5, 8, 19.5, 16, 1.8), S(1.8, 10, 1.8, 14, 1.8),
                    S(22.2, 10, 22.2, 14, 1.8)};
        case Icon::Key:
            return {R(7.5, 12, 4, 2.2), S(11.5, 12, 21, 12, 2.2), S(18, 12, 18, 15.5, 2.2), S(15, 12, 15, 14.5, 2.2)};
        case Icon::Bolt: return {P({13.5, 2, 5, 13.5, 11, 13.5, 10, 22, 19, 9.5, 13, 9.5})};
        case Icon::Shield:
            return {P({12, 2.5, 20, 5.5, 20, 11, 17, 17, 12, 21.5, 7, 17, 4, 11, 4, 5.5}), sub(S(8.5, 12, 11, 14.5, 1.9)),
                    sub(S(11, 14.5, 15.5, 9.5, 1.9))};
        case Icon::Chart:
            return {S(4, 4, 4, 20), S(4, 20, 20, 20), S(7.5, 15, 11, 10.5), S(11, 10.5, 14, 13.5), S(14, 13.5, 19, 7)};
        case Icon::Swap:
            return {S(4, 8, 19, 8), S(15, 4, 19, 8), S(19, 8, 15, 12), S(20, 16, 5, 16), S(9, 12, 5, 16), S(5, 16, 9, 20)};
        case Icon::Search: return {R(10.5, 10.5, 6, 2.2), S(15, 15, 20, 20, 2.6)};
        case Icon::Phone: return {BS(6.5, 2.5, 11, 19, 2.5), S(10.5, 18.5, 13.5, 18.5, 1.8)};
        case Icon::Storage:
            return {BS(3, 4, 18, 6.5, 1.5), BS(3, 13.5, 18, 6.5, 1.5), D(7, 7.25, 1.2), D(7, 16.75, 1.2)};
        case Icon::Hash: return {S(10, 4, 8, 20), S(16, 4, 14, 20), S(4.5, 9, 20, 9), S(4, 15, 19.5, 15)};
        case Icon::Touch: return {D(12, 12, 2.6), R(12, 12, 6, 1.8), R(12, 12, 9.6, 1.4)};
        case Icon::Camera: return {BS(2.5, 7, 19, 13.5, 2.5), R(12, 13.75, 3.8), B(8, 4, 8, 4, 1.5)};
        case Icon::Keyboard: {
            Def d{BS(2, 5.5, 20, 13, 2)};
            for (int k = 0; k < 4; k++) d.push_back(B(5 + k * 3.7f, 8.5, 2.2, 2, 0.4));
            d.push_back(S(8, 15, 16, 15, 2));
            return d;
        }
        case Icon::Moon: return {D(12, 12, 9), sub(D(16.5, 8, 7.5))};
        case Icon::Wrench:
            return {S(4.5, 19.5, 12.5, 11.5, 3.4), A(15.5, 8.5, 4.6, 2.8, 150, 270)};
        case Icon::Eye:
            return {P({2, 12, 7, 6.5, 12, 5, 17, 6.5, 22, 12, 17, 17.5, 12, 19, 7, 17.5}), sub(D(12, 12, 4.4)),
                    D(12, 12, 2.4)};
        default: return {};
    }
}

struct Mask {
    int size = 0;
    std::vector<uint8_t> a;
};

std::map<std::pair<int, int>, Mask> g_cache;

const Mask& mask(Icon icon, int size) {
    auto key = std::make_pair((int)icon, size);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second;
    Mask m;
    m.size = size;
    m.a.assign(size_t(size) * size, 0);
    Def def = define(icon);
    float unit = 24.0f / size;
    float pxPerUnit = size / 24.0f;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float px = (x + 0.5f) * unit, py = (y + 0.5f) * unit;
            float d = 1e9f;
            for (auto& s : def) {
                float sd = sdShape(px, py, s);
                d = s.sub ? std::fmax(d, -sd) : std::fmin(d, sd);
            }
            float cov = 0.5f - d * pxPerUnit;
            m.a[size_t(y) * size + x] = (uint8_t)(cov <= 0 ? 0 : cov >= 1 ? 255 : cov * 255);
        }
    }
    return g_cache.emplace(key, std::move(m)).first->second;
}

}  // namespace

void draw(Canvas& c, Icon icon, float x, float y, float size, Color col) {
    if (icon == Icon::None) return;
    int s = std::max(8, (int)std::lround(size));
    const Mask& m = mask(icon, s);
    c.drawMask(m.a.data(), s, s, s, x, y, col);
}

}  // namespace tw::icons
