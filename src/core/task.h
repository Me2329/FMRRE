#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tw {

// One long-running operation (flash, backup, wipe...) executed on a worker
// thread while the UI keeps animating. Only one task runs at a time.
class Task {
public:
    using Fn = std::function<bool(Task&)>;
    static Task& get();

    bool start(const std::string& title, Fn fn);
    void join();

    bool running() const { return running_; }
    bool done() const { return done_; }
    bool success() const { return success_; }
    const std::string& title() const { return title_; }
    int64_t startedMs() const { return startMs_; }
    int64_t endedMs() const { return endMs_; }

    // Progress: overall fraction 0..1. Sub-ranges let a step map its own 0..1
    // into a slice of the bar (used by update-binary "progress" commands).
    void setProgress(float f);
    float progress() const { return progress_; }
    void setRange(float start, float len) { rangeStart_ = start; rangeLen_ = len; }
    void setSubProgress(float f) { setProgress(rangeStart_ + rangeLen_ * f); }
    float rangeStart() const { return rangeStart_; }
    float rangeLen() const { return rangeLen_; }

    void setStatus(const std::string& s);
    std::string status() const;
    void setDetail(const std::string& s);  // e.g. "34.2 MB/s"
    std::string detail() const;

    void print(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    std::vector<std::string> lines(size_t max) const;

    void requestCancel() { cancel_ = true; }
    bool cancelled() const { return cancel_; }

    // Set when an installed zip asks for a reboot etc.
    std::atomic<bool> wantsRebootPrompt{false};

private:
    std::thread th_;
    std::atomic<bool> running_{false}, done_{false}, success_{false}, cancel_{false};
    std::atomic<float> progress_{0};
    float rangeStart_ = 0, rangeLen_ = 1;
    std::string title_;
    int64_t startMs_ = 0, endMs_ = 0;
    mutable std::mutex mu_;
    std::string status_, detail_;
    std::vector<std::string> lines_;
};

}  // namespace tw
