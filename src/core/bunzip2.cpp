#include "core/bunzip2.h"

#include <algorithm>
#include <cstring>

namespace tw {

namespace {

constexpr int kMaxGroups = 6, kMaxAlpha = 258, kMaxCodeLen = 20, kGroupSize = 50;
constexpr uint32_t kMaxSelectors = 2 + 900000 / kGroupSize;
constexpr int kRunA = 0, kRunB = 1;

// bzip2 uses the big-endian (non-reflected) CRC-32, polynomial 0x04C11DB7.
struct CrcTable {
    uint32_t t[256];
    CrcTable() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; k++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
            t[i] = c;
        }
    }
};
const CrcTable& crcTable() {
    static CrcTable t;
    return t;
}

struct Huffman {
    int minLen = 0, maxLen = 0;
    int32_t limit[kMaxCodeLen + 2] = {}, base[kMaxCodeLen + 2] = {};
    uint16_t perm[kMaxAlpha] = {};

    // Canonical codes: shorter first, then by symbol (like bzip2's hbCreateDecodeTables).
    void build(const uint8_t* len, int alphaSize) {
        minLen = 32;
        maxLen = 0;
        for (int i = 0; i < alphaSize; i++) {
            minLen = std::min<int>(minLen, len[i]);
            maxLen = std::max<int>(maxLen, len[i]);
        }
        int pp = 0;
        for (int l = minLen; l <= maxLen; l++)
            for (int s = 0; s < alphaSize; s++)
                if (len[s] == l) perm[pp++] = (uint16_t)s;
        int32_t count[kMaxCodeLen + 2] = {};
        for (int s = 0; s < alphaSize; s++) count[len[s]]++;
        // base[l] = first index in perm[] of the codes of length l; limit[l] = largest code of length l
        int32_t code = 0, idx = 0;
        for (int l = 0; l <= kMaxCodeLen + 1; l++) { limit[l] = -1; base[l] = 0; }
        for (int l = minLen; l <= maxLen; l++) {
            base[l] = idx - code;  // perm index = code - (first code of this length) + first index
            code += count[l];
            idx += count[l];
            limit[l] = code - 1;
            code <<= 1;
        }
    }
};

}  // namespace

Bunzip2::Bunzip2(Reader in) : in_(std::move(in)), inBuf_(64 * 1024) {}

bool Bunzip2::fail(const char* what) {
    if (state_ != State::Error) err_ = std::string("bzip2: ") + what;
    state_ = State::Error;
    return false;
}

bool Bunzip2::fill() {
    if (inEof_) return false;
    ssize_t r = in_(inBuf_.data(), inBuf_.size());
    if (r < 0) { inEof_ = true; return fail("read error"); }
    if (r == 0) { inEof_ = true; return false; }
    inPos_ = 0;
    inLen_ = (size_t)r;
    return true;
}

bool Bunzip2::need(int n) {
    while (bitCount_ < n) {
        if (inPos_ >= inLen_ && !fill()) return false;
        bitBuf_ = (bitBuf_ << 8) | inBuf_[inPos_++];
        bitCount_ += 8;
    }
    return true;
}

uint32_t Bunzip2::take(int n) {
    bitCount_ -= n;
    return uint32_t(bitBuf_ >> bitCount_) & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1));
}

bool Bunzip2::bits(int n, uint32_t& v) {
    if (!need(n)) return state_ == State::Error ? false : fail("unexpected end of data");
    v = take(n);
    return true;
}

bool Bunzip2::readStreamHeader(bool first) {
    // At a stream boundary the bit reader is byte aligned.
    bitCount_ -= bitCount_ % 8;
    uint32_t b = 0, z = 0, h = 0, level = 0;
    if (!first) {
        // Another stream may follow (concatenated .bz2). Anything else (padding) ends the data.
        if (!need(8)) { if (state_ == State::Error) return false; state_ = State::End; return true; }
        b = take(8);
        if (b != 'B' || !need(24)) { state_ = State::End; return true; }
        z = take(8); h = take(8); level = take(8);
        if (z != 'Z' || h != 'h' || level < '1' || level > '9') { state_ = State::End; return true; }
    } else {
        if (!bits(8, b) || !bits(8, z) || !bits(8, h) || !bits(8, level)) return false;
        if (b != 'B' || z != 'Z' || h != 'h') return fail("not a bzip2 stream");
        if (level < '1' || level > '9') return fail("bad block size");
    }
    blockMax_ = (level - '0') * 100000;
    streamCrc_ = 0;
    state_ = State::BlockStart;
    return true;
}

