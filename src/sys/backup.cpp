#include "sys/backup.h"
#include "sys/history.h"
#include "core/crypt.h"
#include "core/log.h"
#include "core/md5.h"
#include "core/sha256.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"
#include "sys/tar.h"
#include "miniz.h"

#include <algorithm>
#include <cerrno>
#include <functional>
#include <map>
#include <csignal>
#include <cstring>
#include <memory>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tw::backup {

std::string backupRoot() {
    return Partitions::get().appDir() + "/BACKUPS/" + device::info().serial;
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
    bool open(const std::string& path, bool gzip, const std::string& password = "") {
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd_ < 0) return false;
        gzip_ = gzip;
        if (!password.empty()) {
            enc_ = std::make_unique<crypt::Stream>();
            std::string header;
            if (!enc_->beginEncrypt(password, header) || !putRaw(header.data(), header.size())) return false;
        }
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
        if (ok && enc_) {
            std::string tag = enc_->tag();
            ok = putRaw(tag.data(), tag.size());
        }
        if (fd_ >= 0) { fsync(fd_); ::close(fd_); fd_ = -1; }
        return ok;
    }
    std::string digest() { return sha_.hexDigest(); }
    uint64_t written() const { return out_; }

private:
    bool put(const void* d, size_t n) {
        if (!enc_) return putRaw(d, n);
        tmp_.assign((const uint8_t*)d, (const uint8_t*)d + n);
        enc_->process(tmp_.data(), n);
        return putRaw(tmp_.data(), n);
    }
    bool putRaw(const void* d, size_t n) {
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
    std::unique_ptr<crypt::Stream> enc_;
    std::vector<uint8_t> tmp_;
};

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
        item.file = p.name + ".emmc.win" + std::string(o.compress ? ".gz" : "") + (o.password.empty() ? "" : ".enc");
        item.encrypted = !o.password.empty();
        t.setStatus("Backing up " + p.displayName);
        t.print("Backing up %s (image)...", p.displayName.c_str());
        int in = open(p.blockDev.c_str(), O_RDONLY | O_CLOEXEC);
        if (in < 0) { t.print("  cannot open %s: %s", p.blockDev.c_str(), strerror(errno)); return false; }
        uint64_t total = Partitions::blockSize(p.blockDev);
        if (!sink.open(dir + "/" + item.file, o.compress, o.password)) { close(in); return false; }
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
    item.file = p.name + "." + p.fsType + ".win" + std::string(o.compress ? ".gz" : "") + (o.password.empty() ? "" : ".enc");
    item.encrypted = !o.password.empty();
    t.setStatus("Backing up " + p.displayName);
    t.print("Backing up %s (%s)...", p.displayName.c_str(), p.fsType.c_str());
    uint64_t est = estimateSize(p.mountPoint);
    std::vector<std::string> excl = isDataLike(p) ? std::vector<std::string>{"media", "lost+found"} : std::vector<std::string>{"lost+found"};
    if (!sink.open(dir + "/" + item.file, o.compress, o.password)) return false;
    // FMRRE's own tar: SELinux labels and the other extended attributes always go in.
    uint64_t done = 0;
    tar::Stats ts;
    std::string terr;
    bool ok = tar::create(p.contentPath(), excl, [&](const void* d, size_t n) {
        if (!sink.write(d, n)) return false;
        done += n;
        if (est) t.setSubProgress(std::min(0.99f, float(done) / est));
        speed(t, done, t0);
        return true;
    }, ts, terr, [&]() { return t.cancelled(); });
    ok = sink.close() && ok && !t.cancelled();
    if (!terr.empty() && !t.cancelled()) t.print("  %s", terr.c_str());
    for (auto& n : ts.notes) t.print("  note: %s", n.c_str());
    item.size = sink.written();
    if (o.digest) item.sha256 = sink.digest();
    t.print("  %llu files, %llu folders, %llu links, %d SELinux labels: %s -> %s", (unsigned long long)ts.files,
            (unsigned long long)ts.dirs, (unsigned long long)ts.links, ts.labels, util::humanSize(done).c_str(),
            util::humanSize(item.size).c_str());
    return ok;
}

