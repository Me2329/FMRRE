#include "core/crc32.h"

namespace tw::crc32 {

namespace {

constexpr uint32_t kPoly = 0xEDB88320u;  // reflected

struct Tables {
    uint32_t t[8][256];  // t[k][b]: the CRC of byte b followed by k zero bytes
    uint32_t x2n[32];    // x^(2^k) mod the polynomial
    Tables() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ kPoly : c >> 1;
            t[0][i] = c;
        }
        for (int i = 0; i < 256; i++)
            for (int k = 1; k < 8; k++) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFF];
        uint32_t p = 1u << 30;  // x^1 (bit 31 is x^0)
        x2n[0] = p;
        for (int k = 1; k < 32; k++) x2n[k] = p = mul(p, p);
    }
    // a * b modulo the polynomial; a must not be 0.
    static uint32_t mul(uint32_t a, uint32_t b) {
        uint32_t m = 1u << 31, p = 0;
        for (;;) {
            if (a & m) {
                p ^= b;
                if ((a & (m - 1)) == 0) break;
            }
            m >>= 1;
            b = b & 1 ? (b >> 1) ^ kPoly : b >> 1;
        }
        return p;
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

}  // namespace

uint32_t update(uint32_t crc, const void* data, size_t n) {
    const Tables& T = tables();
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = ~crc;
    for (; n >= 8; p += 8, n -= 8) {
        uint32_t a = le32(p) ^ c, b = le32(p + 4);
        c = T.t[7][a & 0xFF] ^ T.t[6][(a >> 8) & 0xFF] ^ T.t[5][(a >> 16) & 0xFF] ^ T.t[4][a >> 24] ^ T.t[3][b & 0xFF] ^
            T.t[2][(b >> 8) & 0xFF] ^ T.t[1][(b >> 16) & 0xFF] ^ T.t[0][b >> 24];
    }
    while (n--) c = T.t[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

uint32_t combine(uint32_t crcA, uint32_t crcB, uint64_t lenB) {
    // crcA * x^(8 lenB) + crcB (zlib's crc32_combine)
    const Tables& T = tables();
    uint32_t p = 1u << 31;  // x^0
    unsigned k = 3;         // x^8 per byte
    for (uint64_t n = lenB; n; n >>= 1, k++)
        if (n & 1) p = Tables::mul(T.x2n[k & 31], p);
    return Tables::mul(p, crcA) ^ crcB;
}

}  // namespace tw::crc32
