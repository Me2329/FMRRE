#include "ui/app.h"

#include <cmath>
#include <ctime>
#include <unistd.h>

#include "core/log.h"
#include "core/i18n.h"
#include "core/settings.h"
#include "core/sha256.h"
#include "core/util.h"
#include "gfx/png.h"
#include "platform/device.h"
#include "sys/adbserver.h"
#include "sys/logsave.h"
#include "sys/partitions.h"
#include "ui/screens/screens.h"

namespace tw {

static const char* kVersion = "2.0.0";

App::App(Display* d, EvdevInput* in, InputQueue* q) : display_(d), input_(in), queue_(q) {
    W = d->width();
    H = d->height();
    canvas.resize(W, H);
    ui.S = std::min(W / 400.0f, H / 780.0f);
    font::init();
    applySettings();
    bootTime_ = util::nowMs();
    lastInput_ = bootTime_;
    if (!Settings::get().str("pin_hash").empty()) locked_ = true;
    stack_.push_back(makeHomeScreen());
    stack_.back()->onShow(*this);
    // First boot: let the user pick display quality before anything else (older phones run smoother on low detail).
    if (Settings::get().integer("detail", -1) < 0 && !device::isSim()) {
        stack_.push_back(makeDetailScreen(true));
        stack_.back()->onShow(*this);
    }
}

void App::applySettings() {
    Settings& s = Settings::get();
    ui.th = makeTheme(s.integer("accent", 0), s.integer("style", 0));
    i18n::setLanguage(s.str("lang", "en"));
    // Detail level (slow phones): 0 = low (no shadows, glows or animations), 1 = high, -1 = not chosen yet (full).
    bool low = s.integer("detail", -1) == 0;
    canvas.setLowDetail(low);
    if (low) ui.th.glows = false;
    ui.animations = !low && s.flag("animations", device::animationsByDefault());
    ui.animSpeed = s.integer("anim_speed", 100) / 100.0f;
    topInset = ui.dp((float)s.integer("inset_top", device::isSim() ? 0 : 24));
    bottomInset = ui.dp((float)s.integer("inset_bottom", device::isSim() ? 0 : 8));
    display_->setRotate180(s.flag("rotate180"));
    display_->setSwapRB(s.flag("swap_rb"));
    if (input_) input_->setTransform(s.flag("touch_swap"), s.flag("touch_flipx"), s.flag("touch_flipy"));
}

void App::saveLogs() {
    auto where = logsave::save("asked", logsave::Removable::Mount);
    if (where.empty()) toast("Couldn't save the log", Icon::Warning, ui.th.danger);
    else toast("Logs saved in " + where.back(), Icon::Check, ui.th.ok);  // the last place is the easiest to reach
}

void App::vibrate(int ms) {
    if (Settings::get().flag("vibrate", true)) device::vibrate(ms);
}

// ---- navigation -----------------------------------------------------------------
struct PendingOp { int kind; std::unique_ptr<Screen> s; };
static std::vector<PendingOp> g_ops;

void App::push(std::unique_ptr<Screen> s) { g_ops.push_back({0, std::move(s)}); }
void App::pop() { g_ops.push_back({1, nullptr}); }
void App::home() { g_ops.push_back({2, nullptr}); }

void App::back() {
    if (consoleOpen_) { consoleOpen_ = false; return; }
    if (dialog_) { closeDialog(); return; }
    Screen* s = top();
    if (s && s->onBack(*this)) return;
    if (stack_.size() > 1) pop();
}

void App::dialog(Dialog d) {
    dialog_ = std::make_unique<Dialog>(std::move(d));
    dialogStart_ = util::nowMs();
}

void App::closeDialog() { dialog_.reset(); }

void App::confirm(const std::string& title, const std::string& msg, const std::string& slide, std::function<void()> yes,
                  bool danger, Icon icon) {
    Dialog d;
    d.title = title;
    d.message = msg;
    d.icon = icon;
    d.danger = danger;
    d.slideLabel = slide;
    d.buttons = {"Cancel"};
    d.onResult = [yes](int r) { if (r == 1) yes(); };
    dialog(std::move(d));
}

void App::toast(const std::string& msg, Icon icon, Color tint) {
    toast_ = msg;
    toastIcon_ = icon;
    toastTint_ = tint;
    toastStart_ = util::nowMs();
    LOGI("toast: %s", msg.c_str());
}

void App::runTask(const std::string& title, Icon icon, Task::Fn fn) {
    if (Task::get().running()) {
        toast("Another operation is still running", Icon::Warning, ui.th.warn);
        return;
    }
    // A flash cut short by a dead battery can leave the phone unbootable: ask first below 10%.
    device::Battery b = device::battery();
    if (!lowBatteryOk_ && b.percent >= 0 && b.percent < 10 && !b.charging) {
        confirm("Battery is low",
                "Battery at " + std::to_string(b.percent) + "% and not charging. If the phone dies while flashing, it may not boot any "
                "more - plug in the charger first.",
                "Swipe to continue anyway",
                [this, title, icon, fn] {
                    lowBatteryOk_ = true;
                    runTask(title, icon, fn);
                    lowBatteryOk_ = false;
                },
                true, Icon::Battery);
        return;
    }
    Task::get().start(title, std::move(fn));
    push(makeOperationScreen(icon));
}

void App::lockNow() {
    if (!Settings::get().str("pin_hash").empty()) {
        locked_ = true;
        pinEntry_.clear();
    }
}

std::string App::screenshot() {
    std::string dir = Partitions::get().storagePath() + "/Pictures/Screenshots";
    if (!util::mkdirs(dir)) dir = device::path("/tmp");
    std::string p = dir + "/fmrre_" + util::dateStamp() + ".png";
    if (savePng(canvas, p)) {
        toast("Screenshot saved", Icon::Camera);
        LOGI("Screenshot: %s", p.c_str());
        return p;
    }
    toast("Screenshot failed", Icon::Warning, ui.th.danger);
    return "";
}

// ---- main loop -------------------------------------------------------------------
void App::run() {
    int64_t lastFrame = 0;
    while (running_) {
        display_->pumpEvents(*queue_);
        bool busy = ui.wantsFrame() || transitioning_ || !pending_.empty() || Task::get().running() || dialog_ ||
                    !g_ops.empty() || util::nowMs() - bootTime_ < 1500;
        if (dimState_ == 2) busy = false;
        bool ambient = !busy && ui.wantsAmbient() && dimState_ == 0;
        int timeout = busy ? 0 : (ambient ? 30 : 250);
        if (input_) input_->poll(*queue_, timeout);
        else if (timeout) usleep(timeout * 1000);
        auto evs = queue_->drain();
        int64_t now = util::nowMs();
        if (!evs.empty()) {
            lastInput_ = now;
            if (dimState_ > 0) {
                // Wake up and swallow the waking input.
                if (dimState_ == 2) display_->setBlank(false);
                if (savedBrightness_ >= 0) device::setBrightness(savedBrightness_);
                dimState_ = 0;
                evs.clear();
                pending_.clear();
                lastStatus_ = 0;
                // Make sure a half-finished touch doesn't leave the UI thinking the finger is down.
                pending_.push_back({EvType::TouchUp, -1, -1});
            }
            pending_.insert(pending_.end(), evs.begin(), evs.end());
        }
        // Screen timeout
        int timeoutSec = Settings::get().integer("screen_timeout", 120);
        if (timeoutSec > 0 && dimState_ == 0 && now - lastInput_ > timeoutSec * 1000) {
            savedBrightness_ = device::brightness();
            device::setBrightness(std::max(1, device::brightnessMax() / 20));
            dimState_ = 1;
        } else if (dimState_ == 1 && now - lastInput_ > (timeoutSec + 15) * 1000 && !Task::get().running()) {
            display_->setBlank(true);
            dimState_ = 2;
            if (Settings::get().flag("lock_on_blank", true)) lockNow();
        }
        tickIdle();
        if (dimState_ == 2) continue;

        // One touch transition per frame so quick taps are never lost.
        std::vector<InputEvent> fe;
        while (!pending_.empty()) {
            InputEvent e = pending_.front();
            pending_.erase(pending_.begin());
            fe.push_back(e);
            if (e.type == EvType::TouchDown || e.type == EvType::TouchUp) break;
        }
        std::string clk = util::timeString(Settings::get().flag("clock24", true));
        bool ambientDue = ambient && now - lastFrame >= 66;
        if (!fe.empty() || busy || ambientDue || clk != clock_ || now - lastStatus_ > 2000) {
            if (now - lastFrame < 14) usleep((14 - (now - lastFrame)) * 1000);
            lastFrame = util::nowMs();
            frame(fe);
        }
    }
}

void App::tickIdle() {
    int64_t now = util::nowMs();
    if (!idleHook || now - lastIdle_ < 300) return;
    lastIdle_ = now;
    idleHook(*this);
}

void App::handleGlobalKeys(const std::vector<InputEvent>& evs) {
    for (auto& e : evs) {
        if (e.type == EvType::TouchDown) vibrate(10);
        if (e.type != EvType::KeyDown && e.type != EvType::KeyUp) continue;
        bool down = e.type == EvType::KeyDown;
        if (e.key == Key::VolDown) volDownHeld_ = down;
        if (e.key == Key::Power) powerHeld_ = down;
        if (down && volDownHeld_ && powerHeld_) {
            screenshot();
            continue;
        }
        if (!down) continue;
        if (locked_) continue;
        if (e.key == Key::Back || e.key == Key::Escape) back();
        if (e.key == Key::Home && !Task::get().running()) home();
    }
}

void App::startTransition(bool forward) {
    if (!ui.animations) return;
    snapshot_ = canvas;
    transitioning_ = true;
    transForward_ = forward;
    transStart_ = util::nowMs();
}

void App::updateStatus() {
    int64_t now = util::nowMs();
    clock_ = util::timeString(Settings::get().flag("clock24", true));
    if (now - lastStatus_ < 2000) return;
    lastStatus_ = now;
    auto b = device::battery();
    battery_ = b.percent;
    charging_ = b.charging;
    cpuTemp_ = device::cpuTempC();
}

void App::frame(const std::vector<InputEvent>& evs) {
    int64_t now = util::nowMs();
    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    handleGlobalKeys(evs);
    adbserver::setLocked(locked_);  // no adb shell or file transfer while the PIN lock is on
    updateStatus();
    tickIdle();
    ui.beginFrame(canvas, evs, now);
    canvas.setOrigin(0, 0);
    canvas.setAlpha(1);
    Rect full(0, 0, (float)W, (float)H);
    ui.background(full);

    float statusH = topInset + ui.dp(30);
    float navH = ui.dp(78) + bottomInset;
    Rect content(0, statusH, (float)W, H - statusH - navH);
    bool splash = now - bootTime_ < 1300 && ui.animations;
    bool overlay = dialog_ || consoleOpen_ || locked_ || splash;
    bool opRunning = Task::get().running();

    auto drawScreen = [&](Screen& s, bool canBack) {
        Rect area = content;
        if (s.hasHeader()) drawHeader(s, area.cutTop(ui.dp(66)), canBack);
        s.draw(ui, *this, area);
    };

    Screen* s = top();
    if (transitioning_) {
        float t = (now - transStart_) / (340.0f / ui.animSpeed);
        if (t >= 1) { transitioning_ = false; t = 1; }
        float e = easeOutCubic(t);
        ui.inputEnabled = false;
        canvas.pushClip(content);
        if (transForward_) {
            canvas.blit(snapshot_, -W * 0.28f * e, 0);
            canvas.fillRect(content, rgba(0, 0, 0, int(110 * e)));
            float x = W * (1 - e);
            canvas.setOrigin(x, 0);
            canvas.shadow(Rect(0, content.y, (float)W, content.h), 0, ui.dp(24), rgba(0, 0, 0, 120));
            canvas.pushClip(Rect(0, content.y, (float)W, content.h));
            ui.background(full);
            if (s) drawScreen(*s, stack_.size() > 1);
            canvas.popClip();
            canvas.setOrigin(0, 0);
        } else {
            canvas.setOrigin(-W * 0.28f * (1 - e), 0);
            if (s) drawScreen(*s, stack_.size() > 1);
            canvas.setOrigin(0, 0);
            canvas.fillRect(content, rgba(0, 0, 0, int(110 * (1 - e))));
            canvas.shadow(Rect(W * e, content.y, (float)W, content.h), 0, ui.dp(24), rgba(0, 0, 0, 120));
            canvas.blit(snapshot_, W * e, 0);
        }
        canvas.popClip();
        ui.keepAnimating();
    } else if (s) {
        ui.inputEnabled = !overlay;
        canvas.pushClip(content);
        drawScreen(*s, stack_.size() > 1);
        canvas.popClip();
    }

    ui.inputEnabled = !overlay && !transitioning_;
    drawNav(Rect(0, H - navH, (float)W, navH));
    drawStatusBar(Rect(0, 0, (float)W, statusH));

    ui.inputEnabled = !locked_ && !splash;
    if (consoleOpen_ || consoleAnim_ > 0.01f) {
        ui.inputEnabled = !locked_ && !splash && !dialog_;
        drawConsole();
    }
    ui.inputEnabled = !locked_ && !splash;
    if (dialog_) drawDialog();
    drawToast();
    ui.inputEnabled = !splash;
    if (locked_) drawLock();
    if (splash) drawSplash();
    ui.endFrame();

    // Apply deferred navigation now that the frame is fully drawn (snapshot = this frame).
    if (!g_ops.empty()) {
        auto ops = std::move(g_ops);
        g_ops.clear();
        bool forward = true, changed = false;
        for (auto& op : ops) {
            if (op.kind == 0) {
                stack_.push_back(std::move(op.s));
                stack_.back()->onShow(*this);
                forward = true;
                changed = true;
            } else if (op.kind == 1 && stack_.size() > 1) {
                graveyard_.push_back(std::move(stack_.back()));
                stack_.pop_back();
                stack_.back()->onShow(*this);
                forward = false;
                changed = true;
            } else if (op.kind == 2 && stack_.size() > 1) {
                while (stack_.size() > 1) {
                    graveyard_.push_back(std::move(stack_.back()));
                    stack_.pop_back();
                }
                stack_.back()->onShow(*this);
                forward = false;
                changed = true;
            }
        }
        if (changed) {
            startTransition(forward);
            ui.resetFocus();
        }
    }
    if (!transitioning_) graveyard_.clear();
    if (wasTaskRunning_ && !opRunning) vibrate(60);
    wasTaskRunning_ = opRunning;
    struct timespec ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    frameMsTotal += (ts1.tv_sec - ts0.tv_sec) * 1000.0 + (ts1.tv_nsec - ts0.tv_nsec) / 1e6;
    frameCount++;
    display_->present(canvas);
}

// ---- chrome ---------------------------------------------------------------------
void App::drawStatusBar(const Rect& r) {
    Theme& th = ui.th;
    Rect b(r.x + ui.dp(20), r.y + topInset, r.w - ui.dp(40), r.h - topInset);
    font::drawIn(canvas, Face::SemiBold, ui.dp(14), b, clock_, th.text);
    float x = b.r();
    // battery
    if (battery_ >= 0) {
        std::string pct = std::to_string(battery_) + "%";
        float tw = font::measure(Face::SemiBold, ui.dp(13), pct);
        font::drawIn(canvas, Face::SemiBold, ui.dp(13), Rect(x - tw, b.y, tw, b.h), pct, th.text);
        x -= tw + ui.dp(4);
        float bs = ui.dp(22);
        Rect bi(x - bs, b.cy() - bs / 2, bs, bs);
        icons::draw(canvas, Icon::Battery, bi.x, bi.y, bs, th.textDim);
        Color fc = battery_ <= 15 ? th.danger : (charging_ ? th.ok : th.text);
        float fw = (bs * 13.6f / 24) * battery_ / 100.0f;
        canvas.fillRoundRect(Rect(bi.x + bs * 4.3f / 24, bi.y + bs * 8.8f / 24, std::max(ui.dp(1.5f), fw), bs * 6.4f / 24),
                             ui.dp(1), fc);
        if (charging_) icons::drawCentered(canvas, Icon::Bolt, bi.x - ui.dp(6), b.cy(), ui.dp(13), th.ok);
        x = bi.x - (charging_ ? ui.dp(16) : ui.dp(6));
    }
    if (cpuTemp_ > -100) {
        char t[16];
        if (Settings::get().flag("fahrenheit")) snprintf(t, sizeof(t), "%.0f\xC2\xB0""F", cpuTemp_ * 9 / 5 + 32);
        else snprintf(t, sizeof(t), "%.0f\xC2\xB0""C", cpuTemp_);
        float tw = font::measure(Face::SemiBold, ui.dp(13), t);
        Color tc = cpuTemp_ > 70 ? th.danger : (cpuTemp_ > 55 ? th.warn : th.textDim);
        font::drawIn(canvas, Face::SemiBold, ui.dp(13), Rect(x - tw, b.y, tw, b.h), t, tc);
        x -= tw;
        icons::drawCentered(canvas, Icon::Thermo, x - ui.dp(8), b.cy(), ui.dp(15), tc);
        x -= ui.dp(22);
    }
    if (device::isSim()) ui.badge(b.x + font::measure(Face::SemiBold, ui.dp(14), clock_) + ui.dp(10), b.cy(), "SIM", th.warn);
}

void App::drawHeader(Screen& s, const Rect& r, bool canBack) {
    Theme& th = ui.th;
    float x = r.x + ui.dp(16);
    if (canBack) {
        Rect br(r.x + ui.dp(10), r.cy() - ui.dp(22), ui.dp(44), ui.dp(44));
        if (ui.iconButton(ui.id("hdr_back"), br, Icon::Back, th.text, true)) back();
        x = br.r() + ui.dp(12);
    }
    float aw = s.headerActionsWidth();
    float right = r.r() - ui.dp(12) - (aw > 0 ? ui.dp(aw + 8) : 0);
    if (aw > 0) s.headerActions(ui, *this, Rect(r.r() - ui.dp(aw + 12), r.y, ui.dp(aw), r.h));
    std::string sub = s.subtitle();
    if (sub.empty()) {
        font::drawIn(canvas, Face::Bold, ui.dp(24), Rect(x, r.y, right - x, r.h), s.title(), th.text);
    } else {
        font::drawIn(canvas, Face::Bold, ui.dp(22), Rect(x, r.cy() - ui.dp(23), right - x, ui.dp(28)), s.title(), th.text);
        font::drawIn(canvas, Face::Regular, ui.dp(12.5f), Rect(x, r.cy() + ui.dp(4), right - x, ui.dp(18)), sub, th.textDim);
    }
}

void App::drawNav(const Rect& r) {
    Theme& th = ui.th;
    float h = ui.dp(60);
    float w = std::min(r.w - ui.dp(32), ui.dp(360));
    Rect pill(r.cx() - w / 2, r.y + ui.dp(6), w, h);
    canvas.shadow(pill, h / 2, ui.dp(18), rgba(0, 0, 0, th.light ? 30 : 120), ui.dp(4));
    canvas.fillRoundRect(pill, h / 2, th.light ? rgb(0xFFFFFF) : mix(th.surface, th.surfaceHi, 0.4f));
    if (!th.light) canvas.strokeRoundRect(pill, h / 2, ui.dp(1), withAlpha(rgb(0xFFFFFF), 0.06f));
    bool op = Task::get().running() && top() && !top()->isOperation();
    struct NavBtn { Icon icon; const char* label; };
    NavBtn btns[] = {{Icon::Back, "Back"}, {Icon::Home, "Home"}, {op ? Icon::Refresh : Icon::Console, op ? "Task" : "Console"}};
    float bw = pill.w / 3;
    for (int i = 0; i < 3; i++) {
        Rect br(pill.x + i * bw, pill.y, bw, h);
        Id id = ui.id("nav", i);
        int res = ui.clickable(id, br.inset(ui.dp(6)));
        bool active = (i == 1 && depth() == 1) || (i == 2 && consoleOpen_);
        float a = ui.anim(id ^ 0x1717ull, active ? 1.f : 0.f, 14);
        Rect ind(br.cx() - ui.dp(28), br.y + ui.dp(8), ui.dp(56), ui.dp(28));
        if (a > 0.01f) canvas.fillRoundRect(ind.inset((1 - a) * ui.dp(14), 0), ui.dp(14), withAlpha(th.accent, 0.18f * a));
        ui.drawPressFx(id, br.inset(ui.dp(6)), ui.dp(22), th.text);
        Color col = mix(th.textDim, th.accent, a);
        if (i == 2 && op) {
            col = th.accent;
            float rot = (ui.now % 1200) / 1200.0f * 6.283f;
            canvas.strokeArc(ind.cx(), ind.cy(), ui.dp(9), ui.dp(2.2f), rot, 4.2f, th.accent);
            ui.keepAnimating();
        } else {
            icons::drawCentered(canvas, btns[i].icon, ind.cx(), ind.cy(), ui.dp(22), col);
        }
        font::drawIn(canvas, Face::SemiBold, ui.dp(11), Rect(br.x, br.y + ui.dp(37), br.w, ui.dp(16)), btns[i].label, col,
                     AlignCenter);
        if (res & Clicked) {
            if (i == 0) back();
            else if (i == 1) {
                consoleOpen_ = false;
                if (!Task::get().running() || !top() || !top()->isOperation()) home();
            } else if (op) {
                consoleOpen_ = false;
                push(makeOperationScreen(Icon::Refresh));
            } else {
                toggleConsole();
            }
        }
    }
}

void App::drawToast() {
    if (toast_.empty()) return;
    float t = (ui.now - toastStart_) / 1000.0f;
    if (t > 2.8f) { toast_.clear(); return; }
    float a = std::min(1.f, t * 6) * std::min(1.f, (2.8f - t) * 4);
    Theme& th = ui.th;
    float fs = ui.dp(14);
    float w = std::min(W - ui.dp(48), font::measure(Face::SemiBold, fs, toast_) + ui.dp(64));
    float y = H - bottomInset - ui.dp(150) + (1 - easeOutCubic(std::min(1.f, t * 4))) * ui.dp(30);
    Rect r(W / 2.f - w / 2, y, w, ui.dp(46));
    canvas.setAlpha(a);
    canvas.shadow(r, r.h / 2, ui.dp(16), rgba(0, 0, 0, 140), ui.dp(4));
    canvas.fillRoundRect(r, r.h / 2, th.light ? rgb(0x1E293B) : th.surfaceHi);
    Color tint = toastTint_ ? toastTint_ : th.accent;
    icons::drawCentered(canvas, toastIcon_, r.x + ui.dp(26), r.cy(), ui.dp(20), tint);
    font::drawIn(canvas, Face::SemiBold, fs, Rect(r.x + ui.dp(44), r.y, r.w - ui.dp(58), r.h), toast_,
                 th.light ? rgb(0xF8FAFC) : th.text);
    canvas.setAlpha(1);
    ui.keepAnimating();
}

void App::drawDialog() {
    Theme& th = ui.th;
    Dialog& d = *dialog_;
    float t = easeOutCubic((ui.now - dialogStart_) / (260.0f / ui.animSpeed));
    if (!ui.animations) t = 1;
    if (t < 1) ui.keepAnimating();
    canvas.fillRect(Rect(0, 0, (float)W, (float)H), rgba(0, 0, 0, int(150 * t)));
    Color tint = d.tint ? d.tint : (d.danger ? th.danger : th.accent);
    std::unique_ptr<Dialog> keep;  // callbacks may replace the dialog
    auto finish = [&](int result, bool action) {
        keep = std::move(dialog_);
        if (action && keep->onAction) keep->onAction(result);
        if (!action && keep->onResult) keep->onResult(result);
    };

    if (!d.actions.empty()) {
        // Bottom action sheet
        float rowH = ui.dp(56);
        float h = ui.dp(86) + rowH * d.actions.size() + ui.dp(12) + bottomInset;
        h = std::min(h, H * 0.85f);
        Rect sheet(ui.dp(8), H - h * t, W - ui.dp(16), h + ui.dp(30));
        if (ui.justReleased() && !sheet.contains(ui.touchX(), ui.touchY()) && !ui.dragging() && t >= 1) {
            finish(-1, true);
            return;
        }
        canvas.shadow(sheet, ui.dp(28), ui.dp(24), rgba(0, 0, 0, 150));
        canvas.fillRoundRect(sheet, ui.dp(28), th.light ? rgb(0xFFFFFF) : th.surface);
        canvas.fillRoundRect(Rect(sheet.cx() - ui.dp(20), sheet.y + ui.dp(10), ui.dp(40), ui.dp(4)), ui.dp(2), th.textFaint);
        font::drawIn(canvas, Face::Bold, ui.dp(19), Rect(sheet.x + ui.dp(24), sheet.y + ui.dp(24), sheet.w - ui.dp(48), ui.dp(28)),
                     d.title, th.text);
        if (!d.message.empty())
            font::drawIn(canvas, Face::Regular, ui.dp(13), Rect(sheet.x + ui.dp(24), sheet.y + ui.dp(52), sheet.w - ui.dp(48), ui.dp(20)),
                         d.message, th.textDim);
        Rect list(sheet.x + ui.dp(8), sheet.y + ui.dp(80), sheet.w - ui.dp(16), h - ui.dp(86) - bottomInset);
        ui.beginScroll(ui.id("sheet_scroll"), list);
        float y = list.y;
        for (size_t i = 0; i < d.actions.size(); i++) {
            Color ic = d.actions[i].second.find("Delete") != std::string::npos ? th.danger : th.accent;
            if (ui.listItem(ui.id("sheet", (int)i), Rect(list.x, y, list.w, rowH - ui.dp(4)), d.actions[i].first,
                            d.actions[i].second, "", "", ic, false) & Clicked) {
                ui.endScroll(y + rowH);
                finish((int)i, true);
                return;
            }
            y += rowH;
        }
        ui.endScroll(y);
        return;
    }

    float w = std::min(W - ui.dp(40), ui.dp(380));
    float pad = ui.dp(24);
    auto lines = font::wrap(Face::Regular, ui.dp(14.5f), d.message, w - pad * 2);
    float msgH = lines.size() * font::lineHeight(Face::Regular, ui.dp(14.5f));
    float h = pad + ui.dp(56) + ui.dp(14) + ui.dp(30) + ui.dp(8) + msgH + ui.dp(24) + ui.dp(54) + pad;
    if (!d.slideLabel.empty()) h += ui.dp(52);
    Rect card(W / 2.f - w / 2, H / 2.f - h / 2 + (1 - t) * ui.dp(40), w, h);
    canvas.setAlpha(t);
    canvas.shadow(card, ui.dp(28), ui.dp(30), rgba(0, 0, 0, 160), ui.dp(8));
    canvas.fillRoundRect(card, ui.dp(28), th.light ? rgb(0xFFFFFF) : th.surface);
    canvas.setAlpha(1);
    float y = card.y + pad;
    Rect ic(card.cx() - ui.dp(28), y, ui.dp(56), ui.dp(56));
    canvas.fillCircle(ic.cx(), ic.cy(), ui.dp(28), withAlpha(tint, 0.16f));
    icons::drawCentered(canvas, d.icon, ic.cx(), ic.cy(), ui.dp(28), tint);
    y += ui.dp(56) + ui.dp(14);
    font::drawIn(canvas, Face::Bold, ui.dp(20), Rect(card.x + pad, y, card.w - pad * 2, ui.dp(30)), d.title, th.text, AlignCenter);
    y += ui.dp(38);
    for (auto& l : lines) {
        float lw = font::measure(Face::Regular, ui.dp(14.5f), l);
        font::draw(canvas, Face::Regular, ui.dp(14.5f), card.cx() - lw / 2, y, l, th.textDim);
        y += font::lineHeight(Face::Regular, ui.dp(14.5f));
    }
    y += ui.dp(24);
    if (!d.slideLabel.empty()) {
        if (ui.slideToConfirm(ui.id("dlg_slide"), Rect(card.x + pad, y, card.w - pad * 2, ui.dp(58)), d.slideLabel, tint)) {
            finish(1, false);
            return;
        }
        y += ui.dp(66);
        if (ui.button(ui.id("dlg_cancel"), Rect(card.x + pad, y, card.w - pad * 2, ui.dp(46)), "Cancel", Icon::None,
                      BtnStyle::Ghost)) {
            finish(0, false);
            return;
        }
    } else {
        size_t n = std::max<size_t>(1, d.buttons.size());
        float gap = ui.dp(10);
        float bw = (card.w - pad * 2 - gap * (n - 1)) / n;
        for (size_t i = 0; i < n; i++) {
            std::string lab = d.buttons.empty() ? "OK" : d.buttons[i];
            BtnStyle st = i == n - 1 ? (d.danger ? BtnStyle::Danger : BtnStyle::Filled) : BtnStyle::Tonal;
            if (ui.button(ui.id("dlg_btn", (int)i), Rect(card.x + pad + i * (bw + gap), y, bw, ui.dp(50)), lab, Icon::None, st)) {
                finish((int)i, false);
                return;
            }
        }
    }
    if (ui.justReleased() && !card.contains(ui.touchX(), ui.touchY()) && !ui.dragging() && t >= 1 && d.slideLabel.empty())
        finish(-1, false);
}

void App::drawConsole() {
    Theme& th = ui.th;
    consoleAnim_ = ui.anim(ui.id("console_anim"), consoleOpen_ ? 1.f : 0.f, 12);
    float e = consoleAnim_;
    canvas.fillRect(Rect(0, 0, (float)W, (float)H), rgba(0, 0, 0, int(120 * e)));
    float h = H * 0.72f;
    Rect sheet(0, H - h * e, (float)W, h + ui.dp(30));
    if (consoleOpen_ && ui.justReleased() && ui.touchY() < sheet.y && !ui.dragging()) consoleOpen_ = false;
    canvas.shadow(sheet, ui.dp(28), ui.dp(24), rgba(0, 0, 0, 160));
    canvas.fillRoundRect(sheet, ui.dp(28), th.light ? rgb(0x0F172A) : mix(th.bgBottom, rgb(0x000000), 0.4f));
    canvas.fillRoundRect(Rect(sheet.cx() - ui.dp(20), sheet.y + ui.dp(10), ui.dp(40), ui.dp(4)), ui.dp(2), rgb(0x475569));
    Rect head(sheet.x + ui.dp(20), sheet.y + ui.dp(22), sheet.w - ui.dp(40), ui.dp(40));
    icons::drawCentered(canvas, Icon::Console, head.x + ui.dp(12), head.cy(), ui.dp(22), th.accent);
    font::drawIn(canvas, Face::Bold, ui.dp(19), Rect(head.x + ui.dp(34), head.y, head.w, head.h), "Console", rgb(0xF1F5F9));
    if (ui.iconButton(ui.id("con_close"), Rect(head.r() - ui.dp(40), head.y, ui.dp(40), ui.dp(40)), Icon::Close, rgb(0xCBD5E1), true))
        consoleOpen_ = false;
    if (ui.iconButton(ui.id("con_save"), Rect(head.r() - ui.dp(88), head.y, ui.dp(40), ui.dp(40)), Icon::Download, rgb(0xCBD5E1), true)) {
        saveLogs();
    }
    Rect body(sheet.x + ui.dp(12), head.b() + ui.dp(8), sheet.w - ui.dp(24), h - ui.dp(80) - bottomInset);
    Id sid = ui.id("console_scroll");
    static uint64_t lastGen = 0;
    uint64_t gen = log::generation();
    auto lines = log::tail(400);
    float fs = ui.dp(12.5f);
    float lh = font::lineHeight(Face::Mono, fs);
    if (gen != lastGen || (consoleOpen_ && e < 0.2f)) {
        lastGen = gen;
        // wrap computed below; jump to end next frame
        ui.value(sid ^ 0xE0Full) = 1;
    }
    ui.beginScroll(sid, body);
    float y = body.y;
    i18n::NoTranslate raw;  // log lines
    for (auto& l : lines) {
        Color col = rgb(0xCBD5E1);
        if (l.level == LogLevel::Error) col = th.danger;
        else if (l.level == LogLevel::Warn) col = th.warn;
        else if (l.level == LogLevel::Print) col = rgb(0xF8FAFC);
        else if (l.level == LogLevel::Debug) col = rgb(0x64748B);
        for (auto& wl : font::wrap(Face::Mono, fs, l.text, body.w - ui.dp(8))) {
            font::draw(canvas, Face::Mono, fs, body.x + ui.dp(4), y, wl, col);
            y += lh;
        }
    }
    ui.endScroll(y + ui.dp(8));
    if (ui.value(sid ^ 0xE0Full) > 0) {
        ui.scrollToEnd(sid);
        ui.value(sid ^ 0xE0Full) = 0;
        ui.keepAnimating();
    }
}

void App::drawLock() {
    Theme& th = ui.th;
    ui.background(Rect(0, 0, (float)W, (float)H));
    time_t tt = time(nullptr);
    struct tm tm;
    localtime_r(&tt, &tm);
    std::string date = i18n::longDate(tm.tm_wday, tm.tm_mday, tm.tm_mon);
    float y = topInset + ui.dp(70);
    font::drawIn(canvas, Face::Bold, ui.dp(64), Rect(0, y, (float)W, ui.dp(80)), util::timeString(Settings::get().flag("clock24", true)),
                 th.text, AlignCenter);
    y += ui.dp(80);
    font::drawIn(canvas, Face::Regular, ui.dp(16), Rect(0, y, (float)W, ui.dp(24)), date, th.textDim, AlignCenter);
    y += ui.dp(56);
    icons::drawCentered(canvas, Icon::Lock, W / 2.f, y, ui.dp(26), th.accent);
    y += ui.dp(26);
    font::drawIn(canvas, Face::SemiBold, ui.dp(15), Rect(0, y, (float)W, ui.dp(24)), "Enter PIN to unlock FMRRE", th.text, AlignCenter);
    y += ui.dp(40);
    // dots
    float shake = 0;
    if (pinShake_) {
        float st = (ui.now - pinShake_) / 400.0f;
        if (st < 1) { shake = std::sin(st * 30) * (1 - st) * ui.dp(14); ui.keepAnimating(); }
        else pinShake_ = 0;
    }
    int n = std::max<int>(4, (int)pinEntry_.size());
    float gap = ui.dp(22);
    float x0 = W / 2.f - (n - 1) * gap / 2 + shake;
    for (int i = 0; i < n; i++) {
        bool on = i < (int)pinEntry_.size();
        if (on) canvas.fillCircle(x0 + i * gap, y, ui.dp(7), pinShake_ ? th.danger : th.accent);
        else canvas.strokeCircle(x0 + i * gap, y, ui.dp(6), ui.dp(1.6f), th.textFaint);
    }
    y += ui.dp(36);
    // keypad
    float kw = ui.dp(78), kh = ui.dp(64);
    float gx = W / 2.f - kw * 1.5f - ui.dp(12);
    const char* keys[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "del", "0", "ok"};
    for (int i = 0; i < 12; i++) {
        int row = i / 3, col = i % 3;
        Rect kr(gx + col * (kw + ui.dp(12)), y + row * (kh + ui.dp(10)), kw, kh);
        Id id = ui.id("pin", i);
        int res = ui.clickable(id, kr);
        bool special = i == 9 || i == 11;
        canvas.fillRoundRect(kr, kh / 2, special ? th.surface2 : th.surface);
        ui.drawPressFx(id, kr, kh / 2, th.accent);
        if (i == 9) icons::drawCentered(canvas, Icon::Back, kr.cx(), kr.cy(), ui.dp(22), th.text);
        else if (i == 11) icons::drawCentered(canvas, Icon::Check, kr.cx(), kr.cy(), ui.dp(24), th.accent);
        else font::drawIn(canvas, Face::SemiBold, ui.dp(26), kr, keys[i], th.text, AlignCenter);
        if (res & Clicked) {
            if (i == 9) { if (!pinEntry_.empty()) pinEntry_.pop_back(); }
            else if (i == 11) {
                if (Sha256::hexOf(pinEntry_ + kPinSalt) == Settings::get().str("pin_hash")) {
                    locked_ = false;
                    toast("Unlocked", Icon::Lock);
                } else {
                    pinShake_ = ui.now;
                    vibrate(120);
                }
                pinEntry_.clear();
            } else if (pinEntry_.size() < 12) {
                pinEntry_ += keys[i];
            }
        }
    }
}

void App::drawSplash() {
    Theme& th = ui.th;
    float t = (ui.now - bootTime_) / 1300.0f;
    float out = std::clamp((t - 0.75f) / 0.25f, 0.f, 1.f);
    canvas.setAlpha(1 - out);
    canvas.gradientV(Rect(0, 0, (float)W, (float)H), th.bgTop, th.bgBottom);
    canvas.radialGlow(W / 2.f, H / 2.f, W * 0.8f, withAlpha(th.accent, 0.25f));
    float s = ui.dp(112) * (0.8f + 0.2f * easeOutBack(std::min(1.f, t * 2.2f)));
    Rect logo(W / 2.f - s / 2, H / 2.f - s / 2 - ui.dp(30), s, s);
    canvas.shadow(logo, s * 0.28f, ui.dp(30), withAlpha(th.accent, 0.5f), ui.dp(6));
    canvas.fillRoundRect(logo, s * 0.28f, th.accent);
    canvas.radialGlowRR(logo.r(), logo.b(), s, withAlpha(th.accent2, 0.9f), logo, s * 0.28f);
    drawLogoText(canvas, logo, s * 0.36f, th.onAccent);
    float sweep = 6.283f * easeInOutCubic(std::min(1.f, t * 1.4f));
    canvas.strokeArc(logo.cx(), logo.cy(), s * 0.78f, ui.dp(3), 0, sweep, withAlpha(th.accent, 0.8f));
    font::drawIn(canvas, Face::Bold, ui.dp(26), Rect(0, logo.b() + ui.dp(40), (float)W, ui.dp(34)), "FMRRE", th.text, AlignCenter);
    font::drawIn(canvas, Face::Regular, ui.dp(14), Rect(0, logo.b() + ui.dp(76), (float)W, ui.dp(22)),
                 std::string("FMR Recovery Environment \xE2\x80\xA2 ") + kVersion, th.textDim, AlignCenter);
    canvas.setAlpha(1);
    ui.keepAnimating();
}

}  // namespace tw
