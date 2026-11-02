#include "ui/theme.h"

namespace tw {

const AccentDef kAccents[] = {
    {"Teal", rgb(0x2DD4BF), rgb(0x22D3EE)},   {"Violet", rgb(0xA78BFA), rgb(0xF472B6)},
    {"Ocean", rgb(0x60A5FA), rgb(0x818CF8)},  {"Sunset", rgb(0xFB923C), rgb(0xF43F5E)},
    {"Lime", rgb(0xA3E635), rgb(0x34D399)},   {"Rose", rgb(0xFB7185), rgb(0xE879F9)},
    {"Amber", rgb(0xFBBF24), rgb(0xF97316)},  {"Ice", rgb(0x7DD3FC), rgb(0xC4B5FD)},
};
const int kAccentCount = sizeof(kAccents) / sizeof(kAccents[0]);
const char* const kStyles[] = {"Midnight", "AMOLED", "Aurora", "Light"};
const int kStyleCount = 4;

Theme makeTheme(int accent, int style) {
    if (accent < 0 || accent >= kAccentCount) accent = 0;
    const AccentDef& a = kAccents[accent];
    Theme t;
    t.accent = a.a;
    t.accent2 = a.b;
    t.danger = rgb(0xF87171);
    t.warn = rgb(0xFBBF24);
    t.ok = rgb(0x4ADE80);
    t.info = rgb(0x60A5FA);
    switch (style) {
        case 1:  // AMOLED
            t.bgTop = rgb(0x000000);
            t.bgBottom = rgb(0x000000);
            t.glows = false;
            t.surface = rgb(0x0E0E10);
            t.surface2 = rgb(0x17171A);
            t.surfaceHi = rgb(0x222227);
            t.outline = rgb(0x26262C);
            break;
        case 2:  // Aurora
            t.bgTop = rgb(0x0A0618);
            t.bgBottom = rgb(0x050B14);
            t.glow1 = withAlpha(a.a, 0.30f);
            t.glow2 = withAlpha(a.b, 0.26f);
            t.surface = rgba(255, 255, 255, 14);
            t.surface2 = rgba(255, 255, 255, 22);
            t.surfaceHi = rgba(255, 255, 255, 34);
            t.outline = rgba(255, 255, 255, 26);
            break;
        case 3:  // Light
            t.light = true;
            t.bgTop = rgb(0xF4F6FB);
            t.bgBottom = rgb(0xE9EDF5);
            t.glow1 = withAlpha(a.a, 0.18f);
            t.glow2 = withAlpha(a.b, 0.12f);
            t.surface = rgb(0xFFFFFF);
            t.surface2 = rgb(0xF1F3F8);
            t.surfaceHi = rgb(0xE4E8F0);
            t.outline = rgb(0xD9DEE8);
            t.text = rgb(0x0F172A);
            t.textDim = rgb(0x475569);
            t.textFaint = rgb(0x94A3B8);
            t.accent = darken(a.a, 0.25f);
            t.accent2 = darken(a.b, 0.2f);
            t.accentSoft = withAlpha(t.accent, 0.14f);
            t.onAccent = rgb(0xFFFFFF);
            t.danger = rgb(0xDC2626);
            t.warn = rgb(0xD97706);
            t.ok = rgb(0x16A34A);
            return t;
        default:  // Midnight
            t.bgTop = rgb(0x0B1222);
            t.bgBottom = rgb(0x070B16);
            t.glow1 = withAlpha(a.a, 0.16f);
            t.glow2 = withAlpha(a.b, 0.10f);
            t.surface = rgb(0x111A2E);
            t.surface2 = rgb(0x17223A);
            t.surfaceHi = rgb(0x1F2C48);
            t.outline = rgb(0x22304D);
            break;
    }
    t.text = rgb(0xF1F5F9);
    t.textDim = rgb(0xA3B1C6);
    t.textFaint = rgb(0x64748B);
    t.accentSoft = withAlpha(a.a, 0.16f);
    t.onAccent = rgb(0x05131A);
    return t;
}

}  // namespace tw
