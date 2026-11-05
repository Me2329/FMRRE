#include "sys/fusefile.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "core/log.h"
#include "core/sha256.h"
#include "core/util.h"

namespace tw::fusefile {

namespace {

// The kernel's FUSE protocol (include/uapi/linux/fuse.h), version 7.22: what every kernel since 3.x speaks, and
// newer ones still accept. Only what one read-only file in one folder needs.
enum : uint32_t {
    kLookup = 1, kForget = 2, kGetattr = 3, kOpen = 14, kRead = 15, kStatfs = 17, kRelease = 18, kFsync = 20,
    kGetxattr = 22, kListxattr = 23, kFlush = 25, kInit = 26, kOpendir = 27, kReaddir = 28, kReleasedir = 29,
    kFsyncdir = 30, kAccess = 34, kInterrupt = 36, kDestroy = 38, kBatchForget = 42,
};
constexpr uint64_t kRootId = 1, kFileId = 2;
constexpr uint32_t kOurMinor = 22, kKeepCache = 2 /* FOPEN_KEEP_CACHE */;

struct InHeader {
    uint32_t len, opcode;
    uint64_t unique, nodeid;
    uint32_t uid, gid, pid, padding;
};
struct OutHeader {
    uint32_t len;
    int32_t error;
    uint64_t unique;
};
struct Attr {
    uint64_t ino, size, blocks, atime, mtime, ctime;
    uint32_t atimensec, mtimensec, ctimensec, mode, nlink, uid, gid, rdev, blksize, padding;
};
struct EntryOut {
    uint64_t nodeid, generation, entryValid, attrValid;
    uint32_t entryValidNsec, attrValidNsec;
    Attr attr;
};
struct AttrOut {
    uint64_t attrValid;
    uint32_t attrValidNsec, dummy;
    Attr attr;
};
struct InitIn {
    uint32_t major, minor, maxReadahead, flags;
};
struct InitOut {  // the 7.22 layout (24 bytes); newer kernels take a shorter reply than their own struct
    uint32_t major, minor, maxReadahead, flags;
    uint16_t maxBackground, congestionThreshold;
    uint32_t maxWrite;
};
struct OpenIn {
    uint32_t flags, unused;
};
struct OpenOut {
    uint64_t fh;
    uint32_t openFlags, padding;
};
struct ReadIn {
    uint64_t fh, offset;
    uint32_t size, readFlags;
    uint64_t lockOwner;
    uint32_t flags, padding;
};
struct Kstatfs {
    uint64_t blocks, bfree, bavail, files, ffree;
    uint32_t bsize, namelen, frsize, padding, spare[6];
};
struct Dirent {
    uint64_t ino, off;
    uint32_t namelen, type;
};
static_assert(sizeof(InHeader) == 40 && sizeof(OutHeader) == 16 && sizeof(Attr) == 88 && sizeof(EntryOut) == 128 &&
                  sizeof(AttrOut) == 104 && sizeof(InitOut) == 24 && sizeof(ReadIn) == 40 && sizeof(Kstatfs) == 80,
              "FUSE structures");

}  // namespace

struct Server::Impl {
    std::string dir, name;
    uint64_t size = 0;
    uint32_t bs = 0;
    FetchFn fetch;
    int fd = -1;
    bool mounted = false;
    std::thread th;
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> fetched{0};
    std::mutex pm;
    std::string problem;
    time_t now = time(nullptr);
    // each block's SHA-256 from its first fetch
    std::vector<std::string> hashes;
    // the last few blocks: the kernel reads in pages and runs of pages, often from the same block
    struct Cached {
        uint64_t block = UINT64_MAX;
        std::string data;
    };
    Cached cache[4];
    unsigned next = 0;

    void note(const std::string& p) {
        std::lock_guard<std::mutex> lk(pm);
        if (problem.empty()) problem = p;
        LOGW("sideload: %s", p.c_str());
    }

    const std::string* block(uint64_t b) {
        for (auto& c : cache)
            if (c.block == b) return &c.data;
        Cached& c = cache[next++ % 4];
        c.block = UINT64_MAX;
        if (!fetch(b, c.data)) {
            note("block " + std::to_string(b) + " didn't come from the computer");
            return nullptr;
        }
        uint64_t want = std::min<uint64_t>(bs, size - b * bs);
        if (c.data.size() != want) {
            note("block " + std::to_string(b) + " has the wrong size");
            return nullptr;
        }
        fetched += want;
        Sha256 s;
        s.update(c.data.data(), c.data.size());
        std::string h = s.digest();
        if (hashes[b].empty()) {
            hashes[b] = h;
        } else if (hashes[b] != h) {
            note("the computer sent different data for block " + std::to_string(b) + " the second time - refused");
            return nullptr;
        }
        c.block = b;
        return &c.data;
    }

