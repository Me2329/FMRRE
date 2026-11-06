#include "core/lz4.h"

#include <algorithm>
#include <cstring>

namespace tw {

namespace {

constexpr uint32_t P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
constexpr uint32_t kMagic = 0x184D2204;

inline uint32_t rotl(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint32_t mix(uint32_t acc, uint32_t in) { return rotl(acc + in * P2, 13) * P1; }

}  // namespace

// ---- xxHash32 ----------------------------------------------------------------------------------------

void Xxh32::reset(uint32_t seed) {
    seed_ = seed;
    v_[0] = seed + P1 + P2;
    v_[1] = seed + P2;
    v_[2] = seed;
    v_[3] = seed - P1;
    total_ = 0;
    memLen_ = 0;
}

void Xxh32::stripe(const uint8_t* p) {
    for (int k = 0; k < 4; k++) v_[k] = mix(v_[k], le32(p + 4 * k));
}

void Xxh32::update(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    total_ += n;
    if (memLen_ + n < 16) {
        memcpy(mem_ + memLen_, p, n);
        memLen_ += n;
        return;
    }
    if (memLen_) {
        size_t take = 16 - memLen_;
        memcpy(mem_ + memLen_, p, take);
        stripe(mem_);
        p += take;
        n -= take;
        memLen_ = 0;
    }
    for (; n >= 16; p += 16, n -= 16) stripe(p);
    memcpy(mem_, p, n);
    memLen_ = n;
}

uint32_t Xxh32::digest() const {
    uint32_t h = total_ >= 16 ? rotl(v_[0], 1) + rotl(v_[1], 7) + rotl(v_[2], 12) + rotl(v_[3], 18) : seed_ + P5;
    h += uint32_t(total_);
    size_t k = 0;
    for (; k + 4 <= memLen_; k += 4) h = rotl(h + le32(mem_ + k) * P3, 17) * P4;
    for (; k < memLen_; k++) h = rotl(h + mem_[k] * P5, 11) * P1;
    h ^= h >> 15;
    h *= P2;
    h ^= h >> 13;
    h *= P3;
    h ^= h >> 16;
    return h;
}

uint32_t Xxh32::of(const void* data, size_t n, uint32_t seed) {
    Xxh32 x(seed);
    x.update(data, n);
    return x.digest();
}

// ---- blocks ------------------------------------------------------------------------------------------

bool lz4Block(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t limit, std::string& err) {
    const size_t base = out.size();
    out.reserve(base + limit);
    size_t i = 0;
    auto bad = [&](const char* why) { err = why; return false; };
    auto length = [&](size_t& len) {  // 15 means "more bytes follow", each 255 means "and more"
        if (len != 15) return true;
        for (;;) {
            if (i >= n) return false;
            uint8_t b = src[i++];
            len += b;
            if (b != 255) return true;
        }
    };
    while (i < n) {
        uint8_t token = src[i++];
        size_t lit = token >> 4;
        if (!length(lit) || lit > n - i) return bad("damaged block (literals run past its end)");
        if (out.size() - base + lit > limit) return bad("damaged block (more data than a block holds)");
        out.insert(out.end(), src + i, src + i + lit);
        i += lit;
        if (i == n) return true;  // the last sequence has literals only
        if (n - i < 2) return bad("damaged block (cut off)");
        size_t off = size_t(src[i]) | size_t(src[i + 1]) << 8;
        i += 2;
        if (off == 0 || off > out.size()) return bad("damaged block (a match points before the data)");
        size_t ml = token & 15;
        if (!length(ml)) return bad("damaged block (cut off)");
        ml += 4;
        if (out.size() - base + ml > limit) return bad("damaged block (more data than a block holds)");
        size_t pos = out.size();
        out.resize(pos + ml);
        uint8_t* d = out.data();
        if (off >= ml) memcpy(d + pos, d + pos - off, ml);
        else
            for (size_t k = 0; k < ml; k++) d[pos + k] = d[pos + k - off];  // overlapping: repeats the last `off` bytes
    }
    return true;  // an empty block
}

// ---- frames ------------------------------------------------------------------------------------------

bool Lz4Frame::readExact(uint8_t* p, size_t n, bool* cleanEof) {
    size_t got = 0;
    while (got < n) {
        size_t r = in_(p + got, n - got);
        if (r == 0) {
            if (cleanEof && got == 0) { *cleanEof = true; return false; }
            return fail("the file is cut off");
        }
        got += r;
    }
    return true;
}

bool Lz4Frame::nextFrame() {
    for (;;) {
        uint8_t m[4];
        bool eof = false;
        if (!readExact(m, 4, &eof)) {
            if (eof && frames_ > 0) { done_ = true; return false; }
            return eof ? fail("empty file") : false;
        }
        uint32_t magic = le32(m);
        if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {  // skippable frame: size, then that many bytes
            uint8_t s[4];
            if (!readExact(s, 4)) return false;
            std::vector<uint8_t> skip(std::min<uint32_t>(le32(s), 1u << 20));
            for (uint32_t left = le32(s); left;) {
                size_t k = std::min<size_t>(left, skip.size());
                if (!readExact(skip.data(), k)) return false;
                left -= uint32_t(k);
            }
            continue;
        }
        if (magic != kMagic) {
            if (frames_ > 0) { done_ = true; return false; }  // trailing bytes after the last frame are ignored
            return fail("not an LZ4 frame");
        }
        uint8_t d[15];
        if (!readExact(d, 2)) return false;
        uint8_t flg = d[0], bd = d[1];
        if ((flg >> 6) != 1) return fail("unsupported frame version");
        if ((flg & 0x02) || (bd & 0x8F)) return fail("damaged frame header");
        if (flg & 0x01) return fail("frames that need a dictionary are not supported");
        int bs = (bd >> 4) & 7;
        if (bs < 4) return fail("damaged frame header (block size)");
        maxBlock_ = size_t(1) << (8 + 2 * bs);  // 4: 64 KiB ... 7: 4 MiB
        independent_ = flg & 0x20;
        blockSum_ = flg & 0x10;
        contentSum_ = flg & 0x04;
        size_t len = 2;
        if (flg & 0x08) {  // content size
            if (!readExact(d + len, 8)) return false;
            len += 8;
        }
        uint8_t hc;
        if (!readExact(&hc, 1)) return false;
        if (uint8_t(Xxh32::of(d, len) >> 8) != hc) return fail("damaged frame header (checksum)");
        content_.reset();
        out_.clear();
        outPos_ = 0;
        inFrame_ = true;
        frames_++;
        return true;
    }
}

bool Lz4Frame::nextBlock() {
    uint8_t s[4];
    if (!readExact(s, 4)) return false;
    uint32_t size = le32(s);
    if (size == 0) {  // end of the frame
        inFrame_ = false;
        if (contentSum_) {
            if (!readExact(s, 4)) return false;
            if (le32(s) != content_.digest()) return fail("damaged data (content checksum mismatch)");
        }
        return true;
    }
    bool raw = size & 0x80000000u;
    size &= 0x7FFFFFFFu;
    if (size > maxBlock_) return fail("damaged block size");
    blk_.resize(size);
    if (!readExact(blk_.data(), size)) return false;
    if (blockSum_) {
        if (!readExact(s, 4)) return false;
        if (le32(s) != Xxh32::of(blk_.data(), size)) return fail("damaged data (block checksum mismatch)");
    }
    // Linked blocks can refer to the last 64 KiB of earlier output: keep it in front of the new block.
    size_t keep = independent_ ? 0 : std::min<size_t>(out_.size(), 64 << 10);
    if (keep) memmove(out_.data(), out_.data() + out_.size() - keep, keep);
    out_.resize(keep);
    if (raw) {
        out_.insert(out_.end(), blk_.begin(), blk_.end());
    } else {
        std::string e;
        if (!lz4Block(blk_.data(), size, out_, maxBlock_, e)) return fail(e);
    }
    outPos_ = keep;
    content_.update(out_.data() + keep, out_.size() - keep);
    return true;
}

ssize_t Lz4Frame::read(void* dst, size_t n) {
    for (;;) {
        if (outPos_ < out_.size()) {
            size_t k = std::min(n, out_.size() - outPos_);
            memcpy(dst, out_.data() + outPos_, k);
            outPos_ += k;
            return ssize_t(k);
        }
        if (failed_) return -1;
        if (done_) return 0;
        if (!inFrame_) {
            if (!nextFrame()) {
                if (failed_) return -1;
                continue;  // done_
            }
        } else if (!nextBlock()) {
            return -1;
        }
    }
}

}  // namespace tw
