#include "sys/adbserver.h"

#include <condition_variable>
#include <deque>
#include <dirent.h>
#include <map>
#include <mutex>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <vector>

#include "sys/adblink.h"
#include "sys/pathcheck.h"

extern char** environ;

namespace tw::adbserver {

namespace {

using namespace adbd::detail;

std::atomic<bool> g_locked{false};
const char kLockedMsg[] = "FMRRE is locked: enter the PIN on the phone first.";

// ---- streams ---------------------------------------------------------------------------------------
// adb runs several streams over one link. Each side may send one WRTE per stream and then waits for its OKAY, so a
// slow reader holds the sender back: here the OKAY goes out when the service has taken the data, never before.

struct Conn;

struct Stream {
    Conn* conn = nullptr;
    uint32_t local = 0, remote = 0;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> in;  // from the computer, not yet taken by the service
    bool ready = true;           // the computer has acknowledged our last WRTE
    bool closed = false;         // closed by the computer, or the link is gone

    bool write(const char* p, size_t n);
    bool write(const std::string& s) { return write(s.data(), s.size()); }
    bool read(std::string& chunk);
    void markClosed() {
        {
            std::lock_guard<std::mutex> l(mu);
            closed = true;
        }
        cv.notify_all();
    }
    bool isClosed() {
        std::lock_guard<std::mutex> l(mu);
        return closed;
    }
};

struct Conn {
    std::unique_ptr<Link> link;
    std::mutex sendMu, mu;
    uint32_t maxData = 4096, nextId = 1;
    std::map<uint32_t, std::shared_ptr<Stream>> streams;
    std::atomic<int> workers{0};

    bool send(uint32_t cmd, uint32_t a0, uint32_t a1, const std::string& data = "") {
        std::lock_guard<std::mutex> l(sendMu);
        return sendPacket(*link, cmd, a0, a1, data);
    }
    std::shared_ptr<Stream> find(uint32_t local) {
        std::lock_guard<std::mutex> l(mu);
        auto it = streams.find(local);
        return it == streams.end() ? nullptr : it->second;
    }
    void drop(uint32_t local) {
        std::lock_guard<std::mutex> l(mu);
        streams.erase(local);
    }
    void closeAll() {
        std::map<uint32_t, std::shared_ptr<Stream>> all;
        {
            std::lock_guard<std::mutex> l(mu);
            all.swap(streams);
        }
        for (auto& s : all) s.second->markClosed();
    }
    // The services notice their closed streams and leave (a running command is hung up first).
    bool drain(int ms) {
        for (int i = 0; i < ms / 50 && workers > 0; i++) usleep(50 * 1000);
        return workers == 0;
    }
};

bool Stream::write(const char* p, size_t n) {
    while (n) {
        {
            std::unique_lock<std::mutex> l(mu);
            cv.wait(l, [&] { return ready || closed; });
            if (closed) return false;
            ready = false;
        }
        size_t k = std::min<size_t>(n, conn->maxData);
        if (!conn->send(A_WRTE, local, remote, std::string(p, k))) return false;
        p += k;
        n -= k;
    }
    return true;
}

bool Stream::read(std::string& chunk) {
    {
        std::unique_lock<std::mutex> l(mu);
        cv.wait(l, [&] { return !in.empty() || closed; });
        if (in.empty()) return false;
        chunk = std::move(in.front());
        in.pop_front();
    }
    conn->send(A_OKAY, local, remote);  // taken: the computer may send its next piece
    return true;
}

// The service is done: tell the computer (unless it closed first) and forget the stream.
void finish(Conn& c, Stream& s) {
    if (!s.isClosed()) c.send(A_CLSE, s.local, s.remote);
    s.markClosed();
    c.drop(s.local);
}

std::string u32le(uint32_t v) {
    char b[4];
    put32((uint8_t*)b, v);
    return std::string(b, 4);
}
uint32_t rd32(const std::string& s, size_t off) { return le32((const uint8_t*)s.data() + off); }

// ---- shell -----------------------------------------------------------------------------------------

std::string findShell() {
    if (getenv("FMRRE_ADB_NOSH")) return "";  // tests: a ramdisk without a shell
    for (auto p : {"/system/bin/sh", "/sbin/sh", "/bin/sh", "/vendor/bin/sh"})
        if (access(p, X_OK) == 0) return p;
    return "";
}

const char kDevicePath[] = "/sbin:/system/bin:/vendor/bin:/system/xbin:/bin:/usr/bin";

std::string findInPath(const std::string& name) {
    if (name.find('/') != std::string::npos) return access(name.c_str(), X_OK) == 0 ? name : "";
    const char* env = device::isSim() ? getenv("PATH") : nullptr;
    for (auto& dir : util::split(env ? env : kDevicePath, ':')) {
        std::string p = dir + "/" + name;
        struct stat st;
        if (!dir.empty() && stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0) return p;
    }
    return "";
}

// A command line without a shell to read it: words, with '...', "..." and backslashes.
std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool have = false;
    char quote = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char ch = s[i];
        if (quote) {
            if (ch == quote) quote = 0;
            else if (ch == '\\' && quote == '"' && i + 1 < s.size()) cur += s[++i];
            else cur += ch;
        } else if (ch == '\'' || ch == '"') {
            quote = ch;
            have = true;
        } else if (ch == '\\' && i + 1 < s.size()) {
            cur += s[++i];
            have = true;
        } else if (ch == ' ' || ch == '\t' || ch == '\n') {
            if (have || !cur.empty()) out.push_back(cur);
            cur.clear();
            have = false;
        } else {
            cur += ch;
        }
    }
    if (have || !cur.empty()) out.push_back(cur);
    return out;
}

