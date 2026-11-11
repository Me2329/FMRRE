#pragma once
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace tw {

enum class Key {
    None = 0,
    VolUp, VolDown, Power, Back, Home, Menu,
    Enter, Backspace, Left, Right, Up, Down, Escape,
};

enum class EvType { TouchDown, TouchMove, TouchUp, KeyDown, KeyUp, Text };

struct InputEvent {
    EvType type;
    int x = 0, y = 0;
    Key key = Key::None;
    std::string text;
    int rawCode = 0;  // linux keycode, for the key tester
};

class InputQueue {
public:
    void push(const InputEvent& e) {
        std::lock_guard<std::mutex> lk(mu_);
        q_.push_back(e);
    }
    std::vector<InputEvent> drain() {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<InputEvent> v(q_.begin(), q_.end());
        q_.clear();
        return v;
    }
    bool empty() {
        std::lock_guard<std::mutex> lk(mu_);
        return q_.empty();
    }

private:
    std::mutex mu_;
    std::deque<InputEvent> q_;
};

// Linux evdev reader (/dev/input/event*): multitouch protocol A/B, single
// touch screens, hardware keys. Coordinates are scaled to the display.
class EvdevInput {
public:
    bool open(int screenW, int screenH);
    // Wait up to timeoutMs for input; decoded events go into q.
    void poll(InputQueue& q, int timeoutMs);
    void setTransform(bool swapXY, bool flipX, bool flipY) { swap_ = swapXY; flipX_ = flipX; flipY_ = flipY; }
    int deviceCount() const { return (int)devs_.size(); }
    bool hasTouch() const;
    std::vector<std::string> deviceNames() const;
    // Tests: a touch panel that isn't there (multitouch or single touch, with its coordinate range), and raw
    // events for it, decoded exactly like a real device's.
    int addFake(bool multitouch, int maxX, int maxY, int screenW, int screenH);
    void feed(int dev, int type, int code, int value, InputQueue& q);

private:
    struct Dev {
        int fd = -1;
        std::string name;
        bool touch = false, mt = false;
        int minX = 0, maxX = 0, minY = 0, maxY = 0;
        int curX = 0, curY = 0;
        bool down = false, wasDown = false;
        int slot = 0;       // protocol B: the slot the events that follow are about
        int primary = -1;   // protocol B: the slot of the finger being followed (the first one down)
        bool protoA = false;      // protocol A seen (contacts end with SYN_MT_REPORT, no slots)
        int contacts = 0;         // protocol A: contacts in the frame so far
        bool moved = false;
    };
    void handle(Dev& d, int type, int code, int value, InputQueue& q);
    std::vector<Dev> devs_;
    int w_ = 0, h_ = 0;
    bool swap_ = false, flipX_ = false, flipY_ = false;
};

}  // namespace tw