    Attr attr(uint64_t id) const {
        Attr a{};
        a.ino = id;
        a.atime = a.mtime = a.ctime = uint64_t(now);
        a.uid = getuid();
        a.gid = getgid();
        a.blksize = bs;
        if (id == kRootId) {
            a.mode = S_IFDIR | 0555;
            a.nlink = 2;
        } else {
            a.mode = S_IFREG | 0444;
            a.nlink = 1;
            a.size = size;
            a.blocks = (size + 511) / 512;
        }
        return a;
    }

    void reply(uint64_t unique, int err, const void* data = nullptr, size_t n = 0) {
        std::string out(sizeof(OutHeader) + n, '\0');
        OutHeader h{uint32_t(out.size()), err ? -err : 0, unique};
        memcpy(&out[0], &h, sizeof h);
        if (n) memcpy(&out[sizeof h], data, n);
        if (::write(fd, out.data(), out.size()) < 0 && errno != ENOENT) LOGW("fuse: reply: %s", strerror(errno));
    }

    void read(const InHeader& h, const ReadIn& r) {
        if (h.nodeid != kFileId) return reply(h.unique, EISDIR);
        if (r.offset >= size) return reply(h.unique, 0);
        uint64_t n = std::min<uint64_t>(r.size, size - r.offset);
        std::string out(size_t(n), '\0');
        for (uint64_t done = 0; done < n;) {
            uint64_t at = r.offset + done, b = at / bs, in = at % bs;
            const std::string* d = block(b);
            if (!d) return reply(h.unique, EIO);
            uint64_t k = std::min<uint64_t>(n - done, d->size() - in);
            memcpy(&out[size_t(done)], d->data() + in, size_t(k));
            done += k;
        }
        reply(h.unique, 0, out.data(), out.size());
    }

    void readdir(const InHeader& h, const ReadIn& r) {
        if (h.nodeid != kRootId) return reply(h.unique, ENOTDIR);
        struct E {
            const char* n;
            uint64_t ino;
            uint32_t type;
        } es[] = {{".", kRootId, DT_DIR}, {"..", kRootId, DT_DIR}, {name.c_str(), kFileId, DT_REG}};
        std::string out;
        for (uint64_t i = r.offset; i < 3; i++) {
            size_t nl = strlen(es[i].n), rec = (sizeof(Dirent) + nl + 7) & ~size_t(7);
            if (out.size() + rec > r.size) break;
            Dirent d{es[i].ino, i + 1, uint32_t(nl), es[i].type};
            std::string e(rec, '\0');
            memcpy(&e[0], &d, sizeof d);
            memcpy(&e[sizeof d], es[i].n, nl);
            out += e;
        }
        reply(h.unique, 0, out.data(), out.size());
    }

