#include "sys/adbd.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <endian.h>
#include <fcntl.h>
#include <linux/usb/functionfs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>

#include "core/log.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/adblink.h"
#include "sys/adbserver.h"
#include "sys/fusefile.h"
#include "sys/partitions.h"

namespace tw::adbd {

namespace {

using namespace detail;

constexpr uint64_t kMaxPackage = uint64_t(64) << 30;

// Where the package goes: the first place with room for it (and a margin).
std::string pickDir(uint64_t size, std::string& why) {
    auto& P = Partitions::get();
    struct Cand {
        std::string dir;
        uint64_t margin;
    };
    std::vector<Cand> cands;
    Partition* data = P.find("/data");
    if (data && P.isMounted(*data) && !P.dataEncrypted()) cands.push_back({P.appDir() + "/sideload", 64ull << 20});
    Partition* cache = P.find("/cache");
    if (cache && P.isMounted(*cache)) cands.push_back({cache->contentPath() + "/sideload", 16ull << 20});
    cands.push_back({device::path("/tmp/sideload"), 256ull << 20});  // RAM: leave plenty for the install
    uint64_t best = 0;
    for (auto& c : cands) {
        util::mkdirs(c.dir);
        unlink((c.dir + "/package.zip").c_str());
        struct statvfs vf;
        if (statvfs(c.dir.c_str(), &vf) != 0) continue;
        uint64_t freeB = uint64_t(vf.f_bavail) * vf.f_frsize;
        best = std::max(best, freeB);
        if (freeB >= size + c.margin) return c.dir;
    }
    why = "no room for a " + util::humanSize(size) + " package (at most " + util::humanSize(best) +
          " free) - copy it to the phone over MTP and install it from storage";
    return "";
}

}  // namespace

struct Sideload::Impl {
    std::unique_ptr<Link> link;
    uint32_t maxData = 4096;
    uint32_t localId = 0x464d52, remoteId = 0;  // "FMR"
    bool streamOpen = false, finished = false;
    std::string pathOut;
    std::thread watch;
    std::atomic<bool> stopWatch{false};
    std::unique_ptr<fusefile::Server> fuse;  // the package served from the computer, block by block, while it installs
    uint64_t pkgSize = 0, blockSize = 0;