static void writeManifest(const std::string& dir, const std::vector<Item>& items) {
    std::string m = "# FMRRE backup manifest\n";
    m += "device=" + device::info().device + "\nmodel=" + device::info().model + "\ncreated=" + util::dateStamp() + "\n";
    for (auto& i : items) {
        m += "item=" + i.part + "|" + i.file + "|" + i.type + "|" + (i.compressed ? "1" : "0") + "|" +
             std::to_string(i.size) + "|" + i.sha256 + "|" + (i.encrypted ? "1" : "0") + "\n";
        if (!i.sha256.empty()) util::writeFile(dir + "/" + i.file + ".sha2", i.sha256 + "  " + i.file + "\n");
    }
    util::writeFile(dir + "/fmrre.manifest", m);
}

bool create(const Options& o, Task& t) {
    std::string dir = backupRoot() + "/" + o.name;
    if (!util::mkdirs(dir)) {
        t.print("Cannot create backup folder %s", dir.c_str());
        return false;
    }
    t.print("Backup folder: %s", dir.c_str());
    t.print("Compression: %s, SHA-256: %s, encryption: %s", o.compress ? "gzip" : "off", o.digest ? "on" : "off",
            o.password.empty() ? "off" : "AES-256");
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
    // Ours, then classic TWRP's (and TWRP v2's, from before the rename) - same layout.
    std::string legacy = Partitions::get().legacyDir() + "/BACKUPS/" + device::info().serial;
    for (const std::string& root : {backupRoot(), legacy}) {
        for (auto& e : util::listDir(root)) {
            if (!e.dir) continue;
            Set s;
            s.name = e.name;
            s.path = root + "/" + e.name;
            s.mtime = e.mtime;
            bool dup = false;
            for (auto& o : out) dup |= o.name == s.name;
            if (dup) continue;  // same name in both folders: the newer FMRRE one wins
            std::string m;
            if (util::readFile(s.path + "/fmrre.manifest", m) || util::readFile(s.path + "/twrpv2.manifest", m)) {
                s.hasManifest = true;
                for (auto& line : util::split(m, '\n')) {
                    if (!util::startsWith(line, "item=")) continue;
                    auto f = util::split(line.substr(5), '|', false);
                    if (f.size() < 6) continue;
                    Item i{f[0], f[1], f[2], f[3] == "1", (uint64_t)std::stoull(f[4]), f[5]};
                    i.encrypted = f.size() > 6 ? f[6] == "1" : crypt::isEncrypted(s.path + "/" + i.file);
                    s.items.push_back(i);
                }
            } else {
                // Classic TWRP backup: infer from file names (boot.emmc.win, data.ext4.win, and the split parts
                // TWRP makes of big archives: data.ext4.win000, data.ext4.win001 ...).
                std::map<std::string, std::vector<std::pair<std::string, uint64_t>>> groups;  // "data.ext4.win" -> parts
                for (auto& f : util::listDir(s.path)) {
                    if (f.dir) continue;
                    std::string base = f.name;
                    size_t w = base.rfind(".win");
                    if (w == std::string::npos) continue;
                    std::string tail = base.substr(w + 4);
                    if (!tail.empty() && (tail.size() != 3 || tail.find_first_not_of("0123456789") != std::string::npos)) continue;
                    groups[base.substr(0, w + 4)].push_back({f.name, f.size});
                }
                for (auto& g : groups) {
                    auto parts = util::split(g.first, '.');
                    if (parts.size() < 3) continue;
                    std::sort(g.second.begin(), g.second.end());
                    Item i;
                    i.part = parts[0];
                    i.type = parts[1] == "emmc" ? "img" : "tar";
                    for (size_t k = 0; k < g.second.size(); k++) {
                        std::string sha = util::readLine(s.path + "/" + g.second[k].first + ".sha2");
                        sha = sha.empty() ? "" : util::splitWs(sha)[0];
                        if (sha.empty()) {  // TWRP 3.0 / 3.1 wrote MD5 digests
                            std::string md5 = util::readLine(s.path + "/" + g.second[k].first + ".md5");
                            if (!md5.empty()) sha = "md5:" + util::splitWs(md5)[0];
                        }
                        if (k == 0) i.file = g.second[k].first, i.sha256 = sha;
                        else i.extra.push_back({g.second[k].first, sha});
                        i.size += g.second[k].second;
                    }
                    if (i.type == "img" && !i.extra.empty()) continue;  // images are never split: not a TWRP backup
                    s.items.push_back(i);
                }
            }
            for (auto& i : s.items) s.size += i.size;
            out.push_back(s);
        }
    }
    std::sort(out.begin(), out.end(), [](const Set& a, const Set& b) { return a.mtime > b.mtime; });
    return out;
}

