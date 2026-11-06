#include "core/pubkey.h"

#include <cstring>

#include "core/der.h"

namespace tw::pk {

namespace {

using Limbs = std::vector<uint32_t>;  // little-endian 32-bit words

// Big-endian bytes -> `words` limbs; false when the number doesn't fit.
bool fromBytes(const uint8_t* p, size_t n, size_t words, Limbs& out) {
    out.assign(words, 0);
    for (size_t i = 0; i < n; i++) {
        uint8_t b = p[n - 1 - i];  // the i-th byte from the least significant end
        if (!b) continue;
        if (i / 4 >= words) return false;
        out[i / 4] |= uint32_t(b) << (8 * (i % 4));
    }
    return true;
}

std::vector<uint8_t> toBytes(const Limbs& a, size_t n) {
    std::vector<uint8_t> out(n, 0);
    for (size_t i = 0; i < n && i / 4 < a.size(); i++) out[n - 1 - i] = uint8_t(a[i / 4] >> (8 * (i % 4)));
    return out;
}

std::vector<uint8_t> strip(const std::vector<uint8_t>& v) {
    size_t i = 0;
    while (i < v.size() && v[i] == 0) i++;
    return std::vector<uint8_t>(v.begin() + i, v.end());
}

bool isZero(const Limbs& a) {
    for (uint32_t w : a)
        if (w) return false;
    return true;
}

int cmp(const Limbs& a, const Limbs& b) {
    for (size_t i = a.size(); i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

uint32_t addTo(Limbs& a, const Limbs& b) {
    uint64_t c = 0;
    for (size_t i = 0; i < a.size(); i++) {
        c += uint64_t(a[i]) + b[i];
        a[i] = uint32_t(c);
        c >>= 32;
    }
    return uint32_t(c);
}

uint32_t subFrom(Limbs& a, const Limbs& b) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < a.size(); i++) {
        uint64_t d = uint64_t(a[i]) - b[i] - borrow;
        a[i] = uint32_t(d);
        borrow = d >> 63;
    }
    return uint32_t(borrow);
}

bool bit(const Limbs& a, size_t i) { return (a[i / 32] >> (i % 32)) & 1; }

// Arithmetic modulo an odd number in Montgomery form: x is kept as x*R mod m, R = 2^(32*words).
class Mont {
public:
    explicit Mont(const Limbs& m) : m_(m), k_(m.size()) {
        uint32_t inv = m_[0];  // m^-1 mod 2^32 by Newton's iteration (already right to 3 bits, m being odd)
        for (int i = 0; i < 5; i++) inv *= 2 - m_[0] * inv;
        m0inv_ = 0u - inv;
        Limbs r(k_, 0);
        r[0] = 1;
        for (size_t i = 0; i < 64 * k_; i++) {  // R^2 mod m: double 1 that many times
            uint32_t carry = addTo(r, r);
            if (carry || cmp(r, m_) >= 0) subFrom(r, m_);
        }
        rr_ = r;
        one_ = mul(unit(), rr_);
    }
    const Limbs& mod() const { return m_; }
    Limbs unit() const {
        Limbs u(k_, 0);
        u[0] = 1;
        return u;
    }
    const Limbs& one() const { return one_; }  // 1 in Montgomery form

