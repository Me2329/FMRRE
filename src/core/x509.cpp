#include "core/x509.h"

#include "core/der.h"
#include "core/sha256.h"
#include "miniz.h"

namespace tw::x509 {

const char* const kSha1 = "1.3.14.3.2.26";
const char* const kSha256 = "2.16.840.1.101.3.4.2.1";

namespace {

const char* const kRsaEncryption = "1.2.840.113549.1.1.1";
const char* const kEcPublicKey = "1.2.840.10045.2.1";
const char* const kPrime256v1 = "1.2.840.10045.3.1.7";
const char* const kSignedDataOid = "1.2.840.113549.1.7.2";

// What Android's recovery (install/verifier.cpp) does with each certificate signature algorithm.
int hashLenFor(const std::string& alg) {
    if (alg == "1.2.840.113549.1.1.4" || alg == "1.2.840.113549.1.1.5" ||  // md5WithRSA (treated as SHA-1), sha1WithRSA
        alg == "1.3.14.3.2.3" || alg == "1.3.14.3.2.29")                   // the same, old OIW numbers
        return 20;
    if (alg == "1.2.840.113549.1.1.11" || alg == "1.2.840.10045.4.3.2") return 32;  // sha256WithRSA, ecdsa-with-SHA256
    return 0;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

// A directory string as UTF-8, without control characters (it ends up in logs and on screen).
std::string text(const der::Tlv& t) {
    std::string out;
    switch (t.tag) {
        case der::kUtf8:
        case der::kPrintable:
        case der::kIa5:
            out.assign((const char*)t.p, t.len);
            break;
        case der::kT61:  // Latin-1 in practice
            for (size_t i = 0; i < t.len; i++) appendUtf8(out, t.p[i]);
            break;
        case der::kBmp:
            for (size_t i = 0; i + 1 < t.len; i += 2) appendUtf8(out, uint32_t(t.p[i]) << 8 | t.p[i + 1]);
            break;
        case der::kUniversal:
            for (size_t i = 0; i + 3 < t.len; i += 4)
                appendUtf8(out, uint32_t(t.p[i]) << 24 | uint32_t(t.p[i + 1]) << 16 | uint32_t(t.p[i + 2]) << 8 | t.p[i + 3]);
            break;
        default:
            return "";
    }
    for (auto& ch : out)
        if ((unsigned char)ch < 0x20 || ch == 0x7F) ch = '?';
    if (out.size() > 160) {
        out.resize(160);
        while (!out.empty() && (uint8_t(out.back()) & 0xC0) == 0x80) out.pop_back();  // don't end inside a character
        if (!out.empty() && uint8_t(out.back()) >= 0xC0) out.pop_back();
    }
    return out;
}

void readName(const der::Tlv& name, Cert& c) {
    der::Reader rdns(name);
    der::Tlv set;
    while (rdns.next(set)) {
        if (set.tag != der::kSet) continue;
        der::Reader atvs(set);
        der::Tlv atv;
        while (atvs.next(atv)) {
            if (atv.tag != der::kSequence) continue;
            der::Reader f(atv);
            der::Tlv type, value;
            if (!f.next(type, der::kOid) || !f.next(value)) continue;
            std::string o = der::oid(type), v = text(value);
            if (o == "2.5.4.3") c.cn = v;
            else if (o == "2.5.4.10") c.o = v;
            else if (o == "2.5.4.11") c.ou = v;
            else if (o == "2.5.4.7") c.l = v;
            else if (o == "2.5.4.8") c.st = v;
            else if (o == "2.5.4.6") c.c = v;
            else if (o == "1.2.840.113549.1.9.1") c.email = v;
        }
    }
}

// A positive INTEGER, leading zero bytes dropped - so the same key compares equal whichever certificate it is in.
bool positiveInt(const der::Tlv& t, std::vector<uint8_t>& out) {
    if (t.tag != der::kInteger || t.len == 0 || (t.p[0] & 0x80)) return false;
    size_t i = 0;
    while (i + 1 < t.len && t.p[i] == 0) i++;
    out.assign(t.p + i, t.p + t.len);
    return true;
}

bool readKey(const der::Tlv& spki, Cert& c) {
    der::Reader r(spki);
    der::Tlv alg, bits;
    if (!r.next(alg, der::kSequence) || !r.next(bits, der::kBitString) || bits.len < 1 || bits.p[0] != 0) return false;
    der::Reader ar(alg);
    der::Tlv algOid, params;
    if (!ar.next(algOid, der::kOid)) return false;
    std::string a = der::oid(algOid);
    const uint8_t* kp = bits.p + 1;
    size_t kn = bits.len - 1;
    if (a == kRsaEncryption) {
        der::Reader kr(kp, kn);
        der::Tlv seq, n, e;
        if (!kr.next(seq, der::kSequence)) return false;
        der::Reader nr(seq);
        if (!nr.next(n) || !nr.next(e) || !positiveInt(n, c.rsa.n) || !positiveInt(e, c.rsa.e)) return false;
        c.key = Cert::Key::Rsa;
        return true;
    }
    if (a == kEcPublicKey) {
        if (!ar.next(params, der::kOid) || der::oid(params) != kPrime256v1) {
            c.error = "an EC key on another curve than P-256";
            return false;
        }
        if (kn != 65 || kp[0] != 0x04) {
            c.error = "a compressed EC key";
            return false;
        }
        c.ec.x.assign(kp + 1, kp + 33);
        c.ec.y.assign(kp + 33, kp + 65);
        c.key = Cert::Key::Ec;
        return true;
    }
    c.error = "an unsupported key type (" + a + ")";
    return false;
}

}  // namespace

Cert parse(const uint8_t* p, size_t n) {
    Cert c;
    der::Reader top(p, n);
    der::Tlv cert, tbs, sigAlg, t, innerAlg, issuer, validity, subject, spki;
    if (!top.next(cert, der::kSequence)) {
        c.error = "not a certificate";
        return c;
    }
    c.der.assign(cert.raw, cert.raw + cert.rawLen);
    der::Reader cr(cert);
    if (!cr.next(tbs, der::kSequence) || !cr.next(sigAlg, der::kSequence)) {
        c.error = "a damaged certificate";
        return c;
    }
    c.sigAlg = der::algorithm(sigAlg);
    c.hashLen = hashLenFor(c.sigAlg);
    der::Reader tr(tbs);
    bool ok = tr.next(t);
    if (ok && t.tag == der::ctx(0)) ok = tr.next(t);  // version
    ok = ok && t.tag == der::kInteger;
    if (ok) c.serial.assign(t.p, t.p + t.len);
    ok = ok && tr.next(innerAlg, der::kSequence) && tr.next(issuer, der::kSequence) && tr.next(validity, der::kSequence) &&
         tr.next(subject, der::kSequence) && tr.next(spki, der::kSequence);
    if (!ok) {
        c.error = "a damaged certificate";
        return c;
    }
    c.issuerRaw.assign(issuer.raw, issuer.raw + issuer.rawLen);
    readName(subject, c);
    c.spki.assign(spki.raw, spki.raw + spki.rawLen);
    if (!readKey(spki, c)) {
        if (c.error.empty()) c.error = "a damaged public key";
        return c;
    }
    c.ok = true;
    return c;
}

std::string Cert::subject() const {
    std::string s;
    auto add = [&](const char* k, const std::string& v) {
        if (!v.empty()) s += (s.empty() ? "" : ", ") + std::string(k) + "=" + v;
    };
    add("CN", cn);
    add("OU", ou);
    add("O", o);
    add("L", l);
    add("ST", st);
    add("C", c);
    add("E", email);
    return s;
}

std::string Cert::name() const { return !cn.empty() ? cn : !o.empty() ? o : !ou.empty() ? ou : email; }

std::string Cert::keyDesc() const {
    if (key == Key::Rsa) return "RSA-" + std::to_string(rsa.bits());
    if (key == Key::Ec) return "EC P-256";
    return "unknown key";
}

std::string Cert::keyId() const {
    Sha256 h;
    h.update(spki.data(), spki.size());
    return Sha256::toHex(h.digest());
}

bool Cert::sameKey(const Cert& b) const {
    if (key != b.key || key == Key::None) return false;
    if (key == Key::Rsa) return rsa.n == b.rsa.n && rsa.e == b.rsa.e;
    return ec.x == b.ec.x && ec.y == b.ec.y;
}

bool Cert::isAospTestKey() const { return cn == "Android" && o == "Android" && email == "android@android.com"; }

std::string Cert::pem() const {
    std::string b64 = base64Encode(der.data(), der.size()), out = "-----BEGIN CERTIFICATE-----\n";
    for (size_t i = 0; i < b64.size(); i += 64) out += b64.substr(i, 64) + "\n";
    return out + "-----END CERTIFICATE-----\n";
}

std::vector<Cert> load(const std::string& bytes) {
    std::vector<Cert> v;
    static const std::string kBegin = "-----BEGIN CERTIFICATE-----", kEnd = "-----END CERTIFICATE-----";
    size_t pos = 0;
    bool pem = false;
    while ((pos = bytes.find(kBegin, pos)) != std::string::npos) {
        pem = true;
        size_t s = pos + kBegin.size(), e = bytes.find(kEnd, s);
        if (e == std::string::npos) break;
        std::string der = base64Decode(bytes.substr(s, e - s));
        Cert c = parse((const uint8_t*)der.data(), der.size());
        if (c.ok) v.push_back(std::move(c));
        pos = e + kEnd.size();
    }
    if (!pem && !bytes.empty()) {
        Cert c = parse((const uint8_t*)bytes.data(), bytes.size());
        if (c.ok) v.push_back(std::move(c));
    }
    return v;
}

std::vector<Cert> loadZip(const std::string& path, std::string* err) {
    std::vector<Cert> v;
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, path.c_str(), 0)) {
        if (err) *err = mz_zip_get_error_string(mz_zip_get_last_error(&z));
        return v;
    }
    int n = (int)mz_zip_reader_get_num_files(&z);
    for (int i = 0; i < n && i < 64; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st) || st.m_is_directory || st.m_uncomp_size > (1u << 20)) continue;
        size_t len = 0;
        void* p = mz_zip_reader_extract_to_heap(&z, i, &len, 0);
        if (!p) continue;
        for (auto& c : load(std::string((const char*)p, len))) v.push_back(std::move(c));
        mz_free(p);
    }
    mz_zip_reader_end(&z);
    if (v.empty() && err) *err = "no certificates in it";
    return v;
}

