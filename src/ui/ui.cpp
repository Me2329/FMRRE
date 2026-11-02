#include "ui/ui.h"

#include <cmath>
#include <cstring>

namespace tw {

float easeOutCubic(float t) { t = std::clamp(t, 0.f, 1.f); return 1 - std::pow(1 - t, 3.f); }
float easeInOutCubic(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.f) / 2;
}
float easeOutBack(float t) {
    t = std::clamp(t, 0.f, 1.f);
    const float c1 = 1.70158f, c3 = c1 + 1;
    return 1 + c3 * std::pow(t - 1, 3.f) + c1 * std::pow(t - 1, 2.f);
}

// ---------------------------------------------------------------------------------
void Ui::beginFrame(Canvas& canvas, const std::vector<InputEvent>& evs, int64_t nowMs) {
    c = &canvas;
    dt = now ? std::clamp((nowMs - now) / 1000.0f, 0.001f, 0.05f) : 0.016f;
    now = nowMs;
    evs_ = evs;
    pressed_ = released_ = false;
    keys_.clear();
    animating_ = false;
    ambient_ = false;
    activateFocus_ = false;
    idStack_.clear();
    float slop = dp(9);
    for (auto& e : evs) {
        switch (e.type) {
            case EvType::TouchDown:
                down_ = pressed_ = true;
                tx_ = px_ = e.x;
                ty_ = py_ = e.y;
                pressTime_ = now;
                dragActive_ = false;
                longFired_ = false;
                focusVisible_ = false;
                break;
            case EvType::TouchMove:
                tx_ = e.x;
                ty_ = e.y;
                if (down_ && capture_ == 0 && std::hypot(tx_ - px_, ty_ - py_) > slop) dragActive_ = true;
                break;
            case EvType::TouchUp:
                tx_ = e.x;
                ty_ = e.y;
                if (down_) released_ = true;
                down_ = false;
                break;
            case EvType::KeyDown: {
                keys_.push_back(e.key);
                int n = std::max(prevFocusCount_, 1);
                if (e.key == Key::VolUp || e.key == Key::Up) {
                    focusIndex_ = focusVisible_ ? (focusIndex_ - 1 + n) % n : 0;
                    focusVisible_ = true;
                } else if (e.key == Key::VolDown || e.key == Key::Down) {
                    focusIndex_ = focusVisible_ ? (focusIndex_ + 1) % n : 0;
                    focusVisible_ = true;
                } else if ((e.key == Key::Power || e.key == Key::Enter) && focusVisible_) {
                    activateFocus_ = true;
                }
                break;
            }
            case EvType::Text: typed_ += e.text; break;
            default: break;
        }
    }
    focusCount_ = 0;
    focusedId_ = 0;
    if (down_) animating_ = true;
}

void Ui::endFrame() {
    prevFocusCount_ = focusCount_;
    if (focusIndex_ >= focusCount_) focusIndex_ = focusCount_ ? focusCount_ - 1 : -1;
    if (released_) {
        active_ = 0;
        capture_ = 0;
    }
    // Garbage-collect widget state not seen for a while.
    if (ws_.size() > 600) {
        for (auto it = ws_.begin(); it != ws_.end();) {
            if (now - it->second.lastSeen > 10000) it = ws_.erase(it);
            else ++it;
        }
    }
}

bool Ui::keyPressed(Key k) const {
    for (auto x : keys_)
        if (x == k) return true;
    return false;
}

void Ui::consumeKey(Key k) {
    for (auto& x : keys_)
        if (x == k) x = Key::None;
}

std::string Ui::takeTypedText() {
    std::string s;
    s.swap(typed_);
    return s;
}

Id Ui::id(const char* s, int64_t n) const {
    uint64_t h = 1469598103934665603ull;
    for (Id p : idStack_) { h ^= p; h *= 1099511628211ull; }
    for (const char* p = s; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ull; }
    h ^= (uint64_t)n * 0x9E3779B97F4A7C15ull;
    h *= 1099511628211ull;
    return h ? h : 1;
}

Ui::WState& Ui::st(Id i) {
    WState& s = ws_[i];
    s.lastSeen = now;
    return s;
}

float Ui::anim(Id i, float target, float speed) {
    WState& s = st(i);
    if (!animations) { s.v = target; return target; }
    float k = 1 - std::exp(-speed * animSpeed * dt);
    s.v += (target - s.v) * k;
    if (std::fabs(target - s.v) > 0.002f) animating_ = true;
    else s.v = target;
    return s.v;
}

float& Ui::value(Id i, float def) {
    auto it = values_.find(i);
    if (it == values_.end()) it = values_.emplace(i, def).first;
    return it->second;
}

bool Ui::hitVisible(const Rect& r, float px, float py) const {
    Rect clip = c->clipLocal();
    float lx = px - c->ox(), ly = py - c->oy();
    return r.contains(lx, ly) && clip.contains(lx, ly);
}