// The plaintext of a backup file: decrypted if it is encrypted, with a one-shot peek for the gzip magic.
class Reader {
public:
    bool open(const std::string& file, const std::string& password, std::string& err) {
        fd_ = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) { err = strerror(errno); return false; }
        total_ = util::fileSize(file);
        left_ = total_;
        char magic[4] = {0};
        if (::pread(fd_, magic, 4, 0) == 4 && !memcmp(magic, "OAES", 4)) {  // TWRP's own encryption (OpenAES)
            err = "encrypted by TWRP (its OpenAES format), which FMRRE can't decrypt - restore it with TWRP, or back up "
                  "again with FMRRE (its encryption works with a password)";
            return false;
        }
        if (crypt::isEncrypted(file)) {
            uint8_t h[crypt::kHeaderSize];
            if (total_ < crypt::kHeaderSize + crypt::kTagSize || ::read(fd_, h, sizeof(h)) != (ssize_t)sizeof(h)) { err = "truncated"; return false; }
            if (password.empty()) { err = "this backup is encrypted - a password is needed"; return false; }
            dec_ = std::make_unique<crypt::Stream>();
            if (!dec_->beginDecrypt(password, h, err)) return false;
            consumed_ = crypt::kHeaderSize;
            left_ = total_ - crypt::kHeaderSize - crypt::kTagSize;
        }
        return true;
    }
    ~Reader() { if (fd_ >= 0) ::close(fd_); }
    ssize_t read(uint8_t* b, size_t n) {
        if (!peek_.empty()) {
            size_t k = std::min(n, peek_.size());
            memcpy(b, peek_.data(), k);
            peek_.erase(0, k);
            return (ssize_t)k;
        }
        n = (size_t)std::min<uint64_t>(n, left_);
        if (!n) return 0;
        ssize_t r;
        do r = ::read(fd_, b, n); while (r < 0 && errno == EINTR);
        if (r <= 0) return r;
        if (dec_) dec_->process(b, (size_t)r);
        left_ -= (uint64_t)r;
        consumed_ += (uint64_t)r;
        return r;
    }
    bool readFull(uint8_t* b, size_t n) {
        while (n) {
            ssize_t r = read(b, n);
            if (r <= 0) return false;
            b += r;
            n -= (size_t)r;
        }
        return true;
    }
    bool skip(size_t n) {
        uint8_t tmp[512];
        while (n) {
            size_t k = std::min(n, sizeof(tmp));
            if (!readFull(tmp, k)) return false;
            n -= k;
        }
        return true;
    }
    bool isGzip() {
        uint8_t m[2];
        if (!readFull(m, 2)) return false;
        peek_.assign((const char*)m, 2);
        return m[0] == 0x1f && m[1] == 0x8b;
    }
    // Encrypted files: read what's left of the data, then compare the HMAC at the end.
    bool finish(std::string& err) {
        if (!dec_) return true;
        peek_.clear();
        uint8_t tmp[1 << 16];
        while (left_) if (read(tmp, sizeof(tmp)) <= 0) { err = "read error"; return false; }
        uint8_t tag[crypt::kTagSize];
        if (::read(fd_, tag, sizeof(tag)) != (ssize_t)sizeof(tag)) { err = "truncated"; return false; }
        if (dec_->tag() != std::string((const char*)tag, sizeof(tag))) { err = "the backup was modified or is corrupt (HMAC mismatch)"; return false; }
        return true;
    }
    uint64_t consumed() const { return consumed_; }
    uint64_t total() const { return total_; }

