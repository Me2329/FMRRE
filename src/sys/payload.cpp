#include "sys/payload.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "sys/history.h"
#include "core/bunzip2.h"
#include "core/log.h"
#include "core/sha256.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"
#include "sys/pathcheck.h"
#include "sys/super.h"
#include "sys/verity.h"
#include "miniz.h"

// Must match third_party/xz/xz_config.h (that header defines min() and can't be included from C++).
#define XZ_USE_CRC64
#define XZ_DEC_CONCATENATED
extern "C" {
#include "xz.h"
}

namespace tw::payload {

const char* opName(uint32_t t) {
    switch (t) {
        case kReplace: return "REPLACE";
        case kReplaceBz: return "REPLACE_BZ";
        case kMove: return "MOVE";
        case kBsdiff: return "BSDIFF";
        case kSourceCopy: return "SOURCE_COPY";
        case kSourceBsdiff: return "SOURCE_BSDIFF";
        case kZero: return "ZERO";
        case kDiscard: return "DISCARD";
        case kReplaceXz: return "REPLACE_XZ";
        case kPuffdiff: return "PUFFDIFF";
        case kBrotliBsdiff: return "BROTLI_BSDIFF";
        case kZucchini: return "ZUCCHINI";
        case kLz4diffBsdiff: return "LZ4DIFF_BSDIFF";
        case kLz4diffPuffdiff: return "LZ4DIFF_PUFFDIFF";
        default: return "unknown";
    }
}

static bool supportedOp(uint32_t t) { return t == kReplace || t == kReplaceBz || t == kReplaceXz || t == kZero || t == kDiscard; }

uint64_t Op::dstBytes(uint32_t bs) const {
    uint64_t n = 0;
    for (auto& e : dst) n += e.count;
    return n * bs;
}

uint64_t PartitionUpdate::dataBytes(uint32_t bs) const {
    uint64_t n = 0;
    for (auto& o : ops)
        if (o.type != kZero && o.type != kDiscard) n += o.dstBytes(bs);
    return n;
}

namespace {

// ---- protobuf wire format (just what update_metadata.proto needs) ------------------
struct Pb {
    const uint8_t* p;
    const uint8_t* e;
    bool ok = true;