    void handle(const uint8_t* m, size_t n) {
        if (n < sizeof(InHeader)) return;
        InHeader h;
        memcpy(&h, m, sizeof h);
        const uint8_t* arg = m + sizeof h;
        size_t an = n - sizeof h;
        switch (h.opcode) {
            case kInit: {
                InitIn in{};
                memcpy(&in, arg, std::min(an, sizeof in));
                InitOut o{7, kOurMinor, std::min<uint32_t>(in.maxReadahead, 1u << 20), 0, 4, 3, 4096};
                if (in.major < 7) return reply(h.unique, EPROTO);
                return reply(h.unique, 0, &o, sizeof o);
            }
            case kLookup: {
                std::string nm((const char*)arg, strnlen((const char*)arg, an));
                if (h.nodeid != kRootId || nm != name) return reply(h.unique, ENOENT);
                EntryOut e{};
                e.nodeid = kFileId;
                e.entryValid = e.attrValid = 3600;
                e.attr = attr(kFileId);
                return reply(h.unique, 0, &e, sizeof e);
            }
            case kGetattr: {
                if (h.nodeid != kRootId && h.nodeid != kFileId) return reply(h.unique, ENOENT);
                AttrOut a{};
                a.attrValid = 3600;
                a.attr = attr(h.nodeid);
                return reply(h.unique, 0, &a, sizeof a);
            }
            case kOpen: {
                OpenIn in{};
                memcpy(&in, arg, std::min(an, sizeof in));
                if (h.nodeid != kFileId) return reply(h.unique, EISDIR);
                if ((in.flags & O_ACCMODE) != O_RDONLY) return reply(h.unique, EROFS);
                OpenOut o{0, kKeepCache, 0};  // keep the pages cached between opens: the installer opens it several times
                return reply(h.unique, 0, &o, sizeof o);
            }
            case kOpendir: {
                if (h.nodeid != kRootId) return reply(h.unique, ENOTDIR);
                OpenOut o{};
                return reply(h.unique, 0, &o, sizeof o);
            }
            case kRead:
            case kReaddir: {
                ReadIn r{};
                memcpy(&r, arg, std::min(an, sizeof r));
                return h.opcode == kRead ? read(h, r) : readdir(h, r);
            }
            case kStatfs: {
                Kstatfs s{};
                s.bsize = s.frsize = bs;
                s.blocks = (size + bs - 1) / bs;
                s.files = 1;
                s.namelen = 255;
                return reply(h.unique, 0, &s, sizeof s);
            }
            case kRelease:
            case kReleasedir:
            case kFlush:
            case kFsync:
            case kFsyncdir:
            case kAccess: return reply(h.unique, 0);
            case kForget:
            case kBatchForget:
            case kInterrupt: return;  // no reply
            case kDestroy:
                stopping = true;
                return reply(h.unique, 0);
            default: return reply(h.unique, ENOSYS);  // xattrs, writes, links...: "not supported", not asked again
        }
    }

    void loop() {
        std::vector<uint8_t> buf(64 * 1024 + 4096);  // more than any request (writes are refused, max_write 4 KiB)
        while (!stopping) {
            pollfd p{fd, POLLIN, 0};
            int r = poll(&p, 1, 200);
            if (r == 0 || (r < 0 && errno == EINTR)) continue;
            if (r < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
            ssize_t n = ::read(fd, buf.data(), buf.size());
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == ENOENT) continue;  // ENOENT: an interrupted request
                break;                                                             // ENODEV: unmounted
            }
            handle(buf.data(), size_t(n));
        }
    }
};

Server::Server() : d_(new Impl) {}
Server::~Server() { stop(); }

std::string Server::path() const { return d_->dir + "/" + d_->name; }
uint64_t Server::fetched() const { return d_->fetched; }
std::string Server::problem() const {
    std::lock_guard<std::mutex> lk(d_->pm);
    return d_->problem;
}

bool Server::start(const std::string& dir, const std::string& name, uint64_t size, uint32_t blockSize, FetchFn fetch,
                   std::string& err) {
    Impl& d = *d_;
    if (d.mounted || size == 0 || blockSize == 0) return err = "bad arguments", false;
    d.dir = dir;
    d.name = name;
    d.size = size;
    d.bs = blockSize;
    d.fetch = std::move(fetch);
    d.hashes.assign(size_t((size + blockSize - 1) / blockSize), std::string());
    umount2(dir.c_str(), MNT_DETACH);  // left over from a session that didn't end
    util::mkdirs(dir);
    d.fd = ::open("/dev/fuse", O_RDWR | O_CLOEXEC);
    if (d.fd < 0) return err = std::string("no FUSE (/dev/fuse: ") + strerror(errno) + ")", false;
    char opts[160];
    snprintf(opts, sizeof opts, "fd=%d,rootmode=40000,user_id=%u,group_id=%u,allow_other,max_read=%u", d.fd, unsigned(getuid()),
             unsigned(getgid()), blockSize);
    if (mount("fmrre-sideload", dir.c_str(), "fuse", MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RDONLY, opts) != 0) {
        err = std::string("can't mount FUSE on ") + dir + ": " + strerror(errno);
        ::close(d.fd);
        d.fd = -1;
        return false;
    }
    d.mounted = true;
    d.stopping = false;
    d.th = std::thread([&d] { d.loop(); });
    return true;
}

void Server::stop() {
    Impl& d = *d_;
    if (!d.mounted) return;
    d.stopping = true;
    umount2(d.dir.c_str(), MNT_DETACH);
    if (d.th.joinable()) d.th.join();
    ::close(d.fd);  // aborts the connection: anything still open there fails from now on
    d.fd = -1;
    d.mounted = false;
    rmdir(d.dir.c_str());
}

}  // namespace tw::fusefile