// "shell:ls -l", "shell,v2,raw:ls", "shell,v2,TERM=xterm-256color,pty:", "exec:cat file"
bool parseShell(const std::string& svc, bool& v2, bool& raw, bool& pty, std::string& term, std::string& cmd) {
    size_t colon = svc.find(':');
    if (colon == std::string::npos) return false;
    auto parts = util::split(svc.substr(0, colon), ',');
    cmd = svc.substr(colon + 1);
    if (parts.empty()) return false;
    if (parts[0] == "exec") {
        raw = true;
        return true;
    }
    if (parts[0] != "shell") return false;
    for (size_t i = 1; i < parts.size(); i++) {
        if (parts[i] == "v2") v2 = true;
        else if (parts[i] == "raw") raw = true;
        else if (parts[i] == "pty") pty = true;
        else if (util::startsWith(parts[i], "TERM=")) term = parts[i].substr(5);
    }
    return true;
}

struct Child {
    pid_t pid = -1;
    int in = -1, out = -1, err = -1;  // our ends (with a terminal, in and out are the same fd)
    bool pty = false;
};

bool writeAll(int fd, const char* p, size_t n) {
    while (n) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= size_t(w);
    }
    return true;
}

bool spawn(const std::string& exe, const std::vector<std::string>& argv, bool pty, bool mergeErr, const std::string& term, Child& c,
           std::string& why) {
    int pin[2] = {-1, -1}, pout[2] = {-1, -1}, perr[2] = {-1, -1}, master = -1;
    std::string slave;
    if (pty) {
        master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        const char* name = master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0 ? ptsname(master) : nullptr;
        if (name) {
            slave = name;
        } else {  // no /dev/ptmx or devpts in this ramdisk: plain pipes
            if (master >= 0) close(master);
            master = -1;
            pty = false;
        }
    }
    if (!pty && (pipe2(pin, O_CLOEXEC) != 0 || pipe2(pout, O_CLOEXEC) != 0 || (!mergeErr && pipe2(perr, O_CLOEXEC) != 0))) {
        why = std::string("pipe: ") + strerror(errno);
        for (int fd : {pin[0], pin[1], pout[0], pout[1], perr[0], perr[1]})
            if (fd >= 0) close(fd);
        return false;
    }
    // Everything the child needs is made before fork(): after it, in a program with threads, only system calls are safe.
    std::vector<std::string> envs;
    if (device::isSim()) {
        for (char** e = environ; e && *e; e++)
            if (strncmp(*e, "TERM=", 5) != 0) envs.push_back(*e);
    } else {
        envs = {std::string("PATH=") + kDevicePath, "HOME=/", "ANDROID_ROOT=/system", "ANDROID_DATA=/data", "EXTERNAL_STORAGE=/sdcard",
                "TMPDIR=/tmp"};
    }
    envs.push_back("TERM=" + (term.empty() ? std::string("dumb") : term));
    std::vector<char*> av, ev;
    for (auto& a : argv) av.push_back((char*)a.c_str());
    av.push_back(nullptr);
    for (auto& e : envs) ev.push_back((char*)e.c_str());
    ev.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        why = std::string("fork: ") + strerror(errno);
        for (int fd : {pin[0], pin[1], pout[0], pout[1], perr[0], perr[1], master})
            if (fd >= 0) close(fd);
        return false;
    }
    if (pid == 0) {
        setsid();  // its own session and group: a hang-up reaches everything the command started
        if (pty) {
            int s = ::open(slave.c_str(), O_RDWR);
            if (s < 0) _exit(126);
            ioctl(s, TIOCSCTTY, 0);
            dup2(s, 0);
            dup2(s, 1);
            dup2(s, 2);
            if (s > 2) close(s);
        } else {
            dup2(pin[0], 0);
            dup2(pout[1], 1);
            dup2(mergeErr ? pout[1] : perr[1], 2);
        }
        signal(SIGPIPE, SIG_DFL);
        if (!device::isSim() && chdir("/") != 0) {
        }
        execve(exe.c_str(), av.data(), ev.data());
        _exit(127);
    }
    c.pid = pid;
    c.pty = pty;
    if (pty) {
        c.in = c.out = master;
    } else {
        close(pin[0]);
        close(pout[1]);
        if (perr[1] >= 0) close(perr[1]);
        c.in = pin[1];
        c.out = pout[0];
        c.err = perr[0];
    }
    return true;
}

