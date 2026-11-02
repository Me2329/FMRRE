#include "ui/screens/screens.h"

#include <atomic>
#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "core/log.h"
#include "core/util.h"
#include "platform/device.h"

namespace tw {

namespace {

class TerminalScreen : public Screen {
public:
    TerminalScreen(const std::string& initial, const std::string& prefill) {
        cwd_ = device::isSim() ? device::simRoot() : "/";
        add("TWRP v2 terminal - commands run as root via " + util::shellPath());
        add("Tip: tap the keyboard icon to hide/show the keyboard.");
        if (!prefill.empty()) {
            add("Command ready below - check it, then press the run button.");
            input_ = prefill;
        }
        if (!initial.empty()) exec(initial);
    }
    ~TerminalScreen() override {
        if (running_) kill(pid_, SIGKILL);
        if (th_.joinable()) th_.join();
    }
    std::string title() const override { return "Terminal"; }
    std::string subtitle() const override { return running_ ? "running..." : cwd_; }

    float headerActionsWidth() const override { return 100; }
    void headerActions(Ui& ui, App&, Rect r) override {
        float s = ui.dp(44);
        Rect b(r.r() - s, r.cy() - s / 2, s, s);
        if (ui.iconButton(ui.id("t_kb"), b, Icon::Keyboard, kb_ ? ui.th.accent : ui.th.text, true)) kb_ = !kb_;
        b.x -= s + ui.dp(8);
        if (running_) {
            if (ui.iconButton(ui.id("t_kill"), b, Icon::Close, ui.th.danger, true)) {
                kill(pid_, SIGINT);
                add("^C");
            }
        } else if (ui.iconButton(ui.id("t_clear"), b, Icon::Trash, ui.th.text, true)) {
            std::lock_guard<std::mutex> lk(mu_);
            lines_.clear();
        }
    }

    void add(const std::string& l) {
        std::lock_guard<std::mutex> lk(mu_);
        lines_.push_back(l);
        if (lines_.size() > 3000) lines_.erase(lines_.begin(), lines_.begin() + 500);
        dirty_ = true;
    }

    void exec(const std::string& cmdIn) {
        std::string cmd = util::trim(cmdIn);
        if (cmd.empty() || running_) return;
        if (history_.empty() || history_.back() != cmd) history_.push_back(cmd);
        histPos_ = (int)history_.size();
        add("$ " + cmd);
        if (cmd == "clear") { std::lock_guard<std::mutex> lk(mu_); lines_.clear(); return; }
        if (cmd == "cd" || util::startsWith(cmd, "cd ")) {
            std::string t = util::trim(cmd.size() > 2 ? cmd.substr(3) : "/");
            std::string np = t.empty() || t == "~" ? (device::isSim() ? device::simRoot() : "/")
                             : (t[0] == '/' ? device::path(t) : util::joinPath(cwd_, t));
            char real[4096];
            if (realpath(np.c_str(), real) && util::isDir(real)) cwd_ = real;
            else add("cd: " + t + ": No such directory");
            return;
        }
        if (th_.joinable()) th_.join();
        int p[2];
        if (pipe2(p, O_CLOEXEC) != 0) return;
        pid_t pid = fork();
        if (pid == 0) {
            dup2(p[1], 1);
            dup2(p[1], 2);
            setpgid(0, 0);
            if (chdir(cwd_.c_str()) != 0) {}
            setenv("TERM", "dumb", 1);
            setenv("PATH", "/system/bin:/sbin:/bin:/usr/bin:/vendor/bin:/system/xbin", 0);
            std::string sh = util::shellPath();
            execl(sh.c_str(), "sh", "-c", cmd.c_str(), (char*)nullptr);
            _exit(127);
        }
        close(p[1]);
        pid_ = pid;
        running_ = true;
        int fd = p[0];
        th_ = std::thread([this, fd, pid]() {
            std::string pending;
            char buf[4096];
            for (;;) {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                pending.append(buf, n);
                size_t nl;
                while ((nl = pending.find('\n')) != std::string::npos) {
                    std::string l = pending.substr(0, nl);
                    for (auto& ch : l) if (ch == '\t') ch = ' ';
                    if (!l.empty() && l.back() == '\r') l.pop_back();
                    add(l);
                    pending.erase(0, nl + 1);
                }
            }
            if (!pending.empty()) add(pending);
            close(fd);
            int st = 0;
            waitpid(pid, &st, 0);
            int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            if (rc != 0) add("[exit " + std::to_string(rc) + "]");
            running_ = false;
        });
    }

