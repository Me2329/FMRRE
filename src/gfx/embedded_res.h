#pragma once
#include <cstddef>

namespace tw::res {
struct Blob { const unsigned char* data; size_t size; };
extern const Blob font_regular;
extern const Blob font_semibold;
extern const Blob font_bold;
extern const Blob font_mono;
}