void shellService(Conn& conn, Stream& s, bool v2, bool raw, bool wantPty, const std::string& term, const std::string& cmd) {
    // To the computer: shell protocol v2 frames ([id][length][data]; 1 = stdout, 2 = stderr, 3 = exit code), or the
    // plain bytes of the old protocol.
    auto out = [&](int id, const char* p, size_t n) {
        if (!v2) return s.write(p, n);
        std::string f(1, char(id));
        f += u32le(uint32_t(n));
        f.append(p, n);
        return s.write(f);
    };
    auto leave = [&](const std::string& msg, int code) {
        if (!msg.empty()) out(2, msg.data(), msg.size());
        char e = char(code);
        if (v2) out(3, &e, 1);
        finish(conn, s);
    };
    if (g_locked) return leave(std::string(kLockedMsg) + "\n", 1);

    std::string sh = findShell(), exe;
    std::vector<std::string> argv;
    if (cmd.empty()) {
        if (sh.empty())
            return leave("This recovery's ramdisk has no shell (sh), so there is no interactive session.\n"
                         "Commands run directly all the same:  adb shell fmrre help\n", 127);
        argv = {"sh"};
        exe = sh;
    } else if (!sh.empty()) {
        argv = {"sh", "-c", cmd};
        exe = sh;
    } else {
        argv = splitWords(cmd);
        if (argv.empty()) return leave("", 0);
        std::string base = util::basename(argv[0]);
        if (base == "fmrre" || base == "twrp") {  // FMRRE's own command line: this very program
            exe = "/proc/self/exe";
            argv[0] = base;
            if (device::isSim()) argv.insert(argv.begin(), {"recovery", "--sim", "--sim-root", device::simRoot()});
        } else {
            exe = findInPath(argv[0]);
            if (exe.empty())
                return leave(argv[0] + ": not found. (This ramdisk has no shell: only programs can be run - try: fmrre help)\n", 127);
        }
    }
    Child c;
    std::string why;
    if (!spawn(exe, argv, !raw && (wantPty || cmd.empty()), !v2, term, c, why)) return leave("adb shell: " + why + "\n", 126);

    std::atomic<bool> exited{false};
    auto pump = [&](int fd, int id) {  // the command's output, to the computer
        std::vector<char> buf(32 * 1024);
        for (;;) {
            pollfd p{fd, POLLIN, 0};
            int r = poll(&p, 1, 100);
            if (r == 0) {
                if (exited) break;  // gone, and nothing left to read (a grandchild may keep the pipe open for ever)
                continue;
            }
            if (r < 0 && errno == EINTR) continue;
            ssize_t n = r < 0 ? -1 : ::read(fd, buf.data(), buf.size());
            if (n <= 0 || !out(id, buf.data(), size_t(n))) break;  // end of output (EIO on a terminal), or the computer left
        }
    };
    std::thread tOut(pump, c.out, 1), tErr;
    if (c.err >= 0) tErr = std::thread(pump, c.err, 2);
    std::atomic<int> in{c.in};
    std::thread tIn([&] {  // the computer's input, to the command
        std::string chunk, buf;
        while (s.read(chunk)) {
            int fd = in;
            if (!v2) {
                if (fd >= 0) writeAll(fd, chunk.data(), chunk.size());
                continue;
            }
            buf += chunk;
            while (buf.size() >= 5) {
                uint32_t len = rd32(buf, 1);
                if (len > kMaxPayload) return;
                if (buf.size() < 5 + size_t(len)) break;
                int id = (unsigned char)buf[0];
                if (id == 0 && fd >= 0) {
                    writeAll(fd, buf.data() + 5, len);
                } else if (id == 4 && !c.pty && fd >= 0) {  // end of input
                    in = -1;
                    close(fd);
                    fd = -1;
                } else if (id == 5 && c.pty) {  // the window's size: "rows x cols, xpixels x ypixels"
                    struct winsize ws {};
                    unsigned rows = 0, cols = 0, xp = 0, yp = 0;
                    if (sscanf(buf.substr(5, len).c_str(), "%ux%u,%ux%u", &rows, &cols, &xp, &yp) >= 2) {
                        ws.ws_row = (unsigned short)rows;
                        ws.ws_col = (unsigned short)cols;
                        ws.ws_xpixel = (unsigned short)xp;
                        ws.ws_ypixel = (unsigned short)yp;
                        ioctl(c.out, TIOCSWINSZ, &ws);
                    }
                }
                buf.erase(0, 5 + size_t(len));
            }
        }
        // The computer closed the stream (Ctrl-C, the window closed, the cable pulled): the command goes with it.
        if (!exited) kill(-c.pid, SIGHUP);
        for (int i = 0; i < 40 && !exited; i++) usleep(50 * 1000);
        if (!exited) kill(-c.pid, SIGKILL);
    });

    int status = 0;
    while (waitpid(c.pid, &status, 0) < 0 && errno == EINTR) {
    }
    exited = true;
    tOut.join();
    if (tErr.joinable()) tErr.join();
    char code = char(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
    if (v2) out(3, &code, 1);
    finish(conn, s);  // also wakes tIn
    tIn.join();
    int fin = in;
    if (fin >= 0) close(fin);
    if (!c.pty && c.out >= 0) close(c.out);
    if (c.err >= 0) close(c.err);
}

// ---- sync: adb pull / push / ls ---------------------------------------------------------------------
// Requests are an id (STAT, LIST, RECV, SEND, QUIT) and a length; file data travels in DATA pieces of up to 64 KiB.

void syncService(Conn& conn, Stream& s) {
    std::string buf;
    auto need = [&](size_t n) {
        while (buf.size() < n) {
            std::string c;
            if (!s.read(c)) return false;
            buf += c;
        }
        return true;
    };
    auto fail = [&](const std::string& msg) { return s.write("FAIL" + u32le(uint32_t(msg.size())) + msg); };
    auto errnoMsg = [](const std::string& what) { return what + ": " + strerror(errno); };
    for (;;) {
        if (!need(8)) break;
        std::string id = buf.substr(0, 4);
        uint32_t len = rd32(buf, 4);
        if (id == "QUIT" || len > 4096 || !need(8 + size_t(len))) break;
        std::string arg = buf.substr(8, len);
        buf.erase(0, 8 + size_t(len));
        const bool locked = g_locked;

        if (id == "STAT") {
            struct stat st {};
            bool ok = !locked && lstat(pathcheck::resolve(arg).c_str(), &st) == 0;
            if (!s.write("STAT" + u32le(ok ? st.st_mode : 0) + u32le(ok ? uint32_t(st.st_size) : 0) + u32le(ok ? uint32_t(st.st_mtime) : 0))) break;
        } else if (id == "LIST") {
            std::string dir = pathcheck::resolve(arg), all;
            DIR* d = locked ? nullptr : opendir(dir.c_str());
            while (auto* e = d ? readdir(d) : nullptr) {
                struct stat st {};
                if (lstat((dir + "/" + e->d_name).c_str(), &st) != 0) continue;
                std::string name = e->d_name;
                all += "DENT" + u32le(st.st_mode) + u32le(uint32_t(st.st_size)) + u32le(uint32_t(st.st_mtime)) + u32le(uint32_t(name.size())) + name;
            }
            if (d) closedir(d);
            if (!s.write(all + "DONE" + std::string(16, '\0'))) break;
        } else if (id == "RECV") {  // pull
            std::string path = pathcheck::resolve(arg);
            struct stat st {};
            int fd = locked ? -1 : ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0 || fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
                std::string why = locked ? kLockedMsg : fd < 0 ? errnoMsg(arg) : arg + ": is a folder";
                if (fd >= 0) close(fd);
                if (!fail(why)) break;
                continue;
            }
            std::string piece(8 + 64 * 1024, '\0');
            bool ok = true;
            for (;;) {
                ssize_t n = ::read(fd, &piece[8], 64 * 1024);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) {
                    ok = fail(errnoMsg("reading " + arg));
                    break;
                }
                if (n == 0) {
                    ok = s.write("DONE" + u32le(0));
                    break;
                }
                memcpy(&piece[0], "DATA", 4);
                put32((uint8_t*)&piece[4], uint32_t(n));
                if (!s.write(piece.data(), 8 + size_t(n))) {
                    ok = false;
                    break;
                }
            }
            close(fd);
            if (!ok) break;
        } else if (id == "SEND") {  // push: "path,mode", then DATA..., then DONE with the file's time
            size_t comma = arg.rfind(',');
            std::string path = pathcheck::resolve(arg.substr(0, comma));
            uint32_t mode = comma == std::string::npos ? 0644 : uint32_t(strtoul(arg.c_str() + comma + 1, nullptr, 10));
            std::string error = locked ? kLockedMsg : "", link;
            int fd = -1;
            if (error.empty()) {
                util::mkdirs(util::dirname(path));
                if (S_ISLNK(mode)) {
                    unlink(path.c_str());
                } else {
                    fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode & 0777 ? mode & 0777 : 0644);
                    if (fd < 0) error = errnoMsg(arg.substr(0, comma));
                }
            }
            uint32_t mtime = 0;
            bool gone = false;
            for (;;) {  // every piece is read even after an error: the computer only listens once it has sent DONE
                if (!need(8)) { gone = true; break; }
                std::string pid = buf.substr(0, 4);
                uint32_t n = rd32(buf, 4);
                if (pid == "DONE") {
                    mtime = n;
                    buf.erase(0, 8);
                    break;
                }
                if (pid != "DATA" || n > kMaxPayload || !need(8 + size_t(n))) { gone = true; break; }
                if (error.empty()) {
                    if (S_ISLNK(mode)) link.append(buf, 8, n);
                    else if (!writeAll(fd, buf.data() + 8, n)) error = errnoMsg("writing " + arg.substr(0, comma));
                }
                buf.erase(0, 8 + size_t(n));
            }
            if (fd >= 0) {
                fchmod(fd, mode & 0777 ? mode & 0777 : 0644);
                if (close(fd) != 0 && error.empty()) error = errnoMsg("writing " + arg.substr(0, comma));
            }
            if (gone) {  // cut off half way: no half file left behind
                if (fd >= 0) unlink(path.c_str());
                break;
            }
            if (error.empty() && S_ISLNK(mode) && symlink(link.c_str(), path.c_str()) != 0) error = errnoMsg(arg.substr(0, comma));
            if (!error.empty()) {
                if (fd >= 0) unlink(path.c_str());
                if (!fail(error)) break;
                continue;
            }
            if (!S_ISLNK(mode) && mtime) {
                struct timeval tv[2] = {{time_t(mtime), 0}, {time_t(mtime), 0}};
                utimes(path.c_str(), tv);
            }
            if (!s.write("OKAY" + u32le(0))) break;
        } else {
            fail("FMRRE's adb doesn't know the request " + id);
            break;
        }
    }
    finish(conn, s);
}

