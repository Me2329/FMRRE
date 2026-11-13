#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/task.h"
#include "gfx/canvas.h"
#include "platform/display.h"
#include "platform/input.h"
#include "ui/ui.h"

namespace tw {

class App;

class Screen {
public:
    virtual ~Screen() = default;
    virtual std::string title() const = 0;
    virtual std::string subtitle() const { return ""; }
    virtual bool hasHeader() const { return true; }
    virtual void draw(Ui& ui, App& app, Rect area) = 0;
    virtual void onShow(App&) {}
    // Return true to consume the back action.
    virtual bool onBack(App&) { return false; }
    virtual void headerActions(Ui&, App&, Rect) {}
    virtual float headerActionsWidth() const { return 0; }  // in dp
    virtual bool isOperation() const { return false; }
};

struct Dialog {
    std::string title, message;
    Icon icon = Icon::Info;
    Color tint = 0;
    std::vector<std::string> buttons;          // last one is the primary action
    std::function<void(int)> onResult;         // index of pressed button (-1 = dismissed)
    std::string slideLabel;                    // if set: swipe-to-confirm replaces the primary button
    // Action sheet mode: a list of actions instead of buttons.
    std::vector<std::pair<Icon, std::string>> actions;
    std::function<void(int)> onAction;
    bool danger = false;
};

class App {
public:
    App(Display* d, EvdevInput* in, InputQueue* q);
    void run();
    // Render one frame with the given events (also used by the headless script runner).
    void frame(const std::vector<InputEvent>& evs);
    void quit() { running_ = false; }

    void push(std::unique_ptr<Screen> s);
    void pop();
    void home();
    Screen* top() { return stack_.empty() ? nullptr : stack_.back().get(); }
    size_t depth() const { return stack_.size(); }

    void dialog(Dialog d);
    void closeDialog();
    void confirm(const std::string& title, const std::string& msg, const std::string& slide,
                 std::function<void()> yes, bool danger = true, Icon icon = Icon::Warning);
    void toast(const std::string& msg, Icon icon = Icon::Info, Color tint = 0);
    void saveLogs();  // recovery log + kernel log + SELinux report to /cache/recovery, a card, readable storage
    void runTask(const std::string& title, Icon icon, Task::Fn fn);
    void toggleConsole() { consoleOpen_ = !consoleOpen_; }

    void applySettings();
    std::string screenshot();
    void lockNow();
    void vibrate(int ms = 18);

    // Called about three times a second, also while the screen is idle (OpenRecoveryScript / CLI queue).
    std::function<void(App&)> idleHook;

    Ui ui;
    Canvas canvas;
    int W = 0, H = 0;
    float topInset = 0, bottomInset = 0;
    double frameMsTotal = 0;
    int frameCount = 0;

private:
    void drawStatusBar(const Rect& r);
    void drawHeader(Screen& s, const Rect& r, bool canBack);
    void drawNav(const Rect& r);
    void drawDialog();
    void drawToast();
    void drawConsole();
    void drawLock();
    void drawSplash();
    void handleGlobalKeys(const std::vector<InputEvent>& evs);
    void back();
    void startTransition(bool forward);
    void updateStatus();
    void tickIdle();
    int64_t lastIdle_ = 0;

    Display* display_;
    EvdevInput* input_;
    InputQueue* queue_;
    bool running_ = true;
    std::vector<std::unique_ptr<Screen>> stack_;
    std::vector<std::unique_ptr<Screen>> graveyard_;

    // transitions
    Canvas snapshot_;
    bool transitioning_ = false, transForward_ = true;
    int64_t transStart_ = 0;

    // overlays
    std::unique_ptr<Dialog> dialog_;
    int64_t dialogStart_ = 0;
    std::string toast_;
    Icon toastIcon_ = Icon::Info;
    Color toastTint_ = 0;
    int64_t toastStart_ = 0;
    bool consoleOpen_ = false;
    float consoleAnim_ = 0;
    bool locked_ = false;
    std::string pinEntry_;
    int64_t pinShake_ = 0;
    int64_t bootTime_ = 0;

    // status cache
    int64_t lastStatus_ = 0;
    int battery_ = -1;
    bool charging_ = false;
    float cpuTemp_ = -1000;
    std::string clock_;

    // power management
    int64_t lastInput_ = 0;
    int dimState_ = 0;  // 0 on, 1 dimmed, 2 off
    int savedBrightness_ = -1;
    bool volDownHeld_ = false, powerHeld_ = false;
    std::vector<InputEvent> pending_;
    bool wasTaskRunning_ = false;
    bool lowBatteryOk_ = false;  // the user swiped past the low battery warning for this task
};

}  // namespace tw
