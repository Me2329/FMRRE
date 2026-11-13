#include "core/task.h"
#include "core/log.h"
#include "core/util.h"

#include <cstdarg>

namespace tw {

Task& Task::get() {
    static Task t;
    return t;
}

bool Task::start(const std::string& title, Fn fn) {
    if (running_) return false;
    join();
    title_ = title;
    {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = "Starting...";
        detail_.clear();
        lines_.clear();
        totalLines_ = 0;
    }
    progress_ = 0;
    rangeStart_ = 0;
    rangeLen_ = 1;
    cancel_ = false;
    done_ = false;
    success_ = false;
    wantsRebootPrompt = false;
    running_ = true;
    startMs_ = util::nowMs();
    LOGI("=== Task started: %s ===", title.c_str());
    th_ = std::thread([this, fn]() {
        bool ok = false;
        try {
            ok = fn(*this);
        } catch (const std::exception& e) {
            print("Exception: %s", e.what());
        }
        success_ = ok;
        if (ok) progress_ = 1.0f;
        endMs_ = util::nowMs();
        LOGI("=== Task %s: %s (%s) ===", ok ? "succeeded" : "FAILED", title_.c_str(),
             util::humanDuration(endMs_ - startMs_).c_str());
        running_ = false;
        done_ = true;
    });
    return true;
}

void Task::join() {
    if (th_.joinable()) th_.join();
}

void Task::setProgress(float f) {
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    progress_ = f;
}

void Task::setStatus(const std::string& s) {
    std::lock_guard<std::mutex> lk(mu_);
    status_ = s;
}

std::string Task::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

void Task::setDetail(const std::string& s) {
    std::lock_guard<std::mutex> lk(mu_);
    detail_ = s;
}

std::string Task::detail() const {
    std::lock_guard<std::mutex> lk(mu_);
    return detail_;
}

void Task::print(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log::raw(LogLevel::Print, buf);
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& l : util::split(buf, '\n', false)) { lines_.push_back(l); totalLines_++; }
    if (lines_.size() > 500) lines_.erase(lines_.begin(), lines_.begin() + (lines_.size() - 500));
}

std::vector<std::string> Task::lines(size_t max) const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = std::min(max, lines_.size());
    return std::vector<std::string>(lines_.end() - n, lines_.end());
}

std::vector<std::string> Task::linesSince(size_t& cursor) const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t first = totalLines_ - lines_.size();
    if (cursor < first) cursor = first;
    std::vector<std::string> out(lines_.begin() + (cursor - first), lines_.end());
    cursor = totalLines_;
    return out;
}

}  // namespace tw