    void draw(Ui& ui, App&, Rect area) override {
        Theme& th = ui.th;
        float pad = ui.dp(12);
        if (kb_) {
            Rect kbr = area.cutBottom(ui.keyboardHeight());
            if (ui.keyboard(ui.id("t_kbd"), kbr, input_)) {
                exec(input_);
                input_.clear();
            }
        } else {
            input_ += ui.takeTypedText();
        }
        // History via vol keys when keyboard shown is awkward; offer buttons instead.
        Rect inRow = area.cutBottom(ui.dp(64)).inset(pad, ui.dp(6));
        float bs = inRow.h;
        if (ui.iconButton(ui.id("t_hist"), Rect(inRow.x, inRow.y, bs, bs), Icon::Up, th.textDim, true) && !history_.empty()) {
            histPos_ = std::max(0, histPos_ - 1);
            input_ = history_[histPos_];
        }
        Rect field(inRow.x + bs + ui.dp(8), inRow.y, inRow.w - bs * 2 - ui.dp(16), inRow.h);
        ui.c->fillRoundRect(field, ui.dp(14), th.surface2);
        std::string shown = "$ " + input_;
        float fs = ui.dp(14);
        while (shown.size() > 2 && font::measure(Face::Mono, fs, shown) > field.w - ui.dp(24)) shown.erase(2, 1);
        font::drawIn(*ui.c, Face::Mono, fs, field.inset(ui.dp(12), 0), shown, th.text);
        if ((ui.now / 530) % 2 == 0) {
            float cx = field.x + ui.dp(12) + font::measure(Face::Mono, fs, shown);
            ui.c->fillRect(Rect(cx, field.cy() - ui.dp(9), ui.dp(8), ui.dp(18)), withAlpha(th.accent, 0.8f));
        }
        ui.ambient();
        Rect runR(field.r() + ui.dp(8), inRow.y, bs, bs);
        ui.c->fillRoundRect(runR, bs / 2, running_ ? th.surfaceHi : th.accent);
        if (ui.iconButton(ui.id("t_run"), runR, running_ ? Icon::Timer : Icon::Forward, running_ ? th.textDim : th.onAccent, false)) {
            exec(input_);
            input_.clear();
        }
        // Quick commands
        Rect chips = area.cutBottom(ui.dp(46));
        static const char* quick[] = {"ls -la", "df -h", "getprop ro.product.device", "dmesg | tail -n 40", "logcat -d | tail -n 60",
                                      "cat /proc/partitions", "mount", "ip addr", "uname -a", "free -m"};
        float qx = pad;
        for (int i = 0; i < 10; i++) {
            float w = ui.chipWidth(quick[i]);
            Rect cr(qx - chipScroll_, chips.y + ui.dp(4), w, ui.dp(36));
            if (cr.r() > 0 && cr.x < area.r()) {
                if (ui.chip(ui.id("tq", i), cr, quick[i], false)) exec(quick[i]);
            }
            qx += w + ui.dp(8);
        }
        // horizontal scroll of chips via drag
        float maxScroll = std::max(0.f, qx - area.w);
        if (ui.touchDown() && ui.dragging() && chips.contains(ui.touchX(), ui.touchY() - ui.c->oy())) {
            chipScroll_ = std::clamp(chipScroll_ - (ui.touchX() - lastX_), 0.f, maxScroll);
        }
        lastX_ = ui.touchX();

        // Output
        Rect out = area.inset(pad, ui.dp(4));
        ui.card(out, ui.dp(16), th.light ? rgb(0x0F172A) : mix(th.bgBottom, rgb(0x000000), 0.5f), false);
        Rect body = out.inset(ui.dp(10));
        float ofs = ui.dp(12.5f);
        float lh = font::lineHeight(Face::Mono, ofs);
        Id sid = ui.id("t_scroll");
        std::vector<std::string> snap;
        bool dirty;
        {
            std::lock_guard<std::mutex> lk(mu_);
            snap = lines_;
            dirty = dirty_;
            dirty_ = false;
        }
        ui.beginScroll(sid, body);
        float y = body.y;
        Rect clip = ui.c->clipLocal();
        for (auto& l : snap) {
            auto wl = font::wrap(Face::Mono, ofs, l, body.w - ui.dp(4));
            for (auto& w : wl) {
                if (y + lh >= clip.y && y <= clip.b()) {
                    Color col = util::startsWith(l, "$ ") ? th.accent : (util::startsWith(l, "[exit") ? th.danger : rgb(0xE2E8F0));
                    font::draw(*ui.c, Face::Mono, ofs, body.x, y, w, col);
                }
                y += lh;
            }
        }
        ui.endScroll(y + ui.dp(6));
        if (dirty) { ui.scrollToEnd(sid); ui.keepAnimating(); }
        if (running_) ui.keepAnimating();
    }

private:
    std::mutex mu_;
    std::vector<std::string> lines_;
    bool dirty_ = false;
    std::atomic<bool> running_{false};
    pid_t pid_ = -1;
    std::thread th_;
    std::string input_, cwd_;
    std::vector<std::string> history_;
    int histPos_ = 0;
    bool kb_ = true;
    float chipScroll_ = 0, lastX_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeTerminalScreen(const std::string& initialCmd, const std::string& prefill) {
    return std::make_unique<TerminalScreen>(initialCmd, prefill);
}

}  // namespace tw
