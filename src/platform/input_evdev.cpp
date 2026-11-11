#include "platform/input.h"
#include "core/log.h"
#include "core/util.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>

namespace tw {

#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1) / BITS_PER_LONG) + 1)
static bool testBit(const unsigned long* arr, int bit) { return (arr[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1; }

bool EvdevInput::open(int screenW, int screenH) {
    w_ = screenW;
    h_ = screenH;
    DIR* d = opendir("/dev/input");
    if (!d) {
        LOGW("input: /dev/input not available");
        return false;
    }
    while (auto* e = readdir(d)) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        std::string path = std::string("/dev/input/") + e->d_name;
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        Dev dev;
        dev.fd = fd;
        char name[128] = "?";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        dev.name = name;
        unsigned long evbits[NBITS(EV_MAX)] = {0};
        unsigned long absbits[NBITS(ABS_MAX)] = {0};
        unsigned long keybits[NBITS(KEY_MAX)] = {0};
        unsigned long props[NBITS(INPUT_PROP_MAX)] = {0};
        ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
        // A touchpad (pointer, not on the screen) is not a touch screen. Old kernels can't say: assume a screen.
        bool pointer = ioctl(fd, EVIOCGPROP(sizeof(props)), props) >= 0 && testBit(props, INPUT_PROP_POINTER) &&
                       !testBit(props, INPUT_PROP_DIRECT);
        if (testBit(evbits, EV_ABS) && !pointer) {
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
            struct input_absinfo ax, ay;
            if (testBit(absbits, ABS_MT_POSITION_X) && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) == 0 &&
                ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) == 0) {
                dev.touch = dev.mt = true;
                struct input_absinfo slot;
                if (testBit(absbits, ABS_MT_SLOT) && ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slot) == 0) dev.slot = slot.value;
            } else if (testBit(absbits, ABS_X) && testBit(keybits, BTN_TOUCH) && ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 &&
                       ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0) {
                dev.touch = true;  // X/Y axes alone are not enough: accelerometers and joysticks have them too
            }
            if (dev.touch) {
                dev.minX = ax.minimum; dev.maxX = ax.maximum;
                dev.minY = ay.minimum; dev.maxY = ay.maximum;
                if (dev.maxX <= dev.minX || dev.maxY <= dev.minY) {  // a driver that doesn't give its range
                    LOGW("input: %s reports no coordinate range - assuming the screen's", name);
                    dev.minX = dev.minY = 0;
                    dev.maxX = screenW - 1;
                    dev.maxY = screenH - 1;
                }
            }
        }
        // Samsung kernels keep an input device switched off until its "enabled" attribute is set (Android's input
        // service does that; a stock recovery without touch never does).
        std::string enabled = std::string("/sys/class/input/") + e->d_name + "/device/enabled";
        FILE* ef = fopen(enabled.c_str(), "re");
        if (ef) {
            int c = fgetc(ef);
            fclose(ef);
            if (c == '0' && (ef = fopen(enabled.c_str(), "we"))) {
                bool ok = fputc('1', ef) != EOF;
                ok = fclose(ef) == 0 && ok;
                LOGI("input: %s was switched off - %s", name, ok ? "switched on" : "could not switch it on");
            }
        }
        LOGI("input: %s '%s'%s", path.c_str(), name, dev.touch ? (dev.mt ? " [multitouch]" : " [touch]") : "");
        devs_.push_back(dev);
    }
    closedir(d);
    if (!hasTouch()) LOGW("input: no touch screen found - the volume keys move the focus, Power selects");
    return !devs_.empty();
}

bool EvdevInput::hasTouch() const {
    for (auto& d : devs_)
        if (d.touch) return true;
    return false;
}

int EvdevInput::addFake(bool multitouch, int maxX, int maxY, int screenW, int screenH) {
    w_ = screenW;
    h_ = screenH;
    Dev d;
    d.name = "fake";
    d.touch = true;
    d.mt = multitouch;
    d.maxX = maxX;
    d.maxY = maxY;
    devs_.push_back(d);
    return (int)devs_.size() - 1;
}

void EvdevInput::feed(int dev, int type, int code, int value, InputQueue& q) { handle(devs_[size_t(dev)], type, code, value, q); }

std::vector<std::string> EvdevInput::deviceNames() const {
    std::vector<std::string> v;
    for (auto& d : devs_) v.push_back(d.name + (d.touch ? " (touch)" : ""));
    return v;
}