bool Bunzip2::readBlock() {
    uint32_t m1, m2;
    if (!bits(24, m1) || !bits(24, m2)) return false;
    if (m1 == 0x177245 && m2 == 0x385090) {  // end of stream
        uint32_t crc;
        if (!bits(32, crc)) return false;
        if (crc != streamCrc_) return fail("stream CRC mismatch");
        return readStreamHeader(false);
    }
    if (m1 != 0x314159 || m2 != 0x265359) return fail("bad block header");
    uint32_t v;
    if (!bits(32, blockCrc_)) return false;
    if (!bits(1, v)) return false;
    if (v) return fail("randomised blocks are not supported");
    uint32_t origPtr;
    if (!bits(24, origPtr)) return false;

    // Symbol map: which byte values occur in this block.
    uint8_t seqToUnseq[256];
    int nInUse = 0;
    uint32_t used16;
    if (!bits(16, used16)) return false;
    for (int i = 0; i < 16; i++)
        if (used16 & (0x8000u >> i)) {
            uint32_t u;
            if (!bits(16, u)) return false;
            for (int j = 0; j < 16; j++)
                if (u & (0x8000u >> j)) seqToUnseq[nInUse++] = uint8_t(i * 16 + j);
        }
    if (nInUse == 0) return fail("empty symbol map");
    int alphaSize = nInUse + 2;

    uint32_t nGroups, nSelectors;
    if (!bits(3, nGroups) || !bits(15, nSelectors)) return false;
    if (nGroups < 2 || nGroups > (uint32_t)kMaxGroups || nSelectors < 1) return fail("bad Huffman group header");
    std::vector<uint8_t> selector(std::min(nSelectors, kMaxSelectors));
    uint8_t pos[kMaxGroups];
    for (uint32_t g = 0; g < nGroups; g++) pos[g] = (uint8_t)g;
    for (uint32_t i = 0; i < nSelectors; i++) {
        uint32_t j = 0;
        for (;;) {
            if (!bits(1, v)) return false;
            if (!v) break;
            if (++j >= nGroups) return fail("bad selector");
        }
        if (i >= kMaxSelectors) continue;  // bzip2 1.0.8 ignores the excess too
        uint8_t t = pos[j];
        memmove(pos + 1, pos, j);
        pos[0] = t;
        selector[i] = t;
    }
    nSelectors = (uint32_t)selector.size();

    Huffman huff[kMaxGroups];
    for (uint32_t g = 0; g < nGroups; g++) {
        uint8_t len[kMaxAlpha];
        uint32_t cur;
        if (!bits(5, cur)) return false;
        for (int s = 0; s < alphaSize; s++) {
            for (;;) {
                if (cur < 1 || cur > (uint32_t)kMaxCodeLen) return fail("bad code length");
                if (!bits(1, v)) return false;
                if (!v) break;
                if (!bits(1, v)) return false;
                cur += v ? -1 : 1;
            }
            len[s] = (uint8_t)cur;
        }
        huff[g].build(len, alphaSize);
    }

    // Huffman + run-length (RUNA/RUNB) + move-to-front decode into tt_.
    if (tt_.size() < blockMax_) tt_.resize(blockMax_);
    uint32_t counts[256] = {};
    uint8_t mtf[256];
    for (int i = 0; i < 256; i++) mtf[i] = (uint8_t)i;
    uint32_t n = 0, groupIdx = 0, groupLeft = 0;
    const Huffman* hf = nullptr;
    uint32_t runLen = 0, runBit = 1;
    const int eob = nInUse + 1;
    for (;;) {
        if (groupLeft == 0) {
            if (groupIdx >= nSelectors) return fail("ran out of selectors");
            hf = &huff[selector[groupIdx++]];
            groupLeft = kGroupSize;
        }
        groupLeft--;
        int len = hf->minLen;
        if (!need(len)) return state_ == State::Error ? false : fail("unexpected end of data");
        int32_t code = (int32_t)take(len);
        while (code > hf->limit[len]) {
            if (++len > hf->maxLen) return fail("bad Huffman code");
            if (!bits(1, v)) return false;
            code = (code << 1) | (int32_t)v;
        }
        int idx = hf->base[len] + code;
        if (idx < 0 || idx >= alphaSize) return fail("bad Huffman code");
        int sym = hf->perm[idx];

        if (sym == kRunA || sym == kRunB) {
            if (runBit > (1u << 21)) return fail("run too long");
            runLen += (sym == kRunA ? 1u : 2u) * runBit;
            runBit <<= 1;
            continue;
        }
        if (runLen) {  // flush the pending run of the front symbol
            uint8_t b = seqToUnseq[mtf[0]];
            if (runLen > blockMax_ - n) return fail("block too long");
            counts[b] += runLen;
            for (uint32_t k = 0; k < runLen; k++) tt_[n++] = b;
            runLen = 0;
            runBit = 1;
        }
        if (sym == eob) break;
        int m = sym - 1;  // move-to-front index
        if (n >= blockMax_) return fail("block too long");
        uint8_t s = mtf[m];
        memmove(mtf + 1, mtf, (size_t)m);
        mtf[0] = s;
        uint8_t b = seqToUnseq[s];
        counts[b]++;
        tt_[n++] = b;
    }
    if (origPtr >= n) return fail("bad BWT origin");

    // Inverse Burrows-Wheeler transform: link every position to its successor.
    uint32_t cum[256], sum = 0;
    for (int i = 0; i < 256; i++) { cum[i] = sum; sum += counts[i]; }
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = uint8_t(tt_[i] & 0xff);
        tt_[cum[b]++] |= i << 8;
    }
    tPos_ = tt_[origPtr] >> 8;
    left_ = n;
    crc_ = 0xFFFFFFFFu;
    last_ = -1;
    run_ = 0;
    repeat_ = 0;
    state_ = State::Output;
    return true;
}

