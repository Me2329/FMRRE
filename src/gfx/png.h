#pragma once
#include <string>
#include "gfx/canvas.h"

namespace tw {
// Save a canvas as PNG (used for screenshots: VolDown+Power or Advanced > Screenshot).
bool savePng(const Canvas& c, const std::string& path);
}