int Ui::clickable(Id i, const Rect& r, bool focusable) {
    WState& s = st(i);
    int res = 0;
    if (!inputEnabled) return 0;
    if (focusable) {
        int idx = focusCount_++;
        if (idx == focusIndex_ && focusVisible_) {
            focusedId_ = i;
            if (activateFocus_) {
                res |= Clicked;
                activateFocus_ = false;
                s.rippleStart = now;
                s.rx = r.w / 2;
                s.ry = r.h / 2;
                animating_ = true;
            }
            // Keep the focused widget visible inside the innermost scroll area.
            if (!scrollStack_.empty() && !keys_.empty()) {
                ScrollCtx& ctx = scrollStack_.back();
                ScrollState& ss = scroll_[ctx.id];
                float top = ctx.vp.y + ss.off, bot = top + ctx.vp.h, m = dp(12);
                if (r.y < top) ss.off = std::max(0.f, r.y - ctx.vp.y - m);
                else if (r.b() > bot) ss.off = r.b() - ctx.vp.y - ctx.vp.h + m;
                ss.vel = 0;
                animating_ = true;
            }
        }
    }
    if (pressed_ && active_ == 0 && capture_ == 0 && hitVisible(r, tx_, ty_)) {
        active_ = i;
        s.rippleStart = now;
        s.rx = (tx_ - c->ox()) - r.x;
        s.ry = (ty_ - c->oy()) - r.y;
    }
    if (active_ == i) {
        animating_ = true;
        if (dragActive_) {
            active_ = 0;
        } else if (down_ && !longFired_ && now - pressTime_ > 550) {
            res |= LongPressed;
            longFired_ = true;
        } else if (released_) {
            if (!longFired_ && hitVisible(r, tx_, ty_)) res |= Clicked;
            active_ = 0;
        }
    }
    return res;
}

void Ui::drawPressFx(Id i, const Rect& r, float radius, Color tint) {
    WState& s = st(i);
    float p = anim(i ^ 0x5bd1e995ull, isPressed(i) ? 1.f : 0.f, 22);
    if (p > 0.01f) c->fillRoundRect(r, radius, withAlpha(tint, 0.10f * p));
    if (s.rippleStart && animations) {
        float t = (now - s.rippleStart) / (420.0f / animSpeed);
        if (t < 1.4f) {
            float maxR = std::hypot(std::max(s.rx, r.w - s.rx), std::max(s.ry, r.h - s.ry));
            float rad = maxR * easeOutCubic(std::min(t, 1.f));
            float a = isPressed(i) ? 0.16f : 0.16f * std::max(0.f, 1 - (t - 0.3f) / 1.1f);
            if (a > 0) c->fillCircleInRR(r.x + s.rx, r.y + s.ry, rad, r, radius, withAlpha(tint, a));
            animating_ = true;
        } else {
            s.rippleStart = 0;
        }
    }
    drawFocusRing(i, r, radius);
}

void Ui::drawFocusRing(Id i, const Rect& r, float radius) {
    if (!isFocused(i)) return;
    float pulse = 0.65f + 0.35f * std::sin(now / 180.0f);
    c->strokeRoundRect(r.inset(-dp(2)), radius + dp(2), dp(2.5f), withAlpha(th.accent, pulse));
    ambient_ = true;
}

// ---------------------------------------------------------------------------------
void Ui::background(const Rect& r) {
    // The backdrop is static per theme: render it once, then just copy it.
    int w = (int)r.w, h = (int)r.h;
    uint64_t key = (uint64_t(w) << 48) ^ (uint64_t(h) << 32) ^ th.bgTop ^ (uint64_t(th.bgBottom) << 7) ^
                   (uint64_t(th.glow1) << 13) ^ (uint64_t(th.glow2) << 21) ^ (th.glows ? 0x9999ull : 0);
    if (key != bgKey_ || bgCache_.width() != w || bgCache_.height() != h) {
        bgCache_.resize(w, h);
        Rect full(0, 0, r.w, r.h);
        bgCache_.gradientV(full, th.bgTop, th.bgBottom);
        if (th.glows) {
            bgCache_.radialGlow(r.w * 0.15f, r.h * 0.12f, r.w * 0.9f, th.glow1);
            bgCache_.radialGlow(r.w * 0.95f, r.h * 0.78f, r.w * 0.8f, th.glow2);
        }
        bgKey_ = key;
    }
    c->blit(bgCache_, r.x, r.y);
}

void Ui::card(const Rect& r, float radius, Color fill, bool shadow) {
    if (radius < 0) radius = dp(20);
    Color f = fill ? fill : th.surface;
    bool opaque = (f >> 24) == 255;
    if (shadow && !th.light) c->shadow(r, radius, dp(14), rgba(0, 0, 0, 90), dp(4), opaque);
    if (shadow && th.light) c->shadow(r, radius, dp(12), rgba(15, 23, 42, 24), dp(3), opaque);
    c->fillRoundRect(r, radius, f);
    if (!th.light) c->strokeRoundRect(r, radius, std::max(1.f, dp(0.8f)), withAlpha(rgb(0xFFFFFF), 0.05f));
}

