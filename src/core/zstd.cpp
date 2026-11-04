#include "core/zstd.h"

#include <algorithm>
#include <cstring>

namespace tw {

namespace {

inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint32_t le16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
inline uint32_t le24(const uint8_t* p) { return le16(p) | uint32_t(p[2]) << 16; }
inline uint32_t le32(const uint8_t* p) { return le24(p) | uint32_t(p[3]) << 24; }
inline uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }
inline int highbit(uint32_t v) {  // v > 0
    int b = 0;
    while (v >>= 1) b++;
    return b;
}

constexpr uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull,
                   P4 = 0x85EBCA77C2B2AE63ull, P5 = 0x27D4EB2F165667C5ull;
inline uint64_t xround(uint64_t acc, uint64_t in) { return rotl64(acc + in * P2, 31) * P1; }
inline uint64_t xmerge(uint64_t acc, uint64_t v) { return (acc ^ xround(0, v)) * P1 + P4; }

constexpr uint32_t kMagic = 0xFD2FB528;
constexpr size_t kBlockMax = 128 << 10;

// ---- bit readers ---------------------------------------------------------------------------------

// Forward, least significant bit first: FSE table descriptions (a few dozen bytes).
struct FwdBits {
    const uint8_t* p;
    size_t n, pos = 0;
    uint32_t peek(int k) const {
        uint32_t v = 0;
        for (int i = 0; i < k; i++) {
            size_t b = pos + size_t(i);
            if (b / 8 < n && ((p[b / 8] >> (b % 8)) & 1)) v |= 1u << i;
        }
        return v;
    }
    uint32_t read(int k) {
        uint32_t v = peek(k);
        pos += size_t(k);
        return v;
    }
    bool over() const { return pos > n * 8; }
    size_t bytes() const { return (pos + 7) / 8; }
};

// Backward, from the end of the stream (whose last byte holds a 1 marking where the bits start): Huffman streams
// and FSE-coded sequences. Reading past the start gives zeros and makes left() negative.
struct BackBits {
    const uint8_t* start = nullptr;
    const uint8_t* ptr = nullptr;
    uint64_t cont = 0;
    unsigned used = 0;  // bits of cont already consumed, from the top
    int64_t left = 0;   // bits not read yet

    bool init(const uint8_t* src, size_t n) {
        if (!n || !src[n - 1]) return false;
        start = src;
        unsigned pad = 8 - unsigned(highbit(src[n - 1]));
        left = int64_t(n) * 8 - pad;
        if (n >= 8) {
            ptr = src + n - 8;
            cont = le64(ptr);
        } else {
            uint8_t b[8] = {0};
            memcpy(b + 8 - n, src, n);
            cont = le64(b);
            ptr = start;
        }
        used = pad;
        return true;
    }
    void reload() {  // afterwards at least 57 bits can be read (until the start is reached)
        if (used < 8 || ptr == start) return;
        size_t back = std::min<size_t>(used >> 3, size_t(ptr - start));
        ptr -= back;
        used -= unsigned(back * 8);
        cont = le64(ptr);
    }
    uint64_t look(unsigned k) const { return k == 0 || used >= 64 ? 0 : (cont << used) >> (64 - k); }
    void skip(unsigned k) {
        used += k;
        left -= int64_t(k);
    }
    uint64_t read(unsigned k) {
        uint64_t v = look(k);
        skip(k);
        return v;
    }
};

// ---- FSE ------------------------------------------------------------------------------------------

struct FseEntry {
    uint16_t sym = 0;
    uint8_t bits = 0;
    uint16_t base = 0;  // next state = base + the next `bits` bits
};

struct Fse {
    std::vector<FseEntry> t;
    int log = 0;
    bool valid = false;

