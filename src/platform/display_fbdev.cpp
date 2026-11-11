// Legacy framebuffer backend (/dev/graphics/fb0) with double buffering via
// panning when the driver supports it. Handles 32bpp in any channel order
// and RGB565.
#include "platform/display.h"
#include "core/log.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>

namespace tw {

namespace {

class FbDisplay : public Display {
public:
    ~FbDisplay() override {
        if (map_) munmap(map_, mapLen_);
        if (fd_ >= 0) close(fd_);
    }

    bool init() override {
        const char* paths[] = {"/dev/graphics/fb0", "/dev/fb0"};
        for (auto p : paths) {
            fd_ = open(p, O_RDWR | O_CLOEXEC);
            if (fd_ >= 0) break;
        }
        if (fd_ < 0) return false;
        if (ioctl(fd_, FBIOGET_VSCREENINFO, &vi_) != 0 || ioctl(fd_, FBIOGET_FSCREENINFO, &fi_) != 0) return false;
        if (vi_.bits_per_pixel != 32 && vi_.bits_per_pixel != 16) {
            LOGW("fbdev: unsupported bpp %u", vi_.bits_per_pixel);
            return false;
        }
        // Try to enable double buffering.
        fb_var_screeninfo v = vi_;
        v.yres_virtual = vi_.yres * 2;
        v.yoffset = 0;
        v.activate = FB_ACTIVATE_NOW;
        if (ioctl(fd_, FBIOPUT_VSCREENINFO, &v) == 0) {
            ioctl(fd_, FBIOGET_VSCREENINFO, &vi_);
            ioctl(fd_, FBIOGET_FSCREENINFO, &fi_);
        }
        mapLen_ = fi_.smem_len;
        void* p = mmap(nullptr, mapLen_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) return false;
        map_ = (uint8_t*)p;
        doubleBuf_ = vi_.yres_virtual >= vi_.yres * 2 && mapLen_ >= size_t(fi_.line_length) * vi_.yres * 2;
        LOGI("fbdev: %ux%u %ubpp r%u g%u b%u line=%u %s", vi_.xres, vi_.yres, vi_.bits_per_pixel, vi_.red.offset,
             vi_.green.offset, vi_.blue.offset, fi_.line_length, doubleBuf_ ? "double-buffered" : "single");
        ro_ = vi_.red.offset, go_ = vi_.green.offset, bo_ = vi_.blue.offset;
        if (vi_.bits_per_pixel == 32 && (ro_ == go_ || go_ == bo_ || ro_ == bo_ || ro_ > 24 || go_ > 24 || bo_ > 24)) {
            // A driver that doesn't say where the channels are: BGRA is the usual layout.
            LOGW("fbdev: no usable channel layout reported - assuming BGRA (Settings > Swap red and blue if colours are wrong)");
            ro_ = 16, go_ = 8, bo_ = 0;
        }
        ioctl(fd_, FBIOBLANK, FB_BLANK_UNBLANK);
        return true;
    }

    int width() const override { return vi_.xres; }
    int height() const override { return vi_.yres; }
    const char* name() const override { return "fbdev"; }

    void present(const Canvas& c) override {
        int page = doubleBuf_ ? back_ : 0;
        uint8_t* base = map_ + size_t(page) * vi_.yres * fi_.line_length;
        int w = std::min(c.width(), (int)vi_.xres), h = std::min(c.height(), (int)vi_.yres);
        uint32_t ro = swapRB_ ? bo_ : ro_, bo = swapRB_ ? ro_ : bo_, go = go_;
        bool native = vi_.bits_per_pixel == 32 && ro == 16 && go == 8 && bo == 0;
        for (int y = 0; y < h; y++) {
            int sy = rotate180_ ? c.height() - 1 - y : y;
            const uint32_t* s = c.pixels() + size_t(sy) * c.width();
            uint8_t* row = base + size_t(y) * fi_.line_length;
            if (vi_.bits_per_pixel == 32) {
                uint32_t* d = (uint32_t*)row;
                if (native && !rotate180_) {
                    memcpy(d, s, size_t(w) * 4);
                    continue;
                }
                for (int x = 0; x < w; x++) {
                    uint32_t p = s[rotate180_ ? c.width() - 1 - x : x];
                    uint32_t r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
                    d[x] = (r << ro) | (g << go) | (b << bo) | (vi_.transp.length ? (255u << vi_.transp.offset) : 0);
                }
            } else {
                uint16_t* d = (uint16_t*)row;
                for (int x = 0; x < w; x++) {
                    uint32_t p = s[rotate180_ ? c.width() - 1 - x : x];
                    if (swapRB_) p = (p & 0xFF00FF00u) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
                    d[x] = uint16_t(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
                }
            }
        }
        if (doubleBuf_) {
            vi_.yoffset = page * vi_.yres;
            vi_.activate = FB_ACTIVATE_VBL;
            if (ioctl(fd_, FBIOPAN_DISPLAY, &vi_) != 0) {
                vi_.activate = FB_ACTIVATE_NOW;
                ioctl(fd_, FBIOPUT_VSCREENINFO, &vi_);
            }
            back_ ^= 1;
        } else {
            // One buffer: some drivers (command-mode panels, older Exynos / MSM fbdev) only send it to the panel when
            // asked. Harmless where the panel refreshes by itself.
            vi_.yoffset = 0;
            vi_.activate = FB_ACTIVATE_VBL;
            ioctl(fd_, FBIOPAN_DISPLAY, &vi_);
        }
    }

    void setBlank(bool blank) override { ioctl(fd_, FBIOBLANK, blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK); }

private:
    int fd_ = -1;
    fb_var_screeninfo vi_{};
    fb_fix_screeninfo fi_{};
    uint8_t* map_ = nullptr;
    size_t mapLen_ = 0;
    bool doubleBuf_ = false;
    int back_ = 1;
    uint32_t ro_ = 16, go_ = 8, bo_ = 0;  // where the driver keeps red / green / blue in a 32-bit pixel
};

class HeadlessDisplay : public Display {
public:
    HeadlessDisplay(int w, int h) : w_(w), h_(h) {}
    bool init() override { return true; }
    int width() const override { return w_; }
    int height() const override { return h_; }
    const char* name() const override { return "headless"; }
    void present(const Canvas&) override {}

private:
    int w_, h_;
};

}  // namespace

std::unique_ptr<Display> createFbDisplay() { return std::make_unique<FbDisplay>(); }
std::unique_ptr<Display> createHeadlessDisplay(int w, int h) { return std::make_unique<HeadlessDisplay>(w, h); }

}  // namespace tw