private:
    int fd_ = -1;
    uint64_t total_ = 0, left_ = 0, consumed_ = 0;
    std::unique_ptr<crypt::Stream> dec_;
    std::string peek_;
};

// Read a backup file, decrypt and decompress if needed, and feed the raw bytes to `out`.
using PlainSink = std::function<bool(const unsigned char*, size_t)>;

// The backup file's plaintext (decrypted, gunzipped) into `sink`.
static bool streamOut(const std::string& file, const PlainSink& sink, Task& t, uint64_t& written, const std::string& password) {
    Reader in;
    std::string err;
    if (!in.open(file, password, err)) { t.print("  %s: %s", util::basename(file).c_str(), err.c_str()); return false; }
    bool gz = in.isGzip();
    std::vector<unsigned char> ib(1 << 18), ob(1 << 18);
    mz_stream zs{};
    bool ok = true;
    int64_t t0 = util::nowMs();
    auto writeAll = [&](const unsigned char* d, size_t n) {
        if (!sink(d, n)) return false;
        written += n;
        return true;
    };
    if (gz) {
        // Parse gzip header (we accept FNAME/FEXTRA/FCOMMENT for classic TWRP/pigz backups).
        unsigned char h[10];
        if (!in.readFull(h, 10)) return false;
        int flg = h[3];
        auto skipZ = [&]() { unsigned char c; while (in.readFull(&c, 1) && c) {} };
        if (flg & 4) { unsigned char x[2] = {0, 0}; if (!in.readFull(x, 2) || !in.skip(size_t(x[0] | (x[1] << 8)))) return false; }
        if (flg & 8) skipZ();
        if (flg & 16) skipZ();
        if (flg & 2 && !in.skip(2)) return false;
        mz_inflateInit2(&zs, -MZ_DEFAULT_WINDOW_BITS);
    }
    bool streamEnd = false;
    while (ok && !streamEnd && !t.cancelled()) {
        ssize_t n = in.read(ib.data(), ib.size());
        if (n <= 0) break;
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
        t.setSubProgress(float(in.consumed()) / std::max<uint64_t>(in.total(), 1));
        speed(t, written, t0);
    }
    if (gz) mz_inflateEnd(&zs);
    if (ok && !t.cancelled() && !in.finish(err)) { t.print("  %s", err.c_str()); ok = false; }
    return ok && !t.cancelled();
}

static bool streamOut(const std::string& file, int out, Task& t, uint64_t& written, const std::string& password) {
    return streamOut(file, [out](const unsigned char* d, size_t n) {
        while (n) {
            ssize_t r = write(out, d, n);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            d += r;
            n -= size_t(r);
        }
        return true;
    }, t, written, password);
}

static bool verifyFile(const Set& s, const std::string& file, const std::string& sha256, Task& t) {
    if (sha256.empty()) {
        t.print("  %s: no digest stored, skipped", file.c_str());
        return true;
    }
    t.setStatus("Verifying " + file);
    if (util::startsWith(sha256, "md5:")) {  // an old TWRP backup's .md5
        bool ok = util::toLower(Md5::hexOfFile(s.path + "/" + file)) == util::toLower(sha256.substr(4));
        t.print("  %s: %s", file.c_str(), ok ? "MD5 OK" : "DIGEST MISMATCH!");
        return ok;
    }
    struct Ctx { Task* t; } ctx{&t};
    std::string got = Sha256::hexOfFile(
        s.path + "/" + file, [](uint64_t d, uint64_t tot, void* c) { ((Ctx*)c)->t->setSubProgress(float(d) / tot); }, &ctx);
    bool ok = got == util::toLower(sha256);
    t.print("  %s: %s", file.c_str(), ok ? "SHA-256 OK" : "DIGEST MISMATCH!");
    return ok;
}