bool Ui::button(Id i, const Rect& r, const std::string& text, Icon icon, BtnStyle style) {
    int res = clickable(i, r);
    float rad = std::min(r.h / 2, dp(16));
    Color bg = 0, fg = th.text, fx = th.text;
    switch (style) {
        case BtnStyle::Filled: bg = th.accent; fg = th.onAccent; fx = th.onAccent; break;
        case BtnStyle::Tonal: bg = th.accentSoft; fg = th.accent; fx = th.accent; break;
        case BtnStyle::Outline: bg = 0; fg = th.text; break;
        case BtnStyle::Ghost: bg = 0; fg = th.accent; fx = th.accent; break;
        case BtnStyle::Danger: bg = withAlpha(th.danger, 0.18f); fg = th.danger; fx = th.danger; break;
    }
    float press = anim(i ^ 0xabcdefull, isPressed(i) ? 1.f : 0.f, 20);
    Rect rr = r.inset(press * dp(1.5f));
    if (style == BtnStyle::Filled) {
        c->shadow(rr, rad, dp(10), withAlpha(th.accent, 0.35f), dp(3));
        c->fillRoundRect(rr, rad, th.accent);
    } else if (bg) {
        c->fillRoundRect(rr, rad, bg);
    }
    if (style == BtnStyle::Outline) c->strokeRoundRect(rr, rad, std::max(1.f, dp(1.2f)), th.outline);
    drawPressFx(i, rr, rad, fx);
    float fs = dp(15);
    float iconS = icon != Icon::None ? dp(20) : 0;
    float tw = font::measure(Face::SemiBold, fs, text);
    float total = tw + (iconS ? iconS + dp(8) : 0);
    float x = rr.cx() - total / 2;
    if (iconS) {
        icons::draw(*c, icon, x, rr.cy() - iconS / 2, iconS, fg);
        x += iconS + dp(8);
    }
    font::draw(*c, Face::SemiBold, fs, x, rr.cy() - font::lineHeight(Face::SemiBold, fs) / 2, text, fg);
    return res & Clicked;
}

bool Ui::iconButton(Id i, const Rect& r, Icon icon, Color col, bool filledBg) {
    int res = clickable(i, r);
    float rad = std::min(r.w, r.h) / 2;
    if (filledBg) c->fillRoundRect(r, rad, th.surface2);
    drawPressFx(i, r, rad, th.text);
    float s = std::min(r.w, r.h) * 0.5f;
    icons::drawCentered(*c, icon, r.cx(), r.cy(), s, col ? col : th.text);
    return res & Clicked;
}

bool Ui::tile(Id i, const Rect& r, Icon icon, const std::string& title, const std::string& sub, Color tint) {
    int res = clickable(i, r);
    float press = anim(i ^ 0x1234ull, isPressed(i) ? 1.f : 0.f, 18);
    Rect rr = r.inset(press * dp(3));
    float rad = dp(22);
    card(rr, rad, th.surface);
    // tinted corner glow
    c->radialGlowRR(rr.r() - dp(10), rr.y + dp(10), rr.w * 0.62f, withAlpha(tint, th.light ? 0.10f : 0.14f), rr, rad);
    float ic = dp(44);
    Rect icR(rr.x + dp(16), rr.y + dp(16), ic, ic);
    c->fillRoundRect(icR, dp(14), withAlpha(tint, 0.18f));
    icons::drawCentered(*c, icon, icR.cx(), icR.cy(), dp(24), tint);
    font::drawIn(*c, Face::SemiBold, dp(17), Rect(rr.x + dp(16), rr.b() - dp(52), rr.w - dp(24), dp(24)), title, th.text);
    font::drawIn(*c, Face::Regular, dp(12.5f), Rect(rr.x + dp(16), rr.b() - dp(30), rr.w - dp(24), dp(18)), sub,
                 th.textDim);
    drawPressFx(i, rr, rad, tint);
    return res & Clicked;
}

