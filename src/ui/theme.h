#pragma once
#include "gfx/canvas.h"

namespace tw {

struct Theme {
    Color bgTop, bgBottom, glow1, glow2;
    bool glows = true;
    Color surface, surface2, surfaceHi, outline;
    Color text, textDim, textFaint;
    Color accent, accent2, accentSoft, onAccent;
    Color danger, warn, ok, info;
    bool light = false;
};

struct AccentDef {
    const char* name;
    Color a, b;
};

extern const AccentDef kAccents[];
extern const int kAccentCount;
extern const char* const kStyles[];
extern const int kStyleCount;

Theme makeTheme(int accent, int style);

}  // namespace tw