    bool more() const { return ok && p < e; }
    uint64_t varint() {
        uint64_t v = 0;
        for (int s = 0; s < 64 && p < e; s += 7) {
            uint8_t b = *p++;
            v |= uint64_t(b & 0x7f) << s;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    bool key(uint32_t& field, uint32_t& wire) {
        uint64_t k = varint();
        field = uint32_t(k >> 3);
        wire = uint32_t(k & 7);
        return ok;
    }
    Pb sub() {
        uint64_t n = varint();
        if (!ok || n > uint64_t(e - p)) { ok = false; return Pb{p, p}; }
        Pb s{p, p + n};
        p += n;
        return s;
    }
    std::string bytes() {
        Pb s = sub();
        return ok ? std::string((const char*)s.p, size_t(s.e - s.p)) : std::string();
    }
    void skip(uint32_t wire) {
        switch (wire) {
            case 0: varint(); break;
            case 1: if (e - p < 8) ok = false; else p += 8; break;
            case 2: sub(); break;
            case 5: if (e - p < 4) ok = false; else p += 4; break;
            default: ok = false;
        }
    }
};

bool parseExtent(Pb s, Extent& x) {
    uint32_t f, w;
    while (s.more() && s.key(f, w)) {
        if (f == 1 && w == 0) x.start = s.varint();
        else if (f == 2 && w == 0) x.count = s.varint();
        else s.skip(w);
    }
    return s.ok;
}

bool parseOp(Pb s, Op& o) {
    uint32_t f, w;
    while (s.more() && s.key(f, w)) {
        if (f == 1 && w == 0) o.type = (uint32_t)s.varint();
        else if (f == 2 && w == 0) o.dataOffset = s.varint();
        else if (f == 3 && w == 0) o.dataLength = s.varint();
        else if (f == 4 && w == 2) { s.sub(); o.hasSource = true; }
        else if (f == 6 && w == 2) {
            Extent x;
            if (!parseExtent(s.sub(), x)) return false;
            o.dst.push_back(x);
        } else if (f == 8 && w == 2) o.dataHash = s.bytes();
        else s.skip(w);
    }
    return s.ok;
}

bool parseInfo(Pb s, uint64_t& size, std::string& hash) {
    uint32_t f, w;
    while (s.more() && s.key(f, w)) {
        if (f == 1 && w == 0) size = s.varint();
        else if (f == 2 && w == 2) hash = s.bytes();
        else s.skip(w);
    }
    return s.ok;
}

bool parsePartition(Pb s, PartitionUpdate& pu) {
    uint32_t f, w;
    while (s.more() && s.key(f, w)) {
        if (w == 2 && f == 1) pu.name = s.bytes();
        else if (w == 2 && f == 4) pu.fsType = s.bytes();
        else if (w == 2 && f == 6) { uint64_t sz = 0; std::string h; parseInfo(s.sub(), sz, h); pu.hasOldInfo = true; }
        else if (w == 2 && f == 7) { if (!parseInfo(s.sub(), pu.newSize, pu.newHash)) return false; }
        else if (w == 2 && f == 8) {
            Op o;
            if (!parseOp(s.sub(), o)) return false;
            pu.ops.push_back(std::move(o));
        } else if (w == 2 && f == 10) { if (!parseExtent(s.sub(), pu.hashTreeData)) return false; }
        else if (w == 2 && f == 11) { if (!parseExtent(s.sub(), pu.hashTree)) return false; }
        else if (w == 2 && f == 12) pu.hashAlgorithm = s.bytes();
        else if (w == 2 && f == 13) pu.hashSalt = s.bytes();
        else if (w == 2 && f == 14) { if (!parseExtent(s.sub(), pu.fecData)) return false; }
        else if (w == 2 && f == 15) { if (!parseExtent(s.sub(), pu.fec)) return false; }
        else if (w == 0 && f == 16) pu.fecRoots = (uint32_t)s.varint();
        else if (w == 2 && f == 17) pu.version = s.bytes();
        else s.skip(w);
    }
    return s.ok;
}

bool parseDynamic(Pb s, Payload& p) {
    uint32_t f, w;
    while (s.more() && s.key(f, w)) {
        if (f == 1 && w == 2) {
            Pb g = s.sub();
            Group gr;
            uint32_t gf, gw;
            while (g.more() && g.key(gf, gw)) {
                if (gf == 1 && gw == 2) gr.name = g.bytes();
                else if (gf == 2 && gw == 0) gr.size = g.varint();
                else if (gf == 3 && gw == 2) gr.partitions.push_back(g.bytes());
                else g.skip(gw);
            }
            if (!g.ok) return false;
            p.groups.push_back(std::move(gr));
        } else if (f == 2 && w == 0) p.snapshotEnabled = s.varint() != 0;
        else if (f == 3 && w == 0) p.vabc = s.varint() != 0;
        else s.skip(w);
    }
    return s.ok;
}

bool parseManifest(const uint8_t* d, size_t n, Payload& p) {
    Pb s{d, d + n};
    uint32_t f, w;
    while (s.more() && s.key(f, w)) {
        if (f == 3 && w == 0) p.blockSize = (uint32_t)s.varint();
        else if (f == 12 && w == 0) p.minorVersion = (uint32_t)s.varint();
        else if (f == 13 && w == 2) {
            PartitionUpdate pu;
            if (!parsePartition(s.sub(), pu)) return false;
            p.parts.push_back(std::move(pu));
        } else if (f == 14 && w == 0) p.maxTimestamp = (int64_t)s.varint();
        else if (f == 15 && w == 2) { if (!parseDynamic(s.sub(), p)) return false; }
        else if (f == 16 && w == 0) p.partial = s.varint() != 0;
        else if (f == 18 && w == 2) p.securityPatch = s.bytes();
        else s.skip(w);
    }
    return s.ok;
}

uint64_t be(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

std::map<std::string, std::string> parseProps(const std::string& txt) {
    std::map<std::string, std::string> m;
    for (auto& l : util::split(txt, '\n')) {
        size_t eq = l.find('=');
        if (eq != std::string::npos) m[util::trim(l.substr(0, eq))] = util::trim(l.substr(eq + 1));
    }
    return m;
}

bool preadAll(int fd, void* buf, size_t n, uint64_t off) {
    char* p = (char*)buf;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return true;
}

bool pwriteAll(int fd, const void* buf, size_t n, uint64_t off) {
    const char* p = (const char*)buf;
    while (n) {
        ssize_t r = pwrite(fd, p, n, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return true;
}

std::string zipText(mz_zip_archive& z, const char* name) {
    size_t n = 0;
    void* d = mz_zip_reader_extract_file_to_heap(&z, name, &n, 0);
    if (!d) return "";
    std::string s((const char*)d, std::min<size_t>(n, 1 << 20));
    mz_free(d);
    return s;
}

// ---- writing operations ----------------------------------------------------------

// Lays the output of one operation over its destination extents.
struct ExtentWriter {
    int fd;
    uint32_t bs;
    const std::vector<Extent>* ext;
    size_t i = 0;
    uint64_t pos = 0, written = 0;
    std::string err;

    bool write(const uint8_t* d, size_t n) {
        while (n) {
            if (i >= ext->size()) { err = "operation data is longer than its destination"; return false; }
            const Extent& e = (*ext)[i];
            uint64_t len = e.count * bs, k = std::min<uint64_t>(n, len - pos);
            if (!pwriteAll(fd, d, (size_t)k, e.start * bs + pos)) { err = std::string("write failed: ") + strerror(errno); return false; }
            d += k; n -= (size_t)k; pos += k; written += k;
            if (pos == len) { i++; pos = 0; }
        }
        return true;
    }
};

bool zeroRange(int fd, uint64_t off, uint64_t len, bool blockDev) {
    if (blockDev) {
        uint64_t r[2] = {off, len};
        if (ioctl(fd, BLKZEROOUT, r) == 0) return true;
    }
    static const std::vector<uint8_t> zeros(1 << 20, 0);
    while (len) {
        size_t k = (size_t)std::min<uint64_t>(len, zeros.size());
        if (!pwriteAll(fd, zeros.data(), k, off)) return false;
        off += k; len -= k;
    }
    return true;
}

class XzDecoder {
public:
    XzDecoder() {
        static bool crc = false;
        if (!crc) { xz_crc32_init(); xz_crc64_init(); crc = true; }
        d_ = xz_dec_init(XZ_DYNALLOC, 64u << 20);
    }
    ~XzDecoder() { if (d_) xz_dec_end(d_); }
    bool run(const std::string& in, ExtentWriter& w, std::string& err) {
        if (!d_) { err = "xz: out of memory"; return false; }
        xz_dec_reset(d_);
        std::vector<uint8_t> out(256 << 10);
        xz_buf b{};
        b.in = (const uint8_t*)in.data();
        b.in_size = in.size();
        for (;;) {
            b.out = out.data();
            b.out_pos = 0;
            b.out_size = out.size();
            xz_ret r = xz_dec_catrun(d_, &b, 1);
            if (b.out_pos && !w.write(out.data(), b.out_pos)) { err = w.err; return false; }
            if (r == XZ_STREAM_END) return true;
            if (r != XZ_OK) {
                err = r == XZ_MEMLIMIT_ERROR || r == XZ_MEM_ERROR ? "xz: dictionary too large"
                      : r == XZ_OPTIONS_ERROR ? "xz: unsupported options" : "xz: corrupt data";
                return false;
            }
            if (b.out_pos == 0 && b.in_pos == b.in_size) { err = "xz: truncated data"; return false; }
        }
    }

private:
    xz_dec* d_ = nullptr;
};

struct Progress {
    Task& t;
    uint64_t total, base = 0;
    int64_t startMs = util::nowMs(), lastMs = 0;
    uint64_t bytesForSpeed = 0;
    void set(uint64_t done) {
        int64_t now = util::nowMs();
        if (now - lastMs < 100 && done < total) return;
        lastMs = now;
        t.setSubProgress(total ? float(double(base + done) / double(total)) : 1.f);
        double s = (now - startMs) / 1000.0;
        if (s > 0.5) t.setDetail(util::humanSize(uint64_t((base + done) / s)) + "/s");
    }
};

bool applyOps(const Payload& p, const PartitionUpdate& pu, int pfd, int fd, bool fresh, bool blockDev, Task& t, Progress& pr,
              std::string& err) {
    const uint32_t bs = p.blockSize;
    XzDecoder xz;
    uint64_t done = 0;
    std::vector<uint8_t> buf(1 << 20);
    for (size_t i = 0; i < pu.ops.size(); i++) {
        if (t.cancelled()) { err = "cancelled"; return false; }
        const Op& op = pu.ops[i];
        uint64_t want = op.dstBytes(bs);
        uint64_t at = p.base + p.dataStart + op.dataOffset;
        ExtentWriter w{fd, bs, &op.dst};
        auto where = [&]() { return pu.name + " operation " + std::to_string(i + 1) + "/" + std::to_string(pu.ops.size()) + " (" + opName(op.type) + ")"; };
        switch (op.type) {
            case kZero:
            case kDiscard:
                if (!fresh)
                    for (auto& e : op.dst)
                        if (!zeroRange(fd, e.start * bs, e.count * bs, blockDev)) { err = where() + ": write failed: " + strerror(errno); return false; }
                break;
            case kReplace: {
                if (op.dataLength != want) { err = where() + ": data size doesn't match its destination"; return false; }
                Sha256 h;
                for (uint64_t off = 0; off < op.dataLength;) {
                    size_t k = (size_t)std::min<uint64_t>(buf.size(), op.dataLength - off);
                    if (!preadAll(pfd, buf.data(), k, at + off)) { err = where() + ": cannot read the payload (file truncated?)"; return false; }
                    h.update(buf.data(), k);
                    if (!w.write(buf.data(), k)) { err = where() + ": " + w.err; return false; }
                    off += k;
                }
                if (!op.dataHash.empty() && h.digest() != op.dataHash) {
                    err = where() + ": data is corrupt (SHA-256 mismatch) - download the zip again";
                    return false;
                }
                break;
            }
            case kReplaceBz:
            case kReplaceXz: {
                if (op.dataLength > (512u << 20)) { err = where() + ": compressed blob too large"; return false; }
                std::string blob(op.dataLength, '\0');
                if (!preadAll(pfd, &blob[0], blob.size(), at)) { err = where() + ": cannot read the payload (file truncated?)"; return false; }
                if (!op.dataHash.empty()) {
                    Sha256 h;
                    h.update(blob.data(), blob.size());
                    if (h.digest() != op.dataHash) { err = where() + ": data is corrupt (SHA-256 mismatch) - download the zip again"; return false; }
                }
                std::string e2;
                if (op.type == kReplaceXz) {
                    if (!xz.run(blob, w, e2)) { err = where() + ": " + e2; return false; }
                } else {
                    size_t pos = 0;
                    Bunzip2 bz([&](uint8_t* out, size_t cap) -> ssize_t {
                        size_t k = std::min(cap, blob.size() - pos);
                        memcpy(out, blob.data() + pos, k);
                        pos += k;
                        return (ssize_t)k;
                    });
                    for (;;) {
                        ssize_t r = bz.read(buf.data(), buf.size());
                        if (r < 0) { err = where() + ": " + bz.error(); return false; }
                        if (r == 0) break;
                        if (!w.write(buf.data(), (size_t)r)) { err = where() + ": " + w.err; return false; }
                    }
                }
                if (w.written != want) {
                    err = where() + ": produced " + std::to_string(w.written) + " bytes instead of " + std::to_string(want);
                    return false;
                }
                break;
            }
            default:
                err = where() + ": not supported (incremental OTA)";
                return false;
        }
        done += want;
        pr.set(done);
    }
    return true;
}

bool computeVerity(const Payload& p, const PartitionUpdate& pu, int fd, Task& t, std::string& err) {
    const uint64_t bs = p.blockSize;
    if (pu.hashTree.count) {
        t.setStatus("Computing the dm-verity hash tree of " + pu.name);
        std::string root;
        if (!verity::buildHashTree(fd, pu.hashTreeData.start * bs, pu.hashTreeData.count * bs, pu.hashTree.start * bs,
                                   pu.hashTree.count * bs, (uint32_t)bs, pu.hashAlgorithm.empty() ? "sha256" : pu.hashAlgorithm,
                                   pu.hashSalt, root, err)) {
            err = pu.name + ": " + err;
            return false;
        }
        t.print("  %s: hash tree computed (root %s...)", pu.name.c_str(), Sha256::toHex(root).substr(0, 16).c_str());
    }
    if (pu.fec.count) {
        t.setStatus("Computing error correction (FEC) of " + pu.name);
        if (!verity::encodeFec(fd, pu.fecData.start * bs, pu.fecData.count * bs, pu.fec.start * bs, pu.fec.count * bs, pu.fecRoots,
                               (uint32_t)bs, err)) {
            err = pu.name + ": " + err;
            return false;
        }
        t.print("  %s: FEC computed (%u roots)", pu.name.c_str(), pu.fecRoots);
    }
    return true;
}

// Drop cached pages so a read-back really comes from the storage, not from what we just wrote.
void dropCache(int fd, bool blockDev) {
    fsync(fd);
    if (blockDev) ioctl(fd, BLKFLSBUF, 0);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
}

bool hashMatches(int fd, uint64_t size, const std::string& want, Task& t, std::string& err) {
    if (want.empty()) return true;
    Sha256 h;
    std::vector<uint8_t> buf(1 << 20);
    for (uint64_t off = 0; off < size;) {
        if (t.cancelled()) { err = "cancelled"; return false; }
        size_t k = (size_t)std::min<uint64_t>(buf.size(), size - off);
        if (!preadAll(fd, buf.data(), k, off)) { err = std::string("read-back failed: ") + strerror(errno); return false; }
        h.update(buf.data(), k);
        off += k;
    }
    if (h.digest() != want) { err = "SHA-256 of the written partition doesn't match the payload"; return false; }
    return true;
}

std::string otherSlot() { return device::info().slot == "_b" ? "_a" : "_b"; }

std::string partitionDevice(const std::string& name, const std::string& suffix) {
    std::string by = Partitions::byNameDir();
    for (auto c : {name + suffix, name})
        if (util::exists(by + "/" + c)) return by + "/" + c;
    return "";
}

std::vector<lp::SlotGroup> slotGroups(const Payload& p) {
    std::vector<lp::SlotGroup> out;
    for (auto& g : p.groups) {
        lp::SlotGroup sg;
        sg.name = g.name;
        sg.maxSize = g.size;
        for (auto& n : g.partitions) {
            const PartitionUpdate* pu = p.find(n);
            sg.parts.push_back({n, pu ? pu->newSize : 0});
        }
        out.push_back(std::move(sg));
    }
    return out;
}

}  // namespace

// ---- Payload ------------------------------------------------------------------------

bool Payload::full() const { return unsupported().empty(); }

std::vector<std::string> Payload::unsupported() const {
    std::vector<std::string> out;
    for (auto& pu : parts)
        for (auto& o : pu.ops)
            if (!supportedOp(o.type)) {
                std::string n = opName(o.type);
                if (n == "unknown") n = "type " + std::to_string(o.type);
                if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
            }
    return out;
}

bool Payload::isDynamic(const std::string& part) const {
    for (auto& g : groups)
        if (std::find(g.partitions.begin(), g.partitions.end(), part) != g.partitions.end()) return true;
    return false;
}

const PartitionUpdate* Payload::find(const std::string& name) const {
    for (auto& pu : parts)
        if (pu.name == name) return &pu;
    return nullptr;
}

uint64_t Payload::totalWriteBytes() const {
    uint64_t n = 0;
    for (auto& pu : parts) n += pu.newSize;
    return n;
}

std::string Payload::device() const {
    auto it = metadata.find("pre-device");
    return it == metadata.end() ? "" : it->second;
}

std::string Payload::describe() const {
    std::string s = full() ? "full OTA" : "incremental OTA";
    s += " \xE2\x80\xA2 " + std::to_string(parts.size()) + " partitions \xE2\x80\xA2 " + util::humanSize(totalWriteBytes());
    if (!securityPatch.empty()) s += " \xE2\x80\xA2 patch " + securityPatch;
    return s;
}

bool isOtaZip(const std::string& path) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, path.c_str(), 0)) return false;
    bool r = mz_zip_reader_locate_file(&z, "payload.bin", nullptr, 0) >= 0;
    mz_zip_reader_end(&z);
    return r;
}

bool open(const std::string& path, Payload& p, std::string& err) {
    p = Payload();
    p.path = path;
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { err = path + ": " + strerror(errno); return false; }
    struct stat st;
    fstat(fd, &st);
    uint64_t fileSize = (uint64_t)st.st_size;
    uint8_t h[24] = {};
    if (!preadAll(fd, h, 4, 0)) { close(fd); err = "file too small"; return false; }
    if (memcmp(h, "PK\x03\x04", 4) == 0) {
        mz_zip_archive z{};
        if (!mz_zip_reader_init_file(&z, path.c_str(), 0)) { close(fd); err = "not a readable zip"; return false; }
        int idx = mz_zip_reader_locate_file(&z, "payload.bin", nullptr, 0);
        mz_zip_archive_file_stat zs;
        if (idx < 0 || !mz_zip_reader_file_stat(&z, idx, &zs)) {
            mz_zip_reader_end(&z);
            close(fd);
            err = "this zip has no payload.bin (not an A/B OTA package)";
            return false;
        }
        p.metadata = parseProps(zipText(z, "META-INF/com/android/metadata"));
        p.properties = parseProps(zipText(z, "payload_properties.txt"));
        mz_zip_reader_end(&z);
        if (zs.m_method != 0) {
            close(fd);
            err = "payload.bin is compressed inside this zip; extract it (unzip) and select payload.bin itself";
            return false;
        }
        uint8_t lh[30];
        if (!preadAll(fd, lh, sizeof(lh), zs.m_local_header_ofs) || be(lh, 1) != 0x50 || lh[1] != 0x4b || lh[2] != 3 || lh[3] != 4) {
            close(fd);
            err = "corrupt zip (local header of payload.bin)";
            return false;
        }
        p.inZip = true;
        p.base = zs.m_local_header_ofs + 30 + (lh[26] | lh[27] << 8) + (lh[28] | lh[29] << 8);
        p.size = zs.m_comp_size;
    } else {
        p.base = 0;
        p.size = fileSize;
    }
    if (p.base + p.size > fileSize) { close(fd); err = "file is truncated (incomplete download?)"; return false; }
    if (p.size < 24 || !preadAll(fd, h, 24, p.base) || memcmp(h, "CrAU", 4) != 0) {
        close(fd);
        err = "not an update_engine payload (no CrAU header)";
        return false;
    }
    p.version = be(h + 4, 8);
    p.manifestSize = be(h + 12, 8);
    if (p.version != 2) { close(fd); err = "payload format version " + std::to_string(p.version) + " is not supported (needs 2)"; return false; }
    p.metadataSigSize = (uint32_t)be(h + 20, 4);
    const uint64_t header = 24;
    if (p.manifestSize > (64u << 20) || header + p.manifestSize + p.metadataSigSize > p.size) {
        close(fd);
        err = "corrupt payload header";
        return false;
    }
    std::string man(p.manifestSize, '\0');
    bool ok = preadAll(fd, &man[0], man.size(), p.base + header);
    close(fd);
    if (!ok) { err = "cannot read the manifest"; return false; }
    if (!parseManifest((const uint8_t*)man.data(), man.size(), p)) { err = "corrupt manifest"; return false; }
    p.dataStart = header + p.manifestSize + p.metadataSigSize;
    if (!p.blockSize || p.blockSize % 512) { err = "bad block size " + std::to_string(p.blockSize); return false; }
    if (p.parts.empty()) { err = "the payload contains no partitions"; return false; }
    // Every operation must stay inside the payload and inside its partition.
    const uint64_t blobs = p.size - p.dataStart;
    for (auto& pu : p.parts) {
        if (pu.name.empty() || pu.name.find('/') != std::string::npos || pu.name.size() > 32) { err = "bad partition name in payload"; return false; }
        uint64_t blocks = (pu.newSize + p.blockSize - 1) / p.blockSize;
        for (auto& o : pu.ops) {
            if (o.dataLength && (o.dataOffset > blobs || o.dataLength > blobs - o.dataOffset)) {
                err = pu.name + ": operation data outside the payload (truncated download?)";
                return false;
            }
            for (auto& e : o.dst)
                if (pu.newSize && (e.start > blocks || e.count > blocks - e.start)) { err = pu.name + ": operation outside the partition"; return false; }
        }
        for (const Extent* e : {&pu.hashTreeData, &pu.hashTree, &pu.fecData, &pu.fec})
            if (e->count && (e->start > blocks || e->count > blocks - e->start)) { err = pu.name + ": verity data outside the partition"; return false; }
    }
    return true;
}

// ---- extract ------------------------------------------------------------------------

bool extract(const Payload& p, const std::vector<std::string>& names, const std::string& outDir, bool verify, Task& t) {
    std::vector<const PartitionUpdate*> sel;
    for (auto& pu : p.parts)
        if (names.empty() || std::find(names.begin(), names.end(), pu.name) != names.end()) sel.push_back(&pu);
    for (auto& n : names)
        if (!p.find(n)) { t.print("No partition named '%s' in this payload.", n.c_str()); return false; }
    if (!p.full()) {
        t.print("This is an incremental OTA (%s): its images are patches against the old build and can't be extracted alone.",
                util::trim(p.unsupported()[0]).c_str());
        return false;
    }
    if (!util::mkdirs(outDir)) { t.print("Cannot create %s", outDir.c_str()); return false; }
    uint64_t need = 0, total = 0;
    for (auto* pu : sel) {
        need += pu->dataBytes(p.blockSize) + (pu->hashTree.count + pu->fec.count) * p.blockSize;
        total += pu->newSize;
    }
    struct statvfs vf;
    if (statvfs(outDir.c_str(), &vf) == 0) {
        uint64_t free = uint64_t(vf.f_bavail) * vf.f_frsize;
        if (free < need) {
            t.print("Not enough space in %s: need %s, %s free.", outDir.c_str(), util::humanSize(need).c_str(), util::humanSize(free).c_str());
            return false;
        }
    }
    int pfd = ::open(p.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (pfd < 0) { t.print("Cannot open %s", p.path.c_str()); return false; }
    t.print("Extracting %zu partition(s) to %s", sel.size(), pathcheck::pretty(outDir).c_str());
    Progress pr{t, total};
    const float base = t.rangeStart(), len = t.rangeLen();
    bool ok = true;
    for (auto* pu : sel) {
        std::string out = outDir + "/" + pu->name + ".img";
        t.setStatus("Extracting " + pu->name + ".img");
        int fd = ::open(out.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0 || ftruncate(fd, (off_t)pu->newSize) != 0) {
            t.print("Cannot create %s: %s", out.c_str(), strerror(errno));
            if (fd >= 0) close(fd);
            ok = false;
            break;
        }
        std::string err;
        t.setRange(base, len);
        bool good = applyOps(p, *pu, pfd, fd, true, false, t, pr, err) && computeVerity(p, *pu, fd, t, err);
        if (good) dropCache(fd, false);
        if (good && verify) {
            t.setStatus("Verifying " + pu->name + ".img");
            good = hashMatches(fd, pu->newSize, pu->newHash, t, err);
        }
        fsync(fd);
        close(fd);
        pr.base += pu->newSize;
        if (!good) {
            t.print("%s: %s", pu->name.c_str(), err.c_str());
            unlink(out.c_str());
            ok = false;
            break;
        }
        t.print("  %-14s %10s  %s", (pu->name + ".img").c_str(), util::humanSize(pu->newSize).c_str(),
                verify && !pu->newHash.empty() ? "SHA-256 OK" : "");
    }
    close(pfd);
    t.setRange(base, len);
    t.setSubProgress(1);
    if (ok) t.print("Done. Images are in %s", pathcheck::pretty(outDir).c_str());
    return ok;
}

// ---- install ------------------------------------------------------------------------

Plan plan(const Payload& p, const InstallOptions& o) {
    Plan pl;
    const device::Info& di = device::info();
    if (!p.full()) {
        pl.error = "Incremental OTA (" + p.unsupported()[0] + "): it patches one exact build. Use the full OTA / ROM zip.";
        return pl;
    }
    if (p.partial) pl.notes.push_back("Partial update: only the partitions in it are written; the rest of the slot is not touched.");
    std::string dev = p.device();
    if (!dev.empty() && !o.force) {
        bool match = false;
        for (auto& d : util::split(dev, ','))
            for (auto& d2 : util::split(d, '|'))
                if (util::trim(d2) == di.device) match = true;
        if (!match && !di.device.empty()) {
            pl.error = "This OTA is for '" + dev + "', but this phone is '" + di.device + "'.";
            pl.deviceMismatch = true;
            return pl;
        }
    }
    if (!di.ab) { pl.error = "payload.bin OTAs are for A/B phones, and this one has no slots."; return pl; }
    pl.targetSlot = o.slot.empty() ? otherSlot() : o.slot;
    if (pl.targetSlot != "_a" && pl.targetSlot != "_b") { pl.error = "Unknown slot " + pl.targetSlot; return pl; }
    std::string why;
    if (!p.groups.empty()) {
        if (lp::busy(why)) { pl.error = why; return pl; }
        lp::SlotFit f = lp::fitSlot(slotGroups(p), pl.targetSlot);
        pl.superNeeded = f.needed;
        pl.superFree = f.available;
        if (!f.fits) { pl.error = "The new system partitions don't fit in super: " + f.error; return pl; }
        if (f.needsFreeSource) {
            if (!o.freeSourceIfNeeded) { pl.error = "super only has room for one slot; allow removing the current slot's partitions"; return pl; }
            pl.needsFreeSource = true;
            pl.notes.push_back("super only has room for one slot: the current slot (" + di.slot +
                               ")'s system partitions are removed first, like Android's own recovery does. Don't reboot before the install "
                               "finishes.");
        }
    }
    bool bootSide = false;
    for (auto& pu : p.parts) {
        if (p.isDynamic(pu.name)) continue;
        std::string d = partitionDevice(pu.name, pl.targetSlot);
        if (d.empty()) { pl.error = "This phone has no " + pu.name + pl.targetSlot + " partition - is the OTA for another model?"; return pl; }
        uint64_t sz = Partitions::blockSize(d);
        if (sz && sz < pu.newSize) {
            pl.error = pu.name + pl.targetSlot + " is " + util::humanSize(sz) + " but the new image is " + util::humanSize(pu.newSize);
            return pl;
        }
        if (pu.name == "boot" || pu.name == "vendor_boot" || pu.name == "init_boot") bootSide = true;
    }
    if (bootSide && partitionDevice("recovery", pl.targetSlot).empty())
        pl.notes.push_back("This phone keeps its recovery in boot/vendor_boot, which the update replaces: after it you get the ROM's "
                           "recovery. Reinstall FMRRE (Advanced > Install FMRRE permanently) to keep it.");
    if (o.switchSlot) pl.notes.push_back("Slot " + std::string(pl.targetSlot == "_b" ? "B" : "A") + " becomes active: reboot to start the new system.");
    pl.notes.push_back("Data is not wiped. Coming from another ROM? Format Data afterwards.");
    pl.ok = true;
    return pl;
}

static bool installImpl(const Payload& p, const InstallOptions& o, Task& t) {
    Plan pl = plan(p, o);
    auto md = [&](const char* k) {
        auto it = p.metadata.find(k);
        return it == p.metadata.end() ? std::string() : it->second;
    };
    t.print("A/B OTA: %s", util::basename(p.path).c_str());
    if (!md("post-build").empty()) t.print("  Build:  %s", md("post-build").c_str());
    t.print("  %s", p.describe().c_str());
    if (!pl.ok) { t.print("Not installing: %s", pl.error.c_str()); return false; }
    const std::string slot = pl.targetSlot;
    t.print("  Target: slot %s", slot == "_b" ? "B" : "A");
    for (auto& n : pl.notes) t.print("  Note: %s", n.c_str());

    const float base = t.rangeStart(), len = t.rangeLen();
    if (!p.groups.empty()) {
        t.setStatus("Preparing dynamic partitions");
        t.setRange(base, len * 0.02f);
        if (!lp::prepareSlot(slotGroups(p), slot, o.freeSourceIfNeeded, t)) return false;
    }
    int pfd = ::open(p.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (pfd < 0) { t.print("Cannot open %s", p.path.c_str()); return false; }
    uint64_t total = std::max<uint64_t>(1, p.totalWriteBytes());
    Progress pr{t, total};
    t.setRange(base + len * 0.02f, len * 0.96f);
    bool ok = true;
    for (auto& pu : p.parts) {
        std::string name = pu.name + slot;
        std::string dev = p.isDynamic(pu.name) ? lp::mappedPath(name) : partitionDevice(pu.name, slot);
        if (dev.empty()) { t.print("%s: no block device", name.c_str()); ok = false; break; }
        t.setStatus("Writing " + name);
        std::string err;
        if (!lp::ensureWritable(dev, err)) { t.print("%s is read-only: %s", name.c_str(), err.c_str()); ok = false; break; }
        int fd = ::open(dev.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) { t.print("Cannot open %s: %s", dev.c_str(), strerror(errno)); ok = false; break; }
        struct stat st;
        fstat(fd, &st);
        bool blk = S_ISBLK(st.st_mode);
        if (!blk && (uint64_t)st.st_size < pu.newSize && ftruncate(fd, (off_t)pu.newSize) != 0) {}
        int64_t t0 = util::nowMs();
        bool good = applyOps(p, pu, pfd, fd, false, blk, t, pr, err) && computeVerity(p, pu, fd, t, err);
        if (good) dropCache(fd, blk);
        if (good && o.verify) {
            t.setStatus("Verifying " + name);
            good = hashMatches(fd, pu.newSize, pu.newHash, t, err);
        }
        close(fd);
        pr.base += pu.newSize;
        if (!good) { t.print("%s: %s", name.c_str(), err.c_str()); ok = false; break; }
        t.print("  %-16s %10s  %s (%s)", name.c_str(), util::humanSize(pu.newSize).c_str(),
                o.verify && !pu.newHash.empty() ? "verified" : "written", util::humanDuration(util::nowMs() - t0).c_str());
    }
    close(pfd);
    if (!ok) {
        t.print("The install did NOT finish. Slot %s is incomplete and was not made active; your current slot (%s) is unchanged%s.",
                slot == "_b" ? "B" : "A", device::info().slot == "_b" ? "B" : "A",
                pl.needsFreeSource ? " except for its system partitions, which were removed: install again before rebooting" : "");
        return false;
    }
    t.setRange(base, len);
    t.setProgress(base + len * 0.99f);
    if (o.switchSlot) {
        std::string msg;
        if (!device::setActiveSlot(slot, msg)) {
            t.print("Installed, but switching slots failed: %s", msg.c_str());
            return false;
        }
        t.print("%s", msg.c_str());
    }
    t.setSubProgress(1);
    t.print("OTA installed to slot %s.", slot == "_b" ? "B" : "A");
    return true;
}

bool install(const Payload& p, const InstallOptions& o, Task& t) {
    bool ok = installImpl(p, o, t);
    std::string slot = o.slot.empty() ? (device::info().slot == "_b" ? "A" : "B") : o.slot.substr(1);
    history::record("A/B OTA", util::basename(p.path) + " -> slot " + util::toLower(slot) +
                                   (o.historyNote.empty() ? "" : " - " + o.historyNote), ok);
    return ok;
}

}  // namespace tw::payload
