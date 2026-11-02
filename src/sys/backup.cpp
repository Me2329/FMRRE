#include "sys/backup.h"
#include "core/log.h"
#include "core/sha256.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"
#include "miniz.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tw::backup {

std::string backupRoot() {
    return Partitions::get().storagePath() + "/TWRP/BACKUPS/" + device::info().serial;
}

static bool isDataLike(const Partition& p) { return p.mountPoint == "/data"; }

uint64_t estimateSize(const std::string& mp) {
    Partition* p = Partitions::get().find(mp);
    if (!p) return 0;
    if (p->isImage()) return p->size ? p->size : Partitions::blockSize(p->blockDev);
    if (!Partitions::get().mount(*p)) return 0;
    if (isDataLike(*p)) {
        uint64_t total = 0;
        for (auto& e : util::listDir(p->contentPath()))
            if (e.name != "media" && e.name != "lost+found") total += util::dirSize(util::joinPath(p->contentPath(), e.name));
        return total;
    }
    return p->used ? p->used : util::dirSize(p->contentPath());
}

// ---- streaming writer: optional gzip + sha256 + progress -------------------------
class Sink {
public:
    bool open(const std::string& path, bool gzip) {
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd_ < 0) return false;
        gzip_ = gzip;
        if (gzip_) {
            memset(&zs_, 0, sizeof(zs_));
            if (mz_deflateInit2(&zs_, 1, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY) != MZ_OK) return false;
            const unsigned char hdr[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
            return put(hdr, 10);
        }
        return true;
    }
    bool write(const void* data, size_t n) {
        in_ += n;
        if (!gzip_) return put(data, n);
        crc_ = (uint32_t)mz_crc32(crc_, (const unsigned char*)data, n);
        zs_.next_in = (const unsigned char*)data;
        zs_.avail_in = (unsigned)n;
        return pump(MZ_NO_FLUSH);
    }
    bool close() {
        bool ok = true;
        if (gzip_) {
            ok = pump(MZ_FINISH);
            mz_deflateEnd(&zs_);
            unsigned char tail[8];
            for (int i = 0; i < 4; i++) tail[i] = uint8_t(crc_ >> (8 * i));
            for (int i = 0; i < 4; i++) tail[4 + i] = uint8_t(uint32_t(in_) >> (8 * i));
            ok = ok && put(tail, 8);
        }
        if (fd_ >= 0) { fsync(fd_); ::close(fd_); fd_ = -1; }
        return ok;
    }
    std::string digest() { return sha_.hexDigest(); }
    uint64_t written() const { return out_; }

private:
    bool put(const void* d, size_t n) {
        sha_.update(d, n);
        out_ += n;
        const char* p = (const char*)d;
        while (n) {
            ssize_t r = ::write(fd_, p, n);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            p += r;
            n -= r;
        }
        return true;
    }
    bool pump(int flush) {
        unsigned char buf[1 << 16];
        for (;;) {
            zs_.next_out = buf;
            zs_.avail_out = sizeof(buf);
            int r = mz_deflate(&zs_, flush);
            size_t have = sizeof(buf) - zs_.avail_out;
            if (have && !put(buf, have)) return false;
            if (r == MZ_STREAM_END) return true;
            if (r != MZ_OK && r != MZ_BUF_ERROR) return false;
            if (flush == MZ_NO_FLUSH && zs_.avail_in == 0) return true;
            if (flush == MZ_FINISH && have == 0 && r == MZ_BUF_ERROR) return true;
        }
    }
    int fd_ = -1;
    bool gzip_ = false;
    mz_stream zs_{};
    uint32_t crc_ = 0;
    uint64_t in_ = 0, out_ = 0;
    Sha256 sha_;
};

// Spawn a shell command with its stdout (read) or stdin (write) connected to us.
static pid_t spawn(const std::string& cmd, int& fd, bool readFromChild) {
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        if (readFromChild) {
            dup2(p[1], 1);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) dup2(devnull, 2);
        } else {
            dup2(p[0], 0);
        }
        std::string sh = util::shellPath();
        execl(sh.c_str(), "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    if (readFromChild) { close(p[1]); fd = p[0]; }
    else { close(p[0]); fd = p[1]; }
    return pid;
}

// Preserve SELinux labels when the available tar can: toybox tar has --selinux,
// GNU tar has --xattrs. Without labels a restored /data may not boot until
// contexts are fixed (Advanced > Fix storage contexts / restorecon).
static std::string tarLabelFlags() {
    static std::string flags = [] {
        std::string help;
        util::runCapture("tar --help 2>&1", help);
        if (help.find("--selinux") != std::string::npos) return std::string(" --selinux");
        if (help.find("--xattrs") != std::string::npos) return std::string(" --xattrs --xattrs-include='*'");
        return std::string();
    }();
    return flags;
}

