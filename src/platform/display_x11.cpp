// Desktop simulator window (host builds only). Mouse = touch,
// Up/Down/Enter = Vol+/Vol-/Power, Esc = Back, typing = text input.
#ifdef TW_HAVE_X11
#include "platform/display.h"
#include "core/log.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
// X11 macros that collide with our enums
#undef None
#undef Bool
#undef Status
#include <cstring>
#include <vector>

namespace tw {

namespace {

class X11Display : public Display {
public:
    X11Display(int w, int h) : w_(w), h_(h) {}
    ~X11Display() override {
        if (img_) { img_->data = nullptr; XDestroyImage(img_); }
        if (dpy_) XCloseDisplay(dpy_);
    }
    bool init() override {
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) return false;
        int scr = DefaultScreen(dpy_);
        win_ = XCreateSimpleWindow(dpy_, RootWindow(dpy_, scr), 0, 0, w_, h_, 0, 0, 0);
        XStoreName(dpy_, win_, "TWRP v2 - simulator");
        XSelectInput(dpy_, win_, ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask |
                                     KeyReleaseMask);
        XSizeHints hints{};
        hints.flags = PMinSize | PMaxSize;
        hints.min_width = hints.max_width = w_;
        hints.min_height = hints.max_height = h_;
        XSetWMNormalHints(dpy_, win_, &hints);
        XMapWindow(dpy_, win_);
        gc_ = XCreateGC(dpy_, win_, 0, nullptr);
        buf_.resize(size_t(w_) * h_);
        img_ = XCreateImage(dpy_, DefaultVisual(dpy_, scr), DefaultDepth(dpy_, scr), ZPixmap, 0, (char*)buf_.data(), w_,
                            h_, 32, 0);
        return img_ != nullptr;
    }
    int width() const override { return w_; }
    int height() const override { return h_; }
    const char* name() const override { return "x11-sim"; }
    void present(const Canvas& c) override {
        memcpy(buf_.data(), c.pixels(), buf_.size() * 4);
        XPutImage(dpy_, win_, gc_, img_, 0, 0, 0, 0, w_, h_);
        XFlush(dpy_);
    }
    void pumpEvents(InputQueue& q) override {
        while (XPending(dpy_)) {
            XEvent ev;
            XNextEvent(dpy_, &ev);
            switch (ev.type) {
                case ButtonPress:
                    if (ev.xbutton.button == 1) { down_ = true; q.push({EvType::TouchDown, ev.xbutton.x, ev.xbutton.y}); }
                    break;
                case ButtonRelease:
                    if (ev.xbutton.button == 1) { down_ = false; q.push({EvType::TouchUp, ev.xbutton.x, ev.xbutton.y}); }
                    break;
                case MotionNotify:
                    if (down_) q.push({EvType::TouchMove, ev.xmotion.x, ev.xmotion.y});
                    break;
                case KeyPress:
                case KeyRelease: {
                    char txt[8] = {0};
                    KeySym ks;
                    XLookupString(&ev.xkey, txt, sizeof(txt) - 1, &ks, nullptr);
                    Key k = Key::None;
                    switch (ks) {
                        case XK_Up: k = Key::VolUp; break;
                        case XK_Down: k = Key::VolDown; break;
                        case XK_Return: k = Key::Power; break;
                        case XK_Escape: k = Key::Back; break;
                        case XK_BackSpace: k = Key::Backspace; break;
                        case XK_F1: k = Key::Home; break;
                    }
                    bool press = ev.type == KeyPress;
                    if (k != Key::None) {
                        InputEvent e{press ? EvType::KeyDown : EvType::KeyUp};
                        e.key = k;
                        q.push(e);
                    } else if (press && txt[0] >= 32) {
                        InputEvent e{EvType::Text};
                        e.text = txt;
                        q.push(e);
                    }
                    break;
                }
            }
        }
    }

private:
    int w_, h_;
    ::Display* dpy_ = nullptr;
    Window win_ = 0;
    GC gc_{};
    XImage* img_ = nullptr;
    std::vector<uint32_t> buf_;
    bool down_ = false;
};

}  // namespace

std::unique_ptr<Display> createX11Display(int w, int h) { return std::make_unique<X11Display>(w, h); }

}  // namespace tw
#endif
