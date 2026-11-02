#include "gfx/font.h"
#include "gfx/embedded_res.h"

#include <cmath>
#include <unordered_map>
#include <vector>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma GCC diagnostic pop

namespace tw::font {

struct Glyph {
    int w = 0, h = 0, xoff = 0, yoff = 0;
    float advance = 0;
    std::vector<uint8_t> bmp;
};

struct FontData {
    stbtt_fontinfo info;
    int ascent = 0, descent = 0, lineGap = 0;
    bool ok = false;
};

static FontData g_fonts[(int)Face::Count];
static std::unordered_map<uint64_t, Glyph> g_cache;

void init() {
    const res::Blob* blobs[] = {&res::font_regular, &res::font_semibold, &res::font_bold, &res::font_mono};
    for (int i = 0; i < (int)Face::Count; i++) {
        FontData& f = g_fonts[i];
        const unsigned char* d = blobs[i]->data;
        if (stbtt_InitFont(&f.info, d, stbtt_GetFontOffsetForIndex(d, 0))) {
            stbtt_GetFontVMetrics(&f.info, &f.ascent, &f.descent, &f.lineGap);
            f.ok = true;
        }
    }
}

static uint32_t nextCp(const std::string& s, size_t& i) {
    unsigned char c = s[i++];
    if (c < 0x80) return c;
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    uint32_t cp = c & (0x3F >> n);
    for (int k = 0; k < n && i < s.size(); k++) cp = (cp << 6) | (s[i++] & 0x3F);
    return cp;
}

static const Glyph& glyph(Face f, int px, uint32_t cp) {
    uint64_t key = (uint64_t(f) << 48) | (uint64_t(px) << 32) | cp;
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second;
    Glyph g;
    FontData& fd = g_fonts[(int)f];
    if (fd.ok) {
        float scale = stbtt_ScaleForPixelHeight(&fd.info, (float)px);
        int idx = stbtt_FindGlyphIndex(&fd.info, cp);
        if (idx == 0 && cp != ' ') idx = stbtt_FindGlyphIndex(&fd.info, '?');
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&fd.info, idx, &adv, &lsb);
        g.advance = adv * scale;
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&fd.info, idx, scale, scale, &x0, &y0, &x1, &y1);
        g.w = x1 - x0;
        g.h = y1 - y0;
        g.xoff = x0;
        g.yoff = y0;
        if (g.w > 0 && g.h > 0) {
            g.bmp.resize(size_t(g.w) * g.h);
            stbtt_MakeGlyphBitmap(&fd.info, g.bmp.data(), g.w, g.h, g.w, scale, scale, idx);
        }
    }
    if (g_cache.size() > 6000) g_cache.clear();
    return g_cache.emplace(key, std::move(g)).first->second;
}

static float ascentPx(Face f, int px) {
    FontData& fd = g_fonts[(int)f];
    if (!fd.ok) return px * 0.8f;
    return fd.ascent * stbtt_ScaleForPixelHeight(&fd.info, (float)px);
}

float lineHeight(Face f, float px) {
    (void)f;
    return std::round(px * 1.25f);
}

float draw(Canvas& c, Face f, float fpx, float x, float y, const std::string& s, Color col) {
    int px = std::max(4, (int)std::lround(fpx));
    float base = std::round(y + (lineHeight(f, px) - px) / 2 + ascentPx(f, px));
    float pen = x;
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = nextCp(s, i);
        const Glyph& g = glyph(f, px, cp);
        if (!g.bmp.empty()) c.drawMask(g.bmp.data(), g.w, g.h, g.w, std::round(pen) + g.xoff, base + g.yoff, col);
        pen += g.advance;
    }
    return pen - x;
}

float measure(Face f, float fpx, const std::string& s) {
    int px = std::max(4, (int)std::lround(fpx));
    float w = 0;
    size_t i = 0;
    while (i < s.size()) w += glyph(f, px, nextCp(s, i)).advance;
    return w;
}

std::string ellipsize(Face f, float px, const std::string& s, float maxW) {
    if (measure(f, px, s) <= maxW) return s;
    std::string ell = "\xE2\x80\xA6";
    float ew = measure(f, px, ell);
    std::string out;
    size_t i = 0;
    float w = 0;
    while (i < s.size()) {
        size_t st = i;
        uint32_t cp = nextCp(s, i);
        (void)cp;
        std::string ch = s.substr(st, i - st);
        float cw = measure(f, px, ch);
        if (w + cw + ew > maxW) break;
        out += ch;
        w += cw;
    }
    return out + ell;
}

void drawIn(Canvas& c, Face f, float px, const Rect& r, const std::string& s, Color col, int align) {
    std::string t = ellipsize(f, px, s, r.w);
    float w = measure(f, px, t);
    float x = r.x;
    if (align == AlignCenter) x = r.x + (r.w - w) / 2;
    else if (align == AlignRight) x = r.r() - w;
    float y = r.y + (r.h - lineHeight(f, px)) / 2;
    draw(c, f, px, x, y, t, col);
}

std::vector<std::string> wrap(Face f, float px, const std::string& s, float maxW) {
    std::vector<std::string> lines;
    for (auto& para : [&] {
             std::vector<std::string> p;
             std::string cur;
             for (char ch : s) {
                 if (ch == '\n') { p.push_back(cur); cur.clear(); }
                 else cur += ch;
             }
             p.push_back(cur);
             return p;
         }()) {
        std::string line;
        std::string word;
        auto flushWord = [&]() {
            if (word.empty()) return;
            std::string cand = line.empty() ? word : line + " " + word;
            if (measure(f, px, cand) <= maxW || line.empty()) {
                line = cand;
            } else {
                lines.push_back(line);
                line = word;
            }
            // Hard-break very long words.
            while (measure(f, px, line) > maxW && line.size() > 1) {
                size_t cut = line.size();
                while (cut > 1 && measure(f, px, line.substr(0, cut)) > maxW) cut--;
                lines.push_back(line.substr(0, cut));
                line = line.substr(cut);
            }
            word.clear();
        };
        for (char ch : para) {
            if (ch == ' ') flushWord();
            else word += ch;
        }
        flushWord();
        lines.push_back(line);
    }
    return lines;
}

}  // namespace tw::font