static int waitChild(pid_t pid) {
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void speed(Task& t, uint64_t done, int64_t t0) {
    int64_t el = util::nowMs() - t0;
    if (el > 300) t.setDetail(util::humanSize(done * 1000 / el) + "/s");
}

static bool backupOne(Partition& p, const std::string& dir, const Options& o, Task& t, Item& item, float pBase, float pLen) {
    item.part = p.name;
    t.setRange(pBase, pLen);
    t.setSubProgress(0);
    int64_t t0 = util::nowMs();
    std::vector<char> buf(1 << 20);
    Sink sink;
    if (p.isImage()) {
        item.type = "img";
        item.compressed = o.compress;
        item.file = p.name + ".emmc.win" + std::string(o.compress ? ".gz" : "");
        t.setStatus("Backing up " + p.displayName);
        t.print("Backing up %s (image)...", p.displayName.c_str());
        int in = open(p.blockDev.c_str(), O_RDONLY | O_CLOEXEC);
        if (in < 0) { t.print("  cannot open %s: %s", p.blockDev.c_str(), strerror(errno)); return false; }
        uint64_t total = Partitions::blockSize(p.blockDev);
        if (!sink.open(dir + "/" + item.file, o.compress)) { close(in); return false; }
        uint64_t done = 0;
        bool ok = true;
        while (!t.cancelled()) {
            ssize_t n = read(in, buf.data(), buf.size());
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            if (!sink.write(buf.data(), n)) { ok = false; break; }
            done += n;
            if (total) t.setSubProgress(float(done) / total);
            speed(t, done, t0);
        }
        close(in);
        ok = sink.close() && ok && !t.cancelled();
        item.size = sink.written();
        if (o.digest) item.sha256 = sink.digest();
        t.print("  %s -> %s", util::humanSize(done).c_str(), util::humanSize(item.size).c_str());
        return ok;
    }
    // Filesystem partition: stream tar through our compressor.
    if (!Partitions::get().mount(p)) { t.print("  cannot mount %s", p.displayName.c_str()); return false; }
    item.type = "tar";
    item.compressed = o.compress;
    item.file = p.name + "." + p.fsType + ".win" + std::string(o.compress ? ".gz" : "");
    t.setStatus("Backing up " + p.displayName);
    t.print("Backing up %s (%s)...", p.displayName.c_str(), p.fsType.c_str());
    uint64_t est = estimateSize(p.mountPoint);
    std::string excl = isDataLike(p) ? " --exclude=./media --exclude=./lost+found" : " --exclude=./lost+found";
    std::string labels = tarLabelFlags();
    if (labels.empty()) t.print("  note: this tar can't store SELinux labels");
    std::string cmd = "cd " + util::shellQuote(p.contentPath()) + " && tar -cf -" + labels + excl + " .";
    int fd = -1;
    pid_t pid = spawn(cmd, fd, true);
    if (pid < 0) return false;
    if (!sink.open(dir + "/" + item.file, o.compress)) { close(fd); waitChild(pid); return false; }
    uint64_t done = 0;
    bool ok = true;
    while (!t.cancelled()) {
        ssize_t n = read(fd, buf.data(), buf.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (!sink.write(buf.data(), n)) { ok = false; break; }
        done += n;
        if (est) t.setSubProgress(std::min(0.99f, float(done) / est));
        speed(t, done, t0);
    }
    close(fd);
    if (t.cancelled()) kill(pid, SIGTERM);
    int rc = waitChild(pid);
    ok = sink.close() && ok && !t.cancelled();
    if (rc != 0) t.print("  tar exited with %d (some files may be missing)", rc);
    item.size = sink.written();
    if (o.digest) item.sha256 = sink.digest();
    t.print("  %s archived -> %s", util::humanSize(done).c_str(), util::humanSize(item.size).c_str());
    return ok;
}

static void writeManifest(const std::string& dir, const std::vector<Item>& items) {
    std::string m = "# TWRP v2 backup manifest\n";
    m += "device=" + device::info().device + "\nmodel=" + device::info().model + "\ncreated=" + util::dateStamp() + "\n";
    for (auto& i : items) {
        m += "item=" + i.part + "|" + i.file + "|" + i.type + "|" + (i.compressed ? "1" : "0") + "|" +
             std::to_string(i.size) + "|" + i.sha256 + "\n";
        if (!i.sha256.empty()) util::writeFile(dir + "/" + i.file + ".sha2", i.sha256 + "  " + i.file + "\n");
    }
    util::writeFile(dir + "/twrpv2.manifest", m);
}

bool create(const Options& o, Task& t) {
    std::string dir = backupRoot() + "/" + o.name;
    if (!util::mkdirs(dir)) {
        t.print("Cannot create backup folder %s", dir.c_str());
        return false;
    }
    t.print("Backup folder: %s", dir.c_str());
    t.print("Compression: %s, SHA-256: %s", o.compress ? "gzip" : "off", o.digest ? "on" : "off");
    // Weight progress per partition by estimated size.
    std::vector<uint64_t> est;
    uint64_t total = 0;
    for (auto& mp : o.parts) {
        uint64_t e = std::max<uint64_t>(estimateSize(mp), 1 << 20);
        est.push_back(e);
        total += e;
    }
    std::vector<Item> items;
    float base = 0;
    bool ok = true;
    for (size_t i = 0; i < o.parts.size() && ok; i++) {
        Partition* p = Partitions::get().find(o.parts[i]);
        if (!p) continue;
        float len = float(est[i]) / total;
        Item it;
        if (!backupOne(*p, dir, o, t, it, base, len)) {
            t.print("Backup of %s FAILED", p->displayName.c_str());
            ok = false;
        }
        items.push_back(it);
        base += len;
    }
    writeManifest(dir, items);
    uint64_t sz = 0;
    for (auto& i : items) sz += i.size;
    if (ok) t.print("Backup complete: %s in %s", util::humanSize(sz).c_str(),
                    util::humanDuration(util::nowMs() - t.startedMs()).c_str());
    return ok;
}

std::vector<Set> list() {
    std::vector<Set> out;
    std::string root = backupRoot();
    for (auto& e : util::listDir(root)) {
        if (!e.dir) continue;
        Set s;
        s.name = e.name;
        s.path = root + "/" + e.name;
        s.mtime = e.mtime;
        std::string m;
        if (util::readFile(s.path + "/twrpv2.manifest", m)) {
            s.hasManifest = true;
            for (auto& line : util::split(m, '\n')) {
                if (!util::startsWith(line, "item=")) continue;
                auto f = util::split(line.substr(5), '|', false);
                if (f.size() < 6) continue;
                Item i{f[0], f[1], f[2], f[3] == "1", (uint64_t)std::stoull(f[4]), f[5]};
                s.items.push_back(i);
            }
        } else {
            // Classic TWRP backup: infer from file names (boot.emmc.win, data.ext4.win, ...).
            for (auto& f : util::listDir(s.path)) {
                if (f.dir || !util::endsWith(f.name, ".win") || util::endsWith(f.name, ".sha2")) continue;
                auto parts = util::split(f.name, '.');
                if (parts.size() < 3) continue;
                Item i;
                i.part = parts[0];
                i.file = f.name;
                i.type = parts[1] == "emmc" ? "img" : "tar";
                i.size = f.size;
                std::string sha = util::readLine(s.path + "/" + f.name + ".sha2");
                if (!sha.empty()) i.sha256 = util::splitWs(sha)[0];
                s.items.push_back(i);
            }
        }
        for (auto& i : s.items) s.size += i.size;
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const Set& a, const Set& b) { return a.mtime > b.mtime; });
    return out;
}

static bool isGzip(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    unsigned char m[2] = {0, 0};
    bool r = read(fd, m, 2) == 2 && m[0] == 0x1f && m[1] == 0x8b;
    close(fd);
    return r;
}

// Read a backup file, decompress if needed, and feed the raw bytes to `out`.
static bool streamOut(const std::string& file, int out, Task& t, uint64_t& written) {
    int in = open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    uint64_t total = util::fileSize(file), consumed = 0;
    bool gz = isGzip(file);
    std::vector<unsigned char> ib(1 << 18), ob(1 << 18);
    mz_stream zs{};
    bool ok = true;
    int64_t t0 = util::nowMs();
    auto writeAll = [&](const unsigned char* d, size_t n) {
        while (n) {
            ssize_t r = write(out, d, n);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            d += r;
            n -= r;
            written += r;
        }
        return true;
    };
    if (gz) {
        // Parse gzip header (we accept FNAME/FEXTRA/FCOMMENT for classic TWRP/pigz backups).
        unsigned char h[10];
        if (read(in, h, 10) != 10) { close(in); return false; }
        consumed = 10;
        int flg = h[3];
        auto skipZ = [&]() { unsigned char c; while (read(in, &c, 1) == 1 && c) consumed++; consumed++; };
        if (flg & 4) { unsigned char x[2] = {0, 0}; if (read(in, x, 2) != 2) {} int xl = x[0] | (x[1] << 8); lseek(in, xl, SEEK_CUR); consumed += 2 + xl; }
        if (flg & 8) skipZ();
        if (flg & 16) skipZ();
        if (flg & 2) { lseek(in, 2, SEEK_CUR); consumed += 2; }
        mz_inflateInit2(&zs, -MZ_DEFAULT_WINDOW_BITS);
    }
    bool streamEnd = false;
    while (ok && !streamEnd && !t.cancelled()) {
        ssize_t n = read(in, ib.data(), ib.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        consumed += n;
        if (!gz) {
            ok = writeAll(ib.data(), n);
        } else {
            zs.next_in = ib.data();
            zs.avail_in = (unsigned)n;
            while (zs.avail_in && ok) {
                zs.next_out = ob.data();
                zs.avail_out = (unsigned)ob.size();
                int r = mz_inflate(&zs, MZ_NO_FLUSH);
                size_t have = ob.size() - zs.avail_out;
                if (have) ok = writeAll(ob.data(), have);
                if (r == MZ_STREAM_END) {
                    // pigz/TWRP may concatenate gzip members; we only support one member.
                    streamEnd = true;
                    break;
                }
                if (r != MZ_OK && r != MZ_BUF_ERROR) { t.print("  decompression error %d", r); ok = false; }
                if (r == MZ_BUF_ERROR && zs.avail_in == 0) break;
            }
        }
        t.setSubProgress(float(consumed) / std::max<uint64_t>(total, 1));
        speed(t, written, t0);
    }
    if (gz) mz_inflateEnd(&zs);
    close(in);
    return ok && !t.cancelled();
}

static bool verifyItem(const Set& s, const Item& i, Task& t) {
    if (i.sha256.empty()) {
        t.print("  %s: no digest stored, skipped", i.file.c_str());
        return true;
    }
    t.setStatus("Verifying " + i.file);
    struct Ctx { Task* t; } ctx{&t};
    std::string got = Sha256::hexOfFile(
        s.path + "/" + i.file, [](uint64_t d, uint64_t tot, void* c) { ((Ctx*)c)->t->setSubProgress(float(d) / tot); },
        &ctx);
    bool ok = got == i.sha256;
    t.print("  %s: %s", i.file.c_str(), ok ? "SHA-256 OK" : "DIGEST MISMATCH!");
    return ok;
}

bool verify(const Set& s, Task& t) {
    t.print("Verifying backup %s", s.name.c_str());
    bool ok = true;
    for (size_t k = 0; k < s.items.size(); k++) {
        t.setRange(float(k) / s.items.size(), 1.0f / s.items.size());
        ok = verifyItem(s, s.items[k], t) && ok;
    }
    return ok;
}

bool restore(const Set& s, const std::vector<std::string>& parts, bool verifyFirst, Task& t) {
    std::vector<const Item*> todo;
    for (auto& i : s.items)
        if (std::find(parts.begin(), parts.end(), i.part) != parts.end()) todo.push_back(&i);
    if (todo.empty()) { t.print("Nothing selected"); return false; }
    float verifyShare = verifyFirst ? 0.25f : 0.f;
    if (verifyFirst) {
        t.print("Checking digests before restoring...");
        for (size_t k = 0; k < todo.size(); k++) {
            t.setRange(verifyShare * k / todo.size(), verifyShare / todo.size());
            if (!verifyItem(s, *todo[k], t)) {
                t.print("Aborting: backup is corrupt.");
                return false;
            }
        }
    }
    for (size_t k = 0; k < todo.size(); k++) {
        const Item& i = *todo[k];
        t.setRange(verifyShare + (1 - verifyShare) * k / todo.size(), (1 - verifyShare) / todo.size());
        Partition* p = Partitions::get().find(i.part);
        if (!p) p = Partitions::get().find("/" + i.part);
        if (!p) { t.print("Partition %s not found on this device - skipped", i.part.c_str()); continue; }
        t.setStatus("Restoring " + p->displayName);
        t.print("Restoring %s...", p->displayName.c_str());
        std::string file = s.path + "/" + i.file;
        uint64_t written = 0;
        if (i.type == "img") {
            int out = open(p->blockDev.c_str(), O_WRONLY | O_CLOEXEC);
            if (out < 0) { t.print("  cannot open %s", p->blockDev.c_str()); return false; }
            bool ok = streamOut(file, out, t, written);
            fsync(out);
            close(out);
            if (!ok) { t.print("  FAILED"); return false; }
        } else {
            if (!Partitions::get().wipe(*p, t)) t.print("  warning: wipe before restore was incomplete");
            if (!Partitions::get().mount(*p)) { t.print("  cannot mount %s", p->displayName.c_str()); return false; }
            int fd = -1;
            pid_t pid = spawn("cd " + util::shellQuote(p->contentPath()) + " && tar -xf -" + tarLabelFlags(), fd, false);
            if (pid < 0) return false;
            bool ok = streamOut(file, fd, t, written);
            close(fd);
            int rc = waitChild(pid);
            if (!ok || rc != 0) { t.print("  FAILED (tar exit %d)", rc); return false; }
        }
        t.print("  %s restored", util::humanSize(written).c_str());
    }
    sync();
    return true;
}

}  // namespace tw::backup