int Ui::listItem(Id i, const Rect& r, Icon icon, const std::string& title, const std::string& sub,
                 const std::string& trailing, Color iconTint, bool chevron) {
    int res = clickable(i, r);
    float rad = dp(16);
    drawPressFx(i, r, rad, th.text);
    float x = r.x + dp(12);
    if (icon != Icon::None) {
        Color t = iconTint ? iconTint : th.accent;
        float s = dp(40);
        Rect ir(x, r.cy() - s / 2, s, s);
        c->fillRoundRect(ir, dp(12), withAlpha(t, 0.16f));
        icons::drawCentered(*c, icon, ir.cx(), ir.cy(), dp(22), t);
        x += s + dp(14);
    }
    float right = r.r() - dp(12);
    if (chevron) {
        icons::drawCentered(*c, Icon::Forward, right - dp(9), r.cy(), dp(18), th.textFaint);
        right -= dp(26);
    }
    if (!trailing.empty()) {
        float tw = std::min(font::measure(Face::Regular, dp(13), trailing), r.w * 0.35f);
        font::drawIn(*c, Face::Regular, dp(13), Rect(right - tw, r.y, tw, r.h), trailing, th.textDim, AlignRight);
        right -= tw + dp(10);
    }
    if (sub.empty()) {
        font::drawIn(*c, Face::SemiBold, dp(15.5f), Rect(x, r.y, right - x, r.h), title, th.text);
    } else {
        font::drawIn(*c, Face::SemiBold, dp(15.5f), Rect(x, r.cy() - dp(21), right - x, dp(22)), title, th.text);
        font::drawIn(*c, Face::Regular, dp(12.5f), Rect(x, r.cy() + dp(1), right - x, dp(20)), sub, th.textDim);
    }
    return res;
}

void Ui::switchVisual(Id i, const Rect& r, bool on) {
    float t = anim(i ^ 0x77ull, on ? 1.f : 0.f, 16);
    Color track = mix(th.surfaceHi, th.accent, t);
    c->fillRoundRect(r, r.h / 2, track);
    float kr = r.h / 2 - dp(3);
    float kx = r.x + r.h / 2 + (r.w - r.h) * t;
    c->fillCircle(kx, r.cy() + dp(1), kr, rgba(0, 0, 0, 50), dp(3));
    c->fillCircle(kx, r.cy(), kr, mix(th.textDim, th.light ? rgb(0xFFFFFF) : th.onAccent, t));
    if (t > 0.5f) icons::drawCentered(*c, Icon::Check, kx, r.cy(), kr * 1.2f, withAlpha(th.accent, (t - 0.5f) * 2));
}

void Ui::checkboxVisual(Id i, const Rect& r, bool on) {
    float t = anim(i ^ 0x99ull, on ? 1.f : 0.f, 18);
    c->fillRoundRect(r, dp(7), mix(th.surfaceHi, th.accent, t));
    if (t < 0.99f) c->strokeRoundRect(r, dp(7), dp(1.6f), withAlpha(th.textFaint, 1 - t));
    if (t > 0.05f) {
        float s = r.w * (0.6f + 0.25f * easeOutBack(t));
        icons::drawCentered(*c, Icon::Check, r.cx(), r.cy(), s, withAlpha(th.onAccent, t));
    }
}

bool Ui::toggleRow(Id i, const Rect& r, Icon icon, const std::string& title, const std::string& sub, bool& v) {
    int res = clickable(i, r);
    if (res & Clicked) v = !v;
    drawPressFx(i, r, dp(16), th.text);
    float x = r.x + dp(12);
    if (icon != Icon::None) {
        icons::drawCentered(*c, icon, x + dp(12), r.cy(), dp(22), th.textDim);
        x += dp(38);
    }
    Rect sw(r.r() - dp(64), r.cy() - dp(14), dp(50), dp(28));
    switchVisual(i, sw, v);
    float tw = sw.x - x - dp(10);
    if (sub.empty()) {
        font::drawIn(*c, Face::SemiBold, dp(15), Rect(x, r.y, tw, r.h), title, th.text);
    } else {
        font::drawIn(*c, Face::SemiBold, dp(15), Rect(x, r.cy() - dp(20), tw, dp(21)), title, th.text);
        font::drawIn(*c, Face::Regular, dp(12.5f), Rect(x, r.cy() + dp(1), tw, dp(19)), sub, th.textDim);
    }
    return res & Clicked;
}

bool Ui::checkRow(Id i, const Rect& r, const std::string& title, const std::string& sub, bool& v,
                  const std::string& trailing) {
    int res = clickable(i, r);
    if (res & Clicked) v = !v;
    drawPressFx(i, r, dp(14), th.text);
    float cb = dp(24);
    checkboxVisual(i, Rect(r.x + dp(12), r.cy() - cb / 2, cb, cb), v);
    float x = r.x + dp(50);
    float right = r.r() - dp(12);
    if (!trailing.empty()) {
        float tw = font::measure(Face::Regular, dp(13), trailing);
        font::drawIn(*c, Face::Regular, dp(13), Rect(right - tw, r.y, tw, r.h), trailing, th.textDim, AlignRight);
        right -= tw + dp(10);
    }
    if (sub.empty()) {
        font::drawIn(*c, Face::SemiBold, dp(15), Rect(x, r.y, right - x, r.h), title, th.text);
    } else {
        font::drawIn(*c, Face::SemiBold, dp(15), Rect(x, r.cy() - dp(20), right - x, dp(21)), title, th.text);
        font::drawIn(*c, Face::Regular, dp(12.5f), Rect(x, r.cy() + dp(1), right - x, dp(19)), sub, th.textDim);
    }
    return res & Clicked;
}

