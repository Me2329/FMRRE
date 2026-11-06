#pragma once
// A small reader for DER-encoded ASN.1 - enough for certificates, PKCS#7 signatures and ECDSA signature values.
#include <cstddef>
#include <cstdint>
#include <string>

namespace tw::der {

constexpr uint8_t kInteger = 0x02, kBitString = 0x03, kOctetString = 0x04, kNull = 0x05, kOid = 0x06, kUtf8 = 0x0C,
                  kPrintable = 0x13, kT61 = 0x14, kIa5 = 0x16, kUtcTime = 0x17, kGenTime = 0x18, kUniversal = 0x1C,
                  kBmp = 0x1E, kSequence = 0x30, kSet = 0x31;
// Context-specific tag [n]: constructed (EXPLICIT, or IMPLICIT over a SET / SEQUENCE) unless said otherwise.
constexpr uint8_t ctx(int n, bool constructed = true) { return uint8_t(0x80 | (constructed ? 0x20 : 0) | n); }

struct Tlv {
    uint8_t tag = 0;
    const uint8_t* p = nullptr;  // contents
    size_t len = 0;
    const uint8_t* raw = nullptr;  // header + contents
    size_t rawLen = 0;
};

// Elements one after another. Indefinite lengths and high tag numbers are refused (they don't occur in DER
// certificates or signatures); every length is checked against what is left, so a damaged length can't make it
// read outside the buffer.
class Reader {
public:
    Reader() = default;
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
    explicit Reader(const Tlv& t) : p_(t.p), end_(t.p + t.len) {}

    bool next(Tlv& t) {
        if (!p_ || end_ - p_ < 2) return false;
        const uint8_t* s = p_;
        size_t left = size_t(end_ - s);
        if ((s[0] & 0x1F) == 0x1F) return false;
        size_t i = 2, len = s[1];
        if (len & 0x80) {
            size_t nb = len & 0x7F;
            if (nb == 0 || nb > 4 || left < 2 + nb) return false;
            len = 0;
            for (size_t k = 0; k < nb; k++) len = (len << 8) | s[i++];
        }
        if (left - i < len) return false;
        t.tag = s[0];
        t.p = s + i;
        t.len = len;
        t.raw = s;
        t.rawLen = i + len;
        p_ = s + i + len;
        return true;
    }
    // The next element, which must have this tag.
    bool next(Tlv& t, uint8_t tag) { return next(t) && t.tag == tag; }
    bool peekTag(uint8_t& tag) const {
        if (!p_ || p_ >= end_) return false;
        tag = *p_;
        return true;
    }
    bool atEnd() const { return p_ == end_; }

private:
    const uint8_t* p_ = nullptr;
    const uint8_t* end_ = nullptr;
};

// "1.2.840.113549.1.1.11"; empty when it isn't a valid object identifier.
inline std::string oid(const Tlv& t) {
    if (t.tag != kOid || t.len == 0 || (t.p[t.len - 1] & 0x80)) return "";
    std::string out;
    uint64_t v = 0;
    bool first = true;
    for (size_t i = 0; i < t.len; i++) {
        if (v >> 56) return "";
        v = (v << 7) | (t.p[i] & 0x7F);
        if (t.p[i] & 0x80) continue;
        if (first) {
            uint64_t a = v < 40 ? 0 : v < 80 ? 1 : 2;
            out = std::to_string(a) + "." + std::to_string(v - 40 * a);
            first = false;
        } else {
            out += "." + std::to_string(v);
        }
        v = 0;
    }
    return out;
}

// The algorithm OID of an AlgorithmIdentifier SEQUENCE { OID, parameters }.
inline std::string algorithm(const Tlv& seq) {
    if (seq.tag != kSequence) return "";
    Reader r(seq);
    Tlv o;
    return r.next(o, kOid) ? oid(o) : "";
}

}  // namespace tw::der