// ---- the connection --------------------------------------------------------------------------------

std::string banner() {
    const auto& di = device::info();
    std::string name = device::prop("ro.product.name");
    return "recovery::ro.product.name=" + (name.empty() ? di.device : name) + ";ro.product.model=" + di.model + ";ro.product.device=" +
           di.device + ";features=shell_v2";
}

void openService(const std::shared_ptr<Conn>& conn, uint32_t remote, const std::string& svc) {
    bool v2 = false, raw = false, pty = false;
    std::string term, cmd;
    enum { None, Shell, Sync, Reboot } kind = None;
    if (svc == "sync:") kind = Sync;
    else if (util::startsWith(svc, "reboot:")) kind = Reboot;
    else if (parseShell(svc, v2, raw, pty, term, cmd)) kind = Shell;
    if (kind == None) {
        LOGI("adbd: '%s' is not offered here", svc.substr(0, 60).c_str());
        conn->send(A_CLSE, 0, remote);
        return;
    }
    auto s = std::make_shared<Stream>();
    s->conn = conn.get();
    s->remote = remote;
    {
        std::lock_guard<std::mutex> l(conn->mu);
        s->local = conn->nextId++;
        conn->streams[s->local] = s;
    }
    conn->send(A_OKAY, s->local, remote);
    conn->workers++;
    std::thread([conn, s, kind, v2, raw, pty, term, cmd, svc] {
        if (kind == Shell) {
            if (!cmd.empty()) LOGI("adbd: shell: %s", cmd.substr(0, 200).c_str());
            shellService(*conn, *s, v2, raw, pty, term, cmd);
        } else if (kind == Sync) {
            syncService(*conn, *s);
        } else {
            std::string target = svc.substr(7);
            LOGI("adbd: the computer asked to reboot%s", target.empty() ? "" : (" to " + target).c_str());
            finish(*conn, *s);
            usleep(300 * 1000);  // let the answer reach the computer
            device::reboot(target);
        }
        conn->workers--;
    }).detach();
}