bool Ui::slider(Id i, const Rect& r, float& v) {
    bool changed = false;
    Rect hit = r.inset(0, -dp(10));
    if (inputEnabled && pressed_ && capture_ == 0 && active_ == 0 && hitVisible(hit, tx_, ty_)) capture_ = i;
    if (capture_ == i && (down_ || released_)) {
        float nv = std::clamp(((tx_ - c->ox()) - r.x) / r.w, 0.f, 1.f);
        if (nv != v) { v = nv; changed = true; }
        animating_ = true;
    }
    if (inputEnabled) {
        // Hardware keys: focus with vol keys, power cycles in 10% steps.
        int idx = focusCount_++;
        if (idx == focusIndex_ && focusVisible_) {
            focusedId_ = i;
            if (activateFocus_) {
                v = v >= 0.999f ? 0.f : std::min(1.f, v + 0.1f);
                changed = true;
                activateFocus_ = false;
            }
        }
    }
    float h = dp(6);
    Rect track(r.x, r.cy() - h / 2, r.w, h);
    c->fillRoundRect(track, h / 2, th.surfaceHi);
    Rect fill(r.x, track.y, r.w * v, h);
    c->fillRoundRect(fill, h / 2, th.accent);
    float grow = anim(i ^ 0x4242ull, capture_ == i ? 1.f : 0.f, 18);
    float kx = r.x + r.w * v;
    if (grow > 0.01f) c->fillCircle(kx, r.cy(), dp(20) * grow, withAlpha(th.accent, 0.18f));
    c->fillCircle(kx, r.cy(), dp(10) + dp(2) * grow, th.accent);
    c->fillCircle(kx, r.cy(), dp(4), th.onAccent);
    if (isFocused(i)) drawFocusRing(i, r.inset(-dp(6), -dp(8)), dp(12));
    return changed;
}

float Ui::chipWidth(const std::string& text, Icon icon) const {
    return font::measure(Face::SemiBold, dp(13.5f), text) + dp(28) + (icon != Icon::None ? dp(22) : 0);
}

bool Ui::chip(Id i, const Rect& r, const std::string& text, bool selected, Icon icon) {
    int res = clickable(i, r);
    float t = anim(i ^ 0x3131ull, selected ? 1.f : 0.f, 16);
    float rad = r.h / 2;
    c->fillRoundRect(r, rad, mix(th.surface2, th.accentSoft, t));
    c->strokeRoundRect(r, rad, std::max(1.f, dp(1.1f)), mix(th.outline, th.accent, t));
    drawPressFx(i, r, rad, th.accent);
    Color fg = mix(th.textDim, th.accent, t);
    float x = r.x + dp(14);
    if (icon != Icon::None) {
        icons::drawCentered(*c, icon, x + dp(8), r.cy(), dp(16), fg);
        x += dp(22);
    }
    font::drawIn(*c, Face::SemiBold, dp(13.5f), Rect(x, r.y, r.r() - x - dp(10), r.h), text, fg);
    return res & Clicked;
}