static Key mapKey(int code) {
    switch (code) {
        case KEY_VOLUMEUP: return Key::VolUp;
        case KEY_VOLUMEDOWN: return Key::VolDown;
        case KEY_POWER: return Key::Power;
        case KEY_BACK: case KEY_ESC: return Key::Back;
        case 0x2bf: return Key::Back;  // KEY_WINK in Samsung's kernels: the Bixby key (S8 - Note 10 have no Back key)
        case KEY_HOME: case KEY_HOMEPAGE: return Key::Home;
        case KEY_MENU: return Key::Menu;
        case KEY_ENTER: case KEY_OK: case KEY_SELECT: return Key::Enter;
        case KEY_BACKSPACE: return Key::Backspace;
        case KEY_UP: return Key::Up;
        case KEY_DOWN: return Key::Down;
        case KEY_LEFT: return Key::Left;
        case KEY_RIGHT: return Key::Right;
        default: return Key::None;
    }
}

void EvdevInput::handle(Dev& d, int type, int code, int value, InputQueue& q) {
    // Keys: everything below the BTN_* block (mouse, joystick and touch "buttons"), and the keys after it (KEY_OK,
    // KEY_SELECT, Samsung's Bixby key...).
    if (type == EV_KEY && (code < BTN_MISC || (code >= KEY_OK && code < BTN_TRIGGER_HAPPY))) {
        InputEvent e{value ? EvType::KeyDown : EvType::KeyUp};
        if (value == 2) return;  // autorepeat
        e.key = mapKey(code);
        e.rawCode = code;
        q.push(e);
        return;
    }
    if (!d.touch) return;
    if (type == EV_KEY && code == BTN_TOUCH) {
        if (!d.mt) d.down = value != 0;
        else if (value == 0) { d.down = false; d.primary = -1; }  // multitouch: "no finger left" is always true
        return;
    }
    if (type == EV_ABS) {
        // Protocol B follows one finger: the first one down, whatever slot the driver gives it (not always 0).
        // Protocol A has no slots: the first contact of each frame is the one followed.
        bool mine = d.protoA ? d.contacts == 0 : (d.primary < 0 || d.slot == d.primary);
        switch (code) {
            case ABS_MT_SLOT: d.slot = value; break;
            case ABS_MT_TRACKING_ID:
                if (value >= 0 && d.primary < 0) {
                    d.primary = d.slot;
                    d.down = true;
                } else if (value < 0 && d.slot == d.primary) {
                    d.primary = -1;
                    d.down = false;
                }
                break;
            case ABS_MT_POSITION_X: if (mine) { d.curX = value; d.moved = true; } break;
            case ABS_MT_POSITION_Y: if (mine) { d.curY = value; d.moved = true; } break;
            case ABS_MT_TOUCH_MAJOR:
                if (mine && d.mt && value == 0) d.down = false;  // protocol A: a contact of size 0 is a lift
                break;
            case ABS_X: if (!d.mt) { d.curX = value; d.moved = true; } break;
            case ABS_Y: if (!d.mt) { d.curY = value; d.moved = true; } break;
        }
        return;
    }
    if (type == EV_SYN && code == SYN_MT_REPORT && d.mt) {
        // Protocol A: each contact ends with this; one that came with coordinates is a finger on the glass.
        d.protoA = true;
        if (d.moved && d.contacts == 0) d.down = true;
        if (d.moved) d.contacts++;
        return;
    }
    if (type == EV_SYN && code == SYN_REPORT) {
        if (d.protoA) {
            if (d.contacts == 0) d.down = false;  // a frame without contacts: every finger is up
            d.contacts = 0;
        }
        float fx = d.maxX > d.minX ? float(d.curX - d.minX) / (d.maxX - d.minX) : 0;
        float fy = d.maxY > d.minY ? float(d.curY - d.minY) / (d.maxY - d.minY) : 0;
        if (swap_) std::swap(fx, fy);
        if (flipX_) fx = 1 - fx;
        if (flipY_) fy = 1 - fy;
        int x = int(fx * (w_ - 1) + 0.5f), y = int(fy * (h_ - 1) + 0.5f);
        if (d.down && !d.wasDown) q.push({EvType::TouchDown, x, y});
        else if (d.down && d.moved) q.push({EvType::TouchMove, x, y});
        else if (!d.down && d.wasDown) q.push({EvType::TouchUp, x, y});
        d.wasDown = d.down;
        d.moved = false;
    }
}

void EvdevInput::poll(InputQueue& q, int timeoutMs) {
    if (devs_.empty()) {
        if (timeoutMs > 0) usleep(timeoutMs * 1000);
        return;
    }
    std::vector<pollfd> pfds;
    for (auto& d : devs_) pfds.push_back({d.fd, POLLIN, 0});
    int r = ::poll(pfds.data(), pfds.size(), timeoutMs);
    if (r <= 0) return;
    for (size_t i = 0; i < devs_.size(); i++) {
        if (!(pfds[i].revents & POLLIN)) continue;
        struct input_event ev[64];
        ssize_t n;
        while ((n = read(devs_[i].fd, ev, sizeof(ev))) > 0) {
            for (size_t k = 0; k < size_t(n) / sizeof(ev[0]); k++) handle(devs_[i], ev[k].type, ev[k].code, ev[k].value, q);
        }
    }
}

}  // namespace tw