SignedData parseSignedData(const uint8_t* p, size_t n) {
    SignedData s;
    auto fail = [&s](const char* why) {
        s.error = why;
        return s;
    };
    der::Reader top(p, n);
    der::Tlv ci, type, content, sd, version, digestAlgs, encap, t;
    if (!top.next(ci, der::kSequence)) return fail("not a PKCS#7 block");
    der::Reader cr(ci);
    if (!cr.next(type, der::kOid) || der::oid(type) != kSignedDataOid) return fail("not PKCS#7 SignedData");
    if (!cr.next(content, der::ctx(0))) return fail("a damaged PKCS#7 block");
    der::Reader outer(content);
    if (!outer.next(sd, der::kSequence)) return fail("damaged SignedData");
    der::Reader r(sd);
    if (!r.next(version, der::kInteger) || !r.next(digestAlgs, der::kSet) || !r.next(encap, der::kSequence))
        return fail("damaged SignedData");
    for (;;) {  // optional certificates [0] and CRLs [1], then the signer infos
        if (!r.next(t)) return fail("no signer in it");
        if (t.tag == der::ctx(0)) {
            der::Reader certs(t);
            der::Tlv one;
            while (certs.next(one)) {
                if (one.tag != der::kSequence) continue;
                Cert c = parse(one.raw, one.rawLen);
                if (c.ok) s.certs.push_back(std::move(c));
            }
            continue;
        }
        if (t.tag == der::ctx(1)) continue;
        break;
    }
    if (t.tag != der::kSet) return fail("damaged SignedData (signer infos)");
    der::Reader sis(t);
    der::Tlv si, siVersion, sid, digestAlg, sigAlg, sig;
    if (!sis.next(si, der::kSequence)) return fail("no signer in it");
    der::Reader f(si);
    if (!f.next(siVersion, der::kInteger) || !f.next(sid) || !f.next(digestAlg, der::kSequence) || !f.next(sigAlg))
        return fail("a damaged signer info");
    if (sigAlg.tag == der::ctx(0)) return fail("signed attributes, which Android's recovery doesn't accept");
    if (sigAlg.tag != der::kSequence || !f.next(sig, der::kOctetString)) return fail("a damaged signer info");
    if (sid.tag == der::kSequence) {  // issuerAndSerialNumber
        der::Reader ir(sid);
        der::Tlv iss, ser;
        if (ir.next(iss, der::kSequence) && ir.next(ser, der::kInteger)) {
            s.sidIssuer.assign(iss.raw, iss.raw + iss.rawLen);
            s.sidSerial.assign(ser.p, ser.p + ser.len);
        }
    }
    s.digestAlg = der::algorithm(digestAlg);
    s.sigAlg = der::algorithm(sigAlg);
    s.signature.assign(sig.p, sig.p + sig.len);
    s.ok = true;
    return s;
}

const Cert* SignedData::signer() const {
    for (auto& c : certs)
        if (!sidSerial.empty() && c.serial == sidSerial && c.issuerRaw == sidIssuer) return &c;
    return certs.size() == 1 ? &certs[0] : nullptr;
}

std::string base64Decode(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=') break;
        int v = val(c);
        if (v < 0) continue;  // line breaks, spaces
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((acc >> bits) & 0xFF);
        }
    }
    return out;
}

std::string base64Encode(const uint8_t* p, size_t n) {
    static const char* kAbc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out += kAbc[(v >> 18) & 63];
        out += kAbc[(v >> 12) & 63];
        out += i + 1 < n ? kAbc[(v >> 6) & 63] : '=';
        out += i + 2 < n ? kAbc[v & 63] : '=';
    }
    return out;
}

}  // namespace tw::x509
