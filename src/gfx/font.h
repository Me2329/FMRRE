#pragma once
#include <string>
#include "gfx/canvas.h"

namespace tw {

enum class Face { Regular = 0, SemiBold, Bold, Mono, Count };

enum Align { AlignLeft = 0, AlignCenter = 1, AlignRight = 2 };

namespace font {
void init();
// Draw text with its line box's top-left at (x, y). Returns advance width.
float draw(Canvas& c, Face f, float px, float x, float y, const std::string& s, Color col);
float measure(Face f, float px, const std::string& s);
float lineHeight(Face f, float px);
// Draw inside rect: horizontal alignment + vertical centering. Ellipsizes if needed.
void drawIn(Canvas& c, Face f, float px, const Rect& r, const std::string& s, Color col, int align = AlignLeft);
std::string ellipsize(Face f, float px, const std::string& s, float maxW);
// Word-wrap into lines no wider than maxW.
std::vector<std::string> wrap(Face f, float px, const std::string& s, float maxW);
}  // namespace font

}  // namespace tw