bool Ui::slideToConfirm(Id i, const Rect& r, const std::string& text, Color tint) {
    float& pos = value(i, 0);
    float& done = value(i ^ 0xD0Eull, 0);
    float& grab = value(i ^ 0x6AB, 0);
    float knob = r.h - dp(8);
    float travel = r.w - knob - dp(8);
    bool fired = false;
    Rect kr(r.x + dp(4) + travel * pos, r.y + dp(4), knob, knob);
    if (inputEnabled && done == 0 && pressed_ && capture_ == 0 && active_ == 0 && hitVisible(r, tx_, ty_)) {
        capture_ = i;
        float lx = tx_ - c->ox();
        grab = kr.contains(lx, ty_ - c->oy()) ? lx - kr.x : knob / 2;
    }
    if (inputEnabled) {
        int idx = focusCount_++;
        if (idx == focusIndex_ && focusVisible_) {
            focusedId_ = i;
            if (activateFocus_ && done == 0) {
                done = 1;
                pos = 1;
                fired = true;
                activateFocus_ = false;
            }
        }
    }
    if (capture_ == i) {
        animating_ = true;
        pos = std::clamp(((tx_ - c->ox()) - r.x - dp(4) - grab) / travel, 0.f, 1.f);
        if (released_) {
            if (pos > 0.86f) {
                done = 1;
                pos = 1;
                fired = true;
            }
        }
    } else if (done == 0 && pos > 0) {
        pos = std::max(0.f, pos - dt * 4.5f * (0.4f + pos));
        animating_ = true;
    }
    if (done > 0) {
        done += dt;
        animating_ = true;
        if (done > 1.6f) { done = 0; pos = 0; }
    }
    // track
    float rad = r.h / 2;
    c->fillRoundRect(r, rad, th.surface2);
    c->strokeRoundRect(r, rad, std::max(1.f, dp(1)), withAlpha(tint, 0.35f));
    float fx = r.x + dp(4) + travel * pos + knob;
    Rect fill(r.x, r.y, fx - r.x + dp(4), r.h);
    c->pushClip(fill);
    c->fillRoundRect(r, rad, withAlpha(tint, 0.25f + 0.25f * pos));
    c->popClip();
    // label with shimmer
    float la = std::max(0.f, 1 - pos * 1.8f);
    if (la > 0) {
        float lw = font::measure(Face::SemiBold, dp(15), text);
        float lx = r.cx() - lw / 2 + knob / 2;
        font::draw(*c, Face::SemiBold, dp(15), lx, r.cy() - font::lineHeight(Face::SemiBold, dp(15)) / 2, text,
                   withAlpha(th.text, 0.85f * la));
        if (animations) {
            float ph = std::fmod(now / 900.0f, 1.0f);
            for (int k = 0; k < 3; k++) {
                float a = 0.25f + 0.6f * std::max(0.f, std::sin((ph - k * 0.18f) * 6.2831f));
                icons::drawCentered(*c, Icon::Forward, r.r() - dp(46) + k * dp(12), r.cy(), dp(16),
                                    withAlpha(tint, a * la));
            }
            ambient_ = true;
        }
    }
    kr = Rect(r.x + dp(4) + travel * pos, r.y + dp(4), knob, knob);
    c->shadow(kr, knob / 2, dp(8), withAlpha(tint, 0.45f), dp(2));
    c->fillRoundRect(kr, knob / 2, tint);
    icons::drawCentered(*c, done > 0 ? Icon::Check : Icon::Forward, kr.cx(), kr.cy(), knob * 0.48f, th.onAccent);
    if (isFocused(i)) drawFocusRing(i, r, rad);
    return fired;
}

void Ui::progressBar(const Rect& r, float frac, Color col, bool indeterminate) {
    if (!col) col = th.accent;
    float rad = r.h / 2;
    c->fillRoundRect(r, rad, th.surfaceHi);
    if (indeterminate) {
        float ph = std::fmod(now / 1100.0f, 1.0f);
        float w = r.w * 0.35f;
        float x = r.x - w + (r.w + w) * easeInOutCubic(ph);
        c->pushClip(r);
        c->fillRoundRect(Rect(x, r.y, w, r.h), rad, col);
        c->popClip();
        animating_ = true;
        return;
    }
    frac = std::clamp(frac, 0.f, 1.f);
    if (frac <= 0) return;
    Rect f(r.x, r.y, std::max(r.h, r.w * frac), r.h);
    c->fillRoundRect(f, rad, col);
    // moving sheen
    if (animations && frac < 1) {
        float ph = std::fmod(now / 1300.0f, 1.0f);
        c->pushClip(f);
        float sx = f.x + (f.w + dp(80)) * ph - dp(40);
        c->radialGlow(sx, f.cy(), dp(40), rgba(255, 255, 255, 70));
        c->popClip();
        animating_ = true;
    }
}

void Ui::usageBar(const Rect& r, float frac, Color col) {
    if (!col) col = frac > 0.9f ? th.danger : (frac > 0.75f ? th.warn : th.accent);
    c->fillRoundRect(r, r.h / 2, th.surfaceHi);
    if (frac > 0) c->fillRoundRect(Rect(r.x, r.y, std::max(r.h, r.w * std::min(frac, 1.f)), r.h), r.h / 2, col);
}

void Ui::ring(float cx, float cy, float rad, float width, float frac, Color col, Color track) {
    c->strokeCircle(cx, cy, rad, width, track ? track : th.surfaceHi);
    if (frac > 0.001f) c->strokeArc(cx, cy, rad, width, 0, 6.28318f * std::min(frac, 1.f), col ? col : th.accent);
}

void Ui::sectionHeader(const Rect& r, const std::string& s) {
    std::string up;
    for (char ch : s) up += (char)toupper((unsigned char)ch);
    font::drawIn(*c, Face::Bold, dp(12), Rect(r.x + dp(6), r.y, r.w, r.h), up, th.accent);
}

void Ui::label(const Rect& r, const std::string& s, float size, Color col, Face f, int align) {
    font::drawIn(*c, f, size, r, s, col, align);
}

float Ui::textBlock(float x, float y, float w, const std::string& s, float size, Color col, Face f) {
    auto lines = font::wrap(f, size, s, w);
    float lh = font::lineHeight(f, size);
    for (auto& l : lines) {
        font::draw(*c, f, size, x, y, l, col);
        y += lh;
    }
    return lines.size() * lh;
}