ssize_t Bunzip2::read(uint8_t* out, size_t n) {
    const uint32_t* crcT = crcTable().t;
    size_t got = 0;
    while (got < n) {
        switch (state_) {
            case State::Error: return -1;
            case State::End: return (ssize_t)got;
            case State::Header:
                if (!readStreamHeader(true)) return -1;
                continue;
            case State::BlockStart:
                if (!readBlock()) return -1;
                continue;
            case State::Output: break;
        }
        uint32_t crc = crc_;
        while (got < n) {
            if (repeat_) {  // pending repeats of the last byte (initial RLE stage)
                uint8_t b = (uint8_t)last_;
                out[got++] = b;
                crc = (crc << 8) ^ crcT[(crc >> 24) ^ b];
                repeat_--;
                continue;
            }
            if (!left_) break;
            uint32_t e = tt_[tPos_];
            uint8_t b = uint8_t(e & 0xff);
            tPos_ = e >> 8;
            left_--;
            if (run_ == 4) {  // this byte is the count of extra copies
                repeat_ = b;
                run_ = 0;
                continue;
            }
            if ((int)b == last_) run_++;
            else { last_ = b; run_ = 1; }
            out[got++] = b;
            crc = (crc << 8) ^ crcT[(crc >> 24) ^ b];
        }
        crc_ = crc;
        if (!left_ && !repeat_) {
            uint32_t final = ~crc_;
            if (final != blockCrc_) { fail("block CRC mismatch"); return -1; }
            streamCrc_ = ((streamCrc_ << 1) | (streamCrc_ >> 31)) ^ final;
            state_ = State::BlockStart;
        }
    }
    return (ssize_t)got;
}

bool Bunzip2::decode(const uint8_t* in, size_t n, std::string& out, std::string& err, size_t maxOut) {
    size_t pos = 0;
    Bunzip2 d([&](uint8_t* buf, size_t cap) -> ssize_t {
        size_t k = std::min(cap, n - pos);
        memcpy(buf, in + pos, k);
        pos += k;
        return (ssize_t)k;
    });
    out.clear();
    uint8_t buf[65536];
    for (;;) {
        ssize_t r = d.read(buf, sizeof(buf));
        if (r < 0) { err = d.error(); return false; }
        if (r == 0) break;
        if (out.size() + (size_t)r > maxOut) { err = "bzip2: output too large"; return false; }
        out.append((const char*)buf, (size_t)r);
    }
    return true;
}

}  // namespace tw