// One session: from the computer's CNXN until the link drops.
void serve(const std::shared_ptr<Conn>& conn) {
    Packet p;
    while (recvPacket(*conn->link, p)) {
        if (p.cmd == A_CNXN) {  // a new adb server on the computer: start clean. No authentication, as in any recovery.
            conn->closeAll();
            conn->maxData = std::min<uint32_t>(std::max<uint32_t>(p.a1, 4096), kMaxPayload);
            conn->send(A_CNXN, std::min(p.a0, kVersion), kMaxPayload, banner());
            LOGI("adbd: a computer connected");
        } else if (p.cmd == A_OPEN) {
            openService(conn, p.a0, p.data.substr(0, p.data.find('\0')));
        } else if (p.cmd == A_WRTE) {
            auto s = conn->find(p.a1);
            if (!s || s->remote != p.a0) continue;
            {
                std::lock_guard<std::mutex> l(s->mu);
                s->in.push_back(std::move(p.data));
            }
            s->cv.notify_all();
        } else if (p.cmd == A_OKAY) {
            if (auto s = conn->find(p.a1)) {
                {
                    std::lock_guard<std::mutex> l(s->mu);
                    s->ready = true;
                }
                s->cv.notify_all();
            }
        } else if (p.cmd == A_CLSE) {
            if (auto s = conn->find(p.a1)) {
                s->markClosed();
                conn->drop(p.a1);
            }
        }
    }
    conn->closeAll();
}

