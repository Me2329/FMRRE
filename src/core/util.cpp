#include "core/util.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tw::util {

int64_t nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string timeString(bool h24, bool seconds) {
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[32];
    if (h24)
        strftime(buf, sizeof(buf), seconds ? "%H:%M:%S" : "%H:%M", &tm);
    else
        strftime(buf, sizeof(buf), seconds ? "%I:%M:%S %p" : "%I:%M %p", &tm);
    return buf;
}

std::string dateStamp() {
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d--%H-%M-%S", &tm);
    return buf;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char sep, bool skipEmpty) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            if (!skipEmpty || !cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!skipEmpty || !cur.empty()) out.push_back(cur);
    return out;
}

std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (isspace((unsigned char)c)) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
std::string toLower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string humanSize(uint64_t b) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    char buf[32];
    if (i == 0) snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b);
    else snprintf(buf, sizeof(buf), v < 10 ? "%.2f %s" : (v < 100 ? "%.1f %s" : "%.0f %s"), v, u[i]);
    return buf;
}

std::string humanDuration(int64_t ms) {
    int64_t s = ms / 1000;
    char buf[32];
    if (s >= 3600) snprintf(buf, sizeof(buf), "%lldh %02lldm", (long long)(s / 3600), (long long)(s / 60 % 60));
    else snprintf(buf, sizeof(buf), "%lld:%02lld", (long long)(s / 60), (long long)(s % 60));
    return buf;
}

std::string basename(const std::string& p) {
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    size_t i = s.rfind('/');
    return i == std::string::npos ? s : s.substr(i + 1);
}

std::string dirname(const std::string& p) {
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    size_t i = s.rfind('/');
    if (i == std::string::npos) return ".";
    if (i == 0) return "/";
    return s.substr(0, i);
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string shellQuote(const std::string& s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    return o + "'";
}

bool exists(const std::string& p) { struct stat st; return lstat(p.c_str(), &st) == 0; }
bool isDir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
bool isFile(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
uint64_t fileSize(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 ? st.st_size : 0; }

bool readFile(const std::string& p, std::string& out, size_t maxBytes) {
    int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    out.clear();
    char buf[8192];
    while (out.size() < maxBytes) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        out.append(buf, n);
    }
    close(fd);
    return true;
}

std::string readLine(const std::string& p) {
    std::string s;
    if (!readFile(p, s, 4096)) return "";
    size_t nl = s.find('\n');
    if (nl != std::string::npos) s.resize(nl);
    return trim(s);
}

bool writeFile(const std::string& p, const std::string& data) {
    int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return false; }
        off += n;
    }
    close(fd);
    return true;
}

bool mkdirs(const std::string& p, int mode) {
    if (p.empty()) return false;
    if (isDir(p)) return true;
    std::string parent = dirname(p);
    if (parent != p && !isDir(parent)) mkdirs(parent, mode);
    return mkdir(p.c_str(), mode) == 0 || errno == EEXIST;
}

bool removeTree(const std::string& p, const std::vector<std::string>& excludeTopLevel) {
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) return true;
    if (!S_ISDIR(st.st_mode)) return unlink(p.c_str()) == 0;
    bool ok = true;
    DIR* d = opendir(p.c_str());
    if (!d) return false;
    std::vector<std::string> names;
    while (auto* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        names.push_back(e->d_name);
    }
    closedir(d);
    for (auto& n : names) {
        if (std::find(excludeTopLevel.begin(), excludeTopLevel.end(), n) != excludeTopLevel.end()) continue;
        if (!removeTree(joinPath(p, n))) ok = false;
    }
    if (excludeTopLevel.empty() && rmdir(p.c_str()) != 0) ok = false;
    return ok;
}

bool copyFile(const std::string& from, const std::string& to, const std::function<void(uint64_t, uint64_t)>& progress) {
    int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    struct stat st;
    fstat(in, &st);
    uint64_t total = st.st_size;
    if (S_ISBLK(st.st_mode)) ioctl(in, BLKGETSIZE64, &total);  // stat() reports 0 for block devices
    int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_ISREG(st.st_mode) ? (st.st_mode & 0777) : 0644);
    if (out < 0) { close(in); return false; }
    std::vector<char> buf(1 << 20);
    uint64_t done = 0;
    bool ok = true;
    for (;;) {
        ssize_t n = read(in, buf.data(), buf.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (write(out, buf.data(), n) != n) { ok = false; break; }
        done += n;
        if (progress) progress(done, total);
    }
    fsync(out);
    close(in);
    close(out);
    return ok;
}

uint64_t dirSize(const std::string& p) {
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return st.st_size;
    uint64_t total = 0;
    DIR* d = opendir(p.c_str());
    if (!d) return 0;
    while (auto* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        total += dirSize(joinPath(p, e->d_name));
    }
    closedir(d);
    return total;
}

std::vector<DirEntry> listDir(const std::string& p, bool showHidden) {
    std::vector<DirEntry> out;
    DIR* d = opendir(p.c_str());
    if (!d) return out;
    while (auto* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!showHidden && e->d_name[0] == '.') continue;
        DirEntry de;
        de.name = e->d_name;
        std::string full = joinPath(p, de.name);
        struct stat st;
        if (lstat(full.c_str(), &st) == 0) {
            de.link = S_ISLNK(st.st_mode);
            struct stat st2;
            if (de.link && stat(full.c_str(), &st2) == 0) st = st2;
            de.dir = S_ISDIR(st.st_mode);
            de.size = st.st_size;
            de.mtime = st.st_mtime;
            de.mode = st.st_mode;
        }
        out.push_back(de);
    }
    closedir(d);
    return out;
}

std::string shellPath() {
    static const char* cands[] = {"/system/bin/sh", "/sbin/sh", "/bin/sh"};
    for (auto c : cands)
        if (access(c, X_OK) == 0) return c;
    return "/bin/sh";
}

bool haveBinary(const std::string& name) {
    std::string out;
    return runCapture("command -v " + name + " >/dev/null 2>&1", out) == 0;
}

int run(const std::string& cmd, const std::function<void(const std::string&)>& onLine) {
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, 0);
        std::string sh = shellPath();
        execl(sh.c_str(), "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(pfd[1]);
    std::string pending;
    char buf[4096];
    for (;;) {
        ssize_t n = read(pfd[0], buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        pending.append(buf, n);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            if (onLine) onLine(pending.substr(0, nl));
            pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty() && onLine) onLine(pending);
    close(pfd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int runCapture(const std::string& cmd, std::string& out) {
    out.clear();
    return run(cmd, [&](const std::string& l) { out += l; out += '\n'; });
}

}  // namespace tw::util