static bool verifyItem(const Set& s, const Item& i, Task& t) {
    bool ok = verifyFile(s, i.file, i.sha256, t);
    for (auto& e : i.extra) ok = verifyFile(s, e.first, e.second, t) && ok;
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

bool encrypted(const Set& s) {
    for (auto& i : s.items)
        if (i.encrypted) return true;
    return false;
}

bool checkPassword(const Set& s, const std::string& password, std::string& err) {
    for (auto& i : s.items) {
        if (!i.encrypted) continue;
        Reader r;
        if (!r.open(s.path + "/" + i.file, password, err)) { err = i.file + ": " + err; return false; }
    }
    return true;
}

static bool restoreImpl(const Set& s, const std::vector<std::string>& parts, bool verifyFirst, Task& t, const std::string& password) {
    std::vector<const Item*> todo;
    for (auto& i : s.items)
        if (std::find(parts.begin(), parts.end(), i.part) != parts.end()) todo.push_back(&i);
    if (todo.empty()) { t.print("Nothing selected"); return false; }
    // A wrong password must be found before any partition is wiped.
    for (auto* i : todo)
        if (i->encrypted) {
            Reader r;
            std::string err;
            if (!r.open(s.path + "/" + i->file, password, err)) { t.print("%s: %s - nothing was changed.", i->file.c_str(), err.c_str()); return false; }
        }
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
            bool ok = streamOut(file, out, t, written, password);
            fsync(out);
            close(out);
            if (!ok) { t.print("  FAILED"); return false; }
        } else {
            if (!Partitions::get().wipe(*p, t)) t.print("  warning: wipe before restore was incomplete");
            if (!Partitions::get().mount(*p)) { t.print("  cannot mount %s", p->displayName.c_str()); return false; }
            // FMRRE's own tar: labels go back; names of classic TWRP backups ("/data/app/...") land in the partition.
            // A split TWRP backup is one archive per part: all go into the same folder, one after the other.
            tar::Extractor x(p->contentPath(), {p->mountPoint, "/" + p->name});
            auto feed = [&x](const unsigned char* d, size_t n) { return x.feed(d, n); };
            bool ok = streamOut(file, feed, t, written, password);
            for (size_t k = 0; ok && k < i.extra.size(); k++) {
                t.print("  part %zu of %zu", k + 2, i.extra.size() + 1);
                ok = x.nextArchive() && streamOut(s.path + "/" + i.extra[k].first, feed, t, written, password);
            }
            ok = ok && x.finish();
            const tar::Stats& xs = x.stats();
            for (auto& n : xs.notes) t.print("  note: %s", n.c_str());
            if (!ok) { t.print("  FAILED%s", x.error().empty() ? "" : (": " + x.error()).c_str()); return false; }
            t.print("  %llu files, %llu folders, %d SELinux labels put back%s", (unsigned long long)xs.files, (unsigned long long)xs.dirs,
                    xs.labels, xs.xattrFailed ? " - some attributes couldn't be set (Advanced > Fix storage contexts)" : "");
        }
        t.print("  %s restored", util::humanSize(written).c_str());
    }
    sync();
    return true;
}

bool restore(const Set& s, const std::vector<std::string>& parts, bool verifyFirst, Task& t, const std::string& password) {
    bool ok = restoreImpl(s, parts, verifyFirst, t, password);
    std::string what;
    for (auto& p : parts) what += (what.empty() ? "" : ", ") + p;
    history::record("Restore", s.name + " (" + what + ")", ok);
    return ok;
}

}  // namespace tw::backup
