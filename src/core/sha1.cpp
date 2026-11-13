#include "core/sha1.h"

#include <algorithm>
#include <cstring>

namespace tw {

namespace {
inline uint32_t rol(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
}  // namespace

void Sha1::reset() {
    h_[0] = 0x67452301;
    h_[1] = 0xEFCDAB89;
    h_[2] = 0x98BADCFE;
    h_[3] = 0x10325476;
    h_[4] = 0xC3D2E1F0;
    bufLen_ = 0;
    total_ = 0;
}

void Sha1::block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else f = b ^ c ^ d, k = 0xCA62C1D6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::update(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    total_ += len;
    if (bufLen_) {
        size_t take = std::min(len, sizeof(buf_) - bufLen_);
        memcpy(buf_ + bufLen_, p, take);
        bufLen_ += take;
        p += take;
        len -= take;
        if (bufLen_ < 64) return;
        block(buf_);
        bufLen_ = 0;
    }
    for (; len >= 64; p += 64, len -= 64) block(p);
    memcpy(buf_, p, len);
    bufLen_ = len;
}

std::string Sha1::digest() {
    uint64_t bits = total_ * 8;
    uint8_t pad[72] = {0x80};
    size_t padLen = (bufLen_ < 56 ? 56 : 120) - bufLen_;
    for (int i = 0; i < 8; i++) pad[padLen + i] = uint8_t(bits >> (56 - 8 * i));
    update(pad, padLen + 8);
    std::string out(20, '\0');
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = char(h_[i] >> (24 - 8 * j));
    return out;
}

}  // namespace tw
