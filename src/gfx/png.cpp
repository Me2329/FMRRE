#include "gfx/png.h"
#include "core/util.h"
#include "miniz.h"

#include <vector>

namespace tw {

bool savePng(const Canvas& c, const std::string& path) {
    std::vector<uint8_t> rgb(size_t(c.width()) * c.height() * 3);
    const uint32_t* p = c.pixels();
    for (size_t i = 0, n = size_t(c.width()) * c.height(); i < n; i++) {
        rgb[i * 3] = (p[i] >> 16) & 255;
        rgb[i * 3 + 1] = (p[i] >> 8) & 255;
        rgb[i * 3 + 2] = p[i] & 255;
    }
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), c.width(), c.height(), 3, &len, 6, MZ_FALSE);
    if (!png) return false;
    bool ok = util::writeFile(path, std::string((const char*)png, len));
    mz_free(png);
    return ok;
}

}  // namespace tw