void Ui::badge(float x, float cy, const std::string& s, Color col) {
    float fs = dp(11.5f);
    float w = font::measure(Face::SemiBold, fs, s) + dp(14);
    Rect r(x, cy - dp(11), w, dp(22));
    c->fillRoundRect(r, dp(11), withAlpha(col, 0.18f));
    font::drawIn(*c, Face::SemiBold, fs, r, s, col, AlignCenter);
}

// ---------------------------------------------------------------------------------
void Ui::beginScroll(Id i, const Rect& vp) {
    ScrollState& s = scroll_[i];
    s.view = vp.h;
    float maxOff = std::max(0.f, s.content - s.view);
    if (inputEnabled && pressed_ && hitVisible(vp, tx_, ty_)) {
        s.armed = true;
        s.startOff = s.off;
        s.pressY = ty_;
        s.lastY = ty_;
        s.vel = 0;
    }
    if (s.armed && down_ && dragActive_ && capture_ == 0) s.drag = true;
    if (s.drag) {
        float target = s.startOff - (ty_ - s.pressY);
        if (target < 0) target *= 0.35f;
        if (target > maxOff) target = maxOff + (target - maxOff) * 0.35f;
        float inst = -(ty_ - s.lastY) / std::max(dt, 0.001f);
        s.vel = s.vel * 0.5f + inst * 0.5f;
        s.lastY = ty_;
        s.off = target;
        animating_ = true;
    }
    if (released_ || !down_) {
        if (s.drag) s.drag = false;
        s.armed = false;
    }
    if (!s.drag) {
        if (std::fabs(s.vel) > 8) {
            s.off += s.vel * dt;
            s.vel *= std::pow(0.08f, dt);
            animating_ = true;
        } else {
            s.vel = 0;
        }
        if (s.off < 0) {
            s.off += (0 - s.off) * std::min(1.f, dt * 14);
            s.vel *= 0.4f;
            if (s.off > -0.5f) s.off = 0;
            animating_ = true;
        } else if (s.off > maxOff) {
            s.off += (maxOff - s.off) * std::min(1.f, dt * 14);
            s.vel *= 0.4f;
            if (s.off - maxOff < 0.5f) s.off = maxOff;
            animating_ = true;
        }
    }
    float target = (s.drag || std::fabs(s.vel) > 8) ? 1.f : 0.f;
    s.showBar += (target - s.showBar) * std::min(1.f, dt * (target > s.showBar ? 12 : 3));
    if (s.showBar > 0.01f) animating_ = true;
    c->pushClip(vp);
    scrollStack_.push_back({i, vp, c->oy()});
    c->setOrigin(c->ox(), c->oy() - std::round(s.off));
}

void Ui::endScroll(float contentBottom) {
    if (scrollStack_.empty()) return;
    ScrollCtx ctx = scrollStack_.back();
    scrollStack_.pop_back();
    ScrollState& s = scroll_[ctx.id];
    s.content = contentBottom - ctx.vp.y;
    c->setOrigin(c->ox(), ctx.oldOy);
    if (s.content > s.view + 1 && s.showBar > 0.01f) {
        float bh = std::max(dp(30), s.view * s.view / s.content);
        float maxOff = s.content - s.view;
        float frac = std::clamp(s.off / maxOff, 0.f, 1.f);
        Rect bar(ctx.vp.r() - dp(5), ctx.vp.y + dp(2) + frac * (s.view - bh - dp(4)), dp(3), bh);
        c->fillRoundRect(bar, dp(1.5f), withAlpha(th.textDim, 0.6f * s.showBar));
    }
    c->popClip();
}

void Ui::scrollToEnd(Id i) {
    ScrollState& s = scroll_[i];
    s.off = std::max(0.f, s.content - s.view);
    s.vel = 0;
}

void Ui::resetScroll(Id i) { scroll_[i] = ScrollState(); }

// ---------------------------------------------------------------------------------
bool Ui::textField(Id i, const Rect& r, const std::string& text, const std::string& placeholder, bool focused) {
    int res = clickable(i, r, false);
    c->fillRoundRect(r, dp(14), th.surface2);
    c->strokeRoundRect(r, dp(14), focused ? dp(2) : dp(1), focused ? th.accent : th.outline);
    float fs = dp(16);
    Rect tr = r.inset(dp(14), 0);
    std::string shown = text;
    // Keep the end of long text visible.
    while (!shown.empty() && font::measure(Face::Regular, fs, shown) > tr.w - dp(6)) shown.erase(0, 1);
    if (text.empty()) font::drawIn(*c, Face::Regular, fs, tr, placeholder, th.textFaint);
    else font::drawIn(*c, Face::Regular, fs, tr, shown, th.text);
    if (focused && (now / 530) % 2 == 0) {
        float cx = tr.x + (text.empty() ? 0 : font::measure(Face::Regular, fs, shown)) + dp(1);
        c->fillRect(Rect(cx, r.cy() - dp(11), dp(2), dp(22)), th.accent);
    }
    if (focused) ambient_ = true;
    return res & Clicked;
}