    bool send(uint32_t cmd, uint32_t a0, uint32_t a1, const std::string& data = "") {
        uint8_t h[24];
        uint32_t sum = 0;
        for (unsigned char c : data) sum += c;
        put32(h, cmd);
        put32(h + 4, a0);
        put32(h + 8, a1);
        put32(h + 12, uint32_t(data.size()));
        put32(h + 16, sum);
        put32(h + 20, cmd ^ 0xffffffffu);
        if (!link->separatePayload()) {
            std::string all((const char*)h, 24);
            return link->write((all + data).data(), 24 + data.size());
        }
        return link->write(h, 24) && (data.empty() || link->write(data.data(), data.size()));
    }
    bool recv(Packet& p) {
        uint8_t h[24];
        if (!link->readFull(h, 24)) return false;
        p.cmd = le32(h);
        p.a0 = le32(h + 4);
        p.a1 = le32(h + 8);
        uint32_t len = le32(h + 12);
        if ((p.cmd ^ 0xffffffffu) != le32(h + 20) || len > kMaxPayload) {
            LOGW("adbd: damaged message from the computer");
            return false;
        }
        p.data.resize(len);
        return !len || link->readFull(&p.data[0], len);
    }
    // One block of a sideload-host package: asked for by number, sent by the computer in one or more WRTEs.
    bool fetch(uint64_t b, std::string& out) {
        char req[32];
        snprintf(req, sizeof req, "%08" PRIu64, b);
        if (b * blockSize >= pkgSize || !send(A_WRTE, localId, remoteId, std::string(req, 8))) return false;
        uint64_t need = std::min(blockSize, pkgSize - b * blockSize);
        out.clear();
        bool acked = false;
        while (out.size() < need || !acked) {
            Packet q;
            if (!recv(q)) return false;
            if (q.cmd == A_OKAY && q.a1 == localId) {
                acked = true;
            } else if (q.cmd == A_WRTE && q.a1 == localId) {
                if (q.data.size() > need - out.size()) return false;
                out += q.data;
                if (!send(A_OKAY, localId, remoteId)) return false;
            } else if (q.cmd == A_CLSE || q.cmd == A_CNXN) {
                return false;
            }
        }
        return true;
    }
    void stopWatcher() {
        stopWatch = true;
        if (watch.joinable()) watch.join();
    }
    void stopFuse() {
        if (!fuse) return;
        fuse->stop();
        LOGI("adb sideload: %s served from the computer for a %s package%s", util::humanSize(fuse->fetched()).c_str(),
             util::humanSize(pkgSize).c_str(), fuse->problem().empty() ? "" : (" (" + fuse->problem() + ")").c_str());
        fuse.reset();
    }
    void close() {
        stopFuse();
        stopWatcher();
        bool usb = link && link->separatePayload();
        link.reset();  // USB: gives the gadget back to adbd
        if (usb) adbserver::resume();  // FMRRE's own adb takes it again
    }
};

Sideload::Sideload() : d_(new Impl) { reboot = "-"; }

Sideload::~Sideload() {
    if (d_->link && !d_->finished) finish(false);
    d_->close();
}

std::string Sideload::receive(const Options& o, Task& t) {
    Impl& d = *d_;
    // ---- wait for the computer
    int lfd = -1;
    if (o.tcpPort) {
        lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(o.tcpPort));
        inet_pton(AF_INET, o.tcpBind.c_str(), &a.sin_addr);
        if (lfd < 0 || bind(lfd, (sockaddr*)&a, sizeof a) != 0 || listen(lfd, 2) != 0) {
            t.print("adb: can't listen on %s:%d: %s", o.tcpBind.c_str(), o.tcpPort, strerror(errno));
            if (lfd >= 0) ::close(lfd);
            return "";
        }
        t.print("Waiting for adb on %s:%d (adb connect %s:%d, then adb sideload <file>)", o.tcpBind.c_str(), o.tcpPort,
                o.tcpBind.c_str(), o.tcpPort);
    } else {
        adbserver::pause();  // FMRRE's own adb (shell, pull, push) lends the USB link for the sideload
        auto u = std::make_unique<UsbLink>();
        std::string err;
        if (!u->open(err)) {
            t.print("adb sideload over USB: %s", err.c_str());
            adbserver::resume();
            return "";
        }
        d.link = std::move(u);
    }
    // Cancel: unblock whatever the session is waiting for.
    d.stopWatch = false;
    d.watch = std::thread([&d, &t] {
        while (!d.stopWatch) {
            if (t.cancelled() && d.link) {
                d.link->interrupt();
                return;
            }
            usleep(200 * 1000);
        }
    });
    struct ListenCloser {
        int fd;
        ~ListenCloser() {
            if (fd >= 0) ::close(fd);
        }
    } lc{lfd};
    int64_t deadline = util::nowMs() + 10 * 60 * 1000;
    t.setStatus("Waiting for the computer");
    for (;;) {
        if (t.cancelled() || util::nowMs() > deadline) {
            if (!t.cancelled()) t.print("No computer started a sideload in 10 minutes");
            d.close();
            return "";
        }
        if (!d.link) {  // TCP: the next connection
            pollfd p{lfd, POLLIN, 0};
            if (poll(&p, 1, 250) <= 0) continue;
            int c = accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) continue;
            int one = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            d.link = std::make_unique<TcpLink>(c);
        }
        Packet p;
        if (!d.recv(p)) {
            if (o.tcpPort) {  // the adb server went away; it reconnects
                d.stopWatcher();
                d.link.reset();
                d.stopWatch = false;
                d.watch = std::thread([&d, &t] {
                    while (!d.stopWatch) {
                        if (t.cancelled() && d.link) return d.link->interrupt();
                        usleep(200 * 1000);
                    }
                });
                continue;
            }
            if (!t.cancelled()) t.print("adb: the USB connection ended");
            d.close();
            return "";
        }
        if (p.cmd == A_CNXN) {  // the handshake: no authentication in recovery, like minadbd
            d.maxData = std::min<uint32_t>(std::max<uint32_t>(p.a1, 4096), kMaxPayload);
            const auto& di = device::info();
            std::string name = device::prop("ro.product.name");
            std::string banner = "sideload::ro.product.name=" + (name.empty() ? di.device : name) + ";ro.product.model=" + di.model +
                                 ";ro.product.device=" + di.device + ";features=";
            d.send(A_CNXN, std::min(p.a0, kVersion), kMaxPayload, banner);
            t.print("Computer connected - waiting for adb sideload");
            continue;
        }
        if (p.cmd != A_OPEN) continue;
        std::string svc = p.data.substr(0, p.data.find('\0'));
        d.remoteId = p.a0;
        uint64_t size = 0, block = 0;
        bool host = util::startsWith(svc, "sideload-host:"), old = !host && util::startsWith(svc, "sideload:");
        if (util::startsWith(svc, "reboot:")) {  // adb reboot [recovery|bootloader|...]
            d.send(A_OKAY, d.localId, d.remoteId);
            d.send(A_CLSE, d.localId, d.remoteId);
            reboot = svc.substr(7);
            t.print("The computer asked to reboot%s", reboot.empty() ? "" : (" to " + reboot).c_str());
            d.finished = true;
            d.close();
            return "";
        }
        if (host || old) {
            auto f = util::split(svc.substr(svc.find(':') + 1), ':');
            size = f.empty() ? 0 : strtoull(f[0].c_str(), nullptr, 10);
            block = host && f.size() > 1 ? strtoull(f[1].c_str(), nullptr, 10) : 0;
        }
        if ((!host && !old) || size == 0 || size > kMaxPackage || (host && (block == 0 || block > kMaxPayload))) {
            d.send(A_CLSE, 0, d.remoteId);  // not a service of ours
            continue;
        }
        d.pkgSize = size;
        d.blockSize = block;
        if (host && !(device::isSim() && getenv("FMRRE_SIDELOAD_STORE"))) {
            // Served from the computer while it installs, through FUSE as in AOSP's recovery: nothing is stored, so
            // the package needs no free space; blocks read again are checked against their first SHA-256.
            auto fs = std::make_unique<fusefile::Server>();
            std::string ferr;
            if (fs->start(device::path("/sideload"), "package.zip", size, uint32_t(block),
                          [&d](uint64_t b, std::string& out) { return d.fetch(b, out); }, ferr)) {
                d.send(A_OKAY, d.localId, d.remoteId);
                d.streamOpen = true;
                d.fuse = std::move(fs);
                d.pathOut = d.fuse->path();
                t.print("Reading the %s package from the computer as it installs (nothing is stored)", util::humanSize(size).c_str());
                return d.pathOut;
            }
            LOGI("adb sideload: %s - the package is stored first", ferr.c_str());
        }
        std::string why, dir = pickDir(size, why);
        if (dir.empty()) {
            t.print("adb sideload: %s", why.c_str());
            d.send(A_CLSE, 0, d.remoteId);
            continue;
        }
        std::string path = dir + "/package.zip";
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) {
            t.print("adb sideload: can't write %s: %s", path.c_str(), strerror(errno));
            d.send(A_CLSE, 0, d.remoteId);
            continue;
        }
        d.send(A_OKAY, d.localId, d.remoteId);
        d.streamOpen = true;
        t.print("Receiving %s into %s", util::humanSize(size).c_str(), dir.c_str());
        t.setStatus("Receiving the package");
        uint64_t got = 0;
        bool ok = true;
        char det[64];
        auto progress = [&]() {
            t.setSubProgress(float(double(got) / double(size)));
            snprintf(det, sizeof det, "%s / %s", util::humanSize(got).c_str(), util::humanSize(size).c_str());
            t.setDetail(det);
        };
        auto put = [&](const std::string& data, uint64_t at) {
            for (size_t done = 0; done < data.size();) {
                ssize_t w = pwrite(fd, data.data() + done, data.size() - done, off_t(at + done));
                if (w <= 0) return false;
                done += size_t(w);
            }
            return true;
        };
        if (host) {  // ask for each block in turn
            std::string data;
            for (uint64_t b = 0; b * block < size && ok; b++) {
                ok = d.fetch(b, data) && put(data, b * block);
                if (ok) got += data.size();
                progress();
            }
        } else {  // the old protocol: the whole file, pushed
            while (ok && got < size) {
                Packet q;
                if (!d.recv(q)) { ok = false; break; }
                if (q.cmd == A_WRTE && q.a1 == d.localId) {
                    ok = q.data.size() <= size - got && put(q.data, got) && d.send(A_OKAY, d.localId, d.remoteId);
                    got += q.data.size();
                    progress();
                } else if (q.cmd == A_CLSE || q.cmd == A_CNXN) {
                    ok = false;
                }
            }
        }
        if (fsync(fd) != 0) ok = false;
        ::close(fd);
        if (!ok || got != size) {
            unlink(path.c_str());
            if (!t.cancelled()) t.print("adb sideload: the transfer stopped after %s of %s", util::humanSize(got).c_str(), util::humanSize(size).c_str());
            d.close();
            return "";
        }
        t.print("Received %s", util::humanSize(size).c_str());
        if (old) {  // nothing to report later with the old protocol: the transfer was all
            d.send(A_CLSE, d.localId, d.remoteId);
            d.finished = true;
            d.close();
        }
        d.pathOut = path;
        return path;
    }
}

bool Sideload::streamed() const { return d_->fuse != nullptr; }

void Sideload::finish(bool installed) {
    Impl& d = *d_;
    if (d.finished || !d.link || !d.streamOpen) {
        d.close();
        return;
    }
    d.finished = true;
    d.stopFuse();  // the installer is done with the package
    // adb sideload exits 0 on DONEDONE, 1 on FAILFAIL (adb_sideload_install). A computer that went away meanwhile
    // can't make this wait for ever: the link is cut after 5 seconds.
    d.stopWatcher();
    d.stopWatch = false;
    d.watch = std::thread([&d] {
        for (int i = 0; i < 50 && !d.stopWatch; i++) usleep(100 * 1000);
        if (!d.stopWatch && d.link) d.link->interrupt();
    });
    if (d.send(A_WRTE, d.localId, d.remoteId, installed ? "DONEDONE" : "FAILFAIL")) {
        for (int i = 0; i < 4; i++) {  // its acknowledgement, then close
            Packet q;
            if (!d.recv(q) || (q.cmd == A_OKAY && q.a1 == d.localId) || q.cmd == A_CLSE) break;
        }
        d.send(A_CLSE, d.localId, d.remoteId);
    }
    d.close();
}

}  // namespace tw::adbd
