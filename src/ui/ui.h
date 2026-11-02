#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx/canvas.h"
#include "gfx/font.h"
#include "gfx/icons.h"
#include "platform/input.h"
#include "ui/theme.h"

namespace tw {

using Id = uint64_t;

enum class BtnStyle { Filled, Tonal, Outline, Ghost, Danger };

enum ClickFlags { Clicked = 1, LongPressed = 2 };

// Immediate-mode UI toolkit. Screens call widget functions every frame; the
// toolkit keeps small per-widget animation state keyed by Id.
class Ui {
public:
    Canvas* c = nullptr;
    Theme th;
    float S = 1;  // density scale (1dp in px)
    int64_t now = 0;
    float dt = 0.016f;
    bool animations = true;
    float animSpeed = 1.0f;

    float dp(float v) const { return v * S; }

    // ---- frame lifecycle -------------------------------------------------
    void beginFrame(Canvas& canvas, const std::vector<InputEvent>& evs, int64_t nowMs);
    void endFrame();
    bool wantsFrame() const { return animating_; }
    void keepAnimating() { animating_ = true; }
    // Low-priority motion (shimmer, caret blink, pulses): redrawn at ~15 fps to save power.
    void ambient() { ambient_ = true; }
    bool wantsAmbient() const { return ambient_; }

    // ---- input state ----------------------------------------------------
    bool inputEnabled = true;
    bool touchDown() const { return down_; }
    float touchX() const { return tx_; }
    float touchY() const { return ty_; }
    bool justPressed() const { return pressed_; }
    bool justReleased() const { return released_; }
    bool dragging() const { return dragActive_; }
    bool keyPressed(Key k) const;
    const std::vector<InputEvent>& events() const { return evs_; }
    std::string takeTypedText();
    void consumeKey(Key k);

    // ---- ids ------------------------------------------------------------
    Id id(const char* s, int64_t n = 0) const;
    Id id(const std::string& s, int64_t n = 0) const { return id(s.c_str(), n); }
    void pushId(Id i) { idStack_.push_back(i); }
    void popId() { if (!idStack_.empty()) idStack_.pop_back(); }

    // Smoothly animated value per id (exponential approach).
    float anim(Id i, float target, float speed = 14.0f);
    float& value(Id i, float def = 0);

    // ---- interaction ---------------------------------------------------
    // Returns ClickFlags. Registers the rect for hardware-key focus navigation.
    int clickable(Id i, const Rect& r, bool focusable = true);
    bool isPressed(Id i) const { return active_ == i && down_ && !dragActive_; }
    bool isFocused(Id i) const { return focusVisible_ && focusedId_ == i; }
    void drawPressFx(Id i, const Rect& r, float radius, Color tint);
    void drawFocusRing(Id i, const Rect& r, float radius);

    // ---- widgets --------------------------------------------------------
    void background(const Rect& r);
    void card(const Rect& r, float radius = -1, Color fill = 0, bool shadow = true);
    bool button(Id i, const Rect& r, const std::string& label, Icon icon = Icon::None, BtnStyle st = BtnStyle::Filled);
    bool iconButton(Id i, const Rect& r, Icon icon, Color col = 0, bool filledBg = false);
    bool tile(Id i, const Rect& r, Icon icon, const std::string& title, const std::string& sub, Color tint);
    int listItem(Id i, const Rect& r, Icon icon, const std::string& title, const std::string& sub,
                 const std::string& trailing = "", Color iconTint = 0, bool chevron = true);
    bool toggleRow(Id i, const Rect& r, Icon icon, const std::string& title, const std::string& sub, bool& v);
    bool checkRow(Id i, const Rect& r, const std::string& title, const std::string& sub, bool& v,
                  const std::string& trailing = "");
    void switchVisual(Id i, const Rect& r, bool on);
    void checkboxVisual(Id i, const Rect& r, bool on);
    bool slider(Id i, const Rect& r, float& v);
    bool chip(Id i, const Rect& r, const std::string& label, bool selected, Icon icon = Icon::None);
    float chipWidth(const std::string& label, Icon icon = Icon::None) const;
    // Swipe-to-confirm. Returns true on the frame the swipe completes.
    bool slideToConfirm(Id i, const Rect& r, const std::string& label, Color tint);
    void progressBar(const Rect& r, float frac, Color col = 0, bool indeterminate = false);
    void usageBar(const Rect& r, float frac, Color col = 0);
    void ring(float cx, float cy, float rad, float width, float frac, Color col = 0, Color track = 0);
    void sectionHeader(const Rect& r, const std::string& label);
    void label(const Rect& r, const std::string& s, float size, Color col, Face f = Face::Regular, int align = AlignLeft);
    float textBlock(float x, float y, float w, const std::string& s, float size, Color col, Face f = Face::Regular);
    void badge(float x, float cy, const std::string& s, Color col);  // pill label, x = left

    // ---- scrolling ------------------------------------------------------
    // Content is laid out starting at viewport.y; call endScroll with total content height.
    void beginScroll(Id i, const Rect& viewport);
    void endScroll(float contentHeight);
    void scrollToEnd(Id i);
    void resetScroll(Id i);

    // ---- on-screen keyboard -------------------------------------------
    float keyboardHeight() const { return dp(232); }
    // Returns true when Enter was pressed.
    bool keyboard(Id i, const Rect& r, std::string& text);
    bool textField(Id i, const Rect& r, const std::string& text, const std::string& placeholder, bool focused);

private:
    struct WState {
        float v = 0;            // generic animated value
        float press = 0;        // press highlight 0..1
        float rx = 0, ry = 0;   // ripple origin (local)
        int64_t rippleStart = 0;
        int64_t lastSeen = 0;
    };
    struct ScrollState {
        float off = 0, vel = 0, content = 0, view = 0;
        bool armed = false, drag = false;
        float startOff = 0, pressY = 0, lastY = 0;
        int64_t lastT = 0;
        float showBar = 0;
    };
    struct ScrollCtx {
        Id id;
        Rect vp;
        float oldOy;
    };
    WState& st(Id i);
    bool hitVisible(const Rect& r, float px, float py) const;

    std::vector<InputEvent> evs_;
    bool down_ = false, pressed_ = false, released_ = false;
    float tx_ = 0, ty_ = 0, px_ = 0, py_ = 0;  // current & press position
    int64_t pressTime_ = 0;
    bool dragActive_ = false;
    bool longFired_ = false;
    Id active_ = 0, capture_ = 0;
    bool animating_ = false, ambient_ = false;
    std::vector<Key> keys_;
    std::string typed_;
    std::vector<Id> idStack_;
    std::unordered_map<Id, WState> ws_;
    std::unordered_map<Id, ScrollState> scroll_;
    std::unordered_map<Id, float> values_;
    std::vector<ScrollCtx> scrollStack_;
    // focus navigation
    int focusIndex_ = -1, focusCount_ = 0, prevFocusCount_ = 0;
    Id focusedId_ = 0;
    bool focusVisible_ = false, activateFocus_ = false;
    // keyboard
    bool shift_ = false, symbols_ = false;
    // cached full-screen background
    Canvas bgCache_;
    uint64_t bgKey_ = 0;
};

// Easing helpers
float easeOutCubic(float t);
float easeInOutCubic(float t);
float easeOutBack(float t);

}  // namespace tw