    // a*b/R mod m (CIOS); a, b < m.
    Limbs mul(const Limbs& a, const Limbs& b) const {
        std::vector<uint32_t> t(k_ + 2, 0);
        for (size_t i = 0; i < k_; i++) {
            uint64_t c = 0, bi = b[i];
            for (size_t j = 0; j < k_; j++) {
                uint64_t s = t[j] + uint64_t(a[j]) * bi + c;
                t[j] = uint32_t(s);
                c = s >> 32;
            }
            uint64_t s = uint64_t(t[k_]) + c;
            t[k_] = uint32_t(s);
            t[k_ + 1] = uint32_t(s >> 32);
            uint32_t u = t[0] * m0inv_;
            c = (uint64_t(t[0]) + uint64_t(u) * m_[0]) >> 32;
            for (size_t j = 1; j < k_; j++) {
                uint64_t s2 = t[j] + uint64_t(u) * m_[j] + c;
                t[j - 1] = uint32_t(s2);
                c = s2 >> 32;
            }
            s = uint64_t(t[k_]) + c;
            t[k_ - 1] = uint32_t(s);
            t[k_] = t[k_ + 1] + uint32_t(s >> 32);
        }
        Limbs r(t.begin(), t.begin() + k_);
        if (t[k_] || cmp(r, m_) >= 0) subFrom(r, m_);
        return r;
    }
    Limbs sqr(const Limbs& a) const { return mul(a, a); }
    Limbs to(const Limbs& a) const { return mul(a, rr_); }  // a < m
    Limbs from(const Limbs& a) const { return mul(a, unit()); }
    Limbs add(Limbs a, const Limbs& b) const {
        if (addTo(a, b) || cmp(a, m_) >= 0) subFrom(a, m_);
        return a;
    }
    Limbs sub(Limbs a, const Limbs& b) const {
        if (subFrom(a, b)) addTo(a, m_);
        return a;
    }
    // base (Montgomery form) ^ e (a plain number), in Montgomery form.
    Limbs pow(const Limbs& base, const Limbs& e) const {
        Limbs r = one_;
        bool started = false;
        for (size_t i = e.size() * 32; i-- > 0;) {
            if (started) r = sqr(r);
            if (bit(e, i)) {
                r = started ? mul(r, base) : base;
                started = true;
            }
        }
        return r;
    }
    // a^-1 for a prime modulus (Fermat: a^(m-2)); Montgomery form in and out.
    Limbs inv(const Limbs& a) const {
        Limbs e = m_, two(k_, 0);
        two[0] = 2;
        subFrom(e, two);
        return pow(a, e);
    }

private:
    Limbs m_;
    size_t k_;
    uint32_t m0inv_ = 0;
    Limbs rr_, one_;
};

// ---- NIST P-256 (secp256r1) -------------------------------------------------------------------------

Limbs hex256(const char* hex) {
    uint8_t b[32];
    auto v = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (int i = 0; i < 32; i++) b[i] = uint8_t(v(hex[2 * i]) << 4 | v(hex[2 * i + 1]));
    Limbs l;
    fromBytes(b, 32, 8, l);
    return l;
}

struct Curve {
    Mont fp{hex256("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff")};
    Mont fn{hex256("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551")};
    Limbs b = fp.to(hex256("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b"));
    Limbs gx = fp.to(hex256("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"));
    Limbs gy = fp.to(hex256("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"));
};

// Built once and never destroyed: checks run on a background thread that may still be busy at exit.
const Curve& curve() {
    static const Curve* c = new Curve;
    return *c;
}

struct Pt {  // Jacobian coordinates in Montgomery form; z == 0 is the point at infinity
    Limbs x, y, z;
};

Pt infinity(const Curve& C) { return Pt{C.fp.one(), C.fp.one(), Limbs(8, 0)}; }

Pt dbl(const Curve& C, const Pt& P) {  // dbl-2001-b (a = -3)
    const Mont& f = C.fp;
    if (isZero(P.z)) return P;
    Limbs delta = f.sqr(P.z), gamma = f.sqr(P.y), beta = f.mul(P.x, gamma);
    Limbs alpha = f.mul(f.sub(P.x, delta), f.add(P.x, delta));
    alpha = f.add(f.add(alpha, alpha), alpha);
    Limbs beta4 = f.add(beta, beta);
    beta4 = f.add(beta4, beta4);
    Pt R;
    R.x = f.sub(f.sqr(alpha), f.add(beta4, beta4));
    R.z = f.sub(f.sub(f.sqr(f.add(P.y, P.z)), gamma), delta);
    Limbs g8 = f.sqr(gamma);
    g8 = f.add(g8, g8);
    g8 = f.add(g8, g8);
    g8 = f.add(g8, g8);
    R.y = f.sub(f.mul(alpha, f.sub(beta4, R.x)), g8);
    return R;
}

Pt add(const Curve& C, const Pt& P, const Pt& Q) {  // add-2007-bl
    const Mont& f = C.fp;
    if (isZero(P.z)) return Q;
    if (isZero(Q.z)) return P;
    Limbs z1z1 = f.sqr(P.z), z2z2 = f.sqr(Q.z);
    Limbs u1 = f.mul(P.x, z2z2), u2 = f.mul(Q.x, z1z1);
    Limbs s1 = f.mul(f.mul(P.y, Q.z), z2z2), s2 = f.mul(f.mul(Q.y, P.z), z1z1);
    Limbs h = f.sub(u2, u1), r = f.sub(s2, s1);
    if (isZero(h)) return isZero(r) ? dbl(C, P) : infinity(C);  // P == Q, or P == -Q
    r = f.add(r, r);
    Limbs i = f.add(h, h);
    i = f.sqr(i);
    Limbs j = f.mul(h, i), v = f.mul(u1, i);
    Pt R;
    R.x = f.sub(f.sub(f.sqr(r), j), f.add(v, v));
    Limbs s1j = f.mul(s1, j);
    R.y = f.sub(f.mul(r, f.sub(v, R.x)), f.add(s1j, s1j));
    R.z = f.mul(f.sub(f.sub(f.sqr(f.add(P.z, Q.z)), z1z1), z2z2), h);
    return R;
}

Pt mulPt(const Curve& C, const Pt& P, const Limbs& k) {  // k: a plain 256-bit number
    Pt R = infinity(C);
    for (size_t i = 256; i-- > 0;) {
        R = dbl(C, R);
        if (bit(k, i)) R = add(C, R, P);
    }
    return R;
}

bool affineX(const Curve& C, const Pt& P, Limbs& x) {
    if (isZero(P.z)) return false;
    Limbs zi = C.fp.inv(P.z);
    x = C.fp.from(C.fp.mul(P.x, C.fp.sqr(zi)));
    return true;
}

bool onCurve(const Curve& C, const Limbs& x, const Limbs& y) {  // y^2 = x^3 - 3x + b, Montgomery form
    const Mont& f = C.fp;
    Limbs rhs = f.add(f.sub(f.mul(f.sqr(x), x), f.add(f.add(x, x), x)), C.b);
    return cmp(f.sqr(y), rhs) == 0;
}

bool loadPoint(const Curve& C, const EcKey& k, Limbs& xm, Limbs& ym) {
    if (k.x.size() != 32 || k.y.size() != 32) return false;
    Limbs x, y;
    fromBytes(k.x.data(), 32, 8, x);
    fromBytes(k.y.data(), 32, 8, y);
    if (cmp(x, C.fp.mod()) >= 0 || cmp(y, C.fp.mod()) >= 0) return false;
    xm = C.fp.to(x);
    ym = C.fp.to(y);
    return onCurve(C, xm, ym);
}

bool readPositive(der::Reader& r, Limbs& out) {
    der::Tlv t;
    if (!r.next(t, der::kInteger) || t.len == 0 || (t.p[0] & 0x80)) return false;
    return fromBytes(t.p, t.len, 8, out);
}

}  // namespace

int RsaKey::bits() const {
    std::vector<uint8_t> m = strip(n);
    if (m.empty()) return 0;
    int b = int(m.size()) * 8;
    for (uint8_t top = m[0]; !(top & 0x80); top = uint8_t(top << 1)) b--;
    return b;
}

bool rsaVerify(const RsaKey& k, Hash h, const std::string& digest, const uint8_t* sig, size_t sigLen) {
    static const uint8_t kSha1[] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14};
    static const uint8_t kSha256[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                      0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
    const uint8_t* prefix = h == Hash::Sha1 ? kSha1 : kSha256;
    size_t prefixLen = h == Hash::Sha1 ? sizeof(kSha1) : sizeof(kSha256);
    if (digest.size() != (h == Hash::Sha1 ? 20u : 32u)) return false;
    std::vector<uint8_t> n = strip(k.n), e = strip(k.e);
    size_t len = n.size();
    if (len < 64 || len > 1024 || !(n.back() & 1)) return false;            // 512 to 8192 bits, odd
    if (e.empty() || e.size() > 8 || !(e.back() & 1) || (e.size() == 1 && e[0] == 1)) return false;
    if (sigLen != len) return false;
    size_t words = (len + 3) / 4;
    Limbs N, S, E;
    if (!fromBytes(n.data(), len, words, N) || !fromBytes(sig, sigLen, words, S) || !fromBytes(e.data(), e.size(), 2, E))
        return false;
    if (cmp(S, N) >= 0) return false;
    Mont m(N);
    std::vector<uint8_t> em = toBytes(m.from(m.pow(m.to(S), E)), len);
    size_t tLen = prefixLen + digest.size();
    if (len < tLen + 11) return false;
    std::vector<uint8_t> want(len, 0xFF);
    want[0] = 0x00;
    want[1] = 0x01;
    want[len - tLen - 1] = 0x00;
    memcpy(want.data() + len - tLen, prefix, prefixLen);
    memcpy(want.data() + len - digest.size(), digest.data(), digest.size());
    return em == want;
}

bool ecOnCurve(const EcKey& k) {
    Limbs x, y;
    return loadPoint(curve(), k, x, y);
}

bool ecdsaVerify(const EcKey& k, const std::string& digest, const uint8_t* sigDer, size_t sigLen) {
    const Curve& C = curve();
    if (digest.size() != 32) return false;
    der::Reader top(sigDer, sigLen);
    der::Tlv seq;
    if (!top.next(seq, der::kSequence) || !top.atEnd()) return false;
    der::Reader rs(seq);
    Limbs r, s;
    if (!readPositive(rs, r) || !readPositive(rs, s) || !rs.atEnd()) return false;
    const Limbs& n = C.fn.mod();
    if (isZero(r) || isZero(s) || cmp(r, n) >= 0 || cmp(s, n) >= 0) return false;
    Limbs qx, qy;
    if (!loadPoint(C, k, qx, qy)) return false;
    Limbs e;
    fromBytes((const uint8_t*)digest.data(), 32, 8, e);
    if (cmp(e, n) >= 0) subFrom(e, n);
    const Mont& fn = C.fn;
    Limbs w = fn.inv(fn.to(s));
    Limbs u1 = fn.from(fn.mul(fn.to(e), w)), u2 = fn.from(fn.mul(fn.to(r), w));
    Pt G{C.gx, C.gy, C.fp.one()}, Q{qx, qy, C.fp.one()};
    Limbs x;
    if (!affineX(C, add(C, mulPt(C, G, u1), mulPt(C, Q, u2)), x)) return false;
    if (cmp(x, n) >= 0) subFrom(x, n);
    return cmp(x, r) == 0;
}

bool p256SelfTest() {
    const Curve& C = curve();
    if (!onCurve(C, C.gx, C.gy)) return false;
    // (n-1)*G is -G - the same x - and adding G gives the point at infinity: n is G's order.
    Limbs nm1 = C.fn.mod(), one(8, 0);
    one[0] = 1;
    subFrom(nm1, one);
    Pt G{C.gx, C.gy, C.fp.one()};
    Pt P = mulPt(C, G, nm1);
    Limbs x;
    if (!affineX(C, P, x) || cmp(x, C.fp.from(C.gx)) != 0) return false;
    return isZero(add(C, P, G).z);
}

}  // namespace tw::pk
