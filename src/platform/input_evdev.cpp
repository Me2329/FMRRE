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
        ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);
        if (testBit(evbits, EV_ABS)) {
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
            struct input_absinfo ax, ay;
            if (testBit(absbits, ABS_MT_POSITION_X) && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) == 0 &&
                ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) == 0) {
                dev.touch = dev.mt = true;
            } else if (testBit(absbits, ABS_X) && ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 &&
                       ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0) {
                dev.touch = true;
            }
            if (dev.touch) {
                dev.minX = ax.minimum; dev.maxX = ax.maximum;
                dev.minY = ay.minimum; dev.maxY = ay.maximum;
            }
        }
        LOGI("input: %s '%s'%s", path.c_str(), name, dev.touch ? (dev.mt ? " [multitouch]" : " [touch]") : "");
        devs_.push_back(dev);
    }
    closedir(d);
    return !devs_.empty();
}

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
    if (type == EV_KEY && code != BTN_TOUCH && code != BTN_TOOL_FINGER && code < BTN_MISC) {
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
        return;
    }
    if (type == EV_ABS) {
        switch (code) {
            case ABS_MT_SLOT: d.slot = value; break;
            case ABS_MT_TRACKING_ID:
                if (d.slot == 0) d.down = value >= 0;
                break;
            case ABS_MT_POSITION_X: if (d.slot == 0) { d.curX = value; d.moved = true; } break;
            case ABS_MT_POSITION_Y: if (d.slot == 0) { d.curY = value; d.moved = true; } break;
            case ABS_MT_TOUCH_MAJOR: case ABS_MT_PRESSURE:
                // Protocol A devices signal presence via touch major / pressure.
                if (d.slot == 0 && d.mt && value == 0 && code == ABS_MT_TOUCH_MAJOR) d.down = false;
                break;
            case ABS_X: if (!d.mt) { d.curX = value; d.moved = true; } break;
            case ABS_Y: if (!d.mt) { d.curY = value; d.moved = true; } break;
        }
        return;
    }
    if (type == EV_SYN && code == SYN_MT_REPORT && d.mt) {
        // Protocol A: a report with coordinates means finger down.
        if (d.moved) d.down = true;
        return;
    }
    if (type == EV_SYN && code == SYN_REPORT) {
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