    bool build(const int16_t* norm, int nsym, int al) {
        size_t size = size_t(1) << al;
        t.assign(size, FseEntry{});
        log = al;
        std::vector<uint32_t> next(size_t(nsym), 0);
        size_t high = size - 1;
        for (int s = 0; s < nsym; s++) {
            if (norm[s] == -1) {
                t[high--].sym = uint16_t(s);  // "less than one": the last cells, read with all the bits
                next[size_t(s)] = 1;
            } else {
                next[size_t(s)] = uint32_t(norm[s]);
            }
        }
        size_t step = (size >> 1) + (size >> 3) + 3, mask = size - 1, pos = 0;
        for (int s = 0; s < nsym; s++)
            for (int i = 0; i < norm[s]; i++) {
                t[pos].sym = uint16_t(s);
                do pos = (pos + step) & mask;
                while (pos > high);
            }
        if (pos != 0) return false;
        for (size_t u = 0; u < size; u++) {
            uint32_t ns = next[t[u].sym]++;
            int bits = al - highbit(ns);
            if (bits < 0) return false;
            t[u].bits = uint8_t(bits);
            t[u].base = uint16_t((ns << bits) - size);
        }
        valid = true;
        return true;
    }
    void rle(uint16_t sym) {
        t.assign(1, FseEntry{sym, 0, 0});
        log = 0;
        valid = true;
    }
};

// A table description (normalized counts). Returns the bytes it takes, 0 for damaged data.
size_t readNCount(const uint8_t* src, size_t n, int maxSym, int maxLog, int16_t* norm, int& nsym, int& log) {
    if (n == 0) return 0;
    FwdBits b{src, n};
    log = int(b.read(4)) + 5;
    if (log > maxLog) return 0;
    int remaining = (1 << log) + 1, threshold = 1 << log, nbBits = log + 1, s = 0;
    while (remaining > 1 && s <= maxSym) {
        int max = (2 * threshold - 1) - remaining, v;
        int low = int(b.peek(nbBits - 1));
        if (low < max) {
            v = low;
            b.pos += size_t(nbBits - 1);
        } else {
            v = int(b.peek(nbBits));
            if (v >= threshold) v -= max;
            b.pos += size_t(nbBits);
        }
        int count = v - 1;  // -1: "less than one"
        remaining -= count < 0 ? -count : count;
        norm[s++] = int16_t(count);
        if (count == 0) {  // 2-bit repeat flags: that many more zeros, 3 = and more flags follow
            for (;;) {
                int rep = int(b.read(2));
                for (int k = 0; k < rep; k++) {
                    if (s > maxSym) return 0;
                    norm[s++] = 0;
                }
                if (rep != 3 || b.over()) break;
            }
        }
        while (remaining < threshold && threshold > 1) {
            nbBits--;
            threshold >>= 1;
        }
        if (b.over()) return 0;
    }
    if (remaining != 1 || b.over()) return 0;
    nsym = s;
    return b.bytes();
}

// ---- Huffman ----------------------------------------------------------------------------------------

struct Huf {
    std::vector<uint8_t> sym, bits;
    int maxBits = 0;
    bool valid = false;
};

bool readHufTree(const uint8_t* src, size_t n, Huf& h, size_t& used) {
    if (n < 1) return false;
    uint8_t w[257] = {0};
    int nw = 0;
    uint8_t hb = src[0];
    if (hb >= 128) {  // weights stored directly, 4 bits each
        nw = hb - 127;
        size_t bytes = size_t(nw + 1) / 2;
        if (n < 1 + bytes) return false;
        for (int i = 0; i < nw; i++) w[i] = (i % 2 == 0) ? src[1 + i / 2] >> 4 : src[1 + i / 2] & 15;
        used = 1 + bytes;
    } else {  // FSE-compressed weights: two interleaved states until the stream runs out
        if (hb == 0 || n < 1u + hb) return false;
        int16_t norm[256];
        int nsym, al;
        size_t hdr = readNCount(src + 1, hb, 255, 6, norm, nsym, al);
        if (!hdr || hdr >= hb) return false;
        Fse f;
        if (!f.build(norm, nsym, al)) return false;
        BackBits b;
        if (!b.init(src + 1 + hdr, hb - hdr)) return false;
        uint32_t s1 = uint32_t(b.read(unsigned(al))), s2 = uint32_t(b.read(unsigned(al)));
        if (b.left < 0) return false;
        for (;;) {
            if (nw > 254) return false;
            b.reload();
            w[nw++] = uint8_t(f.t[s1].sym);
            s1 = f.t[s1].base + uint32_t(b.read(f.t[s1].bits));
            if (b.left < 0) {
                w[nw++] = uint8_t(f.t[s2].sym);
                break;
            }
            b.reload();
            w[nw++] = uint8_t(f.t[s2].sym);
            s2 = f.t[s2].base + uint32_t(b.read(f.t[s2].bits));
            if (b.left < 0) {
                w[nw++] = uint8_t(f.t[s1].sym);
                break;
            }
        }
        used = 1 + size_t(hb);
    }
    // The last symbol's weight is implied: it completes the total to a power of two.
    uint32_t sum = 0;
    for (int i = 0; i < nw; i++) {
        if (w[i] > 12) return false;
        if (w[i]) sum += 1u << (w[i] - 1);
    }
    if (sum == 0) return false;
    int maxBits = highbit(sum) + 1;
    uint32_t rest = (1u << maxBits) - sum;
    if (maxBits > 12 || rest == 0 || (rest & (rest - 1)) || nw >= 256) return false;
    w[nw++] = uint8_t(highbit(rest) + 1);
    // Codes go to the lowest weights first, then in symbol order: a symbol of weight w owns 2^(w-1) cells.
    h.maxBits = maxBits;
    h.sym.assign(size_t(1) << maxBits, 0);
    h.bits.assign(size_t(1) << maxBits, 0);
    size_t pos = 0;
    for (int wt = 1; wt <= maxBits; wt++)
        for (int s = 0; s < nw; s++)
            if (w[s] == wt) {
                size_t len = size_t(1) << (wt - 1);
                if (pos + len > h.sym.size()) return false;
                std::fill(h.sym.begin() + long(pos), h.sym.begin() + long(pos + len), uint8_t(s));
                std::fill(h.bits.begin() + long(pos), h.bits.begin() + long(pos + len), uint8_t(maxBits + 1 - wt));
                pos += len;
            }
    if (pos != h.sym.size()) return false;
    h.valid = true;
    return true;
}

bool hufStream(const Huf& h, const uint8_t* src, size_t n, uint8_t* out, size_t count) {
    BackBits b;
    if (!b.init(src, n)) return false;
    for (size_t i = 0; i < count; i++) {
        b.reload();
        uint32_t idx = uint32_t(b.look(unsigned(h.maxBits)));
        out[i] = h.sym[idx];
        b.skip(h.bits[idx]);
    }
    return b.left == 0;
}

// ---- sequence codes (RFC 8878 3.1.1.3.2.1) --------------------------------------------------------

const uint32_t kLLBase[36] = {0,  1,  2,  3,  4,  5,  6,   7,   8,   9,   10,   11,   12,   13,   14,   15,    16,    18,
                              20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
const uint8_t kLLBits[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const uint32_t kMLBase[53] = {3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,  17,  18,  19,  20,   21,   22,   23,   24,   25,   26,  27,  28,  29,
                              30, 31, 32, 33, 34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
const uint8_t kMLBits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                             0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const int16_t kLLDefault[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
const int16_t kMLDefault[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
const int16_t kOFDefault[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

}  // namespace

// ---- xxHash64 ---------------------------------------------------------------------------------------

void Xxh64::reset(uint64_t seed) {
    seed_ = seed;
    v_[0] = seed + P1 + P2;
    v_[1] = seed + P2;
    v_[2] = seed;
    v_[3] = seed - P1;
    total_ = 0;
    memLen_ = 0;
}

void Xxh64::update(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    total_ += n;
    if (memLen_ + n < 32) {
        memcpy(mem_ + memLen_, p, n);
        memLen_ += n;
        return;
    }
    if (memLen_) {
        size_t take = 32 - memLen_;
        memcpy(mem_ + memLen_, p, take);
        for (int k = 0; k < 4; k++) v_[k] = xround(v_[k], le64(mem_ + 8 * k));
        p += take;
        n -= take;
        memLen_ = 0;
    }
    for (; n >= 32; p += 32, n -= 32)
        for (int k = 0; k < 4; k++) v_[k] = xround(v_[k], le64(p + 8 * k));
    memcpy(mem_, p, n);
    memLen_ = n;
}

uint64_t Xxh64::digest() const {
    uint64_t h;
    if (total_ >= 32) {
        h = rotl64(v_[0], 1) + rotl64(v_[1], 7) + rotl64(v_[2], 12) + rotl64(v_[3], 18);
        for (int k = 0; k < 4; k++) h = xmerge(h, v_[k]);
    } else {
        h = seed_ + P5;
    }
    h += total_;
    size_t i = 0;
    for (; i + 8 <= memLen_; i += 8) h = rotl64(h ^ xround(0, le64(mem_ + i)), 27) * P1 + P4;
    if (i + 4 <= memLen_) {
        h = rotl64(h ^ (uint64_t(le32(mem_ + i)) * P1), 23) * P2 + P3;
        i += 4;
    }
    for (; i < memLen_; i++) h = rotl64(h ^ (mem_[i] * P5), 11) * P1;
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

uint64_t Xxh64::of(const void* data, size_t n, uint64_t seed) {
    Xxh64 x(seed);
    x.update(data, n);
    return x.digest();
}

// ---- frames -----------------------------------------------------------------------------------------

struct ZstdFrame::Impl {
    ReadFn in;
    size_t maxWindow;
    std::string err;
    // output: history (the window) + what read() still has to hand out
    std::vector<uint8_t> buf;
    size_t end = 0, readPos = 0;
    // frame state
    bool inFrame = false, done = false, failed = false, checksum = false, hasSize = false;
    uint64_t contentSize = 0, produced = 0;
    size_t window = 0, blockMax = 0;
    int frames = 0;
    uint32_t rep[3] = {1, 4, 8};
    Fse ll, of, ml;
    Huf huf;
    Xxh64 xxh;
    std::vector<uint8_t> blk, lit;

    bool fail(const std::string& e) {
        err = "zstd: " + e;
        failed = true;
        return false;
    }
    bool readExact(uint8_t* p, size_t n, bool* cleanEof = nullptr) {
        size_t got = 0;
        while (got < n) {
            size_t r = in(p + got, n - got);
            if (r == 0) {
                if (cleanEof && got == 0) return *cleanEof = true, false;
                return fail("the file is cut off");
            }
            got += r;
        }
        return true;
    }

    bool nextFrame();
    bool nextBlock();
    bool literals(const uint8_t* src, size_t n, size_t& used);
    bool table(Fse& t, int mode, const uint8_t* src, size_t n, size_t& i, int maxSym, int maxLog, const int16_t* def,
               int defN, int defLog);
    bool sequences(const uint8_t* src, size_t n, uint8_t* out, size_t& produced);
    void room();
};

void ZstdFrame::Impl::room() {
    // Called with everything handed out (readPos == end): keep the window, make space for one more block.
    size_t cap = window + std::max<size_t>(kBlockMax, std::min<size_t>(window, 32 << 20)) + kBlockMax;
    if (end + blockMax > buf.size()) {
        if (buf.size() < cap) {
            buf.resize(std::min(cap, std::max(buf.size() * 2, end + blockMax + kBlockMax)));
        }
        if (end + blockMax > buf.size()) {  // full: slide the window to the front
            size_t keep = std::min(end, window);
            memmove(buf.data(), buf.data() + end - keep, keep);
            end = readPos = keep;
        }
    }
}

bool ZstdFrame::Impl::nextFrame() {
    for (;;) {
        uint8_t m[4];
        bool eof = false;
        if (!readExact(m, 4, &eof)) {
            if (eof && frames > 0) return done = true, false;
            return eof ? fail("empty file") : false;
        }
        uint32_t magic = le32(m);
        if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {  // skippable frame
            uint8_t s[4];
            if (!readExact(s, 4)) return false;
            uint8_t skip[4096];
            for (uint32_t left = le32(s); left;) {
                size_t k = std::min<size_t>(left, sizeof skip);
                if (!readExact(skip, k)) return false;
                left -= uint32_t(k);
            }
            continue;
        }
        if (magic != kMagic) {
            if (frames > 0) return done = true, false;  // trailing bytes after the last frame
            return fail("not a Zstandard frame");
        }
        uint8_t fhd;
        if (!readExact(&fhd, 1)) return false;
        int fcsFlag = fhd >> 6, single = (fhd >> 5) & 1, didFlag = fhd & 3;
        if (fhd & 0x08) return fail("damaged frame header (reserved bit)");
        checksum = fhd & 0x04;
        uint64_t win = 0;
        if (!single) {
            uint8_t wd;
            if (!readExact(&wd, 1)) return false;
            uint64_t base = uint64_t(1) << (10 + (wd >> 3));
            win = base + (base / 8) * (wd & 7);
        }
        static const int kDid[4] = {0, 1, 2, 4}, kFcs[4] = {0, 2, 4, 8};
        uint8_t f[8] = {0};
        if (kDid[didFlag]) {
            if (!readExact(f, size_t(kDid[didFlag]))) return false;
            uint32_t did = 0;
            for (int k = kDid[didFlag] - 1; k >= 0; k--) did = did << 8 | f[k];
            if (did) return fail("this file needs a dictionary");
        }
        int fcsSize = fcsFlag == 0 ? (single ? 1 : 0) : kFcs[fcsFlag];
        contentSize = 0;
        if (fcsSize) {
            if (!readExact(f, size_t(fcsSize))) return false;
            for (int k = fcsSize - 1; k >= 0; k--) contentSize = contentSize << 8 | f[k];
            if (fcsSize == 2) contentSize += 256;
        }
        hasSize = fcsSize > 0;
        if (single) win = contentSize;
        if (win > maxWindow)
            return fail("the file needs " + std::to_string(win >> 20) + " MB of history (made with --long or --ultra); up to " +
                        std::to_string(maxWindow >> 20) + " MB are supported");
        window = size_t(win);
        blockMax = std::min<size_t>(std::max<size_t>(window, 1), kBlockMax);
        rep[0] = 1, rep[1] = 4, rep[2] = 8;
        ll.valid = of.valid = ml.valid = huf.valid = false;
        xxh.reset();
        produced = 0;
        inFrame = true;
        frames++;
        return true;
    }
}

bool ZstdFrame::Impl::nextBlock() {
    uint8_t h[3];
    if (!readExact(h, 3)) return false;
    uint32_t v = le24(h);
    bool last = v & 1;
    int type = (v >> 1) & 3;
    size_t size = v >> 3;
    if (type == 3) return fail("damaged block (reserved type)");
    room();
    size_t start = end, made = 0;
    if (type == 0) {  // raw
        if (size > blockMax) return fail("damaged block size");
        if (!readExact(buf.data() + end, size)) return false;
        made = size;
    } else if (type == 1) {  // RLE: one byte, `size` times
        if (size > blockMax) return fail("damaged block size");
        uint8_t b;
        if (!readExact(&b, 1)) return false;
        memset(buf.data() + end, b, size);
        made = size;
    } else {
        if (size > blockMax || size == 0) return fail("damaged block size");
        blk.resize(size);
        if (!readExact(blk.data(), size)) return false;
        size_t used;
        if (!literals(blk.data(), size, used)) return false;
        if (!sequences(blk.data() + used, size - used, buf.data() + end, made)) return false;
    }
    end += made;
    produced += made;
    xxh.update(buf.data() + start, made);
    if (hasSize && produced > contentSize) return fail("more data than the frame header says");
    if (last) {
        if (hasSize && produced != contentSize) return fail("less data than the frame header says");
        if (checksum) {
            uint8_t c[4];
            if (!readExact(c, 4)) return false;
            if (le32(c) != uint32_t(xxh.digest())) return fail("damaged data (content checksum mismatch)");
        }
        inFrame = false;
    }
    return true;
}

bool ZstdFrame::Impl::literals(const uint8_t* src, size_t n, size_t& used) {
    if (n < 1) return fail("damaged block (no literals header)");
    int type = src[0] & 3, sf = (src[0] >> 2) & 3;
    size_t regen, comp = 0, hsz;
    if (type < 2) {  // raw or RLE literals
        if (sf == 0 || sf == 2) {
            regen = src[0] >> 3;
            hsz = 1;
        } else if (sf == 1) {
            if (n < 2) return fail("damaged literals header");
            regen = (src[0] >> 4) | size_t(src[1]) << 4;
            hsz = 2;
        } else {
            if (n < 3) return fail("damaged literals header");
            regen = (src[0] >> 4) | size_t(src[1]) << 4 | size_t(src[2]) << 12;
            hsz = 3;
        }
        if (regen > blockMax) return fail("damaged literals (too many)");
        if (type == 0) {
            if (n - hsz < regen) return fail("damaged literals (cut off)");
            lit.assign(src + hsz, src + hsz + regen);
            used = hsz + regen;
        } else {
            if (n < hsz + 1) return fail("damaged literals (cut off)");
            lit.assign(regen, src[hsz]);
            used = hsz + 1;
        }
        return true;
    }
    if (sf <= 1) {
        if (n < 3) return fail("damaged literals header");
        uint32_t v = le24(src);
        regen = (v >> 4) & 0x3FF;
        comp = (v >> 14) & 0x3FF;
        hsz = 3;
    } else if (sf == 2) {
        if (n < 4) return fail("damaged literals header");
        uint32_t v = le32(src);
        regen = (v >> 4) & 0x3FFF;
        comp = (v >> 18) & 0x3FFF;
        hsz = 4;
    } else {
        if (n < 5) return fail("damaged literals header");
        uint64_t v = le32(src) | uint64_t(src[4]) << 32;
        regen = size_t(v >> 4) & 0x3FFFF;
        comp = size_t(v >> 22) & 0x3FFFF;
        hsz = 5;
    }
    if (regen > blockMax) return fail("damaged literals (too many)");
    if (n - hsz < comp) return fail("damaged literals (cut off)");
    const uint8_t* p = src + hsz;
    size_t left = comp;
    if (type == 2) {
        size_t tu;
        if (!readHufTree(p, left, huf, tu)) return fail("damaged Huffman table");
        p += tu;
        left -= tu;
    } else if (!huf.valid) {
        return fail("damaged block (repeats a Huffman table that doesn't exist)");
    }
    lit.resize(regen);
    if (sf == 0) {
        if (!hufStream(huf, p, left, lit.data(), regen)) return fail("damaged literals (Huffman stream)");
    } else {
        if (left < 6) return fail("damaged literals (jump table)");
        size_t s[4] = {le16(p), le16(p + 2), le16(p + 4), 0};
        p += 6;
        left -= 6;
        if (s[0] + s[1] + s[2] > left) return fail("damaged literals (jump table)");
        s[3] = left - s[0] - s[1] - s[2];
        size_t seg = (regen + 3) / 4;
        if (seg * 3 > regen) return fail("damaged literals (too few for 4 streams)");
        size_t outPos = 0;
        for (int k = 0; k < 4; k++) {
            size_t cnt = k < 3 ? seg : regen - 3 * seg;
            if (!hufStream(huf, p, s[k], lit.data() + outPos, cnt)) return fail("damaged literals (Huffman stream)");
            p += s[k];
            outPos += cnt;
        }
    }
    used = hsz + comp;
    return true;
}

bool ZstdFrame::Impl::table(Fse& t, int mode, const uint8_t* src, size_t n, size_t& i, int maxSym, int maxLog,
                            const int16_t* def, int defN, int defLog) {
    switch (mode) {
        case 0: return t.build(def, defN, defLog);
        case 1:
            if (i >= n || src[i] > maxSym) return false;
            t.rle(src[i++]);
            return true;
        case 2: {
            int16_t norm[64];
            int nsym, log;
            size_t k = readNCount(src + i, n - i, maxSym, maxLog, norm, nsym, log);
            if (!k) return false;
            i += k;
            return t.build(norm, nsym, log);
        }
        default: return t.valid;  // repeat the previous block's table
    }
}

bool ZstdFrame::Impl::sequences(const uint8_t* src, size_t n, uint8_t* out, size_t& made) {
    made = 0;
    if (n < 1) return fail("damaged block (no sequences header)");
    size_t i = 0;
    uint32_t nseq = src[i++];
    if (nseq >= 128) {
        if (nseq == 255) {
            if (n < 3) return fail("damaged sequences header");
            nseq = le16(src + 1) + 0x7F00;
            i = 3;
        } else {
            if (n < 2) return fail("damaged sequences header");
            nseq = ((nseq - 128) << 8) + src[i++];
        }
    }
    const uint8_t* litp = lit.data();
    size_t litLeft = lit.size();
    if (nseq > 0) {
        if (i >= n) return fail("damaged sequences header");
        uint8_t modes = src[i++];
        if (modes & 3) return fail("damaged sequences header (reserved bits)");
        if (!table(ll, modes >> 6, src, n, i, 35, 9, kLLDefault, 36, 6) || !table(of, (modes >> 4) & 3, src, n, i, 31, 8, kOFDefault, 29, 5) ||
            !table(ml, (modes >> 2) & 3, src, n, i, 52, 9, kMLDefault, 53, 6))
            return fail("damaged sequence tables");
        BackBits b;
        if (!b.init(src + i, n - i)) return fail("damaged sequences (bitstream)");
        b.reload();
        uint32_t sl = uint32_t(b.read(unsigned(ll.log))), so = uint32_t(b.read(unsigned(of.log)));
        b.reload();
        uint32_t sm = uint32_t(b.read(unsigned(ml.log)));
        const uint64_t frameBefore = produced;
        for (uint32_t k = 0; k < nseq; k++) {
            uint32_t llc = ll.t[sl].sym, ofc = of.t[so].sym, mlc = ml.t[sm].sym;
            if (llc > 35 || mlc > 52 || ofc > 31) return fail("damaged sequences (code)");
            b.reload();
            uint64_t offv = (uint64_t(1) << ofc) + b.read(ofc);
            b.reload();
            size_t mlen = kMLBase[mlc] + size_t(b.read(kMLBits[mlc]));
            b.reload();
            size_t llen = kLLBase[llc] + size_t(b.read(kLLBits[llc]));
            uint64_t off;
            if (offv > 3) {
                off = offv - 3;
                rep[2] = rep[1], rep[1] = rep[0], rep[0] = uint32_t(off);
            } else {
                uint64_t idx = offv + (llen == 0 ? 1 : 0);
                if (idx == 1) {
                    off = rep[0];
                } else if (idx == 2) {
                    off = rep[1];
                    rep[1] = rep[0], rep[0] = uint32_t(off);
                } else if (idx == 3) {
                    off = rep[2];
                    rep[2] = rep[1], rep[1] = rep[0], rep[0] = uint32_t(off);
                } else {
                    off = uint64_t(rep[0]) - 1;
                    rep[2] = rep[1], rep[1] = rep[0], rep[0] = uint32_t(off);
                }
            }
            if (k + 1 < nseq) {  // next states: literal lengths, match lengths, then offsets
                b.reload();
                sl = ll.t[sl].base + uint32_t(b.read(ll.t[sl].bits));
                b.reload();
                sm = ml.t[sm].base + uint32_t(b.read(ml.t[sm].bits));
                b.reload();
                so = of.t[so].base + uint32_t(b.read(of.t[so].bits));
            }
            if (b.left < 0) return fail("damaged sequences (bitstream overrun)");
            // execute: literals, then the match
            if (llen > litLeft || made + llen + mlen > blockMax) return fail("damaged sequences (more data than a block holds)");
            memcpy(out + made, litp, llen);
            litp += llen;
            litLeft -= llen;
            made += llen;
            size_t have = size_t(out - buf.data()) + made;  // bytes in the buffer before the match
            if (off == 0 || off > frameBefore + made || off > have) return fail("damaged sequences (a match points before the data)");
            uint8_t* d = out + made;
            const uint8_t* s = d - off;
            if (off >= mlen) {
                memcpy(d, s, mlen);
            } else {
                for (size_t q = 0; q < mlen; q++) d[q] = s[q];  // overlapping: repeats the last `off` bytes
            }
            made += mlen;
        }
        if (b.left != 0) return fail("damaged sequences (bitstream not used up)");
    } else if (i != n) {
        return fail("damaged block (data after the sequences header)");
    }
    if (made + litLeft > blockMax) return fail("damaged block (more data than a block holds)");
    memcpy(out + made, litp, litLeft);
    made += litLeft;
    return true;
}

ZstdFrame::ZstdFrame(ReadFn in, size_t maxWindow) : d_(new Impl) {
    d_->in = std::move(in);
    d_->maxWindow = maxWindow;
}

ZstdFrame::~ZstdFrame() = default;

const std::string& ZstdFrame::error() const { return d_->err; }

ssize_t ZstdFrame::read(void* dst, size_t n) {
    Impl& d = *d_;
    for (;;) {
        if (d.readPos < d.end) {
            size_t k = std::min(n, d.end - d.readPos);
            memcpy(dst, d.buf.data() + d.readPos, k);
            d.readPos += k;
            return ssize_t(k);
        }
        if (d.failed) return -1;
        if (d.done) return 0;
        if (!d.inFrame) {
            if (!d.nextFrame()) {
                if (d.failed) return -1;
                continue;  // done
            }
        } else if (!d.nextBlock()) {
            return -1;
        }
    }
}

}  // namespace tw