// ---- the server ------------------------------------------------------------------------------------

struct Server {
    Options opt;
    std::thread th;
    std::atomic<bool> stop{false}, paused{false}, idle{true}, running{false};
    std::mutex mu;
    std::shared_ptr<Conn> conn;
    std::string status = "off";

    void setStatus(const std::string& s) {
        std::lock_guard<std::mutex> l(mu);
        if (s != status) LOGI("adbd: %s", s.c_str());
        status = s;
    }
    void interrupt() {
        std::lock_guard<std::mutex> l(mu);
        if (conn && conn->link) conn->link->interrupt();
    }
    void waitQuietly(int ms) {
        for (int i = 0; i < ms / 100 && !stop && !paused; i++) usleep(100 * 1000);
    }
    void run() {
        int lfd = -1;
        if (opt.tcpPort) {
            lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            int one = 1;
            setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(uint16_t(opt.tcpPort));
            inet_pton(AF_INET, opt.tcpBind.c_str(), &a.sin_addr);
            if (lfd < 0 || bind(lfd, (sockaddr*)&a, sizeof a) != 0 || listen(lfd, 2) != 0) {
                setStatus(std::string("can't listen on ") + opt.tcpBind + ":" + std::to_string(opt.tcpPort) + ": " + strerror(errno));
                if (lfd >= 0) close(lfd);
                idle = true;
                return;
            }
            setStatus("waiting on " + opt.tcpBind + ":" + std::to_string(opt.tcpPort));
        }
        while (!stop) {
            if (paused) {
                idle = true;
                usleep(100 * 1000);
                continue;
            }
            idle = false;
            auto c = std::make_shared<Conn>();
            if (opt.tcpPort) {
                pollfd p{lfd, POLLIN, 0};
                if (poll(&p, 1, 250) <= 0) continue;
                int fd = accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
                if (fd < 0) continue;
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                c->link = std::make_unique<TcpLink>(fd);
            } else {
                auto u = std::make_unique<UsbLink>();
                std::string err;
                if (!u->open(err)) {
                    setStatus("no USB: " + err);
                    u.reset();
                    waitQuietly(5000);
                    continue;
                }
                c->link = std::move(u);
                setStatus("ready on USB (shell, pull, push)");
            }
            {
                std::lock_guard<std::mutex> l(mu);
                conn = c;
            }
            if (opt.tcpPort) {
                serve(c);
            } else {
                // USB: the cable can be pulled and put back; the function stays ours meanwhile.
                while (!stop && !paused) {
                    serve(c);
                    if (!c->drain(6000)) break;  // a service is stuck: start over with a fresh link
                    waitQuietly(300);
                }
            }
            {
                std::lock_guard<std::mutex> l(mu);
                conn.reset();
            }
            c->closeAll();
            c->drain(6000);
        }
        if (lfd >= 0) close(lfd);
        idle = true;
    }
};

Server& server() {
    static Server* s = new Server;  // never destroyed: services on detached threads may outlive main()
    return *s;
}

}  // namespace

bool start(const Options& o) {
    Server& s = server();
    if (s.running) return false;
    s.opt = o;
    s.stop = false;
    s.paused = false;
    s.running = true;
    s.th = std::thread([&s] { s.run(); });
    return true;
}

void stop() {
    Server& s = server();
    if (!s.running) return;
    s.stop = true;
    s.interrupt();
    if (s.th.joinable()) s.th.join();
    s.running = false;
    s.setStatus("off");
}

bool running() { return server().running; }

void setLocked(bool locked) { g_locked = locked; }

void pause() {
    Server& s = server();
    if (!s.running) return;
    s.paused = true;
    for (int i = 0; i < 100 && !s.idle; i++) {
        s.interrupt();
        usleep(100 * 1000);
    }
}

void resume() { server().paused = false; }

std::string status() {
    Server& s = server();
    std::lock_guard<std::mutex> l(s.mu);
    return s.status;
}

int serveForeground(const Options& o) {
    if (!start(o)) return 1;
    for (;;) sleep(3600);
    return 0;
}

}  // namespace tw::adbserver
