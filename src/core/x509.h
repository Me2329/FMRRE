#pragma once
// X.509 certificates (the keys Android trusts for OTAs, in otacerts.zip) and the PKCS#7 SignedData block that
// signapk puts at the end of a signed zip.
#include <cstdint>
#include <string>
#include <vector>

#include "core/pubkey.h"

namespace tw::x509 {

struct Cert {
    bool ok = false;
    std::string error;
    std::vector<uint8_t> der;                     // the whole certificate
    std::string cn, o, ou, l, st, c, email;       // subject
    std::vector<uint8_t> issuerRaw, serial;       // to find the certificate a SignerInfo names
    std::vector<uint8_t> spki;                    // SubjectPublicKeyInfo, DER
    std::string sigAlg;                           // OID of the certificate's own signature algorithm
    // The hash Android's recovery uses with this key - picked from that algorithm like it does: 20 = SHA-1
    // (md5/sha1WithRSA), 32 = SHA-256 (sha256WithRSA, ecdsa-with-SHA256), 0 = a key it can't use.
    int hashLen = 0;
    enum class Key { None, Rsa, Ec } key = Key::None;
    pk::RsaKey rsa;
    pk::EcKey ec;

    std::string subject() const;   // "CN=..., OU=..., O=..." - the parts that are there
    std::string name() const;      // the most telling single name: CN, else O, else OU
    std::string keyDesc() const;   // "RSA-4096", "EC P-256"
    std::string keyId() const;     // SHA-256 of the public key (SubjectPublicKeyInfo), hex
    bool sameKey(const Cert& other) const;
    bool isAospTestKey() const;    // the subject of AOSP's public test keys (testkey, platform, shared, media)
    std::string pem() const;
};

Cert parse(const uint8_t* der, size_t len);
// All CERTIFICATE blocks of PEM text, or the one certificate of a DER file.
std::vector<Cert> load(const std::string& bytes);
// otacerts.zip: certificates (.x509.pem, .pem, .crt, .cer, .der) in a zip, Android's list of OTA keys.
std::vector<Cert> loadZip(const std::string& path, std::string* err = nullptr);
// /res/keys of recoveries before Android 10: AOSP's mincrypt format (DumpPublicKey), keys separated by commas -
// "{64,0xc926ad21,{modulus words},{R^2 words}}" (v1: e=3, SHA-1), "v2 {...}" (e=65537, SHA-1), v3 (e=3, SHA-256),
// v4 (e=65537, SHA-256), "v5 {32,{x bytes},{y bytes}}" (EC P-256, SHA-256); numbers little-endian.
std::vector<Cert> loadMincrypt(const std::string& text, std::string* err = nullptr);
// A key on its own (a PEM "PUBLIC KEY", or DER SubjectPublicKeyInfo), e.g. update_engine's update-payload-key.pub.pem.
std::vector<Cert> loadPublicKeys(const std::string& bytes);
// DER SubjectPublicKeyInfo of a key, so the same key gets the same keyId() wherever it came from.
std::vector<uint8_t> rsaSpki(const std::vector<uint8_t>& n, const std::vector<uint8_t>& e);
std::vector<uint8_t> ecSpki(const std::vector<uint8_t>& x, const std::vector<uint8_t>& y);

// PKCS#7 / CMS SignedData, read the way Android's recovery reads it (verifier.cpp read_pkcs7): the first
// SignerInfo, which must not have signed attributes - the signature is then over the data itself.
struct SignedData {
    bool ok = false;
    std::string error;
    std::vector<Cert> certs;
    std::string digestAlg, sigAlg;  // OIDs from the SignerInfo
    std::vector<uint8_t> signature;
    std::vector<uint8_t> sidIssuer, sidSerial;
    const Cert* signer() const;  // the certificate the SignerInfo names, else the only one
};
SignedData parseSignedData(const uint8_t* p, size_t n);

std::string base64Decode(const std::string& s);
std::string base64Encode(const uint8_t* p, size_t n);

// OIDs
extern const char* const kSha1;
extern const char* const kSha256;

}  // namespace tw::x509