bool Ui::keyboard(Id kid, const Rect& r, std::string& text) {
    bool enter = false;
    // Physical keyboard input (simulator / USB OTG keyboards).
    text += takeTypedText();
    if (keyPressed(Key::Backspace) && !text.empty()) text.pop_back();

    c->fillRect(r, th.light ? rgb(0xDDE2EC) : mix(th.bgBottom, rgb(0x000000), 0.35f));
    c->fillRect(Rect(r.x, r.y, r.w, std::max(1.f, dp(1))), th.outline);
    static const char* letters[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const char* syms[3] = {"1234567890", "-/:;()$&@\"", "!|~<>=+*?"};
    const char** rows = symbols_ ? syms : letters;
    float pad = dp(4);
    float rowH = (r.h - pad * 2) / 4;
    float keyW = (r.w - pad * 2) / 10;
    pushId(kid);
    auto key = [&](const Rect& kr, const std::string& lab, Icon icon, bool special, int64_t n) -> bool {
        Id i = id("k", n);
        int res = clickable(i, kr, false);
        Rect vr = kr.inset(dp(2.5f));
        bool pr = isPressed(i);
        c->fillRoundRect(vr, dp(8), pr ? th.accent : (special ? th.surfaceHi : th.surface2));
        Color fg = pr ? th.onAccent : th.text;
        if (icon != Icon::None) icons::drawCentered(*c, icon, vr.cx(), vr.cy(), dp(20), fg);
        else font::drawIn(*c, special ? Face::SemiBold : Face::Regular, special ? dp(13) : dp(19), vr, lab, fg, AlignCenter);
        if (pr && !special && !lab.empty()) {
            // key preview bubble
            Rect b(vr.x - dp(6), vr.y - dp(54), vr.w + dp(12), dp(50));
            c->shadow(b, dp(12), dp(8), rgba(0, 0, 0, 90));
            c->fillRoundRect(b, dp(12), th.surfaceHi);
            font::drawIn(*c, Face::SemiBold, dp(24), b, lab, th.text, AlignCenter);
        }
        return res & Clicked;
    };
    for (int row = 0; row < 3; row++) {
        std::string keys = rows[row];
        float y = r.y + pad + row * rowH;
        float w = keys.size() * keyW;
        float x = r.x + pad + (r.w - pad * 2 - w) / 2;
        if (row == 2) {
            // shift / backspace on the sides
            if (!symbols_) {
                if (key(Rect(r.x + pad, y, keyW * 1.5f, rowH), shift_ ? "SHIFT" : "shift", Icon::Up, true, 900))
                    shift_ = !shift_;
            }
            if (key(Rect(r.r() - pad - keyW * 1.5f, y, keyW * 1.5f, rowH), "", Icon::Back, true, 901) && !text.empty()) {
                // remove one UTF-8 codepoint
                size_t n = text.size() - 1;
                while (n > 0 && (text[n] & 0xC0) == 0x80) n--;
                text.erase(n);
            }
            if (symbols_) x = r.x + pad + keyW * 0.5f;
        }
        for (size_t k = 0; k < keys.size(); k++) {
            std::string ch(1, keys[k]);
            if (shift_ && !symbols_) ch[0] = (char)toupper(ch[0]);
            if (key(Rect(x + k * keyW, y, keyW, rowH), ch, Icon::None, false, row * 100 + (int)k)) {
                text += ch;
                if (shift_) shift_ = false;
            }
        }
    }
    float y = r.y + pad + 3 * rowH;
    float x = r.x + pad;
    if (key(Rect(x, y, keyW * 1.5f, rowH), symbols_ ? "ABC" : "?123", Icon::None, true, 910)) symbols_ = !symbols_;
    x += keyW * 1.5f;
    if (key(Rect(x, y, keyW, rowH), symbols_ ? "," : ",", Icon::None, false, 911)) text += ",";
    x += keyW;
    if (key(Rect(x, y, keyW * 4, rowH), "space", Icon::None, true, 912)) text += " ";
    x += keyW * 4;
    if (key(Rect(x, y, keyW, rowH), ".", Icon::None, false, 913)) text += ".";
    x += keyW;
    std::string extra = symbols_ ? "#" : "/";
    if (key(Rect(x, y, keyW, rowH), extra, Icon::None, false, 914)) text += extra;
    x += keyW;
    Rect er(x, y, r.r() - pad - x, rowH);
    Id ei = id("enter");
    int res = clickable(ei, er, false);
    Rect evr = er.inset(dp(2.5f));
    c->fillRoundRect(evr, dp(8), isPressed(ei) ? lighten(th.accent, 0.2f) : th.accent);
    icons::drawCentered(*c, Icon::Check, evr.cx(), evr.cy(), dp(22), th.onAccent);
    if (res & Clicked) enter = true;
    popId();
    return enter;
}

}  // namespace tw
