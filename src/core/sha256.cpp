#include "core/sha256.h"

#include <algorithm>
#include <cstring>
#if defined(__aarch64__) && defined(__linux__)
#include <arm_neon.h>
#include <sys/auxv.h>
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <immintrin.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace tw {

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

namespace {

void blocksSoft(uint32_t* hs, const uint8_t* p, size_t n) {
    for (; n--; p += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = hs[0], b = hs[1], c = hs[2], d = hs[3], e = hs[4], f = hs[5], g = hs[6], h = hs[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + S1 + ch + K[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        hs[0] += a; hs[1] += b; hs[2] += c; hs[3] += d; hs[4] += e; hs[5] += f; hs[6] += g; hs[7] += h;
    }
}

#if defined(__aarch64__) && defined(__linux__)
// ARMv8 Cryptography Extensions (almost every 64-bit phone CPU): 4 rounds per instruction pair.
__attribute__((target("sha2"))) void blocksArm(uint32_t* hs, const uint8_t* p, size_t n) {
    uint32x4_t s0 = vld1q_u32(hs), s1 = vld1q_u32(hs + 4);
    for (; n--; p += 64) {
        const uint32x4_t a0 = s0, a1 = s1;
        uint32x4_t m[4];
        for (int i = 0; i < 4; i++) m[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16 * i)));
        for (int g = 0; g < 16; g++) {
            uint32x4_t wk = vaddq_u32(m[g & 3], vld1q_u32(K + 4 * g)), t = s0;
            s0 = vsha256hq_u32(s0, s1, wk);
            s1 = vsha256h2q_u32(s1, t, wk);
            if (g < 12) m[g & 3] = vsha256su1q_u32(vsha256su0q_u32(m[g & 3], m[(g + 1) & 3]), m[(g + 2) & 3], m[(g + 3) & 3]);
        }
        s0 = vaddq_u32(s0, a0);
        s1 = vaddq_u32(s1, a1);
    }
    vst1q_u32(hs, s0);
    vst1q_u32(hs + 4, s1);
}
bool haveArm() { return (getauxval(AT_HWCAP) & (1UL << 6)) != 0; }  // HWCAP_SHA2
#endif

#if defined(__x86_64__) || defined(__i386__)
// Intel SHA extensions (most x86 CPUs since ~2017, so emulators and Chromebooks too).
__attribute__((target("sha,sse4.1"))) void blocksX86(uint32_t* hs, const uint8_t* p, size_t n) {
    const __m128i mask = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    __m128i t = _mm_loadu_si128((const __m128i*)hs), s1 = _mm_loadu_si128((const __m128i*)(hs + 4));
    t = _mm_shuffle_epi32(t, 0xB1);              // CDAB
    s1 = _mm_shuffle_epi32(s1, 0x1B);            // EFGH
    __m128i s0 = _mm_alignr_epi8(t, s1, 8);      // ABEF
    s1 = _mm_blend_epi16(s1, t, 0xF0);           // CDGH
    for (; n--; p += 64) {
        const __m128i a0 = s0, a1 = s1;
        __m128i m[4];
        for (int i = 0; i < 4; i++) m[i] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(p + 16 * i)), mask);
        for (int g = 0; g < 16; g++) {
            __m128i wk = _mm_add_epi32(m[g & 3], _mm_loadu_si128((const __m128i*)(K + 4 * g)));
            s1 = _mm_sha256rnds2_epu32(s1, s0, wk);
            s0 = _mm_sha256rnds2_epu32(s0, s1, _mm_shuffle_epi32(wk, 0x0E));
            if (g < 12) {
                __m128i x = _mm_sha256msg1_epu32(m[g & 3], m[(g + 1) & 3]);
                x = _mm_add_epi32(x, _mm_alignr_epi8(m[(g + 3) & 3], m[(g + 2) & 3], 4));
                m[g & 3] = _mm_sha256msg2_epu32(x, m[(g + 3) & 3]);
            }
        }
        s0 = _mm_add_epi32(s0, a0);
        s1 = _mm_add_epi32(s1, a1);
    }
    t = _mm_shuffle_epi32(s0, 0x1B);             // FEBA
    s1 = _mm_shuffle_epi32(s1, 0xB1);            // DCHG
    s0 = _mm_blend_epi16(t, s1, 0xF0);           // DCBA
    s1 = _mm_alignr_epi8(s1, t, 8);              // HGFE
    _mm_storeu_si128((__m128i*)hs, s0);
    _mm_storeu_si128((__m128i*)(hs + 4), s1);
}
bool haveX86() {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 19)) || !(c & (1u << 9))) return false;  // SSE4.1, SSSE3
    return __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & (1u << 29));                          // SHA
}
#endif

using BlocksFn = void (*)(uint32_t*, const uint8_t*, size_t);
struct Backend { BlocksFn fn; const char* name; };
Backend pick() {
#if defined(__aarch64__) && defined(__linux__)
    if (haveArm()) return {blocksArm, "ARMv8 SHA2 instructions"};
#endif
#if defined(__x86_64__) || defined(__i386__)
    if (haveX86()) return {blocksX86, "Intel SHA extensions"};
#endif
    return {blocksSoft, "portable C"};
}
Backend& backend() {
    static Backend b = pick();
    return b;
}

}  // namespace

const char* Sha256::backend() { return ::tw::backend().name; }
void Sha256::forceSoftware(bool on) { ::tw::backend() = on ? Backend{blocksSoft, "portable C"} : pick(); }

void Sha256::reset() {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h_, init, sizeof(h_));
    bufLen_ = 0;
    total_ = 0;
}

void Sha256::block(const uint8_t* p) { ::tw::backend().fn(h_, p, 1); }

void Sha256::update(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    total_ += len;
    if (bufLen_) {
        size_t n = std::min(len, 64 - bufLen_);
        memcpy(buf_ + bufLen_, p, n);
        bufLen_ += n; p += n; len -= n;
        if (bufLen_ == 64) { block(buf_); bufLen_ = 0; }
    }
    if (len >= 64) {  // whole blocks straight from the input, in one call
        size_t n = len / 64;
        ::tw::backend().fn(h_, p, n);
        p += n * 64;
        len -= n * 64;
    }
    if (len) { memcpy(buf_, p, len); bufLen_ = len; }
}

std::string Sha256::digest() {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t z = 0;
    while (bufLen_ != 56) update(&z, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = uint8_t(bits >> (56 - 8 * i));
    update(lenb, 8);
    std::string out(32, '\0');
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = char(uint8_t(h_[i] >> (24 - j * 8)));
    reset();
    return out;
}

std::string Sha256::hexDigest() { return toHex(digest()); }

std::string Sha256::toHex(const std::string& bin) {
    static const char* hx = "0123456789abcdef";
    std::string out;
    out.reserve(bin.size() * 2);
    for (unsigned char v : bin) {
        out += hx[v >> 4];
        out += hx[v & 15];
    }
    return out;
}

std::string Sha256::hexOf(const std::string& s) {
    Sha256 h;
    h.update(s.data(), s.size());
    return h.hexDigest();
}

std::string Sha256::hexOfFile(const std::string& path, void (*progress)(uint64_t, uint64_t, void*), void* ctx) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    struct stat st;
    fstat(fd, &st);
    Sha256 h;
    std::vector<uint8_t> buf(1 << 20);
    uint64_t done = 0;
    for (;;) {
        ssize_t n = read(fd, buf.data(), buf.size());
        if (n <= 0) break;
        h.update(buf.data(), n);
        done += n;
        if (progress) progress(done, st.st_size, ctx);
    }
    close(fd);
    return h.hexDigest();
}

}  // namespace tw
