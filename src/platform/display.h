#pragma once
#include <memory>
#include <string>
#include "gfx/canvas.h"
#include "platform/input.h"

namespace tw {

// Output backend. On a phone this is DRM/KMS (modern devices) or the legacy
// fbdev (/dev/graphics/fb0). The simulator backends exist for development.
class Display {
public:
    virtual ~Display() = default;
    virtual bool init() = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual void present(const Canvas& c) = 0;
    virtual void setBlank(bool blank) { (void)blank; }
    virtual const char* name() const = 0;
    // Backends that are also input sources (X11 window) push events here.
    virtual void pumpEvents(InputQueue& q) { (void)q; }
    void setRotate180(bool r) { rotate180_ = r; }

protected:
    bool rotate180_ = false;
};

std::unique_ptr<Display> createDrmDisplay();
std::unique_ptr<Display> createFbDisplay();
std::unique_ptr<Display> createHeadlessDisplay(int w, int h);
#ifdef TW_HAVE_X11
std::unique_ptr<Display> createX11Display(int w, int h);
#endif

// Pick the best available hardware backend: DRM first, then fbdev.
std::unique_ptr<Display> openHardwareDisplay();

}  // namespace tw
